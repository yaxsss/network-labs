/*
 * option_matrix.c —— TCP 选项的"作用域"实测矩阵
 *
 * 核心问题：服务端设 TCP_NODELAY、客户端没设，会怎样？
 *          TCP_QUICKACK 设在发送方，有用吗？
 *
 * 编译: gcc -O0 -Wall -o option_matrix option_matrix.c
 * 运行: ./option_matrix
 *
 * 场景固定：客户端 write("H"); write("I");  （write-write-read，雷区写法）
 *          服务端收齐两个字节才回响应（ACK 不捎带）
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

#ifndef TCP_QUICKACK
#define TCP_QUICKACK 12
#endif

#define ROUNDS 4
#define BASE   9300

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* ---------------- 服务端 ---------------- */
/* srv_nodelay: 是否给响应 socket 设 TCP_NODELAY
 * ack_mode   : 1 = quickack（立即回 ACK） 0 = 延迟确认 */
static void server(int port, int srv_nodelay, int ack_mode)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    /* 必须在 listening socket 上设，被 accept 的连接继承（否则新连接
     * 默认带 quickack 额度，前 16 个包秒回，现象消失） */
    if (srv_nodelay)
        setsockopt(lfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    { int v = ack_mode; setsockopt(lfd, IPPROTO_TCP, TCP_QUICKACK, &v, sizeof(v)); }

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); exit(1); }
    if (listen(lfd, 64) < 0) { perror("listen"); exit(1); }

    while (1) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;

        { int v = ack_mode; setsockopt(cfd, IPPROTO_TCP, TCP_QUICKACK, &v, sizeof(v)); }

        write(cfd, "G", 1);                 /* ready 信号，消除时序竞态 */

        char buf[64];
        if (read(cfd, buf, 1) <= 0) { close(cfd); continue; }

        { int v = ack_mode; setsockopt(cfd, IPPROTO_TCP, TCP_QUICKACK, &v, sizeof(v)); }

        if (read(cfd, buf + 1, 1) > 0)
            write(cfd, "P", 1);             /* 收齐才回，ACK 未被捎带 */
        close(cfd);
    }
}

/* ---------------- 客户端 ---------------- */
/* cli_nodelay : 是否 TCP_NODELAY（解开本端 Nagle）
 * cli_quickack: 是否 TCP_QUICKACK（预期无效——ACK 是服务端发的） */
static void run_client(int port, int cli_nodelay, int cli_quickack,
                       const char *label)
{
    printf("\n--- %s ---\n", label);
    double sum = 0; int cnt = 0;

    for (int i = 0; i < ROUNDS; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (cli_nodelay) {
            int on = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        }
        if (cli_quickack) {
            int on = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &on, sizeof(on));
        }
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);

        /* 重试 connect，等服务端 listen */
        int ok = 0;
        for (int k = 0; k < 200; k++) {
            if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) { ok = 1; break; }
            usleep(50 * 1000);
        }
        if (!ok) { close(fd); printf("  connect failed\n"); return; }

        char go[4];
        if (read(fd, go, 1) <= 0) { close(fd); continue; }   /* 等 ready */

        double t0 = now_ms();
        write(fd, "H", 1);
        write(fd, "I", 1);

        char r[16];
        ssize_t n = read(fd, r, sizeof(r));
        double cost = now_ms() - t0;

        printf("  round %d: %7.2f ms\n", i, cost);
        if (n > 0) { sum += cost; cnt++; }
        close(fd);
        usleep(150 * 1000);
    }
    if (cnt) printf("  >>> 平均 %.2f ms\n", sum / cnt);
    fflush(stdout);
}

int main(void)
{
    printf("========================================================\n");
    printf(" TCP 选项作用域矩阵（客户端固定 write(\"H\");write(\"I\")）\n");
    printf("========================================================");

    struct Case {
        int srv_nodelay, srv_ack;      /* 服务端：NODELAY / ACK 模式 */
        int cli_nodelay, cli_quickack; /* 客户端 */
        const char *label;
    } cases[] = {
        { 0, 0, 0, 0, "A. 都不设（基准）                      → 预期 40ms" },
        { 1, 0, 0, 0, "B. 【只有服务端 NODELAY】              → 关键！预期仍 40ms" },
        { 0, 0, 1, 0, "C. 只有客户端 NODELAY                  → 预期 ~0ms" },
        { 0, 1, 0, 0, "D. 服务端 QUICKACK                     → 预期 ~0ms" },
        { 0, 0, 0, 1, "E. 【客户端 QUICKACK】（发送方设，无效）→ 预期仍 40ms" },
        { 1, 1, 1, 0, "F. 都设                                → 预期 ~0ms" },
    };
    int n = sizeof(cases) / sizeof(cases[0]);

    for (int i = 0; i < n; i++) {
        pid_t ps = fork();
        if (ps == 0) { server(BASE + i, cases[i].srv_nodelay, cases[i].srv_ack); _exit(0); }
        usleep(250 * 1000);

        run_client(BASE + i, cases[i].cli_nodelay, cases[i].cli_quickack, cases[i].label);

        kill(ps, SIGKILL); waitpid(ps, NULL, 0);
    }

    printf("\n========================================================\n");
    printf(" 关键结论：\n");
    printf("   B vs C → NODELAY 只管自己发的包，谁发小包谁才需要设\n");
    printf("   B vs D → 想让 ACK 快必须设 QUICKACK，NODELAY 管不了 ACK\n");
    printf("   E      → QUICKACK 设在发送方毫无意义（ACK 是接收方发的）\n");
    printf("========================================================\n");
    return 0;
}
