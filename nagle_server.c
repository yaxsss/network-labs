/*
 * nagle_server.c —— Nagle + Delayed ACK 互锁实验：服务端
 *
 * 关键设计：收到第一个字节后【不回响应】，阻塞在第二次 read 上。
 * 这样 ACK 没有被响应数据捎带（piggyback），只能等延迟确认定时器。
 * 于是对端被 Nagle 卡住的第二个小包，要等 40ms 才能发出。
 *
 * 编译：gcc -O0 -o nagle_server nagle_server.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#ifndef TCP_QUICKACK
#define TCP_QUICKACK 12
#endif

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* 相对服务进程启动时刻的毫秒数，方便看包间隔 */
static double t0_g = 0.0;
static void log_ts(const char *tag, int cfd, const char *extra)
{
    printf("  [server] %-22s fd=%d  t=%7.2f ms %s\n",
           tag, cfd, now_ms() - t0_g, extra ? extra : "");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int port = 9999;
    /* noquickack=1 → 主动关掉 quickack，强制走延迟确认（默认行为，但消除不确定性） */
    int noquickack = 1;
    /* max_conn>0 → 处理这么多连接后自动退出（方便脚本化跑完就收工） */
    int max_conn = 0;

    if (argc > 1) port        = atoi(argv[1]);
    if (argc > 2) noquickack  = atoi(argv[2]);
    if (argc > 3) max_conn    = atoi(argv[3]);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    /* 在 listening socket 上设置 → 被 accept 出来的连接继承。
     * 必须设在这里：新连接默认带 quickack 额度(quick=16)，
     * 只在 accept 之后设会来不及，包一到就被秒回 ACK，40ms 现象消失。 */
    if (noquickack) {
        int off_inherit = 0;
        setsockopt(lfd, IPPROTO_TCP, TCP_QUICKACK, &off_inherit, sizeof(off_inherit));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(lfd, 16) < 0) {
        perror("listen"); return 1;
    }

    t0_g = now_ms();
    printf("server listening on :%d  (noquickack=%d)\n", port, noquickack);
    printf("  noquickack=1 → 关闭 quickack，强制延迟确认（默认路径，稳定复现 40ms）\n");
    printf("  noquickack=0 → 不做任何设置，交给内核启发式（可能秒回 ACK，40ms 消失）\n\n");

    int served = 0;
    while (1) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { perror("accept"); continue; }

        if (max_conn > 0 && ++served > max_conn) {
            log_ts("max_conn reached, exit", cfd, "");
            close(cfd);
            break;
        }

        log_ts("accept", cfd, "");

        /*
         * 关掉 quickack。注意：TCP_QUICKACK 是一次性的(one-shot)，
         * 内核会在特定条件下重置，所以每次 recv 之后都要重新设置才稳。
         */
        if (noquickack) {
            int off = 0;
            setsockopt(cfd, IPPROTO_TCP, TCP_QUICKACK, &off, sizeof(off));
        }

        /* ready 信号：让客户端在我们的 setsockopt 生效之后才开始发包 */
        write(cfd, "G", 1);

        char buf[64];
        /* 第一次读：拿到第一个小包。刻意不写回任何数据 */
        ssize_t n1 = read(cfd, buf, 1);
        log_ts("read #1 done", cfd, (n1 == 1) ? "byte='H'" : "closed");
        if (n1 <= 0) { close(cfd); continue; }

        if (noquickack) {
            int off = 0;
            setsockopt(cfd, IPPROTO_TCP, TCP_QUICKACK, &off, sizeof(off));
        }

        /* 第二次读：阻塞在这里，等那个被 Nagle 扣住的第二个小包 */
        double t_before = now_ms();
        ssize_t n2 = read(cfd, buf + 1, 1);
        double waited = now_ms() - t_before;

        char msg[128];
        snprintf(msg, sizeof(msg), "waited %.2f ms", waited);
        log_ts("read #2 done", cfd, msg);

        if (n2 > 0) {
            /* 收齐了才回响应 —— 这一步的 ACK 才是被"数据"逼出来的 */
            write(cfd, "P", 1);
            log_ts("reply sent", cfd, "");
        }
        close(cfd);
    }
    return 0;
}
