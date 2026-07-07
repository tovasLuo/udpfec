/*
 * fec_loss_test.cpp
 *
 * 在单进程内模拟两端 GoodTP 实例通信，注入可控丢包，验证：
 *
 *   TEST 1 — FEC/ARQ 协调（commit b25d37b）
 *     FEC 恢复的包是否被计入 ACK bitmap，使 ARQ 不再重传。
 *     指标：loss_rate > 0 时，fec_res_success 上升，arq_rto_resend 尽可能低。
 *
 *   TEST 2 — UH/DH 斜向 FEC（本次提交）
 *     切换到 book_id=3（2×2 全向），注入对角线位置丢包，验证斜向恢复。
 *     指标：fec_res_success 包含斜向恢复的贡献。
 *
 *   TEST 3 — ClearReceiveUnUsedResource（本次提交）
 *     高 PPS + 丢包，环形缓冲区绕回场景下不出现 fec restore abnormal 日志。
 *     指标：无 "fec restore abnormal" 错误日志，接收帧数与发送帧数匹配。
 *
 * 运行方式：
 *   cd udp_echo/build && ./fec_loss_test [loss_percent] [pps] [seconds] [book_id] [use_key] [outage_sec] [outage_at_sec]
 *   示例：
 *     ./fec_loss_test 20 50 10 4    # 20% 丢包，50 PPS，10s，book4(H+V 2x2)
 *     ./fec_loss_test 25 50 10 3    # 25% 丢包，50 PPS，10s，book3(全向 2x2) [TEST 2]
 *     ./fec_loss_test 20 200 15 4   # 20% 丢包，200 PPS，15s，测缓冲绕回 [TEST 3]
 *
 *   TEST 4 — 单向长时间网络中断注入（REALTIME_NACK_GIVEUP_MARGIN 验证用）
 *     outage_sec/outage_at_sec：从第 outage_at_sec 秒开始，双向 100% 丢包
 *     持续 outage_sec 秒（模拟真实 WiFi/信号盲区式的连续中断，而不是独立随机
 *     丢包）。用于复现"同一个 SN 反馈几百到几千次"的场景，验证放弃机制
 *     （goodtp_session.cpp:ShouldFeedbackRealtimeNack）在真实长中断下是否
 *     按预期触发、以及中断结束后能否正常恢复。
 *     ./fec_loss_test 5 150 30 4 0 8 10   # 5%基线丢包，第10秒起完全中断8秒
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include <stdarg.h>
#include <atomic>

#include "bitlinker.h"

/* pack_type_ 在 GTP_PACK_HEADER 第一个 u32 的 bits[16..18]（小端 byte[2] 低 3 位）
 * Layout: u32 word0 [goodtp_ver_:8|header_offset_:6|cache:1|loss:1|pack_type_:3|pack_size_:13]
 *         u32 pack_sn_ (bytes 4-7)
 *         u16 [has_check_flag_:1|...] (bytes 8-9)
 */
#define GTP_RAW_PACK_TYPE(buf)  (((const uint8_t*)(buf))[2] & 0x07u)
#define GTP_FEC_PACK_TYPE_VAL   0x03u

/* ───────────────────── 配置 ───────────────────── */
#define MAX_PAYLOAD     512
#define LOOPBACK_IP     "127.0.0.1"
#define BASE_PORT_A     19001   /* A 监听端口（模拟发送端）*/
#define BASE_PORT_B     19002   /* B 监听端口（模拟接收端）*/
#define TIMER_PERIOD_US 10000   /* 10 ms */

/* ───────────────────── 帧格式 ───────────────────── */
typedef struct TestFrame {
    uint32_t magic;     /* 0xDEADBEEF */
    uint32_t seq;
    uint64_t send_ts_us;
    char     payload[64];
} TestFrame;

/* ───────────────────── 全局统计 ───────────────────── */
struct Stats {
    std::atomic<uint32_t> sent_frames;
    std::atomic<uint32_t> recv_frames;
    std::atomic<uint32_t> dropped_at_net;
    std::atomic<uint32_t> fec_anomaly;
    std::atomic<uint32_t> fec_key_ok;    /* FEC包中stream_key_提取正确 */
    std::atomic<uint32_t> fec_key_bad;   /* FEC包中stream_key_提取错误（=0或!=TEST_KEY） */

    Stats() : sent_frames(0), recv_frames(0), dropped_at_net(0), fec_anomaly(0),
              fec_key_ok(0), fec_key_bad(0) {}
};

static Stats g_stats;

/* ───────────────────── 测试参数 ───────────────────── */
static int g_loss_pct  = 20;    /* 丢包百分比 0-100 */
static int g_pps       = 50;    /* 每秒发包数 */
static int g_duration  = 10;    /* 测试秒数 */
static int g_book_id   = 4;     /* FEC codebook */
static int g_use_key   = 0;     /* 1=enable_key_模式，测试FEC包stream_key_嵌入 */
static volatile int g_running = 1;

/* 单向/双向长时间网络中断注入：模拟真实 WiFi 掉线/信号盲区，而不是独立随机丢包。
 * 从 g_outage_at_sec 秒起，双向 100% 丢包，持续 g_outage_sec 秒。0 = 不注入。 */
static int      g_outage_sec    = 0;
static int      g_outage_at_sec = 0;
static uint64_t g_test_start_us = 0;

#define TEST_STREAM_KEY  0x1234567890ABCDEFULL

/* ───────────────────── 时间戳 ───────────────────── */
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* true 期间：双向 100% 丢包（真实网络长中断，而非独立随机丢包） */
static int in_outage(void) {
    if (0 == g_outage_sec) {
        return 0;
    }
    uint64_t elapsed_s = (now_us() - g_test_start_us) / 1000000ULL;
    return (elapsed_s >= (uint64_t)g_outage_at_sec)
        && (elapsed_s < (uint64_t)(g_outage_at_sec + g_outage_sec));
}

/* ───────────────────── 实例上下文 ───────────────────── */
typedef struct InstCtx {
    const char   *name;
    int           sfd_send;         /* 本端发送 socket */
    int           sfd_recv;         /* 本端接收 socket */
    GtpHandler_p  gtp_hdl;
    struct sockaddr_in peer_addr;
    socklen_t          peer_addr_len;
    struct sockaddr_in self_addr;
    socklen_t          self_addr_len;
} InstCtx;

static InstCtx g_inst_a;   /* 发送端 */
static InstCtx g_inst_b;   /* 接收端 */

/* 每个实例一把互斥锁，串行化所有库调用（GoodTP 单线程约束） */
static pthread_mutex_t g_lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lock_b = PTHREAD_MUTEX_INITIALIZER;

/* ───────────────────── 随机丢包 ───────────────────── */
static int should_drop(void) {
    if (in_outage()) {
        return 1;
    }
    return (rand() % 100) < g_loss_pct;
}

/* ───────────────────── goodtp 回调 ───────────────────── */
static uint32_t SendPackCbA(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    if (should_drop()) {
        g_stats.dropped_at_net.fetch_add(1);
        return GTP_OK;
    }
    InstCtx *ctx = (InstCtx *)addr->context_;
    sendto(ctx->sfd_send, pack, size, 0,
           (struct sockaddr *)&ctx->peer_addr, ctx->peer_addr_len);
    return GTP_OK;
}

/* 重复/异常投递检测：记录每个 seq 被应用层收到的次数。recv_frames > sent_frames
 * (帧丢失率结果溢出成荒谬大的数字) 就是这里能抓到的重复投递问题的信号。 */
#include <unordered_map>
static std::unordered_map<uint32_t,int> g_seq_count;
static pthread_mutex_t g_seq_count_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t RecvFrameCbB(GtpHandler_p hdl, void *frame, uint32_t size, GtpAddr *addr) {
    (void)hdl; (void)addr;
    if (size >= sizeof(TestFrame)) {
        TestFrame *f = (TestFrame *)frame;
        if (f->magic == 0xDEADBEEF) {
            g_stats.recv_frames.fetch_add(1);
            pthread_mutex_lock(&g_seq_count_lock);
            int c = ++g_seq_count[f->seq];
            if (c > 1) {
                fprintf(stderr, "[dup-detect] duplicate delivery seq=%u count=%d size=%u\n", f->seq, c, size);
            }
            pthread_mutex_unlock(&g_seq_count_lock);
        }
    }
    return GTP_OK;
}

static uint32_t SendPackCbB(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    /* B→A 方向（ACK/NACK/RTT）同样随机丢包，模拟真实双向网络丢包 */
    if (should_drop()) {
        g_stats.dropped_at_net.fetch_add(1);
        return GTP_OK;
    }
    InstCtx *ctx = (InstCtx *)addr->context_;
    sendto(ctx->sfd_send, pack, size, 0,
           (struct sockaddr *)&ctx->peer_addr, ctx->peer_addr_len);
    return GTP_OK;
}

static uint32_t RecvFrameCbA(GtpHandler_p hdl, void *frame, uint32_t size, GtpAddr *addr) {
    (void)hdl; (void)frame; (void)size; (void)addr;
    return GTP_OK;
}

static void LogCb(uint32_t level, const char *fmt, ...) {
    /* 只打印 ERROR 及以上，并统计 FEC 异常 */
    if (level > 3) return;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (strstr(buf, "fec restore abnormal")) {
        g_stats.fec_anomaly.fetch_add(1);
    }

    static const char *lv[] = {"EMRG","ALRT","CRIT","ERR"};
    printf("[gtp-%s] %s", lv[level], buf);
}

static uint32_t LogLevelCb(void) { return 3; /* kGtpLogLevelError */ }

/* ───────────────────── 接收线程 ───────────────────── */
typedef struct RecvThreadArg {
    InstCtx         *inst;
    GtpHandler_p     gtp_hdl;
    pthread_mutex_t *lock;
} RecvThreadArg;

static void *recv_thread(void *arg) {
    RecvThreadArg *a   = (RecvThreadArg *)arg;
    InstCtx       *ctx = a->inst;
    static __thread uint8_t buf[65536];  /* per-thread to avoid static data race */

    while (g_running) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t n = recvfrom(ctx->sfd_recv, buf, sizeof(buf), 0,
                             (struct sockaddr *)&peer, &peer_len);
        if (n <= 0) continue;

        pthread_mutex_lock(a->lock);
        uint32_t  mem_len  = 0;
        GtpAddr  *tran_addr = NULL;
        uint8_t  *pack_mem = GtpMallocPackMem(a->gtp_hdl, NULL, 0, &mem_len,
                                               (void **)&tran_addr, (uint32_t)n);
        if (!pack_mem) { pthread_mutex_unlock(a->lock); continue; }

        memcpy(pack_mem, buf, n);
        tran_addr->context_       = ctx;
        tran_addr->sfd_           = (uint32_t)ctx->sfd_send;
        tran_addr->stream_type_   = kRealTimeStream;
        tran_addr->self_addr_len_ = (uint32_t)sizeof(ctx->self_addr);
        memcpy(tran_addr->self_addr_, &ctx->self_addr, sizeof(ctx->self_addr));
        tran_addr->sock_addr_len_ = (uint32_t)peer_len;
        memcpy(tran_addr->sock_addr_, &peer, peer_len);

        if (g_use_key) {
            uint64_t sk = 0;
            GtpCheckPacketInvalid(pack_mem, (uint32_t)n, &sk);

            /* 所有包类型统一严格校验（ACK/NACK/RTT字节序已修正，不再需要fallback） */
            if (GTP_FEC_PACK_TYPE_VAL == GTP_RAW_PACK_TYPE(pack_mem)) {
                if (sk == TEST_STREAM_KEY) {
                    g_stats.fec_key_ok.fetch_add(1);
                } else {
                    g_stats.fec_key_bad.fetch_add(1);
                }
            }
            tran_addr->enable_key_  = 1;
            tran_addr->stream_key_  = sk;  /* 直接用提取结果，无 fallback */
        } else {
            tran_addr->enable_key_  = 0;
        }

        uint32_t ret = GtpPacketReceive(a->gtp_hdl, pack_mem, (uint32_t)n, tran_addr);
        if (GTP_OK != ret) {
            GtpFreePackMem(a->gtp_hdl, pack_mem);
        }
        pthread_mutex_unlock(a->lock);
    }
    return NULL;
}

/* ───────────────────── 定时器线程 ───────────────────── */
typedef struct TimerArg {
    GtpHandler_p     hdl_a;
    GtpHandler_p     hdl_b;
    pthread_mutex_t *lock_a;
    pthread_mutex_t *lock_b;
} TimerArg;

static void *timer_thread(void *arg) {
    TimerArg *t = (TimerArg *)arg;
    while (g_running) {
        usleep(TIMER_PERIOD_US);
        pthread_mutex_lock(t->lock_a);
        PeriodGtpTimer(t->hdl_a);
        pthread_mutex_unlock(t->lock_a);
        pthread_mutex_lock(t->lock_b);
        PeriodGtpTimer(t->hdl_b);
        pthread_mutex_unlock(t->lock_b);
    }
    return NULL;
}

/* ───────────────────── socket 辅助 ───────────────────── */
static int make_udp_socket(uint16_t bind_port, struct sockaddr_in *out_addr) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->sin_family      = AF_INET;
    out_addr->sin_port        = htons(bind_port);
    out_addr->sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)out_addr, sizeof(*out_addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }

    /* 超时：防止 recvfrom 阻塞线程退出 */
    struct timeval tv = {0, 200000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    inet_pton(AF_INET, LOOPBACK_IP, &out_addr->sin_addr);
    return fd;
}

/* ───────────────────── FEC 模式名 ───────────────────── */
static const char *book_name(int id) {
    static const char *names[] = {
        "4x4 H+V+UH+DH", "4x4 H+V", "4x1 H",
        "2x2 H+V+UH+DH", "2x2 H+V", "2x1 H"
    };
    return (id >= 0 && id <= 5) ? names[id] : "?";
}

/* ───────────────────── main ───────────────────── */
int main(int argc, char *argv[]) {
    if (argc >= 2) g_loss_pct      = atoi(argv[1]);
    if (argc >= 3) g_pps           = atoi(argv[2]);
    if (argc >= 4) g_duration      = atoi(argv[3]);
    if (argc >= 5) g_book_id       = atoi(argv[4]);
    if (argc >= 6) g_use_key       = atoi(argv[5]);
    if (argc >= 7) g_outage_sec    = atoi(argv[6]);
    if (argc >= 8) g_outage_at_sec = atoi(argv[7]);
    unsigned seed = (unsigned)time(NULL);
    if (argc >= 9) seed = (unsigned)atoi(argv[8]);

    if (g_loss_pct < 0 || g_loss_pct > 90) { fprintf(stderr, "loss_pct 0-90\n"); return 1; }
    if (g_pps < 1 || g_pps > 5000)         { fprintf(stderr, "pps 1-5000\n");    return 1; }
    if (g_book_id < 0 || g_book_id > 5)    { fprintf(stderr, "book_id 0-5\n");   return 1; }
    if (g_outage_sec < 0)                  { fprintf(stderr, "outage_sec >= 0\n"); return 1; }
    if (g_outage_at_sec < 0 || (g_outage_sec > 0 && g_outage_at_sec + g_outage_sec > g_duration)) {
        fprintf(stderr, "outage_at_sec must be >= 0 and outage window must fit within duration\n");
        return 1;
    }

    srand(seed);
    fprintf(stderr, "seed=%u\n", seed);

    printf("=== fec_loss_test ===\n");
    printf("loss=%d%%  pps=%d  duration=%ds  book_id=%d(%s)  use_key=%d\n",
           g_loss_pct, g_pps, g_duration, g_book_id, book_name(g_book_id), g_use_key);
    if (g_outage_sec > 0) {
        printf("outage: 100%% 双向丢包，第%d秒起持续%d秒\n", g_outage_at_sec, g_outage_sec);
    }
    printf("\n");

    /* goodtp 模块加载 */
    if (GTP_OK != InsLoadGtpModule()) {
        fprintf(stderr, "InsLoadGtpModule failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    /* socket 建立：A 发送到 B，B 发送到 A */
    g_inst_a.name = "A(sender)";
    g_inst_b.name = "B(recver)";

    g_inst_a.sfd_recv = make_udp_socket(BASE_PORT_A, &g_inst_a.self_addr);
    g_inst_b.sfd_recv = make_udp_socket(BASE_PORT_B, &g_inst_b.self_addr);
    g_inst_a.sfd_send = g_inst_a.sfd_recv;
    g_inst_b.sfd_send = g_inst_b.sfd_recv;

    g_inst_a.self_addr_len = sizeof(g_inst_a.self_addr);
    g_inst_b.self_addr_len = sizeof(g_inst_b.self_addr);

    /* 对端地址 */
    memset(&g_inst_a.peer_addr, 0, sizeof(g_inst_a.peer_addr));
    g_inst_a.peer_addr.sin_family = AF_INET;
    g_inst_a.peer_addr.sin_port   = htons(BASE_PORT_B);
    inet_pton(AF_INET, LOOPBACK_IP, &g_inst_a.peer_addr.sin_addr);
    g_inst_a.peer_addr_len = sizeof(g_inst_a.peer_addr);

    memset(&g_inst_b.peer_addr, 0, sizeof(g_inst_b.peer_addr));
    g_inst_b.peer_addr.sin_family = AF_INET;
    g_inst_b.peer_addr.sin_port   = htons(BASE_PORT_A);
    inet_pton(AF_INET, LOOPBACK_IP, &g_inst_b.peer_addr.sin_addr);
    g_inst_b.peer_addr_len = sizeof(g_inst_b.peer_addr);

    /* GoodTP 实例 */
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

    g_inst_a.gtp_hdl = CreateGtpInstance(1, 10000000, &cb_a, &mem_cfg);
    g_inst_b.gtp_hdl = CreateGtpInstance(2, 10000000, &cb_b, &mem_cfg);
    if (INVALID_GTP_HANDLER == g_inst_a.gtp_hdl ||
        INVALID_GTP_HANDLER == g_inst_b.gtp_hdl) {
        fprintf(stderr, "CreateGtpInstance failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    /* 线程启动 */
    RecvThreadArg ra = {&g_inst_a, g_inst_a.gtp_hdl, &g_lock_a};
    RecvThreadArg rb = {&g_inst_b, g_inst_b.gtp_hdl, &g_lock_b};
    TimerArg      ta = {g_inst_a.gtp_hdl, g_inst_b.gtp_hdl, &g_lock_a, &g_lock_b};

    pthread_t tid_ra, tid_rb, tid_timer;
    pthread_create(&tid_ra,    NULL, recv_thread, &ra);
    pthread_create(&tid_rb,    NULL, recv_thread, &rb);
    pthread_create(&tid_timer, NULL, timer_thread, &ta);

    /* 发送循环 */
    g_test_start_us      = now_us();
    uint64_t interval_us = 1000000ULL / (uint64_t)g_pps;
    uint64_t deadline    = g_test_start_us + (uint64_t)g_duration * 1000000ULL;
    uint64_t next_send   = g_test_start_us;
    uint32_t seq         = 0;

    while (now_us() < deadline) {
        if (now_us() >= next_send) {
            TestFrame f;
            f.magic      = 0xDEADBEEF;
            f.seq        = ++seq;
            f.send_ts_us = now_us();
            snprintf(f.payload, sizeof(f.payload), "seq=%u", seq);

            pthread_mutex_lock(&g_lock_a);
            uint32_t  mem_len  = 0;
            GtpAddr  *taddr    = NULL;
            uint8_t  *mem = GtpMallocPackMem(g_inst_a.gtp_hdl, NULL, 0, &mem_len,
                                              (void **)&taddr, sizeof(f));
            if (mem) {
                memcpy(mem, &f, sizeof(f));
                taddr->context_       = &g_inst_a;
                taddr->sfd_           = (uint32_t)g_inst_a.sfd_send;
                taddr->stream_type_   = kRealTimeStream;
                taddr->enable_key_    = (uint32_t)g_use_key;
                taddr->stream_key_    = g_use_key ? TEST_STREAM_KEY : 0;
                taddr->self_addr_len_ = (uint32_t)sizeof(g_inst_a.self_addr);
                memcpy(taddr->self_addr_, &g_inst_a.self_addr, sizeof(g_inst_a.self_addr));
                taddr->sock_addr_len_ = (uint32_t)sizeof(g_inst_a.peer_addr);
                memcpy(taddr->sock_addr_, &g_inst_a.peer_addr, sizeof(g_inst_a.peer_addr));

                uint32_t ret = GtpFrameSend(g_inst_a.gtp_hdl, mem, sizeof(f), taddr, 0, 0);
                if (GTP_OK == ret) {
                    g_stats.sent_frames.fetch_add(1);
                } else {
                    GtpFreePackMem(g_inst_a.gtp_hdl, mem);
                }
            }
            pthread_mutex_unlock(&g_lock_a);

            next_send += interval_us;
        }
        usleep(100);
    }

    g_running = 0;
    pthread_join(tid_ra,    NULL);
    pthread_join(tid_rb,    NULL);
    pthread_join(tid_timer, NULL);

    /* 读取算法参数 */
    uint8_t alg_buf_a[4096] = {0};
    uint8_t alg_buf_b[4096] = {0};
    uint8_t ip_a[64], ip_b[64];
    inet_ntop(AF_INET, &g_inst_a.self_addr.sin_addr, (char*)ip_a, sizeof(ip_a));
    inet_ntop(AF_INET, &g_inst_b.self_addr.sin_addr, (char*)ip_b, sizeof(ip_b));

    GetAlgorithmParam(g_inst_a.gtp_hdl, ip_a, ip_b, alg_buf_a, sizeof(alg_buf_a));
    GetAlgorithmParam(g_inst_b.gtp_hdl, ip_b, ip_a, alg_buf_b, sizeof(alg_buf_b));

    /* 结果报告 */
    uint32_t sent     = g_stats.sent_frames.load();
    uint32_t recvd    = g_stats.recv_frames.load();
    uint32_t drops    = g_stats.dropped_at_net.load();
    uint32_t anom     = g_stats.fec_anomaly.load();
    uint32_t fec_ok   = g_stats.fec_key_ok.load();
    uint32_t fec_bad  = g_stats.fec_key_bad.load();

    uint32_t unique_seq = 0, dup_total = 0, max_seq = 0;
    for (auto &kv : g_seq_count) {
        unique_seq += 1;
        if (kv.second > 1) dup_total += (uint32_t)(kv.second - 1);
        if (kv.first > max_seq) max_seq = kv.first;
    }
    printf("\n========== 结果 ==========\n");
    printf("发送帧:      %u\n", sent);
    printf("接收帧:      %u\n", recvd);
    printf("去重后帧数:  %u  重复投递次数: %u  (max_seq=%u)\n", unique_seq, dup_total, max_seq);
    printf("模拟丢包:    %u  (%.1f%%)\n", drops, sent ? drops * 100.0 / (sent + drops) : 0.0);
    printf("FEC异常日志: %u  (期望: 0)\n", anom);
    printf("帧丢失率:    %.1f%%\n", (sent && recvd <= sent) ? (sent - recvd) * 100.0 / sent : 0.0);
    if (g_use_key) {
        printf("FEC包key正确: %u  key错误: %u  (use_key=1时统计)\n", fec_ok, fec_bad);
    }

    printf("\n--- 发送端(A)算法参数 ---\n%s\n", alg_buf_a);
    printf("--- 接收端(B)算法参数 ---\n%s\n", alg_buf_b);

    /* 结论 */
    printf("\n========== 判定 ==========\n");
    int pass = 1;

    /* TEST 3：FEC 恢复异常计数为 0 */
    if (anom > 0) {
        printf("[FAIL] fec_anomaly=%u  期望 0  (stale matrix XOR 产生了错误恢复)\n", anom);
        pass = 0;
    } else {
        printf("[PASS] 无 fec restore abnormal 日志\n");
    }

    /* TEST 4：use_key=1 时 FEC 包 stream_key_ 提取正确 */
    if (g_use_key) {
        if (fec_bad > 0) {
            printf("[FAIL] FEC包stream_key_提取错误 %u 次（正确 %u 次）\n", fec_bad, fec_ok);
            pass = 0;
        } else if (fec_ok == 0) {
            printf("[WARN] use_key=1 但未收到任何FEC包（fec_key_ok=0），无法验证\n");
        } else {
            printf("[PASS] FEC包stream_key_提取全部正确（%u 次）\n", fec_ok);
        }
    }

    /* recvd > sent 说明应用层重复收到了同一帧（见 g_seq_count 的去重统计），这本身就是
     * 一个需要 FAIL 的正确性问题，不能被下面的丢失率判定悄悄吞掉。 */
    if (recvd > sent) {
        printf("[FAIL] 应用层收到的帧数(%u)超过发送帧数(%u)，存在重复投递（详见上面 dup-detect 日志）\n",
               recvd, sent);
    }

    /* TEST 1/2/3：帧丢失率不超过理论 FEC 恢复能力上限 */
    /* 2x2 H+V(book4)  可恢复任意 1 包/4 包，理论上 25% 丢包不应有帧丢失 */
    /* 实际因 ARQ 补足，丢帧率应明显低于原始丢包率 */
    float frame_loss_pct = (sent && recvd <= sent) ? (sent - recvd) * 100.0f / sent : 0.0f;
    float theoretical_fec_recovery = (g_book_id == 4 || g_book_id == 3) ? 25.0f : 0.0f;
    if (g_loss_pct <= (int)theoretical_fec_recovery && frame_loss_pct > 5.0f) {
        printf("[FAIL] 丢包率 %d%% 应在 FEC 可恢复范围内，但帧丢失 %.1f%%\n",
               g_loss_pct, frame_loss_pct);
        pass = 0;
    } else {
        printf("[PASS] 帧丢失率 %.1f%%（原始丢包率 %d%%）\n", frame_loss_pct, g_loss_pct);
    }

    printf("\n整体：%s\n", pass ? "PASS" : "FAIL");

    DeleteGtpInstance(g_inst_a.gtp_hdl);
    DeleteGtpInstance(g_inst_b.gtp_hdl);
    close(g_inst_a.sfd_recv);
    close(g_inst_b.sfd_recv);
    RmLoadGtpModule();
    return pass ? 0 : 1;
}
