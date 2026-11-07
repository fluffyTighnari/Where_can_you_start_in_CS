#pragma once
#include <vector>
#include <memory>
#include "Help/Error/error.hpp"

/*
 * Storage：存储引擎门面。
 *
 * 持有 Pager（页缓存 / 磁盘 IO）与 SymbolTable（表元信息）两个组件，
 * 上层（Executor）只通过本类访问它们，不直接接触二者的所有权。
 *
 * 用 unique_ptr 独占持有，release_pager() 可把 Pager 的所有权交出去——
 * 这是为了在初始化阶段先建 Pager、再建 SymbolTable（需要读 Pager），
 * 最后把两者一起装进 Storage。缺陷：初始化后 Storage 就不可分割了。
 */
struct SymbolTable;
struct Pager_t;

class Storage{
    private:
    std::unique_ptr<Pager_t> pager;
    std::unique_ptr<SymbolTable> symbol_table;

    public:
    Storage(std::unique_ptr<Pager_t> pager_t,std::unique_ptr<SymbolTable> symbol_table_t);

    const Pager_t& get_Pager() const;

    Pager_t& get_Pager();

    const SymbolTable& get_symboltable() const;

    SymbolTable& get_symboltable();

    /* 返回裸指针，便于在只读上下文中使用而不转移所有权 */
    const SymbolTable* acquire_const_SymbolTable() const;

    SymbolTable* acquire_SymbolTable();

    /* 交出 Pager 的所有权，调用后本 Storage 不再持有 pager */
    std::unique_ptr<Pager_t> release_pager();
};

Result<std::unique_ptr<Storage>> InitStorage(std::unique_ptr<Pager_t> pager_t,std::unique_ptr<SymbolTable> symbol_table_t);
