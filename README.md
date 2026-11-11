# 深入探究底层原理 — 计算机基础造轮子项目集

从零实现计算机系统的几个核心组件，在造轮子的过程中理解系统软件的真实工作方式。
所有项目都用 C / C++ 编写，不依赖第三方库，只使用 POSIX / 标准库接口。

| 项目 | 语言 | 核心主题 | 规模 |
|------|------|----------|------|
| [Allocator](#allocator--内存分配器) | C | 堆管理 / 空闲链表 / 内存对齐 | ~530 行 |
| [Shell](#shell--命令行解释器) | C | 进程 / 信号 / 管道 / 终端 | ~550 行 |
| [documents system + web](#documents-system--web--文件系统与-web-服务器) | C | 文件系统 / HTTP / 并发 / 网络 | ~2700 行 |
| [database](#database--关系型数据库) | C++ | SQL 解析 / B+ 树 / 页缓存 | ~5000 行实现 + ~2000 行测试 |

各项目的内部架构、模块职责与关键流程，见对应目录下的 `README.md`。

---

## Allocator · 内存分配器

**位置：** `Allocator/`

### 功能

一个用户态内存分配器，直接用 `sbrk()` 向操作系统扩展堆，用双向空闲链表管理空闲块。
对外提供与 C 标准库语义一致的三个接口：

| 接口 | 行为 |
|------|------|
| `my_malloc(size)` | first-fit 遍历空闲链表；块过大则分裂，尾部残余归还；无合适块则向系统申请新段 |
| `my_free(ptr)` | 校验指针合法性后标记空闲，再向前、向后双向合并相邻空闲块 |
| `my_realloc(ptr, size)` | 原地收缩 / 借用后继空闲块原地扩容 / 申请新块 + 拷贝 + 释放旧块 |

另外提供两个调试接口：`allocator_get_stats()` 采集堆统计，`allocator_validate()` 遍历全堆校验全部不变量。

### 构建

```bash
cd Allocator
gcc -std=c11 -O2 -c allocator.c      # 生成 allocator.o
```

生成的是库文件，由使用方链接。`allocator.h` 是唯一需要 `#include` 的头文件；
`Memory Allocator.h` 是旧入口，仅保留包含路径兼容，新代码不要用。

依赖 POSIX 的 `sbrk()`，需在 Linux / WSL / macOS 下构建（原生 Windows MinGW 没有 `sbrk`）。

### 能学到的知识

- **堆扩展机制**：`sbrk()` 与 program break 的工作方式
- **空闲链表**：双向链表的插入/摘除，first-fit 分配策略
- **块分裂与合并**：分配时分裂减少内部碎片，释放时双向合并减少外部碎片
- **块头设计**：在用户指针前隐藏元数据（大小、前块大小、空闲标志、链表指针），用户完全无感知
- **内存对齐**：位运算向上取整到 16 字节，理解对齐如何减少内部碎片
- **与 libc 共存的现实约束**：`brk` 是进程级资源，可能被 libc 的 `malloc`/`printf` 一并推进，
  因此堆按"段（arena）"记录，只在段内合并，不与他人互相踩踏
- **健壮性设计**：魔术字检测野指针与越界写，拒绝未对齐指针与重复释放，全堆不变量校验

---

## Shell · 命令行解释器

**位置：** `Shell/`

### 功能

一个可交互的迷你 Shell，支持：

| 能力 | 说明 |
|------|------|
| 内建命令 | `cd`（含 `cd -` 切换上一目录，维护 `OLDPWD`）、`exit` |
| 外部命令 | `fork` + `execvp` 执行任意可执行文件 |
| 单管道 | `cmd1 \| cmd2`，`pipe` + `dup2` 重定向标准输入输出 |
| 后台运行 | 命令末尾加 `&`，任务计入作业表，退出时统一终止 |
| 原始终端输入 | `termios` 关闭行缓冲与回显，自行处理逐字符读取与退格 |

### 构建与运行

```bash
cd Shell
gcc -std=c11 -O2 myshell.c -o myshell
./myshell
```

### 能学到的知识

- **进程模型**：`fork` / `execvp` / `waitpid` 的经典组合，父子进程关系
- **Unix 信号**：`sigaction` 注册处理函数，`SIGCHLD` 回收僵尸进程，
  后台模式 fork 前阻塞 `SIGCHLD` 以消除记录 PID 前的竞态
- **管道与 I/O 重定向**：`pipe()` + `dup2()` 实现进程间通信，理解文件描述符的本质
- **终端编程**：`termios` 原始模式，手动回显与退格擦除
- **作业管理**：`&` 语法解析、后台 PID 表维护、定期清理与退出时 `SIGHUP`
- **交互式命令行的解析**：命令拆分、管道识别、后台标记

---

## documents system + web · 文件系统与 Web 服务器

**位置：** `documents system + web/`

### 功能

两个模块组合而成：底层是一个类 ext2 的简易文件系统（运行在磁盘镜像文件上），
上层是一个支持静态资源、CGI、线程池与 epoll 的 HTTP 服务器，并带 Cookie/Session 登录。

**文件系统**

| 能力 | 说明 |
|------|------|
| 磁盘布局 | 1MB 镜像 = 2048 块 × 512B：超级块 / inode 位图 / 块位图 / inode 表 / 数据区 |
| 目录操作 | 创建目录、列出目录内容、列出根目录成员 |
| 文件操作 | 创建、删除、按偏移读写（支持跨块）、打印 |
| 多级索引 | 6 个直接指针 + 一级间接 + 二级间接，支持较大文件 |
| 并发保护 | 整卷一把互斥锁，所有接口可被多个工作线程调用 |

**Web 服务器**

| 能力 | 说明 |
|------|------|
| I/O 多路复用 | `epoll` 单线程管理大量连接 |
| 线程池 | 4 个工作线程 + 有界任务队列（上限 128），队列满则拒绝连接 |
| 静态资源 | 按扩展名给出 `Content-Type`，chunked 传输 |
| RESTful 文件操作 | `GET`/`POST`/`PUT`/`DELETE` 映射到虚拟文件系统 |
| 登录会话 | Cookie + Session，token 取自 `/dev/urandom`，24 小时过期 |
| CGI | `fork` + `exec` 调外部程序，管道对接标准输入输出，含目录穿越防护 |
| 优雅退出 | `SIGINT` 置标志位由主循环处理，`SIGPIPE` 忽略 |

### 构建与运行

```bash
cd "documents system + web"
gcc -std=gnu11 -O2 -pthread web2.c achieve.c interface.c help.c -o webserver
gcc -std=gnu11 -O2 exec/test.c -o exec/test        # CGI 示例程序（可选）
./webserver                                        # 监听 8080，首页取自 ./www/index.html
```

运行前需要存在 `disk.img` 镜像。镜像由 `fs_create()` 负责格式化，仓库中未提供命令行入口，
首次部署需先调用一次该函数；未挂载成功时服务器会直接退出。

### 能学到的知识

**文件系统方向**

- **磁盘布局设计**：超级块 → inode 位图 → 块位图 → inode 表 → 数据区的经典 ext2 布局
- **inode 原理**：为什么需要 inode，多级索引指针如何支撑大文件
- **位图管理**：用 1 bit / 1 个对象 管理空闲 inode 与空闲块
- **目录的本质**：目录是特殊文件，内容是 `(inode 号, 文件名)` 数组
- **虚拟磁盘**：用普通文件模拟磁盘，块读写是最底层的抽象
- **元数据一致性**：哪些改动必须立即落盘，卸载时如何统一同步

**Web / 网络方向**

- **HTTP 协议**：请求行/头部/正文解析，状态码与 `Content-Type`，chunked 编码
- **Socket 编程**：`socket` / `bind` / `listen` / `accept` 全流程，TCP 选项的作用
- **I/O 多路复用**：`epoll` 的 Reactor 模型，单线程管理多连接
- **线程池**：任务队列 + 互斥锁 + 条件变量的生产者-消费者模型
- **CGI 协议**：环境变量传参、管道对接标准流、路径逃逸的三重防护
- **会话管理**：Cookie / Session 机制，token 生成与过期策略
- **信号处理**：异步信号安全（处理函数里只置标志位）、`SIGPIPE` 的必要性

---

## database · 关系型数据库

**位置：** `database/`

### 功能

一个分层实现的关系型数据库，覆盖 SQL 解析、执行调度、存储引擎、B+ 树索引、页缓存。

| SQL | 支持 |
|-----|------|
| `create table` | `create table t (col1 type1, col2 type2...);`，类型支持 int / float / text / varchar，**第 1 列为主键** |
| `insert` | `insert into t values(...)` 按位置对应；`insert into t (c1,c2) values(...)` 按列名对应 |
| `select` | `select * from t [where ...]`、`select c1,c2 from t [where ...]` |
| `update` | `update t set col = val where ...`（必须带 where） |
| `delete` | `delete from t [where ...]`，无 where 时清空全表 |
| `drop table` / `show tables` | 删除表数据与元信息 / 列出所有表 |

谓词仅支持**主键单个等值条件**（`where pk = value`）。

### 构建与运行

```bash
cd database
make                  # 构建 database.exe（依赖 g++，C++17）
./database.exe        # 数据库文件为当前目录下的 sql.db

make test_btree       # B+ 树单元测试（内存页管理器，不触碰磁盘）
make test_integration # 端到端集成测试（SQL → 结果集）
make check            # 全部测试
```

### 能学到的知识

- **SQL 解析三段式**：词法分析（`tokenize`）→ 语法分析（`parse`，手写递归下降 + `std::variant` 表示 AST）→ 语义检查（`sense`，符号表校验表/列/类型）
- **分层架构与边界**：Parse / Executor / Storage / Pager / B+Tree 各层职责与依赖方向
- **B+ 树实现**：节点分裂与合并、借位、`StackPath` 回溯式级联处理、叶子链表与顺序扫描
- **存储格式设计**：`[RowHeader][int][size][string]` 的行式布局，按偏移数组定位列，紧凑存储与跳读
- **页缓存与 LRU**：哈希表 O(1) 定位 + 双向链表维护淘汰顺序，脏页延迟写回
- **元数据自举**：系统表本身也是一棵 B+ 树，重启时扫描它恢复所有表定义
- **C++ 工程实践**：`std::variant` 做结果/错误双返回、`unique_ptr` 表达所有权、`std::visit` 按类型分派、`static_assert` 把布局约束提前到编译期

---

## 学习路径

各项目按依赖关系由浅入深：

```
Allocator   内存与指针、链表与对齐
    ↓
Shell       进程、信号、文件描述符
    ↓
documents   文件系统布局 + 网络并发
    ↓
database    综合运用：数据结构 + 分层设计 + 持久化
```

每个项目建议的阅读顺序：先读头文件了解接口与数据结构，再读实现的文件头注释理解整体机制，
最后顺着主流程函数深入细节。
