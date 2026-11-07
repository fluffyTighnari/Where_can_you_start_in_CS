/*
 * test.cpp —— 词法 / 语法 / 语义三层的交互式测试
 *
 * 三个 DEBUG_* 宏分别控制是否启用对应层的测试，可以单独打开任意一层：
 *   DEBUG_TOKENIZE  打印 tokenize 的结果
 *   DEBUG_PARSE     打印 parse_command 产出的 AST
 *   DEBUG_SENSE     执行语义检查
 * 通过标准输入逐行喂 SQL，适合手工验证解析行为。
 */
#include <string>
#include <iostream>
#include <vector>
#define DEBUG_TOKENIZE
#define DEBUG_PARSE
#define DEBUG_SENSE

#ifdef DEBUG_TOKENIZE
#include "tokenize.hpp"
#endif

#ifdef DEBUG_PARSE
#include "parse.hpp"
#endif

#ifdef DEBUG_SENSE
#include "sense.hpp"
#endif

int main(){

    #ifdef DEBUG_SENSE
        auto symbol_table = InitSymbolTable();
    #endif

    while(1){

        #ifdef DEBUG_TOKENIZE
        std::string input{};
        getline(std::cin,input);
        auto p = tokenize(input);
        std::vector<Token> res{};
        std::cout << "——tokenize——" << std::endl;
        if(std::holds_alternative<std::vector<Token>>(p)){
            res = std::get<std::vector<Token>>(p);
            for(auto& t : res){
                t.print_message();
            }
        }
        else{
            Error a = std::get<Error>(p);
            std::cout << a.error_message() << std::endl;
            continue;
        }
        #endif

        #ifdef DEBUG_PARSE
        auto q = parse_command(res);
        std::unique_ptr<Command> command;
        std::cout << "——parse——" << std::endl;
        if(std::holds_alternative<std::unique_ptr<Command>>(q)){
            command = move(std::get<std::unique_ptr<Command>>(q));
            (*command).print_message();
        }
        else{
            Error b = std::get<Error>(q);
            std::cout << b.error_message() << std::endl;
            continue;
        }
        #endif

        #ifdef DEBUG_SENSE
        std::cout << "——sense——" << std::endl;
        auto m = parse_meaning(command.get(),symbol_table.get());
        if(std::holds_alternative<Error>(m)){
            Error b = std::get<Error>(m);
            std::cout << b.error_message() << std::endl;
            continue;
        }
        else{
            std::cout << "语义检查通过" << std::endl;
        }
        #endif

    }

    return 0;
}