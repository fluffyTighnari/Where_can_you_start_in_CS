# documents system + web 内部架构

文件系统与 Web 服务器的实现说明。对外功能与构建方式见仓库根目录 `README.md`。

## 分层

```
┌──────────────────────────────────────────────────┐
│ web2.c        epoll 事件循环 + 线程池 + 优雅退出  │
└───────────────────────┬──────────────────────────┘
                        │ 连接交给工作线程
┌───────────────────────▼──────────────────────────┐
│ achieve.c     HTTP 解析 / 路由 / 会话 / CGI       │
│               文件操作 → 调用 interface.h         │
└───────────────────────┬──────────────────────────┘
                        │ fs_* 接口（内部加锁）
┌───────────────────────▼──────────────────────────┐
│ interface.c   目录与文件操作、锁、参数校验        │
└───────────────────────┬──────────────────────────┘
                        │ 位图 / inode / 块操作
┌───────────────────────▼──────────────────────────┐
│ help.c        超级块、inode、位图、多级索引       │
└───────────────────────┬──────────────────────────┘
                        ▼
                 磁盘镜像 disk.img
```

依赖方向严格单向：上层只调用下层，下层不知道上层存在。

| 文件 | 行数 | 职责 |
|------|------|------|
| `help.h` / `help.c` | ~430 | 磁盘布局定义、位图操作、inode 分配释放、块读写、多级索引解析 |
| `interface.h` / `interface.c` | ~950 | 文件系统对外 API：目录与文件操作，全部加锁 |
| `achieve.h` / `achieve.c` | ~950 | HTTP 请求解析与路由、会话管理、CGI、响应发送 |
| `web2.c` | ~300 | 服务器主循环：监听、epoll、线程池、信号与关闭 |
| `exec/test.c` | ~45 | CGI 示例程序，回显服务器传入的环境变量 |

## 磁盘布局

1MB 镜像 = 2048 块 × 512B：

```
 block 0        超级块 Super_block
 block 1        inode 位图          64 bit（实际只用 max_files = 64 个）
 block 2        数据块位图
 block 3 ~ 10   inode 表            8 块 × 8 个/块 = 64 个 inode
 block 11 ~    数据区               文件内容与目录内容
```

`FScontext` 是挂载后的内存视图，缓存超级块、两张位图与整张 inode 表：

```
FScontext
  ├── disk_fp
  ├── super_block               block 0 副本
  ├── inodemap                  block 1 副本
  ├── bitmap                    block 2 副本
  ├── inodetable                block 3+ 副本
  └── begin_block_of_*          各区域起始块号
```

挂载时一次性全部读入内存，卸载时一次性全部写回；运行期的修改采取"改内存 + 立即写对应块"的策略。

### inode 与多级索引

```
inode
  ├── type / size / block / create_time / modify_time
  ├── pointer[6]         → 前 6 个数据块
  ├── in_pointer         → 一级间接块（块内 128 个 uint32，各指一个数据块）
  └── double_pointer     → 二级间接块（其每一项再指向一个一级间接块）
```

`fs_get_file_block(inode, n)` / `fs_set_file_block(inode, n, block)` 把逻辑块号 `n`
（1 基）翻译成物理块号，内部按 `n <= 6` / `n < 6 + 128` 分段处理。
实现中的容量上限是 `6 + 128 = 134` 块，二级间接指针在释放路径上会遍历，但分配路径未使用。

### 目录

目录是特殊文件，内容是 `Director_entry` 数组，每块 16 项：

```c
typedef struct{
    uint32_t inode_num;          // 0 表示空槽位
    char     filename[28];
}Director_entry;
```

`inode->block` 记录目录占用的块数，`inode->size` 记录有效目录项的累计字节数。
删除文件只是把目录项清零，不压缩目录块。

项目只有两层目录：根目录（inode 0）下是用户目录（目录名即用户名），用户目录下是文件。
因此所有查找都是固定的两步：

```
get_dir_inode_from_root(dirname)   在根目录中按名字找到用户目录 inode
        ↓
get_inode_num_in_dir(dir, filename) 在用户目录中按名字找到目标文件 inode
```

## 文件系统 API 与并发

`interface.c` 的每个公开函数都是同一套骨架：

```
pthread_mutex_lock(&fs_mutex)
    ├─ 参数校验（文件名长度、fscontext 是否已挂载）
    ├─ 定位父目录 inode / 目标文件 inode
    ├─ 执行操作（改内存 + 写回相关块）
    └─ 释放锁
```

只有 `fs_create` / `fs_mount` / `fs_unmount` 特殊：它们管理 `fscontext` 本身，挂载与卸载内部会加锁。

锁的粒度是**整卷一把锁**。这很粗，但文件操作会同时改动位图、inode 表和数据块，
细粒度锁需要维护大量顺序约束；对当前规模来说，粗锁换来的正确性更划算。

元数据落盘策略：

| 改动 | 是否立即写回 |
|------|--------------|
| inode 表（`fs_init_inode` / `write_back_inode`） | 是，写回该 inode 所在的整块 |
| inode 位图 | 是 |
| 数据块位图 | 是 |
| 文件数据块 | 是，`fs_write_file` 中逐块写回 |
| 超级块（空闲计数） | 否，卸载时统一写回 |

## 文件读写：跨块处理

`fs_read_file` / `fs_write_file` 支持任意 `offset` 与 `size`，需要把字节区间映射到逻辑块区间：

```
beginblock = offset / block_size            起始逻辑块（0 基）
endblock   = (offset + size - 1) / block_size  结束逻辑块
curoffset  = offset % block_size            首块内的起始偏移
```

循环遍历 `[beginblock, endblock]`：
首块从 `curoffset` 开始，中间块取满整块，末块只取剩余部分，`curoffset` 在首块之后归零。

写入是显式扩容/收缩的：

```
needblock = ceil((offset + size) / block_size)
    needblock > 当前块数 → 逐块 fs_alloc_data_block 并写入索引
                          中途失败则回滚已分配的块（回滚到 orig_block）
    needblock < 当前块数 → 从尾部逐块 fs_free_data_block
```

写回采用"读整块 → 局部覆盖 → 写整块"，这样块内未被覆盖的其他字节保持不变。

## Web 服务器

### 并发模型：Reactor + 线程池

```
主线程                       工作线程 × 4
  epoll_wait                 ┌─────────────────┐
     │                       │ worker_thread   │
     ├─ 监听 socket 可读      │  等条件变量      │
     │     accept            │  取任务          │
     │     malloc 连接对象    │  释放锁          │
     │     add_task ────────▶│  execute_connect │
     │                       └─────────────────┘
     └─ 检查 g_shutdown_requested
```

- 主线程只做 `epoll_wait` 和 `accept`，绝不做业务处理
- 任务队列有界（`MAX_QUEUE_SIZE = 128`），满则回绝新连接并关闭 socket，避免无界内存增长
- 队列用互斥锁 + 条件变量保护；等待用 `while` 循环判定，防虚假唤醒
- 关闭时置 `shutdown` 标志并 `pthread_cond_broadcast`，工作线程把队列清空后退出

### 请求生命周期：`execute_connect()`

```
recv 一次读入整个请求（假设 ≤ 4KB）
    │
    ├─ strstr 定位 "\r\n\r\n" 取出 body
    ├─ strstr 取 Content-Length
    ├─ strstr 取 Cookie 头（截到行尾）
    ├─ deal_with_request：解析 method / path
    ├─ 剥离 path 中的查询串（'?' 之后）
    │
    └─ 路由
         POST /login        → handle_login
         GET  /checklogin   → check_login_status
         GET  /logout       → handle_logout
         /cgi/*             → handle_cgi_request
         GET  /             → 返回 ./www/index.html
         其他               → 需登录，按 method 操作虚拟文件系统
```

**注意解析顺序**：`deal_with_request` 会在请求行末尾写 `'\0'`，
这之后原有的 `strstr` 就找不到后面的头部了。因此 Cookie 与 Content-Length
必须在调用它**之前**提取。这是当前实现的隐含约束。

其余路由都要求已登录（`get_username_from_cookie` 成功），用户名即该用户的目录名：

| method + path | 行为 |
|---------------|------|
| `GET /list` | 列出当前用户目录 |
| `GET /members` | 列出根目录成员（所有用户） |
| `GET /<file>` | 读取文件内容并返回 |
| `POST /<file>` | 创建空文件；已存在返回 409 |
| `PUT /<file>` | 写入文件；正文超出首次 recv 时循环接收 |
| `DELETE /<file>` | 删除文件 |

### 会话管理

`Session sessions[MAX_SESSIONS]`，64 个固定槽位，`session_mutex` 保护：

```
session_create    找空槽；无空槽则回收一个已过期的；生成 token，设 24h 过期
session_lookup    token 比对；命中但已过期则销毁并返回失败
session_destroy   标记槽位空闲
```

token 优先从 `/dev/urandom` 取真随机字节，读取失败才退回 `rand()`。
下发给客户端时为 `Set-Cookie: session=...; HttpOnly; SameSite=Lax`，
登出时把 `Expires` 设为 1970 年，让浏览器立即清除。

### CGI

```
安全校验三重
  1. 拒绝含 "../" 的路径，拒绝以 / 或 \ 开头的绝对路径
  2. realpath 解析后必须仍在 exec/ 目录内（含边界检查，防 /execfoo 绕过、防符号链接逃逸）
  3. 必须是普通文件且带可执行位

数据通路
  POST → in_pipe  把请求体写入子进程 stdin
  所有 → out_pipe 子进程 stdout 逐块转发给 client_fd

子进程准备
  dup2 重定向标准流
  关闭 3 ~ _SC_OPEN_MAX 的全部继承 fd（否则 CGI 持有 socket，连接无法真正关闭）
  按 CGI/1.1 规范设置环境变量：HTTP_COOKIE / REQUEST_METHOD / PATH_INFO /
                                GATEWAY_INTERFACE / SERVER_PROTOCOL / CONTENT_LENGTH
  execl 执行目标程序
```

### 响应发送

三个函数，编码方式不同：

| 函数 | 用于 | 编码 |
|------|------|------|
| `send_error` | 错误状态 | `Content-Length` + 正文即状态描述 |
| `send_file_response` | `./www/index.html`（磁盘文件） | chunked，边读边发 |
| `send_file_content` | 虚拟文件系统中的文件 | `Content-Length` + 按扩展名推断 `Content-Type` |
| `send_response` | 操作成功提示 | chunked |

chunked 格式：每块 `<十六进制长度>\r\n<数据>\r\n`，以 `0\r\n\r\n` 结束。

### 信号处理

| 信号 | 处理 |
|------|------|
| `SIGINT` | 处理函数只置 `volatile sig_atomic_t g_shutdown_requested = 1`，实际清理在主循环里做（异步信号安全） |
| `SIGPIPE` | `SIG_IGN`。客户端提前断开时 `write` 不应杀死整个进程 |

主循环的 `epoll_wait` 带 500ms 超时，就是为定期检查这个标志位。

## 已知约束

- **目录层级固定为两层**，不支持嵌套目录
- **单文件上限 134 块**（约 68KB），受多级索引实现限制
- **请求一次性读取 4KB**，不解析分片到达的请求，也不支持 `Expect: 100-continue`
- **响应统一短连接**（`Connection: close`），无 keep-alive
- **整卷一把锁**，文件操作之间无并行度
- **`achieve.c` 中局部 `sessions` 与 `fs_mutex` 是两套独立的锁**，不存在嵌套加锁，因此没有死锁风险
- 镜像格式化的入口 `fs_create()` 未接入命令行，需在首次部署时另行调用
