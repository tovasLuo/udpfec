/*
 * UDP Echo Server — 集成 goodtp 可靠传输库
 *
 * 架构：
 *   1. 主线程：InsLoadGtpModule → CreateGtpInstance → epoll 收发循环
 *   2. 定时器：每 10ms 调用 PeriodGtpTimer()
 *   3. send_pack_cb_  : goodtp 发包时回调，实际调用 sendto()
 *   4. receive_frame_cb_ : goodtp 解包后回调，把数据 echo 回客户端
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include <stdarg.h>

#include "bitlinker.h"

/* ───────────── 帧格式（与客户端约定） ───────────── */
typedef struct EchoFrame {
    uint32_t magic;        /* 0xA5A55A5A */
    uint32_t seq;
    uint64_t send_ts_us;
    char     payload[128];
} EchoFrame;

/* ───────────── 配置 ───────────── */
#define LISTEN_PORT      9000
#define TIMER_PERIOD_US  10000   /* 10 ms */
#define RECV_BUF_SZ      (65536)

/* ───────────── goodtp 包类型解析 ───────────── */
#define GTP_PACK_TYPE(buf)  (((*(uint32_t*)(buf)) >> 16) & 0x7u)
#define GTP_TYPE_DATA    0x01
#define GTP_TYPE_ACK     0x02
#define GTP_TYPE_FEC     0x03
#define GTP_TYPE_RTT_REQ 0x04
#define GTP_TYPE_RTT_RES 0x05
#define GTP_TYPE_NACK    0x06

typedef struct PackTypeStat {
    uint32_t data;
    uint32_t ack;
    uint32_t fec;
    uint32_t rtt_req;
    uint32_t rtt_res;
    uint32_t nack;
    uint32_t other;
} PackTypeStat;

static void PackTypeStat_add(PackTypeStat *s, const void *buf) {
    uint32_t t = GTP_PACK_TYPE(buf);
    switch (t) {
        case GTP_TYPE_DATA:    s->data++;    break;
        case GTP_TYPE_ACK:     s->ack++;     break;
        case GTP_TYPE_FEC:     s->fec++;     break;
        case GTP_TYPE_RTT_REQ: s->rtt_req++; break;
        case GTP_TYPE_RTT_RES: s->rtt_res++; break;
        case GTP_TYPE_NACK:    s->nack++;    break;
        default:               s->other++;   break;
    }
}

static void PackTypeStat_print(const char *tag, const PackTypeStat *s) {
    printf("%s data=%-4u ack=%-4u fec=%-4u nack=%-4u rtt_req=%-4u rtt_res=%-4u other=%-4u\n",
           tag, s->data, s->ack, s->fec, s->nack, s->rtt_req, s->rtt_res, s->other);
}

/* ───────────── 全局状态 ───────────── */
typedef struct ServerCtx {
    int           sfd;           /* 监听 socket */
    GtpHandler_p  gtp_hdl;       /* goodtp 实例句柄 */
    volatile int  running;       /* 主循环标志 */
    uint32_t recv_pack_cnt;      /* recvfrom 收到的原始 UDP 包数 */
    uint32_t recv_frame_cnt;     /* goodtp 解出应用帧数 */
    uint32_t send_pack_cnt;      /* goodtp 触发 sendto 的包数（听控制/重传/FEC）*/
    PackTypeStat snd_stat;       /* 发出包类型统计 */
    PackTypeStat rcv_stat;       /* 收到包类型统计 */
} ServerCtx;

static ServerCtx g_ctx;

/* ───────────── 工具：获取微秒时间戳 ───────────── */
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* ───────────── goodtp 回调：真正发包（调用 sendto） ───────────── */
static uint32_t SendPackCallback(GtpHandler_p gtp_hdl, void *pack, uint32_t size, GtpAddr *tran_addr) {
    (void)gtp_hdl;
    ServerCtx *ctx = (ServerCtx *)tran_addr->context_;
    ctx->send_pack_cnt++;
    PackTypeStat_add(&ctx->snd_stat, pack);

    ssize_t nret = sendto(ctx->sfd, pack, size, 0,
                          (struct sockaddr *)tran_addr->sock_addr_,
                          tran_addr->sock_addr_len_);
    if ((ssize_t)size != nret) {
        fprintf(stderr, "[server] sendto failed: %s\n", strerror(errno));
    }
    return GTP_OK;
}

/* ───────────── goodtp 回调：收到完整应用帧后 echo ───────────── */
static uint32_t ReceiveFrameCallback(GtpHandler_p gtp_hdl, void *frame, uint32_t size, GtpAddr *tran_addr) {
    ServerCtx *ctx = (ServerCtx *)tran_addr->context_;

    /* 解析并打印 EchoFrame */
    if (size >= sizeof(EchoFrame)) {
        EchoFrame *ef   = (EchoFrame *)frame;
        uint64_t   delay = now_us() - ef->send_ts_us;
        char peer_ip[64] = {0};
        uint16_t peer_port = 0;
        struct sockaddr_in *sin = (struct sockaddr_in *)tran_addr->sock_addr_;
        inet_ntop(AF_INET, &sin->sin_addr, peer_ip, sizeof(peer_ip));
        peer_port = ntohs(sin->sin_port);

        ctx->recv_frame_cnt++;
        uint32_t extra_recv = (ctx->recv_pack_cnt > ctx->recv_frame_cnt)
                              ? (ctx->recv_pack_cnt - ctx->recv_frame_cnt) : 0;
        printf("[server] recv  seq=%-5u  delay=%4llu us  from=%s:%u"
               "  | rcv_packs=%-4u rcv_frames=%-4u extra=%-4u"
               "  | snd_packs=%-4u  payload: %s\n",
               ef->seq, (unsigned long long)delay, peer_ip, peer_port,
               ctx->recv_pack_cnt, ctx->recv_frame_cnt, extra_recv,
               ctx->send_pack_cnt, ef->payload);
        PackTypeStat_print("         [server rcv]", &ctx->rcv_stat);
        PackTypeStat_print("         [server snd]", &ctx->snd_stat);
    } else {
        printf("[server] recv %u bytes (unknown format)\n", size);
    }

    /* 申请 goodtp 内存并发回（echo） */
    uint32_t   mem_len  = 0;
    GtpAddr   *snd_addr = NULL;
    uint8_t   *snd_mem  = GtpMallocPackMem(gtp_hdl, NULL, 0, &mem_len,
                                            (void **)&snd_addr, size);
    if (NULL == snd_mem) {
        fprintf(stderr, "[server] GtpMallocPackMem failed\n");
        return GTP_OK;
    }

    memcpy(snd_mem, frame, size);

    /* 使用来源地址作为目标地址 */
    snd_addr->context_      = ctx;
    snd_addr->sfd_          = (uint32_t)ctx->sfd;
    snd_addr->stream_type_  = kReliableStream;
    snd_addr->enable_key_   = 0;
    snd_addr->self_addr_len_= 0;
    snd_addr->sock_addr_len_= tran_addr->sock_addr_len_;
    memcpy(snd_addr->sock_addr_, tran_addr->sock_addr_, tran_addr->sock_addr_len_);

    uint32_t ret = GtpFrameSend(gtp_hdl, snd_mem, size, snd_addr, 0, 0);
    if (GTP_OK != ret) {
        fprintf(stderr, "[server] GtpFrameSend failed: 0x%08x  %s\n", ret, LastGtpErrorInfo());
        GtpFreePackMem(gtp_hdl, snd_mem);
    }

    return GTP_OK;
}
static void GtpLogCallback(uint32_t log_level, const char *fmt, ...) {
    static const char *level_str[] = {
        "EMERG","ALERT","CRIT","ERROR","WARN","NOTICE","INFO","DEBUG"
    };
    const char *lv = (log_level < 8) ? level_str[log_level] : "?";
    printf("[goodtp-svr][%s] ", lv);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static uint32_t GtpLogLevelCallback(void) {
    return 7; /* kGtpLogLevelDebug — 打印全部 */
}
/* ───────────── 信号处理 ───────────── */
static void OnSignal(int sig) {
    (void)sig;
    g_ctx.running = 0;
}

/* ───────────── 创建并绑定 UDP socket ───────────── */
static int CreateUdpSocket(uint16_t port) {
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0) {
        perror("socket");
        return -1;
    }

    int reuse = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int sndbuf = 1024 * 1024 * 2;
    int rcvbuf = 1024 * 1024 * 2;
    setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(sfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sfd);
        return -1;
    }
    return sfd;
}

/* ───────────── main ───────────── */
int main(void) {
    signal(SIGINT,  OnSignal);
    signal(SIGTERM, OnSignal);

    /* 1. 加载 goodtp 模块（进程级，只调一次） */
    if (GTP_OK != InsLoadGtpModule()) {
        fprintf(stderr, "[server] InsLoadGtpModule failed: %s\n", LastGtpErrorInfo());
        return 1;
    }
    printf("[server] goodtp module loaded.\n");

    uint8_t ver[256] = {0};
    GetGtpVersion(ver);
    printf("[server] goodtp version: %s\n", ver);

    /* 2. 建立 UDP socket */
    g_ctx.sfd     = CreateUdpSocket(LISTEN_PORT);
    g_ctx.running        = 1;
    g_ctx.recv_pack_cnt  = 0;
    g_ctx.recv_frame_cnt = 0;
    g_ctx.send_pack_cnt  = 0;
    memset(&g_ctx.snd_stat, 0, sizeof(g_ctx.snd_stat));
    memset(&g_ctx.rcv_stat, 0, sizeof(g_ctx.rcv_stat));
    if (g_ctx.sfd < 0) {
        RmLoadGtpModule();
        return 1;
    }
    printf("[server] listening on UDP port %d\n", LISTEN_PORT);

    /* 3. 创建 goodtp 实例 */
    GtpCallBackParam reg_cb;
    memset(&reg_cb, 0, sizeof(reg_cb));
    reg_cb.send_pack_cb_    = SendPackCallback;
    reg_cb.receive_frame_cb_= ReceiveFrameCallback;
    reg_cb.write_log_cb_    = GtpLogCallback;
    reg_cb.cur_log_level_cb_= GtpLogLevelCallback;

    MemPoolConfig mem_cfg;
    memset(&mem_cfg, 0, sizeof(mem_cfg));
    mem_cfg.m_256bytes_num_ = 64;
    mem_cfg.m_512bytes_num_ = 64;
    mem_cfg.m_1k_num_       = 128;
    mem_cfg.m_1_5k_num_     = 128;
    mem_cfg.m_4k_num_       = 32;
    mem_cfg.m_8k_num_       = 16;
    mem_cfg.m_64k_num_      = 8;

    /* system_id=1, session_ttl_us=30s */
    g_ctx.gtp_hdl = CreateGtpInstance(1, 30000000, &reg_cb, &mem_cfg);
    if (INVALID_GTP_HANDLER == g_ctx.gtp_hdl) {
        fprintf(stderr, "[server] CreateGtpInstance failed: %s\n", LastGtpErrorInfo());
        close(g_ctx.sfd);
        RmLoadGtpModule();
        return 1;
    }
    printf("[server] goodtp instance created.\n");

    /* 4. 启动 epoll 收包主循环（PeriodGtpTimer 在主线程调用，保证单线程） */
    int epfd = epoll_create1(0);
    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = g_ctx.sfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, g_ctx.sfd, &ev);

    static uint8_t raw_buf[RECV_BUF_SZ];

    uint64_t next_timer_ts = now_us();
    printf("[server] running. press Ctrl+C to stop.\n");

    while (g_ctx.running) {
        struct epoll_event events[4];
        int n = epoll_wait(epfd, events, 4, 5 /* ms，保证定时器精度 */);

        /* 定时器：每 10ms 调用一次（主线程内，无跨线程问题）*/
        uint64_t now = now_us();
        if (now >= next_timer_ts) {
            PeriodGtpTimer(g_ctx.gtp_hdl);
            next_timer_ts = now + TIMER_PERIOD_US;
        }

        if (n <= 0) continue;

        for (int i = 0; i < n; ++i) {
            if (!(events[i].events & EPOLLIN)) continue;

            struct sockaddr_storage peer_addr;
            socklen_t peer_len = sizeof(peer_addr);

            ssize_t recv_size = recvfrom(g_ctx.sfd, raw_buf, sizeof(raw_buf), 0,
                                         (struct sockaddr *)&peer_addr, &peer_len);
            if (recv_size <= 0) continue;
            g_ctx.recv_pack_cnt++;
            PackTypeStat_add(&g_ctx.rcv_stat, raw_buf);

            /* 申请 goodtp 包内存（包含 GtpAddr）*/
            uint32_t  mem_len  = 0;
            GtpAddr  *tran_addr= NULL;
            uint8_t  *pack_mem = GtpMallocPackMem(g_ctx.gtp_hdl, NULL, 0, &mem_len,
                                                   (void **)&tran_addr, (uint32_t)recv_size);
            if (NULL == pack_mem) {
                fprintf(stderr, "[server] GtpMallocPackMem failed\n");
                continue;
            }

            memcpy(pack_mem, raw_buf, (uint32_t)recv_size);

            /* 填充 GtpAddr（来源地址用于 goodtp 识别 session） */
            tran_addr->context_      = &g_ctx;
            tran_addr->sfd_          = (uint32_t)g_ctx.sfd;
            tran_addr->stream_type_  = kReliableStream;
            tran_addr->enable_key_   = 0;
            tran_addr->self_addr_len_= 0;
            tran_addr->sock_addr_len_= (uint32_t)peer_len;
            memcpy(tran_addr->sock_addr_, &peer_addr, (uint32_t)peer_len);

            uint32_t ret = GtpPacketReceive(g_ctx.gtp_hdl, pack_mem, (uint32_t)recv_size, tran_addr);
            if (GTP_OK != ret) {
                fprintf(stderr, "[server] GtpPacketReceive failed: 0x%08x\n", ret);
                GtpFreePackMem(g_ctx.gtp_hdl, pack_mem);
            }
        }
    }

    /* 6. 清理 */
    printf("[server] shutting down...\n");
    close(epfd);
    DeleteGtpInstance(g_ctx.gtp_hdl);
    close(g_ctx.sfd);
    RmLoadGtpModule();
    printf("[server] done.\n");
    return 0;
}
