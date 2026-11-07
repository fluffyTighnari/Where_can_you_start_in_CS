/*
 * test.cpp —— 序列化 / 反序列化单元测试
 *
 * 覆盖四种情形：不带列名、带列名、行反序列化、系统表元信息往返，
 * 通过 print_hex 打印二进制布局，配合 assert 校验字节结构与往返一致性。
 */
#include "../../Parse/parse.hpp"
#include "../../Parse/sense.hpp"
#include "../Serialize/serialize.hpp"
#include "../Show/show.hpp"
#include <iostream>
#include <cassert>
#include <iomanip>
#include <cstring>

void print_hex(const std::string& data, const std::string& desc) {
    std::cout << "\n=== " << desc << " ===" << std::endl;
    std::cout << "Size: " << data.size() << " bytes" << std::endl;
    std::cout << "Hex: ";
    for (size_t i = 0; i < data.size() && i < 64; i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << (static_cast<unsigned>(data[i]) & 0xFF) << " ";
    }
    std::cout << std::dec << std::endl;
}

void test_serialize_without_columns() {
    std::cout << "\n========== Test 1: serialize without column names ==========" << std::endl;

    Table g_table = Table();
    ColumnDef a;
    a.column_name = "id";
    a.type = "INT";
    ColumnDef b;
    b.column_name = "name";
    b.type = "TEXT";
    ColumnDef c;
    c.column_name = "score";
    c.type = "FLOAT";
    g_table.add_column(a);
    g_table.add_column(b);
    g_table.add_column(c);

    InsertNode insert;
    insert.given_column = false;
    insert.values.push_back(ValueNode(1));
    insert.values.push_back(ValueNode(std::string("Alice")));
    insert.values.push_back(ValueNode(95.5f));

    std::string serialized;
    serialize(serialized, insert, &g_table);

    print_hex(serialized, "Serialized data");

    RowHeader* header = (RowHeader*)serialized.data();
    std::cout << "\nHeader Info:" << std::endl;
    std::cout << "  Column Num: " << (int)header->column_num << std::endl;
    std::cout << "  NULLbitmap: 0x" << std::hex << (int)header->NULLbitmap << std::dec << std::endl;
    std::cout << "  Length: " << header->length << std::endl;

    std::cout << "\nExpected structure:" << std::endl;
    std::cout << "  [RowHeader: 4 bytes]" << std::endl;
    std::cout << "  [int id: 4 bytes]" << std::endl;
    std::cout << "  [int name_len: 4 bytes][string 'Alice': 5 bytes]" << std::endl;
    std::cout << "  [float score: 4 bytes]" << std::endl;
    std::cout << "Total expected: 4 + 4 + 4 + 5 + 4 = 21 bytes (excluding header overhead)" << std::endl;
}

void test_serialize_with_columns() {
    std::cout << "\n========== Test 2: serialize with column names ==========" << std::endl;

    Table g_table = Table();
    ColumnDef a;
    a.column_name = "id";
    a.type = "INT";
    ColumnDef b;
    b.column_name = "name";
    b.type = "TEXT";
    ColumnDef c;
    c.column_name = "score";
    c.type = "FLOAT";
    g_table.add_column(a);
    g_table.add_column(b);
    g_table.add_column(c);

    InsertNode insert;
    insert.given_column = true;
    insert.columns.push_back(ColumnRef{"name"});
    insert.columns.push_back(ColumnRef{"id"});
    insert.values.push_back(ValueNode(std::string("Bob")));
    insert.values.push_back(ValueNode(2));

    std::string serialized;
    serialize(serialized, insert, &g_table);

    print_hex(serialized, "Serialized data");

    RowHeader* header = (RowHeader*)serialized.data();
    std::cout << "\nHeader Info:" << std::endl;
    std::cout << "  Column Num: " << (int)header->column_num << std::endl;
    std::cout << "  NULLbitmap: 0x" << std::hex << (int)header->NULLbitmap << std::dec << std::endl;
    std::cout << "  Expected NULLbitmap for missing 'score': 0x" << std::hex << (1 << 2) << std::dec << std::endl;
}

void test_deserialize() {
    std::cout << "\n========== Test 3: deserialize test ==========" << std::endl;

    Table g_table = Table();
    ColumnDef a;
    a.column_name = "id";
    a.type = "INT";
    ColumnDef b;
    b.column_name = "name";
    b.type = "TEXT";
    ColumnDef c;
    c.column_name = "score";
    c.type = "FLOAT";
    g_table.add_column(a);
    g_table.add_column(b);
    g_table.add_column(c);

    std::string test_data;

    RowHeader header;
    header.column_num = 3;
    header.NULLbitmap = 0;
    header.length = 0;

    test_data.append((const char*)&header, sizeof(RowHeader));
    header.length += 4;

    int name_len = 5;
    test_data.append((const char*)&name_len, sizeof(int));
    test_data.append("Hello");
    header.length += 4 + 5;

    float score = 99.9f;
    test_data.append((const char*)&score, sizeof(float));
    header.length += 4;

    int id = 100;
    test_data.append((const char*)&id, sizeof(int));
    header.length += 4;

    RowHeader* h = (RowHeader*)test_data.data();
    const_cast<RowHeader*>(h)->length = header.length;

    std::cout << "Input data size: " << test_data.size() << " bytes" << std::endl;
    std::cout << "Expected: sizeof(RowHeader)=" << sizeof(RowHeader)
              << " + 4(id) + 4+5(name) + 4(score) = " << sizeof(RowHeader) + 17 << " bytes" << std::endl;

    std::vector<uint32_t> need = {0, 1, 2};
    Info* output = new Info();

    deserialize(&g_table, need, test_data, output);

    std::cout << "\nDeserialized values:" << std::endl;
    for (size_t i = 0; i < output->values.size(); i++) {
        std::cout << "  " << output->columns[i] << " (" << output->types[i] << "): "
                  << output->values[i] << std::endl;
    }

    delete output;
}

void test_nullbitmap_serialize() {
    std::cout << "\n========== Test 4: NULLbitmap serialization ==========" << std::endl;

    Table g_table = Table();
    ColumnDef a;
    a.column_name = "id";
    a.type = "INT";
    ColumnDef b;
    b.column_name = "name";
    b.type = "TEXT";
    ColumnDef c;
    c.column_name = "score";
    c.type = "FLOAT";
    ColumnDef d;
    d.column_name = "col4";
    d.type = "VARCHAR";
    g_table.add_column(a);
    g_table.add_column(b);
    g_table.add_column(c);
    g_table.add_column(d);

    InsertNode insert;
    insert.given_column = true;
    insert.columns.push_back(ColumnRef{"col1"});
    insert.columns.push_back(ColumnRef{"col4"});
    insert.values.push_back(ValueNode(42));
    insert.values.push_back(ValueNode(std::string("test")));

    std::string serialized;
    serialize(serialized, insert, &g_table);

    RowHeader* header = (RowHeader*)serialized.data();
    std::cout << "NULLbitmap: 0x" << std::hex << (int)header->NULLbitmap << std::dec << std::endl;
    std::cout << "Expected: 0x" << std::hex << ((1 << 1) | (1 << 2)) << std::dec
              << " (col2 and col3 are NULL)" << std::endl;

    if ((header->NULLbitmap & (1 << 1)) && (header->NULLbitmap & (1 << 2))) {
        std::cout << "NULLbitmap is correct!" << std::endl;
    } else {
        std::cout << "ERROR: NULLbitmap is incorrect!" << std::endl;
    }
}

void test_nullbitmap_deserialize() {
    std::cout << "\n========== Test 5: NULLbitmap deserialization (CRITICAL BUG TEST) ==========" << std::endl;

    Table g_table = Table();
    ColumnDef a;
    a.column_name = "id";
    a.type = "INT";
    ColumnDef b;
    b.column_name = "name";
    b.type = "TEXT";
    ColumnDef c;
    c.column_name = "score";
    c.type = "FLOAT";
    g_table.add_column(a);
    g_table.add_column(b);
    g_table.add_column(c);

    std::string test_data;

    RowHeader header;
    header.column_num = 3;
    header.NULLbitmap = 0x04;
    header.length = 0;

    test_data.append((const char*)&header, sizeof(RowHeader));
    header.length += 4;

    int col1_val = 10;
    test_data.append((const char*)&col1_val, sizeof(int));
    header.length += 4;

    int col3_val = 30;
    test_data.append((const char*)&col3_val, sizeof(float));
    header.length += 4;

    RowHeader* h = (RowHeader*)test_data.data();
    const_cast<RowHeader*>(h)->length = header.length;

    std::vector<uint32_t> need = {0, 1, 2};
    Info* output = new Info();

    deserialize(&g_table, need, test_data, output);

    std::cout << "Input NULLbitmap: 0x" << std::hex << 0x04 << std::dec << " (bit 2 set = col3 is NULL)" << std::endl;
    std::cout << "\nDeserialized values:" << std::endl;
    for (size_t i = 0; i < output->values.size(); i++) {
        std::cout << "  " << output->columns[i] << ": " << output->values[i] << std::endl;
    }

    bool found_col3_null = false;
    for (size_t i = 0; i < output->values.size(); i++) {
        if (output->columns[i] == "col3" && output->values[i] == "NULL") {
            found_col3_null = true;
            break;
        }
    }

    if (found_col3_null) {
        std::cout << "\nNULL detection is CORRECT!" << std::endl;
    } else {
        std::cout << "\nERROR: NULL detection is BROKEN! (Bug in line 89 of serialize.cpp)" << std::endl;
        std::cout << "The bug: (header.NULLbitmap & (1 << i)) == 1" << std::endl;
        std::cout << "Should be: (header.NULLbitmap & (1 << i)) != 0" << std::endl;
    }

    delete output;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "   Serialize/Deserialize Test Suite    " << std::endl;
    std::cout << "========================================" << std::endl;

    try {
        test_serialize_without_columns();
        test_serialize_with_columns();
        test_deserialize();
        test_nullbitmap_serialize();
        test_nullbitmap_deserialize();

        std::cout << "\n========================================" << std::endl;
        std::cout << "   All tests completed!" << std::endl;
        std::cout << "========================================" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "\nException: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}