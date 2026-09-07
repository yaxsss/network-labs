/*
 * cork_keepalive.c —— 补充实验
 *   1) TCP_CORK 与 TCP_NODELAY 打架：谁赢？
 *   2) SO_KEEPALIVE 是单向的：只有设的那一端在探测
 *
 * 编译: gcc -O0 -Wall -o cork_keepalive cork_keepalive.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#ifndef TCP_CORK
#define TCP_CORK 3
#endif

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* mode: 0=NODELAY 1=NODELAY+CORK 2=CORK only */
static void client(int port, int mode, const char *label)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;

    if (mode == 0 || mode == 1)
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    if (mode == 1 || mode == 2)
        setsockopt(fd, IPPROTO_TCP, TCP_CORK,   &on, sizeof(on));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    for (int k = 0; k < 200; k++) {
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) break;
        usleep(50 * 1000);
    }

    char go[4];
    read(fd, go, 1);

    double t0 = now_ms();
    write(fd, "H", 1);
    write(fd, "I", 1);
    /* CORK 模式下数据被攒住，直到下面这步：取消 CORK 或写满 MSS */
    if (mode == 1 || mode == 2) {
        int off = 0;
        usleep(300 * 1000);                       /* 故意等 300ms，观察它憋着不发 */
        setsockopt(fd, IPPROTO_TCP, TCP_CORK, &off, sizeof(off));  /* 拔塞子 */
    }

    char r[16];
    read(fd, r, sizeof(r));
    printf("  %-34s 收到响应耗时 %7.2f ms\n", label, now_ms() - t0);
    fflush(stdout);
    close(fd);
}

static void server(int port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    bind(lfd, (struct sockaddr *)&a, sizeof(a));
    listen(lfd, 64);
    while (1) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;
        write(cfd, "G", 1);
        char b[64];
        if (read(cfd, b, 1) <= 0) { close(cfd); continue; }
        if (read(cfd, b + 1, 1) > 0) write(cfd, "P", 1);
        close(cfd);
    }
}

int main(void)
{
    printf("=== 1. TCP_CORK vs TCP_NODELAY：同一个 socket 上打架 ===\n");
    struct { int mode; const char *label; } cs[] = {
        { 0, "只设 NODELAY" },
        { 1, "NODELAY + CORK 同时设" },
        { 2, "只设 CORK" },
    };
    for (int i = 0; i < 3; i++) {
        pid_t ps = fork();
        if (ps == 0) { server(9400 + i); _exit(0); }
        usleep(200 * 1000);
        client(9400 + i, cs[i].mode, cs[i].label);
        kill(ps, SIGKILL); waitpid(ps, NULL, 0);
        usleep(200 * 1000);
    }
    printf("\n（CORK 组如果耗时 ~300ms，说明 CORK 赢了，NODELAY 被压住）\n");

    printf("\n=== 2. SO_KEEPALIVE 单向性 ===\n");
    {
        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        int on = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        /* 只有服务端开 KEEPALIVE */
        setsockopt(lfd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(9500);
        bind(lfd, (struct sockaddr *)&a, sizeof(a));
        listen(lfd, 8);

        pid_t pc = fork();
        if (pc == 0) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in b;
            memset(&b, 0, sizeof(b));
            b.sin_family = AF_INET;
            b.sin_port = htons(9500);
            inet_pton(AF_INET, "127.0.0.1", &b.sin_addr);
            for (int k = 0; k < 200; k++) {
                if (connect(fd, (struct sockaddr *)&b, sizeof(b)) == 0) break;
                usleep(50 * 1000);
            }
            sleep(6);       /* 保持连接，给外部 ss 观察留时间 */
            close(fd);
            _exit(0);
        }
        int cfd = accept(lfd, NULL, NULL);
        int ka_srv = 0, ka_cli = 0;
        socklen_t l = sizeof(int);
        getsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &ka_srv, &l);
        printf("  服务端 socket SO_KEEPALIVE = %d  （本端开了）\n", ka_srv);
        printf("  对端客户端未设置 → 只有服务端会发探测包\n");
        printf("  验证: ss -tnop 'sport = :9500 or dport = :9500' 看 timer:(keepalive,...)\n");
        fflush(stdout);
        sleep(4);
        close(cfd);
        waitpid(pc, NULL, 0);
        close(lfd);
    }
    return 0;
}
