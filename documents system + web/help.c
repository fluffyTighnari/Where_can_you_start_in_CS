/*
 * help.c —— 文件系统基础层实现
 *
 * 提供三组底层能力：
 *   1. 位图操作   map_set / map_clear / map_check
 *   2. inode 管理 fs_init_inode / fs_alloc_inode / fs_free_inode
 *   3. 块管理     fs_read_block / fs_write_block / fs_alloc_data_block / fs_free_data_block
 *                fs_get_file_block / fs_set_file_block（多级索引解析）
 *
 * 所有修改都作用在内存中的 FScontext 副本上，并同步写回对应磁盘块，
 * 因此运行期的内存视图与磁盘内容保持一致，异常退出也不会丢失元数据。
 */
#ifndef HELP_C
#define HELP_C
#include "help.h"
#endif

/* 置位：第 bit 个对象标记为已占用 */
void map_set(uint8_t* map,uint32_t bit){
    if(!map){
        return;
    }
    map[bit / 8] |= (1 << (bit % 8));   /* 定位到所在字节的对应 bit */
}

/* 清位：第 bit 个对象标记为空闲 */
void map_clear(uint8_t* map,uint32_t bit){
    if(!map){
        return;
    }
    map[bit / 8] &= ~(1 << (bit % 8));
}

/* 查询：返回 1 已占用 / 0 空闲 / -1 参数非法 */
int map_check(uint8_t* map,uint32_t bit){
    if(!map){
        return -1;
    }
    return (map[bit / 8] >> (bit % 8)) & 1;
}


/* 把 inode 重置为全零并填入基础字段，用于新建文件或目录 */
void fs_init_inode(uint32_t inode_num,Inode_type type){
    Inode* inode = &fscontext->inodetable[inode_num];
    memset(inode,0,sizeof(Inode));   /* 清空旧数据，索引指针一并归零 */
    inode->type = type;
    inode->size = 0;
    inode->block = 0;
    inode->create_time = (uint32_t)time(NULL);
    inode->modify_time = (uint32_t)time(NULL);
    for(int i = 0;i < 6;i++){
        inode->pointer[i] = 0;
    }
    inode->in_pointer = 0;
    inode->double_pointer = 0;
}

/*
 * 分配一个 inode（类型为普通文件）并落盘。
 * 返回 inode 号；无空闲 inode 时返回 (uint32_t)-1。
 * inode 号 0 保留给根目录，因此扫描从 1 开始。
 */
uint32_t fs_alloc_inode(){
    if(fscontext->super_block.free_inodes == 0){
        perror("无空闲inode");
        return (uint32_t)-1;
    }
    uint32_t inode_num = 0;
    for(int i = 1;i < max_files;i++){
        if(map_check(fscontext->inodemap,i) == 0){
            inode_num = i;
            map_set(fscontext->inodemap,i);   /* 在位图上占位 */
            break;
        }
    }
    fs_init_inode(inode_num,inode_file);
    fscontext->super_block.free_inodes--;
    fs_write_block(fscontext->begin_block_of_inodemap,fscontext->inodemap);

    /* inode 表按块落盘：每个块装 8 个 inode，需写回新 inode 所在的那一整块 */
    uint32_t target_block = inode_num / 8;
    fs_write_block(target_block + fscontext->begin_block_of_inodetable,fscontext->inodetable + target_block * 8);

    return inode_num;
}

/* 与 fs_alloc_inode 相同，只是把新 inode 标记为目录类型（供 fs_mkdir 使用） */
uint32_t fs_alloc_inode_for_dir(){
    if(fscontext->super_block.free_inodes == 0){
        perror("无空闲inode");
        return (uint32_t)-1;
    }
    uint32_t inode_num = 0;
    for(int i = 1;i < max_files;i++){
        if(map_check(fscontext->inodemap,i) == 0){
            inode_num = i;
            map_set(fscontext->inodemap,i);
            break;
        }
    }
    fs_init_inode(inode_num,inode_director);
    fscontext->super_block.free_inodes--;
    fs_write_block(fscontext->begin_block_of_inodemap,fscontext->inodemap);
    uint32_t target_block = inode_num / 8;
    fs_write_block(target_block + fscontext->begin_block_of_inodetable,fscontext->inodetable + target_block * 8);
    return inode_num;
}

/*
 * 释放 inode：先递归归还它占用的所有数据块（直接块 → 间接块 → 双重间接块），
 * 再清空 inode 内容、清位图、更新空闲计数。
 * 注意必须先读间接索引块的内容才能知道它指向哪些数据块，读完再释放索引块本身。
 */
void fs_free_inode(uint32_t inode_num){
    if(inode_num >= max_files){
        return;
    }

    Inode* inode = &fscontext->inodetable[inode_num];

    /* 第一级：6 个直接指针 */
    for(int i = 0;i < 6;i++){
        if(inode->pointer[i] != 0){
            fs_free_data_block(inode->pointer[i]);
            inode->pointer[i] = 0;
        }
    }

    /* 第二级：一级间接索引块 */
    if(inode->in_pointer != 0){
        uint32_t blocks_free[block_size / sizeof(uint32_t)] = {0};
        if(fs_read_block(inode->in_pointer,blocks_free) == 0){
            for(uint32_t i = 0;i < block_size / sizeof(uint32_t);i++){
                if(blocks_free[i] != 0){
                    fs_free_data_block(blocks_free[i]);
                }
            }
        }
        fs_free_data_block(inode->in_pointer);   /* 索引块自身也要归还 */
        inode->in_pointer = 0;
    }

    /* 第三级：二级间接索引块（每项再指向一个一级索引块） */
    if(inode->double_pointer != 0){
        uint32_t level1[block_size / sizeof(uint32_t)] = {0};
        if(fs_read_block(inode->double_pointer, level1) == 0){
            for(uint32_t i = 0;i < block_size / sizeof(uint32_t);i++){
                if(level1[i] != 0){
                    uint32_t level2[block_size / sizeof(uint32_t)] = {0};
                    if(fs_read_block(level1[i], level2) == 0){
                        for(uint32_t j = 0;j < block_size / sizeof(uint32_t);j++){
                            if(level2[j] != 0){
                                fs_free_data_block(level2[j]);
                            }
                        }
                    }
                    fs_free_data_block(level1[i]);
                }
            }
        }
        fs_free_data_block(inode->double_pointer);
        inode->double_pointer = 0;
    }

    map_clear(fscontext->inodemap,inode_num);
    memset(inode,0,sizeof(Inode));
    fscontext->super_block.free_inodes++;

    /* 位图与 inode 表同步落盘 */
    fs_write_block(fscontext->begin_block_of_inodemap,fscontext->inodemap);
    uint32_t target_block = inode_num / 8;
    fs_write_block(fscontext->begin_block_of_inodetable + target_block,fscontext->inodetable + target_block * 8);
}


/* 按块号读一整块到 buffer；块号越界或 IO 失败返回 -1 */
int fs_read_block(uint32_t block_num,void* buffer){
    if(!fscontext->disk_fp || block_num >= total_block){
        return -1;
    }

    if(fseek(fscontext->disk_fp,block_num * block_size,SEEK_SET) != 0){
        return -1;
    }

    return fread(buffer,block_size,1,fscontext->disk_fp) == 1 ? 0 : -1;
}

/* 按块号写一整块；块号越界或 IO 失败返回 -1 */
int fs_write_block(uint32_t block_num,void* buffer){
    if(!fscontext->disk_fp || block_num >= total_block){
        return -1;
    }

    if(fseek(fscontext->disk_fp,block_num * block_size,SEEK_SET) != 0){
        return -1;
    }

    return fwrite(buffer,block_size,1,fscontext->disk_fp) == 1 ? 0 : -1;
}

/*
 * 分配一个数据块：在数据块位图中找第一个空闲位。
 * 新块清零后落盘（避免残留旧数据），位图与超级块同步更新。
 * 返回绝对块号；无空闲块返回 (uint32_t)-1。
 */
uint32_t fs_alloc_data_block(){
    if(fscontext->super_block.free_blocks == 0){
        perror("无空闲块");
        return (uint32_t)-1;
    }
    uint32_t data_num = 0;
    int found = 0;
    for(uint32_t i = 0; i < fscontext->super_block.data_blocks; i++){
        if(map_check(fscontext->bitmap,i) == 0){
            data_num = i;                    /* data_num 是相对数据区的下标 */
            map_set(fscontext->bitmap,i);
            found = 1;
            break;
        }
    }
    if (!found) {
        return (uint32_t)-1;
    }
    char new_block[block_size] = {0};
    fscontext->super_block.free_blocks--;
    fs_write_block(fscontext->begin_block_of_data + data_num,new_block);   /* 清零新块 */

    fs_write_block(fscontext->begin_block_of_bitmap,fscontext->bitmap);

    return data_num + fscontext->begin_block_of_data;   /* 返回绝对块号 */
}

/* 释放数据块：入参是绝对块号，需先换算成位图下标再清位 */
void fs_free_data_block(uint32_t data_num){
    if(data_num < fscontext->begin_block_of_data || data_num >= fscontext->begin_block_of_data + fscontext->super_block.data_blocks){
        return;   /* 不是数据区的块，直接忽略（防止误释放元数据块） */
    }
    uint32_t data_index = data_num - fscontext->begin_block_of_data;

    if(map_check(fscontext->bitmap,data_index) == 1){
        map_clear(fscontext->bitmap,data_index);
        fscontext->super_block.free_blocks++;
        fs_write_block(fscontext->begin_block_of_bitmap,fscontext->bitmap);
    }
}

/*
 * 取文件第 num 个逻辑块（从 1 开始）对应的物理块号，0 表示未分配。
 * num <= 6      → 直接指针
 * 7 <= num < 134 → 一级间接索引块中的第 num-7 项
 */
uint32_t fs_get_file_block(Inode* inode,uint32_t num){
    if(num < 1 || num > 6 + (block_size/sizeof(uint32_t))){
        return 0;
    }
    if(num <= 6){
        return inode->pointer[num-1];
    }
    uint32_t maxnum = 6 + block_size / sizeof(uint32_t);
    if(num < maxnum){
        if(inode->in_pointer == 0){
            return 0;   /* 一级间接块尚未分配 */
        }
        uint32_t in_blocks[block_size / sizeof(uint32_t)];
        if(fs_read_block(inode->in_pointer,in_blocks) != 0){
            return 0;
        }
        return in_blocks[num-7];
    }
    return 0;
}

/*
 * 设置文件第 num 个逻辑块对应的物理块号。
 * 若目标落在一级间接区且间接块尚未分配，则按需分配并清零。
 * 注意：本函数只改索引，不分配数据块本身（数据块由调用方传入）。
 */
uint32_t fs_set_file_block(Inode* inode,uint32_t num,uint32_t block_index){
    if(num < 1 || num > 6 + (block_size/sizeof(uint32_t))){
        return -1;
    }
    if(num <= 6){
        inode->pointer[num-1] = block_index;
        return 0;
    }
    uint32_t maxnum = 6 + block_size / sizeof(uint32_t);
    if(num < maxnum){
        if(inode->in_pointer == 0){
            inode->in_pointer = fs_alloc_data_block();   /* 延迟分配索引块 */
            if(inode->in_pointer == (uint32_t)-1){
                return -1;
            }
            uint32_t zero_block[block_size / sizeof(uint32_t)] = {0};
            fs_write_block(inode->in_pointer,zero_block);
        }
        uint32_t buffer[block_size / sizeof(uint32_t)] = {0};
        if(fs_read_block(inode->in_pointer,buffer) != 0){
            return -1;
        }
        buffer[num-7] = block_index;
        fs_write_block(inode->in_pointer,buffer);
        return 0;
    }
    return -1;
}
