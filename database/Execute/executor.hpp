#pragma once

/*
 * executor.hpp —— 执行器对外接口
 *
 * 输入是抽象语法树 Command，输出是查询结果 Info。
 * 执行器是 Parse 层与 Storage 层之间的调度者：从符号表取元信息，
 * 调用 B+ 树接口完成读写，再把本次改动统一落盘。
 */

#include "Parse/parse.hpp"
#include "Help/Error/error.hpp"
#include "Help/Show/show.hpp"
#include "Storage_engine/storage.hpp"
#include <string>
#include <vector>
#include <memory>

Result<std::unique_ptr<Info>> execute_command(Command* command, Storage* storage);
