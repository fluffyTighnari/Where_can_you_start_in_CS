/*
 * web2.c —— Web 服务器主循环（epoll + 线程池）
 *
 * 并发模型是「Reactor + 工作线程池」：
 *   主线程只做 epoll_wait 与 accept，绝不处理业务；
 *   新连接封装成任务投入队列，由 4 个工作线程各自执行 execute_connect。
 *
 * 关键设计点：
 *   - 队列满时直接拒绝连接（回退给客户端后关闭），避免无界内存增长
 *   - SIGINT 只置位 g_shutdown_requested，由主循环检查后优雅关闭
 *   - SIGPIPE 必须忽略：向已关闭的连接 write 不应杀死整个进程
 */
#include "achieve.h"
#include "interface.h"
#include <signal.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <fcntl.h>

#define THREAD_POOL_SIZE 4    /* 工作线程数 */
#define MAX_QUEUE_SIZE 128    /* 任务队列上限，超过则拒绝新连接 */
#define MAX_EVENTS 1024       /* 单次 epoll_wait 最多返回的事件数 */

int server_fd_global = -1;
volatile sig_atomic_t g_shutdown_requested = 0;   /* 信号处理函数中只写这个标志 */

/* 任务队列节点：一个待处理的客户端连接 */
typedef struct task_node {
    struct message_of_client* client;
    struct task_node* next;
} task_node;

/* 线程池：任务队列 + 工作线程 + 互斥锁/条件变量 */
typedef struct thread_pool {
    task_node* queue_head;
    task_node* queue_tail;
    pthread_t threads[THREAD_POOL_SIZE];
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int queue_size;
    int shutdown;
} thread_pool;

thread_pool* g_thread_pool = NULL;

/*
 * 工作线程主循环：阻塞在条件变量上等任务，取到任务后释放锁再处理。
 * 退出条件：收到了 shutdown 通知且队列已清空。
 */
static void* worker_thread(void* arg) {
    thread_pool* pool = (thread_pool*)arg;

    while (1) {
        pthread_mutex_lock(&pool->mutex);

        /* 用 while 而非 if 等待：防止虚假唤醒 */
        while (pool->queue_size == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->cond, &pool->mutex);
        }

        if (pool->shutdown && pool->queue_size == 0) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        /* 从队首摘下一个任务 */
        task_node* task = pool->queue_head;
        if (task) {
            pool->queue_head = task->next;
            if (pool->queue_tail == task) {
                pool->queue_tail = NULL;
            }
            pool->queue_size--;
        }

        pthread_mutex_unlock(&pool->mutex);

        /* 业务处理在锁外进行：execute_connect 可能执行很久 */
        if (task) {
            execute_connect(task->client);
            free(task);
        }
    }

    return NULL;
}

/* 初始化线程池：建队列 → 初始化同步原语 → 拉起所有工作线程 */
thread_pool* init_thread_pool() {
    thread_pool* pool = (thread_pool*)malloc(sizeof(thread_pool));
    if (!pool) {
        perror("malloc thread pool");
        return NULL;
    }

    pool->queue_head = NULL;
    pool->queue_tail = NULL;
    pool->queue_size = 0;
    pool->shutdown = 0;

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);

    /* 逐个创建线程；任一失败则取消已创建的线程并释放资源 */
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0) {
            perror("pthread_create");
            for (int j = 0; j < i; j++) {
                pthread_cancel(pool->threads[j]);
            }
            free(pool);
            return NULL;
        }
    }

    return pool;
}

/*
 * 提交任务（生产者端）：把连接挂到队尾并唤醒一个工作线程。
 * 队列已满返回 -1，调用方需自行关闭该连接。
 */
int add_task(thread_pool* pool, struct message_of_client* client) {
    if (!pool || !client) {
        return -1;
    }

    task_node* task = (task_node*)malloc(sizeof(task_node));
    if (!task) {
        perror("malloc task node");
        return -1;
    }
    task->client = client;
    task->next = NULL;

    pthread_mutex_lock(&pool->mutex);

    if (pool->queue_size >= MAX_QUEUE_SIZE) {
        pthread_mutex_unlock(&pool->mutex);
        free(task);
        return -1;
    }

    if (pool->queue_tail) {
        pool->queue_tail->next = task;
    } else {
        pool->queue_head = task;
    }
    pool->queue_tail = task;
    pool->queue_size++;

    pthread_cond_signal(&pool->cond);   /* 只唤醒一个线程即可 */
    pthread_mutex_unlock(&pool->mutex);
    return 0;
}

/*
 * 销毁线程池：置 shutdown 并广播唤醒所有线程，等它们退出后清理残留任务。
 * 工作线程会先把队列中的任务处理完才退出。
 */
void destroy_thread_pool(thread_pool* pool) {
    if (!pool) {
        return;
    }

    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->cond);   /* 必须广播，否则只有一个线程能感知退出 */
    pthread_mutex_unlock(&pool->mutex);

    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    /* 理论上队列已空，但异常路径下仍需释放残留任务 */
    task_node* task = pool->queue_head;
    while (task) {
        task_node* next = task->next;
        free(task->client);
        free(task);
        task = next;
    }

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);

    free(pool);
}

/* SIGINT 处理函数：只置标志位（异步信号安全），真正的清理在主循环里做 */
static void signal_handler(int sig) {
    if (sig == SIGINT) {
        g_shutdown_requested = 1;
    }
}

int main(){
    int server_fd,client_fd;
    struct sockaddr_in client_addr;
    socklen_t addr_size = sizeof(client_addr);

    /* 注册信号：SIGINT 用于优雅退出；SIGPIPE 忽略以免写入已断开连接时进程被杀 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* 挂载虚拟文件系统：整个服务生命周期只挂载一次 */
    if(fs_mount("./disk.img") != 0) {
        printf("文件系统挂载失败，请先创建 disk.img 镜像\n");
        return 1;
    }
    printf("文件系统挂载成功\n");

    /* 创建线程池 */
    g_thread_pool = init_thread_pool();
    if (!g_thread_pool) {
        printf("线程池初始化失败\n");
        fs_unmount();
        return 1;
    }
    printf("线程池初始化成功，工作线程数：%d\n", THREAD_POOL_SIZE);

    server_fd = socket_new_serverfd();
    server_fd_global = server_fd;

    /* 创建 epoll 实例（Reactor 核心） */
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        destroy_thread_pool(g_thread_pool);
        fs_unmount();
        return 1;
    }

    /* 把监听 socket 注册到 epoll，关注可读事件（即新连接到达） */
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
        perror("epoll_ctl add server fd");
        close(epoll_fd);
        destroy_thread_pool(g_thread_pool);
        fs_unmount();
        return 1;
    }

    struct epoll_event events[MAX_EVENTS];

    /* 事件循环：500ms 超时是为了定期检查 g_shutdown_requested */
    while(!g_shutdown_requested){
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 500);
        if (nfds < 0) {
            if (errno == EINTR) continue;   /* 被信号打断，继续等待 */
            perror("epoll_wait");
            continue;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_fd) {
                client_fd = accept(server_fd,(struct sockaddr*)&client_addr,&addr_size);
                if(client_fd < 0){
                    if (errno == EINTR || errno == EAGAIN) continue;
                    perror("accept");
                    continue;
                }

                /* 连接信息封装成任务，交给线程池处理 */
                struct message_of_client* client = (struct message_of_client*)malloc(sizeof(struct message_of_client));
                if(client == NULL){
                    perror("malloc");
                    close(client_fd);
                    continue;
                }
                client->client_addr = client_addr;
                client->client_fd = client_fd;

                if (add_task(g_thread_pool, client) != 0) {
                    perror("add_task");
                    free(client);
                    close(client_fd);   /* 队列满：拒绝服务 */
                    continue;
                }
                printf("新客户端连接已加入任务队列\n");
            }
        }
    }

    /* 优雅关闭：先停止接收新连接，再等线程池清空，最后卸载文件系统 */
    printf("\n收到 SIGINT，正在优雅退出...\n");
    close(server_fd);
    server_fd_global = -1;
    destroy_thread_pool(g_thread_pool);
    g_thread_pool = NULL;
    close(epoll_fd);
    fs_unmount();
    printf("已清理完毕，退出。\n");
    return 0;
}
