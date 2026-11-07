/*
 * interface.c —— 文件系统对外 API 实现
 *
 * 层次关系：Web 层 → interface.c（本文件，加锁与参数校验）→ help.c（位图/inode/块管理）
 *
 * 目录只有两层（根目录 → 用户目录 → 文件），因此查找路径固定为两步：
 *   get_dir_inode_from_root() 在根目录中按名字找到用户目录 inode，
 *   get_inode_num_in_dir()    再在用户目录中按名字找到目标文件 inode。
 *
 * 并发安全：整卷只有一把 fs_mutex，所有操作串行化。粒度较粗，
 * 但写路径会同时改动位图、inode 表与数据块，粗粒度锁能避免大量一致性问题。
 *
 * 落盘策略：元数据（inode / 位图 / 超级块）改动后立即写回磁盘；
 * 文件数据块在 fs_write_file 中逐块写回。
 */
#ifndef INTERFACE_C
#define INTERFACE_C
#include "help.h"
#include "interface.h"
#include <pthread.h>
#endif

FScontext *fscontext = NULL;                          /* 全局挂载上下文 */
pthread_mutex_t fs_mutex = PTHREAD_MUTEX_INITIALIZER; /* 保护整个文件系统 */

/*
 * 把内存中的某个 inode 写回磁盘。
 * inode 表按块组织（8 个 inode / 块），因此要写回该 inode 所在的整块。
 * 入参是指向 inodetable 内部的指针，用指针差反推 inode 下标。
 */
static void write_back_inode(Inode* inode) {
    uint32_t inode_idx = (uint32_t)(inode - fscontext->inodetable);
    uint32_t target_block = inode_idx / inodes_per_block;
    fs_write_block(fscontext->begin_block_of_inodetable + target_block,
                   fscontext->inodetable + target_block * inodes_per_block);
}

/*
 * 在 parent 目录中按名字查找子项的 inode 号，找不到返回 0。
 * 目录内容按块存储，每块 16 个目录项；inode_num 为 0 的槽位表示已删除/未使用。
 */
static uint32_t get_inode_num_in_dir(Inode* parent, const char* name) {
    Director_entry directors[directors_per_block];
    uint32_t block_num = parent->block;
    for (uint32_t i = 0; i < block_num; i++) {
        uint32_t block_addr = fs_get_file_block(parent, i + 1);   /* 逻辑块号从 1 开始 */
        if (block_addr == 0) break;
        if (fs_read_block(block_addr, directors) != 0) continue;
        for (uint32_t j = 0; j < directors_per_block; j++) {
            if (directors[j].inode_num != 0 && strcmp(directors[j].filename, name) == 0)
                return directors[j].inode_num;
        }
    }
    return 0;
}

/* 在根目录下按名字找到用户目录的 inode；要求目标确实是目录类型，否则返回 NULL */
static Inode* get_dir_inode_from_root(const char* dirname) {
    if (!dirname || strlen(dirname) == 0 || fscontext == NULL) return NULL;
    Inode* root_inode = &fscontext->inodetable[0];   /* inode 0 固定为根目录 */
    if (root_inode->type != inode_director) return NULL;
    uint32_t inode_num = get_inode_num_in_dir(root_inode, dirname);
    if (inode_num == 0) return NULL;
    Inode* dir = &fscontext->inodetable[inode_num];
    return (dir->type == inode_director) ? dir : NULL;
}

/*
 * 把一条目录项加入 parent 目录。
 * 先在已有目录块中找空槽（inode_num == 0），全部占满则新分配一个数据块。
 * 目录的 size 与 block 字段随目录项增减同步维护。
 */
static int add_entry_to_dir(Inode* parent, const Director_entry* entry) {
    Director_entry directors[directors_per_block];
    uint32_t block_num = parent->block;
    for (uint32_t i = 0; i < block_num; i++) {
        uint32_t block_addr = fs_get_file_block(parent, i + 1);
        if (block_addr == 0) break;
        if (fs_read_block(block_addr, directors) != 0) continue;
        for (uint32_t j = 0; j < directors_per_block; j++) {
            if (directors[j].inode_num == 0) {   /* 找到空槽 */
                directors[j] = *entry;
                if (fs_write_block(block_addr, directors) == 0) {
                    parent->size += sizeof(Director_entry);
                    parent->modify_time = (uint32_t)time(NULL);
                    write_back_inode(parent);
                    return 0;
                }
                return -1;
            }
        }
    }
    /* 已有块都满了：为目录扩展一个新块 */
    uint32_t new_block = fs_alloc_data_block();
    if (new_block == (uint32_t)-1) return -1;
    memset(directors, 0, sizeof(directors));
    directors[0] = *entry;
    if (fs_write_block(new_block, directors) != 0) {
        fs_free_data_block(new_block);
        return -1;
    }
    if (fs_set_file_block(parent, parent->block + 1, new_block) != 0) {
        fs_free_data_block(new_block);
        return -1;
    }
    parent->block++;
    parent->size += sizeof(Director_entry);
    parent->modify_time = (uint32_t)time(NULL);
    write_back_inode(parent);
    return 0;
}

/*
 * 创建并格式化一块全新的磁盘镜像。
 * 布局：超级块 → inode 位图 → 块位图 → inode 表（8 块）→ 数据区。
 * 格式化完成后关闭文件并释放 fscontext——真正使用前需要再调用 fs_mount。
 */
void fs_create(const char* disk_path){
    FILE* test = fopen(disk_path,"rb");
    if(test != NULL){
        fclose(test);
        perror("磁盘文件已存在\n");
        return;
    }

    /* 若已有挂载中的文件系统，先卸载避免全局状态冲突 */
    if(fscontext != NULL){
        fs_unmount();
    }
    fscontext = (FScontext*)malloc(sizeof(FScontext));
    if(fscontext == NULL){
        perror("分配文件系统上下文失败\n");
        return;
    }
    memset(fscontext, 0, sizeof(FScontext));

    FILE* fp = fopen(disk_path,"wb+");
    if(fp == NULL){
        perror("创建磁盘镜像文件失败\n");
        free(fscontext);
        fscontext = NULL;
        return;
    }
    fscontext->disk_fp = fp;
    fscontext->begin_block_of_inodemap = 1;
    fscontext->begin_block_of_bitmap = 2;
    fscontext->begin_block_of_inodetable = 3;
    fscontext->begin_block_of_data = fscontext->begin_block_of_inodetable + max_files / inodes_per_block;

    Super_block* super_block = &fscontext->super_block;
    super_block->magic = 0xDEADBEEFU;
    super_block->total_blocks = total_block;
    super_block->inodemap_blocks = 1;
    super_block->bitmap_blocks = 1;
    super_block->inodetable_blocks = fscontext->begin_block_of_data - fscontext->begin_block_of_inodetable;
    super_block->free_blocks = super_block->total_blocks - fscontext->begin_block_of_data;
    super_block->data_blocks = super_block->free_blocks;
    super_block->root_inode = 0;
    super_block->free_inodes = max_files - 1;   /* inode 0 已被根目录占用 */

    fscontext->inodemap = (uint8_t*)calloc(block_size,1);
    fscontext->bitmap = (uint8_t*)calloc(block_size,1);
    fscontext->inodetable = (Inode*)calloc(block_size * super_block->inodetable_blocks,1);
    if(!fscontext->inodemap || !fscontext->bitmap || !fscontext->inodetable){
        perror("分配文件系统元数据失败\n");
        fclose(fp);
        free(fscontext->inodemap);
        free(fscontext->bitmap);
        free(fscontext->inodetable);
        free(fscontext);
        fscontext = NULL;
        return;
    }

    /* 初始化根目录：inode 0 置为占用，类型为目录，但初始不含任何目录项 */
    map_set(fscontext->inodemap,0);
    Inode* inode = &fscontext->inodetable[0];
    memset(inode,0,sizeof(Inode));
    inode->type = inode_director;
    inode->create_time = (uint32_t)time(NULL);
    inode->modify_time = inode->create_time;

    if(fs_write_block(0,super_block) != 0){
        perror("写入超级块失败\n");
    }

    if(fs_write_block(1,fscontext->inodemap) != 0){
        perror("写入 inode 位图失败\n");
    }

    if(fs_write_block(2,fscontext->bitmap) != 0){
        perror("写入数据块位图失败\n");
    }

    /* inode 表逐块落盘，源地址按 inodes_per_block 递增 */
    for(uint32_t i = 0;i < super_block->inodetable_blocks;i++){
        if(fs_write_block(fscontext->begin_block_of_inodetable+i,&fscontext->inodetable[i*inodes_per_block]) != 0){
            perror("写入 inode 表失败\n");
        }
    }

    /* 数据区整片清零，避免镜像文件中残留旧内容 */
    uint8_t* zero_block = (uint8_t*)calloc(block_size,1);
    if(!zero_block){
        perror("分配清零缓冲区失败\n");
        fclose(fp);
        free(fscontext->inodemap);
        free(fscontext->bitmap);
        free(fscontext->inodetable);
        free(fscontext);
        fscontext = NULL;
        return;
    }
    for(uint32_t i = 0;i < super_block->data_blocks;i++){
        if(fs_write_block(fscontext->begin_block_of_data+i,zero_block) != 0){
            perror("清零数据块失败\n");
        }
    }

    fclose(fscontext->disk_fp);
    fscontext->disk_fp = NULL;
    free(fscontext->inodemap);
    free(fscontext->bitmap);
    free(fscontext->inodetable);
    free(fscontext);
    fscontext = NULL;
    free(zero_block);

    printf("文件系统格式化完成，镜像已创建\n");
}


/*
 * 挂载磁盘镜像：读取超级块并校验魔数，然后把位图与 inode 表缓存到内存。
 * 挂载后所有操作都作用在内存副本上，由各接口负责写回。
 */
int fs_mount(const char* disk_path){
    pthread_mutex_lock(&fs_mutex);

    /* 已挂载则先卸载，保证同一时刻只有一份内存视图 */
    if(fscontext != NULL){
        fs_unmount();
    }

    fscontext = (FScontext*)malloc(sizeof(FScontext));
    if(fscontext == NULL){
        perror("分配文件系统上下文失败\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }

    memset(fscontext, 0, sizeof(FScontext));

    fscontext->disk_fp = fopen(disk_path,"rb+");
    if(fscontext->disk_fp == NULL){
        perror("打开磁盘镜像失败\n");
        free(fscontext);
        fscontext = NULL;
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    Super_block super_block;
    if(fs_read_block(0,&super_block) != 0){
        perror("读取超级块失败\n");
        fclose(fscontext->disk_fp);
        free(fscontext);
        fscontext = NULL;
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    if(super_block.magic != magic_num){
        perror("镜像魔数校验失败\n");
        fclose(fscontext->disk_fp);
        free(fscontext);
        fscontext = NULL;
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    fscontext->super_block = super_block;

    /* 各区域起始块号：超级块固定占 0，位图各占 1 块，inode 表紧随其后 */
    fscontext->begin_block_of_inodemap = 1;
    fscontext->begin_block_of_bitmap = 2;
    fscontext->begin_block_of_inodetable = 3;
    fscontext->begin_block_of_data = 3 + super_block.inodetable_blocks;

    fscontext->inodemap = (uint8_t*)calloc(block_size,1);
    fscontext->bitmap = (uint8_t*)calloc(block_size,1);
    fscontext->inodetable = (Inode*)calloc(block_size * super_block.inodetable_blocks,1);
    if(!fscontext->inodemap || !fscontext->bitmap || !fscontext->inodetable){
        perror("分配文件系统元数据失败\n");
        fclose(fscontext->disk_fp);
        free(fscontext->inodemap);
        free(fscontext->bitmap);
        free(fscontext->inodetable);
        free(fscontext);
        fscontext = NULL;
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }

    if(fs_read_block(1,fscontext->inodemap) != 0){
        perror("读取 inode 位图失败\n");
        goto mount_fail;
    }

    if(fs_read_block(2,fscontext->bitmap) != 0){
        perror("读取数据块位图失败\n");
        goto mount_fail;
    }

    /* inode 表逐块读入内存 */
    for(uint32_t i = 0;i < fscontext->super_block.inodetable_blocks;i++){
        if(fs_read_block(fscontext->begin_block_of_inodetable+i,&fscontext->inodetable[i*inodes_per_block]) != 0){
            perror("读取 inode 表失败\n");
            goto mount_fail;
        }
    }

    pthread_mutex_unlock(&fs_mutex);
    return 0;

    /* 统一失败路径：释放全部资源并解锁 */
    mount_fail:
    free(fscontext->inodemap);
    free(fscontext->bitmap);
    free(fscontext->inodetable);
    fclose(fscontext->disk_fp);
    free(fscontext);
    fscontext = NULL;
    pthread_mutex_unlock(&fs_mutex);
    return -1;
}

/*
 * 卸载：把内存中的超级块、位图、inode 表全部写回磁盘，然后释放资源。
 * 这是内存副本与磁盘内容最终同步的地方。
 */
void fs_unmount(){
    pthread_mutex_lock(&fs_mutex);
    if(fscontext == NULL || fscontext->disk_fp == NULL){
        perror("文件系统未挂载\n");
        pthread_mutex_unlock(&fs_mutex);
        return;
    }

    fs_write_block(0,&fscontext->super_block);
    fs_write_block(fscontext->begin_block_of_inodemap,fscontext->inodemap);
    fs_write_block(fscontext->begin_block_of_bitmap,fscontext->bitmap);
    for(uint32_t i = 0;i < fscontext->super_block.inodetable_blocks;i++){
        fs_write_block(fscontext->begin_block_of_inodetable+i,
                      &fscontext->inodetable[i*inodes_per_block]);
    }

    free(fscontext->inodemap);
    free(fscontext->bitmap);
    free(fscontext->inodetable);
    fclose(fscontext->disk_fp);
    free(fscontext);
    fscontext = NULL;
    pthread_mutex_unlock(&fs_mutex);
}

/* 创建目录：在根目录（或在 current_dir 下）新建一个目录项与对应目录 inode */
int fs_mkdir(const char* dirname, const char* current_dir){
    pthread_mutex_lock(&fs_mutex);

    if(!dirname || strlen(dirname) > name_length || fscontext == NULL){
        perror("目录名非法或文件系统未挂载\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    Inode* parent = &fscontext->inodetable[0];   /* 默认父目录是根目录 */
    if(current_dir && strlen(current_dir) > 0){
        parent = get_dir_inode_from_root(current_dir);
        if(!parent){
            perror("指定的父目录不存在\n");
            pthread_mutex_unlock(&fs_mutex);
            return -1;
        }
    }
    if(parent->type != inode_director){
        perror("父节点不是目录\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    if(get_inode_num_in_dir(parent, dirname) != 0){
        perror("同名目录已存在\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }

    uint32_t new_inode = fs_alloc_inode_for_dir();
    if(new_inode == (uint32_t)-1){
        perror("inode 分配失败\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    Director_entry new_entry = {0};
    new_entry.inode_num = new_inode;
    strncpy(new_entry.filename, dirname, name_length - 1);
    new_entry.filename[name_length - 1] = '\0';

    if(add_entry_to_dir(parent, &new_entry) != 0){
        fs_free_inode(new_inode);   /* 目录项写入失败，回滚已分配的 inode */
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    printf("目录 %s 创建成功，inode 号 %u\n", dirname, new_inode);
    pthread_mutex_unlock(&fs_mutex);
    return 0;
}

/* 创建空文件：先查重，再分配 inode 并写入目录项 */
int fs_create_file(const char* filename, const char* dirname){
    pthread_mutex_lock(&fs_mutex);

    if(!filename || strlen(filename) > name_length || fscontext == NULL){
        perror("文件名非法或文件系统未挂载\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    Inode* dir_inode = get_dir_inode_from_root(dirname);
    if(!dir_inode){
        perror("用户目录不存在\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }

    /* 遍历目录块检查同名文件 */
    Director_entry directors[directors_per_block];
    uint32_t block_num = dir_inode->block;
    for(uint32_t i = 0;i < block_num;i++){
        uint32_t block_addr = fs_get_file_block(dir_inode,i+1);
        if(block_addr == 0) break;
        if(fs_read_block(block_addr,directors) != 0) continue;
        for(uint32_t j = 0;j < directors_per_block;j++){
            if(directors[j].inode_num != 0 && strcmp(directors[j].filename,filename) == 0){
                perror("同名文件已存在\n");
                pthread_mutex_unlock(&fs_mutex);
                return -1;
            }
        }
    }

    Director_entry new_entry = {0};
    uint32_t new_inode = fs_alloc_inode();
    if(new_inode == (uint32_t)-1){
        perror("inode 分配失败\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    new_entry.inode_num = new_inode;
    strncpy(new_entry.filename,filename,name_length-1);
    new_entry.filename[name_length-1] = '\0';

    if(add_entry_to_dir(dir_inode, &new_entry) != 0){
        fs_free_inode(new_inode);
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    printf("文件 %s 在 %s 下创建成功，inode 号 %u\n", filename, dirname, new_inode);
    pthread_mutex_unlock(&fs_mutex);
    return 0;
}

/* 删除文件：释放 inode（连带其所有数据块），并把目录项槽位清零 */
int fs_delete_file(const char* filename, const char* dirname){
    pthread_mutex_lock(&fs_mutex);

    if(!filename || strlen(filename) > name_length || fscontext == NULL){
        perror("文件名非法或文件系统未挂载\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    Inode* dir_inode = get_dir_inode_from_root(dirname);
    if(!dir_inode){
        perror("用户目录不存在\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }

    Director_entry directors[directors_per_block];
    uint32_t block_num = dir_inode->block;
    for(uint32_t i = 0;i < block_num;i++){
        uint32_t block_addr = fs_get_file_block(dir_inode,i+1);
        if(block_addr == 0) break;
        if(fs_read_block(block_addr,directors) != 0) continue;
        for(uint32_t j = 0;j < directors_per_block;j++){
            if(directors[j].inode_num != 0 && strcmp(directors[j].filename,filename) == 0){
                fs_free_inode(directors[j].inode_num);      /* 先归还 inode 与数据块 */
                memset(&directors[j],0,sizeof(Director_entry));   /* 再清空目录项 */
                dir_inode->size -= sizeof(Director_entry);
                dir_inode->modify_time = (uint32_t)time(NULL);
                fs_write_block(block_addr,directors);
                write_back_inode(dir_inode);
                printf("文件 %s 删除成功\n",filename);
                pthread_mutex_unlock(&fs_mutex);
                return 0;
            }
        }
    }
    perror("要删除的文件不存在\n");
    pthread_mutex_unlock(&fs_mutex);
    return -1;
}

/*
 * 读文件：支持任意 offset 与 size，跨块读取。
 * 返回实际读到的字节数（遇到文件尾会提前结束）；失败返回 -1。
 * 计算方式：把 [offset, offset+size) 映射成逻辑块区间 [beginblock, endblock]，
 * 首块从 curoffset 开始，末块只取剩余部分。
 */
int fs_read_file(const char* filename, const char* dirname, void* buffer, uint32_t size, uint32_t offset){
    pthread_mutex_lock(&fs_mutex);

    if(!filename || strlen(filename) > name_length || size == 0 || !buffer || fscontext == NULL){
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    Inode* dir_inode = get_dir_inode_from_root(dirname);
    if(!dir_inode){
        perror("用户目录不存在\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }

    uint32_t target_inode_num = get_inode_num_in_dir(dir_inode, filename);
    if(target_inode_num == 0){
        perror("文件不存在!\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    Inode* target_inode = &fscontext->inodetable[target_inode_num];
    if(target_inode->type != inode_file){
        perror("目标不是普通文件\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    if(target_inode->size < offset){
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    /* 实际可读字节数取 min(请求 size, 文件剩余长度) */
    int realsize = target_inode->size - offset > (int)size ? (int)size : target_inode->size - offset;

    int beginblock = offset / block_size;                    /* 起始逻辑块（0 基） */
    int endblock = (offset + realsize - 1) / block_size;     /* 结束逻辑块（0 基） */
    int curoffset = offset % block_size;                     /* 首块内的起始偏移 */
    int totalread = 0;

    for(int i = beginblock;i <= endblock;i++){
        uint8_t buf[block_size];
        memset(buf, 0, sizeof(buf));
        uint32_t block_num = fs_get_file_block(target_inode,i+1);   /* 索引接口是 1 基 */
        if(block_num != 0){
            if(fs_read_block(block_num,buf) != 0){
                perror("读取数据块失败\n");
                pthread_mutex_unlock(&fs_mutex);
                return -1;
            }
        }

        /* 末块只取剩余部分，中间块取满整块 */
        int cursize = i == endblock ? realsize - totalread : block_size - curoffset;
        if (totalread + cursize > (int)size) {
            pthread_mutex_unlock(&fs_mutex);
            return -1;
        }
        memcpy((uint8_t*)buffer + totalread,buf + curoffset,cursize);
        totalread += cursize;
        curoffset = 0;   /* 后续块都从块首开始 */
    }
    pthread_mutex_unlock(&fs_mutex);
    return totalread;
}

/*
 * 写文件：支持任意 offset，自动按需扩容/收缩数据块。
 * 返回实际写入字节数；失败返回 -1。
 *
 * 扩容/收缩规则：
 *   needblock = ceil((offset + size) / block_size)
 *   needblock > 当前块数 → 逐块分配；中途分配失败则回滚已分配的块
 *   needblock < 当前块数 → 从尾部逐块释放
 */
int fs_write_file(const char* filename, const char* dirname, const void* buffer, uint32_t size, uint32_t offset){
    pthread_mutex_lock(&fs_mutex);

    if(!filename || strlen(filename) > name_length || size == 0 || !buffer || fscontext == NULL){
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    Inode* dir_inode = get_dir_inode_from_root(dirname);
    if(!dir_inode){
        perror("用户目录不存在\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }

    uint32_t target_inode_num = get_inode_num_in_dir(dir_inode, filename);
    if(target_inode_num == 0){
        perror("文件不存在!\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    Inode* target_inode = &fscontext->inodetable[target_inode_num];
    if(target_inode->type != inode_file){
        perror("目标不是普通文件\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    /* 容量上限 = 直接块 6 + 一级间接 128 块 */
    if(offset + size > 6*block_size + (block_size/sizeof(uint32_t))*block_size){
        perror("超出单文件容量上限\n");
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    int needblock = (offset + size + block_size - 1) / block_size;
    int curblock = target_inode->block;
    int orig_block = target_inode->block;   /* 记录原块数，供分配失败时回滚 */
    int newblock = needblock - curblock;
    int ret = -1;

    if(newblock > 0) {
        /* 扩容：逐块分配并写入索引，失败时回滚到 orig_block */
        int alloc_try = newblock;
        while(alloc_try > 0){
            uint32_t block_num = fs_alloc_data_block();
            if(block_num == (uint32_t)-1){
                while (target_inode->block > (uint32_t)orig_block) {
                    uint32_t bn = fs_get_file_block(target_inode, target_inode->block);
                    if (bn != 0) fs_free_data_block(bn);
                    target_inode->block--;
                }
                pthread_mutex_unlock(&fs_mutex);
                return -1;
            }
            fs_set_file_block(target_inode, ++curblock, block_num);
            target_inode->block++;
            alloc_try--;
        }
    } else if(newblock < 0) {
        /* 收缩：从尾部释放多余的块（覆盖写变小文件的情况） */
        int blocks_to_free = -newblock;
        while(blocks_to_free--){
            uint32_t block_num = fs_get_file_block(target_inode, curblock);
            if(block_num != 0){
                fs_free_data_block(block_num);
            }
            curblock--;
            target_inode->block--;
        }
    }

    /* 逐块读出 → 局部覆盖 → 写回，避免破坏块内未被覆盖的其他字节 */
    int beginblock = offset / block_size;
    int endblock = (offset + size - 1) / block_size;
    int curoffset = offset % block_size;
    int totalwrite = 0;
    uint8_t buf[block_size];
    int write_ok = 1;
    for(int i = beginblock;i <= endblock;i++){
        uint32_t block_addr = fs_get_file_block(target_inode,i+1);
        if(block_addr == 0){
            write_ok = 0;
            break;
        }
        if(fs_read_block(block_addr,buf) != 0){
            perror("读取块失败\n");
            write_ok = 0;
            break;
        }
        int cursize = i == endblock ? (int)size - totalwrite : block_size - curoffset;
        memcpy(buf+curoffset,(const uint8_t*)buffer+totalwrite,cursize);
        totalwrite += cursize;
        curoffset = 0;

        fs_write_block(block_addr,buf);
    }
    if (write_ok) {
        target_inode->size = offset + size;   /* 写入成功才更新文件大小 */
        target_inode->modify_time = (uint32_t)time(NULL);
        ret = totalwrite;
    }
    write_back_inode(target_inode);

    pthread_mutex_unlock(&fs_mutex);
    return ret;
}

/* 打印整个文件到 stdout：按 1KB 分块循环读取，注意读操作前必须先释放锁 */
void fs_cat_file(const char* filename, const char* dirname){
    pthread_mutex_lock(&fs_mutex);
    if(!filename || fscontext == NULL) {
        pthread_mutex_unlock(&fs_mutex);
        return;
    }
    uint8_t buf[1024];
    int read;
    uint32_t offset = 0;
    pthread_mutex_unlock(&fs_mutex);   /* fs_read_file 内部会重新加锁，这里不能持锁调用 */
    while((read = fs_read_file(filename, dirname, buf, sizeof(buf), offset)) > 0){
        fwrite(buf,1,read,stdout);
        offset += read;
    }
    printf("\n");
}

/* 打印超级块中的容量统计 */
void fs_show_information(){
    pthread_mutex_lock(&fs_mutex);
    if(fscontext == NULL) {
        pthread_mutex_unlock(&fs_mutex);
        return;
    }
    Super_block* super_block = &fscontext->super_block;

    double usage = (1.0 - (double)super_block->free_blocks / super_block->data_blocks) * 100;
    printf("\n=== 文件系统信息 ===\n");
    printf(" 总块数:      %u\n", super_block->total_blocks);
    printf(" 数据块数:    %u\n", super_block->data_blocks);
    printf(" 空闲块数:    %u\n", super_block->free_blocks);
    printf(" 空闲inode数: %u\n", super_block->free_inodes);
    printf(" 空间使用率:  %.2f%%\n", usage);
    pthread_mutex_unlock(&fs_mutex);
}

/* 打印目录内容到 stdout：遍历目录块中的每个有效目录项，输出目标 inode 的摘要 */
void fs_list_director(const char* dirname){
    pthread_mutex_lock(&fs_mutex);
    if(fscontext == NULL) {
        pthread_mutex_unlock(&fs_mutex);
        return;
    }
    Inode* dir_inode = get_dir_inode_from_root(dirname);
    if(!dir_inode){
        perror("目录不存在\n");
        pthread_mutex_unlock(&fs_mutex);
        return;
    }
    printf("\n=== [%s] 目录列表 ===\n", dirname);
    printf(" %-20s %-10s %-8s %-6s %s\n", "文件名", "大小(字节)", "块数", "inode", "类型");
    Director_entry buf[directors_per_block];
    uint32_t block_num = dir_inode->block;
    int count = 0;
    for(uint32_t i = 0;i < block_num;i++){
        uint32_t block_addr = fs_get_file_block(dir_inode,i+1);
        if(block_addr == 0) break;
        if(fs_read_block(block_addr,buf) != 0){
            perror("读取目录块失败\n");
            continue;
        }
        for(int j = 0;j < directors_per_block;j++){
            if(buf[j].inode_num != 0){
                Inode* target_inode = &fscontext->inodetable[buf[j].inode_num];
                const char* type = target_inode->type == inode_file ? "文件" : "目录";
                printf(" %-20s %-10u %-8u %-6u %s\n",buf[j].filename,target_inode->size,target_inode->block,buf[j].inode_num,type);
                count++;
            }
        }
    }
    printf("共 %d 项\n", count);
    pthread_mutex_unlock(&fs_mutex);
}

/*
 * fs_list_director 的缓冲区版本：把结果格式化进 buf 供 HTTP 响应使用。
 * 每次 snprintf 后检查返回值，一旦空间不足立即返回 -1（避免截断产生半个 UTF-8 字符）。
 */
int fs_list_director_to_buf(const char* dirname, char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return -1;
    pthread_mutex_lock(&fs_mutex);
    if (fscontext == NULL || !dirname) {
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    Inode* dir_inode = get_dir_inode_from_root(dirname);
    if (!dir_inode) {
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    size_t off = 0;
    int n;
    n = snprintf(buf + off, buf_size - off, "\n=== [%s] 目录列表 ===\n", dirname);
    if (n < 0 || (size_t)n >= buf_size - off) { pthread_mutex_unlock(&fs_mutex); return -1; }
    off += (size_t)n;
    n = snprintf(buf + off, buf_size - off, " %-20s %-10s %-8s %-6s %s\n",
                 "文件名", "大小(字节)", "块数", "inode", "类型");
    if (n < 0 || (size_t)n >= buf_size - off) { pthread_mutex_unlock(&fs_mutex); return -1; }
    off += (size_t)n;

    Director_entry entry_buf[directors_per_block];
    uint32_t block_num = dir_inode->block;
    int count = 0;
    for (uint32_t i = 0; i < block_num; i++) {
        uint32_t block_addr = fs_get_file_block(dir_inode, i + 1);
        if (block_addr == 0) break;
        if (fs_read_block(block_addr, entry_buf) != 0) continue;
        for (int j = 0; j < directors_per_block; j++) {
            if (entry_buf[j].inode_num != 0) {
                Inode* target_inode = &fscontext->inodetable[entry_buf[j].inode_num];
                const char* type = target_inode->type == inode_file ? "文件" : "目录";
                n = snprintf(buf + off, buf_size - off,
                             " %-20s %-10u %-8u %-6u %s\n",
                             entry_buf[j].filename, target_inode->size,
                             target_inode->block, entry_buf[j].inode_num, type);
                if (n < 0 || (size_t)n >= buf_size - off) {
                    pthread_mutex_unlock(&fs_mutex);
                    return -1;
                }
                off += (size_t)n;
                count++;
            }
        }
    }
    n = snprintf(buf + off, buf_size - off, "共 %d 项\n", count);
    if (n < 0 || (size_t)n >= buf_size - off) { pthread_mutex_unlock(&fs_mutex); return -1; }
    off += (size_t)n;

    pthread_mutex_unlock(&fs_mutex);
    return (int)off;
}

/* 列出根目录成员（即所有用户目录），缓冲区版本 */
int fs_list_member_to_buf(char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return -1;
    pthread_mutex_lock(&fs_mutex);
    if (fscontext == NULL) {
        pthread_mutex_unlock(&fs_mutex);
        return -1;
    }
    size_t off = 0;
    int n;
    n = snprintf(buf + off, buf_size - off, "\n=== 根目录成员列表（所有用户/目录） ===\n");
    if (n < 0 || (size_t)n >= buf_size - off) { pthread_mutex_unlock(&fs_mutex); return -1; }
    off += (size_t)n;
    n = snprintf(buf + off, buf_size - off, " %-20s %-10s %-8s %-6s %s\n",
                 "名称", "大小(字节)", "块数", "inode", "类型");
    if (n < 0 || (size_t)n >= buf_size - off) { pthread_mutex_unlock(&fs_mutex); return -1; }
    off += (size_t)n;

    Director_entry entries[directors_per_block];
    Inode* root_inode = &fscontext->inodetable[0];
    uint32_t block_num = root_inode->block;
    int count = 0;
    for (uint32_t i = 0; i < block_num; i++) {
        uint32_t block_addr = fs_get_file_block(root_inode, i + 1);
        if (block_addr == 0) break;
        if (fs_read_block(block_addr, entries) != 0) continue;
        for (int j = 0; j < directors_per_block; j++) {
            if (entries[j].inode_num != 0) {
                Inode* target_inode = &fscontext->inodetable[entries[j].inode_num];
                const char* type = target_inode->type == inode_file ? "文件" : "目录";
                n = snprintf(buf + off, buf_size - off,
                             " %-20s %-10u %-8u %-6u %s\n",
                             entries[j].filename, target_inode->size,
                             target_inode->block, entries[j].inode_num, type);
                if (n < 0 || (size_t)n >= buf_size - off) {
                    pthread_mutex_unlock(&fs_mutex);
                    return -1;
                }
                off += (size_t)n;
                count++;
            }
        }
    }
    n = snprintf(buf + off, buf_size - off, "共 %d 个成员\n", count);
    if (n < 0 || (size_t)n >= buf_size - off) { pthread_mutex_unlock(&fs_mutex); return -1; }
    off += (size_t)n;

    pthread_mutex_unlock(&fs_mutex);
    return (int)off;
}

/* 列出根目录成员，直接打印到 stdout（调试用） */
void fs_list_member(){
    pthread_mutex_lock(&fs_mutex);
    if(fscontext == NULL) {
        pthread_mutex_unlock(&fs_mutex);
        return;
    }
    printf("\n=== 根目录成员列表（所有用户/目录） ===\n");
    printf(" %-20s %-10s %-8s %-6s %s\n", "名称", "大小(字节)", "块数", "inode", "类型");
    Director_entry buf[directors_per_block];
    Inode* root_inode = &fscontext->inodetable[0];
    uint32_t block_num = root_inode->block;
    int count = 0;
    for(uint32_t i = 0;i < block_num;i++){
        uint32_t block_addr = fs_get_file_block(root_inode,i+1);
        if(block_addr == 0) break;
        if(fs_read_block(block_addr,buf) != 0){
            perror("读取目录块失败\n");
            continue;
        }
        for(int j = 0;j < directors_per_block;j++){
            if(buf[j].inode_num != 0){
                Inode* target_inode = &fscontext->inodetable[buf[j].inode_num];
                const char* type = target_inode->type == inode_file ? "文件" : "目录";
                printf(" %-20s %-10u %-8u %-6u %s\n",buf[j].filename,target_inode->size,target_inode->block,buf[j].inode_num,type);
                count++;
            }
        }
    }
    printf("共 %d 个成员\n", count);
    pthread_mutex_unlock(&fs_mutex);
}
