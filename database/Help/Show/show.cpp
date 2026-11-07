#include "show.hpp"
#include <iomanip>
#include <iostream>
#include <vector>
#include <string>

/*
 * 把结果渲染成 ASCII 表格，形如：
 *   +------+-------+
 *   | id   | name  |
 *   +------+-------+
 *   | INT  | TEXT  |
 *   +------+-------+
 *   | 1    | alice |
 *   +------+-------+
 *
 * 排版分两趟：先算出每列宽度（列名、类型名、所有数据中最长者 + 2 的留白），
 * 再统一按宽度输出，保证边框对齐。
 */
void Info::print_message(){
    size_t col_count = columns.size();
    if (col_count == 0 || col_count != types.size()) {
        std::cout << "Empty result." << std::endl;
        return;
    }

    size_t row_count = values.size() / col_count;

    /* 第一趟：确定每列宽度 */
    std::vector<size_t> widths(col_count);
    for (size_t i = 0; i < col_count; ++i) {
        widths[i] = std::max(columns[i].size(), types[i].size()) + 2;
    }
    for (size_t r = 0; r < row_count; ++r) {
        for (size_t i = 0; i < col_count; ++i) {
            size_t val_len = values[r * col_count + i].size();
            if (val_len + 2 > widths[i]) {
                widths[i] = val_len + 2;
            }
        }
    }

    /* 第二趟：按宽度输出，分隔线由每列宽度的 '-' 拼成 */
    auto print_line = [&]() {
        for (size_t w : widths)
            std::cout << "+" << std::string(w, '-');
        std::cout << "+" << std::endl;
    };

    print_line();

    for (size_t i = 0; i < col_count; ++i)
        std::cout << "| " << std::left << std::setw(widths[i]-1) << columns[i];
    std::cout << "|" << std::endl;

    print_line();

    for (size_t i = 0; i < col_count; ++i)
        std::cout << "| " << std::left << std::setw(widths[i]-1) << types[i];
    std::cout << "|" << std::endl;

    print_line();

    for (size_t r = 0; r < row_count; ++r) {
        for (size_t i = 0; i < col_count; ++i)
            std::cout << "| " << std::left << std::setw(widths[i]-1) << values[r * col_count + i];
        std::cout << "|" << std::endl;
    }

    print_line();

    std::cout << row_count << " row(s) in set." << std::endl;
}
