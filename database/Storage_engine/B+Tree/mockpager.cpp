/*
 * mockpager.cpp —— 纯内存页管理器，仅供 B+ 树单元测试使用
 *
 * 与真实 Pager_t 保持相同的接口，但把所有页存在一个哈希表里，不碰磁盘。
 * 这样 test_btree 可以脱离 sql.db 独立验证 B+ 树的分裂/合并逻辑。
 * 编译时定义 BTREE_USE_MOCK_PAGER 即启用本实现。
 */
#include "mockpager.hpp"
#include "Btree.hpp"
#include <iostream>

Pager_t::Pager_t() {
    main_page = std::make_unique<mainpage>();
    memset(main_page.get(), 0, sizeof(mainpage));
    main_page->magic = MAGIC;
    main_page->page_size = PAGE_SIZE;
    main_page->version = 1;
    main_page->pages_num = 0;
    // 新库：系统表未初始化（system_page = 0）
    // 第一次 CREATE TABLE 时由 executor 初始化系统表
    main_page->system_page = 0;
    new_db = true;
}

int Pager_t::get_fd() const {
    return -1;
}

/* 读页：内存中不存在即视为非法页号 */
bool Pager_t::read_page(uint32_t page_num, char* buffer) {
    auto it = pages.find(page_num);
    if (it == pages.end()) return false;
    memcpy(buffer, it->second.get(), PAGE_SIZE);
    return true;
}

bool Pager_t::write_page(uint32_t page_num, const char* buffer) {
    auto copy = std::make_unique<char[]>(PAGE_SIZE);
    memcpy(copy.get(), buffer, PAGE_SIZE);
    pages[page_num] = std::move(copy);
    return true;
}

/* update 与 write 的区别：已存在则原地覆盖，不存在则新建 */
bool Pager_t::update_page(uint32_t page_num, const char* buffer) {
    auto it = pages.find(page_num);
    if (it == pages.end()) {
        return write_page(page_num, buffer);
    }
    memcpy(it->second.get(), buffer, PAGE_SIZE);
    return true;
}

bool Pager_t::delete_page(uint32_t page_num) {
    if (!is_page_allocated(page_num)) {
        return false;
    }
    pages.erase(page_num);
    uint32_t idx = page_num / 32;
    uint32_t bit = page_num % 32;
    main_page->bitmap[idx] &= ~(1u << bit);
    main_page->pages_num--;
    return true;
}

uint32_t Pager_t::alloc_new_page() {
    for (int i = 0; i < 16; i++) {
        uint32_t temp = main_page->bitmap[i];
        for (int j = 0; j < 32; j++) {
            if (((temp >> j) & 1) == 0) {
                uint32_t page_num = i * 32 + j;
                if (page_num == 0) continue;
                auto zero = std::make_unique<char[]>(PAGE_SIZE);
                memset(zero.get(), 0, PAGE_SIZE);
                pages[page_num] = std::move(zero);
                main_page->bitmap[i] |= (1u << j);
                main_page->pages_num++;
                return page_num;
            }
        }
    }
    return 0;
}

bool Pager_t::is_page_allocated(uint32_t page_num) const {
    if (page_num == 0) return true;
    if (page_num >= MAX_DB_PAGES) return false;
    auto it = pages.find(page_num);
    return it != pages.end();
}

bool Pager_t::flush_page() {
    return true;
}

/*
 * 调试用：把内存中所有页按节点结构打印出来。
 * 定义 DEBUG 时调用 Btree_Node::print 输出完整内容，否则只输出摘要。
 */
void Pager_t::dump_all() {
#ifdef DEBUG
    for (auto& [num, buf] : pages) {
        Btree_Node* node = reinterpret_cast<Btree_Node*>(buf.get());
        node->print();
    }
#else
    for (auto& [num, buf] : pages) {
        Btree_Node* node = reinterpret_cast<Btree_Node*>(buf.get());
        fprintf(stderr, "page=%u type=%s key_num=%u root=%d father=%u\n",
                num, node->is_leaf() ? "LEAF" : "MID", node->key_number, node->root, node->father);
    }
#endif
}
