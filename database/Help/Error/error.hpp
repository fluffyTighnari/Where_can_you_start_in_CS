//error.hpp
#pragma once
#include <variant>
#include <string>

/*
 * 统一的错误对象与结果类型。
 *
 * Result<T> = std::variant<T, Error>：
 *   函数要么返回正常值 T，要么返回 Error，调用方用 std::holds_alternative 判定。
 * 这样避免了返回裸指针 + 全局错误码，也让每一层都能原样向上传递错误。
 *
 * Layer 记录错误发生在哪一层，便于定位问题：
 *   Parser   词法/语法/语义检查
 *   Executor 命令执行调度
 *   Pager    页缓存与磁盘 IO
 *   Btree    B+ 树结构操作
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

    std::string error_message();
};

template<typename T>
using Result = std::variant<T,Error>;
