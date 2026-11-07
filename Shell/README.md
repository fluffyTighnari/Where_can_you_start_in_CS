# Shell 内部架构

迷你 Shell 的实现说明。对外功能与构建方式见仓库根目录 `README.md`。

## 文件构成

单文件实现：`myshell.c`。分成四层，从下到上依次是终端层、解析层、执行层、信号层。

```
main 主循环
  ├── clean_up_processes()     回收已结束的后台进程
  ├── print_prompt()           打印彩色提示符（当前路径）
  ├── getinput()               原始终端模式逐字符读取
  ├── deal_with_input()        切分为 Command 结构
  ├── 内建命令: exit / cd      由 shell 自身执行
  ├── execute_command()        单命令执行
  └── execute_pipe_command()   管道执行

信号层
  ├── handle_child()           SIGCHLD：非阻塞回收僵尸
  ├── handle_int()             SIGINT：shell 忽略，交前台子进程
  └── SIGTSTP / SIGQUIT        直接忽略
```

## 核心数据结构

```c
typedef struct Command{
    char* split_command[MAX_ARGS];  // 参数数组，末尾为 NULL（execvp 要求）
    int   size_of_command;          // 参数个数
    int   pipe;                     // 是否含管道符
    int   background;               // 是否后台运行
}Command;
```

解析阶段一次性把"词法"与"命令形状"都记录下来：参数数组供 `execvp` 使用，
`pipe` / `background` 两个标志决定后续走哪条执行路径。管道命令不做成嵌套结构，
而是识别出 `|` 的位置后现场拆成 `cmd1` / `cmd2` 两个 `Command`——因为只支持单管道。

后台任务只用一个扁平数组跟踪：

```c
static int   bg_count;                 // 当前后台任务数
static pid_t bg_pids[MAX_BG_JOBS];     // PID 表，0 表示空闲槽位
```

## 终端层：`getinput()`

默认终端是行缓冲 + 回显，退格由内核处理。本 Shell 关闭这两个特性以接管输入：

```
tcgetattr 备份原属性 → g_old_tio / g_tio_saved
c_lflag &= ~(ICANON | ECHO)    关闭规范模式与回显
VMIN = 1, VTIME = 0            每读 1 字节即返回，无超时
    │
    └─ 逐字符循环
          '\n'        → 结束输入
          8 / 127     → 退格：i--，输出 "\b \b" 擦除屏幕字符
          其他         → 存入缓冲区并自行 putchar 回显
          缓冲区满     → 丢弃剩余字符直到换行，并输出截断警告
    │
    └─ 恢复终端属性
```

终端属性恢复有两条路径：正常返回时显式恢复；异常退出时由 `atexit(restore_terminal)` 兜底。
`safe_exit()` 在 `exit()` 前也会先恢复，避免进程退出后终端停留在无回显状态。

## 解析层：`deal_with_input()`

按空格 / 制表符切分为 token，用"延迟一位"的技巧处理 `|` 和 `&`：

```
prev_token = NULL
for each token:
    若 prev_token 不为空:
        若 prev_token == "|" → 置 pipe 标志（不存入参数数组）
        否则存入参数数组
    prev_token = token
循环结束后单独处理最后一个 token:
    若为 "&" → 置 background 标志
    若为 "|" → 置 pipe 标志
    否则存入参数数组
```

之所以延迟一位，是因为 `|` 与 `&` 本身不是参数，但需要它前面的 token 已经入栈后才能判断。
这一层故意保持简单：不处理引号、转义、通配符展开。

## 执行层

### 内建命令

`cd` 和 `exit` 必须由 shell 自身执行，因为子进程的 `chdir` 不影响父进程。

`cd` 的三种形态：

| 输入 | 行为 |
|------|------|
| `cd` | 切到 `$HOME` |
| `cd -` | 切到 `$OLDPWD`，并打印该路径 |
| `cd path` | 切到 `path` |

成功切换后把原来的工作目录写入 `OLDPWD`。

`exit` 会先向所有后台任务发 `SIGHUP` 再退出。

### 单命令 `execute_command()`

```
fork
 ├─ 子进程: 恢复 SIGINT / SIGTSTP / SIGQUIT 为默认 → execvp
 │           execvp 失败按 errno 映射退出码：ENOENT→127, EACCES→126, 其他→1
 │           用 _exit 而非 exit，避免冲刷父进程继承来的 stdio 缓冲区
 │
 └─ 父进程:
       前台 → waitpid_retry 阻塞等待
       后台 → 记录 PID 到 bg_pids，打印 "[作业号] PID"
              超出上限则改为前台执行
```

**后台模式的竞态**：如果 fork 之后、记录 PID 之前子进程就退出了，
`SIGCHLD` 处理函数会先把它回收掉，随后父进程记录的就是一个已失效的 PID。
解决办法是后台模式下在 `fork` **之前**阻塞 `SIGCHLD`，记录完成后再解除阻塞——
被阻塞期间到达的信号不会丢失，解除后会补投递。

### 管道 `execute_pipe_command()`

```
pipe(pipefd)                     pipefd[0] 读端，pipefd[1] 写端
    │
    ├─ fork 子进程 1
    │     close(读端)
    │     dup2(写端, STDOUT)      标准输出改为管道写端
    │     close(写端)             重定向后原 fd 可关闭
    │     execvp(cmd1)
    │
    ├─ fork 子进程 2
    │     close(写端)
    │     dup2(读端, STDIN)       标准输入改为管道读端
    │     close(读端)
    │     execvp(cmd2)
    │
    └─ 父进程
          close(两端)             关键：写端不关闭，读端永远等不到 EOF
          前台 → 依次 waitpid 两个子进程
          后台 → 两个 PID 都记入 bg_pids，作业号 = 已记录的进程对数
```

任一 `fork` 失败时都要关闭管道两端；第二个 `fork` 失败还要 `SIGKILL` 掉第一个子进程，
否则它会一直挂在管道读端上。

## 信号层

| 信号 | 处理 | 理由 |
|------|------|------|
| `SIGCHLD` | `handle_child()`：`while(waitpid(-1, &status, WNOHANG) > 0)` 循环回收 | 一次信号可能对应多个子进程退出，必须循环。处理函数内保存并恢复 `errno` |
| `SIGINT` | `handle_int()`：空函数 | Ctrl+C 交给前台子进程处理，shell 自身不能被中断 |
| `SIGTSTP` / `SIGQUIT` | `SIG_IGN` | 避免 shell 自身被挂起或退出 |

所有 `sigaction` 都设置 `SA_RESTART`，让被信号打断的系统调用自动重启，
省去在业务代码里到处处理 `EINTR`。

此外 `waitpid_retry()` 单独封装了前台等待：即使有 `SA_RESTART`，
`waitpid` 在特定场景仍可能返回 `EINTR`，这里再兜一层重试循环。

## 后台任务的生命周期

```
启动            bg_pids[bg_count++] = pid
                ↓
每轮循环开始    clean_up_processes()：对每个 PID 做 WNOHANG 探测
                返回 0（仍在运行）→ 保留并压缩到数组前部
                返回 >0（已退出）→ 丢弃
                返回 <0（ECHILD 等）→ 一并丢弃
                ↓
用户输入 exit   遍历 bg_pids，逐个 kill(SIGHUP)，然后退出
```

用"压缩到前部"的方式维护数组，避免删除中间元素后的空洞。
