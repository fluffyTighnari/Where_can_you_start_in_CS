/*
 * executor.cpp —— 执行器：把 AST 命令落到存储引擎上
 *
 * 每个 execute_xxx 函数的套路都一致：
 *   1. 从 SymbolTable 取表的元信息与根页号
 *   2. 调用 B+ 树接口完成实际的读写
 *   3. 把本次产生的脏节点 / 新页 / 待删页统一落盘
 *   4. 根页号若发生变化（分裂或降高），回写符号表
 *
 * 分层要点：本层不解析 SQL，也不关心页内字节布局，
 * 只负责"调度"——选哪个接口、按什么顺序落盘、出错怎么回滚。
 */
#include "executor.hpp"
#include "Parse/parse.hpp"
#include "Parse/sense.hpp"
#include "Help/Error/error.hpp"
#include "Help/Show/show.hpp"
#include "Help/Serialize/serialize.hpp"
#include "Storage_engine/storage.hpp"
#include "Storage_engine/B+Tree/Btree.hpp"
#include "Storage_engine/B+Tree/StackPath.hpp"
#include <memory>
#include <string>
#include <vector>
#include <iostream>
#include <unordered_map>
#include <variant>

/* 把谓词右侧的字面量转成 B+ 树键；NULL 用 0 兜底（语义层已排除） */
static Key make_key_from_value(const ValueNode& val){
    switch(val.type){
        case ValueNode::Sort::Int:
            return Key(val.Inum);
        case ValueNode::Sort::Float:
            return Key(val.Fnum);
        case ValueNode::Sort::String:
            return Key(val.str.c_str());
        default:
            return Key(0);
    }
}

/*
 * CREATE TABLE 要做三件事：
 *   1. 分配一个新页作为该表 B+ 树的根（空叶子节点）
 *   2. 在符号表里登记表定义与根页号
 *   3. 把表元信息写入系统 B+ 树，保证重启后能恢复
 *
 * 系统表采用懒初始化：第一次 create 时才分配 system_page 的根页。
 */
static Result<std::unique_ptr<Info>> execute_create(CreateNode* create, Storage* storage){
    auto info = std::make_unique<Info>();
    info->need_print = false;
    info->output = "";

    auto& pager = storage->get_Pager();
    SymbolTable* st = storage->acquire_SymbolTable();

    TableNode tn = create->table_name;
    if(st->get_table_message(tn) != nullptr){
        return Error("table already exists", Error::Layer::Executor);
    }

    uint32_t new_page = pager.alloc_new_page();
    if(new_page == 0){
        return Error("failed to allocate new page", Error::Layer::Pager);
    }

    Btree_Node root;
    memset(&root, 0, sizeof(root));
    root.dirty = true;
    root.root = true;
    root.type = Btree_Node::Type::LEAF_NODE;
    root.key_number = 0;
    root.father = 0;
    root.page_num = new_page;
    root.pre_leaf = 0;
    root.next_leaf = 0;
    root.curval = 0;

    if(!pager.update_page(new_page, reinterpret_cast<char*>(&root))){
        return Error("failed to write root page", Error::Layer::Pager);
    }

    auto cols = create->columns;
    if(!st->add_table(create->table_name, cols)){
        return Error("failed to add table to symbol table", Error::Layer::Executor);
    }

    st->set_root_page(create->table_name.table_name, new_page);

    // 写入系统 B+ 树（懒初始化 + 插入表元数据）
    uint32_t sys_root = pager.main_page->system_page;
    if(sys_root == 0){
        sys_root = pager.alloc_new_page();
        if(sys_root == 0){
            return Error("failed to allocate system table root", Error::Layer::Pager);
        }
        Btree_Node sys_node;
        memset(&sys_node, 0, sizeof(sys_node));
        sys_node.root = true;
        sys_node.type = Btree_Node::Type::LEAF_NODE;
        sys_node.page_num = sys_root;
        sys_node.key_number = 0;
        sys_node.dirty = true;
        if(!pager.update_page(sys_root, reinterpret_cast<char*>(&sys_node))){
            return Error("failed to init system table root", Error::Layer::Pager);
        }
        pager.main_page->system_page = sys_root;
        pager.flush_page();
    }

    // 序列化表元数据并插入系统 B+ 树
    std::string sys_value;
    serialize_system(create->table_name.table_name, *create, sys_value, new_page);
    Key sys_key(create->table_name.table_name.c_str());

    StackPath sys_path;
    std::vector<std::unique_ptr<Btree_Node>> sys_new_pages;
    uint32_t new_sys_root = sys_root;

    auto sys_result = Btree_insert(sys_root, sys_key, sys_value, pager, sys_path, sys_new_pages, new_sys_root);
    if(std::holds_alternative<Error>(sys_result)){
        return std::get<Error>(sys_result);
    }

    if(!sys_path.flush_all(pager)){
        return Error("failed to flush sys path", Error::Layer::Pager);
    }

    for(auto& np : sys_new_pages){
        if(np->dirty){
            np->dirty = false;
            if(!pager.update_page(np->page_num, reinterpret_cast<char*>(np.get()))){
                return Error("failed to write sys new page", Error::Layer::Pager);
            }
        }
    }

    if(new_sys_root != sys_root){
        pager.main_page->system_page = new_sys_root;
        pager.flush_page();
    }

    info->output = "Table created successfully.";
    return info;
}

/*
 * INSERT：第一列的值即主键（本项目约定），序列化成一行后插入 B+ 树。
 * 落盘顺序：先把 StackPath 上的修改写回，再写新分裂出的节点，最后更新根页号。
 */
static Result<std::unique_ptr<Info>> execute_insert(InsertNode* insert, Storage* storage){
    auto info = std::make_unique<Info>();
    info->need_print = false;

    SymbolTable* st = storage->acquire_SymbolTable();
    TableNode tn = insert->table_name;
    Table* table = st->get_table_message(tn);
    if(!table){
        return Error("table does not exist", Error::Layer::Executor);
    }

    uint32_t col_num = table->get_column_num();
    if(col_num == 0){
        return Error("table has no columns", Error::Layer::Executor);
    }

    Key key = make_key_from_value(insert->values[0]);

    std::string serialized;
    serialize(serialized, *insert, table);

    auto& pager = storage->get_Pager();

    uint32_t root_page = st->get_root_page(insert->table_name.table_name);
    if(root_page == 0){
        return Error("table root page not found", Error::Layer::Executor);
    }

    StackPath path;
    std::vector<std::unique_ptr<Btree_Node>> new_pages;
    uint32_t new_root = root_page;

    auto result = Btree_insert(root_page, key, serialized, pager, path, new_pages, new_root);
    if(std::holds_alternative<Error>(result)){
        return std::get<Error>(result);
    }

    if(!path.flush_all(pager)){
        return Error("failed to flush path", Error::Layer::Pager);
    }

    for(auto& np : new_pages){
        if(np->dirty){
            np->dirty = false;
            if(!pager.update_page(np->page_num, reinterpret_cast<char*>(np.get()))){
                return Error("failed to write new page", Error::Layer::Pager);
            }
        }
    }

    if(new_root != root_page){
        st->set_root_page(insert->table_name.table_name, new_root);
    }

    info->output = "Insert successful.";
    return info;
}

/*
 * SELECT 两条路径：
 *   带 where → Btree_find 走索引精确定位单行
 *   无 where → Btree_scan 全表顺序扫描
 *
 * need_cols 记录实际要输出的列下标；deserialize 会跳过其余列，
 * 因此投影下推到了反序列化阶段，避免无谓的字符串构造。
 */
static Result<std::unique_ptr<Info>> execute_select(SelectNode* select, Storage* storage){
    auto info = std::make_unique<Info>();
    info->need_print = true;

    SymbolTable* st = storage->acquire_SymbolTable();
    TableNode tn = select->table_name;
    Table* table = st->get_table_message(tn);
    if(!table){
        return Error("table does not exist", Error::Layer::Executor);
    }

    uint32_t root_page = st->get_root_page(select->table_name.table_name);
    if(root_page == 0){
        return Error("table has no data", Error::Layer::Executor);
    }

    auto& pager = storage->get_Pager();

    std::vector<uint32_t> need_cols;
    if(select->select_all){
        for(uint32_t i = 0; i < table->get_column_num(); i++){
            need_cols.push_back(i);
        }
    } else {
        for(auto& col : select->columns){
            for(uint32_t i = 0; i < table->get_column_num(); i++){
                const Column* c = table->get_column(i);
                if(c && c->column_name == col.column_name){
                    need_cols.push_back(i);
                    break;
                }
            }
        }
    }

    if(select->given_expr){
        Key key = make_key_from_value(select->expr.rightexpr);
        auto result = Btree_find(root_page, key, pager);
        if(std::holds_alternative<Error>(result)){
            auto& err = std::get<Error>(result);
            if(err.layer == Error::Layer::Btree && err.message == "invalid key"){
                // 找不到 key 不是错误，返回空结果
                info->output = "No rows found.";
                return info;
            }
            return err;
        }
        auto& data = std::get<std::string>(result);

        Info* out = info.get();
        deserialize(table, need_cols, data, out);
    } else {
        auto result = Btree_scan(root_page, pager);
        if(std::holds_alternative<Error>(result)){
            return std::get<Error>(result);
        }
        auto& data = std::get<std::string>(result);

        Info* out = info.get();
        deserialize(table, need_cols, data, out);
    }

    if(info->columns.empty()){
        info->output = "No rows found.";
    } else {
        info->output = "";
    }
    return info;
}

/*
 * DELETE 按条件分成两条路径：
 *   无 where → Btree_delete_range 递归释放整棵树（保留根页并清空）
 *   有 where → Btree_delete 定位单行删除，可能触发借位/合并
 *
 * 合并产生的待删页必须在结构修改成功后才真正 delete_page，
 * 否则中途失败会留下悬空引用。
 */
static Result<std::unique_ptr<Info>> execute_delete(DeleteNode* del, Storage* storage){
    auto info = std::make_unique<Info>();
    info->need_print = false;

    SymbolTable* st = storage->acquire_SymbolTable();
    TableNode tn = del->table_name;
    Table* table = st->get_table_message(tn);
    if(!table){
        return Error("table does not exist", Error::Layer::Executor);
    }

    uint32_t root_page = st->get_root_page(del->table_name.table_name);
    if(root_page == 0){
        return Error("table has no data", Error::Layer::Executor);
    }

    auto& pager = storage->get_Pager();

    if(!del->given_expr){
        std::vector<uint32_t> del_pages;
        uint32_t new_root = root_page;
        auto result = Btree_delete_range(root_page, pager, del_pages, new_root);
        if(std::holds_alternative<Error>(result)){
            return std::get<Error>(result);
        }
        for(auto p : del_pages){
            pager.delete_page(p);
        }
        st->set_root_page(del->table_name.table_name, new_root);
        info->output = "All rows deleted.";
        return info;
    }

    Key key = make_key_from_value(del->expr.rightexpr);
    StackPath path;
    std::vector<uint32_t> del_pages;
    std::vector<std::unique_ptr<Btree_Node>> update_pages;
    uint32_t new_root = root_page;

    auto result = Btree_delete(root_page, key, pager, path, del_pages, update_pages, new_root);
    if(std::holds_alternative<Error>(result)){
        return std::get<Error>(result);
    }

    if(!path.flush_all(pager)){
        return Error("failed to flush path", Error::Layer::Pager);
    }

    for(auto& up : update_pages){
        if(up->dirty){
            up->dirty = false;
            if(!pager.update_page(up->page_num, reinterpret_cast<char*>(up.get()))){
                return Error("failed to write update page", Error::Layer::Pager);
            }
        }
    }

    for(auto dp : del_pages){
        pager.delete_page(dp);
    }

    if(new_root != root_page){
        st->set_root_page(del->table_name.table_name, new_root);
    }

    info->output = "Delete successful.";
    return info;
}

/*
 * DROP TABLE 分三步，顺序不能反：
 *   1. 清空并释放该表的数据页（Btree_delete_range 保留根页，之后再单独释放根页）
 *   2. 从系统 B+ 树中删掉表元信息（系统表自身的分裂/合并同样要落盘）
 *   3. 从内存符号表中移除
 * 注意第 2 步失败不影响第 3 步——元信息残留比内存与磁盘不一致更容易接受。
 */
static Result<std::unique_ptr<Info>> execute_drop(DropNode* drop, Storage* storage){
    auto info = std::make_unique<Info>();
    info->need_print = false;

    auto& pager = storage->get_Pager();
    SymbolTable* st = storage->acquire_SymbolTable();

    std::string table_name = drop->table_name.table_name;
    uint32_t root_page = st->get_root_page(table_name);
    if(root_page == 0){
        return Error("table does not exist", Error::Layer::Executor);
    }

    // 1. 删除表的所有数据页（保留根页，由 delete_range 清空）
    std::vector<uint32_t> del_pages;
    uint32_t new_root = root_page;
    auto del_result = Btree_delete_range(root_page, pager, del_pages, new_root);
    if(std::holds_alternative<Error>(del_result)){
        return std::get<Error>(del_result);
    }
    for(auto dp : del_pages){
        pager.delete_page(dp);
    }
    // 根页也要释放（表完全删除了）
    pager.delete_page(new_root);

    // 2. 从系统 B+ 树中删除表元数据
    uint32_t sys_root = pager.main_page->system_page;
    if(sys_root != 0){
        Key sys_key(table_name.c_str());
        StackPath sys_path;
        std::vector<uint32_t> sys_del_pages;
        std::vector<std::unique_ptr<Btree_Node>> sys_update_pages;
        uint32_t new_sys_root = sys_root;

        auto sys_del_result = Btree_delete(sys_root, sys_key, pager, sys_path, sys_del_pages, sys_update_pages, new_sys_root);
        if(!std::holds_alternative<Error>(sys_del_result)){
            if(!sys_path.flush_all(pager)){
                return Error("failed to flush sys path", Error::Layer::Pager);
            }
            for(auto& up : sys_update_pages){
                if(up->dirty){
                    up->dirty = false;
                    if(!pager.update_page(up->page_num, reinterpret_cast<char*>(up.get()))){
                        return Error("failed to write sys update page", Error::Layer::Pager);
                    }
                }
            }
            for(auto dp : sys_del_pages){
                pager.delete_page(dp);
            }
            if(new_sys_root != sys_root){
                pager.main_page->system_page = new_sys_root;
                pager.flush_page();
            }
        }
    }

    // 3. 从符号表中移除
    st->remove_table(table_name);

    info->output = "Table dropped successfully.";
    return info;
}

/*
 * SHOW TABLES：遍历符号表中的根页映射，输出表名与主键信息。
 * 主键约定为第 0 列，因此直接取 get_column(0)。
 */
static Result<std::unique_ptr<Info>> execute_show_tables(ShowTablesNode* show, Storage* storage){
    auto info = std::make_unique<Info>();
    info->need_print = true;

    info->columns.push_back("TABLE_NAME");
    info->types.push_back("TEXT");
    info->columns.push_back("PRIMARY_KEY");
    info->types.push_back("TEXT");
    info->columns.push_back("PK_TYPE");
    info->types.push_back("TEXT");

    SymbolTable* st = storage->acquire_SymbolTable();

    auto& root_pages = st->get_root_pages();
    for(auto& [name, page] : root_pages){
        info->values.push_back(name);
        TableNode tn;
        tn.table_name = name;
        const Table* table = st->get_table_message(tn);
        if(table && table->get_column_num() > 0){
            const Column* pk = table->get_column(0);
            info->values.push_back(pk->column_name);
            std::string pk_type;
            switch(pk->type){
                case Type::INT: pk_type = "INT"; break;
                case Type::FLOAT: pk_type = "FLOAT"; break;
                case Type::TEXT: pk_type = "TEXT"; break;
                case Type::VARCHAR: pk_type = "VARCHAR"; break;
            }
            info->values.push_back(pk_type);
        } else {
            info->values.push_back("");
            info->values.push_back("");
        }
    }

    if(info->values.empty()){
        info->output = "Empty set.";
    } else {
        info->output = "";
    }

    return info;
}

/*
 * UPDATE 的实现方式是"读整行 → 改一列 → 重新序列化 → 写回"：
 * 因为 B+ 树没有局部更新单列的能力，整行重写的代价换来实现上的简单。
 * 目前要求必须带 where，否则拒绝（避免全表更新的歧义）。
 */
static Result<std::unique_ptr<Info>> execute_update(UpdateNode* update, Storage* storage){
    auto info = std::make_unique<Info>();
    info->need_print = false;

    SymbolTable* st = storage->acquire_SymbolTable();
    TableNode tn = update->table_name;
    Table* table = st->get_table_message(tn);
    if(!table){
        return Error("table does not exist", Error::Layer::Executor);
    }

    uint32_t root_page = st->get_root_page(update->table_name.table_name);
    if(root_page == 0){
        return Error("table has no data", Error::Layer::Executor);
    }

    auto& pager = storage->get_Pager();

    if(!update->given_expr){
        info->output = "Update without WHERE is not supported.";
        return info;
    }

    // 找到要更新的列
    uint32_t set_col_idx = table->get_column_num();
    for(uint32_t i = 0; i < table->get_column_num(); i++){
        auto col = table->get_column(i);
        if(col && col->column_name == update->set.leftexpr.column_name){
            set_col_idx = i;
            break;
        }
    }
    if(set_col_idx == table->get_column_num()){
        return Error("column not found", Error::Layer::Executor);
    }

    Key key = make_key_from_value(update->expr.rightexpr);

    auto find_result = Btree_find(root_page, key, pager);
    if(std::holds_alternative<Error>(find_result)){
        auto& err = std::get<Error>(find_result);
        if(err.layer == Error::Layer::Btree && err.message == "invalid key"){
            info->output = "No rows matched.";
            return info;
        }
        return err;
    }

    // 反序列化当前行的所有列
    std::string row_data = std::get<std::string>(find_result);
    std::vector<uint32_t> all_cols;
    for(uint32_t i = 0; i < table->get_column_num(); i++){
        all_cols.push_back(i);
    }
    Info temp_info;
    deserialize(table, all_cols, row_data, &temp_info);

    // 修改指定列的值
    std::string new_val_str;
    auto col = table->get_column(set_col_idx);
    if(col->type == Type::INT){
        new_val_str = std::to_string(update->set.rightexpr.Inum);
    }
    else if(col->type == Type::FLOAT){
        new_val_str = std::to_string(update->set.rightexpr.Fnum);
    }
    else{
        new_val_str = update->set.rightexpr.str;
    }
    temp_info.values[set_col_idx] = new_val_str;

    // 重新序列化
    std::string new_row;
    if(!serialize_row_from_strings(table, temp_info.values, new_row)){
        return Error("failed to serialize updated row", Error::Layer::Executor);
    }

    Btree_Node updated_node;
    auto upd_result = Btree_update(root_page, key, new_row, pager, updated_node);
    if(std::holds_alternative<Error>(upd_result)){
        return std::get<Error>(upd_result);
    }

    if(!pager.update_page(updated_node.page_num, reinterpret_cast<char*>(&updated_node))){
        return Error("failed to write updated page", Error::Layer::Pager);
    }

    info->output = "Update successful.";
    return info;
}

/* 按 AST 节点类型分派到对应的执行函数 */
Result<std::unique_ptr<Info>> execute_command(Command* command, Storage* storage){
    if(!command || !command->Main_command){
        return Error("invalid command", Error::Layer::Executor);
    }

    auto& ast = *command->Main_command;

    if(std::holds_alternative<CreateNode>(ast)){
        return execute_create(&std::get<CreateNode>(ast), storage);
    }
    else if(std::holds_alternative<InsertNode>(ast)){
        return execute_insert(&std::get<InsertNode>(ast), storage);
    }
    else if(std::holds_alternative<SelectNode>(ast)){
        return execute_select(&std::get<SelectNode>(ast), storage);
    }
    else if(std::holds_alternative<DeleteNode>(ast)){
        return execute_delete(&std::get<DeleteNode>(ast), storage);
    }
    else if(std::holds_alternative<UpdateNode>(ast)){
        return execute_update(&std::get<UpdateNode>(ast), storage);
    }
    else if(std::holds_alternative<DropNode>(ast)){
        return execute_drop(&std::get<DropNode>(ast), storage);
    }
    else if(std::holds_alternative<ShowTablesNode>(ast)){
        return execute_show_tables(&std::get<ShowTablesNode>(ast), storage);
    }

    return Error("unknown command type", Error::Layer::Executor);
}
