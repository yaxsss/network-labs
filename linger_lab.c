/*
 * linger_lab.c —— SO_LINGER 三种取值实测：close() 到底发 FIN 还是 RST
 *
 * 编译: gcc -O0 -Wall -o linger_lab linger_lab.c
 * 运行: ./linger_lab            （自动跑完 3 组，配合 sniffer.py 抓包）
 *
 * 设计：客户端连上后【故意不读】，服务端用非阻塞写把发送缓冲区填满，
 *       保证 close() 那一刻发送缓冲区里一定有残留数据。
 *       这样三种 linger 行为的差异才会暴露出来。
 *
 *   case 0: l_onoff = 0            （默认，内核后台发）
 *   case 1: l_onoff=1, l_linger=0  （声称：立即 RST）
 *   case 2: l_onoff=1, l_linger=2  （声称：等 2 秒，发完→FIN；超时→RST）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define TOTAL (8 * 1024 * 1024)   /* 8MB，保证填不满、有残留 */
#define BASE  9200

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* 带重试的 connect：等服务端 listen 起来 */
static int connect_retry(int port)
{
    for (int i = 0; i < 200; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) return fd;
        close(fd);
        usleep(50 * 1000);
    }
    return -1;
}

/* ---------- 客户端：连上后故意不读 ---------- */
static void client(int port)
{
    int drain = (port >= 9210);   /* 9210+ 端口 = 正常读，数据能发完 */
    int fd = connect_retry(port);
    if (fd < 0) { fprintf(stderr, "connect failed\n"); return; }
    write(fd, "x", 1);                 /* 告诉服务端我连上了 */

    if (drain) {
        /* 对照组：正常读，让发送缓冲区的数据能发完 */
        char b[65536];
        ssize_t n;
        while ((n = read(fd, b, sizeof(b))) > 0) { }
        sleep(1);
    } else {
        /* 关键：什么都不读，让对端发送缓冲区堆积 */
        sleep(6);
    }
    close(fd);
}

/* ---------- 服务端 ---------- */
static void server(int port, int onoff, int linger_sec, const char *label)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); exit(1); }
    if (listen(lfd, 8) < 0) { perror("listen"); exit(1); }

    int cfd = accept(lfd, NULL, NULL);
    char c;
    read(cfd, &c, 1);                  /* 等客户端的连接信号 */

    /* 设置 SO_LINGER */
    if (onoff) {
        struct linger lg;
        lg.l_onoff  = onoff;
        lg.l_linger = linger_sec;
        if (setsockopt(cfd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) < 0)
            perror("setsockopt SO_LINGER");
    }

    /* 非阻塞写 8MB，把发送缓冲区填满 */
    fcntl(cfd, F_SETFL, O_NONBLOCK);
    char *buf = malloc(TOTAL);
    memset(buf, 'A', TOTAL);
    size_t off = 0;
    while (off < TOTAL) {
        ssize_t n = write(cfd, buf + off, TOTAL - off);
        if (n > 0) { off += n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0) break;
    }
    printf("  [%s] 写入 %zu 字节后 EAGAIN（发送缓冲区有残留）\n", label, off);
    fflush(stdout);

    /* 关键一步：close，观察它发 FIN 还是 RST */
    double t0 = now_ms();
    int r = close(cfd);
    double cost = now_ms() - t0;

    printf("  [%s] close() 返回 %d，耗时 %.1f ms%s\n",
           label, r, cost, (r < 0) ? "，errno=EWOULDBLOCK（超时！）" : "");
    fflush(stdout);

    free(buf);
    close(lfd);
    sleep(4);                          /* 留出观察窗口：默认模式后台还在发 */
}

int main(int argc, char **argv)
{
    int only = (argc > 1) ? atoi(argv[1]) : -1;
    printf("================================================\n");
    printf(" SO_LINGER 实测：close() 发 FIN 还是 RST？\n");
    printf(" （客户端故意不读，保证发送缓冲区有残留）\n");
    printf("================================================\n");
    fflush(stdout);

    struct { int onoff; int sec; const char *label; } cases[] = {
        { 0, 0, "case0 l_onoff=0 (默认)" },
        { 1, 0, "case1 l_onoff=1 l_linger=0" },
        { 1, 2, "case2 l_onoff=1 l_linger=2" },
        { 1, 2, "case3 l_onoff=1 l_linger=2 (对端正常读)" },
    };

    for (int i = 0; i < 4; i++) {
        if (only >= 0 && i != only) continue;
        printf("\n--- %s ---\n", cases[i].label);
        fflush(stdout);
        pid_t ps = fork();
        if (ps == 0) { server(BASE + i + (i==3 ? 10 : 0), cases[i].onoff, cases[i].sec, cases[i].label); _exit(0); }
        usleep(100 * 1000);

        pid_t pc = fork();
        if (pc == 0) { client(BASE + i + (i==3 ? 10 : 0)); _exit(0); }

        waitpid(ps, NULL, 0);
        fflush(stdout);
        kill(pc, SIGKILL); waitpid(pc, NULL, 0);
        usleep(300 * 1000);
    }

    printf("\n完成。对比抓包里 close 之后的那一个包：F=FIN, R=RST\n");
    return 0;
}
