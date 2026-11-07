//Main.cpp
/*
 * 数据库入口：初始化各层组件，然后进入 REPL 循环。
 *
 * 启动顺序（后面的依赖前面的）：
 *   InitPager      打开/创建 sql.db，得到页管理器
 *   InitSymbolTable 扫描系统 B+ 树，恢复所有表的元信息
 *   InitStorage    把二者装进 Storage 门面
 *
 * 每条 SQL 的处理流水线：
 *   tokenize → parse_command（语法）→ parse_meaning（语义）→ execute_command（执行）
 * 任何一步返回 Error 都只打印错误并继续下一条，不会终止进程。
 *
 * 设计上的简化原则：
 *   每个表只允许以 int 类型为主键（为了方便）
 *   where 谓词只支持单个等值条件（为了简化）
 */
#include <iostream>
#include <string>
#include <cstdio>
#include <memory>
#include <variant>
#include "Parse/parse.hpp"
#include "Storage_engine/storage.hpp"
#include "Execute/executor.hpp"
#include "Storage_engine/pager.hpp"
#include "Parse/sense.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using std::cout;
using std::string;
using std::unique_ptr;
using std::move;

/* Windows 控制台默认不是 UTF-8，中文输出会乱码，这里显式切换代码页 */
static void setup_console_encoding(){
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

int main(){
    setup_console_encoding();

    /* 第一层：页管理器，负责 sql.db 的页读写与缓存 */
    auto pager_result = InitPager("sql.db");
    if(std::holds_alternative<Error>(pager_result)){
        auto& err = std::get<Error>(pager_result);
        std::cerr << err.error_message() << std::endl;
        return -1;
    }
    auto pager = std::move(std::get<unique_ptr<Pager_t>>(pager_result));

    /* 第二层：符号表，从系统 B+ 树恢复所有表的元信息 */
    auto symbol_table = InitSymbolTable(*pager);

    /* 第三层：Storage 门面，统一持有 pager 与 symbol_table */
    auto storage_result = InitStorage(std::move(pager), std::move(symbol_table));
    if(std::holds_alternative<Error>(storage_result)){
        auto& err = std::get<Error>(storage_result);
        std::cerr << err.error_message() << std::endl;
        return -1;
    }
    auto storage = std::move(std::get<unique_ptr<Storage>>(storage_result));

    printf("Database started successfully.\nSupported commands:\n"
           "  create table table_name (col1 type1, col2 type2...);\n"
           "    types: int, float, text, varchar (first column is primary key)\n"
           "  drop table table_name;\n"
           "  show tables;\n"
           "  insert into table_name (col1,col2,...) values(val1,val2,...);\n"
           "  insert into table_name values(val1,val2,...);\n"
           "  select * from table_name [where col=val];\n"
           "  select col1,col2 from table_name [where col=val];\n"
           "  delete from table_name [where col=val];\n"
           "  update table_name set col=val where col=val;\n"
           "Note: predicate only supports single equality on primary key.\n\n");

    while(true){
        cout << "SQLite >>";
        string input;
        if(!getline(std::cin, input)){
            break;   /* EOF（Ctrl+D / Ctrl+Z）退出 */
        }
        if(input.empty()){
            continue;
        }

        /* 第一段：词法 + 语法，产出 AST */
        auto command_result = parse_command(input);
        if(std::holds_alternative<Error>(command_result)){
            auto& err = std::get<Error>(command_result);
            std::cout << err.error_message() << std::endl;
            continue;
        }
        auto command = std::move(std::get<unique_ptr<Command>>(command_result));

        /* 第二段：语义检查，确认表/列存在且类型匹配 */
        auto meaning_result = parse_meaning(command.get(), *storage);
        if(std::holds_alternative<Error>(meaning_result)){
            auto& err = std::get<Error>(meaning_result);
            std::cout << err.error_message() << std::endl;
            continue;
        }

        /* 第三段：执行，返回结果集或一句提示 */
        auto exec_result = execute_command(command.get(), storage.get());
        if(std::holds_alternative<Error>(exec_result)){
            auto& err = std::get<Error>(exec_result);
            std::cout << err.error_message() << std::endl;
            continue;
        }
        auto info = std::move(std::get<unique_ptr<Info>>(exec_result));

        if(info->need_print){
            info->print_message();   /* 结果集 → 表格 */
        }
        if(!info->output.empty()){
            std::cout << info->output << std::endl;   /* 提示文本 */
        }
    }
    return 0;
}
