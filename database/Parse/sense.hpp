//sense.hpp
#pragma once

#include "Help/Error/error.hpp"
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <memory>
#include <sstream>

/*
 * 语义分析层：利用符号表进行语义检查。
 *
 * 语法层只保证"句子结构正确"，本层负责回答"表和列真的存在吗、类型是否匹配"。
 * 核心数据结构：
 *   Column       列定义（名字 + 类型）
 *   Table        一张表的完整列信息，用哈希表做名字 → 下标 的快速索引
 *   SymbolTable  所有表的集合，同时记录每张表数据 B+ 树的根页号
 *
 * 符号表在启动时由系统表（system_page 指向的 B+ 树）重建：
 * 每行元信息对应一张表，这样重启后能恢复所有表的定义。
 */
struct Pager_t;
struct token;

struct TableNode;
struct ColumnRef;
struct ColumnDef;
struct PredicateNode;
struct ValueNode;

struct CreateNode;
struct SelectNode;
struct DeleteNode;
struct UpdateNode;
struct InsertNode;
struct DropNode;
struct ShowTablesNode;
struct Command;
class Storage;

enum class Type{
    INT,
    FLOAT,
    TEXT,
    VARCHAR
};

class Column{
    public:
    std::string column_name;
    Type type;
    bool can_NULL; /*暂时默认全支持NULL*/

    /* 由类型名字符串构造，构造时即完成合法性检查（由调用方先校验） */
    Column(std::string& a,std::string& b,bool c):column_name(a),can_NULL(c){
        if(b == "INT"){
            type = Type::INT;
        }
        else if(b == "FLOAT"){
            type = Type::FLOAT;
        }
        else if(b == "TEXT"){
            type = Type::TEXT;
        }
        else if(b == "VARCHAR"){
            type = Type::VARCHAR;
        }
    }
};

class Table{
    public:
    bool add_column(ColumnDef& column); /*暂时没有用*/

    bool column_exist(ColumnRef& column_name) const;

    bool type_valid(ColumnRef& column_name,ValueNode& value) const;

    bool type_valid(int index,ValueNode& value) const;

    uint32_t get_column_num() const{
        return column_num;
    }

    const Column* get_column(uint32_t index) const{
        return index < column_num ? &columns[index] : nullptr;
    }

    private:
    uint32_t column_num = 0;
    std::vector<Column> columns; /*默认主键：下标0*/
    std::unordered_map<std::string,uint32_t> index;   /* 列名 → 列下标 */
};

class SymbolTable{
    public:
    const Table* get_table_message(TableNode& table_name) const;

    Table* get_table_message(TableNode& table_name);

    bool add_table(TableNode& table_name,std::vector<ColumnDef>& column);

    /* 恢复已有表时使用：额外带回数据 B+ 树的根页号 */
    bool add_table_with_root(const std::string& table_name, std::vector<ColumnDef>& columns, uint32_t root_page);

    uint32_t get_table_num() const{
        return table_num;
    }

    uint32_t get_root_page(const std::string& table_name) const;

    void set_root_page(const std::string& table_name, uint32_t page);

    const std::unordered_map<std::string,uint32_t>& get_root_pages() const{
        return root_pages;
    }

    bool remove_table(const std::string& table_name);

    private:
    uint32_t table_num = 0;
    std::unordered_map<std::string,Table> tables;          /* 表名 → 表定义 */
    std::unordered_map<std::string,uint32_t> root_pages;   /* 表名 → 根页号 */
};

/* 从系统 B+ 树重建符号表；新建的库返回空表 */
std::unique_ptr<SymbolTable> InitSymbolTable(Pager_t& pager);

Result<bool> parse_meaning(Command* command, const Storage& storage_t);

/* 各命令的语义检查：表/列存在性、类型匹配、谓词合法性 */
bool parse_create_cmd(CreateNode& create,const SymbolTable& symbol_table,std::string& error);

bool parse_delete_cmd(DeleteNode& del,const SymbolTable& symbol_table,std::string& error);

bool parse_select_cmd(SelectNode& select,const SymbolTable& symbol_table,std::string& error);

bool parse_insert_cmd(InsertNode& insert,const SymbolTable& symbol_table,std::string& error);

bool parse_update_cmd(UpdateNode& update,const SymbolTable& symbol_table,std::string& error);

bool parse_drop_cmd(DropNode& drop,const SymbolTable& symbol_table,std::string& error);

bool parse_expr(const Table* table,PredicateNode& expr);
