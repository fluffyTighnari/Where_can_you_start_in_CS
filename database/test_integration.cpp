/*
 * test_integration.cpp —— 端到端集成测试
 *
 * 在每个用例中走完整流水线：SQL 字符串 → tokenize → parse → sense → execute，
 * 然后断言结果集内容，用于回归验证各层协同工作的正确性。
 *
 * 三组宏构成一个极简的断言框架：
 *   TEST(name)  打印用例名并开始计时点
 *   PASS()      记录通过
 *   FAIL(msg)   记录失败并 assert 中断，便于定位
 * 统计结果在程序末尾汇总，g_failed 非零时返回非零退出码。
 */
#include <iostream>
#include <string>
#include <memory>
#include <variant>
#include <cassert>
#include <vector>

#include "Parse/parse.hpp"
#include "Parse/sense.hpp"
#include "Storage_engine/storage.hpp"
#include "Storage_engine/pager.hpp"
#include "Execute/executor.hpp"
#include "Help/Error/error.hpp"
#include "Help/Show/show.hpp"

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) do { \
    std::cout << "[RUN ] " << name << " ... " << std::flush; \
} while(0)

#define PASS() do { \
    std::cout << "PASS\n"; \
    g_passed++; \
} while(0)

#define FAIL(msg) do { \
    std::cout << "FAIL: " << msg << "\n"; \
    g_failed++; \
    assert(false); \
} while(0)

static std::unique_ptr<Storage> create_test_storage() {
    auto pager = std::make_unique<Pager_t>();
    auto symbol_table = InitSymbolTable(*pager);
    auto storage_result = InitStorage(std::move(pager), std::move(symbol_table));
    if (std::holds_alternative<Error>(storage_result)) {
        return nullptr;
    }
    return std::move(std::get<std::unique_ptr<Storage>>(storage_result));
}

static std::unique_ptr<Info> run_sql(Storage* storage, const std::string& sql) {
    std::string input = sql;
    auto cmd_result = parse_command(input);
    if (std::holds_alternative<Error>(cmd_result)) {
        return nullptr;
    }
    auto command = std::move(std::get<std::unique_ptr<Command>>(cmd_result));

    auto meaning_result = parse_meaning(command.get(), *storage);
    if (std::holds_alternative<Error>(meaning_result)) {
        return nullptr;
    }

    auto exec_result = execute_command(command.get(), storage);
    if (std::holds_alternative<Error>(exec_result)) {
        return nullptr;
    }
    return std::move(std::get<std::unique_ptr<Info>>(exec_result));
}

static void test_create_and_insert() {
    TEST("create table + insert single row");
    auto storage = create_test_storage();
    assert(storage != nullptr);

    auto info = run_sql(storage.get(), "create table users (id int, name text);");
    assert(info != nullptr);

    info = run_sql(storage.get(), "insert into users values(1, 'alice');");
    assert(info != nullptr);
    assert(info->output == "Insert successful.");

    PASS();
}

static void test_select_single_row() {
    TEST("select single row by primary key");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table users (id int, name text);");
    run_sql(storage.get(), "insert into users values(1, 'alice');");

    auto info = run_sql(storage.get(), "select * from users where id = 1;");
    assert(info != nullptr);
    // columns 存列名，values 存值（每行 N 个值，展平到一维数组）
    assert(info->columns.size() == 2);  // id, name
    assert(info->values.size() == 2);   // 1 行 * 2 列
    assert(info->values[1] == "alice");

    PASS();
}

static void test_select_all() {
    TEST("select * (full table scan)");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table t (id int, val int);");

    for (int i = 1; i <= 20; i++) {
        run_sql(storage.get(), "insert into t values(" + std::to_string(i) + ", " + std::to_string(i*10) + ");");
    }

    auto info = run_sql(storage.get(), "select * from t;");
    assert(info != nullptr);
    assert(info->columns.size() == 2);   // 2 列
    assert(info->values.size() == 40);   // 20 行 * 2 列 = 40

    PASS();
}

static void test_delete_single_row() {
    TEST("delete single row");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table t (id int, val text);");
    run_sql(storage.get(), "insert into t values(1, 'a');");
    run_sql(storage.get(), "insert into t values(2, 'b');");

    auto info = run_sql(storage.get(), "delete from t where id = 1;");
    assert(info != nullptr);

    info = run_sql(storage.get(), "select * from t;");
    assert(info != nullptr);
    assert(info->values.size() == 2);  // 1 行 * 2 列

    PASS();
}

static void test_delete_all_rows() {
    TEST("delete all rows (range delete)");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table t (id int, val text);");
    for (int i = 1; i <= 10; i++) {
        run_sql(storage.get(), "insert into t values(" + std::to_string(i) + ", 'x');");
    }

    auto info = run_sql(storage.get(), "delete from t;");
    assert(info != nullptr);
    assert(info->output == "All rows deleted.");

    info = run_sql(storage.get(), "select * from t;");
    assert(info != nullptr);
    // 删除后表仍存在但无数据
    assert(info->values.empty());

    PASS();
}

static void test_insert_after_split() {
    TEST("insert triggers root split then select");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table t (id int, val text);");

    for (int i = 1; i <= 20; i++) {
        auto info = run_sql(storage.get(), "insert into t values(" + std::to_string(i) + ", 'v" + std::to_string(i) + "');");
        assert(info != nullptr);
    }

    auto info = run_sql(storage.get(), "select * from t;");
    assert(info != nullptr);
    assert(info->values.size() == 40);  // 20 行 * 2 列

    // 单独查每个 key
    for (int i = 1; i <= 20; i++) {
        info = run_sql(storage.get(), "select * from t where id = " + std::to_string(i) + ";");
        assert(info != nullptr);
        assert(info->values.size() == 2);  // 1 行 * 2 列
    }

    PASS();
}

static void test_many_inserts_and_deletes() {
    TEST("many inserts then sequential deletes");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table t (id int, val int);");

    const int N = 100;
    for (int i = 1; i <= N; i++) {
        auto info = run_sql(storage.get(), "insert into t values(" + std::to_string(i) + ", " + std::to_string(i*2) + ");");
        assert(info != nullptr);
    }

    auto info = run_sql(storage.get(), "select * from t;");
    assert(info != nullptr);
    assert((int)info->values.size() == N * 2);  // N 行 * 2 列

    // 删除前一半
    for (int i = 1; i <= N / 2; i++) {
        auto info2 = run_sql(storage.get(), "delete from t where id = " + std::to_string(i) + ";");
        assert(info2 != nullptr);
    }

    info = run_sql(storage.get(), "select * from t;");
    assert(info != nullptr);
    assert((int)info->values.size() == (N / 2) * 2);

    // 剩余的都能查到
    for (int i = N/2 + 1; i <= N; i++) {
        info = run_sql(storage.get(), "select * from t where id = " + std::to_string(i) + ";");
        assert(info != nullptr);
        assert(info->values.size() == 2);  // 1 行 * 2 列
    }

    PASS();
}

static void test_duplicate_insert_rejected() {
    TEST("duplicate primary key rejected");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table t (id int, val text);");
    run_sql(storage.get(), "insert into t values(1, 'a');");

    auto info = run_sql(storage.get(), "insert into t values(1, 'b');");
    assert(info == nullptr);  // 应该报错

    PASS();
}

static void test_select_nonexistent() {
    TEST("select nonexistent key returns empty");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table t (id int, val text);");
    run_sql(storage.get(), "insert into t values(1, 'a');");

    auto info = run_sql(storage.get(), "select * from t where id = 999;");
    assert(info != nullptr);
    assert(info->values.empty());

    PASS();
}

static void test_varchar_key() {
    TEST("varchar primary key");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table t (name varchar, age int);");
    run_sql(storage.get(), "insert into t values('bob', 30);");
    run_sql(storage.get(), "insert into t values('alice', 25);");
    run_sql(storage.get(), "insert into t values('charlie', 35);");

    auto info = run_sql(storage.get(), "select * from t where name = 'alice';");
    assert(info != nullptr);
    assert(info->values.size() == 2);  // 1 行 * 2 列

    info = run_sql(storage.get(), "select * from t;");
    assert(info != nullptr);
    assert(info->values.size() == 6);  // 3 行 * 2 列

    PASS();
}

static void test_float_key() {
    TEST("float primary key");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table t (score float, name text);");
    run_sql(storage.get(), "insert into t values(95.5, 'alice');");
    run_sql(storage.get(), "insert into t values(88.0, 'bob');");

    auto info = run_sql(storage.get(), "select * from t where score = 95.5;");
    assert(info != nullptr);
    assert(info->values.size() == 2);  // 1 行 * 2 列

    PASS();
}

static void test_update_int_column() {
    TEST("update int column");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table t (id int, age int);");
    run_sql(storage.get(), "insert into t values(1, 25);");

    auto info = run_sql(storage.get(), "update t set age = 30 where id = 1;");
    assert(info != nullptr);
    assert(info->output == "Update successful.");

    info = run_sql(storage.get(), "select * from t where id = 1;");
    assert(info != nullptr);
    assert(info->values.size() == 2);
    assert(info->values[0] == "1");
    assert(info->values[1] == "30");

    PASS();
}

static void test_update_varchar_column() {
    TEST("update varchar column");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table t (id int, name text);");
    run_sql(storage.get(), "insert into t values(1, 'alice');");

    auto info = run_sql(storage.get(), "update t set name = 'bob' where id = 1;");
    assert(info != nullptr);

    info = run_sql(storage.get(), "select * from t where id = 1;");
    assert(info != nullptr);
    assert(info->values.size() == 2);
    assert(info->values[1] == "bob");

    PASS();
}

static void test_update_nonexistent_key() {
    TEST("update nonexistent key returns no match");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table t (id int, val int);");
    run_sql(storage.get(), "insert into t values(1, 10);");

    auto info = run_sql(storage.get(), "update t set val = 20 where id = 999;");
    assert(info != nullptr);
    assert(info->output == "No rows matched.");

    // 原值不变
    info = run_sql(storage.get(), "select * from t where id = 1;");
    assert(info != nullptr);
    assert(info->values[1] == "10");

    PASS();
}

static void test_update_multiple_rows_independent() {
    TEST("update one row doesn't affect others");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table t (id int, val text);");
    run_sql(storage.get(), "insert into t values(1, 'a');");
    run_sql(storage.get(), "insert into t values(2, 'b');");
    run_sql(storage.get(), "insert into t values(3, 'c');");

    run_sql(storage.get(), "update t set val = 'xxx' where id = 2;");

    auto info = run_sql(storage.get(), "select * from t;");
    assert(info != nullptr);
    assert(info->values.size() == 6);  // 3 行 * 2 列
    assert(info->values[1] == "a");
    assert(info->values[3] == "xxx");
    assert(info->values[5] == "c");

    PASS();
}

static void test_select_specific_columns() {
    TEST("select specific columns");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table t (id int, name text, age int);");
    run_sql(storage.get(), "insert into t values(1, 'alice', 25);");

    auto info = run_sql(storage.get(), "select name, age from t where id = 1;");
    assert(info != nullptr);
    assert(info->columns.size() == 2);
    // 列名被解析器转为大写（SQL 标准行为）
    assert(info->columns[0] == "NAME");
    assert(info->columns[1] == "AGE");
    assert(info->values.size() == 2);
    assert(info->values[0] == "alice");
    assert(info->values[1] == "25");

    PASS();
}

static void test_multiple_tables() {
    TEST("multiple tables independent");
    auto storage = create_test_storage();
    run_sql(storage.get(), "create table users (id int, name text);");
    run_sql(storage.get(), "create table orders (id int, total int);");

    run_sql(storage.get(), "insert into users values(1, 'alice');");
    run_sql(storage.get(), "insert into orders values(1, 100);");

    auto info = run_sql(storage.get(), "select * from users where id = 1;");
    assert(info != nullptr);
    assert(info->values.size() == 2);
    assert(info->values[1] == "alice");

    info = run_sql(storage.get(), "select * from orders where id = 1;");
    assert(info != nullptr);
    assert(info->values.size() == 2);
    assert(info->values[1] == "100");

    PASS();
}

static std::unique_ptr<Storage> simulate_restart(std::unique_ptr<Storage> old_storage) {
    auto pager = old_storage->release_pager();
    pager->new_db = false;

    auto new_symbol_table = InitSymbolTable(*pager);

    auto storage_result = InitStorage(std::move(pager), std::move(new_symbol_table));
    if (std::holds_alternative<Error>(storage_result)) {
        return nullptr;
    }
    return std::move(std::get<std::unique_ptr<Storage>>(storage_result));
}

static void test_persistence_single_table() {
    TEST("persistence: table and data survive restart");
    auto storage = create_test_storage();

    run_sql(storage.get(), "create table users (id int, name text);");
    run_sql(storage.get(), "insert into users values(1, 'alice');");
    run_sql(storage.get(), "insert into users values(2, 'bob');");

    auto info = run_sql(storage.get(), "select * from users where id = 1;");
    assert(info != nullptr);
    assert(info->values[1] == "alice");

    storage = simulate_restart(std::move(storage));
    assert(storage != nullptr);

    info = run_sql(storage.get(), "select * from users where id = 1;");
    assert(info != nullptr);
    assert(info->columns.size() == 2);
    assert(info->values.size() == 2);
    assert(info->values[0] == "1");
    assert(info->values[1] == "alice");

    info = run_sql(storage.get(), "select * from users where id = 2;");
    assert(info != nullptr);
    assert(info->values[1] == "bob");

    info = run_sql(storage.get(), "select * from users;");
    assert(info != nullptr);
    assert(info->values.size() == 4);

    PASS();
}

static void test_persistence_multiple_tables() {
    TEST("persistence: multiple tables survive restart");
    auto storage = create_test_storage();

    run_sql(storage.get(), "create table users (id int, name text);");
    run_sql(storage.get(), "create table orders (id int, total int);");
    run_sql(storage.get(), "insert into users values(1, 'alice');");
    run_sql(storage.get(), "insert into orders values(100, 999);");

    storage = simulate_restart(std::move(storage));
    assert(storage != nullptr);

    auto info = run_sql(storage.get(), "select * from users where id = 1;");
    assert(info != nullptr);
    assert(info->values[1] == "alice");

    info = run_sql(storage.get(), "select * from orders where id = 100;");
    assert(info != nullptr);
    assert(info->values[1] == "999");

    PASS();
}

static void test_persistence_duplicate_after_restart() {
    TEST("persistence: duplicate table rejected after restart");
    auto storage = create_test_storage();

    run_sql(storage.get(), "create table t (id int);");

    storage = simulate_restart(std::move(storage));
    assert(storage != nullptr);

    auto info = run_sql(storage.get(), "create table t (id int);");
    assert(info == nullptr);

    PASS();
}

static void test_persistence_insert_after_restart() {
    TEST("persistence: insert and delete work after restart");
    auto storage = create_test_storage();

    run_sql(storage.get(), "create table items (id int, val text);");
    run_sql(storage.get(), "insert into items values(1, 'a');");

    storage = simulate_restart(std::move(storage));
    assert(storage != nullptr);

    auto info = run_sql(storage.get(), "insert into items values(2, 'b');");
    assert(info != nullptr);
    assert(info->output == "Insert successful.");

    info = run_sql(storage.get(), "select * from items;");
    assert(info != nullptr);
    assert(info->values.size() == 4);

    info = run_sql(storage.get(), "delete from items where id = 1;");
    assert(info != nullptr);
    assert(info->output == "Delete successful.");

    info = run_sql(storage.get(), "select * from items;");
    assert(info != nullptr);
    assert(info->values.size() == 2);
    assert(info->values[1] == "b");

    PASS();
}

static void test_drop_table() {
    TEST("drop table removes table and data");
    auto storage = create_test_storage();

    run_sql(storage.get(), "create table users (id int, name text);");
    run_sql(storage.get(), "insert into users values(1, 'alice');");
    run_sql(storage.get(), "insert into users values(2, 'bob');");

    auto info = run_sql(storage.get(), "drop table users;");
    assert(info != nullptr);
    assert(info->output == "Table dropped successfully.");

    info = run_sql(storage.get(), "select * from users where id = 1;");
    assert(info == nullptr);

    info = run_sql(storage.get(), "create table users (id int, name text);");
    assert(info != nullptr);
    assert(info->output == "Table created successfully.");

    PASS();
}

static void test_drop_nonexistent_table() {
    TEST("drop nonexistent table returns error");
    auto storage = create_test_storage();

    auto info = run_sql(storage.get(), "drop table no_such_table;");
    assert(info == nullptr);

    PASS();
}

static void test_drop_persistence() {
    TEST("drop table persists across restart");
    auto storage = create_test_storage();

    run_sql(storage.get(), "create table users (id int, name text);");
    run_sql(storage.get(), "create table orders (id int, total int);");
    run_sql(storage.get(), "insert into users values(1, 'alice');");

    storage = simulate_restart(std::move(storage));
    assert(storage != nullptr);

    auto info = run_sql(storage.get(), "drop table users;");
    assert(info != nullptr);

    storage = simulate_restart(std::move(storage));
    assert(storage != nullptr);

    info = run_sql(storage.get(), "select * from users where id = 1;");
    assert(info == nullptr);

    info = run_sql(storage.get(), "select * from orders where id = 1;");
    assert(info != nullptr);

    info = run_sql(storage.get(), "create table users (id int, name text);");
    assert(info != nullptr);

    PASS();
}

static void test_show_tables() {
    TEST("show tables lists all tables with primary key info");
    auto storage = create_test_storage();

    auto info = run_sql(storage.get(), "show tables;");
    assert(info != nullptr);
    assert(info->columns.size() == 3);
    assert(info->columns[0] == "TABLE_NAME");
    assert(info->columns[1] == "PRIMARY_KEY");
    assert(info->columns[2] == "PK_TYPE");
    assert(info->values.size() == 0);

    run_sql(storage.get(), "create table users (id int, name text);");
    run_sql(storage.get(), "create table orders (oid float, total int);");

    info = run_sql(storage.get(), "show tables;");
    assert(info != nullptr);
    assert(info->values.size() == 6); // 2 rows * 3 cols

    bool has_users = false, has_orders = false;
    for(size_t i = 0; i < info->values.size(); i += 3){
        if(info->values[i] == "USERS"){
            has_users = true;
            assert(info->values[i+1] == "ID");
            assert(info->values[i+2] == "INT");
        }
        if(info->values[i] == "ORDERS"){
            has_orders = true;
            assert(info->values[i+1] == "OID");
            assert(info->values[i+2] == "FLOAT");
        }
    }
    assert(has_users);
    assert(has_orders);

    run_sql(storage.get(), "drop table users;");
    info = run_sql(storage.get(), "show tables;");
    assert(info != nullptr);
    assert(info->values.size() == 3); // 1 row * 3 cols
    assert(info->values[0] == "ORDERS");

    PASS();
}

static void test_show_tables_persistence() {
    TEST("show tables persists across restart");
    auto storage = create_test_storage();

    run_sql(storage.get(), "create table t1 (id int, name text);");
    run_sql(storage.get(), "create table t2 (sid text, val int);");

    storage = simulate_restart(std::move(storage));
    assert(storage != nullptr);

    auto info = run_sql(storage.get(), "show tables;");
    assert(info != nullptr);
    assert(info->values.size() == 6); // 2 rows * 3 cols

    bool has_t1 = false, has_t2 = false;
    for(size_t i = 0; i < info->values.size(); i += 3){
        if(info->values[i] == "T1"){
            has_t1 = true;
            assert(info->values[i+1] == "ID");
            assert(info->values[i+2] == "INT");
        }
        if(info->values[i] == "T2"){
            has_t2 = true;
            assert(info->values[i+1] == "SID");
            assert(info->values[i+2] == "TEXT");
        }
    }
    assert(has_t1);
    assert(has_t2);

    PASS();
}

int main() {
    std::cout << "========== Integration Test Suite ==========\n\n";

    test_create_and_insert();
    test_select_single_row();
    test_select_all();
    test_delete_single_row();
    test_delete_all_rows();
    test_insert_after_split();
    test_many_inserts_and_deletes();
    test_duplicate_insert_rejected();
    test_select_nonexistent();
    test_varchar_key();
    test_float_key();
    test_update_int_column();
    test_update_varchar_column();
    test_update_nonexistent_key();
    test_update_multiple_rows_independent();
    test_select_specific_columns();
    test_multiple_tables();
    test_persistence_single_table();
    test_persistence_multiple_tables();
    test_persistence_duplicate_after_restart();
    test_persistence_insert_after_restart();
    test_drop_table();
    test_drop_nonexistent_table();
    test_drop_persistence();
    test_show_tables();
    test_show_tables_persistence();

    std::cout << "\n========== Summary ==========\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";
    std::cout << "Total:  " << g_passed + g_failed << "\n";

    return g_failed == 0 ? 0 : 1;
}
