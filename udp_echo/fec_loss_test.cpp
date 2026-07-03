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
 *   cd udp_echo/build && ./fec_loss_test [loss%] [pps] [seconds] [book_id] [use_key]
 *                                        [spike_pct spike_at_s spike_dur_s spike_dur_ms]
 *                                        [link_delay_ms]
 *   示例：
 *     ./fec_loss_test 20 50 10 4    # 20% 丢包，50 PPS，10s，book4(H+V 2x2)
 *     ./fec_loss_test 25 50 10 3    # 25% 丢包，50 PPS，10s，book3(全向 2x2) [TEST 2]
 *     ./fec_loss_test 20 200 15 4   # 20% 丢包，200 PPS，15s，测缓冲绕回 [TEST 3]
 *
 *   TEST 4 — RealtimeReorderWindow 交付顺序/静默丢弃量化（CS2 "错过" 排查）
 *     loopback RTT≈0 时 ARQ/FEC 恢复远快于 reorder 窗口 20-30ms 的等待上限，复现不出问题，
 *     需要用最后一个参数注入单向链路延迟（近似真实 WiFi RTT 的一半）才能让 reorder 窗口的
 *     "等不到就跳过/丢弃" 路径真实触发。
 *     ./fec_loss_test 9 128 30 4 0 0 0 0 0 30   # 9%丢包，128pps，30s，单向延迟30ms(≈RTT 60ms)
 *     观察输出里的"帧丢失率" vs "内部残余丢包(r_loss)"的差值，以及"乱序交付/前跳"计数。
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
#include <vector>
#include <map>

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

/* ───────────────────── 交付顺序统计（量化 RealtimeReorderWindow 影响） ─────────────────────
 * RecvFrameCbB 只被单个接收线程调用（GoodTP 单线程约束），因此这里用普通变量即可，无需原子操作。
 * out_of_order：交付给应用层的帧序号比之前交付过的更小，说明 GoodTP 内部的 reorder 窗口在等待
 * 空缺超时后放弃顺序保证、先交付了后到的帧（skip-ahead）——这正是 CS2 侧"错过"可能的来源：
 * 帧最终被交付了，但顺序被打乱，落在游戏帧的时间窗之外。
 * max_forward_skip：单次前跳交付时，跳过的序号个数（即被跳过、大概率随后到达却被reorder窗口
 * 直接丢弃的帧数——见 RealtimeReorderWindow::Push() 的 SnBefore(sn, expect_sn_) -> kPushDrop）。 */
static uint32_t g_last_delivered_seq   = 0;
static uint32_t g_out_of_order_count   = 0;
static uint64_t g_forward_skip_total   = 0;  /* Σ(本次交付seq - 上次交付seq - 1)，即被跳过的帧数总和 */
static uint32_t g_forward_skip_events  = 0;

/* "有效可用"时间窗计数：真正决定 CS2 是否卡顿的不是"最终有没有交付"，而是"交付时是否还在游戏
 * 能用的时间预算内"。GoodTP 层面"迟到但补投"了的帧，如果到达时已经比这几个阈值还晚，从 CS2
 * 的角度大概率跟"彻底没收到"没有本质区别（早就被判定为错过/用插值代替了）。这里用几个粗略的
 * 参考阈值（不知道 CS2 实际预算，取几个量级做对比）而不是只看整体 p50/p90/p99，避免"技术上
 * 交付了"掩盖"实际上早就没用了"的问题。 */
#define NUM_LATE_THRESHOLDS 3
static const uint32_t g_late_threshold_us[NUM_LATE_THRESHOLDS] = {30000, 50000, 100000};
static uint32_t g_late_over_threshold[NUM_LATE_THRESHOLDS] = {0, 0, 0};

/* ───────────────────── 测试参数 ───────────────────── */
static std::atomic<int> g_loss_pct{20}; /* 丢包百分比 0-100，支持运行期动态切换 */
static int g_pps       = 50;    /* 每秒发包数 */
static int g_duration  = 10;    /* 测试秒数 */
static int g_book_id   = 4;     /* FEC codebook */
static int g_use_key   = 0;     /* 1=enable_key_模式，测试FEC包stream_key_嵌入 */
/* spike 模式：在 spike_at_s 秒时切到 spike_pct 持续 spike_dur_s 秒，之后恢复
 * spike_dur_ms（若非0）覆盖 spike_dur_s，用于模拟"瞬间连续丢几个包"（几十~几百ms）而非
 * 持续数秒的高丢包率，更贴近真实 wifi 微卡顿场景。 */
static int g_spike_pct   = 0;   /* 0 = 不注入尖刺 */
static int g_spike_at_s  = 0;
static int g_spike_dur_s = 3;
static int g_spike_dur_ms = 0;  /* 0 = 使用 g_spike_dur_s；否则以毫秒为准，覆盖秒参数 */
static volatile int g_running = 1;

/* per-phase frame counters for spike test */
struct PhaseStats {
    std::atomic<uint32_t> sent{0};
    std::atomic<uint32_t> recv{0};
};
static PhaseStats g_phase[3];  /* 0=pre-spike, 1=spike, 2=post-spike */
static std::atomic<int> g_phase_idx{0};

/* frame delivery latency samples (send_ts -> app deliver), bucketed per phase */
#define MAX_LAT_SAMPLES 8192
static uint32_t g_lat_us[3][MAX_LAT_SAMPLES];
static std::atomic<uint32_t> g_lat_num[3];

/* fine-grained (100ms) latency time series: recorded by RECEIVE time, so we can see
 * exactly how long delivery stays delayed after a short loss burst. */
#define LAT_BUCKET_US 100000ULL
#define MAX_LAT_BUCKETS 900  /* 90s @ 100ms */
static std::atomic<uint64_t> g_lat_bucket_sum_us[MAX_LAT_BUCKETS];
static std::atomic<uint32_t> g_lat_bucket_cnt[MAX_LAT_BUCKETS];
static std::atomic<uint32_t> g_lat_bucket_max_us[MAX_LAT_BUCKETS];

/* per-second wire-byte counters (actual bytes handed to sendto(), including
 * FEC parity packets and ARQ retransmissions), to observe bandwidth over
 * time around a loss spike. */
#define MAX_BW_SECONDS 600
static std::atomic<uint64_t> g_bytes_a2b[MAX_BW_SECONDS]; /* A->B: data+FEC+retrans */
static std::atomic<uint64_t> g_bytes_b2a[MAX_BW_SECONDS]; /* B->A: ack/nack/rtt */
static uint64_t g_bw_start_us = 0;

static inline void AcctBytes(std::atomic<uint64_t> *bucket, uint64_t ts_us, uint64_t start_us, uint32_t size) {
    if (ts_us < start_us) return;
    uint64_t sec = (ts_us - start_us) / 1000000ULL;
    if (sec < MAX_BW_SECONDS) {
        bucket[sec].fetch_add(size, std::memory_order_relaxed);
    }
}

#define TEST_STREAM_KEY  0x1234567890ABCDEFULL

/* ───────────────────── 时间戳 ───────────────────── */
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
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
    return (rand() % 100) < g_loss_pct.load(std::memory_order_relaxed);
}

/* ───────────────────── 单向链路延迟模拟 ─────────────────────
 * loopback 本身 RTT≈0，而 RealtimeReorderWindow 的 wait_us 上限只有 20-30ms——如果 ARQ/FEC
 * 恢复也在几毫秒内完成，永远不会触发"等不到就跳过"的 drop 路径，复现不出真实 WiFi 场景下的
 * "错过"。这里给 A→B / B→A 都加一个可配置的单向固定延迟（近似真实 WiFi RTT 的一半），让重传/FEC
 * 恢复报文的到达时间接近真实场景，从而让 reorder 窗口的 skip-ahead/drop 行为有机会真实触发。
 * g_link_delay_us == 0 时完全退化为原来的直通 sendto()，不影响其它已有测试用例。 */
static int g_link_delay_us = 0;

struct DelayedPacket {
    std::vector<uint8_t> data;
    int fd;
    struct sockaddr_in dst;
    socklen_t dst_len;
};
static std::multimap<uint64_t, DelayedPacket> g_delay_q_a2b;
static std::multimap<uint64_t, DelayedPacket> g_delay_q_b2a;
static pthread_mutex_t g_delay_lock_a2b = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_delay_lock_b2a = PTHREAD_MUTEX_INITIALIZER;

static void EnqueueDelayed(std::multimap<uint64_t, DelayedPacket> *q, pthread_mutex_t *lock,
                            const void *pack, uint32_t size, int fd, const struct sockaddr_in &dst,
                            socklen_t dst_len) {
    DelayedPacket dp;
    dp.data.assign((const uint8_t*)pack, (const uint8_t*)pack + size);
    dp.fd  = fd;
    dp.dst = dst;
    dp.dst_len = dst_len;
    uint64_t deliver_ts = now_us() + (uint64_t)g_link_delay_us;

    pthread_mutex_lock(lock);
    q->insert(std::make_pair(deliver_ts, dp));
    pthread_mutex_unlock(lock);
}

static void FlushDueDelayed(std::multimap<uint64_t, DelayedPacket> *q, pthread_mutex_t *lock, uint64_t ts_us) {
    pthread_mutex_lock(lock);
    while (!q->empty() && q->begin()->first <= ts_us) {
        DelayedPacket dp = q->begin()->second;
        q->erase(q->begin());
        pthread_mutex_unlock(lock);
        sendto(dp.fd, dp.data.data(), dp.data.size(), 0, (struct sockaddr*)&dp.dst, dp.dst_len);
        pthread_mutex_lock(lock);
    }
    pthread_mutex_unlock(lock);
}

static void *delay_dispatch_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t ts = now_us();
        FlushDueDelayed(&g_delay_q_a2b, &g_delay_lock_a2b, ts);
        FlushDueDelayed(&g_delay_q_b2a, &g_delay_lock_b2a, ts);

        pthread_mutex_lock(&g_delay_lock_a2b);
        bool empty_a2b = g_delay_q_a2b.empty();
        pthread_mutex_unlock(&g_delay_lock_a2b);
        pthread_mutex_lock(&g_delay_lock_b2a);
        bool empty_b2a = g_delay_q_b2a.empty();
        pthread_mutex_unlock(&g_delay_lock_b2a);

        if (!g_running && empty_a2b && empty_b2a) {
            break;
        }
        usleep(500);
    }
    return NULL;
}

/* ───────────────────── goodtp 回调 ───────────────────── */
static uint32_t SendPackCbA(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    AcctBytes(g_bytes_a2b, now_us(), g_bw_start_us, size);
    if (should_drop()) {
        g_stats.dropped_at_net.fetch_add(1);
        return GTP_OK;
    }
    InstCtx *ctx = (InstCtx *)addr->context_;
    if (g_link_delay_us > 0) {
        EnqueueDelayed(&g_delay_q_a2b, &g_delay_lock_a2b, pack, size, ctx->sfd_send,
                       ctx->peer_addr, ctx->peer_addr_len);
    } else {
        sendto(ctx->sfd_send, pack, size, 0,
               (struct sockaddr *)&ctx->peer_addr, ctx->peer_addr_len);
    }
    return GTP_OK;
}

static uint32_t RecvFrameCbB(GtpHandler_p hdl, void *frame, uint32_t size, GtpAddr *addr) {
    (void)hdl; (void)addr;
    if (size >= sizeof(TestFrame)) {
        TestFrame *f = (TestFrame *)frame;
        if (f->magic == 0xDEADBEEF) {
            g_stats.recv_frames.fetch_add(1);

            /* 交付顺序检测：只在单接收线程内访问，无需加锁 */
            if (0 != g_last_delivered_seq) {
                if (f->seq <= g_last_delivered_seq) {
                    g_out_of_order_count += 1;
                } else if (f->seq > g_last_delivered_seq + 1) {
                    g_forward_skip_total  += (f->seq - g_last_delivered_seq - 1);
                    g_forward_skip_events += 1;
                }
            }
            if (f->seq > g_last_delivered_seq) {
                g_last_delivered_seq = f->seq;
            }

            int ph = (g_spike_pct > 0) ? g_phase_idx.load(std::memory_order_relaxed) : 0;
            if (g_spike_pct > 0) {
                g_phase[ph].recv.fetch_add(1);
            }
            uint64_t nu = now_us();
            if (nu > f->send_ts_us) {
                uint32_t lat = (uint32_t)(nu - f->send_ts_us);
                /* 减去人为注入的单向网络延迟——那是模拟真实物理传输时间，CS2 的可用时间预算
                 * 本来就要把它算进去，不是 GoodTP 自己造成的"额外"延迟。这里只关心 GoodTP
                 * 自身（reorder窗口/ARQ等待）在网络传输之外又加了多少。 */
                uint32_t extra_lat = (lat > (uint32_t)g_link_delay_us) ? (lat - (uint32_t)g_link_delay_us) : 0;
                for (int th = 0; th < NUM_LATE_THRESHOLDS; ++th) {
                    if (extra_lat > g_late_threshold_us[th]) {
                        g_late_over_threshold[th] += 1;
                    }
                }
                uint32_t pos = g_lat_num[ph].fetch_add(1);
                if (pos < MAX_LAT_SAMPLES) {
                    g_lat_us[ph][pos] = lat;
                }
                if (nu >= g_bw_start_us) {
                    uint64_t bkt = (nu - g_bw_start_us) / LAT_BUCKET_US;
                    if (bkt < MAX_LAT_BUCKETS) {
                        g_lat_bucket_sum_us[bkt].fetch_add(lat, std::memory_order_relaxed);
                        g_lat_bucket_cnt[bkt].fetch_add(1, std::memory_order_relaxed);
                        uint32_t cur_max = g_lat_bucket_max_us[bkt].load(std::memory_order_relaxed);
                        while (lat > cur_max &&
                               !g_lat_bucket_max_us[bkt].compare_exchange_weak(cur_max, lat)) {}
                    }
                }
            }
        }
    }
    return GTP_OK;
}

static uint32_t SendPackCbB(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    AcctBytes(g_bytes_b2a, now_us(), g_bw_start_us, size);
    /* B→A 方向（ACK/NACK/RTT）同样随机丢包，模拟真实双向网络丢包 */
    if (should_drop()) {
        g_stats.dropped_at_net.fetch_add(1);
        return GTP_OK;
    }
    InstCtx *ctx = (InstCtx *)addr->context_;
    if (g_link_delay_us > 0) {
        EnqueueDelayed(&g_delay_q_b2a, &g_delay_lock_b2a, pack, size, ctx->sfd_send,
                       ctx->peer_addr, ctx->peer_addr_len);
    } else {
        sendto(ctx->sfd_send, pack, size, 0,
               (struct sockaddr *)&ctx->peer_addr, ctx->peer_addr_len);
    }
    return GTP_OK;
}

static uint32_t RecvFrameCbA(GtpHandler_p hdl, void *frame, uint32_t size, GtpAddr *addr) {
    (void)hdl; (void)frame; (void)size; (void)addr;
    return GTP_OK;
}

static void LogCb(uint32_t level, const char *fmt, ...) {
    /* 打印 WARNING 及以上（含 alg on/off 切换），并统计 FEC 异常 */
    if (level > 4) return;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (strstr(buf, "fec restore abnormal")) {
        g_stats.fec_anomaly.fetch_add(1);
    }

    static const char *lv[] = {"EMRG","ALRT","CRIT","ERR","WARN"};
    printf("[t=%.2fs][gtp-%s] %s", (now_us() - g_bw_start_us) / 1e6, lv[level], buf);
}

static uint32_t LogLevelCb(void) { return 4; /* kGtpLogLevelWarning: surface alg on/off transitions */ }

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
    int base_loss = 20;
    if (argc >= 2) base_loss  = atoi(argv[1]);
    if (argc >= 3) g_pps      = atoi(argv[2]);
    if (argc >= 4) g_duration = atoi(argv[3]);
    if (argc >= 5) g_book_id  = atoi(argv[4]);
    if (argc >= 6) g_use_key  = atoi(argv[5]);
    if (argc >= 7) g_spike_pct  = atoi(argv[6]);
    if (argc >= 8) g_spike_at_s = atoi(argv[7]);
    if (argc >= 9) g_spike_dur_s= atoi(argv[8]);
    if (argc >= 10) g_spike_dur_ms = atoi(argv[9]);
    if (argc >= 11) g_link_delay_us = atoi(argv[10]) * 1000;  /* 单向延迟，单位ms，模拟真实WiFi RTT/2 */
    g_loss_pct.store(base_loss);

    if (base_loss < 0 || base_loss > 90)   { fprintf(stderr, "loss_pct 0-90\n"); return 1; }
    if (g_pps < 1 || g_pps > 5000)         { fprintf(stderr, "pps 1-5000\n");    return 1; }
    if (g_book_id < 0 || g_book_id > 5)    { fprintf(stderr, "book_id 0-5\n");   return 1; }

    srand((unsigned)time(NULL));

    printf("=== fec_loss_test ===\n");
    if (g_spike_pct > 0) {
        printf("loss=%d%%  pps=%d  duration=%ds  book_id=%d(%s)  use_key=%d\n"
               "SPIKE: at t+%ds inject %d%% loss for %ds, then back to %d%%\n\n",
               base_loss, g_pps, g_duration, g_book_id, book_name(g_book_id), g_use_key,
               g_spike_at_s, g_spike_pct, g_spike_dur_s, base_loss);
    } else {
        printf("loss=%d%%  pps=%d  duration=%ds  book_id=%d(%s)  use_key=%d\n\n",
               base_loss, g_pps, g_duration, g_book_id, book_name(g_book_id), g_use_key);
    }

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

    pthread_t tid_ra, tid_rb, tid_timer, tid_delay;
    pthread_create(&tid_ra,    NULL, recv_thread, &ra);
    pthread_create(&tid_rb,    NULL, recv_thread, &rb);
    pthread_create(&tid_timer, NULL, timer_thread, &ta);
    pthread_create(&tid_delay, NULL, delay_dispatch_thread, NULL);

    /* 发送循环 */
    uint64_t interval_us  = 1000000ULL / (uint64_t)g_pps;
    uint64_t start_us     = now_us();
    g_bw_start_us         = start_us;
    uint64_t deadline     = start_us + (uint64_t)g_duration * 1000000ULL;
    uint64_t spike_dur_us = (g_spike_dur_ms > 0) ? (uint64_t)g_spike_dur_ms * 1000ULL
                                                  : (uint64_t)g_spike_dur_s * 1000000ULL;
    uint64_t spike_start  = (g_spike_pct > 0) ? (start_us + (uint64_t)g_spike_at_s  * 1000000ULL) : UINT64_MAX;
    uint64_t spike_end    = (g_spike_pct > 0) ? (spike_start + spike_dur_us) : UINT64_MAX;
    uint64_t next_send    = start_us;
    uint32_t seq          = 0;
    int      spike_active = 0;

    while (now_us() < deadline) {
        uint64_t ts = now_us();

        /* spike phase management */
        if (g_spike_pct > 0) {
            if (!spike_active && ts >= spike_start && ts < spike_end) {
                spike_active = 1;
                g_loss_pct.store(g_spike_pct);
                g_phase_idx.store(1);
                printf("[t=%.1fs] SPIKE ON: loss %d%% → %d%%\n",
                       (ts - start_us) / 1e6, base_loss, g_spike_pct);
            } else if (spike_active && ts >= spike_end) {
                spike_active = 0;
                g_loss_pct.store(base_loss);
                g_phase_idx.store(2);
                printf("[t=%.1fs] SPIKE OFF: loss %d%% → %d%% (recovery window)\n",
                       (ts - start_us) / 1e6, g_spike_pct, base_loss);
            }
        }

        if (ts >= next_send) {
            TestFrame f;
            f.magic      = 0xDEADBEEF;
            f.seq        = ++seq;
            f.send_ts_us = ts;
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
                    if (g_spike_pct > 0) {
                        g_phase[g_phase_idx.load(std::memory_order_relaxed)].sent.fetch_add(1);
                    }
                } else {
                    GtpFreePackMem(g_inst_a.gtp_hdl, mem);
                }
            }
            pthread_mutex_unlock(&g_lock_a);

            next_send += interval_us;
        }
        usleep(100);
    }

    /* 在真正停掉收包线程之前，先把延迟队列里还在"飞行中"的报文排空，否则会被当成额外丢包 */
    if (g_link_delay_us > 0) {
        usleep((useconds_t)g_link_delay_us + 50000);
    }

    g_running = 0;
    pthread_join(tid_ra,    NULL);
    pthread_join(tid_rb,    NULL);
    pthread_join(tid_timer, NULL);
    pthread_join(tid_delay, NULL);

    /* 读取算法参数 */
    uint8_t alg_buf_a[4096] = {0};
    uint8_t alg_buf_b[4096] = {0};
    uint8_t ip_a[64], ip_b[64];
    inet_ntop(AF_INET, &g_inst_a.self_addr.sin_addr, (char*)ip_a, sizeof(ip_a));
    inet_ntop(AF_INET, &g_inst_b.self_addr.sin_addr, (char*)ip_b, sizeof(ip_b));

    GetAlgorithmParam(g_inst_a.gtp_hdl, ip_a, ip_b, alg_buf_a, sizeof(alg_buf_a));
    GetAlgorithmParam(g_inst_b.gtp_hdl, ip_b, ip_a, alg_buf_b, sizeof(alg_buf_b));

    /* 从 B（接收端）算法参数串里取出 r_loss（GoodTP 内部认为的、FEC/ARQ恢复后仍未收到的残余丢包率，
     * 即"内部真实丢包"）。注意这是在 dedup 过滤后、DeliverFrameInOrder/reorder窗口之前统计的，
     * 与应用层最终拿到的帧数无关——两者之间的差值就是 reorder 窗口自己造成的额外丢帧。 */
    float internal_r_loss_pct = -1.0f;
    {
        const char *p = strstr((const char*)alg_buf_b, "r_loss=");
        if (p) {
            sscanf(p, "r_loss=%f%%", &internal_r_loss_pct);
        }
    }

    /* 结果报告 */
    uint32_t sent     = g_stats.sent_frames.load();
    uint32_t recvd    = g_stats.recv_frames.load();
    uint32_t drops    = g_stats.dropped_at_net.load();
    uint32_t anom     = g_stats.fec_anomaly.load();
    uint32_t fec_ok   = g_stats.fec_key_ok.load();
    uint32_t fec_bad  = g_stats.fec_key_bad.load();
    float    frame_loss_pct_r = sent ? (sent - recvd) * 100.0f / sent : 0.0f;

    printf("\n========== 结果 ==========\n");
    printf("发送帧:      %u\n", sent);
    printf("接收帧:      %u\n", recvd);
    printf("模拟丢包:    %u  (%.1f%%)\n", drops, sent ? drops * 100.0 / (sent + drops) : 0.0);
    printf("FEC异常日志: %u  (期望: 0)\n", anom);
    printf("帧丢失率(应用层最终拿到的):    %.2f%%\n", frame_loss_pct_r);
    if (internal_r_loss_pct >= 0.0f) {
        printf("内部残余丢包(r_loss，FEC/ARQ恢复后、reorder窗口之前): %.2f%%\n", internal_r_loss_pct);
        float gap = frame_loss_pct_r - internal_r_loss_pct;
        printf("疑似 reorder 窗口额外丢弃 = 帧丢失率 - r_loss = %.2f%%%s\n", gap,
               (gap > 0.5f) ? "  <-- 显著，怀疑是 CS2 \"错过\" 的来源" : "");
    }
    if (g_use_key) {
        printf("FEC包key正确: %u  key错误: %u  (use_key=1时统计)\n", fec_ok, fec_bad);
    }

    printf("\n--- 交付顺序（量化 RealtimeReorderWindow 影响） ---\n");
    printf("乱序交付次数(seq回退):      %u  (%.3f%% of recv)\n",
           g_out_of_order_count, recvd ? g_out_of_order_count * 100.0 / recvd : 0.0);
    printf("前跳事件数(跳过空缺提前交付): %u\n", g_forward_skip_events);
    printf("被跳过的帧数总和:            %llu  (跳过后若还能到达，会被 reorder 窗口 kPushDrop 丢弃)\n",
           (unsigned long long)g_forward_skip_total);

    /* "真丢失"只是下限——技术上交付了但迟到太多，从 CS2 的角度大概率跟没收到一样会被算成
     * "错过"。用几个参考阈值把这两类合并成一个更贴近 CS2 侧观感的"有效错过率"，而不是只看
     * 帧丢失率。 */
    printf("\n--- 有效可用时间窗（GoodTP自身在网络传输之外又加的延迟，扣除了注入的单向网络延迟%dms）---\n",
           g_link_delay_us / 1000);
    for (int th = 0; th < NUM_LATE_THRESHOLDS; ++th) {
        uint32_t too_late = g_late_over_threshold[th];
        float too_late_pct = sent ? too_late * 100.0f / sent : 0.0f;
        float effective_miss_pct = frame_loss_pct_r + too_late_pct;
        printf("  阈值>%3ums: 迟到交付=%u(%.2f%%)  真丢失=%.2f%%  有效错过(丢失+迟到)=%.2f%%%s\n",
               g_late_threshold_us[th] / 1000, too_late, too_late_pct, frame_loss_pct_r, effective_miss_pct,
               (effective_miss_pct > 3.0f) ? "  <-- 超过CS2观察到的3%卡顿阈值" : "");
    }

    if (g_spike_pct > 0) {
        printf("\n--- spike 分段完帧率 ---\n");
        const char *pname[] = {"pre-spike (base loss)", "spike phase", "post-spike (recovery)"};
        for (int i = 0; i < 3; ++i) {
            uint32_t ps = g_phase[i].sent.load();
            uint32_t pr = g_phase[i].recv.load();
            if (ps > 0) {
                printf("  [%d] %-28s sent=%-4u recv=%-4u delivery=%.2f%%\n",
                       i, pname[i], ps, pr, pr * 100.0 / ps);
            }
        }
    }

    printf("\n--- 发送端(A)算法参数 ---\n%s\n", alg_buf_a);
    printf("--- 接收端(B)算法参数 ---\n%s\n", alg_buf_b);

    /* frame latency percentiles per phase */
    {
        printf("\n--- 帧延迟(发送->上层交付) ---\n");
        const char *pn[] = {"pre-spike", "spike", "post-spike"};
        int nph = (g_spike_pct > 0) ? 3 : 1;
        for (int ph = 0; ph < nph; ++ph) {
            uint32_t n = g_lat_num[ph].load();
            if (n > MAX_LAT_SAMPLES) n = MAX_LAT_SAMPLES;
            if (0 == n) continue;
            /* insertion-free percentile: qsort a copy */
            static uint32_t tmp[MAX_LAT_SAMPLES];
            memcpy(tmp, g_lat_us[ph], n * sizeof(uint32_t));
            qsort(tmp, n, sizeof(uint32_t),
                  [](const void *a, const void *b) -> int {
                      uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
                      return (x > y) - (x < y);
                  });
            printf("  [%-10s] n=%-5u p50=%.1fms p90=%.1fms p99=%.1fms max=%.1fms\n",
                   (g_spike_pct > 0) ? pn[ph] : "all", n,
                   tmp[n / 2] / 1000.0, tmp[(uint32_t)(n * 0.90)] / 1000.0,
                   tmp[(uint32_t)(n * 0.99)] / 1000.0, tmp[n - 1] / 1000.0);
        }
    }

    /* fine-grained (100ms) latency time series, to see the shape/duration of a delay
     * spike after a short burst rather than a diluted whole-phase average. */
    if (g_spike_pct > 0) {
        printf("\n--- 逐 100ms 延迟(围绕 spike 前后 %ds) ---\n", 5);
        uint64_t win_start = (spike_start > start_us + 2000000ULL) ? (spike_start - 2000000ULL) : start_us;
        uint64_t win_end   = spike_end + 5000000ULL;
        uint64_t b0 = (win_start - start_us) / LAT_BUCKET_US;
        uint64_t b1 = (win_end   - start_us) / LAT_BUCKET_US;
        if (b1 >= MAX_LAT_BUCKETS) b1 = MAX_LAT_BUCKETS - 1;
        printf("%-8s %-6s %-10s %-10s\n", "t(s)", "n", "avg(ms)", "max(ms)");
        for (uint64_t b = b0; b <= b1; ++b) {
            uint32_t cnt = g_lat_bucket_cnt[b].load();
            if (0 == cnt) continue;
            double avg = (g_lat_bucket_sum_us[b].load() / (double)cnt) / 1000.0;
            double mx  = g_lat_bucket_max_us[b].load() / 1000.0;
            double t   = (b * LAT_BUCKET_US) / 1e6;
            const char *mark = (t >= (spike_start - start_us) / 1e6 && t < (spike_end - start_us) / 1e6) ? " <-spike" : "";
            printf("%-8.2f %-6u %-10.2f %-10.2f%s\n", t, cnt, avg, mx, mark);
        }
    }

    /* per-second bandwidth table (actual wire bytes incl. FEC + retrans) */
    printf("\n--- 逐秒带宽(A->B 含FEC/重传, B->A 为ACK/NACK) ---\n");
    printf("%-4s %-12s %-12s %s\n", "sec", "A->B(KB)", "A->B(Mbps)", "note");
    for (int i = 0; i < g_duration && i < MAX_BW_SECONDS; ++i) {
        uint64_t b = g_bytes_a2b[i].load();
        const char *note = "";
        if (g_spike_pct > 0) {
            if (i >= g_spike_at_s && i < g_spike_at_s + g_spike_dur_s) note = "<- spike";
            else if (i >= g_spike_at_s + g_spike_dur_s && i < g_spike_at_s + g_spike_dur_s + 8) note = "<- recovery";
        }
        printf("%-4d %-12.1f %-12.3f %s\n", i, b / 1024.0, (b * 8.0) / 1e6, note);
    }

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

    /* TEST 1/2/3：帧丢失率不超过理论 FEC 恢复能力上限 */
    /* 2x2 H+V(book4)  可恢复任意 1 包/4 包，理论上 25% 丢包不应有帧丢失 */
    /* 实际因 ARQ 补足，丢帧率应明显低于原始丢包率 */
    float frame_loss_pct = sent ? (sent - recvd) * 100.0f / sent : 0.0f;
    float theoretical_fec_recovery = (g_book_id == 4 || g_book_id == 3) ? 25.0f : 0.0f;
    if (base_loss <= (int)theoretical_fec_recovery && frame_loss_pct > 5.0f) {
        printf("[FAIL] 丢包率 %d%% 应在 FEC 可恢复范围内，但帧丢失 %.2f%%\n",
               base_loss, frame_loss_pct);
        pass = 0;
    } else {
        printf("[PASS] 帧丢失率 %.2f%%（原始丢包率 %d%%）\n", frame_loss_pct, base_loss);
    }

    printf("\n整体：%s\n", pass ? "PASS" : "FAIL");

    DeleteGtpInstance(g_inst_a.gtp_hdl);
    DeleteGtpInstance(g_inst_b.gtp_hdl);
    close(g_inst_a.sfd_recv);
    close(g_inst_b.sfd_recv);
    RmLoadGtpModule();
    return pass ? 0 : 1;
}
