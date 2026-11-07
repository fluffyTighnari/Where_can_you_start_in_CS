/*
 * pager.cpp —— 页缓存实现（LRU 淘汰）
 *
 * Windows 与 POSIX 两套 IO 分支：
 *   POSIX 用 pread / pwrite —— 无文件偏移的隐式状态，可安全地"任意位置读写"
 *   Windows 用 _lseek + _read/_write 组合
 * 两套接口统一封装在本文件内，上层无感知。
 */
#include "pager.hpp"
#include <memory>
#include <cstring>
#include <cstdio>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _S_IREAD
#define _S_IREAD 0x0100
#endif
#ifndef _S_IWRITE
#define _S_IWRITE 0x0080
#endif
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

Pager_t::Pager_t(const char* db_name){
    /* 只创建/打开文件，真正的初始化（判断是否新库）在 InitPager 中 */
#ifdef _WIN32
    fd = _open(db_name, _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    fd = open(db_name, O_RDWR | O_CREAT, 0644);
#endif
    main_page = std::make_unique<mainpage>();
}

/* 析构时先落盘再释放所有缓存页，保证没有未写回的数据 */
Pager_t::~Pager_t(){
    flush_page();
    if(fd >= 0){
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
    }
    for(auto& pair : hash){
        delete pair.second;
    }
    hash.clear();
}

/* 把缓存中所有脏页写回，最后把主页面写到 page 0 */
bool Pager_t::flush_page(){
    for(auto& [page_num,page] : hash){
        if(page->dirty){
            if(!write_page(page_num)){
                return false;
            }
        }
    }
#ifdef _WIN32
    int ret = _lseek(fd, 0, SEEK_SET);
    if(ret == -1) return false;
    int n = _write(fd, main_page.get(), PAGE_SIZE);
    if(n != PAGE_SIZE) return false;
#else
    ssize_t ret = pwrite(fd, main_page.get(), PAGE_SIZE, 0);
    if(ret != PAGE_SIZE) return false;
#endif
    return true;
}

/* 把页移到链表头部，标记为最近使用 */
void Pager_t::move_to_head(Page* page) {
    if(page == head){
        return;
    }
    remove_from_list(page);
    page->next = head;
    page->pre = nullptr;
    if(head){
        head->pre = page;
    }
    head = page;
    if(!tail){
        tail = page;
    }
}

/* 从双向链表摘除，注意同时处理头尾指针 */
void Pager_t::remove_from_list(Page* page) {
    if(page->pre){
        page->pre->next = page->next;
    }
    else{
        head = page->next;
    }
    if(page->next){
        page->next->pre = page->pre;
    }
    else{
        tail = page->pre;
    }
    page->pre = page->next = nullptr;
}

/*
 * 缓存超过上限时，从链表尾部（最久未使用）开始淘汰。
 * 淘汰前若页是脏的，必须先写回磁盘，否则数据丢失。
 */
void Pager_t::evict_if_needed() {
    while (hash.size() > MAX_CACHED_PAGES && tail) {
        Page* temp = tail;
        if(temp->dirty){
            write_page(temp->page_num);
        }
        remove_from_list(temp);
        hash.erase(temp->page_num);
        delete temp;
    }
}

/* 查询位图中某页是否已分配；page 0 永远视为已分配（主页面） */
bool Pager_t::is_page_allocated(uint32_t page_num) const {
    if (page_num == 0) return true;
    if (page_num >= MAX_DB_PAGES) return false;
    uint32_t idx = page_num / 32;
    uint32_t bit = page_num % 32;
    return (main_page->bitmap[idx] >> bit) & 1;
}

/*
 * 打开数据库文件并初始化主页面。
 * 文件长度为 0 → 新库，写入带魔数的初始主页面并标记 new_db；
 * 否则读入主页面并校验魔数，防止把别的文件当数据库打开。
 */
Result<std::unique_ptr<Pager_t>> InitPager(const std::string& db_name){
    auto target_pager = std::make_unique<Pager_t>(db_name.c_str());
    if(target_pager->get_fd() < 0){
        std::string temp = "failed to open the file:";
        temp.append(db_name);
        return Error(temp,Error::Layer::Pager);
    }

#ifdef _WIN32
    struct _stati64 st{};
    _fstati64(target_pager->get_fd(), &st);
    __int64 file_size = st.st_size;
#else
    struct stat st{};
    fstat(target_pager->get_fd(),&st);
    off_t file_size = st.st_size;
#endif

    std::string error;
    if(file_size == 0){
        target_pager->main_page->magic = 0x66666666;
        target_pager->main_page->page_size = PAGE_SIZE;
        target_pager->main_page->version = 1;
        target_pager->main_page->pages_num = 0;
        target_pager->main_page->system_page = 0;
        target_pager->new_db = true;

#ifdef _WIN32
        _lseek(target_pager->get_fd(), 0, SEEK_SET);
        int res = _write(target_pager->get_fd(), (void*)target_pager->main_page.get(), PAGE_SIZE);
        if(res < 0){
            return Error("failed to write the file",Error::Layer::Pager);
        }
#else
        ssize_t res = pwrite(target_pager->get_fd(),(void*)target_pager->main_page.get(),PAGE_SIZE,0);
        if(res < 0){
            return Error("failed to write the file",Error::Layer::Pager);
        }
#endif
    }
    else{
#ifdef _WIN32
        _lseek(target_pager->get_fd(), 0, SEEK_SET);
        int n = _read(target_pager->get_fd(), (void*)target_pager->main_page.get(), PAGE_SIZE);
        if(n < 0){
            return Error("failed to read the file",Error::Layer::Pager);
        }
#else
        ssize_t n = pread(target_pager->get_fd(),(void*)target_pager->main_page.get(),PAGE_SIZE,0);
        if(n < 0){
            return Error("failed to write the file",Error::Layer::Pager);
        }
#endif
        if(target_pager->main_page->magic != MAGIC){
            return Error("invalid magic",Error::Layer::Pager);
        }
    }
    return target_pager;
}

int Pager_t::get_fd() const{
    return fd;
}

/*
 * 读页：命中缓存则直接拷贝并提升为最近使用；未命中则从磁盘读入、
 * 插入链表头部与哈希表，然后再检查是否需要淘汰。
 */
bool Pager_t::read_page(uint32_t page_num,char* buffer){
    if(!is_page_allocated(page_num)){
        return false;
    }
    auto it = hash.find(page_num);
    if(it != hash.end()){
        Page* page = it->second;
        move_to_head(page);
        memcpy(buffer, page->data, PAGE_SIZE);
        return true;
    }
    else{
        Page* page = new Page;
#ifdef _WIN32
        _lseek(fd, page_num * PAGE_SIZE, SEEK_SET);
        int size = _read(fd, page->data, PAGE_SIZE);
        if(size == -1){
            delete page;
            return false;
        }
#else
        ssize_t size = pread(fd,page->data,PAGE_SIZE,page_num * PAGE_SIZE);
        if(size == -1){
            perror("pread");
            delete page;
            return false;
        }
#endif
        page->page_num = page_num;
        hash[page_num] = page;

        evict_if_needed();

        page->pre = nullptr;
        page->next = head;
        if(head != nullptr){
            head->pre = page;
        }
        head = page;
        if(tail == nullptr){
            tail = page;
        }
        memcpy(buffer,page->data,PAGE_SIZE);
    }
    return true;
}

/*
 * 把指定页从缓存写回磁盘。
 * 页不在缓存中、或不是脏页，都视为已完成（无需 IO）。
 */
bool Pager_t::write_page(uint32_t page_num){
    if(!hash.count(page_num)){
        return true;
    }
    Page* page = hash[page_num];
    if(!page->dirty){
        return true;
    }
#ifdef _WIN32
    _lseek(fd, page_num * PAGE_SIZE, SEEK_SET);
    int n = _write(fd, page->data, PAGE_SIZE);
    if(n != PAGE_SIZE){
        return false;
    }
#else
    ssize_t n = pwrite(fd,page->data,PAGE_SIZE,page_num * PAGE_SIZE);
    if(n != PAGE_SIZE){
        return false;
    }
#endif
    page->dirty = false;
    return true;
}

/*
 * 更新页内容：写入缓存并置脏位，不立即落盘。
 * 页不在缓存中时就新建一个缓存项，同样按 LRU 规则维护链表。
 */
bool Pager_t::update_page(uint32_t page_num,const char* buffer){
    if(!is_page_allocated(page_num)){
        return false;
    }
    if(hash.count(page_num)){
        Page* page = hash[page_num];
        memcpy(page->data,buffer,PAGE_SIZE);
        move_to_head(page);
        page->dirty = true;
    }
    else{
        Page* page = new Page();
        page->page_num = page_num;
        memcpy(page->data, buffer, PAGE_SIZE);
        page->dirty = true;
        hash[page_num] = page;

        page->pre = nullptr;
        page->next = head;
        if (head != nullptr){
            head->pre = page;
        }
        head = page;
        if (tail == nullptr){
            tail = page;
        }
        evict_if_needed();
    }
    return true;
}

/* 释放一页：清缓存项 + 清位图 + 更新页计数。磁盘上的旧数据不需要擦除。 */
bool Pager_t::delete_page(uint32_t page_num){
    if(!is_page_allocated(page_num)){
        return false;
    }

    if(hash.count(page_num)){
        Page* page = hash[page_num];
        remove_from_list(page);
        delete page;
        hash.erase(page_num);
    }
    uint32_t idx = page_num / 32;
    uint32_t bit = page_num % 32;
    main_page->bitmap[idx] &= ~(1 << bit);
    main_page->pages_num--;
    return true;
}

/*
 * 分配新页：在位图（16 个 uint32 = 512 bit）中找第一个空闲位。
 * page 0 是主页面，必须跳过。
 * 新页先整体清零写入磁盘，再更新位图并返回页号；找不到空闲页返回 0。
 */
uint32_t Pager_t::alloc_new_page(){
    for(int i = 0; i < 16; i++){
        uint32_t temp = main_page->bitmap[i];
        for(int j = 0; j < 32; j++){
            if(((temp >> j) & 1) == 0){
                uint32_t page_num = i*32 + j;
                if (page_num == 0) continue;
                char zero[PAGE_SIZE] = {0};
#ifdef _WIN32
                _lseek(fd, page_num * PAGE_SIZE, SEEK_SET);
                if(_write(fd, zero, PAGE_SIZE) != PAGE_SIZE){
                    return 0;
                }
#else
                if(pwrite(fd,zero,PAGE_SIZE,page_num * PAGE_SIZE) != PAGE_SIZE){
                    return 0;
                }
#endif
                main_page->bitmap[i] |= (1 << j);
                main_page->pages_num++;
                return page_num;
            }
        }
    }
    return 0;
}
