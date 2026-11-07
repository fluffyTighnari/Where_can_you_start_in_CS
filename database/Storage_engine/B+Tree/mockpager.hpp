#pragma once
#include <unordered_map>
#include <cstdint>
#include <memory>
#include <cstring>
#include <string>

/*
 * mockpager.hpp —— 纯内存页管理器（仅用于 B+ 树单元测试）
 *
 * 提供与真实 Pager_t 一致的接口，但页数据存放在哈希表中，不落盘。
 * 定义 BTREE_USE_MOCK_PAGER 时，pager.hpp 会改为包含本文件，
 * 于是 B+ 树代码无需修改即可在内存中跑测试。
 */

#define PAGE_SIZE 4096U
constexpr uint32_t MAX_DB_PAGES = 512;
constexpr uint32_t MAGIC = 0x66666666;

/* 与真实主页面布局保持一致，便于共用同一套页分配逻辑 */
struct mainpage {
    uint32_t magic;
    uint32_t page_size;
    uint32_t version;
    uint32_t pages_num;
    uint32_t bitmap[16];
    uint32_t system_page;
    uint8_t  reserved[PAGE_SIZE - 21 * sizeof(uint32_t)];
};

class Pager_t {
public:
    std::unordered_map<uint32_t, std::unique_ptr<char[]>> pages;   /* 页号 → 页内容 */
    uint32_t next_page = 1;
    bool new_db = true;
    std::unique_ptr<mainpage> main_page;

    Pager_t();

    int get_fd() const;

    bool read_page(uint32_t page_num, char* buffer);

    bool write_page(uint32_t page_num, const char* buffer);

    bool update_page(uint32_t page_num, const char* buffer);

    bool delete_page(uint32_t page_num);

    uint32_t alloc_new_page();

    bool is_page_allocated(uint32_t page_num) const;

    bool flush_page();

    void dump_all();
};
