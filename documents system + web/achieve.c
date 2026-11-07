/*
 * achieve.c —— Web 服务器业务层实现
 *
 * 职责：
 *   1. HTTP 请求分发：按 method + path 路由到登录、静态资源、文件操作、CGI
 *   2. 会话管理：Cookie/Session 登录态，token 来自 /dev/urandom，24h 过期
 *   3. 文件系统桥接：把 RESTful 请求映射到 interface.c 的虚拟文件系统 API
 *   4. CGI：fork + exec 外部程序，标准输入输出通过管道与 HTTP 响应对接
 *
 * 每个连接由线程池中的一个工作线程独占处理（见 web2.c），
 * 因此连接级的局部变量无需加锁；共享的 sessions 与文件系统各自有锁保护。
 */
#ifndef ACHIEVE_C
#define ACHIEVE_C
#include "achieve.h"
#include "interface.h"
#include <ctype.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <netinet/tcp.h>

#define CGI_DIR "exec"                 /* CGI 可执行程序所在目录 */
#define MAX_SESSIONS 64                /* 同时在线的会话上限 */
#define SESSION_TOKEN_LEN 32           /* token 长度（不含结尾 NUL） */
#endif

/* 一条会话记录：token ↔ 用户名映射 */
typedef struct {
    char token[SESSION_TOKEN_LEN + 1];
    char username[name_length];
    int in_use;                        /* 槽位是否占用 */
    time_t expires;                    /* 过期时间戳 */
} Session;

static Session sessions[MAX_SESSIONS];
static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;   /* 保护 sessions 数组 */

/*
 * 生成随机会话 token。
 * 优先读 /dev/urandom 取真随机字节，读取失败时退回 rand()（安全性较弱但可用）。
 */
static void generate_token(char* buf, size_t len) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        unsigned char rand_bytes[256];
        ssize_t n = read(fd, rand_bytes, sizeof(rand_bytes));
        close(fd);
        if (n > 0) {
            for (size_t i = 0; i < len; i++) {
                buf[i] = charset[rand_bytes[i % (size_t)n] % (sizeof(charset) - 1)];
            }
            buf[len] = '\0';
            return;
        }
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = charset[rand() % (sizeof(charset) - 1)];
    }
    buf[len] = '\0';
}

/*
 * 创建会话并返回 token 指针（指向 sessions 内部，调用方不应 free）。
 * 先找空槽；若没有空槽，则顺带回收一个已过期的槽位。
 * 返回 NULL 表示会话容量已满。
 */
static const char* session_create(const char* username) {
    pthread_mutex_lock(&session_mutex);
    time_t now = time(NULL);
    int slot = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].in_use) {
            slot = i;
            break;
        }
        if (sessions[i].expires < now) {   /* 顺便回收过期会话 */
            sessions[i].in_use = 0;
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&session_mutex);
        return NULL;
    }
    generate_token(sessions[slot].token, SESSION_TOKEN_LEN);
    strncpy(sessions[slot].username, username, name_length - 1);
    sessions[slot].username[name_length - 1] = '\0';
    sessions[slot].expires = now + 24 * 3600;   /* 24 小时有效期 */
    sessions[slot].in_use = 1;
    const char* token = sessions[slot].token;
    pthread_mutex_unlock(&session_mutex);
    return token;
}

/* 用 token 反查用户名；成功返回 0 并写入 username_buf，失败/过期返回 -1 */
static int session_lookup(const char* token, char* username_buf, size_t buf_size) {
    if (!token || !username_buf || buf_size == 0) return -1;
    pthread_mutex_lock(&session_mutex);
    time_t now = time(NULL);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].in_use && strcmp(sessions[i].token, token) == 0) {
            if (sessions[i].expires < now) {   /* 命中但已过期：销毁并视为失败 */
                sessions[i].in_use = 0;
                pthread_mutex_unlock(&session_mutex);
                return -1;
            }
            strncpy(username_buf, sessions[i].username, buf_size - 1);
            username_buf[buf_size - 1] = '\0';
            pthread_mutex_unlock(&session_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&session_mutex);
    return -1;
}

/* 注销会话：把对应槽位标记为空闲 */
static void session_destroy(const char* token) {
    if (!token) return;
    pthread_mutex_lock(&session_mutex);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].in_use && strcmp(sessions[i].token, token) == 0) {
            sessions[i].in_use = 0;
            break;
        }
    }
    pthread_mutex_unlock(&session_mutex);
}

/*
 * URL 解码（就地修改 s）：%XX → 字节，'+' → 空格。
 * 用读写双指针做原地压缩，解码后长度不会超过原长度。
 * 返回解码后的字符串长度。
 */
static size_t url_decode(char* s) {
    char* p = s;
    char* q = s;
    while (*p) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            int c = 0;
            sscanf(p + 1, "%2x", &c);
            *q++ = (char)c;
            p += 3;
        } else if (*p == '+') {
            *q++ = ' ';
            p++;
        } else {
            *q++ = *p++;
        }
    }
    *q = '\0';
    return (size_t)(q - s);
}


/* 从 Cookie 头中提取 session 值；返回堆上分配的字符串（调用方负责 free） */
static char* get_session_token_from_cookie(const char* cookie) {
    if (!cookie) return NULL;
    char* copy = strdup(cookie);
    if (!copy) return NULL;
    char* session_token = NULL;
    char* tok = strtok(copy, "; ");   /* Cookie 形如 "a=1; session=xxx; b=2" */
    while (tok) {
        if (strncmp(tok, "session=", 8) == 0) {
            session_token = strdup(tok + 8);
            break;
        }
        tok = strtok(NULL, "; ");
    }
    free(copy);
    return session_token;
}

/* 由 Cookie 得到当前登录用户名；返回堆上分配的用户名，未登录返回 NULL */
char* get_username_from_cookie(const char* cookie) {
    char* token = get_session_token_from_cookie(cookie);
    if (!token) return NULL;
    char username[name_length];
    if (session_lookup(token, username, sizeof(username)) != 0) {
        free(token);
        return NULL;
    }
    free(token);
    return strdup(username);
}

/* 检测路径中是否含 "../" 片段，用于阻断目录穿越攻击 */
static int path_contains_traversal(const char* path) {
    const char* p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\\' || p[2] == '\0')) {
            return 1;
        }
        p++;
    }
    return 0;
}

/*
 * 执行 CGI 程序并把输出原样写回客户端。
 *
 * 安全校验三重：
 *   1. 拒绝含 "../" 的路径、拒绝以 / 或 \ 开头的绝对路径
 *   2. realpath 解析后必须仍位于 CGI_DIR 之内（防符号链接逃逸）
 *   3. 目标必须是普通文件且带可执行位
 *
 * 数据通路：
 *   POST → in_pipe  把请求体喂给子进程 stdin
 *   所有方法 → out_pipe  子进程 stdout 直接转发给 client_fd
 * 约定 CGI 程序自行输出完整的 Content-Type 等响应头。
 */
void handle_cgi_request(int client_fd, const char* buffer, const char* method, const char* path, const char* body, const char* cookie) {
    const char* cgi_rel = path + 4;   /* 跳过前缀 "/cgi" */
    if (path_contains_traversal(cgi_rel) || *cgi_rel == '/' || *cgi_rel == '\\') {
        send_error(client_fd, 400, "Bad Request");
        return;
    }

    char cgi_path[512];
    int n = snprintf(cgi_path, sizeof(cgi_path), "%s/%s", CGI_DIR, cgi_rel);
    if (n < 0 || (size_t)n >= sizeof(cgi_path)) {
        send_error(client_fd, 414, "URI Too Long");
        return;
    }

    char resolved[512];
    if (!realpath(cgi_path, resolved)) {
        send_error(client_fd, 404, "CGI Program Not Found");
        return;
    }

    /* 解析后的真实路径必须仍在 CGI_DIR 下（含边界检查，避免 /execfoo 绕过） */
    char cgi_dir_real[512];
    if (!realpath(CGI_DIR, cgi_dir_real)) {
        send_error(client_fd, 500, "Internal Server Error");
        return;
    }
    size_t dir_len = strlen(cgi_dir_real);
    if (strncmp(resolved, cgi_dir_real, dir_len) != 0 ||
        (resolved[dir_len] != '/' && resolved[dir_len] != '\0')) {
        send_error(client_fd, 403, "Forbidden");
        return;
    }

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) {
        send_error(client_fd, 404, "CGI Program Not Found or Not Executable");
        return;
    }

    int out_pipe[2];                    /* 子进程 stdout → 服务器 */
    int in_pipe[2] = {-1, -1};          /* 服务器 → 子进程 stdin，仅 POST 使用 */
    if (pipe(out_pipe) == -1) {
        send_error(client_fd, 500, "Internal Server Error");
        return;
    }

    int is_post = (strcmp(method, "POST") == 0);
    if (is_post) {
        if (pipe(in_pipe) == -1) {
            close(out_pipe[0]);
            close(out_pipe[1]);
            send_error(client_fd, 500, "Internal Server Error");
            return;
        }
    }

    pid_t pid = fork();
    if (pid == -1) {
        send_error(client_fd, 500, "Internal Server Error");
        close(out_pipe[0]);
        close(out_pipe[1]);
        if (in_pipe[0] >= 0) { close(in_pipe[0]); close(in_pipe[1]); }
        return;
    }

    if (pid == 0) {
        /* 子进程：重定向标准输入输出到管道 */
        close(out_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(out_pipe[1]);

        if (is_post) {
            close(in_pipe[1]);
            dup2(in_pipe[0], STDIN_FILENO);
            close(in_pipe[0]);
        } else {
            int devnull = open("/dev/null", O_RDONLY);   /* 非 POST 时给个空 stdin */
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }

        /* 关闭所有继承来的 fd：否则 CGI 会一直持有 socket，导致连接无法真正关闭 */
        long max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0) max_fd = 1024;
        for (long fd = 3; fd < max_fd; fd++) {
            close((int)fd);
        }

        /* 按 CGI 规范通过环境变量传递请求信息 */
        if (cookie) {
            setenv("HTTP_COOKIE", cookie, 1);
        }
        setenv("REQUEST_METHOD", method, 1);
        setenv("PATH_INFO", path, 1);
        setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
        setenv("SERVER_PROTOCOL", "HTTP/1.1", 1);
        if (is_post && body) {
            char cl[32];
            snprintf(cl, sizeof(cl), "%zu", strlen(body));
            setenv("CONTENT_LENGTH", cl, 1);
        }

        execl(resolved, resolved, NULL);

        _exit(1);   /* execl 失败才会走到这里 */
    } else {
        /* 父进程：写请求体 → 转发输出 → 回收子进程 */
        close(out_pipe[1]);
        if (is_post) {
            close(in_pipe[0]);
            if (body && strlen(body) > 0) {
                ssize_t w = write(in_pipe[1], body, strlen(body));
                (void)w;
            }
            close(in_pipe[1]);   /* 关闭写端，子进程才能读到 EOF */
        }

        char cgi_output[4096];
        ssize_t n;
        while ((n = read(out_pipe[0], cgi_output, sizeof(cgi_output))) > 0) {
            send(client_fd, cgi_output, n, 0);
        }

        close(out_pipe[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        (void)status;
    }
}


/* 校验用户名：长度合法、非 "."/".."、不含控制字符与文件系统保留字符 */
static int is_valid_username(const char* name) {
    if (!name || name[0] == '\0' || strlen(name) >= name_length) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    for (const char* p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x1F || c == 0x7F) return 0;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return 0;
    }
    return 1;
}

/*
 * POST /login：请求体形如 "name=xxx"。
 * 首次登录会在根目录下创建同名目录，作为该用户的文件空间。
 * 成功时通过 Set-Cookie 下发 session token。
 */
void handle_login(int client_fd, const char* body) {
    char name[256] = {0};
    if (!body || sscanf(body, "name=%255[^&]", name) < 1 || strlen(name) == 0) {
        send_error(client_fd, 400, "Bad Request");
        return;
    }
    url_decode(name);

    if (!is_valid_username(name)) {
        send_error(client_fd, 400, "Invalid username");
        return;
    }

    fs_mkdir(name, NULL);   /* 已存在时自然失败，忽略即可：登录不要求首次创建 */

    const char* token = session_create(name);
    if (!token) {
        send_error(client_fd, 503, "Service Unavailable");
        return;
    }

    /* 生成与 token 一致的过期时间，写入 Set-Cookie 的 Expires 属性 */
    char expire[64];
    time_t t = time(NULL) + 24 * 3600;
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    strftime(expire, sizeof(expire), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);

    char response[buffer_size];
    snprintf(response, sizeof(response),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Set-Cookie: session=%s; Path=/; Expires=%s; HttpOnly; SameSite=Lax\r\n"
             "Connection: close\r\n"
             "\r\n"
             "{\"success\":true,\"message\":\"Login successful\"}",
             token, expire);
    send(client_fd, response, strlen(response), 0);
}


/* GET /logout：销毁会话，并让浏览器立即清除 Cookie（Expires 设为过去时间） */
void handle_logout(int client_fd, const char* cookie) {
    char* token = get_session_token_from_cookie(cookie);
    if (token) {
        session_destroy(token);
        free(token);
    }
    char response[buffer_size];
    snprintf(response, sizeof(response),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Set-Cookie: session=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT\r\n"
             "Connection: close\r\n"
             "\r\n"
             "{\"success\":true,\"message\":\"Logout successful\"}");
    send(client_fd, response, strlen(response), 0);
}

/* 按文件扩展名推断 Content-Type，未知扩展名回落为二进制流 */
static const char* get_content_type(const char* filename) {
    const char* ext = filename ? strrchr(filename, '.') : NULL;
    if (!ext || !ext[1]) return "application/octet-stream";
    ext++;
    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, "css") == 0) return "text/css";
    if (strcasecmp(ext, "js") == 0) return "application/javascript";
    if (strcasecmp(ext, "json") == 0) return "application/json";
    if (strcasecmp(ext, "txt") == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "webp") == 0) return "image/webp";
    if (strcasecmp(ext, "svg") == 0) return "image/svg+xml";
    if (strcasecmp(ext, "ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, "pdf") == 0) return "application/pdf";
    if (strcasecmp(ext, "zip") == 0) return "application/zip";
    if (strcasecmp(ext, "mp3") == 0) return "audio/mpeg";
    if (strcasecmp(ext, "mp4") == 0) return "video/mp4";
    if (strcasecmp(ext, "xml") == 0) return "application/xml";
    return "application/octet-stream";
}


/*
 * 发送一段内存中的文件内容（来自虚拟文件系统），带 Content-Length。
 * 与 send_file_response（磁盘静态文件、chunked 编码）区分开。
 */
static void send_file_content(int client_fd, int status, const char* filename, const void* data, size_t len) {
    char header[buffer_size];
    const char* ct = get_content_type(filename);
    int n = snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "Cache-Control: max-age=3600\r\n"
             "\r\n",
             status, status == 200 ? "OK" : "Created", ct, len);
    if (n <= 0 || (size_t)n >= sizeof(header)) return;
    write(client_fd, header, (size_t)n);
    if (len > 0 && data) write(client_fd, data, len);
}


/* GET /checklogin：返回当前登录态与用户名，供前端页面判断 */
void check_login_status(int client_fd, const char* cookie) {
    char* username = get_username_from_cookie(cookie);
    char response[buffer_size];
    if (username) {
        snprintf(response, sizeof(response),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "{\"loggedIn\":true,\"user\":\"%s\"}",
                 username);
        free(username);
    } else {
        snprintf(response, sizeof(response),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "{\"loggedIn\":false}");
    }
    send(client_fd, response, strlen(response), 0);
}

/* 创建监听 socket：socket → setsockopt → bind → listen */
int socket_new_serverfd(){
    int server_fd = socket(AF_INET,SOCK_STREAM,0);
    if(server_fd < 0){
        perror("socket");
    }

    /* SO_REUSEADDR：允许重启后立即复用端口，避免 TIME_WAIT 阻塞 */
    int opt = 1;
    if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0){
        perror("设置 socket 选项失败");
    }

    /* 禁用 Nagle 算法，降低小包延迟 */
    if(setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0){
        perror("设置 TCP_NODELAY 选项失败");
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;   /* 监听所有网卡 */
    server_addr.sin_port = htons(PORT);

    if(bind(server_fd,(struct sockaddr *)&server_addr,sizeof(server_addr)) < 0){
        perror("bind");
    }

    if(listen(server_fd,backlog) < 0){
        perror("listen");
    }

    printf("服务器启动成功，监听端口:%d\n等待客户端连接...", PORT);
    return server_fd;
}


/*
 * 单个连接的完整生命周期（由线程池工作线程调用）：
 *   接收请求 → 解析 method/path/body/cookie → 路由 → 发送响应 → 关闭连接
 * 采用短连接模型：一个请求一个响应，随后关闭。
 */
void* execute_connect(void* arguments){
    struct message_of_client* client = (struct message_of_client*)arguments;
    int client_fd = client->client_fd;
    struct sockaddr_in client_addr = client->client_addr;   /* 备用，日志中直接用 client->client_addr */

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client->client_addr.sin_addr, ip_str, sizeof(ip_str));
    printf("线程 %lu: 新的客户端连接 from %s:%d\n",
           (unsigned long)pthread_self(),
           ip_str,
           ntohs(client->client_addr.sin_port));

    /* SO_KEEPALIVE：探测对端是否已死，及时回收连接 */
    int opt = 1;
    if(setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0){
        perror("为客户端连接设置 SO_KEEPALIVE 选项失败");
    }

    /* 响应通常很小，禁用 Nagle 可显著降低交互延迟 */
    if(setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0){
        perror("为客户端连接设置 TCP_NODELAY 选项失败");
    }

    /* 文件系统已在 main 中单次挂载，这里不需要（也不应该）重复挂载 */

    char buffer[buffer_size];
    ssize_t bytes_received;

    /* 清空接收缓冲区，避免残留数据干扰解析 */
    memset(buffer, 0, sizeof(buffer));

    /* 一次性读取整个请求：假设请求（含头部）不超过 4KB */
    bytes_received = recv(client_fd, buffer, sizeof(buffer), 0);
    if(bytes_received <= 0){
        close(client_fd);
        free(client);
        return NULL;
    }
    buffer[bytes_received] = '\0';

    char method[10];
    char path[256];
    char body[buffer_size] = {0};

    /* 定位头部与正文的分界 "\r\n\r\n" */
    char* body_start = strstr(buffer, "\r\n\r\n");
    size_t body_in_buf = 0;
    long content_length = -1;
    if(body_start) {
        body_start += 4;
        body_in_buf = (size_t)((buffer + bytes_received) - body_start);   /* 已收到的正文长度 */
        if (body_in_buf > sizeof(body)) body_in_buf = sizeof(body);
        strncpy(body, body_start, sizeof(body) - 1);
        body[sizeof(body)-1] = '\0';
    }
    char* cl = strstr(buffer, "Content-Length:");
    if (cl) sscanf(cl, "Content-Length: %ld", &content_length);

    /* 提取 Cookie 头：截到行尾，直接指向 buffer 内部 */
    char* cookie_start = strstr(buffer, "Cookie:");
    const char* cookie = NULL;
    if(cookie_start) {
        cookie_start += 7;
        while(*cookie_start == ' ') cookie_start++;
        char* end = strstr(cookie_start, "\r\n");
        if(end) *end = '\0';
        cookie = cookie_start;
    }

    method[0] = '\0';
    path[0] = '\0';
    deal_with_request(buffer, method, path, body);
    if (method[0] == '\0' || path[0] == '\0') {
        send_error(client_fd, 400, "Bad Request");
        close(client_fd);
        free(client);
        return NULL;
    }
    /* 剥离查询串：后续路由只关心路径部分 */
    { char* q = strchr(path, '?'); if (q) *q = '\0'; }

    /* 登录/登出/状态查询优先匹配：这些接口不依赖文件路径 */
    if(strcmp(method, "POST") == 0 && strcmp(path, "/login") == 0) {
        handle_login(client_fd, body);
    } else if(strcmp(method, "GET") == 0 && strcmp(path, "/checklogin") == 0) {
        check_login_status(client_fd, cookie);
    } else if(strcmp(method, "GET") == 0 && strcmp(path, "/logout") == 0) {
        handle_logout(client_fd, cookie);
    } else if (strstr(path, "/cgi/") == path) {
        /* 走 CGI 分支，交给外部可执行程序处理 */
        handle_cgi_request(client_fd, buffer, method, path, body, cookie);
    } else if(strcmp(method,"GET") == 0 && strcmp(path,"/") == 0){
        /* 首页：直接返回静态 HTML */
        char file_path[512];
        snprintf(file_path,sizeof(file_path),"%s/index.html",root);
        send_file_response(client_fd,file_path);
    } else {
        /* 其余请求都需要登录：以 cookie 中的用户名作为该用户的目录名 */
        char* username = get_username_from_cookie(cookie);
        if(!username || strlen(username) == 0) {
            send_error(client_fd, 401, "Unauthorized");
            if(username) free(username);
        } else {
            char file_path[512];
            int error_sent = 0;   /* 标记是否已发送错误响应，避免后续重复写入 */

            if(strcmp(method,"GET") == 0){
                if(strcmp(path,"/list") == 0) {
                    /* 列出当前用户目录下的文件 */
                    char list_buffer[buffer_size];
                    int len = fs_list_director_to_buf(username, list_buffer, sizeof(list_buffer));
                    if (len < 0) len = 0;
                    list_buffer[len] = '\0';
                    send_response(client_fd, 200, list_buffer);
                } else if(strcmp(path,"/members") == 0) {
                    /* 列出根目录成员（所有用户） */
                    char list_buffer[buffer_size];
                    int len = fs_list_member_to_buf(list_buffer, sizeof(list_buffer));
                    if (len < 0) len = 0;
                    list_buffer[len] = '\0';
                    send_response(client_fd, 200, list_buffer);
                } else {
                    /* 读取单个文件：路径去掉开头的 '/' 即为文件名 */
                    char* file_name = path + 1;
                    if(strlen(file_name) > 0) {
                        char file_content[buffer_size];
                        int result = fs_read_file(file_name, username, file_content, sizeof(file_content), 0);
                        if(result >= 0) {
                            send_file_content(client_fd, 200, file_name, file_content, (size_t)result);
                        } else {
                            send_error(client_fd, 404, "File Not Found");
                            error_sent = 1;
                        }
                    } else {
                        send_error(client_fd, 400, "Bad Request");
                        error_sent = 1;
                    }
                }
            } else if(strcmp(method,"POST") == 0) {
                /* 创建空文件，已存在则 409 */
                char* file_name = path + 1;
                if(strlen(file_name) > 0) {
                    int result = fs_create_file(file_name, username);
                    if(result == 0) {
                        send_response(client_fd, 201, "File created successfully");
                    } else {
                        send_error(client_fd, 409, "File Already Exists");
                        error_sent = 1;
                    }
                } else {
                    send_error(client_fd, 400, "Bad Request");
                    error_sent = 1;
                }
            } else if(strcmp(method,"PUT") == 0) {
                /* 上传/覆盖文件内容 */
                char* file_name = path + 1;
                if(strlen(file_name) == 0) {
                    send_error(client_fd, 400, "Bad Request");
                    error_sent = 1;
                } else {
                    size_t put_len;
                    const void* put_body;
                    if (content_length >= 0) {
                        /* 有 Content-Length：以它为准，可能还有剩余数据留在 socket 中 */
                        put_len = (size_t)content_length;
                        put_body = body_start;
                        if (put_len == 0) {
                            fs_create_file(file_name, username);   /* 空文件上传 */
                            send_response(client_fd, 200, "File updated successfully");
                        } else if ((unsigned long)content_length > 4 * buffer_size) {
                            send_error(client_fd, 413, "Payload Too Large");
                            error_sent = 1;
                        } else {
                            if (put_len > body_in_buf) {
                                /* 正文比首次 recv 收到的多：先写已收到部分，再循环收剩余 */
                                fs_create_file(file_name, username);
                                int wr = fs_write_file(file_name, username, put_body, body_in_buf, 0);
                                if (wr < 0) {
                                    send_error(client_fd, 500, "Write failed");
                                    error_sent = 1;
                                } else {
                                    size_t offset = body_in_buf;
                                    char chunk[buffer_size];
                                    while (offset < put_len) {
                                        size_t to_read = put_len - offset;
                                        if (to_read > sizeof(chunk)) to_read = sizeof(chunk);
                                        ssize_t n = recv(client_fd, chunk, to_read, 0);
                                        if (n <= 0) break;
                                        wr = fs_write_file(file_name, username, chunk, (size_t)n, offset);
                                        if (wr < 0) break;
                                        offset += (size_t)n;
                                    }
                                    if (offset == put_len)
                                        send_response(client_fd, 200, "File updated successfully");
                                    else {
                                        send_error(client_fd, 500, "Upload incomplete");
                                        error_sent = 1;
                                    }
                                }
                            } else {
                                /* 正文完整包含在首次 recv 中：一次写入即可 */
                                fs_create_file(file_name, username);
                                int result = fs_write_file(file_name, username, put_body, put_len, 0);
                                if (result >= 0)
                                    send_response(client_fd, 200, "File updated successfully");
                                else {
                                    send_error(client_fd, 500, "Write failed");
                                    error_sent = 1;
                                }
                            }
                        }
                    } else {
                        /* 无 Content-Length：退化为把解析出的 body 当作全部内容 */
                        put_len = strlen(body);
                        put_body = body;
                        if (put_len > 0) {
                            fs_create_file(file_name, username);
                            int result = fs_write_file(file_name, username, put_body, put_len, 0);
                            if (result >= 0)
                                send_response(client_fd, 200, "File updated successfully");
                            else {
                                send_error(client_fd, 500, "Write failed");
                                error_sent = 1;
                            }
                        } else {
                            send_error(client_fd, 400, "Bad Request");
                            error_sent = 1;
                        }
                    }
                }
            } else if(strcmp(method,"DELETE") == 0) {
                char* file_name = path + 1;
                if(strlen(file_name) > 0) {
                    int result = fs_delete_file(file_name, username);
                    if(result == 0) {
                        send_response(client_fd, 200, "File deleted successfully");
                    } else {
                        send_error(client_fd, 404, "File Not Found");
                        error_sent = 1;
                    }
                } else {
                    send_error(client_fd, 400, "Bad Request");
                    error_sent = 1;
                }
            } else {
                send_error(client_fd, 501, "Not Implemented");
                error_sent = 1;
            }

            (void)error_sent;   /* 仅用于标记，响应已统一发送完毕 */
            free(username);
        }
    }

    close(client_fd);
    free(client);

    return NULL;
}

/*
 * 解析请求行：把 "GET /path HTTP/1.1" 拆成 method 与 path。
 * 顺带打印 Host 头与请求体，便于调试。
 * 注意：这里直接在 request 缓冲区上写 '\0'，调用方之后的 strstr 会受到影响，
 * 因此所有需要的头部（Cookie / Content-Length）必须在此之前提取完毕。
 */
void deal_with_request(char* request,char* method,char* path, char* body){
    char* host_header = strstr(request, "Host:");
    if(host_header) {
        char host[256];
        sscanf(host_header, "Host: %s", host);
        printf("Host: %s\n", host);
    }

    char* end_of_line = strstr(request,"\r\n");
    if(end_of_line != NULL){
        *end_of_line = '\0';   /* 截断到请求行末，便于 sscanf 只解析首行 */
    }

    sscanf(request,"%9s %255s",method,path);
    printf("请求方法：%s; 请求路径：%s\n", method, path);
    if(body && strlen(body) > 0) {
        printf("请求体：%s\n", body);
    }
}

/* 发送纯文本错误响应，正文即状态描述 */
void send_error(int client_fd,int eerrno,const char* status){
    char buffer[buffer_size];
    size_t body_len = strlen(status);
    size_t head_len = snprintf(buffer, sizeof(buffer),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: text/plain; charset=utf-8\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n"
             "%s",
             eerrno, status, body_len, status);
    ssize_t n = write(client_fd, buffer, head_len);
    (void)n;
}

/*
 * 发送磁盘上的静态文件（首页 index.html）。
 * 使用 chunked 传输编码，无需预先知道文件大小，可边读边发。
 * 分块格式：每块 "<十六进制长度>\r\n<数据>\r\n"，以 "0\r\n\r\n" 结束。
 */
void send_file_response(int client_fd,char* file_path){
    struct stat file_stat;

    if (stat(file_path, &file_stat) < 0 || !S_ISREG(file_stat.st_mode)) {
        send_error(client_fd, 404, "Not Found");
        return;
    }

    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
        send_error(client_fd, 404, "Not Found");
        return;
    }

    char header[buffer_size] = {0};
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html; charset=utf-8\r\n"
             "Transfer-Encoding: chunked\r\n"
             "Connection: close\r\n"
             "Cache-Control: max-age=86400\r\n"
             "\r\n");
    write(client_fd, header, strlen(header));

    char file_buf[buffer_size] = {0};
    size_t bytes;
    while ((bytes = fread(file_buf, 1, sizeof(file_buf), fp)) > 0) {
        char chunk_header[32];
        snprintf(chunk_header, sizeof(chunk_header), "%zx\r\n", bytes);
        write(client_fd, chunk_header, strlen(chunk_header));
        write(client_fd, file_buf, bytes);
        write(client_fd, "\r\n", 2);
    }
    write(client_fd, "0\r\n\r\n", 5);   /* 结束标记 */

    fclose(fp);
}

/* 发送一段文本消息（同样使用 chunked 编码），用于文件操作的成功提示 */
void send_response(int client_fd, int status, const char* message){
    char header[buffer_size] = {0};
    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: text/plain; charset=utf-8\r\n"
             "Transfer-Encoding: chunked\r\n"
             "Connection: close\r\n"
             "\r\n",
             status, status == 200 ? "OK" : (status == 201 ? "Created" : "Internal Server Error"));
    write(client_fd, header, strlen(header));

    if(message && strlen(message) > 0) {
        size_t len = strlen(message);
        char chunk_header[32];
        snprintf(chunk_header, sizeof(chunk_header), "%zx\r\n", len);
        write(client_fd, chunk_header, strlen(chunk_header));
        write(client_fd, message, len);
        write(client_fd, "\r\n", 2);
    }
    write(client_fd, "0\r\n\r\n", 5);
}
