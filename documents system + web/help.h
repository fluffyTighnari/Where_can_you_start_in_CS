/*
 * help.h —— 文件系统基础层
 *
 * 磁盘布局（1MB 镜像 = 2048 块 × 512B）：
 *   block 0            超级块 Super_block
 *   block 1            inode 位图（每个 bit 对应一个 inode 是否占用）
 *   block 2            数据块位图（每个 bit 对应一个数据块是否占用）
 *   block 3 ~ 10       inode 表（8 块，每块 8 个 inode，共 64 个）
 *   block 11 ~ 2047    数据区（普通文件与目录内容）
 *
 * 核心概念：
 *   - 目录本质上是特殊文件，其内容是一串 Director_entry（inode 号 + 文件名）
 *   - inode 记录文件元信息，通过 6 个直接指针 + 一级间接 + 二级间接定位数据块
 *   - 位图用最简单的 1 bit / 1 个对象 的方式管理空闲资源
 *
 * 本文件的函数在内存中的 FScontext 副本上操作，落盘由 fs_write_block 完成。
 */
#ifndef HELP_H
#define HELP_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stddef.h>
#include <stdbool.h>

#define block_size 512U              /* 单个磁盘块字节数 */
#define size_of_disk (1024U * 1024U) /* 磁盘镜像总字节数：1MB */
#define total_block 2048             /* 磁盘总块数：1MB / 512B */
#define max_files 64                 /* inode 总数上限 */
#define name_length 28U              /* 目录项中文件名的最大字节数 */
#define inodes_per_block 8           /* 每块可容纳的 inode 数：512 / 64 */
#define directors_per_block 16       /* 每块可容纳的目录项数：512 / 32 */
#define magic_num 0xDEADBEEFU        /* 超级块魔数，用于识别合法镜像 */

/* 超级块：位于 block 0，描述整个文件系统的全局布局与空闲统计 */
typedef struct{
uint32_t magic;              /* 魔数，挂载时校验镜像合法性 */
uint32_t total_blocks;       /* 磁盘总块数 */
uint32_t inodemap_blocks;    /* inode 位图占用块数 */
uint32_t bitmap_blocks;      /* 数据块位图占用块数 */
uint32_t inodetable_blocks;  /* inode 表占用块数 */
uint32_t free_blocks;        /* 剩余空闲数据块数 */
uint32_t data_blocks;        /* 数据区总块数 */
uint32_t root_inode;         /* 根目录的 inode 号（固定为 0） */
uint32_t free_inodes;        /* 剩余空闲 inode 数 */
char supplements[476];       /* 填充至整块：512 - 9*4 = 476 字节 */
}Super_block;

/* inode 类型：空闲 / 普通文件 / 目录 */
typedef enum{
    inode_free = 0,
    inode_file = 1,
    inode_director = 2
}Inode_type;

/*
 * inode：文件元信息载体
 * 数据块定位采用 Unix 经典的多级索引：
 *   pointer[0..5] 直接指向前 6 个数据块
 *   in_pointer    指向一个索引块，块内 128 个 uint32 各指向一个数据块
 *   double_pointer 指向二级索引块，其每一项再指向一个一级索引块
 * 最大可寻址块数 = 6 + 128 + 128*128（实现中仅用到前两级）
 */
typedef struct{
    Inode_type type;         /* 文件类型 */
    uint32_t size;           /* 文件字节数 */
    uint32_t block;          /* 已占用的数据块数 */
    uint32_t create_time;    /* 创建时间（Unix 时间戳） */
    uint32_t modify_time;    /* 最后修改时间 */
    uint32_t pointer[6];     /* 6 个直接索引指针 */
    uint32_t in_pointer;     /* 一级间接索引块号，0 表示未分配 */
    uint32_t double_pointer; /* 二级间接索引块号，0 表示未分配 */
    char supplements[12];    /* 填充至 64 字节：4*6 + 4*2 + 4*4 + 12 = 64 */
}Inode;

/* 目录项：目录文件的一条记录，inode_num 为 0 表示该槽位空闲 */
typedef struct{
    uint32_t inode_num;          /* 指向的 inode 号 */
    char filename[name_length];  /* 文件名/子目录名 */
}Director_entry;

/* 挂载上下文：把磁盘上的结构一次性读入内存，作为运行期的全局视图 */
typedef struct{
    FILE* disk_fp;                     /* 磁盘镜像文件句柄 */
    Super_block super_block;           /* 超级块副本（block 0） */
    uint8_t* inodemap;                 /* inode 位图副本（block 1） */
    uint8_t* bitmap;                   /* 数据块位图副本（block 2） */
    Inode* inodetable;                 /* inode 表副本（block 3 起） */
    uint32_t begin_block_of_inodemap;  /* = 2 */
    uint32_t begin_block_of_bitmap;    /* = 3 */
    uint32_t begin_block_of_inodetable;/* = 4，共 8 块 = 64 个 inode × 64B / 512B */
    uint32_t begin_block_of_data;      /* = 12，数据区起始块号 */
}FScontext;

extern FScontext *fscontext;   /* 全局挂载上下文，由 interface.c 定义 */

void map_set(uint8_t* map,uint32_t bit);

void map_clear(uint8_t* map,uint32_t bit);

int map_check(uint8_t* map,uint32_t bit);

void fs_init_inode(uint32_t inode_num,Inode_type type);

uint32_t fs_alloc_inode();

/* 分配一个目录类型的 inode 并置为 inode_director（供 fs_mkdir 使用） */
uint32_t fs_alloc_inode_for_dir();

void fs_free_inode(uint32_t inode_num);

int fs_read_block(uint32_t block_num,void* buffer);

int fs_write_block(uint32_t block_num,void* buffer);

uint32_t fs_alloc_data_block();

void fs_free_data_block(uint32_t data_num);

uint32_t fs_get_file_block(Inode* inode,uint32_t num);

uint32_t fs_set_file_block(Inode* inode,uint32_t num,uint32_t block_index);
#endif
