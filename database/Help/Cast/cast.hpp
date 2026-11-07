#pragma once

#include <cstddef>
#include <cstring>

/*
 * page_cast：把一页原始字节 reinterpret_cast 成指定的结构体类型。
 * 用于在页缓存（char[PAGE_SIZE]）与结构化视图（如 Btree_Node）之间切换，
 * 不拷贝数据，只是换个看法。
 *
 * 两个静态断言把约束提前到编译期：
 *   1. 结构体必须是 standard layout —— 否则内存布局不保证可预测，不能直接映射
 *   2. 结构体大小不能超过一页
 */
template <typename T>
inline T* page_cast(char* page_data) {
    static_assert(std::is_standard_layout_v<T>,"page_cast:nonstandard struct is not supported");
    static_assert(sizeof(T) <= 4096,"page_cast:the size of struct is bigger than a page");
    return reinterpret_cast<T*>(page_data);
}

template <typename T>
inline const T* page_cast(const char* page_data) {
    static_assert(std::is_standard_layout_v<T>,"page_cast:page_cast:nonstandard struct is not supported");
    static_assert(sizeof(T) <= 4096,"page_cast:the size of struct is bigger than a page");
    return reinterpret_cast<const T*>(page_data);
}
