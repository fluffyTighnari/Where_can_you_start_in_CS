/*
 * Memory Allocator.h —— 旧入口，保留用于兼容既有包含路径
 *
 * 原实现把函数定义直接写在头文件里，任何被多个翻译单元包含的工程都会触发
 * 重复定义；同时它依赖 sbrk 却缺少 POSIX 特性宏，在严格 C11 下无法编译。
 * 现已拆分为：
 *   allocator.h —— 公开接口声明
 *   allocator.c —— 实现（编译时需要一并链接）
 *
 * 新代码请直接 #include "allocator.h"。
 */
#ifndef MEMORY_ALLOCATOR_H
#define MEMORY_ALLOCATOR_H

#include "allocator.h"

#endif /* MEMORY_ALLOCATOR_H */
