#pragma once
#include <variant>
#include <string>

/*
 * B+ 树模块的独立错误定义。
 *
 * 与 Help/Error/error.hpp 内容相同，但这里放了一份副本：
 * B+ 树的单元测试可以只依赖本目录下的文件（配合 mockpager），
 * 不必把整个 database 源码树加进编译范围。
 * 生产代码统一使用 Help/Error/error.hpp，两者不要同时 include。
 */

struct Error{

    enum class Layer{//报错层号
        Parser,
        Executor,
        Pager,
        Btree
    };

    Layer layer;
    std::string message;
    Error(const std::string& input,const Layer& where){
        message = input;
        layer = where;
    }
    
    std::string error_message(){
        std::string where_error{};
        switch(layer){
            case Layer::Parser:
            where_error = "解析层";
            break;

            case Layer::Executor:
            where_error = "执行层";
            break;

            case Layer::Pager:
            where_error = "存储层";
            break;

            case Layer::Btree:
            where_error = "B+树层";
        }
        return std::string(where_error + "发生错误：" + message);
    }
};

template<typename T>
using Result = std::variant<T,Error>;