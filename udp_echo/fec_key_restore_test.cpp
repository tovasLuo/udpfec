/*
 * fec_key_restore_test.cpp
 *
 * 4 场景回归测试（各 8 秒，50 pps，enable_key_=1）
 *
 * S1 FEC_KEY  确定性丢包(sn%4==1) + FEC stream_key_ 新固定头验证
 *             旧 memmove 方案在 >10% 丢包时触发 res_pos!=CalcPosInPackCache 错误；
 *             新方案（stream_key_ 位于 Fec2CodePack 固定 bytes16-23）不触发。
 *
 * S2 DUPL     每个 GTP 包（DATA/FEC/ACK/NACK）发送两次
 *             验证 filter_win_ 去重：recv_frames == sent_frames，
 *             且 fec_anomaly == 0（重复 FEC 奇偶包不破坏 XOR 缓冲区）。
 *
 * S3 CORRUPT  sn%5==2 的 DATA 包替换为 4 字节垃圾发送（20% 丢弃+垃圾注入）
 *             验证接收端对非法包健壮性，FEC 恢复后帧交付率 >= 80%。
 *
 * S4 REORDER  连续 DATA 包两两 SN 交换（第 N 先到、第 N-1 后到）
 *             验证 filter_win_ 接受乱序包：recv_frames >= sent*99%，
 *             且 fec_anomaly == 0。
 *
 * 构建：cd udp_echo && cmake . && make fec_key_restore_test
 * 运行：./fec_key_restore_test   返回 0=PASS 1=FAIL
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include <stdarg.h>
#include <atomic>

#include "bitlinker.h"

/* ──────────────── 包头原始字段（小端，不依赖内部结构体）──────────────── */
#define GTP_RAW_PACK_TYPE(p)  (((const uint8_t*)(p))[2] & 0x07u)
#define GTP_RAW_PACK_SN(p)    ({uint32_t _s; memcpy(&_s,(const uint8_t*)(p)+4,4); _s;})

#define GTP_DATA_TYPE  0x01u
#define GTP_FEC_TYPE   0x03u

/* ──────────────── 测试常量 ──────────────── */
#define TEST_STREAM_KEY  0x1234567890ABCDEFULL
#define LOOPBACK_IP      "127.0.0.1"
#define PORT_A           19031
#define PORT_B           19032
#define TIMER_US         10000   /* 10 ms 定时器 */
#define SEND_PPS         50
#define DURATION_S       8       /* 每场景 8 秒 → 400 帧 */

/* ──────────────── 帧格式 ──────────────── */
typedef struct TestFrame {
    uint32_t magic;     /* 0xDEADBEEF */
    uint32_t seq;
    uint64_t send_ts;
    char     data[64];
} TestFrame;

/* ──────────────── 场景 ──────────────── */
typedef enum {
    S_FEC_KEY = 0,
    S_DUPL    = 1,
    S_CORRUPT = 2,
    S_REORDER = 3,
    S_MAX     = 4
} Scenario;

/* ──────────────── 统计（每场景 reset）──────────────── */
struct Stats {
    std::atomic<uint32_t> sent_frames{0};
    std::atomic<uint32_t> recv_frames{0};
    std::atomic<uint32_t> dropped_data{0};  /* S1/S3 丢弃的 DATA 包数 */
    std::atomic<uint32_t> fec_anomaly{0};   /* "fec restore abnormal" 出现次数 */
    std::atomic<uint32_t> fec_key_ok{0};    /* FEC 包 stream_key_ 提取正确 */
    std::atomic<uint32_t> fec_key_bad{0};   /* FEC 包 stream_key_ 提取错误 */
    void reset() {
        sent_frames.store(0); recv_frames.store(0); dropped_data.store(0);
        fec_anomaly.store(0); fec_key_ok.store(0);  fec_key_bad.store(0);
    }
};

static Stats            g_stats;
static volatile int     g_running  = 0;
static Scenario         g_scenario = S_FEC_KEY;

/* S4 乱序缓冲区：暂存一个 DATA 包，下一个到时两两交换发出 */
static uint8_t          g_held_pack[4096];
static int              g_held_pack_size = 0;

/* ──────────────── 实例上下文 ──────────────── */
typedef struct InstCtx {
    const char        *name;
    int                sfd;
    GtpHandler_p       gtp_hdl;
    struct sockaddr_in self_addr;
    struct sockaddr_in peer_addr;
    socklen_t          peer_addr_len;
} InstCtx;

static InstCtx          g_a;   /* 发送端 */
static InstCtx          g_b;   /* 接收端 */
static pthread_mutex_t  g_lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t  g_lock_b = PTHREAD_MUTEX_INITIALIZER;

/* ──────────────── 时间 ──────────────── */
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* ──────────────── 日志回调 ──────────────── */
static void LogCb(uint32_t level, const char *fmt, ...) {
    if (level > 3) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (strstr(buf, "fec restore abnormal"))
        g_stats.fec_anomaly.fetch_add(1);
    static const char *lv[] = {"EMRG","ALRT","CRIT","ERR"};
    printf("[gtp-%s] %s", lv[level], buf);
}
static uint32_t LogLevelCb(void) { return 3; }

/* ──────────────────────────────────────────────────────────────────────
 * A → B 发包回调
 *   S1 FEC_KEY  — sn%4==1 的 DATA 包丢弃
 *   S2 DUPL     — 每个包发两次
 *   S3 CORRUPT  — sn%5==2 的 DATA 包替换为 4B 垃圾（接收端必然丢弃）
 *   S4 REORDER  — DATA 包两两 SN 交换：偶数索引先暂存，奇数到时交换发出
 * ────────────────────────────────────────────────────────────────────── */
static uint32_t SendPackCbA(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    InstCtx        *ctx = (InstCtx *)addr->context_;
    const uint8_t   pt  = GTP_RAW_PACK_TYPE(pack);
    struct sockaddr *dst = (struct sockaddr *)&ctx->peer_addr;
    socklen_t        dlen = ctx->peer_addr_len;

    switch (g_scenario) {

    /* ── S1: 确定性 25% 丢包 ── */
    case S_FEC_KEY:
        if (pt == GTP_DATA_TYPE && (GTP_RAW_PACK_SN(pack) % 4u) == 1u) {
            g_stats.dropped_data.fetch_add(1);
            return GTP_OK;
        }
        sendto(ctx->sfd, pack, size, 0, dst, dlen);
        return GTP_OK;

    /* ── S2: 每包发两次（重复包去重测试）── */
    case S_DUPL:
        sendto(ctx->sfd, pack, size, 0, dst, dlen);
        sendto(ctx->sfd, pack, size, 0, dst, dlen);  /* 复制 */
        return GTP_OK;

    /* ── S3: 20% DATA 包替换为垃圾（错误包鲁棒性测试）── */
    case S_CORRUPT:
        if (pt == GTP_DATA_TYPE && (GTP_RAW_PACK_SN(pack) % 5u) == 2u) {
            static const uint8_t garbage[4] = {0xFF, 0xAB, 0xCD, 0xEF};
            sendto(ctx->sfd, garbage, sizeof(garbage), 0, dst, dlen);
            g_stats.dropped_data.fetch_add(1);
        } else {
            sendto(ctx->sfd, pack, size, 0, dst, dlen);
        }
        return GTP_OK;

    /* ── S4: DATA 两两 SN 交换（乱序到达测试）── */
    case S_REORDER:
        if (pt == GTP_DATA_TYPE) {
            if (g_held_pack_size == 0) {
                /* 暂存第一个（SN 较小的）*/
                if ((int)size <= (int)sizeof(g_held_pack)) {
                    memcpy(g_held_pack, pack, size);
                    g_held_pack_size = (int)size;
                } else {
                    sendto(ctx->sfd, pack, size, 0, dst, dlen);
                }
            } else {
                /* 先发第二个（SN 较大），再发暂存的（SN 较小） */
                sendto(ctx->sfd, pack, size, 0, dst, dlen);
                sendto(ctx->sfd, g_held_pack, g_held_pack_size, 0, dst, dlen);
                g_held_pack_size = 0;
            }
        } else {
            sendto(ctx->sfd, pack, size, 0, dst, dlen);  /* ACK/FEC 直接转发 */
        }
        return GTP_OK;

    default:
        sendto(ctx->sfd, pack, size, 0, dst, dlen);
        return GTP_OK;
    }
}

/* ──────────────── B 收帧回调 ──────────────── */
static uint32_t RecvFrameCbB(GtpHandler_p hdl, void *frame, uint32_t size, GtpAddr *addr) {
    (void)hdl; (void)addr;
    if (size >= sizeof(TestFrame)) {
        TestFrame *f = (TestFrame *)frame;
        if (f->magic == 0xDEADBEEF)
            g_stats.recv_frames.fetch_add(1);
    }
    return GTP_OK;
}

static uint32_t SendPackCbB(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    InstCtx *ctx = (InstCtx *)addr->context_;
    sendto(ctx->sfd, pack, size, 0,
           (struct sockaddr *)&ctx->peer_addr, ctx->peer_addr_len);
    return GTP_OK;
}

static uint32_t RecvFrameCbA(GtpHandler_p hdl, void *frame, uint32_t size, GtpAddr *addr) {
    (void)hdl; (void)frame; (void)size; (void)addr;
    return GTP_OK;
}

/* ──────────────── 接收线程 ──────────────── */
typedef struct {
    InstCtx         *inst;
    GtpHandler_p     gtp_hdl;
    pthread_mutex_t *lock;
    int              is_b;   /* 1=B 端，验证 FEC stream_key_ */
} RecvArg;

static void *recv_thread(void *arg) {
    RecvArg *a   = (RecvArg *)arg;
    InstCtx *ctx = a->inst;
    static __thread uint8_t buf[65536];

    while (g_running) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t n = recvfrom(ctx->sfd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&peer, &peer_len);
        if (n <= 0) continue;

        pthread_mutex_lock(a->lock);
        uint32_t  mem_len = 0;
        GtpAddr  *taddr   = NULL;
        uint8_t  *mem = GtpMallocPackMem(a->gtp_hdl, NULL, 0, &mem_len,
                                          (void **)&taddr, (uint32_t)n);
        if (!mem) { pthread_mutex_unlock(a->lock); continue; }

        memcpy(mem, buf, n);
        taddr->context_       = ctx;
        taddr->sfd_           = (uint32_t)ctx->sfd;
        taddr->stream_type_   = kRealTimeStream;
        taddr->self_addr_len_ = (uint32_t)sizeof(ctx->self_addr);
        memcpy(taddr->self_addr_, &ctx->self_addr, sizeof(ctx->self_addr));
        taddr->sock_addr_len_ = (uint32_t)peer_len;
        memcpy(taddr->sock_addr_, &peer, peer_len);

        /* B 端：提取并验证 FEC 包中的 stream_key_（新固定头部格式）*/
        if (a->is_b) {
            uint64_t sk = 0;
            GtpCheckPacketInvalid(mem, (uint32_t)n, &sk);
            if (GTP_FEC_TYPE == GTP_RAW_PACK_TYPE(mem)) {
                if (sk == TEST_STREAM_KEY) g_stats.fec_key_ok.fetch_add(1);
                else                       g_stats.fec_key_bad.fetch_add(1);
            }
            taddr->enable_key_ = 1;
            taddr->stream_key_ = sk;
        } else {
            taddr->enable_key_ = 0;
        }

        uint32_t ret = GtpPacketReceive(a->gtp_hdl, mem, (uint32_t)n, taddr);
        if (GTP_OK != ret) GtpFreePackMem(a->gtp_hdl, mem);

        pthread_mutex_unlock(a->lock);
    }
    return NULL;
}

/* ──────────────── 定时器线程 ──────────────── */
typedef struct {
    GtpHandler_p     hdl_a, hdl_b;
    pthread_mutex_t *lock_a, *lock_b;
} TimerArg;

static void *timer_thread(void *arg) {
    TimerArg *t = (TimerArg *)arg;
    while (g_running) {
        usleep(TIMER_US);
        pthread_mutex_lock(t->lock_a); PeriodGtpTimer(t->hdl_a); pthread_mutex_unlock(t->lock_a);
        pthread_mutex_lock(t->lock_b); PeriodGtpTimer(t->hdl_b); pthread_mutex_unlock(t->lock_b);
    }
    return NULL;
}

/* ──────────────── socket 辅助 ──────────────── */
static int make_udp_socket(uint16_t port, struct sockaddr_in *out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(out, 0, sizeof(*out));
    out->sin_family      = AF_INET;
    out->sin_port        = htons(port);
    out->sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)out, sizeof(*out)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    struct timeval tv = {0, 200000};   /* 200ms recv timeout */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    inet_pton(AF_INET, LOOPBACK_IP, &out->sin_addr);
    return fd;
}

/* socket 缓冲区排空（场景切换时清除残留包）*/
static void drain_sockets(void) {
    uint8_t tmp[65536];
    while (recvfrom(g_a.sfd, tmp, sizeof(tmp), MSG_DONTWAIT, NULL, NULL) > 0) {}
    while (recvfrom(g_b.sfd, tmp, sizeof(tmp), MSG_DONTWAIT, NULL, NULL) > 0) {}
}

/* ──────────────────────────────────────────────────────────────────────
 * run_scenario — 运行一个测试场景，返回 1=PASS 0=FAIL
 * ────────────────────────────────────────────────────────────────────── */
static int run_scenario(Scenario sc, const char *label) {
    printf("\n====== %s ======\n", label);

    /* 重置全局状态 */
    g_stats.reset();
    g_scenario        = sc;
    g_held_pack_size  = 0;
    g_running         = 1;

    /* GTP 回调 & 内存配置 */
    GtpCallBackParam cb_a = {SendPackCbA, RecvFrameCbA, NULL, LogCb, LogLevelCb, NULL};
    GtpCallBackParam cb_b = {SendPackCbB, RecvFrameCbB, NULL, LogCb, LogLevelCb, NULL};

    MemPoolConfig mem_cfg;
    memset(&mem_cfg, 0, sizeof(mem_cfg));
    mem_cfg.m_256bytes_num_ = 1024;
    mem_cfg.m_512bytes_num_ = 1024;
    mem_cfg.m_1k_num_       = 512;
    mem_cfg.m_1_5k_num_     = 512;
    mem_cfg.m_4k_num_       = 128;
    mem_cfg.m_8k_num_       = 64;
    mem_cfg.m_64k_num_      = 16;

    g_a.gtp_hdl = CreateGtpInstance(1, 10000000, &cb_a, &mem_cfg);
    g_b.gtp_hdl = CreateGtpInstance(2, 10000000, &cb_b, &mem_cfg);
    if (INVALID_GTP_HANDLER == g_a.gtp_hdl || INVALID_GTP_HANDLER == g_b.gtp_hdl) {
        fprintf(stderr, "CreateGtpInstance failed: %s\n", LastGtpErrorInfo());
        return 0;
    }

    /* 启动线程 */
    RecvArg  ra    = {&g_a, g_a.gtp_hdl, &g_lock_a, 0};
    RecvArg  rb    = {&g_b, g_b.gtp_hdl, &g_lock_b, 1};
    TimerArg ta    = {g_a.gtp_hdl, g_b.gtp_hdl, &g_lock_a, &g_lock_b};
    pthread_t tid_ra, tid_rb, tid_timer;
    pthread_create(&tid_ra,    NULL, recv_thread,  &ra);
    pthread_create(&tid_rb,    NULL, recv_thread,  &rb);
    pthread_create(&tid_timer, NULL, timer_thread, &ta);

    /* 发送循环 */
    const uint64_t interval = 1000000ULL / SEND_PPS;
    const uint64_t deadline = now_us() + (uint64_t)DURATION_S * 1000000ULL;
    uint64_t       next_send = now_us();
    uint32_t       seq       = 0;

    while (now_us() < deadline) {
        if (now_us() >= next_send) {
            TestFrame f;
            f.magic   = 0xDEADBEEF;
            f.seq     = ++seq;
            f.send_ts = now_us();
            snprintf(f.data, sizeof(f.data), "s%u", seq);

            pthread_mutex_lock(&g_lock_a);
            uint32_t  mem_len = 0;
            GtpAddr  *taddr   = NULL;
            uint8_t  *mem = GtpMallocPackMem(g_a.gtp_hdl, NULL, 0, &mem_len,
                                              (void **)&taddr, sizeof(f));
            if (mem) {
                memcpy(mem, &f, sizeof(f));
                taddr->context_       = &g_a;
                taddr->sfd_           = (uint32_t)g_a.sfd;
                taddr->stream_type_   = kRealTimeStream;
                taddr->enable_key_    = 1;
                taddr->stream_key_    = TEST_STREAM_KEY;
                taddr->self_addr_len_ = (uint32_t)sizeof(g_a.self_addr);
                memcpy(taddr->self_addr_, &g_a.self_addr, sizeof(g_a.self_addr));
                taddr->sock_addr_len_ = (uint32_t)sizeof(g_a.peer_addr);
                memcpy(taddr->sock_addr_, &g_a.peer_addr, sizeof(g_a.peer_addr));

                uint32_t ret = GtpFrameSend(g_a.gtp_hdl, mem, sizeof(f), taddr, 0, 0);
                if (GTP_OK == ret) g_stats.sent_frames.fetch_add(1);
                else               GtpFreePackMem(g_a.gtp_hdl, mem);
            }
            pthread_mutex_unlock(&g_lock_a);
            next_send += interval;
        }
        usleep(200);
    }

    /* S4: 发送结束后冲刷暂存的最后一个 DATA 包 */
    if (sc == S_REORDER && g_held_pack_size > 0) {
        pthread_mutex_lock(&g_lock_a);
        sendto(g_a.sfd, g_held_pack, g_held_pack_size, 0,
               (struct sockaddr *)&g_a.peer_addr, g_a.peer_addr_len);
        g_held_pack_size = 0;
        pthread_mutex_unlock(&g_lock_a);
    }

    /* 停止线程 */
    g_running = 0;
    pthread_join(tid_ra,    NULL);
    pthread_join(tid_rb,    NULL);
    pthread_join(tid_timer, NULL);

    /* 销毁 GTP 实例 */
    DeleteGtpInstance(g_a.gtp_hdl);
    DeleteGtpInstance(g_b.gtp_hdl);
    g_a.gtp_hdl = INVALID_GTP_HANDLER;
    g_b.gtp_hdl = INVALID_GTP_HANDLER;

    /* 排空 socket 缓冲区（防止下一场景读到残留包）*/
    drain_sockets();

    /* ──── 统计与判定 ──── */
    uint32_t sent  = g_stats.sent_frames.load();
    uint32_t recvd = g_stats.recv_frames.load();
    uint32_t drop  = g_stats.dropped_data.load();
    uint32_t anom  = g_stats.fec_anomaly.load();
    uint32_t fok   = g_stats.fec_key_ok.load();
    uint32_t fbad  = g_stats.fec_key_bad.load();

    printf("发送:%u 接收:%u 丢DATA:%u fec_key_ok:%u fec_key_bad:%u fec_anomaly:%u\n",
           sent, recvd, drop, fok, fbad, anom);

    int pass = 1;

    /* 所有场景公共断言 */
    if (anom > 0) {
        printf("  [FAIL] fec_anomaly=%u  (期望 0，"
               "旧 memmove 方案触发 res_pos!=CalcPosInPackCache)\n", anom);
        pass = 0;
    } else {
        printf("  [PASS] fec_anomaly=0\n");
    }
    if (fbad > 0) {
        printf("  [FAIL] fec_key_bad=%u  "
               "(FEC 包 stream_key_ 从新固定头部提取失败)\n", fbad);
        pass = 0;
    } else if (fok > 0) {
        printf("  [PASS] FEC stream_key_ 验证正确 (%u 包)\n", fok);
    }

    /* 场景专属断言 */
    switch (sc) {
    case S_FEC_KEY:
        if (fok == 0)
            printf("  [WARN] fec_key_ok=0：未收到 FEC 包，stream_key 路径未覆盖\n");
        if (recvd < sent * 90 / 100) {
            printf("  [FAIL] 帧交付率 %u/%u=%.1f%% < 90%%\n",
                   recvd, sent, sent ? recvd*100.0/sent : 0.0);
            pass = 0;
        } else {
            printf("  [PASS] 帧交付率 %.1f%% (sn%%4==1 丢包 + FEC 恢复)\n",
                   sent ? recvd*100.0/sent : 0.0);
        }
        break;

    case S_DUPL:
        /* 重复包去重：filter_win_ 应确保 recv == sent（不多不少）*/
        if (recvd != sent) {
            printf("  [FAIL] 重复包去重异常 recv=%u sent=%u "
                   "(期望相等，filter_win_ 未正确去重)\n", recvd, sent);
            pass = 0;
        } else {
            printf("  [PASS] 重复包去重正确 recv==sent==%u\n", sent);
        }
        break;

    case S_CORRUPT:
        /* 垃圾包不影响接收端稳定性，FEC 恢复 20% 丢弃后交付率 >= 80% */
        if (recvd < sent * 80 / 100) {
            printf("  [FAIL] 错误包场景帧交付率 %u/%u=%.1f%% < 80%%\n",
                   recvd, sent, sent ? recvd*100.0/sent : 0.0);
            pass = 0;
        } else {
            printf("  [PASS] FEC 恢复垃圾注入(20%%)后帧交付率 %.1f%%\n",
                   sent ? recvd*100.0/sent : 0.0);
        }
        break;

    case S_REORDER:
        /* 乱序到达（SN 两两交换）：无丢包，filter_win_ 接受乱序，recv 接近 sent */
        if (recvd < sent * 99 / 100) {
            printf("  [FAIL] 乱序包帧交付率 %u/%u=%.1f%% < 99%%\n",
                   recvd, sent, sent ? recvd*100.0/sent : 0.0);
            pass = 0;
        } else {
            printf("  [PASS] 乱序包帧交付率 %.1f%% (SN 两两交换)\n",
                   sent ? recvd*100.0/sent : 0.0);
        }
        break;

    default:
        break;
    }

    printf("  场景结论: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

/* ──────────────── main ──────────────── */
int main(void) {
    printf("=== fec_key_restore_test — 4 场景回归 ===\n");
    printf("stream_key=0x%016llX  pps=%d  duration=%ds/场景\n\n",
           (unsigned long long)TEST_STREAM_KEY, SEND_PPS, DURATION_S);

    if (GTP_OK != InsLoadGtpModule()) {
        fprintf(stderr, "InsLoadGtpModule failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    /* socket 全程复用（不随场景重建）*/
    g_a.name = "A(sender)";
    g_b.name = "B(recver)";
    g_a.sfd  = make_udp_socket(PORT_A, &g_a.self_addr);
    g_b.sfd  = make_udp_socket(PORT_B, &g_b.self_addr);
    if (g_a.sfd < 0 || g_b.sfd < 0) { RmLoadGtpModule(); return 1; }

    memset(&g_a.peer_addr, 0, sizeof(g_a.peer_addr));
    g_a.peer_addr.sin_family = AF_INET;
    g_a.peer_addr.sin_port   = htons(PORT_B);
    inet_pton(AF_INET, LOOPBACK_IP, &g_a.peer_addr.sin_addr);
    g_a.peer_addr_len = sizeof(g_a.peer_addr);

    memset(&g_b.peer_addr, 0, sizeof(g_b.peer_addr));
    g_b.peer_addr.sin_family = AF_INET;
    g_b.peer_addr.sin_port   = htons(PORT_A);
    inet_pton(AF_INET, LOOPBACK_IP, &g_b.peer_addr.sin_addr);
    g_b.peer_addr_len = sizeof(g_b.peer_addr);

    /* 4 场景顺序执行 */
    int pass = 1;
    pass &= run_scenario(S_FEC_KEY, "S1 FEC_KEY — sn%4==1 确定性丢包 + stream_key 新头验证");
    pass &= run_scenario(S_DUPL,    "S2 DUPL    — 每包发两次（重复包去重）");
    pass &= run_scenario(S_CORRUPT, "S3 CORRUPT — 20%% DATA 替换垃圾（错误包鲁棒性）");
    pass &= run_scenario(S_REORDER, "S4 REORDER — 连续 DATA 两两 SN 交换（乱序包）");

    close(g_a.sfd);
    close(g_b.sfd);
    RmLoadGtpModule();

    printf("\n========== 整体: %s ==========\n\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
