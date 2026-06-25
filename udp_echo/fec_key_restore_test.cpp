/*
 * fec_key_restore_test.cpp
 *
 * 游戏代理端对端质量测试（kReliableStream，各 8+2s，50 pps）
 *
 * 目标：无论网络层发生什么（丢包/重复包/错包/乱序包），
 *       GoodTP 经过 FEC+ARQ 后交付给应用层的帧必须满足：
 *         dup_frames   == 0  （无重复帧）
 *         ooo_frames   == 0  （无乱序帧，kReliableStream sort_buf_ 保序）
 *         corrupt_frames == 0（无数据损坏帧，CRC 验证）
 *
 * 网络注入条件（SendPackCbA 模拟，不影响 B→A 方向的 ACK/NACK）：
 *   S1 FEC_KEY  随机 25% DATA 包丢弃       → FEC+ARQ 恢复
 *   S2 DUPL     随机 5%  DATA 包额外重发   → filter_win_ 去重
 *   S3 CORRUPT  随机 12% DATA 包丢弃       → FEC 恢复
 *               + 随机 8% 全包垃圾并发注入 → 接收端健壮丢弃
 *   S4 REORDER  随机 5%  DATA 包丢弃       → FEC 恢复
 *               + 相邻 DATA 包两两 SN 交换 → sort_buf_ 重排保序
 *               （pair-swap 引入 ~20ms 延迟，< gap_timer=40ms，安全）
 *
 * 同时验证 FEC stream_key_ 新固定头格式（Fec2CodePack bytes16-23）
 *   旧 memmove 方案在 >10% 随机丢包时产生 res_pos!=CalcPosInPackCache
 *
 * 注：多会话删除/重建/重连见 reconnect_test.cpp / multi_session_test.cpp
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

/* ──────────────── 包头原始字段（小端）──────────────── */
#define GTP_RAW_PACK_TYPE(p)  (((const uint8_t*)(p))[2] & 0x07u)
#define GTP_DATA_TYPE  0x01u
#define GTP_FEC_TYPE   0x03u

/* ──────────────── 测试常量 ──────────────── */
#define TEST_STREAM_KEY  0x1234567890ABCDEFULL
#define LOOPBACK_IP      "127.0.0.1"
#define PORT_A           19031
#define PORT_B           19032
#define TIMER_US         10000
#define SEND_PPS         50
#define SEND_DURATION_S  8    /* 发送时长 */
#define DRAIN_S          2    /* 发送后额外等 sort_buf_ 排空 */

/* ──────────────── 帧格式（含 CRC，检测 FEC 恢复数据损坏）──────────────── */
typedef struct TestFrame {
    uint32_t magic;     /* 0xDEADBEEF */
    uint32_t seq;       /* 单调递增，从 1 开始 */
    uint64_t send_ts;
    uint32_t crc;       /* = magic ^ seq ^ hi32(send_ts) ^ lo32(send_ts) */
    char     data[60];
} TestFrame;

static uint32_t frame_crc(const TestFrame *f) {
    return f->magic ^ f->seq
           ^ (uint32_t)(f->send_ts >> 32)
           ^ (uint32_t)(f->send_ts);
}

/* ──────────────── 场景 ──────────────── */
typedef enum {
    S_FEC_KEY = 0,
    S_DUPL    = 1,
    S_CORRUPT = 2,
    S_REORDER = 3,
    S_MAX     = 4
} Scenario;

/* ──────────────────────────────────────────────────────────────────────
 * 统计
 *   fec_anomaly / fec_key_bad / fec_key_ok — 协议层质量
 *   dup_frames / ooo_frames / corrupt_frames — 应用层质量（必须全为 0）
 * ────────────────────────────────────────────────────────────────────── */
struct Stats {
    std::atomic<uint32_t> sent_frames{0};
    std::atomic<uint32_t> recv_frames{0};
    std::atomic<uint32_t> dropped_data{0};
    std::atomic<uint32_t> fec_anomaly{0};
    std::atomic<uint32_t> fec_key_ok{0};
    std::atomic<uint32_t> fec_key_bad{0};
    std::atomic<uint32_t> noise_injected{0};
    /* ── 应用层质量指标（游戏代理核心保证）── */
    std::atomic<uint32_t> dup_frames{0};
    std::atomic<uint32_t> ooo_frames{0};
    std::atomic<uint32_t> corrupt_frames{0};
    void reset() {
        sent_frames=0; recv_frames=0; dropped_data=0;
        fec_anomaly=0; fec_key_ok=0;  fec_key_bad=0; noise_injected=0;
        dup_frames=0;  ooo_frames=0;  corrupt_frames=0;
    }
};

static Stats            g_stats;
static volatile int     g_running  = 0;
static Scenario         g_scenario = S_FEC_KEY;
static unsigned int     g_rand_state = 0;
static int              g_err_type   = 0;

/* B 端应用层序号跟踪（RecvFrameCbB 内，受 g_lock_b 保护，无需 atomic）*/
static uint32_t g_recv_last_seq = 0;

/* S4 乱序：暂存一个 DATA 包，下一个到时两两交换（pair-swap）*/
static uint8_t  g_held_pack[4096];
static int      g_held_pack_size = 0;

/* ──────────────── 实例上下文 ──────────────── */
typedef struct InstCtx {
    const char        *name;
    int                sfd;
    GtpHandler_p       gtp_hdl;
    struct sockaddr_in self_addr;
    struct sockaddr_in peer_addr;
    socklen_t          peer_addr_len;
} InstCtx;

static InstCtx          g_a;
static InstCtx          g_b;
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
    char buf[512];
    va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (strstr(buf, "fec restore abnormal"))
        g_stats.fec_anomaly.fetch_add(1);
    if (level > 3) return;
    static const char *lv[] = {"EMRG","ALRT","CRIT","ERR"};
    printf("[gtp-%s] %s", lv[level], buf);
}
static uint32_t LogLevelCb(void) { return 3; }

/* ──────────────── S3 垃圾注入辅助 ──────────────── */
static void inject_error(int sfd, const struct sockaddr *dst, socklen_t dlen) {
    switch ((g_err_type++) % 3) {
    case 0: { uint8_t p[4]  = {0xFF,0xAB,0xCD,0xEF}; sendto(sfd,p,4,0,dst,dlen); break; }
    case 1: { uint8_t p[10] = {0};                    sendto(sfd,p,10,0,dst,dlen); break; }
    case 2: { uint8_t p[20];
               for(int i=0;i<20;i++) p[i]=(uint8_t)(rand_r(&g_rand_state)&0xFF);
               sendto(sfd,p,20,0,dst,dlen); break; }
    }
    g_stats.noise_injected.fetch_add(1);
}

/* ──────────────────────────────────────────────────────────────────────
 * A → B 发包回调（模拟不同网络注入条件）
 *
 * 注意：B→A 方向（ACK/NACK）通过 SendPackCbB 直通，不施加任何注入。
 *       这确保 ARQ 确认路径正常，隔离被测变量。
 * ────────────────────────────────────────────────────────────────────── */
static uint32_t SendPackCbA(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    InstCtx        *ctx  = (InstCtx *)addr->context_;
    const uint8_t   pt   = GTP_RAW_PACK_TYPE(pack);
    struct sockaddr *dst  = (struct sockaddr *)&ctx->peer_addr;
    socklen_t        dlen = ctx->peer_addr_len;

    switch (g_scenario) {

    /* ── S1: 随机 25% DATA 丢弃（FEC+ARQ 恢复路径压测）── */
    case S_FEC_KEY:
        if (pt == GTP_DATA_TYPE && (rand_r(&g_rand_state) % 100) < 25) {
            g_stats.dropped_data.fetch_add(1);
            return GTP_OK;
        }
        sendto(ctx->sfd, pack, size, 0, dst, dlen);
        return GTP_OK;

    /* ── S2: 随机 5% DATA 额外重发（网络/ARQ 重复包，filter_win_ 去重）── */
    case S_DUPL:
        sendto(ctx->sfd, pack, size, 0, dst, dlen);
        if (pt == GTP_DATA_TYPE && (rand_r(&g_rand_state) % 100) < 5) {
            sendto(ctx->sfd, pack, size, 0, dst, dlen);
            g_stats.dropped_data.fetch_add(1);  /* 复用字段计重复次数 */
        }
        return GTP_OK;

    /* ── S3: 12% DATA 丢弃 + 8% 垃圾并发注入（混合错误场景）── */
    case S_CORRUPT: {
        int do_drop  = (pt == GTP_DATA_TYPE) && ((rand_r(&g_rand_state) % 100) < 12);
        int do_noise = (rand_r(&g_rand_state) % 100) < 8;
        if (do_drop) {
            g_stats.dropped_data.fetch_add(1);
            inject_error(ctx->sfd, dst, dlen);  /* 丢包时用垃圾替换 */
        } else {
            if (do_noise)
                inject_error(ctx->sfd, dst, dlen);  /* 在真包前注入额外垃圾 */
            sendto(ctx->sfd, pack, size, 0, dst, dlen);
        }
        return GTP_OK;
    }

    /* ── S4: 5% DATA 丢弃 + DATA 两两 SN 交换（sort_buf_ 重排验证）──
     *
     * pair-swap 引入约 20ms 延迟（50pps 时 1 包间隔）。
     * gap_timer = 2000000/50 = 40ms > 20ms，sort_buf_ 能在 gap_timer
     * 触发前等到交换后的包，并按 GTP-SN 顺序交付，APP 看到严格递增 seq。
     * FEC/ACK/NACK 直通，保证 FEC 奇偶和 ARQ 反馈不受乱序影响。         */
    case S_REORDER:
        if (pt == GTP_DATA_TYPE) {
            if ((rand_r(&g_rand_state) % 100) < 5) {
                g_stats.dropped_data.fetch_add(1);
                return GTP_OK;
            }
            if (g_held_pack_size == 0) {
                if ((int)size <= (int)sizeof(g_held_pack)) {
                    memcpy(g_held_pack, pack, size);
                    g_held_pack_size = (int)size;
                } else {
                    sendto(ctx->sfd, pack, size, 0, dst, dlen);
                }
            } else {
                /* 先发 SN 较大的（当前），再发暂存的 SN 较小的 */
                sendto(ctx->sfd, pack, size, 0, dst, dlen);
                sendto(ctx->sfd, g_held_pack, g_held_pack_size, 0, dst, dlen);
                g_held_pack_size = 0;
            }
        } else {
            /* FEC / ACK / NACK 直通 */
            sendto(ctx->sfd, pack, size, 0, dst, dlen);
        }
        return GTP_OK;

    default:
        sendto(ctx->sfd, pack, size, 0, dst, dlen);
        return GTP_OK;
    }
}

/* ──────────────────────────────────────────────────────────────────────
 * B 收帧回调 — 应用层质量三重验证
 *
 *   1. corrupt_frames  magic 或 CRC 错误 → FEC 恢复数据损坏
 *   2. dup_frames      seq == last_seq   → 重复帧上报
 *   3. ooo_frames      seq <  last_seq   → 乱序帧上报
 *
 * 使用 kReliableStream：sort_buf_ 按 GTP-SN 重排后再调用此回调，
 * 正常情况下 seq 必然严格递增（可能有 gap，但不会回退）。
 * ────────────────────────────────────────────────────────────────────── */
static uint32_t RecvFrameCbB(GtpHandler_p hdl, void *frame, uint32_t size, GtpAddr *addr) {
    (void)hdl; (void)addr;

    if (size < sizeof(TestFrame)) {
        g_stats.corrupt_frames.fetch_add(1);
        return GTP_OK;
    }
    TestFrame *f = (TestFrame *)frame;

    if (f->magic != 0xDEADBEEF || f->crc != frame_crc(f)) {
        g_stats.corrupt_frames.fetch_add(1);
        return GTP_OK;
    }

    uint32_t seq = f->seq;

    if (g_recv_last_seq > 0 && seq <= g_recv_last_seq) {
        if (seq == g_recv_last_seq) {
            printf("  [ERR] DUP  seq=%u (last=%u)\n", seq, g_recv_last_seq);
            g_stats.dup_frames.fetch_add(1);
        } else {
            printf("  [ERR] OOO  seq=%u (last=%u)\n", seq, g_recv_last_seq);
            g_stats.ooo_frames.fetch_add(1);
        }
    }
    if (seq > g_recv_last_seq)
        g_recv_last_seq = seq;

    g_stats.recv_frames.fetch_add(1);
    return GTP_OK;
}

/* B → A 直通（ACK/NACK 不注入任何干扰）*/
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
        taddr->stream_type_   = kReliableStream;   /* sort_buf_ 保序 */
        taddr->self_addr_len_ = (uint32_t)sizeof(ctx->self_addr);
        memcpy(taddr->self_addr_, &ctx->self_addr, sizeof(ctx->self_addr));
        taddr->sock_addr_len_ = (uint32_t)peer_len;
        memcpy(taddr->sock_addr_, &peer, peer_len);

        /* B 端：验证 FEC 包 stream_key_（新固定头 bytes16-23）
         * n>=40 门卫排除 4/10/20B 垃圾包（其 byte[2]&7 可能偶发==FEC_TYPE）*/
        if (a->is_b) {
            uint64_t sk = 0;
            if ((uint32_t)n >= 40) {
                GtpCheckPacketInvalid(mem, (uint32_t)n, &sk);
                if (GTP_FEC_TYPE == GTP_RAW_PACK_TYPE(mem)) {
                    if (sk == TEST_STREAM_KEY) g_stats.fec_key_ok.fetch_add(1);
                    else                       g_stats.fec_key_bad.fetch_add(1);
                }
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
 * run_scenario
 * ────────────────────────────────────────────────────────────────────── */
static int run_scenario(Scenario sc, const char *label) {
    printf("\n====== %s ======\n", label);

    g_stats.reset();
    g_scenario        = sc;
    g_held_pack_size  = 0;
    g_err_type        = 0;
    g_rand_state      = 0x5EED1234u;
    g_recv_last_seq   = 0;
    g_running         = 1;

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

    /* 发送循环 */
    const uint64_t interval  = 1000000ULL / SEND_PPS;
    const uint64_t deadline  = now_us() + (uint64_t)SEND_DURATION_S * 1000000ULL;
    uint64_t       next_send = now_us();
    uint32_t       seq       = 0;

    while (now_us() < deadline) {
        if (now_us() >= next_send) {
            TestFrame f;
            f.magic   = 0xDEADBEEF;
            f.seq     = ++seq;
            f.send_ts = now_us();
            f.crc     = frame_crc(&f);
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
                taddr->stream_type_   = kReliableStream;
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

    /* S4: 冲刷暂存的最后一个 DATA 包 */
    if (sc == S_REORDER && g_held_pack_size > 0) {
        pthread_mutex_lock(&g_lock_a);
        sendto(g_a.sfd, g_held_pack, g_held_pack_size, 0,
               (struct sockaddr *)&g_a.peer_addr, g_a.peer_addr_len);
        g_held_pack_size = 0;
        pthread_mutex_unlock(&g_lock_a);
    }

    /* 排空等待：让 sort_buf_ 冲刷剩余帧、ARQ 完成最后重传 */
    printf("  [等待 %ds 让 sort_buf_ 和 ARQ 排空...]\n", DRAIN_S);
    uint64_t drain_end = now_us() + (uint64_t)DRAIN_S * 1000000ULL;
    while (now_us() < drain_end) usleep(10000);

    g_running = 0;
    pthread_join(tid_ra,    NULL);
    pthread_join(tid_rb,    NULL);
    pthread_join(tid_timer, NULL);

    DeleteGtpInstance(g_a.gtp_hdl);
    DeleteGtpInstance(g_b.gtp_hdl);
    g_a.gtp_hdl = INVALID_GTP_HANDLER;
    g_b.gtp_hdl = INVALID_GTP_HANDLER;
    drain_sockets();

    /* ──── 结果统计 ──── */
    uint32_t sent    = g_stats.sent_frames.load();
    uint32_t recvd   = g_stats.recv_frames.load();
    uint32_t drop    = g_stats.dropped_data.load();
    uint32_t anom    = g_stats.fec_anomaly.load();
    uint32_t fok     = g_stats.fec_key_ok.load();
    uint32_t fbad    = g_stats.fec_key_bad.load();
    uint32_t noise   = g_stats.noise_injected.load();
    uint32_t dup     = g_stats.dup_frames.load();
    uint32_t ooo     = g_stats.ooo_frames.load();
    uint32_t corrupt = g_stats.corrupt_frames.load();

    printf("  发送帧:%-4u  接收帧:%-4u  丢DATA:%-3u(%.0f%%)  垃圾注入:%-3u\n",
           sent, recvd, drop, sent ? drop*100.0/sent : 0.0, noise);
    printf("  fec_key_ok:%-4u  fec_key_bad:%-2u  fec_anomaly:%-2u\n",
           fok, fbad, anom);
    printf("  [应用层] dup:%u  ooo:%u  corrupt:%u\n", dup, ooo, corrupt);

    int pass = 1;

    /* ── 协议层断言 ── */
    if (anom > 0) {
        printf("  [FAIL] fec_anomaly=%u (旧 memmove 触发 res_pos!=CalcPosInPackCache)\n", anom);
        pass = 0;
    }
    if (fbad > 0) {
        printf("  [FAIL] fec_key_bad=%u (FEC stream_key_ 新固定头提取失败)\n", fbad);
        pass = 0;
    }
    if (fok == 0)
        printf("  [WARN] fec_key_ok=0 (FEC 路径未被覆盖)\n");

    /* ── 应用层三重断言（核心）── */
    if (dup > 0) {
        printf("  [FAIL] dup_frames=%u  — 应用层收到重复帧，filter_win_ 去重失效\n", dup);
        pass = 0;
    } else {
        printf("  [PASS] dup_frames=0  — 无重复帧\n");
    }
    if (ooo > 0) {
        printf("  [FAIL] ooo_frames=%u  — 应用层收到乱序帧，sort_buf_ 保序失效\n", ooo);
        pass = 0;
    } else {
        printf("  [PASS] ooo_frames=0  — 无乱序帧（kReliableStream sort_buf_ 保序）\n");
    }
    if (corrupt > 0) {
        printf("  [FAIL] corrupt_frames=%u  — 数据损坏，FEC XOR 恢复出错\n", corrupt);
        pass = 0;
    } else {
        printf("  [PASS] corrupt_frames=0  — 无数据损坏（CRC 验证通过）\n");
    }

    /* ── 交付率断言 ── */
    uint32_t threshold;
    switch (sc) {
    case S_FEC_KEY: threshold = sent * 85 / 100; break;
    case S_DUPL:    threshold = sent;             break;
    case S_CORRUPT: threshold = sent * 80 / 100; break;
    case S_REORDER: threshold = sent * 85 / 100; break;
    default:        threshold = sent * 80 / 100; break;
    }
    if (recvd < threshold) {
        printf("  [FAIL] 帧交付率 %u/%u=%.1f%% 低于阈值\n",
               recvd, sent, sent ? recvd*100.0/sent : 0.0);
        pass = 0;
    } else {
        printf("  [PASS] 帧交付率 %.1f%%\n", sent ? recvd*100.0/sent : 0.0);
    }

    printf("  场景结论: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

/* ──────────────── main ──────────────── */
int main(void) {
    printf("=== fec_key_restore_test — 游戏代理端对端质量验证 ===\n");
    printf("kReliableStream  stream_key=0x%016llX  pps=%d\n"
           "网络注入→FEC+ARQ恢复→应用层必须: dup=0 ooo=0 corrupt=0\n\n",
           (unsigned long long)TEST_STREAM_KEY, SEND_PPS);

    if (GTP_OK != InsLoadGtpModule()) {
        fprintf(stderr, "InsLoadGtpModule failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    g_a.name = "A(game-client proxy)";
    g_b.name = "B(game-server proxy)";
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
    pass &= run_scenario(S_FEC_KEY, "S1 随机25%丢包         → FEC+ARQ恢复，验证应用层无dup/ooo/corrupt");
    pass &= run_scenario(S_DUPL,    "S2 5%随机重复包        → filter_win_去重，验证应用层无重复帧");
    pass &= run_scenario(S_CORRUPT, "S3 12%丢+8%垃圾注入   → FEC恢复+健壮丢弃，验证应用层无损坏帧");
    pass &= run_scenario(S_REORDER, "S4 5%丢+pair-swap乱序  → sort_buf_保序，验证应用层无乱序帧");

    close(g_a.sfd);
    close(g_b.sfd);
    RmLoadGtpModule();

    printf("\n========== 整体: %s ==========\n\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
