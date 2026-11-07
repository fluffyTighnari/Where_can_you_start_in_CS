#pragma once

/*
 * Pager：页缓存与磁盘 IO 层（可切换为 mock 实现，供 B+ 树单元测试使用）。
 *
 * 磁盘布局（每页 4096B）：
 *   page 0            主页面 mainpage：魔数、版本、页位图、系统表根页号
 *   page 1 ~ 511      数据页（位图中 1 表示已分配）
 *
 * 内存缓存：哈希表（页号 → Page*）做 O(1) 定位 + 双向链表做 LRU 顺序，
 * 链表头部是最近使用过的页，尾部是最久未用的。缓存满时从尾部淘汰。
 * Page.dirty 标记该页被修改过但尚未落盘，淘汰或 flush 时才真正写回。
 */
#ifdef BTREE_USE_MOCK_PAGER
#include "B+Tree/mockpager.hpp"
#else

#include <string>
#include <unordered_map>
#include <cstdint>
#include <memory>
#include <unistd.h>
#include <cstdio>
#include <vector>

#ifdef DEBUG
#include "../Help/Error/error.hpp"
#else
#include "Help/Error/error.hpp"
#endif

constexpr uint32_t PAGE_SIZE = 4096U;
constexpr uint32_t MAX_CACHED_PAGES = 64;   /* 内存中最多缓存的页数 */
constexpr uint32_t MAX_DB_PAGES = 512;      /* 数据库文件最多容纳的页数 */
constexpr uint32_t MAGIC = 0x66666666;      /* 主页面魔数 */

/* 缓存中的一个页：数据 + 脏位 + LRU 双向链表指针 */
typedef struct Page{
    uint32_t page_num;
    char data[PAGE_SIZE];
    bool dirty = false;
    Page* pre = nullptr;
    Page* next = nullptr;
}Page;

/* 主页面（page 0）：整个数据库文件的元信息 */
typedef struct mainpage{
    uint32_t magic;
    uint32_t page_size;
    uint32_t version;
    uint32_t pages_num;
    uint32_t bitmap[16]; /*max_pages : 16 * 32 = 512*/   /* 页分配位图，1 表示已占用 */
    uint32_t system_page; /*system_table 的 root_page*/
    uint8_t  reserved[PAGE_SIZE - 21*sizeof(uint32_t)];
}mainpage;

struct Pager_t{
    public:
    explicit Pager_t(const char* db_name);

    ~Pager_t();

    int get_fd() const;

    bool read_page(uint32_t page_num,char* buffer);

    bool write_page(uint32_t page_num);

    bool update_page(uint32_t page_num,const char* buffer);

    bool delete_page(uint32_t page_num);

    uint32_t alloc_new_page();

    bool is_page_allocated(uint32_t page_num) const;

    /* 把所有脏页与主页面刷到磁盘 */
    bool flush_page();

    bool new_db = false;/*如果是新库，需要在初始化符号表时写入新系统表*/

    std::unique_ptr<mainpage> main_page;

    private:

    /* LRU 链表维护 */
    void move_to_head(Page* page);

    void evict_if_needed();

    void remove_from_list(Page* page);

    int fd;
    std::unordered_map<uint32_t,Page*> hash;   /* 页号 → 缓存页 */
    Page* head = nullptr;                      /* 最近使用 */
    Page* tail = nullptr;                      /* 最久未用 */
    uint32_t current_page = 0;
};

Result<std::unique_ptr<Pager_t>> InitPager(const std::string& db_name);

#endif // BTREE_USE_MOCK_PAGER
