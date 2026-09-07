/*
 * nagle_client.c —— Nagle + Delayed ACK 互锁实验：客户端
 *
 * 用法：./nagle_client [模式] [轮数]
 *   模式: split   (默认) 两次独立的小 write("H") + write("I")  ← 踩雷写法
 *         merged  一次 write("HI")                              ← 正确写法
 *         nodelay 两次小 write，但开 TCP_NODELAY
 *
 * 三种模式的对比，就是这个实验的全部结论。
 *
 * 编译：gcc -O0 -o nagle_client nagle_client.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "split";
    int rounds      = (argc > 2) ? atoi(argv[2]) : 5;
    int port        = 9999;

    int use_nodelay = (strcmp(mode, "nodelay") == 0);
    int use_merged  = (strcmp(mode, "merged")  == 0);

    printf("=== mode=%s  rounds=%d ===\n", mode, rounds);
    printf("  split  : write(\"H\"); write(\"I\");   —— 两次独立小写，Nagle 生效\n");
    printf("  merged : write(\"HI\");               —— 合并成一次，Nagle 无从扣留\n");
    printf("  nodelay: write(\"H\"); write(\"I\"); + TCP_NODELAY\n\n");

    for (int i = 0; i < rounds; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return 1; }

        if (use_nodelay) {
            int on = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("connect"); return 1;
        }

        /* 等服务端 ready（它的 setsockopt 已生效），再开始计时 */
        char go[4];
        if (read(fd, go, 1) <= 0) { close(fd); continue; }

        double t_start = now_ms();

        if (use_merged) {
            /* 对照组：一次写完，只有一个包，不存在"等 ACK" */
            write(fd, "HI", 2);
        } else {
            /* 雷区写法：先 header 再 body，两个都远小于 MSS */
            write(fd, "H", 1);
            write(fd, "I", 1);
        }

        char resp[16];
        ssize_t n = read(fd, resp, sizeof(resp));

        double cost = now_ms() - t_start;
        if (n > 0) {
            printf("  round %d: %.2f ms\n", i, cost);
        } else {
            printf("  round %d: %.2f ms (no response)\n", i, cost);
        }
        fflush(stdout);

        close(fd);
        usleep(200 * 1000);  /* 拉开轮次间隔，避免连接间互相干扰 */
    }
    return 0;
}
