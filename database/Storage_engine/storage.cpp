/*
 * storage.cpp —— 存储引擎门面实现
 *
 * 本文件只有两类内容：
 *   1. 构造与所有权转移
 *   2. 访问器（getter / acquire）
 * 真正的读写逻辑在 pager.cpp（页）与 Btree.cpp（索引）中。
 */
#include <memory>
#include "storage.hpp"
#include "Parse/sense.hpp"
#include "pager.hpp"

Storage::Storage(std::unique_ptr<Pager_t> pager_t,std::unique_ptr<SymbolTable> symbol_table_t)
    : pager(std::move(pager_t)), symbol_table(std::move(symbol_table_t)) {}

const Pager_t& Storage::get_Pager() const{
    return *pager;
}

Pager_t& Storage::get_Pager(){
    return *pager;
}

const SymbolTable& Storage::get_symboltable() const{
    return *symbol_table;
}

SymbolTable& Storage::get_symboltable(){
    return *symbol_table;
}

const SymbolTable* Storage::acquire_const_SymbolTable() const{
    return symbol_table.get();
}

SymbolTable* Storage::acquire_SymbolTable(){
    return symbol_table.get();
}

std::unique_ptr<Pager_t> Storage::release_pager(){
    return std::move(pager);
}

/* 初始化：把 pager 与 symbol_table 的所有权移入 Storage */
Result<std::unique_ptr<Storage>> InitStorage(std::unique_ptr<Pager_t> pager_t,std::unique_ptr<SymbolTable> symbol_table_t){
    std::unique_ptr<Storage> st = std::make_unique<Storage>(std::move(pager_t),std::move(symbol_table_t));
    return st;
}
