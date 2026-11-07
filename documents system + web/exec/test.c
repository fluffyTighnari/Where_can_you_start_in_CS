/*
 * test.c —— CGI 测试程序
 *
 * 被服务器以 fork + exec 方式调用，约定：
 *   1. 自行输出完整的 HTTP 响应头（Content-Type + 空行）
 *   2. 请求信息通过环境变量传入（CGI/1.1 规范）
 *   3. 输出直接经管道转发给客户端
 * 作用是把服务器设置的环境变量原样回显，便于验证 CGI 通路是否打通。
 */
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    /* 头部：必须与正文之间空一行 */
    printf("Content-Type: text/plain; charset=utf-8\r\n");
    printf("\r\n");

    /* 正文 */
    printf("CGI Test Program\n");
    printf("================\n");

    printf("Environment Variables:\n");

    const char* cookie = getenv("HTTP_COOKIE");
    if (cookie) {
        printf("HTTP_COOKIE: %s\n", cookie);
    } else {
        printf("HTTP_COOKIE: (not set)\n");
    }

    const char* method = getenv("REQUEST_METHOD");
    if (method) {
        printf("REQUEST_METHOD: %s\n", method);
    } else {
        printf("REQUEST_METHOD: (not set)\n");
    }

    const char* path = getenv("PATH_INFO");
    if (path) {
        printf("PATH_INFO: %s\n", path);
    } else {
        printf("PATH_INFO: (not set)\n");
    }

    return 0;
}
