/*
 * fec_key_restore_test.cpp
 *
 * 4 场景回归测试（各 8 秒，50 pps，enable_key_=1）贴近游戏代理真实环境
 *
 * S1 FEC_KEY  随机 25% DATA 丢包 + FEC stream_key_ 新固定头验证（bytes16-23）
 *             覆盖同 2×2 块多丢、V 向/斜向恢复路径；
 *             旧 memmove 方案在 >10% 随机丢包时触发 res_pos!=CalcPosInPackCache
 *
 * S2 DUPL     随机 5% DATA 包重复（模拟 ARQ 重传与原包同时到达、多路由重传）
 *             过去 100% 复制不真实；真实场景重复率 1-5%
 *             验证 filter_win_ 去重：recv_frames == sent_frames，fec_anomaly=0
 *
 * S3 CORRUPT  随机 12% DATA 真实丢弃 + 随机 8% 垃圾注入（轮转 3 种）
 *             - 垃圾类型：4B 纯噪声 / 10B 截断 / 20B 随机字节
 *             - 模拟游戏代理收到非 GTP UDP 流量、端口复用、网络截断包
 *             - 真实包丢弃时额外注入垃圾（而非替换），与真实错包并存
 *             验证接收端对非法包鲁棒，FEC 恢复丢弃后帧交付率 >= 85%
 *
 * S4 REORDER  8 槽随机乱序缓冲区（DATA+FEC 入缓冲；ACK/NACK 直传保 ARQ 响应）
 *             + 5% 随机 DATA 丢包（乱序与丢包同时发生，贴近多路由网络）
 *             - 缓冲达 4 包时随机弹出 1 包，窗口 ≈ 4 包/100pps = 40ms
 *             - 结束后乱序冲刷剩余缓冲（模拟网络清空）
 *             验证 filter_win_ 乱序接受 + FEC+ARQ 恢复，帧交付率 >= 90%
 *
 * 注：多会话删除/重建/重连场景见 reconnect_test.cpp / multi_session_test.cpp
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
    std::atomic<uint32_t> dropped_data{0};  /* 实际丢弃 DATA 包数 */
    std::atomic<uint32_t> fec_anomaly{0};   /* "fec restore abnormal" 次数 */
    std::atomic<uint32_t> fec_key_ok{0};    /* FEC 包 stream_key_ 提取正确 */
    std::atomic<uint32_t> fec_key_bad{0};   /* FEC 包 stream_key_ 提取错误 */
    std::atomic<uint32_t> noise_injected{0};/* S3/S4 注入垃圾包次数 */
    void reset() {
        sent_frames.store(0); recv_frames.store(0); dropped_data.store(0);
        fec_anomaly.store(0); fec_key_ok.store(0);  fec_key_bad.store(0);
        noise_injected.store(0);
    }
};

static Stats            g_stats;
static volatile int     g_running  = 0;
static Scenario         g_scenario = S_FEC_KEY;

/* 随机状态（每场景固定种子，结果可复现）*/
static unsigned int     g_rand_state = 0;

/* S3 错误包类型轮转计数器 */
static int              g_err_type = 0;

/* ──────────────── S4 乱序缓冲区（8 槽）──────────────── */
#define RO_CAP 8
static struct {
    uint8_t  data[4096];
    uint32_t size;
} g_ro[RO_CAP];
static int g_ro_n = 0;

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
 * inject_error — 向 B 端注入一个无效包（S3/S4 用）
 *   类型轮转：
 *     0 → 4B  纯噪声（0xFF/0xAB/0xCD/0xEF）
 *     1 → 10B 全零截断包（size < 最小 GTP 头，必然被 InnerCheck 丢弃）
 *     2 → 20B 随机字节（模拟来自其他协议的 UDP 流量）
 * ────────────────────────────────────────────────────────────────────── */
static void inject_error(int sfd, const struct sockaddr *dst, socklen_t dlen) {
    switch ((g_err_type++) % 3) {
    case 0: {
        uint8_t p[4] = {0xFF, 0xAB, 0xCD, 0xEF};
        sendto(sfd, p, sizeof(p), 0, dst, dlen);
        break;
    }
    case 1: {
        uint8_t p[10] = {0};
        sendto(sfd, p, sizeof(p), 0, dst, dlen);
        break;
    }
    case 2: {
        uint8_t p[20];
        for (int i = 0; i < 20; i++)
            p[i] = (uint8_t)(rand_r(&g_rand_state) & 0xFF);
        sendto(sfd, p, sizeof(p), 0, dst, dlen);
        break;
    }
    }
    g_stats.noise_injected.fetch_add(1);
}

/* ──────────────────────────────────────────────────────────────────────
 * ro_push  — 将一个包放入乱序缓冲区；缓冲达 4 包时随机弹出 1 包发出
 * ro_flush — 场景结束后随机顺序冲刷全部剩余包
 * ────────────────────────────────────────────────────────────────────── */
static void ro_push(const void *pack, uint32_t size,
                    int sfd, const struct sockaddr *dst, socklen_t dlen) {
    if (g_ro_n < RO_CAP && size <= sizeof(g_ro[0].data)) {
        memcpy(g_ro[g_ro_n].data, pack, size);
        g_ro[g_ro_n].size = size;
        g_ro_n++;
    } else {
        /* 缓冲溢出或包过大：直接发出（保证不丢） */
        sendto(sfd, pack, size, 0, dst, dlen);
    }

    /* 缓冲 >= 4 包时随机弹出 1 包 */
    if (g_ro_n >= 4) {
        int pick = (int)((unsigned)rand_r(&g_rand_state) % (unsigned)g_ro_n);
        sendto(sfd, g_ro[pick].data, g_ro[pick].size, 0, dst, dlen);
        /* 用末尾元素填补空位（O(1) 删除）*/
        if (pick != g_ro_n - 1)
            memcpy(&g_ro[pick], &g_ro[g_ro_n - 1], sizeof(g_ro[0]));
        g_ro_n--;
    }
}

static void ro_flush(int sfd, const struct sockaddr *dst, socklen_t dlen) {
    while (g_ro_n > 0) {
        int pick = (int)((unsigned)rand_r(&g_rand_state) % (unsigned)g_ro_n);
        sendto(sfd, g_ro[pick].data, g_ro[pick].size, 0, dst, dlen);
        if (pick != g_ro_n - 1)
            memcpy(&g_ro[pick], &g_ro[g_ro_n - 1], sizeof(g_ro[0]));
        g_ro_n--;
    }
}

/* ──────────────────────────────────────────────────────────────────────
 * A → B 发包回调（根据 g_scenario 决定网络行为）
 *
 * S1 FEC_KEY  25% 随机 DATA 丢弃，测 FEC+stream_key 在高丢包下的正确性
 * S2 DUPL     5% 随机 DATA 重复发送（ARQ 重传/多路由偶发重复）
 * S3 CORRUPT  12% 真实 DATA 丢弃 + 8% 全量包注入垃圾（与真包并发到达 B）
 *             丢弃时同样注入垃圾（模拟被噪声替换）
 * S4 REORDER  5% DATA 随机丢弃；DATA+FEC 进 8 槽乱序缓冲区；ACK/NACK 直传
 * ────────────────────────────────────────────────────────────────────── */
static uint32_t SendPackCbA(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    InstCtx        *ctx  = (InstCtx *)addr->context_;
    const uint8_t   pt   = GTP_RAW_PACK_TYPE(pack);
    struct sockaddr *dst  = (struct sockaddr *)&ctx->peer_addr;
    socklen_t        dlen = ctx->peer_addr_len;

    switch (g_scenario) {

    /* ── S1: 随机 25% DATA 丢弃，覆盖同块多丢 / V 向 / 斜向 FEC 路径 ── */
    case S_FEC_KEY:
        if (pt == GTP_DATA_TYPE && (rand_r(&g_rand_state) % 100) < 25) {
            g_stats.dropped_data.fetch_add(1);
            return GTP_OK;
        }
        sendto(ctx->sfd, pack, size, 0, dst, dlen);
        return GTP_OK;

    /* ── S2: 5% 随机重复（ARQ 重传与原包同时落地 / 多路由重复）── */
    case S_DUPL:
        sendto(ctx->sfd, pack, size, 0, dst, dlen);
        if (pt == GTP_DATA_TYPE && (rand_r(&g_rand_state) % 100) < 5) {
            sendto(ctx->sfd, pack, size, 0, dst, dlen);  /* 额外副本 */
            g_stats.dropped_data.fetch_add(1);  /* 复用字段统计重复次数 */
        }
        return GTP_OK;

    /* ── S3: 12% DATA 真实丢弃 + 8% 全包垃圾并发注入 ── */
    case S_CORRUPT: {
        int do_drop = (pt == GTP_DATA_TYPE) && ((rand_r(&g_rand_state) % 100) < 12);
        int do_noise = (rand_r(&g_rand_state) % 100) < 8;
        if (do_drop) {
            g_stats.dropped_data.fetch_add(1);
            inject_error(ctx->sfd, dst, dlen);   /* 丢包时用噪声替换 */
        } else {
            if (do_noise)
                inject_error(ctx->sfd, dst, dlen);  /* 真包前注入额外噪声 */
            sendto(ctx->sfd, pack, size, 0, dst, dlen);
        }
        return GTP_OK;
    }

    /* ── S4: 5% DATA 丢弃 + 8 槽随机乱序缓冲（DATA+FEC），ACK/NACK 直传 ── */
    case S_REORDER:
        if (pt == GTP_DATA_TYPE && (rand_r(&g_rand_state) % 100) < 5) {
            g_stats.dropped_data.fetch_add(1);
            return GTP_OK;
        }
        if (pt == GTP_DATA_TYPE || pt == GTP_FEC_TYPE) {
            ro_push(pack, size, ctx->sfd, dst, dlen);
        } else {
            sendto(ctx->sfd, pack, size, 0, dst, dlen);  /* ACK/NACK 直传 */
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
    int              is_b;
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

        /* B 端：提取并验证 FEC 包 stream_key_（新固定头部格式）
         * 注意：S3 会注入 4/10/20B 垃圾包，它们不是有效 GTP 包，
         * 但随机字节可能使 byte[2]&0x07==FEC_TYPE(0x03)，导致误读 stream_key_。
         * 真实 GTP FEC 包至少有 Fec2CodePack(24B)+XOR数据，远大于 40B，
         * 用 n >= 40 门卫排除所有注入垃圾，仅对真实包验证 key。 */
        if (a->is_b) {
            uint64_t sk = 0;
            if ((uint32_t)n >= 40) {
                GtpCheckPacketInvalid(mem, (uint32_t)n, &sk);
                if (GTP_FEC_TYPE == GTP_RAW_PACK_TYPE(mem)) {
                    if (sk == TEST_STREAM_KEY) g_stats.fec_key_ok.fetch_add(1);
                    else                       g_stats.fec_key_bad.fetch_add(1);
                }
            }
            /* 垃圾包 sk=0，enable_key_=1 时找不到对应 session → 被 GTP 层丢弃 */
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
    struct timeval tv = {0, 200000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    inet_pton(AF_INET, LOOPBACK_IP, &out->sin_addr);
    return fd;
}

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
    g_scenario   = sc;
    g_ro_n       = 0;
    g_err_type   = 0;
    g_rand_state = 0x5EED1234u;
    g_running    = 1;

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

    RecvArg  ra    = {&g_a, g_a.gtp_hdl, &g_lock_a, 0};
    RecvArg  rb    = {&g_b, g_b.gtp_hdl, &g_lock_b, 1};
    TimerArg ta    = {g_a.gtp_hdl, g_b.gtp_hdl, &g_lock_a, &g_lock_b};
    pthread_t tid_ra, tid_rb, tid_timer;
    pthread_create(&tid_ra,    NULL, recv_thread,  &ra);
    pthread_create(&tid_rb,    NULL, recv_thread,  &rb);
    pthread_create(&tid_timer, NULL, timer_thread, &ta);

    const uint64_t interval  = 1000000ULL / SEND_PPS;
    const uint64_t deadline  = now_us() + (uint64_t)DURATION_S * 1000000ULL;
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

    /* S4: 随机顺序冲刷乱序缓冲区剩余包（模拟网络最终清空） */
    if (sc == S_REORDER && g_ro_n > 0) {
        pthread_mutex_lock(&g_lock_a);
        ro_flush(g_a.sfd,
                 (struct sockaddr *)&g_a.peer_addr,
                 g_a.peer_addr_len);
        pthread_mutex_unlock(&g_lock_a);
    }

    g_running = 0;
    pthread_join(tid_ra,    NULL);
    pthread_join(tid_rb,    NULL);
    pthread_join(tid_timer, NULL);

    DeleteGtpInstance(g_a.gtp_hdl);
    DeleteGtpInstance(g_b.gtp_hdl);
    g_a.gtp_hdl = INVALID_GTP_HANDLER;
    g_b.gtp_hdl = INVALID_GTP_HANDLER;
    drain_sockets();

    /* ──── 统计与判定 ──── */
    uint32_t sent  = g_stats.sent_frames.load();
    uint32_t recvd = g_stats.recv_frames.load();
    uint32_t drop  = g_stats.dropped_data.load();
    uint32_t anom  = g_stats.fec_anomaly.load();
    uint32_t fok   = g_stats.fec_key_ok.load();
    uint32_t fbad  = g_stats.fec_key_bad.load();
    uint32_t noise = g_stats.noise_injected.load();

    if (sc == S_DUPL)
        printf("发送:%u 接收:%u 重复包:%u 垃圾注入:%u fec_key_ok:%u "
               "fec_key_bad:%u fec_anomaly:%u\n",
               sent, recvd, drop, noise, fok, fbad, anom);
    else
        printf("发送:%u 接收:%u 丢DATA:%u(%.0f%%) 垃圾注入:%u "
               "fec_key_ok:%u fec_key_bad:%u fec_anomaly:%u\n",
               sent, recvd, drop, sent ? drop*100.0/sent : 0.0,
               noise, fok, fbad, anom);

    int pass = 1;

    /* 所有场景公共断言 */
    if (anom > 0) {
        printf("  [FAIL] fec_anomaly=%u  "
               "(旧 memmove 方案触发 res_pos!=CalcPosInPackCache)\n", anom);
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
            printf("  [WARN] fec_key_ok=0：FEC 路径未被覆盖\n");
        if (recvd < sent * 90 / 100) {
            printf("  [FAIL] 帧交付率 %u/%u=%.1f%% < 90%%\n",
                   recvd, sent, sent ? recvd*100.0/sent : 0.0);
            pass = 0;
        } else {
            printf("  [PASS] 帧交付率 %.1f%% (随机%.0f%%丢包 + FEC恢复)\n",
                   sent ? recvd*100.0/sent : 0.0,
                   sent ? drop*100.0/sent : 0.0);
        }
        break;

    case S_DUPL:
        /* 5% 随机重复：filter_win_ 去重，recv 应精确等于 sent */
        if (recvd != sent) {
            printf("  [FAIL] 重复包去重异常: recv=%u  sent=%u  "
                   "(期望相等，重复了 %u 次)\n", recvd, sent, drop);
            pass = 0;
        } else {
            printf("  [PASS] 重复包去重正确 recv==sent==%u  (5%%随机重复 %u 次)\n",
                   sent, drop);
        }
        break;

    case S_CORRUPT:
        /* 12% 真实丢弃 + 噪声注入；FEC book4 应恢复大部分 */
        if (recvd < sent * 85 / 100) {
            printf("  [FAIL] 帧交付率 %u/%u=%.1f%% < 85%%\n",
                   recvd, sent, sent ? recvd*100.0/sent : 0.0);
            pass = 0;
        } else {
            printf("  [PASS] 帧交付率 %.1f%%  "
                   "(12%%丢弃+%u次垃圾注入，FEC恢复)\n",
                   sent ? recvd*100.0/sent : 0.0, noise);
        }
        break;

    case S_REORDER:
        /* 8 槽乱序 + 5% 丢包；FEC+ARQ 应覆盖大部分损失 */
        if (recvd < sent * 90 / 100) {
            printf("  [FAIL] 帧交付率 %u/%u=%.1f%% < 90%%\n",
                   recvd, sent, sent ? recvd*100.0/sent : 0.0);
            pass = 0;
        } else {
            printf("  [PASS] 帧交付率 %.1f%%  "
                   "(8槽随机乱序 + %.0f%%丢包，FEC+ARQ恢复)\n",
                   sent ? recvd*100.0/sent : 0.0,
                   sent ? drop*100.0/sent : 0.0);
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

    int pass = 1;
    pass &= run_scenario(S_FEC_KEY, "S1 FEC_KEY  随机25%%丢包 + stream_key 新固定头验证");
    pass &= run_scenario(S_DUPL,    "S2 DUPL     5%%随机重复包（ARQ重传/多路由偶发）");
    pass &= run_scenario(S_CORRUPT, "S3 CORRUPT  12%%真实丢弃 + 8%%垃圾并发注入");
    pass &= run_scenario(S_REORDER, "S4 REORDER  8槽随机乱序缓冲 + 5%%随机丢包");

    close(g_a.sfd);
    close(g_b.sfd);
    RmLoadGtpModule();

    printf("\n========== 整体: %s ==========\n\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
