/*
 * nagle_demo.c —— 一键复现 Nagle + Delayed ACK 的 40ms 互锁
 *
 *   编译: gcc -O0 -Wall -o nagle_demo nagle_demo.c
 *   运行: ./nagle_demo
 *
 * 本程序 fork 出子进程当服务端，父进程当客户端，自动跑完 4 组对照实验：
 *
 *   A. split   + 服务端延迟确认  →  预期 ~40ms  ← 踩雷
 *   B. split   + TCP_NODELAY     →  预期 ~0ms   ← 解法1：发送侧
 *   C. merged  + 服务端延迟确认  →  预期 ~0ms   ← 解法2：应用层缓冲合并
 *   D. split   + TCP_QUICKACK    →  预期 ~0ms   ← 解法3：接收侧（one-shot!）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ctype.h>

#ifndef TCP_QUICKACK
#define TCP_QUICKACK 12
#endif

#define ROUNDS 5
#define BASE_PORT 9100

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* ---------------- 服务端 ---------------- */
/* ack_mode: 0 = 关 quickack（走延迟确认）  1 = 开 quickack（立即 ACK） */
static void server_main(int port, int ack_mode)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);

    /* 在 listening socket 上设置，会被 accept 出来的连接继承。
     * 这一步很关键：如果只在 accept 之后设置，数据包有可能先到、
     * 内核以新连接默认的 quickack(quick=16) 秒回 ACK，40ms 就消失了。 */
    int v_inherit = ack_mode;
    setsockopt(lfd, IPPROTO_TCP, TCP_QUICKACK, &v_inherit, sizeof(v_inherit));

    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); exit(1); }
    if (listen(lfd, 64) < 0) { perror("listen"); exit(1); }

    while (1) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;

        int v = ack_mode;                     /* 1 = 立即 ACK, 0 = 延迟 ACK */
        setsockopt(cfd, IPPROTO_TCP, TCP_QUICKACK, &v, sizeof(v));

        /* 发一个 ready 信号：保证 client 开始计时发包时，服务端的
         * setsockopt 已经生效，彻底消除上面的竞态 */
        write(cfd, "G", 1);

        char buf[64];
        /* 第一次读：拿到第一个小包，刻意不回响应（否则 ACK 被捎带，实验失效） */
        if (read(cfd, buf, 1) <= 0) { close(cfd); continue; }

        /* TCP_QUICKACK 是一次性的，每次 recv 后必须重设才稳 */
        setsockopt(cfd, IPPROTO_TCP, TCP_QUICKACK, &v, sizeof(v));

        /* 第二次读：阻塞等那个被 Nagle 扣住的第二个小包 */
        if (read(cfd, buf + 1, 1) > 0)
            write(cfd, "P", 1);               /* 收齐才回 */
        close(cfd);
    }
}

/* ---------------- 客户端 ---------------- */
/* mode: 0=split(两次小写) 1=merged(一次写完) 2=split+TCP_NODELAY */
static void client_rounds(int port, int mode, const char *label)
{
    printf("\n--- %s ---\n", label);
    double sum = 0.0;
    int cnt = 0;

    for (int i = 0; i < ROUNDS; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (mode == 2) {
            int on = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        }
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);

        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
            perror("connect"); close(fd); return;
        }

        /* 等服务端 ready，确保它的 setsockopt 已生效再开始计时 */
        char go[4];
        if (read(fd, go, 1) <= 0) { close(fd); continue; }

        double t0 = now_ms();

        if (mode == 1) {
            write(fd, "HI", 2);               /* 合并成一次写 */
        } else {
            write(fd, "H", 1);                /* 先 header */
            write(fd, "I", 1);                /* 再 body —— 雷区 */
        }

        char r[16];
        ssize_t n = read(fd, r, sizeof(r));
        double cost = now_ms() - t0;

        if (n > 0) { printf("  round %d: %7.2f ms\n", i, cost); sum += cost; cnt++; }
        else       { printf("  round %d: %7.2f ms (no resp)\n", i, cost); }

        close(fd);
        usleep(150 * 1000);                   /* 拉开轮次，避免互相干扰 */
    }
    if (cnt) printf("  >>> 平均 %.2f ms\n", sum / cnt);
}

int main(int argc, char **argv)
{
    /* argv[1] 可选：a/b/c/d 只跑指定分组（配合抓包用） */
    char only = (argc > 1) ? (char)tolower(argv[1][0]) : 0;
    int only_idx = (only >= 'a' && only <= 'd') ? (only - 'a') : -1;

    printf("=========================================================\n");
    printf(" Nagle + Delayed ACK 互锁实验   (rounds=%d)\n", ROUNDS);
    printf("=========================================================");

    struct { int port; int cmode; int ack_mode; const char *label; } cases[] = {
        { BASE_PORT + 0, 0, 0, "A. split     + 延迟确认(默认)   ← 踩雷，预期 ~40ms" },
        { BASE_PORT + 1, 2, 0, "B. split     + TCP_NODELAY      ← 解法1 发送侧" },
        { BASE_PORT + 2, 1, 0, "C. merged    + 延迟确认          ← 解法2 应用层合并" },
        { BASE_PORT + 3, 0, 1, "D. split     + TCP_QUICKACK      ← 解法3 接收侧" },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (only_idx >= 0 && (int)i != only_idx) continue;
        pid_t pid = fork();
        if (pid == 0) {
            server_main(cases[i].port, cases[i].ack_mode);
            _exit(0);
        }
        usleep(250 * 1000);                   /* 等子进程 listen 起来 */
        client_rounds(cases[i].port, cases[i].cmode, cases[i].label);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }

    printf("\n=========================================================\n");
    printf(" 结论：A 明显慢一截 → 那个差值就是延迟确认的 40ms。\n");
    printf("       它不是网络慢，是发送方等 ACK、接收方等数据，互锁。\n");
    printf("=========================================================\n");
    return 0;
}
