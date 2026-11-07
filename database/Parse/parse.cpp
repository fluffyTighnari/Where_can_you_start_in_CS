//parse.cpp
/*
 * 语法解析器实现：Token 序列 → AST。
 *
 * 1.1 版本支持的文法：
 *   create table table_name (col1 type1, col2 type2...);
 *       type 仅支持 int / integer / text / varchar / float，第 1 列为主键
 *   delete from table_name [where ...];
 *   insert into table_name (col1,col2,...) values(val1,val2,...);  要求含主键
 *   insert into table_name values(val1,val2,...);  要求的列与表定义一一对应
 *   select * from table_name [where ...];
 *   select col1,col2,... from table_name [where ...];
 *   update table_name set col = val [where ...];
 *   drop table table_name;
 *   show tables;
 * 谓词仅支持主键 = 单个等值（不支持嵌套谓词）。
 *
 * 待更新：alter 命令
 */
#include "parse.hpp"
#include "tokenize.hpp"
#include <vector>
#include <memory>
#include <iostream>

#ifdef DEBUG
    void ValueNode::print_value(){
        if(type == Sort::String){
            std::cout << str << ' ';
        }
        else if(type == Sort::Int){
            std::cout << Inum << ' ';
        }
        else if(type == Sort::Float){
            std::cout << Fnum << ' ';
        }
    }

    void Command::print_message(){
        std::visit([&](auto&& n){
        using T = std::decay_t<decltype(n)>;
        if constexpr(std::is_same_v<T,CreateNode>){
            auto& sel = std::get<CreateNode>(*Main_command.get());
            /*print: table_name,column_name,type*/
            std::cout << sel.table_name.table_name << std::endl;
            for(auto& p : sel.columns){
                std::cout << p.column_name << ' ' << p.type << std::endl;
            }
            std::cout << std::endl;
        }
        else if constexpr(std::is_same_v<T,DeleteNode>){
            auto& sel = std::get<DeleteNode>(*Main_command.get());
            /*print:table_name,expr*/
            std::cout << sel.table_name.table_name << std::endl;
            if(sel.given_expr){
                std::cout << sel.expr.leftexpr.column_name << ' ' << sel.expr.op << ' ';
                sel.expr.rightexpr.print_value();
                std::cout << std::endl;
            }
            else{
                std::cout << "delete all" << std::endl;
            }
        }
        else if constexpr(std::is_same_v<T,SelectNode>){
            auto& sel = std::get<SelectNode>(*Main_command.get());
            /*print:table_name,column,expr*/
            std::cout << sel.table_name.table_name << std::endl;
            if(sel.select_all){
                std::cout << "select all" << std::endl;
            }
            else{
                for(auto& p : sel.columns){
                    std::cout << p.column_name << ' ';
                }
            }
            std::cout << std::endl;
            if(sel.given_expr){
                std::cout << sel.expr.leftexpr.column_name << ' ' << sel.expr.op << ' ';
                sel.expr.rightexpr.print_value();
                std::cout << std::endl;
            }
            else{
                std::cout << "no expr" << std::endl;
            }
        }
        else if constexpr(std::is_same_v<T,InsertNode>){
            auto& sel = std::get<InsertNode>(*Main_command.get());
            /*print:table_name,column,value*/
            std::cout << sel.table_name.table_name << std::endl;
            if(sel.given_column){
                for(auto& p : sel.columns){
                    std::cout << p.column_name << ' ';
                }
                std::cout << std::endl;
            }
            for(auto& p : sel.values){
                p.print_value();
            }
            std::cout << std::endl;
        }
        else if constexpr(std::is_same_v<T,UpdateNode>){
            auto& sel = std::get<UpdateNode>(*Main_command.get());
            /*print:table_name,set,expr*/
            std::cout << sel.table_name.table_name << std::endl;
            std::cout << sel.set.leftexpr.column_name << ' ' << sel.set.op << ' ';
            sel.set.rightexpr.print_value();
            std::cout << std::endl;
            if(sel.given_expr){
                std::cout << sel.expr.leftexpr.column_name << ' ' << sel.expr.op << ' ';
                sel.expr.rightexpr.print_value();
                std::cout << std::endl;
            }
        }
        else{
            std::cout << "invalid command" << std::endl;
        }
        },*Main_command.get());
    }
#endif


/*
 * 入口：先做词法分析，再按首个关键字选择对应的语法规则。
 * 每个分支的流程都一样：构造对应的 Node → 调用解析函数 → 包成 ASTNode 挂到 Command 上。
 */
Result<std::unique_ptr<Command>> parse_command(std::string& input){
    auto tokens_result = tokenize(input);
    if(std::holds_alternative<Error>(tokens_result)){
        return std::get<Error>(tokens_result);
    }
    auto& tokens = std::get<std::vector<Token>>(tokens_result);
    if(tokens.empty()){
        return Error("empty command", Error::Layer::Parser);
    }

    std::string error{};
    auto p = std::make_unique<Command>();
    if(tokens[0].type == Token::Type::CREATE){
        CreateNode temp;
        if(!parse_create(tokens,temp,error)){
            return Error(error,Error::Layer::Parser);
        }
        auto ptr = std::make_unique<ASTNode>(std::move(temp));
        (*p).Main_command = std::move(ptr);
    }
    else if(tokens[0].type == Token::Type::SELECT){
        SelectNode temp;
        if(!parse_select(tokens,temp,error)){
            return Error(error,Error::Layer::Parser);
        }
        auto ptr = std::make_unique<ASTNode>(std::move(temp));
        (*p).Main_command = std::move(ptr);
    }
    else if(tokens[0].type == Token::Type::DELETE){
        DeleteNode temp;
        if(!parse_delete(tokens,temp,error)){
            return Error(error,Error::Layer::Parser);
        }
        auto ptr = std::make_unique<ASTNode>(std::move(temp));
        (*p).Main_command = std::move(ptr);
    }
    else if(tokens[0].type == Token::Type::INSERT){
        InsertNode temp;
        if(!parse_insert(tokens,temp,error)){
            return Error(error,Error::Layer::Parser);
        }
        auto ptr = std::make_unique<ASTNode>(std::move(temp));
        (*p).Main_command = std::move(ptr);
    }
    else if(tokens[0].type == Token::Type::UPDATE){
        UpdateNode temp;
        if(!parse_update(tokens,temp,error)){
            return Error(error,Error::Layer::Parser);
        }
        auto ptr = std::make_unique<ASTNode>(std::move(temp));
        (*p).Main_command = std::move(ptr);
    }
    else if(tokens[0].type == Token::Type::DROP){
        DropNode temp;
        if(!parse_drop(tokens,temp,error)){
            return Error(error,Error::Layer::Parser);
        }
        auto ptr = std::make_unique<ASTNode>(std::move(temp));
        (*p).Main_command = std::move(ptr);
    }
    else if(tokens[0].type == Token::Type::SHOW){
        ShowTablesNode temp;
        if(!parse_show_tables(tokens,temp,error)){
            return Error(error,Error::Layer::Parser);
        }
        auto ptr = std::make_unique<ASTNode>(std::move(temp));
        (*p).Main_command = std::move(ptr);
    }
    else{
        error = "invalid command";
        return Error(error,Error::Layer::Parser);
    }
    return p;
}



/*
 * SELECT * | col1,col2 FROM table [WHERE expr];
 * 先判定通配符，再解析列列表，最后静态处理 where 与结尾分号。
 */
static bool parse_select(std::vector<Token>& tokens,SelectNode& sel,std::string& error){
    int size = tokens.size();
    int cur = 1;   /* 跳过首部的 SELECT */
    if(cur < size && tokens[cur].type == Token::Type::IDENT && tokens[cur].str == "*"){
        sel.select_all = true;
    }
    else{
        sel.select_all = false;
    }

    if(!parse_ColumnRef(tokens,sel.columns,cur,error)){
        return false;
    }

    /* 通配符也会被 parse_ColumnRef 收进 columns，这里清掉以保持 select_all 语义单一 */
    if(sel.columns.size() == 1 && sel.select_all){
        sel.columns.clear();
    }

    if(cur >= size || !match(tokens[cur],Token::Type::FROM)){
        error = "expected from";
        return false;
    }
    eat(tokens,cur);
    if(!parse_from(tokens,sel.table_name,cur,error)){
        return false;
    }

    if(cur < size && match(tokens[cur],Token::Type::WHERE)){
        eat(tokens,cur);
        sel.given_expr = true;
        if(!parse_where(tokens,sel.expr,cur,error)){
            return false;
        }
    }
    else if(cur < size && match(tokens[cur],Token::Type::END)){
        return true;
    }
    else if(cur >= size){
        error = "expected ;";
        return false;
    }
    else{
        error = "expected where";
        return false;
    }

    if(cur < size && match(tokens[cur],Token::Type::END)){
        return true;
    }
    else{
        error = "expected ;";
        return false;
    }
    
    return true;
}

/* CREATE TABLE name (col type, col type...); —— 第一个列定义即主键 */
static bool parse_create(std::vector<Token>& tokens,CreateNode& cre,std::string& error){
    int size = tokens.size();
    int cur = 1;
    if(cur >= size || !match(tokens[cur],Token::Type::TABLE)){
        error = "expected table";
        return false;
    }
    eat(tokens,cur);
    if(!parse_from(tokens,cre.table_name,cur,error)){
        return false;
    }

    if(cur < size && match(tokens[cur],Token::Type::LPAREN)){
        eat(tokens,cur);
        if(!parse_ColumnDef(tokens,cre.columns,cur,error)){
            return false;
        }
    }
    else{
        error = "expected (";
        return false;
    }

    if(cur >= size || !match(tokens[cur],Token::Type::RPAREN)){
        error = "expected )";
        return false;
    }
    eat(tokens,cur);
    if(cur < size && match(tokens[cur],Token::Type::END)){
        return true;
    }
    else{
        error = "expected ;";
        return false;
    }

    return true;
}

/* DELETE FROM name [WHERE expr]; */
static bool parse_delete(std::vector<Token>& tokens,DeleteNode& del,std::string& error){
    int size = tokens.size();
    int cur = 1;
    if(cur >= size || !match(tokens[cur],Token::Type::FROM)){
        error = "expected from";
        return false;
    }
    eat(tokens,cur);
    if(!parse_from(tokens,del.table_name,cur,error)){
        return false;
    }

    if(cur < size && match(tokens[cur],Token::Type::WHERE)){
        del.given_expr = true;
        eat(tokens,cur);
        if(!parse_where(tokens,del.expr,cur,error)){
            return false;
        }
    }
    else if(cur < size && match(tokens[cur],Token::Type::END)){
        return true;
    }
    else if(cur >= size){
        error = "expected ;";
        return false;
    }
    else{
        error = "expected where";
        return false;
    }

    return true;
}

/*
 * INSERT INTO name [(col1,col2,...)] VALUES(val1,val2,...);
 * 带列名与不带列名两条分支，解析出的都是同一组 values，
 * 区别在于 given_column 标记，决定序列化时按名字对齐还是按位置对齐。
 */
static bool parse_insert(std::vector<Token>& tokens,InsertNode& ins,std::string& error){
    int size = tokens.size();
    int cur = 1;
    if(cur >= size || !match(tokens[cur],Token::Type::INTO)){
        error = "expected into";
        return false;
    }
    eat(tokens,cur);
    if(!parse_from(tokens,ins.table_name,cur,error)){
        return false;
    }

    if(cur < size && match(tokens[cur],Token::Type::VALUES)){
        eat(tokens,cur);
        if(cur >= size || !match(tokens[cur],Token::Type::LPAREN)){
            error = "expected (";
            return false;
        }
        eat(tokens,cur);
        if(!parse_value(tokens,ins.values,cur,error)){
            return false;
        }
        if(cur >= size || !match(tokens[cur],Token::Type::RPAREN)){
            error = "expected )";
            return false;
        }
        eat(tokens,cur);
    }
    else{
        ins.given_column = true;
        if(cur < size){
            if(!match(tokens[cur],Token::Type::LPAREN)){
                error = "expected (";
                return false;
            }
            eat(tokens,cur);
            if(!parse_ColumnRef(tokens,ins.columns,cur,error)){
                return false;
            }
            if(cur >= size || !match(tokens[cur],Token::Type::RPAREN)){
                error = "expected )";
                return false;
            }
            eat(tokens,cur);
        }

        if(cur >= size || !match(tokens[cur],Token::Type::VALUES)){
            error = "expected VALUES";
            return false;
        }
        eat(tokens,cur);
        if(cur >= size || !match(tokens[cur],Token::Type::LPAREN)){
            error = "expected (";
            return false;
        }
        eat(tokens,cur);
        if(!parse_value(tokens,ins.values,cur,error)){
            return false;
        }
        if(cur >= size || !match(tokens[cur],Token::Type::RPAREN)){
            error = "expected )";
            return false;
        }
        eat(tokens,cur);
    }

    if(cur < size && match(tokens[cur],Token::Type::END)){
        return true;
    }
    else{
        error = "expected ;";
        return false;
    }

    return true;
}

/*
 * UPDATE name SET col = val [WHERE expr];
 * set 与 expr 都用 parse_where 解析——它们的语法形状相同，只是语义不同。
 */
static bool parse_update(std::vector<Token>& tokens,UpdateNode& upd,std::string& error){
    int size = tokens.size();
    int cur = 1;
    if(cur >= size){
        error = "expected table_name";
        return false;
    }
    if(!parse_from(tokens,upd.table_name,cur,error)){
        return false;
    }

    if(cur >= size || !match(tokens[cur],Token::Type::SET)){
        error = "expected set";
        return false;
    }
    eat(tokens,cur);
    if(!parse_where(tokens,upd.set,cur,error)){
        return false;
    }

    if(cur < size && match(tokens[cur],Token::Type::WHERE)){
        upd.given_expr = true;
        eat(tokens,cur);
        if(!parse_where(tokens,upd.expr,cur,error)){
            return false;
        }
    }
    else if(cur < size && match(tokens[cur],Token::Type::END)){
        return true;
    }
    else if(cur >= size){
        error = "expected ;";
        return false;
    }
    else{
        error = "expected where";
        return false;
    }

    return true;
}

/* DROP TABLE name; —— 表名直接取 IDENT 的文本，不走 parse_from */
static bool parse_drop(std::vector<Token>& tokens,DropNode& drop,std::string& error){
    int size = tokens.size();
    int cur = 1;
    if(cur >= size || !match(tokens[cur],Token::Type::TABLE)){
        error = "expected TABLE after DROP";
        return false;
    }
    eat(tokens,cur);
    if(cur >= size || !match(tokens[cur],Token::Type::IDENT)){
        error = "expected table_name";
        return false;
    }
    drop.table_name.table_name = tokens[cur].str;
    eat(tokens,cur);

    if(cur < size && match(tokens[cur],Token::Type::END)){
        return true;
    }
    else if(cur >= size){
        error = "expected ;";
        return false;
    }
    else{
        error = "unexpected token after table name";
        return false;
    }
    return true;
}

/* SHOW TABLES; —— TABLES 在词法阶段是 IDENT（未列为关键字），这里按文本比对 */
static bool parse_show_tables(std::vector<Token>& tokens,ShowTablesNode& show,std::string& error){
    int size = tokens.size();
    int cur = 1;
    if(cur >= size || !match(tokens[cur],Token::Type::IDENT) || tokens[cur].str != "TABLES"){
        error = "expected TABLES after SHOW";
        return false;
    }
    eat(tokens,cur);
    if(cur >= size || !match(tokens[cur],Token::Type::END)){
        error = "expected ;";
        return false;
    }
    return true;
}

/*tool func*/

/* 列名列表：col1, col2, ...；遇到 "*" 直接结束循环表示全列 */
static bool parse_ColumnRef(std::vector<Token>& tokens,std::vector<ColumnRef>& input,int& cur,std::string& error){
    int size = tokens.size();
    if(cur >= size){
        error = "expected * or column_name";
        return false;
    }
    while(cur < size){
        if(!match(tokens[cur],Token::Type::IDENT)){
            error = "expected * or column_name";
            return false;
        }

        if(tokens[cur].str == "*"){
            eat(tokens,cur);
            break;
        }
        else{
            ColumnRef temp;
            temp.column_name = eat(tokens,cur).str;
            input.push_back(temp);
        }

        if(cur < size && match(tokens[cur],Token::Type::COMMA)){
            eat(tokens,cur);

            if(cur >= size){
                error = "expected a column_name after a comma";
                return false;
            }
        }
        else{
            break;
        }
    }
    return true;
}

/* 列定义列表：每项是 "列名 类型" 两个连续 IDENT，最后一项后面不能有逗号 */
static bool parse_ColumnDef(std::vector<Token>& tokens,std::vector<ColumnDef>& input,int& cur,std::string& error){
    int size = tokens.size();
    if(cur >= size){
        error = "invalid format";
        return false;
    }
    while(cur < size){
        if(cur >= size - 1 || !match(tokens[cur],Token::Type::IDENT) || !match(tokens[cur + 1],Token::Type::IDENT)){
            error = "invalid format";
            return false;
        }
        ColumnDef temp;
        temp.column_name = eat(tokens,cur).str;
        temp.type = eat(tokens,cur).str;
        input.push_back(temp);

        if(cur < size && match(tokens[cur],Token::Type::COMMA)){
            eat(tokens,cur);

            if(cur >= size){
                error = "expected a column_name after a comma";
                return false;
            }
        }
        else{
            break;
        }
    }
    return true;
}

/* 表名：一个 IDENT */
static bool parse_from(std::vector<Token>& tokens,TableNode& input,int& cur,std::string& error){
    if(cur >= tokens.size() || !match(tokens[cur],Token::Type::IDENT)){
        error = "expected table_name";
        return false;
    }
    TableNode temp;
    temp.table_name = eat(tokens,cur).str;
    input = temp;
    return true;
}

/* 谓词：目前只支持 ColumnRef op ValueNode 的三元形式，op 可以是 = / < / > */
static bool parse_where(std::vector<Token>& tokens,PredicateNode& input,int& cur,std::string& error){
    /*先仅支持A = B类型：ColumnRef = ValueNode*/
    int size = tokens.size();
    if(cur > size - 3){
        error = "expected an expression";
        return false;
    }
    PredicateNode temp;
    if(!match(tokens[cur],Token::Type::IDENT)){
        error = "invalid leftexpr";
        return false;
    }
    ColumnRef col;
    col.column_name = eat(tokens,cur).str;
    input.leftexpr = col;
    if(!match(tokens[cur],Token::Type::OP)){
        error = "invalid operation";
        return false;
    }
    input.op = eat(tokens,cur).str[0];
    if(match(tokens[cur],Token::Type::STRING)){
        input.rightexpr = ValueNode(eat(tokens,cur).str);
    }
    else if(match(tokens[cur],Token::Type::INT)){
        input.rightexpr = ValueNode(eat(tokens,cur).Inum);
    }
    else if(match(tokens[cur],Token::Type::FLOAT)){
        input.rightexpr = ValueNode(eat(tokens,cur).Fnum);
    }
    else{
        error = "invalid rightexpr";
        return false;
    }

    return true;
}

/* 值列表：val1, val2, ...；NULL 关键字映射为 Sort::null 的 ValueNode */
static bool parse_value(std::vector<Token>& tokens,std::vector<ValueNode>& input,int& cur,std::string& error){
    int size = tokens.size();
    if(cur >= size){
        error = "expected value";
        return false;
    }
    while(cur < size){
        if(match(tokens[cur],Token::Type::STRING)){
            input.emplace_back(eat(tokens,cur).str);
        }
        else if(match(tokens[cur],Token::Type::INT)){
            input.emplace_back(eat(tokens,cur).Inum);
        }
        else if(match(tokens[cur],Token::Type::FLOAT)){
            input.emplace_back(eat(tokens,cur).Fnum);
        }
        else if(match(tokens[cur],Token::Type::IDENT) && tokens[cur].str == "NULL"){
            eat(tokens,cur);
            input.emplace_back();
        }
        else{
            error = "invalid value_type";
            return false;
        }

        if(cur < size && match(tokens[cur],Token::Type::COMMA)){
            eat(tokens,cur);

            if(cur >= size){
                error = "expected a value after a comma";
                return false;
            }
        }
        else{
            break;
        }
    }
    return true;
}

/* 消费当前 token 并把游标后移一位 */
static Token eat(std::vector<Token>& tokens, int& cur){
    return tokens[cur++];
}

/* 判断 token 是否是指定类型 */
static bool match(Token& t, Token::Type type){
    return t.type == type;
}