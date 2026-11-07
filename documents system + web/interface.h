/*
 * interface.h —— 文件系统对外 API（供 Web 层调用）
 *
 * 设计约定：
 *   - 目录结构只有两层：根目录下直接是用户目录（dirname 即用户名），
 *     用户目录下是该用户的文件，因此文件名在用户内唯一。
 *   - 除 fs_create / fs_mount / fs_unmount 外，所有接口内部都会加 fs_mutex，
 *     可被多个工作线程并发调用。
 *   - 返回 0 或字节数表示成功，-1 表示失败。
 */
#ifndef INTERFACE_H
#define INTERFACE_H
#include "help.h"
#endif

/* 创建一块全新的磁盘镜像并格式化（已存在则直接返回） */
void fs_create(const char* disk_path);

int fs_mount(const char* disk_path);

void fs_unmount();

/* 在 current_dir 下创建目录；current_dir 为 NULL 表示根目录 */
int fs_mkdir(const char* dirname, const char* current_dir);

/* 在 dirname 目录下创建/删除/读/写/打印文件；dirname 为根目录下的目录名（如用户名） */
int fs_create_file(const char* filename, const char* dirname);

int fs_delete_file(const char* filename, const char* dirname);

int fs_read_file(const char* filename, const char* dirname, void* buffer, uint32_t size, uint32_t offset);

int fs_write_file(const char* filename, const char* dirname, const void* buffer, uint32_t size, uint32_t offset);

void fs_cat_file(const char* filename, const char* dirname);

void fs_show_information();

/* 列出 dirname 目录内容（dirname 为根目录下的目录名） */
void fs_list_director(const char* dirname);

/* 列出根目录成员（即所有已存在的目录名，如已登录用户） */
void fs_list_member();

/* 将目录列表格式化到 buf 中，返回写入字节数（不含结尾 '\0'），失败返回 -1 */
int fs_list_director_to_buf(const char* dirname, char* buf, size_t buf_size);
int fs_list_member_to_buf(char* buf, size_t buf_size);
