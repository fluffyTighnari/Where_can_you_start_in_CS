#pragma once
#include <vector>
#include <memory>
#include <cstdint>
#include "Help/Error/error.hpp"

/*
 * StackPath：B+ 树操作的根到叶路径栈。
 *
 * 插入/删除时，从根一路向下读页，把沿途每个节点压栈。这样在叶子发生分裂或合并后，
 * 可以沿栈回溯到父节点，继续处理级联分裂/合并，最后 flush_all 一次性写回。
 *
 * 注意 path 用 vector<unique_ptr> 保存所有节点，而 curtop 表示"当前逻辑栈顶"：
 * pop 只移动索引、不销毁对象，这样被弹出的节点仍留在 vector 里，
 * 供 flush_all 统一回写（否则被弹出的父节点修改就丢了）。
 */
struct Btree_Node;
struct Pager_t;

class StackPath{
    private:
    std::vector<std::unique_ptr<Btree_Node>> path;
    int curtop = -1;
    uint32_t stacksize = 0;

    public:
    uint32_t size() const;

    void push(std::unique_ptr<Btree_Node> page);

    void pop();

    Btree_Node& top();

    bool empty();

    void clear();

    /* 把所有脏节点写回页缓存，然后清空栈 */
    bool flush_all(Pager_t& pager);
};

bool update_Path(Pager_t& pager,StackPath& path);
