/*
 * StackPath.cpp —— B+ 树路径栈实现
 *
 * 关键设计：pop() 只把 curtop 减一，不销毁节点对象。
 * 节点仍留在 path 向量中，flush_all 才能遍历到"曾经在栈上、已被弹出但改过"的父节点。
 */
#ifdef BTREE_USE_MOCK_PAGER
#include "mockpager.hpp"
#else
#include "Storage_engine/pager.hpp"
#endif
#include "Btree.hpp"
#include "StackPath.hpp"
#include <iostream>

/* 当前逻辑栈深度 */
uint32_t StackPath::size() const{
    return static_cast<uint32_t>(curtop + 1);
}

void StackPath::push(std::unique_ptr<Btree_Node> page){
    path.push_back(std::move(page));
    curtop++;
    stacksize++;
}

/* 只移动栈顶索引，节点对象保留在 path 中供统一回写 */
void StackPath::pop(){
    curtop--;
}

Btree_Node& StackPath::top(){
    return *path[curtop];
}

bool StackPath::empty(){
    return curtop == -1;
}

void StackPath::clear(){
    path.clear();
    curtop = -1;
    stacksize = 0;
}

/* 遍历路径上所有节点，把脏节点写进页缓存后清空栈 */
bool StackPath::flush_all(Pager_t& pager){
    for(auto& node_ptr : path){
        if(node_ptr->is_dirty()){
            node_ptr->dirty = false;
            if(!pager.update_page(node_ptr->page_num, reinterpret_cast<char*>(node_ptr.get()))){
                return false;
            }
        }
    }
    clear();
    return true;
}

bool update_Path(Pager_t& pager,StackPath& path){
    if(!path.flush_all(pager)){
        return false;
    }
    return true;
}
