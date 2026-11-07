/*
 * allocator.c —— 简易用户态内存分配器（实现）
 *
 * 物理布局：堆由若干段连续内存（arena）组成，每段内部是一串首尾相接的块。
 *
 *   arena.begin                                          arena.end
 *   |                                                     |
 *   v                                                     v
 *   [header][user...][header][user...][header][user...]
 *
 * 块头的 size 记录"用户区字节数"，因此下一块 = 本块 + header_size + size；
 * prev_size 记录前一块的用户区字节数（0 表示本段首块），用于向低地址合并。
 * 所有块大小与块头大小都是 ALIGNMENT 的整数倍，堆段起点也按 ALIGNMENT 对齐，
 * 因此块头天然对齐，用户指针也天然对齐。
 *
 * 之所以分成多段：sbrk 的 program break 是进程级资源，libc 的 malloc/printf
 * 同样会用 brk 扩展堆。若强行假设 break 紧跟自己上次的结尾，一旦别的代码推进
 * 了 break 就只能分配失败。这里改为发现 break 不再紧邻上一段时另开新段，
 * 只在段内做合并，既保持块链连续又不与他人互相踩踏。
 *
 * 注意：本分配器不线程安全；同一进程内不要与 libc malloc 交替分配再交替释放
 * （libc 可能 trim 顶层 chunk，把 break 收缩到我们段内），推荐整体替换 malloc。
 */
#define _DEFAULT_SOURCE

#include "allocator.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define BLOCK_MAGIC 0x5A2B10C4u
#define BLOCK_FREE  1u
#define ALIGNMENT   16u
#define MIN_SPLIT   16u  /* 分裂出的空闲块至少要能容纳这么多用户字节，否则不分裂 */
#define MAX_ARENAS  64u  /* 堆段上限，超出后 my_malloc 返回 NULL */
#define SBRK_FAILED ((void *)-1)

typedef struct block_header {
    uint32_t magic;           /* 块头完整性校验，检测野指针与越界写 */
    size_t size;              /* 用户区字节数（不含块头） */
    size_t prev_size;         /* 前一块的用户区字节数，0 表示所在段的首块 */
    struct block_header *next; /* 空闲链表指针，仅空闲块有效 */
    struct block_header *prev;
    uint32_t flags;
} block_header;

typedef struct arena_span {
    char *begin;
    char *end; /* 开区间 */
} arena_span;

static arena_span g_arenas[MAX_ARENAS];
static size_t g_arena_count;
static block_header *g_heap_tail; /* 最新一段的最后一块 */
static block_header *g_free_list;

static size_t align_up(size_t n)
{
    return (n + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1);
}

static size_t header_size(void)
{
    return align_up(sizeof(block_header));
}

static size_t block_span(const block_header *h)
{
    return header_size() + h->size;
}

static void *user_ptr(block_header *h)
{
    return (char *)h + header_size();
}

/* 用整数运算算块头地址：非法指针（如 0x10）也不会触发指针运算 UB */
static block_header *header_of(const void *p)
{
    return (block_header *)((uintptr_t)p - header_size());
}

static int aligned(const void *p)
{
    return ((uintptr_t)p & (ALIGNMENT - 1)) == 0;
}

static arena_span *find_arena(const void *p)
{
    uintptr_t addr = (uintptr_t)p;
    for (size_t i = 0; i < g_arena_count; i++) {
        if (addr >= (uintptr_t)g_arenas[i].begin && addr < (uintptr_t)g_arenas[i].end) {
            return &g_arenas[i];
        }
    }
    return NULL;
}

/* 一个块头必须完整落在某个段内才可能是本分配器的块 */
static int in_heap_block(const void *p)
{
    uintptr_t addr = (uintptr_t)p;
    arena_span *a = find_arena(p);
    return a != NULL && addr + header_size() <= (uintptr_t)a->end;
}

static block_header *block_next(const block_header *h)
{
    char *end = (char *)h + block_span(h);
    arena_span *a = find_arena(h);
    if (a == NULL || end >= a->end) {
        return NULL; /* 段尾之后可能是别的代码的内存，绝不越界访问 */
    }
    return (block_header *)end;
}

static block_header *block_prev(const block_header *h)
{
    if (h->prev_size == 0) {
        return NULL;
    }
    block_header *p = (block_header *)((char *)h - header_size() - h->prev_size);
    arena_span *a = find_arena(h);
    if (a == NULL || (char *)p < a->begin) {
        return NULL;
    }
    return p;
}

static void free_list_insert(block_header *h)
{
    h->flags |= BLOCK_FREE;
    h->prev = NULL;
    h->next = g_free_list;
    if (g_free_list != NULL) {
        g_free_list->prev = h;
    }
    g_free_list = h;
}

static void free_list_remove(block_header *h)
{
    if (h->prev != NULL) {
        h->prev->next = h->next;
    } else {
        g_free_list = h->next;
    }
    if (h->next != NULL) {
        h->next->prev = h->prev;
    }
    h->prev = NULL;
    h->next = NULL;
    h->flags &= ~BLOCK_FREE;
}

/*
 * 合并只发生在段内相邻块之间。调用前提：h 已标记为空闲但尚未进入空闲链表，
 * 因此这两个函数只负责把邻居块从链表里摘出来并进 h。
 */
static block_header *merge_with_prev(block_header *h)
{
    block_header *p = block_prev(h);
    if (p == NULL || (p->flags & BLOCK_FREE) == 0) {
        return h;
    }
    free_list_remove(p);
    p->size += block_span(h);
    block_header *n = block_next(p);
    if (n != NULL) {
        n->prev_size = p->size;
    }
    if (g_heap_tail == h) {
        g_heap_tail = p;
    }
    return p;
}

static block_header *merge_with_next(block_header *h)
{
    block_header *n = block_next(h);
    if (n == NULL || (n->flags & BLOCK_FREE) == 0) {
        return h;
    }
    free_list_remove(n);
    h->size += block_span(n);
    block_header *nn = block_next(h);
    if (nn != NULL) {
        nn->prev_size = h->size;
    }
    if (g_heap_tail == n) {
        g_heap_tail = h;
    }
    return h;
}

/* 把 h 削减到 need 字节，尾部残余作为空闲块归还，并与后继空闲块合并 */
static void split_block(block_header *h, size_t need)
{
    if (h->size < need + header_size() + MIN_SPLIT) {
        return;
    }
    size_t rest = h->size - need - header_size();
    block_header *tail = (block_header *)((char *)h + header_size() + need);
    tail->magic = BLOCK_MAGIC;
    tail->size = rest;
    tail->prev_size = need;
    tail->flags = 0;
    tail->next = NULL;
    tail->prev = NULL;
    h->size = need;
    if (g_heap_tail == h) {
        g_heap_tail = tail;
    }
    /*
     * 原本位于 h 之后的块，其 prev_size 记录的是分裂前 h 的整块大小，
     * 现在紧邻它的是 tail，必须同步改成 rest，否则向低地址合并时会算出越界地址。
     */
    block_header *after_tail = block_next(tail);
    if (after_tail != NULL) {
        after_tail->prev_size = rest;
    }
    free_list_insert(tail);
    merge_with_next(tail);
}

/* 向系统申请一块正好容纳 user_size 的新块，接到最新一段或另开一段 */
static block_header *grow_heap(size_t user_size)
{
    size_t need = header_size() + align_up(user_size);
    char *brk = (char *)sbrk(0);
    if (brk == SBRK_FAILED) {
        return NULL;
    }

    int extend = g_arena_count > 0 &&
                 (char *)((uintptr_t)g_arenas[g_arena_count - 1].end) == brk;
    char *start;
    size_t request = need;

    if (extend) {
        start = brk; /* 段长都是 ALIGNMENT 的倍数，断点必然已对齐 */
    } else {
        if (g_arena_count >= MAX_ARENAS) {
            return NULL;
        }
        size_t pad = (ALIGNMENT - ((uintptr_t)brk & (ALIGNMENT - 1))) & (ALIGNMENT - 1);
        start = brk + pad;
        request += pad; /* 一次 sbrk 同时预留对齐填充，避免两次调用之间的竞态 */
    }

    if (sbrk((intptr_t)request) == SBRK_FAILED) {
        return NULL;
    }

    block_header *h = (block_header *)start;
    h->magic = BLOCK_MAGIC;
    h->size = need - header_size();
    h->prev_size = extend ? g_heap_tail->size : 0;
    h->flags = 0;
    h->next = NULL;
    h->prev = NULL;

    if (extend) {
        g_arenas[g_arena_count - 1].end = start + need;
    } else {
        g_arenas[g_arena_count].begin = start;
        g_arenas[g_arena_count].end = start + need;
        g_arena_count++;
    }
    g_heap_tail = h;
    return h;
}

/* 校验一个用户指针是本分配器分配的、仍然存活的块；失败返回 NULL */
static block_header *live_header(void *ptr)
{
    if (ptr == NULL || !aligned(ptr)) {
        return NULL;
    }
    block_header *h = header_of(ptr);
    if (!aligned(h) || !in_heap_block(h)) {
        return NULL;
    }
    if (h->magic != BLOCK_MAGIC || (h->flags & BLOCK_FREE) != 0) {
        return NULL;
    }
    return h;
}

void *my_malloc(size_t size)
{
    if (size == 0 || size > SIZE_MAX - (ALIGNMENT - 1)) {
        return NULL;
    }
    size_t need = align_up(size);

    for (block_header *cur = g_free_list; cur != NULL; cur = cur->next) {
        if (cur->size >= need) {
            free_list_remove(cur);
            split_block(cur, need);
            return user_ptr(cur);
        }
    }

    block_header *h = grow_heap(need);
    if (h == NULL) {
        return NULL;
    }
    return user_ptr(h);
}

void my_free(void *ptr)
{
    block_header *h = live_header(ptr);
    if (h == NULL) {
        return; /* 空指针、野指针、未对齐指针、重复释放一律忽略，保住堆结构 */
    }
    h->flags |= BLOCK_FREE;
    h = merge_with_prev(h);
    h = merge_with_next(h);
    h->next = NULL;
    h->prev = NULL;
    free_list_insert(h);
}

void *my_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return my_malloc(size);
    }
    if (size == 0 || size > SIZE_MAX - (ALIGNMENT - 1)) {
        my_free(ptr);
        return NULL;
    }
    block_header *h = live_header(ptr);
    if (h == NULL) {
        return NULL;
    }
    size_t need = align_up(size);

    if (need <= h->size) {
        split_block(h, need);
        return ptr;
    }

    /* 原地扩容只可能借助紧邻其后的空闲块，先算清楚再决定是否搬迁 */
    size_t avail = h->size;
    block_header *next = block_next(h);
    if (next != NULL && (next->flags & BLOCK_FREE) != 0) {
        avail += block_span(next);
    }
    if (avail >= need) {
        merge_with_next(h);
        split_block(h, need);
        return ptr;
    }

    void *fresh = my_malloc(size);
    if (fresh == NULL) {
        return NULL; /* 失败时原块保持原样，调用者仍拥有它 */
    }
    /* 只在旧块实际拥有的字节内拷贝，避免读越界 */
    memcpy(fresh, ptr, h->size < size ? h->size : size);
    my_free(ptr);
    return fresh;
}

int allocator_get_stats(allocator_stats *out)
{
    if (out == NULL) {
        return ALLOCATOR_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < g_arena_count; i++) {
        out->heap_bytes += (size_t)(g_arenas[i].end - g_arenas[i].begin);
        for (char *p = g_arenas[i].begin; p < g_arenas[i].end;) {
            block_header *h = (block_header *)p;
            out->block_count++;
            if (h->flags & BLOCK_FREE) {
                out->free_bytes += h->size;
                out->free_block_count++;
            } else {
                out->allocated_bytes += h->size;
            }
            p += block_span(h);
        }
    }
    return ALLOCATOR_OK;
}

int allocator_validate(void)
{
    size_t total_free = 0;

    for (size_t i = 0; i < g_arena_count; i++) {
        arena_span *a = &g_arenas[i];
        if (a->begin == NULL || a->end == NULL || a->begin >= a->end) {
            return ALLOCATOR_ERR_HEAP_TRUNCATED;
        }
        if (!aligned(a->begin)) {
            return ALLOCATOR_ERR_BAD_ALIGN;
        }
        if (i > 0 && a->begin <= g_arenas[i - 1].end) {
            return ALLOCATOR_ERR_HEAP_TRUNCATED; /* 段之间不得重叠 */
        }

        size_t prev_size = 0;
        block_header *prev = NULL;
        char *p = a->begin;
        while (p < a->end) {
            if ((size_t)(a->end - p) < header_size()) {
                return ALLOCATOR_ERR_HEAP_TRUNCATED;
            }
            block_header *h = (block_header *)p;
            if (h->magic != BLOCK_MAGIC) {
                return ALLOCATOR_ERR_BAD_MAGIC;
            }
            if (h->size == 0 || (h->size & (ALIGNMENT - 1)) != 0) {
                return ALLOCATOR_ERR_BAD_SIZE;
            }
            if (h->prev_size != prev_size) {
                return ALLOCATOR_ERR_BAD_PREV_SIZE;
            }
            if (!aligned(user_ptr(h))) {
                return ALLOCATOR_ERR_BAD_ALIGN;
            }
            if (block_span(h) > (size_t)(a->end - p)) {
                return ALLOCATOR_ERR_BLOCK_OVERRUN;
            }
            if (prev != NULL && (prev->flags & BLOCK_FREE) && (h->flags & BLOCK_FREE)) {
                return ALLOCATOR_ERR_ADJACENT_FREE;
            }
            if ((h->flags & BLOCK_FREE) == 0 && (h->next != NULL || h->prev != NULL)) {
                return ALLOCATOR_ERR_STRAY_LINK;
            }
            if (h->flags & BLOCK_FREE) {
                total_free++;
            }
            prev_size = h->size;
            prev = h;
            p += block_span(h);
        }
        if (p != a->end) {
            return ALLOCATOR_ERR_HEAP_TRUNCATED;
        }
    }

    /* 空闲链表必须无环、双向一致，且恰好包含堆内全部空闲块 */
    size_t listed = 0;
    block_header *prev_node = NULL;
    for (block_header *cur = g_free_list; cur != NULL; cur = cur->next) {
        if (++listed > total_free) {
            return ALLOCATOR_ERR_FREE_LIST;
        }
        if (!in_heap_block(cur) || cur->magic != BLOCK_MAGIC) {
            return ALLOCATOR_ERR_FREE_LIST;
        }
        if ((cur->flags & BLOCK_FREE) == 0 || cur->prev != prev_node) {
            return ALLOCATOR_ERR_FREE_LIST;
        }
        prev_node = cur;
    }
    if (listed != total_free) {
        return ALLOCATOR_ERR_FREE_LIST;
    }
    return ALLOCATOR_OK;
}
