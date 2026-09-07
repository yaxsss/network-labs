/*
 * coalesce_lab.c —— 为什么两次 1 字节 write 不会合并？什么时候会合并？
 *
 * 编译: gcc -O0 -Wall -o coalesce_lab coalesce_lab.c
 * 运行: ./coalesce_lab [mode]     mode: 0-4，不给则全跑
 *
 * 配合 sniffer.py 数 C->S 的报文个数和 len：
 *   python3 sniffer.py 9600 3
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

#define PORT 9600

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* 服务端：把收到的字节数打出来，看一次读到几个 */
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
    listen(lfd, 16);

    int cfd = accept(lfd, NULL, NULL);
    write(cfd, "G", 1);                       /* ready */

    /* 等 500ms，让客户端把所有 write 都做完，再一次性看收到了多少 */
    usleep(500 * 1000);

    char buf[256];
    int total = 0;
    int n;
    /* 非阻塞，把当前能读的全读出来 */
    while ((n = recv(cfd, buf + total, sizeof(buf) - 1 - total, MSG_DONTWAIT)) > 0)
        total += n;

    printf("    [server] 一次性读到 %d 字节: \"%.*s\"\n", total, total, buf);
    fflush(stdout);

    write(cfd, "P", 1);
    usleep(200 * 1000);
    close(cfd);
    close(lfd);
}

static int connect_wait(int port)
{
    for (int k = 0; k < 200; k++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in b;
        memset(&b, 0, sizeof(b));
        b.sin_family = AF_INET;
        b.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &b.sin_addr);
        if (connect(fd, (struct sockaddr *)&b, sizeof(b)) == 0) return fd;
        close(fd);
        usleep(50 * 1000);
    }
    return -1;
}

static void client(int mode, const char *label)
{
    int fd = connect_wait(PORT);
    char go[4];
    read(fd, go, 1);                           /* 等 ready */

    double t0 = now_ms();

    switch (mode) {
    case 0:     /* 两次独立 write：默认行为 */
        write(fd, "H", 1);
        write(fd, "I", 1);
        break;

    case 1:     /* MSG_MORE：告诉内核"后面还有" */
        send(fd, "H", 1, MSG_MORE);
        write(fd, "I", 1);
        break;

    case 2:     /* TCP_CORK：塞住，拔塞子才发 */
    {
        int on = 1, off = 0;
        setsockopt(fd, IPPROTO_TCP, TCP_CORK, &on, sizeof(on));
        write(fd, "H", 1);
        write(fd, "I", 1);
        usleep(50 * 1000);
        setsockopt(fd, IPPROTO_TCP, TCP_CORK, &off, sizeof(off));
        break;
    }
    case 3:     /* TCP_NODELAY：不合并，但也不等 ACK */
    {
        int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        write(fd, "H", 1);
        write(fd, "I", 1);
        break;
    }
    case 4:     /* 一次写完 */
        write(fd, "HI", 2);
        break;
    case 5:     /* 5 次极快的 1 字节 write，看 autocorking 会不会合并 */
        write(fd, "A", 1);
        write(fd, "B", 1);
        write(fd, "C", 1);
        write(fd, "D", 1);
        write(fd, "E", 1);
        break;
    }

    char r[16];
    read(fd, r, sizeof(r));
    printf("  %-42s write 耗时 %6.2f ms\n", label, now_ms() - t0);
    fflush(stdout);
    close(fd);
}

int main(int argc, char **argv)
{
    int only = (argc > 1) ? atoi(argv[1]) : -1;

    struct { int m; const char *label; } cs[] = {
        { 0, "mode0 两次 write（默认）" },
        { 1, "mode1 send(MSG_MORE) + write" },
        { 2, "mode2 TCP_CORK" },
        { 3, "mode3 TCP_NODELAY" },
        { 4, "mode4 一次 write(\"HI\")" },
        { 5, "mode5 5 次极快 1 字节 write" },
    };

    printf("==========================================================\n");
    printf(" 小包合并实验（看抓包里 C->S 有几个带数据的报文）\n");
    printf("==========================================================\n");

    for (int i = 0; i < 6; i++) {
        if (only >= 0 && i != only) continue;
        printf("\n--- %s ---\n", cs[i].label);
        fflush(stdout);

        pid_t ps = fork();
        if (ps == 0) { server(PORT); _exit(0); }
        usleep(200 * 1000);

        client(cs[i].m, cs[i].label);

        kill(ps, SIGKILL); waitpid(ps, NULL, 0);
        usleep(300 * 1000);
    }
    return 0;
}

