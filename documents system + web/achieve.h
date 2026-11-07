/*
 * achieve.h —— Web 服务器业务层接口
 *
 * 本层负责 HTTP 请求的语义处理：静态文件、RESTful 文件操作、
 * CGI 动态调用、用户登录与 Session 管理。
 * 服务器主循环（epoll + 线程池）在 web2.c，连接处理入口是 execute_connect。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stddef.h>
#define PORT 8080
#define buffer_size 4096
#define root "./www"           /* 静态资源根目录 */
#define backlog 128            /* listen 队列长度 */

/* 线程池任务：把一个已 accept 的连接连同对端地址交给工作线程 */
struct message_of_client{
int client_fd;
struct sockaddr_in client_addr;
};

int socket_new_serverfd();

void* execute_connect(void* arguments);

char* get_username_from_cookie(const char* cookie);
void handle_login(int client_fd, const char* body);
void check_login_status(int client_fd, const char* cookie);
void handle_logout(int client_fd, const char* cookie);

void deal_with_request(char* buffer,char* method,char* path, char* body);

void send_error(int client_fd,int eerrno,const char* status);

void send_file_response(int client_fd,char* file_path);

void send_response(int client_fd, int status, const char* message);
