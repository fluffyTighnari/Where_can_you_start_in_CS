//parse.hpp
#pragma once

#include "Help/Error/error.hpp"
#include "tokenize.hpp"
#include <variant>
#include <vector>
#include <string>
#include <memory>

/*
 * 语法分析：把 Token 序列组装成 AST（抽象语法树）。
 *
 * 手工递归下降 + std::variant 表示节点：
 *   每个 Node 结构体对应一种 SQL 命令的形状，
 *   ASTNode 是它们的 variant，Command 用 unique_ptr 持有根节点。
 * 下游用 std::visit 按节点类型分派，无需虚函数。
 *
 * 这一层只管"语法是否成立"，不检查表和列是否存在——那属于语义层 sense.cpp。
 * 待更新：彻底解耦语法和语义，在语义分析层进行语义绑定（Identifier/Literal）。
 */

struct TableNode {
    std::string table_name;
};

struct ColumnRef {
    std::string column_name;
};

struct ColumnDef {
    std::string column_name;
    std::string type;
};

/* 字面量节点：三种标量类型 + NULL */
struct ValueNode {
    enum class Sort {
        String,
        Int,
        Float,
        null
    };

    Sort type;
    int Inum;
    float Fnum;
    std::string str;

    ValueNode(int a) : Inum(a), type(Sort::Int) {}
    ValueNode(float a) : Fnum(a), type(Sort::Float) {}
    ValueNode() : type(Sort::null) {}
    ValueNode(const std::string& a) : str(a), type(Sort::String) {}

    #ifdef DEBUG
    void print_value();
    #endif

};

/*
using ExprNode = std::variant<
    TableNode,
    ColumnRef,
    PredicateNode,
    ValueNode
>;
*/

/* 谓词：目前只支持形如 column = value 的单条件 */
struct PredicateNode {
    char op;
    ColumnRef leftexpr;
    ValueNode rightexpr;
};

struct CreateNode {
    TableNode table_name;
    std::vector<ColumnDef> columns;
};

struct SelectNode {
    TableNode table_name;
    bool select_all = true;
    bool given_expr = false;
    std::vector<ColumnRef> columns;
    PredicateNode expr;
};

struct DeleteNode {
    TableNode table_name;
    bool given_expr = false;
    PredicateNode expr;
};

/* update 有两处谓词：set 描述"改成什么"，expr 描述"改哪些行" */
struct UpdateNode {
    TableNode table_name;
    PredicateNode set;
    bool given_expr = false;
    PredicateNode expr;
};

struct InsertNode {
    TableNode table_name;
    bool given_column = false;
    std::vector<ColumnRef> columns;
    std::vector<ValueNode> values;
};

struct DropNode {
    TableNode table_name;
};

struct ShowTablesNode {
};

/* 所有节点类型的集合：variant 让 AST 可以统一持有与分派 */
using ASTNode = std::variant<
    CreateNode,
    SelectNode,
    DeleteNode,
    UpdateNode,
    InsertNode,
    DropNode,
    ShowTablesNode,
    ColumnDef,
    ColumnRef,
    TableNode,
    PredicateNode,
    ValueNode
>;

struct Command {
    std::unique_ptr<ASTNode> Main_command;

    #ifdef DEBUG
    void print_message();
    #endif
};


Result<std::unique_ptr<Command>> parse_command(std::string& input);

/* 各命令的语法规则，每个函数消费 tokens 中从 cur 开始的部分 */
static bool parse_select(std::vector<Token>& tokens,SelectNode& sel,std::string& error);

static bool parse_create(std::vector<Token>& tokens,CreateNode& cre,std::string& error);

static bool parse_delete(std::vector<Token>& tokens,DeleteNode& del,std::string& error);

static bool parse_insert(std::vector<Token>& tokens,InsertNode& ins,std::string& error);

static bool parse_update(std::vector<Token>& tokens,UpdateNode& upd,std::string& error);

static bool parse_drop(std::vector<Token>& tokens,DropNode& drop,std::string& error);

static bool parse_show_tables(std::vector<Token>& tokens,ShowTablesNode& show,std::string& error);

/* 子规则：列名列表 / 列定义列表 / 表名 / 谓词 / 值列表 */
static bool parse_ColumnRef(std::vector<Token>& tokens,std::vector<ColumnRef>& input,int& cur,std::string& error);

static bool parse_ColumnDef(std::vector<Token>& tokens,std::vector<ColumnDef>& input,int& cur,std::string& error);

static bool parse_from(std::vector<Token>& tokens,TableNode& input,int& cur,std::string& error);

static bool parse_where(std::vector<Token>& tokens,PredicateNode& input,int& cur,std::string& error);

static bool parse_value(std::vector<Token>& tokens,std::vector<ValueNode>& input,int& cur,std::string& error);

/* 工具函数：eat 消费当前 token 并前进；match 判断类型 */
static Token eat(std::vector<Token>& tokens, int& cur);

static bool match(Token& t, Token::Type type);
