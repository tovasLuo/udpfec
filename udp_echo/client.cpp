/*
 * UDP Echo Client — 集成 goodtp 可靠传输库
 *
 * 架构：
 *   1. InsLoadGtpModule → CreateGtpInstance
 *   2. 每隔 1s 向服务器发送一帧带序号的消息
 *   3. receive_frame_cb_ 收到 echo 后打印 RTT
 *   4. 定时器线程每 10ms 调 BitLinkerWheel()
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

/* ───────────── 配置 ───────────── */
#define SERVER_IP        "127.0.0.1"
#define SERVER_PORT      9000
#define SEND_INTERVAL_US 1000000    /* 1 s */
#define TIMER_PERIOD_US  10000      /* 10 ms */
#define RECV_BUF_SZ      (65536)

/* ───────────── goodtp 包类型（来自 goodtp_macrodefine.h） ───────────── */
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

/* ───────────── 帧格式（与服务端约定） ───────────── */
typedef struct EchoFrame {
    uint32_t magic;        /* 0xA5A55A5A */
    uint32_t seq;
    uint64_t send_ts_us;
    char     payload[128];
} EchoFrame;

/* ───────────── 全局状态 ───────────── */
typedef struct ClientCtx {
    int           sfd;        /* 发送/接收 socket */
    GtpHandler_p  gtp_hdl;
    volatile int  running;

    struct sockaddr_in server_addr;
    socklen_t          server_addr_len;

    uint32_t send_seq;        /* 发出的应用帧数 */
    uint32_t recv_seq;        /* 收到的应用帧数 */
    uint32_t send_pack_cnt;   /* goodtp 实际触发 sendto 的包数（含控制/重传/FEC）*/
    uint32_t recv_pack_cnt;   /* recvfrom 收到的原始 UDP 包数 */
    PackTypeStat snd_stat;    /* 发出包类型统计 */
    PackTypeStat rcv_stat;    /* 收到包类型统计 */
} ClientCtx;

static ClientCtx g_ctx;

/* ───────────── 获取微秒时间戳 ───────────── */
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* ───────────── goodtp 回调：真正发包 ───────────── */
static uint32_t SendPackCallback(GtpHandler_p gtp_hdl, void *pack, uint32_t size, GtpAddr *tran_addr) {
    (void)gtp_hdl;
    ClientCtx *ctx = (ClientCtx *)tran_addr->context_;
    ctx->send_pack_cnt++;
    PackTypeStat_add(&ctx->snd_stat, pack);

    ssize_t nret = sendto(ctx->sfd, pack, size, 0,
                          (struct sockaddr *)tran_addr->peer_addr_,
                          tran_addr->peer_addr_len_);
    if ((ssize_t)size != nret) {
        fprintf(stderr, "[client] sendto failed: %s\n", strerror(errno));
    }
    return GTP_OK;
}

/* ───────────── goodtp 回调：收到服务端 echo ───────────── */
static uint32_t ReceiveFrameCallback(GtpHandler_p gtp_hdl, void *frame, uint32_t size, GtpAddr *tran_addr) {
    (void)gtp_hdl;
    (void)tran_addr;

    if (size < sizeof(EchoFrame)) {
        fprintf(stderr, "[client] recv too small (%u bytes)\n", size);
        return GTP_OK;
    }

    EchoFrame *ef   = (EchoFrame *)frame;
    uint64_t   rtt  = now_us() - ef->send_ts_us;

    ClientCtx *ctx = (ClientCtx *)tran_addr->context_;
    ctx->recv_seq  = ef->seq;

    uint32_t extra = (ctx->send_pack_cnt > ctx->send_seq)
                     ? (ctx->send_pack_cnt - ctx->send_seq) : 0;
    printf("[client] echo  seq=%-5u  rtt=%4llu us"
           "  | snd_frames=%-4u snd_packs=%-4u extra=%-4u"
           "  | rcv_packs=%-4u  payload: %s\n",
           ef->seq, (unsigned long long)rtt,
           ctx->send_seq, ctx->send_pack_cnt, extra,
           ctx->recv_pack_cnt, ef->payload);
    PackTypeStat_print("         [client snd]", &ctx->snd_stat);
    PackTypeStat_print("         [client rcv]", &ctx->rcv_stat);

    return GTP_OK;
}

static void GtpLogCallback(uint32_t log_level, const char *fmt, ...) {
    static const char *level_str[] = {
        "EMERG","ALERT","CRIT","ERROR","WARN","NOTICE","INFO","DEBUG"
    };
    const char *lv = (log_level < 8) ? level_str[log_level] : "?";
    printf("[goodtp-cli][%s] ", lv);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static uint32_t GtpLogLevelCallback(void) {
    return 7; /* kGtpLogLevelDebug */
}

/* ───────────── 发送一帧 ───────────── */
static void SendEchoRequest(ClientCtx *ctx) {
    EchoFrame ef;
    memset(&ef, 0, sizeof(ef));
    ef.magic      = 0xA5A55A5A;
    ef.seq        = ++ctx->send_seq;
    ef.send_ts_us = now_us();
    snprintf(ef.payload, sizeof(ef.payload), "hello from client, seq=%u", ef.seq);

    uint32_t  mem_len  = 0;
    GtpAddr  *tran_addr= NULL;
    uint8_t  *snd_mem  = GtpMallocPackMem(ctx->gtp_hdl, NULL, 0, &mem_len,
                                            (void **)&tran_addr, (uint32_t)sizeof(ef));
    if (NULL == snd_mem) {
        fprintf(stderr, "[client] GtpMallocPackMem failed\n");
        return;
    }

    memcpy(snd_mem, &ef, sizeof(ef));

    tran_addr->context_      = ctx;
    tran_addr->sfd_          = (uint32_t)ctx->sfd;
    tran_addr->stream_type_  = kSuperReliableStream;
    tran_addr->enable_key_   = 0;
    tran_addr->self_addr_len_= 0;
    tran_addr->peer_addr_len_= ctx->server_addr_len;
    memcpy(tran_addr->peer_addr_, &ctx->server_addr, ctx->server_addr_len);

    uint32_t ret = GtpFrameSend(ctx->gtp_hdl, snd_mem, (uint32_t)sizeof(ef), tran_addr, 0, 0);
    if (GTP_OK != ret) {
        fprintf(stderr, "[client] GtpFrameSend failed: 0x%08x  %s\n", ret, LastGtpErrorInfo());
        GtpFreePackMem(ctx->gtp_hdl, snd_mem);
    } else {
        printf("[client] sent  seq=%-5u  payload: %s\n", ef.seq, ef.payload);
    }
}

/* ───────────── 信号处理 ───────────── */
static void OnSignal(int sig) {
    (void)sig;
    g_ctx.running = 0;
}

/* ───────────── main ───────────── */
int main(int argc, char *argv[]) {
    const char *server_ip   = SERVER_IP;
    uint16_t    server_port = SERVER_PORT;

    if (argc >= 2) server_ip   = argv[1];
    if (argc >= 3) server_port = (uint16_t)atoi(argv[2]);

    signal(SIGINT,  OnSignal);
    signal(SIGTERM, OnSignal);

    /* 1. 加载 goodtp 模块 */
    if (GTP_OK != InsLoadGtpModule()) {
        fprintf(stderr, "[client] InsLoadGtpModule failed: %s\n", LastGtpErrorInfo());
        return 1;
    }
    printf("[client] goodtp module loaded.\n");

    uint8_t ver[256] = {0};
    GetGtpVersion(ver);
    printf("[client] goodtp version: %s\n", ver);

    /* 2. 建立 UDP socket */
    g_ctx.sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_ctx.sfd < 0) {
        perror("socket");
        RmLoadGtpModule();
        return 1;
    }

    int sndbuf = 1024 * 1024 * 2;
    int rcvbuf = 1024 * 1024 * 2;
    setsockopt(g_ctx.sfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(g_ctx.sfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    /* 绑定本地随机端口，保证有固定 src port 供 goodtp 区分 session */
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family      = AF_INET;
    local_addr.sin_port        = 0;  /* OS 自动分配 */
    local_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_ctx.sfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind");
        close(g_ctx.sfd);
        RmLoadGtpModule();
        return 1;
    }

    /* 服务端地址 */
    memset(&g_ctx.server_addr, 0, sizeof(g_ctx.server_addr));
    g_ctx.server_addr.sin_family = AF_INET;
    g_ctx.server_addr.sin_port   = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &g_ctx.server_addr.sin_addr) != 1) {
        fprintf(stderr, "[client] invalid server IP: %s\n", server_ip);
        close(g_ctx.sfd);
        RmLoadGtpModule();
        return 1;
    }
    g_ctx.server_addr_len = sizeof(g_ctx.server_addr);
    g_ctx.running         = 1;
    g_ctx.send_seq        = 0;
    g_ctx.recv_seq        = 0;
    g_ctx.send_pack_cnt   = 0;
    g_ctx.recv_pack_cnt   = 0;
    memset(&g_ctx.snd_stat, 0, sizeof(g_ctx.snd_stat));
    memset(&g_ctx.rcv_stat, 0, sizeof(g_ctx.rcv_stat));

    printf("[client] server: %s:%u\n", server_ip, server_port);

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

    g_ctx.gtp_hdl = CreateGtpInstance(2, 30000000, &reg_cb, &mem_cfg);
    if (INVALID_GTP_HANDLER == g_ctx.gtp_hdl) {
        fprintf(stderr, "[client] CreateGtpInstance failed: %s\n", LastGtpErrorInfo());
        close(g_ctx.sfd);
        RmLoadGtpModule();
        return 1;
    }
    printf("[client] goodtp instance created.\n");

    /* 4. 启动 epoll + 发包主循环（BitLinkerWheel 在主线程调用） */
    int epfd = epoll_create1(0);
    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = g_ctx.sfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, g_ctx.sfd, &ev);

    static uint8_t raw_buf[RECV_BUF_SZ];

    uint64_t next_timer_ts = now_us();
    uint64_t next_send_ts  = now_us();
    printf("[client] running. press Ctrl+C to stop.\n");

    while (g_ctx.running) {
        struct epoll_event events[4];
        int n = epoll_wait(epfd, events, 4, 5 /* ms */);

        /* 定时器 */
        uint64_t now = now_us();
        if (now >= next_timer_ts) {
            BitLinkerWheel(g_ctx.gtp_hdl);
            next_timer_ts = now + TIMER_PERIOD_US;
        }

        /* 处理收包 */
        for (int i = 0; i < n; ++i) {
            if (!(events[i].events & EPOLLIN)) continue;

            struct sockaddr_storage peer_addr;
            socklen_t peer_len = sizeof(peer_addr);

            ssize_t recv_size = recvfrom(g_ctx.sfd, raw_buf, sizeof(raw_buf), 0,
                                         (struct sockaddr *)&peer_addr, &peer_len);
            if (recv_size <= 0) continue;
            g_ctx.recv_pack_cnt++;
            PackTypeStat_add(&g_ctx.rcv_stat, raw_buf);

            uint32_t  mem_len  = 0;
            GtpAddr  *tran_addr= NULL;
            uint8_t  *pack_mem = GtpMallocPackMem(g_ctx.gtp_hdl, NULL, 0, &mem_len,
                                                   (void **)&tran_addr, (uint32_t)recv_size);
            if (NULL == pack_mem) {
                fprintf(stderr, "[client] GtpMallocPackMem failed\n");
                continue;
            }

            memcpy(pack_mem, raw_buf, (uint32_t)recv_size);

            tran_addr->context_      = &g_ctx;
            tran_addr->sfd_          = (uint32_t)g_ctx.sfd;
            tran_addr->stream_type_  = kSuperReliableStream;
            tran_addr->enable_key_   = 0;
            tran_addr->self_addr_len_= 0;
            tran_addr->peer_addr_len_= (uint32_t)peer_len;
            memcpy(tran_addr->peer_addr_, &peer_addr, (uint32_t)peer_len);

            uint32_t ret = GtpPacketReceive(g_ctx.gtp_hdl, pack_mem, (uint32_t)recv_size, tran_addr);
            if (GTP_OK != ret) {
                fprintf(stderr, "[client] GtpPacketReceive failed: 0x%08x\n", ret);
                GtpFreePackMem(g_ctx.gtp_hdl, pack_mem);
            }
        }

        /* 到达发送时间则发一帧 */
        if (now_us() >= next_send_ts) {
            SendEchoRequest(&g_ctx);
            next_send_ts = now_us() + SEND_INTERVAL_US;
        }
    }

    /* 6. 清理 */
    printf("[client] shutting down...\n");
    close(epfd);
    DeleteGtpInstance(g_ctx.gtp_hdl);
    close(g_ctx.sfd);
    RmLoadGtpModule();
    printf("[client] done. sent=%u recv=%u\n", g_ctx.send_seq, g_ctx.recv_seq);
    return 0;
}
