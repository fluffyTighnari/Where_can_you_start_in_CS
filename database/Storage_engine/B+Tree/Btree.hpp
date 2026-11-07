#pragma once

#ifdef BTREE_USE_MOCK_PAGER
#include "mockpager.hpp"
#else
#include "Storage_engine/pager.hpp"
#endif
#include "Help/Error/error.hpp"

#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <cstring>
#include <memory>
#define MAX_KEYS 16          /* 每个节点最多 16 个键（及 17 个子指针） */
#define LOGNEST_MESSAGE 4096U
#define HEAD_SIZE 1202U      /* 节点头部占用的字节数，剩余部分留给 values */
struct Info;
struct StackPath;
struct Pager_t;

/*
 * B+ 树总览
 * ---------
 * 结构特点：
 *   - 中间节点只存键与子页号，不存数据；叶子节点存键与行数据
 *   - 所有叶子通过 pre_leaf / next_leaf 串成一条有序链表，支持顺序扫描
 *   - 每个节点恰好占一页（4096B），直接以内存结构映射磁盘内容
 *
 * 分裂与合并：
 *   插入导致叶子溢出 → Page_Split 拆成两半，分界键上浮到父节点（Split_chain 逐层向上）
 *   删除导致节点键数不足 → 先尝试向左右兄弟借（Borrow_*），
 *   借不到则与兄弟合并（Merge_*），父节点删键，必要时继续向上（Merge_chain）
 *
 * 存储格式见文件末尾的注释。
 */

#pragma pack(push, 1)
/*
 * 键：支持 int / float / varchar 三种类型。
 * 三种值用固定大小的联合体式布局共存（val_int / val_float / str 同时存在），
 * 由 type 决定比较时使用哪一个，比较运算符内部按类型分派。
 */
struct Key{
    enum class Type : uint32_t{
        KEY_INT,
        KEY_FLOAT,
        KEY_VARCHAR
    };

    Type type;

    int val_int;
    float val_float;
    uint32_t length;
    char str[48];

    Key(const char* s){
        length = strlen(s);
        if (length > sizeof(str) - 1){
            length = sizeof(str) - 1;   /* 超长字符串截断，保证定长布局 */
        }
        strncpy(str, s, length);
        str[length] = '\0';

        type = Type::KEY_VARCHAR;
    }

    Key(int val){
        val_int = val;
        type = Type::KEY_INT;
    }

    Key(float val){
        val_float = val;
        type = Type::KEY_FLOAT;
    }

    Key() = default;

    bool operator<(const Key& outer) const{
        if(type == Type::KEY_INT){
            return val_int < outer.val_int;
        }
        else if(type == Type::KEY_FLOAT){
            return val_float < outer.val_float;
        }
        else{
            return strcmp(str,outer.str) < 0;
        }
    }

    bool operator>(const Key& outer) const{
        if(type == Type::KEY_INT){
            return val_int > outer.val_int;
        }
        else if(type == Type::KEY_FLOAT){
            return val_float > outer.val_float;
        }
        else{
            return strcmp(str,outer.str) > 0;
        }
    }

    bool operator<=(const Key& outer) const{
        if(type == Type::KEY_INT){
            return val_int <= outer.val_int;
        }
        else if(type == Type::KEY_FLOAT){
            return val_float <= outer.val_float;
        }
        else{
            return strcmp(str,outer.str) <= 0;
        }
    }

    bool operator>=(const Key& outer) const{
        if(type == Type::KEY_INT){
            return val_int >= outer.val_int;
        }
        else if(type == Type::KEY_FLOAT){
            return val_float >= outer.val_float;
        }
        else{
            return strcmp(str,outer.str) >= 0;
        }
    }

    bool operator==(const Key& outer) const{
        if(type == Type::KEY_INT){
            return val_int == outer.val_int;
        }
        else if(type == Type::KEY_FLOAT){
            return val_float == outer.val_float;
        }
        else{
            return strcmp(str,outer.str) == 0;
        }
    }

    bool operator!=(const Key& outer) const{
        if(type == Type::KEY_INT){
            return val_int != outer.val_int;
        }
        else if(type == Type::KEY_FLOAT){
            return val_float != outer.val_float;
        }
        else{
            return strcmp(str,outer.str) != 0;
        }
    }

};//size:64

/*
 * B+ 树节点：整个 4096 字节就是一页，末尾留一部分给行数据。
 *
 * 布局要点：
 *   keys[i]        第 i 个键
 *   son_pages[i]   中间节点：第 i 个键的左侧子页；son_pages[key_number] 是最右子页
 *   offsets[i]     叶子节点：第 i 个键对应值在 values 中的起始偏移
 *   curval         叶子节点：values 已用字节数
 *   因此第 i 个值的长度 = (i+1 < key_number ? offsets[i+1] : curval) - offsets[i]
 */
#pragma pack(1)
struct Btree_Node{

    enum class Type : uint32_t{
        MID_NODE = 0,
        LEAF_NODE = 1
    };

    bool dirty;
    bool root;
    Type type;
    uint32_t key_number = 0;
    uint32_t father = 0;      /* 父页号，0 表示无父（根） */
    uint32_t page_num;
    uint32_t pre_leaf = 0;    /* 叶子链表的左邻居 */
    uint32_t next_leaf = 0;   /* 叶子链表的右邻居 */

    uint32_t curval = 0;      /* values 区已用字节数（仅叶子有效） */
    uint32_t son_pages[MAX_KEYS + 1];
    uint32_t offsets[MAX_KEYS];
    uint8_t reserves[16];
    Key keys[MAX_KEYS];
    char values[PAGE_SIZE - HEAD_SIZE];

    bool is_dirty() const{
        return dirty;
    }

    bool is_leaf() const{
        return type == Type::LEAF_NODE;
    }

    bool is_root() const{
        return root;
    }

    /* 二分查找：返回第一个 >= key 的位置 */
    uint32_t Find_Key_Index(const Key& key) const;

    uint32_t Find_Next_Page(const Key& key) const;/*MID调用*/

    bool Find_Value(const Key& key,char* buffer, uint32_t& out_size) const;/*LEAF调用*/

    uint32_t Find_Value(char* buffer, uint32_t buf_offset) const;/*LEAF调用, returns bytes written*/

    bool Insert_Key(const Key& key,uint32_t son_page);/*MID调用*/

    bool Insert_Value(const Key& key,const char* buffer, uint32_t val_size);/*LEAF调用*/

    bool Update_Value(const Key& key,const char* buffer, uint32_t val_size);/*LEAF调用*/

    bool Delete_Key(const Key& key,uint32_t son_page);/*MID调用*/

    bool Update_Key(const Key& prekey,const Key& newkey);

    bool Delete_Value(const Key& key);/*LEAF调用*/

    bool clear();/*ROOT调用*/

    /*根据结点类型进行分裂/合并的分类讨论*/

    bool Page_Split(Btree_Node& new_page,uint32_t new_page_num,Key& risekey);

    bool Borrow_Left(Btree_Node& left);

    bool Borrow_Right(Btree_Node& right);

    bool Merge_Left(Btree_Node& left,Key& delkey);

    bool Merge_Right(Btree_Node& right,Key& delkey);

    #ifdef DEBUG

    void print(FILE* fp) const;

    void print() const { print(stdout); }

    bool validate() const;

    #endif

};//size:4096

#pragma pack(pop)

static_assert(sizeof(Btree_Node) == PAGE_SIZE, "Btree_Node size must equal PAGE_SIZE");

/*
 * B+tree存储结构（叶子节点的 values 区，单个节点内多行连续存放）：
 * [RowHeader][int:...][size][string:...][RowHeader][...][...]
 * |offset[0]                            |offset[1]
 *
 * System_page存储结构（系统表的值，一行描述一张表）：
 * [RowHeader][int:page_num][int:column_num][int:size1][string:column][int:size2][string:column_type]
 */

Result<std::string> Btree_find(uint32_t page_num,Key& key,Pager_t& pager);

/* 全量顺序扫描：沿叶子链表把所有 values 拼成一个字符串 */
Result<std::string> Btree_scan(uint32_t page_num,Pager_t& pager);

Result<bool> Btree_insert(uint32_t page_num,Key& key,std::string& value,Pager_t& pager,StackPath& path,std::vector<std::unique_ptr<Btree_Node>>& new_pages,uint32_t& out_root_page);

Result<bool> Btree_update(uint32_t page_num,Key& key,std::string& value,Pager_t& pager,Btree_Node& update_page);

Result<bool> Btree_delete(uint32_t page_num,Key& key,Pager_t& pager,StackPath& path,std::vector<uint32_t>& del_pages,std::vector<std::unique_ptr<Btree_Node>>& update_pages,uint32_t& out_root_page);

/* 清空整棵树：除根节点外全部标记为待删除页 */
Result<bool> Btree_delete_range(uint32_t page_num,Pager_t& pager,std::vector<uint32_t>& del_pages,uint32_t& out_root_page);

/* 沿栈向上逐层分裂；必要时新建根节点 */
bool Split_chain(Btree_Node& curpage,Pager_t& pager,StackPath& path,std::vector<std::unique_ptr<Btree_Node>>& new_pages);

/* 沿栈向上逐层借位/合并；必要时降低树高 */
bool Merge_chain(Btree_Node& curpage,Pager_t& pager,StackPath& path,std::vector<uint32_t>& del_pages,std::vector<std::unique_ptr<Btree_Node>>& update_pages,uint32_t& out_root_page);

bool create_new_root(uint32_t page1,uint32_t page2,Key& risekey,Btree_Node& new_root,uint32_t new_root_page);
