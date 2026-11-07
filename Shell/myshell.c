/*
 * myshell.c —— 简易 Shell
 * 支持：内建命令 cd / exit、外部命令、单管道（|）、后台运行（&）
 * 核心机制：fork + execvp 执行命令；dup2 实现管道重定向；
 *           SIGCHLD 回收子进程；termios 原始模式实现逐字符输入
 */
#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<unistd.h>
#include<sys/wait.h>
#include<sys/types.h>
#include<signal.h>
#include<errno.h>
#include<termios.h>
#include<limits.h>
#include<fcntl.h>

#define MAX_INPUT 1024    /* 单条输入的最大字符数 */
#define MAX_ARGS 64       /* 单条命令最多参数个数（含命令本身） */
#define MAX_BG_JOBS 64    /* 最多同时跟踪的后台任务数 */

static int bg_count = 0;                          /* 当前后台任务数量 */
static pid_t bg_pids[MAX_BG_JOBS] = {0};          /* 后台任务 PID 表，0 表示空闲槽位 */
static struct termios g_old_tio;                  /* 备份的原始终端属性 */
static int g_tio_saved = 0;                       /* 标记终端属性是否已备份（供退出时恢复） */

typedef struct Command{
    char* split_command[MAX_ARGS];  /* 按空格切分后的参数数组（argv 形式，末尾为 NULL） */
    int size_of_command;            /* 参数个数 */
    int pipe;                       /* 是否包含管道符 | */
    int background;                 /* 是否后台运行（命令以 & 结尾） */
}Command;

void print_prompt(void);
void clean_up_processes(void);
void handle_child(int signal);
void handle_int(int signal);
Command deal_with_input(char* input);
void execute_command(Command cmd);
void execute_pipe_command(Command cmd1, Command cmd2, int background);
char* getinput(void);
void restore_terminal(void);
void safe_exit(int code);
int waitpid_retry(pid_t pid, int *status, int options);

int main(void){
    struct sigaction sa_chld, sa_int, sa_ign;
    /* 信号处理说明：
       SIGCHLD：子进程退出时通知父进程，用于回收子进程，防止产生僵尸进程
       SIGINT：用户按下 Ctrl+C 时触发，默认会终止进程，这里由 shell 自行处理
       SIGTSTP / SIGQUIT：终端停止信号与退出信号，shell 选择忽略，避免自身被中断
    */

    /* 注册 SIGCHLD：子进程退出信号 */
    memset(&sa_chld, 0, sizeof(sa_chld));
    /* 清零结构体，避免残留数据影响信号配置 */
    sa_chld.sa_handler = handle_child;
    /* 设置子进程退出信号的处理函数 */
    sigemptyset(&sa_chld.sa_mask);
    /* 初始化信号集，确保没有阻塞的信号 */
    sa_chld.sa_flags = SA_RESTART;
    /* 确保系统调用在信号处理后自动重启，不用自己循环处理EINTR错误 */
    if (sigaction(SIGCHLD, &sa_chld, NULL) == -1) {
        perror("sigaction SIGCHLD");
        exit(1);
    }
    /* sigaction 系统调用：注册信号及其处理方式 */

    /* 注册 SIGINT：shell 本身忽略，交由前台子进程处理 */
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = handle_int;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa_int, NULL) == -1) {
        perror("sigaction SIGINT");
        exit(1);
    }

    /* 忽略 SIGTSTP / SIGQUIT */
    memset(&sa_ign, 0, sizeof(sa_ign));
    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    sa_ign.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &sa_ign, NULL);
    sigaction(SIGQUIT, &sa_ign, NULL);

    atexit(restore_terminal);
    /* 注册退出钩子：程序退出时自动恢复终端设置（类似其他语言中的 defer） */

    Command cmd;          /* 解析后的命令结构 */
    char* input = NULL;   /* 用户输入缓冲区 */

    while(1){
        /* 主循环：读取输入 -> 解析 -> 执行，直到用户输入 exit */
        clean_up_processes();
        /* 清理已经结束的后台进程，防止僵尸进程 */
        print_prompt();
        /* 打印命令提示符 */

        input = getinput();
        /* 获取用户输入 */
        if(input == NULL || strlen(input) == 0){
            /* 用户未输入任何内容，继续下一轮循环 */
            free(input);
            continue;
        }

        cmd = deal_with_input(input);
        /* 解析用户输入，生成命令结构 */

        if(cmd.size_of_command > 0 && strcmp(cmd.split_command[0], "exit") == 0){
            /* 分支1：用户输入 exit，终止所有后台任务 */
            if (bg_count > 0) {
                printf("There are %d background job(s) running.\n", bg_count);
                for (int i = 0; i < bg_count; i++) {
                    if (bg_pids[i] > 0) {
                        kill(bg_pids[i], SIGHUP);  /* 向后台任务发送 SIGHUP 信号终止 */
                    }
                }
            }
            free(input);
            safe_exit(0);
        }

        if(cmd.size_of_command > 0 && strcmp(cmd.split_command[0], "cd") == 0){
            /* 分支2：用户输入 cd，处理目录切换。
               cd 是 shell 内建命令，改变的是 shell 自身的工作目录，而非子进程的目录，
               因此必须由主进程直接执行 */
            const char *target;
            if(cmd.size_of_command < 2){
                /* 未指定目标目录：默认切换到 HOME 目录（与 Linux 原生行为一致） */
                target = getenv("HOME");
                if (target == NULL) {
                    fprintf(stderr, "cd: HOME not set\n");
                    free(input);
                    continue;
                }
            } else if (strcmp(cmd.split_command[1], "-") == 0) {
                /* 输入 cd -：切换到上一次的工作目录，读取环境变量 OLDPWD（与 Linux 原生行为一致） */
                target = getenv("OLDPWD");
                if (target == NULL) {
                    fprintf(stderr, "cd: OLDPWD not set\n");
                    free(input);
                    continue;
                }
                printf("%s\n", target);
            } else {
                /* 指定了目标目录：直接使用用户输入的路径 */
                target = cmd.split_command[1];
            }

            char cwd_before[PATH_MAX];
            int have_old = (getcwd(cwd_before, sizeof(cwd_before)) != NULL);
            /* 获取当前工作目录，用于在成功切换目录后更新 OLDPWD 环境变量 */

            if(chdir(target) != 0){
                perror("cd");
            } else if (have_old) {
                setenv("OLDPWD", cwd_before, 1);
            }

            free(input);
            continue;
        }

        if(cmd.pipe == 0){
            /* 分支3：用户输入普通命令，直接执行 */
            execute_command(cmd);
        } else {
            /* 分支4：用户输入管道命令，执行管道操作 */
            int index_pipe = -1;
            for(int i = 0; i < cmd.size_of_command; i++){
                if(strcmp(cmd.split_command[i], "|") == 0){
                    index_pipe = i; /* 找到管道位置 */
                    break;
                }
            }
            if(index_pipe <= 0 || index_pipe >= cmd.size_of_command - 1){
                fprintf(stderr, "管道语法错误\n");
                free(input);
                continue;
            }
            /* 拆解管道命令为两个独立的命令 */
            Command cmd1;
            cmd1.size_of_command = index_pipe;
            cmd1.pipe = 0;
            cmd1.background = 0;
            int index = 0;
            for(int i = 0; i < cmd1.size_of_command; i++){
                cmd1.split_command[i] = cmd.split_command[index++];
            }
            cmd1.split_command[cmd1.size_of_command] = NULL;

            Command cmd2;
            cmd2.size_of_command = cmd.size_of_command - index_pipe - 1;
            cmd2.pipe = 0;
            cmd2.background = 0;
            index++;
            for(int i = 0; i < cmd2.size_of_command; i++){
                cmd2.split_command[i] = cmd.split_command[index++];
            }
            cmd2.split_command[cmd2.size_of_command] = NULL;

            execute_pipe_command(cmd1, cmd2, cmd.background);
            /* 执行管道命令，传入两个命令结构和后台标志 */
        }
        free(input);
        /* 每轮循环结束时释放输入缓冲区 */
    }
    return 0;
}

void handle_child(int signal){
    /* 子进程退出时内核向父进程发送 SIGCHLD，这里统一回收所有已结束的子进程 */
    (void)signal;   /* 忽略参数，避免未使用警告 */
    int saved_errno = errno;  /* 保存 errno：信号处理函数不应破坏调用方的 errno */
    int status;
    while(waitpid(-1, &status, WNOHANG) > 0)   /* 非阻塞循环回收所有已退出的子进程 */
;
    errno = saved_errno;  /* 恢复调用方原有的 errno */
}

void handle_int(int signal){
    /* 处理 SIGINT 信号：shell 本身不响应，交由前台子进程处理 */
    (void)signal;
}

void clean_up_processes(void){
    /* 每轮循环开始时清理已结束的后台进程，防止产生僵尸进程 */
    int new_count = 0;
    for (int i = 0; i < bg_count; i++) {
        if (bg_pids[i] > 0) {
            int status;
            pid_t ret = waitpid(bg_pids[i], &status, WNOHANG);
            if (ret == 0) {
                bg_pids[new_count++] = bg_pids[i];
            }
            /* ret > 0：已退出，不保留 */
            /* ret < 0：出错（如 ECHILD），一并移除 */
        }
    }
    bg_count = new_count;
}

void print_prompt(void){
    /* 打印命令提示符，显示当前工作目录 */
    char *cwd = getcwd(NULL, 0);
    if(cwd != NULL){
        printf("\033[1;32m%s\033[0m $ ", cwd);
        free(cwd);
    } else {
        printf("myshell $ ");
    }
    fflush(stdout);
}

char* getinput(void){
    /* 读取用户输入：关闭行缓冲与回显，逐字符读取，支持退格删除 */
    char* input = (char*)malloc(MAX_INPUT * sizeof(char));
    /* 分配输入缓冲区 */
    if (input == NULL) {
        perror("malloc");
        safe_exit(1);
    }

    struct termios old_tio, new_tio;
    if (tcgetattr(STDIN_FILENO, &old_tio) == -1) {
        /* 获取终端属性失败，可能是非终端环境 */
        perror("tcgetattr");
        free(input);
        return NULL;
    }
    g_old_tio = old_tio;   /* 备份终端属性，供程序退出时恢复 */
    g_tio_saved = 1;

    new_tio = old_tio;                    /* 基于当前属性做修改 */
    new_tio.c_lflag &= ~(ICANON | ECHO);  /* 关闭规范模式与回显：实现逐字符读取、输入不回显 */
    new_tio.c_cc[VMIN] = 1;               /* read 至少返回 1 字节才返回 */
    new_tio.c_cc[VTIME] = 0;              /* 无超时等待 */
    if (tcsetattr(STDIN_FILENO, TCSANOW, &new_tio) == -1) {
        /* 设置终端属性失败，可能是非终端环境 */
        perror("tcsetattr");
        free(input);
        return NULL;
    }

    int i = 0;
    int truncated = 0;
    int c;
    while (i < MAX_INPUT - 1) {
        /* 读取一个字符 */
        c = getchar();
        if (c == EOF) {
            /* 用户按下 Ctrl+D，表示输入结束 */
            if (feof(stdin)) {
                tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
                g_tio_saved = 0;
                printf("\n");
                free(input);
                safe_exit(0);
            }
            continue;
        }
        if (c == '\n') {
            break;
        } else if (c == 8 || c == 127) {
            if (i > 0) {
                i--;
                printf("\b \b");  /* 光标左移、输出空格覆盖、再左移：擦除屏幕上已显示的字符 */
                fflush(stdout);
            }
        } else {
            input[i++] = (char)c;  /* 存入输入缓冲区 */
            putchar(c);            /* 回显用户输入的字符 */
            fflush(stdout);
        }
    }

    /* 如果输入已满，丢弃剩余字符直到换行 */
    if (i == MAX_INPUT - 1) {
        int peek = getchar();
        if (peek != '\n' && peek != EOF) {
            truncated = 1;
            while (peek != '\n' && peek != EOF) {
                peek = getchar();
            }
        }
    }

    input[i] = '\0';

    if (tcsetattr(STDIN_FILENO, TCSANOW, &old_tio) == -1) {
        perror("tcsetattr restore");
    }
    g_tio_saved = 0;   /* 终端已恢复，清除备份标记 */
    printf("\n");      /* 输出换行，使下次提示符从新行开始 */

    if (truncated) {
        /* 如果输入被截断，给出警告 */
        fprintf(stderr, "Warning: input truncated (max %d characters)\n", MAX_INPUT - 1);
    }

    return input;
}

Command deal_with_input(char* input){
    /* 解析用户输入，生成命令结构 */
    Command cmd;
    cmd.size_of_command = 0;
    cmd.pipe = 0;
    cmd.background = 0;

    char* token = strtok(input, " \t");   /* 按空格或制表符切分输入 */
    char* prev_token = NULL;
    /* 延迟一个 token 再存入：先缓存当前 token，待下一轮循环检查
       它是否为 | 或 & 特殊符号，再决定是否作为参数存入参数数组 */
    while(token != NULL && cmd.size_of_command < MAX_ARGS - 1){
        /* 先把上一个 token 存入 */
        if (prev_token != NULL) {
            if (strcmp(prev_token, "|") == 0) {
                cmd.pipe = 1;
            }
            cmd.split_command[cmd.size_of_command++] = prev_token;
        }
        prev_token = token;
        token = strtok(NULL, " \t");
    }

    /* 处理最后一个 token：如果是 & 则标记后台，不存入参数 */
    if (prev_token != NULL) {
        if (strcmp(prev_token, "&") == 0) {
            cmd.background = 1;
        } else {
            if (strcmp(prev_token, "|") == 0) {
                cmd.pipe = 1;
            }
            cmd.split_command[cmd.size_of_command++] = prev_token;
        }
    }

    cmd.split_command[cmd.size_of_command] = NULL;
    return cmd;
}

int waitpid_retry(pid_t pid, int *status, int options){
    /* 封装 waitpid：当系统调用被信号打断（EINTR）时自动重试，
       避免把"被打断"误判为真正的错误 */
    int ret;
    do {
        ret = waitpid(pid, status, options);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

void execute_command(Command cmd){
    /* 阻塞 SIGCHLD 防止竞态：fork 后、记录 bg_pids 前子进程已退出 */
    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (cmd.background) {
        sigprocmask(SIG_BLOCK, &mask, &oldmask);
    }

    pid_t pid = fork();
    if(pid == 0){
        /* 子进程：恢复默认信号处理 */
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);

        execvp(cmd.split_command[0], cmd.split_command);
        perror("execvp");
        int exit_code;
        if (errno == ENOENT) {
            exit_code = 127;
        } else if (errno == EACCES) {
            exit_code = 126;
        } else {
            exit_code = 1;
        }
        fprintf(stderr, "执行失败: %s\n", cmd.split_command[0]);
        _exit(exit_code);
    } else if(pid < 0){
        perror("fork");
        if (cmd.background) {
            sigprocmask(SIG_SETMASK, &oldmask, NULL);
        }
        return;
    } else {
        if(cmd.background == 1){
            if(bg_count < MAX_BG_JOBS){
                bg_pids[bg_count] = pid;   /* 记录后台进程 PID，供后续清理与终止 */
                printf("[%d] %d\n", bg_count + 1, pid);   /* 打印作业号与进程 PID */
                bg_count++;
            } else {
                fprintf(stderr, "后台进程数量超出限制，改为前台执行\n");
                sigprocmask(SIG_SETMASK, &oldmask, NULL);
                waitpid_retry(pid, NULL, 0);
                return;
            }
            sigprocmask(SIG_SETMASK, &oldmask, NULL);
        } else {
            int status;
            waitpid_retry(pid, &status, 0);
        }
    }
}

void execute_pipe_command(Command cmd1, Command cmd2, int background){
    int pipefd[2];   /* 管道文件描述符：pipefd[0] 读端，pipefd[1] 写端 */
    if(pipe(pipefd) == -1){
        perror("pipe");
        return;
    }

    /* 后台模式下阻塞 SIGCHLD */
    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (background) {
        sigprocmask(SIG_BLOCK, &mask, &oldmask);
    }

    pid_t pid1 = fork();
    if(pid1 == 0){
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);

        close(pipefd[0]);   /* 子进程1 不需要读端 */
        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {   /* 把标准输出重定向到管道写端 */
            perror("dup2 stdout");
            _exit(1);
        }
        close(pipefd[1]);   /* 重定向完成后关闭原写端 */
        execvp(cmd1.split_command[0], cmd1.split_command);
        perror("execvp");
        _exit(errno == ENOENT ? 127 : (errno == EACCES ? 126 : 1));
    }

    if (pid1 < 0) {
        perror("fork pid1");
        close(pipefd[0]);
        close(pipefd[1]);
        if (background) sigprocmask(SIG_SETMASK, &oldmask, NULL);
        return;
    }

    pid_t pid2 = fork();
    if(pid2 == 0){
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);

        close(pipefd[1]);   /* 子进程2 不需要写端 */
        if (dup2(pipefd[0], STDIN_FILENO) == -1) {    /* 把标准输入重定向到管道读端 */
            perror("dup2 stdin");
            _exit(1);
        }
        close(pipefd[0]);   /* 重定向完成后关闭原读端 */
        execvp(cmd2.split_command[0], cmd2.split_command);
        perror("execvp");
        _exit(errno == ENOENT ? 127 : (errno == EACCES ? 126 : 1));
    }

    if (pid2 < 0) {
        perror("fork pid2");
        kill(pid1, SIGKILL);
        close(pipefd[0]);
        close(pipefd[1]);
        waitpid_retry(pid1, NULL, 0);
        if (background) sigprocmask(SIG_SETMASK, &oldmask, NULL);
        return;
    }

    close(pipefd[0]);   /* 父进程关闭两端：若写端不关闭，读端将永远等不到 EOF */
    close(pipefd[1]);

    if (background) {
        if (bg_count + 1 < MAX_BG_JOBS) {
            bg_pids[bg_count++] = pid1;   /* 记录两个后台进程 PID */
            bg_pids[bg_count++] = pid2;
            printf("[%d] %d | %d\n", bg_count / 2, pid1, pid2);   /* 作业号 = 已记录的进程对数 */
        } else {
            fprintf(stderr, "后台进程数量超出限制，改为前台执行\n");
            sigprocmask(SIG_SETMASK, &oldmask, NULL);
            waitpid_retry(pid1, NULL, 0);
            waitpid_retry(pid2, NULL, 0);
            return;
        }
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
    } else {
        int status1, status2;
        waitpid_retry(pid1, &status1, 0);
        waitpid_retry(pid2, &status2, 0);
    }
}

void restore_terminal(void){
    /* 若终端属性已被修改，恢复为原始值（程序退出前调用） */
    if (g_tio_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_old_tio);
        g_tio_saved = 0;
    }
}

void safe_exit(int code){
    restore_terminal();   /* 先恢复终端，避免退出后终端停留在无回显状态 */
    exit(code);
}
