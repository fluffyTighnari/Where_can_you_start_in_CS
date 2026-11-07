/*
 * allocator.h —— 简易用户态内存分配器（对外接口）
 *
 * 以 sbrk() 扩展堆，用双向空闲链表管理空闲块，块头隐藏在每个用户块之前。
 * 本分配器为单线程实现，不可重入，也不提供线程安全保证。
 *
 * 语义遵循 C 标准库：
 *   - my_malloc(0) 返回 NULL
 *   - my_free(NULL) 什么也不做
 *   - my_realloc(NULL, n) 等价于 my_malloc(n)
 *   - my_realloc(p, 0) 等价于 my_free(p) 并返回 NULL
 */
#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 堆内统计信息，用于测试与调试（正常业务无需关心） */
typedef struct allocator_stats {
    size_t heap_bytes;        /* sbrk 已向系统申请的总字节数 */
    size_t allocated_bytes;   /* 已分配块的用户可用字节总和 */
    size_t free_bytes;        /* 空闲块的用户可用字节总和 */
    size_t block_count;       /* 堆内所有块（含空闲块）的数量 */
    size_t free_block_count;  /* 空闲块数量 */
} allocator_stats;

/* allocator_validate() 的返回码 */
enum {
    ALLOCATOR_OK                   = 0,
    ALLOCATOR_ERR_HEAP_TRUNCATED   = -1, /* 块链无法恰好铺满整个堆 */
    ALLOCATOR_ERR_BAD_MAGIC        = -2, /* 块头魔术字损坏（野指针/越界写） */
    ALLOCATOR_ERR_BAD_SIZE         = -3, /* 块大小非法（为 0 或未对齐） */
    ALLOCATOR_ERR_BLOCK_OVERRUN    = -4, /* 块大小越出堆边界 */
    ALLOCATOR_ERR_BAD_PREV_SIZE    = -5, /* 前一块大小与实际不符 */
    ALLOCATOR_ERR_ADJACENT_FREE    = -6, /* 出现两个物理相邻的空闲块（未合并） */
    ALLOCATOR_ERR_FREE_LIST        = -7, /* 空闲链表结构损坏 */
    ALLOCATOR_ERR_STRAY_LINK       = -8, /* 已分配块仍挂着空闲链表指针 */
    ALLOCATOR_ERR_BAD_ALIGN        = -9, /* 用户指针未对齐 */
    ALLOCATOR_ERR_INVALID_ARG      = -10 /* 参数非法 */
};

void *my_malloc(size_t size);
void  my_free(void *ptr);
void *my_realloc(void *ptr, size_t size);

/* 采集统计信息，成功返回 ALLOCATOR_OK */
int allocator_get_stats(allocator_stats *out);

/* 遍历堆与空闲链表校验全部不变量，成功返回 ALLOCATOR_OK，否则返回上面的错误码 */
int allocator_validate(void);

#ifdef __cplusplus
}
#endif

#endif /* ALLOCATOR_H */
