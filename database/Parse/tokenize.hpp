//tokenize.hpp
#pragma once

#include "Help/Error/error.hpp"
#include <vector>
#include <string>
#include <unordered_map>

/*
 * 词法分析：利用手动状态机构建词法解析器。
 *
 * 输入是一条 SQL 字符串，输出是 Token 序列。识别以下几类：
 *   关键字（INSERT/SELECT/...，大小写不敏感，统一转大写）
 *   标识符 IDENT（表名、列名，以及 "*"）
 *   字面量 INT / FLOAT / STRING
 *   符号 LPAREN / RPAREN / COMMA / OP / END(;)
 *
 * 单个 Token 同时携带字符串、整型、浮点三种表示，由 type 决定哪种有效。
 * 这种"冗余存储"让下游解析器不必再关心数值转换。
 */

typedef struct Token{
    enum class Type{
        INSERT,INTO,VALUES,WHERE,SELECT,DELETE,CREATE,UPDATE,DROP,SHOW,TABLE,FROM,SET,
        IDENT,/*column_name、table_name*/
        INT,
        FLOAT,
        STRING,
        LPAREN,
        RPAREN,
        COMMA,
        OP,
        END
    };
    Type type;
    std::string str;   /* 关键字/标识符/字符串字面量的文本 */
    int Inum;          /* INT 字面量的值 */
    float Fnum;        /* FLOAT 字面量的值 */

    Token(Type a,const std::string& b,int c,float d): type(a),str(b),Inum(c),Fnum(d){}

    #ifdef DEBUG
    void print_message();
    #endif

}Token;

Result<std::vector<Token>> tokenize(std::string& input);

#ifdef DEBUG
/* 调试用：Token 类型到名字的映射 */
const std::unordered_map<Token::Type, std::string> type_to_string = {
    {Token::Type::INSERT,   "INSERT"},
    {Token::Type::INTO,     "INTO"},
    {Token::Type::VALUES,   "VALUES"},
    {Token::Type::WHERE,    "WHERE"},
    {Token::Type::SELECT,   "SELECT"},
    {Token::Type::DELETE,   "DELETE"},
    {Token::Type::CREATE,   "CREATE"},
    {Token::Type::UPDATE,   "UPDATE"},
    {Token::Type::DROP,     "DROP"},
    {Token::Type::SHOW,     "SHOW"},
    {Token::Type::TABLE,    "TABLE"},
    {Token::Type::FROM,     "FROM"},
    {Token::Type::SET,      "SET"},
    {Token::Type::IDENT,    "IDENT"},
    {Token::Type::INT,      "INT"},
    {Token::Type::FLOAT,    "FLOAT"},
    {Token::Type::STRING,   "STRING"},
    {Token::Type::LPAREN,   "LPAREN"},
    {Token::Type::RPAREN,   "RPAREN"},
    {Token::Type::COMMA,    "COMMA"},
    {Token::Type::OP,       "OP"},
    {Token::Type::END,      "END"}
};
#endif
