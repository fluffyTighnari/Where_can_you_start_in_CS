#pragma once
#include "Parse/parse.hpp"
#include "Help/Show/show.hpp"
#include "Parse/sense.hpp"
#include <string>

/*
 * 行数据的序列化格式（紧凑二进制，直接以内存布局写入）：
 *
 *   [RowHeader][int:val][size][string:val]... （按表定义中的列顺序依次排列）
 *   RowHeader = { length: uint16, column_num: uint8, NULLbitmap: uint8 }
 *
 *   - 定长列（INT / FLOAT）固定占 4 字节
 *   - 变长列（TEXT / VARCHAR）前面带一个 int 长度
 *   - NULLbitmap 第 i 位为 1 表示第 i 列为 NULL，此时该列不占空间
 *
 * 系统表使用另一套布局，见 serialize_system。
 */

/* 把一条 insert 语句的值序列化成一行二进制数据 */
void serialize(std::string& input, InsertNode& insert, const Table* table);

/* 从二进制数据中解析出行，只取出 need 中指定的列（其余列跳读） */
void deserialize(const Table* table,std::vector<uint32_t>& need,std::string& input, Info* output);

/* 由已解析成字符串的列值重新拼装一行（update 用） */
bool serialize_row_from_strings(const Table* table, const std::vector<std::string>& values, std::string& output);

void serialize_system(const std::string& table_name, CreateNode& create, std::string& input, uint32_t newpage);

bool deserialize_system(const std::string& input, std::string& out_table_name, uint32_t& out_root_page,
                        std::vector<std::string>& out_col_names,
                        std::vector<std::string>& out_col_types);

/* 每行数据共用的头部，按 1 字节对齐以保证磁盘上的布局固定 */
#pragma pack(push, 1)
struct RowHeader{
    uint16_t length;      /* 本行数据的字节数（不含头部自身） */
    uint8_t column_num;   /* 列数 */
    uint8_t NULLbitmap;   /* 第 i 位为 1 表示第 i 列为 NULL */
};
#pragma pack(pop)
