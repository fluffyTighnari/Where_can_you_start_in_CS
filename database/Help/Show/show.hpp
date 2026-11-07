#pragma once

#include <vector>
#include <string>

/*
 * 查询结果的展示载体。
 *
 * 列式存放：values 是展平的一维数组，第 r 行第 c 列位于 values[r * columns.size() + c]。
 * 两种落地方式：
 *   need_print = true  → 用 print_message() 渲染成 ASCII 表格（select / show tables）
 *   output 非空        → 直接打印一句提示文本（create / insert / delete 等）
 */
struct Info{
    bool need_print = false;
    std::vector<std::string> columns;   /* 列名 */
    std::vector<std::string> types;     /* 列类型名 */
    std::vector<std::string> values;    /* 按行优先展平的数据 */
    std::string output;

    void print_message();
};
