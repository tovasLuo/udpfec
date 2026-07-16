/*
 * metrics_matrix_test.cpp
 *
 * 单次场景的详细指标采集工具（基于 fec_loss_test.cpp 的双实例 loopback 测试骨架），
 * 用于回答："FEC 恢复占比多少、NACK/ACK 包网络层丢了多少、ARQ 重传成功/失败/被新
 * staleness 门槛跳过多少、boost 占比多少、投递延迟 p50/p95/p99、带宽多少"。
 *
 * 跟 fec_loss_test.cpp 的区别：不做 PASS/FAIL 判定，只产出一行机器可解析的
 * "REPORT,..." CSV 汇总（外加人类可读的详细文本），供外部脚本跑矩阵后聚合成表格。
 *
 * 用法：
 *   ./metrics_matrix_test <loss_pct> <pps> <seconds> [book_id] [outage_pct outage_dur_s outage_at_s]
 *   ./metrics_matrix_test 10 128 15 4                 # 稳态 10% 丢包
 *   ./metrics_matrix_test 5 128 15 4 30 3 8            # 5%基线 + 第8秒起3秒30%突发丢包
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include <stdarg.h>
#include <atomic>
#include <vector>
#include <algorithm>
#include <unordered_map>

#include "bitlinker.h"

#define GTP_RAW_PACK_TYPE(buf)  (((const uint8_t*)(buf))[2] & 0x07u)
enum {
    kPtData = 0x01, kPtAck = 0x02, kPtFec = 0x03, kPtRttSet = 0x04, kPtRttRes = 0x05, kPtNack = 0x06
};

#define LOOPBACK_IP     "127.0.0.1"
#define BASE_PORT_A     19101
#define BASE_PORT_B     19102
#define TIMER_PERIOD_US 10000

typedef struct TestFrame {
    uint32_t magic;
    uint32_t seq;
    uint64_t send_ts_us;
    char     payload[64];
} TestFrame;

struct Stats {
    std::atomic<uint32_t> sent_frames{0};
    std::atomic<uint32_t> recv_frames{0};
    std::atomic<uint32_t> fec_anomaly{0};

    /* 网络层丢包，按包类型分类（双向合计） */
    std::atomic<uint32_t> net_drop_data_fec{0};
    std::atomic<uint32_t> net_drop_ack_nack{0};
    std::atomic<uint32_t> net_ok_data_fec{0};
    std::atomic<uint32_t> net_ok_ack_nack{0};

    /* 带宽：实际尝试发送的字节数（无论后续是否被模拟网络丢弃——反映本地真实发送量） */
    std::atomic<uint64_t> bytes_a_to_b{0};   /* A→B：DATA/FEC + 部分ACK/NACK(取决于流向) */
    std::atomic<uint64_t> bytes_b_to_a{0};   /* B→A：主要是ACK/NACK */
};

static Stats g_stats;

static int g_loss_pct  = 20;
static int g_pps       = 50;
static int g_duration  = 10;
static int g_book_id   = 4;

static int      g_outage_pct    = 0;   /* 突发期间的丢包率（0=不启用突发场景） */
static int      g_outage_dur_s  = 0;
static int      g_outage_at_s   = 0;
static uint64_t g_test_start_us = 0;
static volatile int g_running   = 1;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

static int cur_loss_pct(void) {
    if (0 == g_outage_pct) {
        return g_loss_pct;
    }
    uint64_t elapsed_s = (now_us() - g_test_start_us) / 1000000ULL;
    if ((elapsed_s >= (uint64_t)g_outage_at_s) && (elapsed_s < (uint64_t)(g_outage_at_s + g_outage_dur_s))) {
        return g_outage_pct;
    }
    return g_loss_pct;
}

static int should_drop(void) {
    return (rand() % 100) < cur_loss_pct();
}

typedef struct InstCtx {
    const char   *name;
    int           sfd_send;
    int           sfd_recv;
    GtpHandler_p  gtp_hdl;
    struct sockaddr_in peer_addr;
    socklen_t          peer_addr_len;
    struct sockaddr_in self_addr;
    socklen_t          self_addr_len;
} InstCtx;

static InstCtx g_inst_a;
static InstCtx g_inst_b;

static pthread_mutex_t g_lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lock_b = PTHREAD_MUTEX_INITIALIZER;

static void classify_and_count(const uint8_t *buf, uint32_t size, int dropped, std::atomic<uint64_t> *bytes_ctr) {
    bytes_ctr->fetch_add(size);
    uint8_t pt = GTP_RAW_PACK_TYPE(buf);
    bool is_ack_nack = (kPtAck == pt) || (kPtNack == pt) || (kPtRttSet == pt) || (kPtRttRes == pt);
    if (dropped) {
        if (is_ack_nack) g_stats.net_drop_ack_nack.fetch_add(1);
        else             g_stats.net_drop_data_fec.fetch_add(1);
    } else {
        if (is_ack_nack) g_stats.net_ok_ack_nack.fetch_add(1);
        else             g_stats.net_ok_data_fec.fetch_add(1);
    }
}

static uint32_t SendPackCbA(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    int drop = should_drop();
    classify_and_count((const uint8_t*)pack, size, drop, &g_stats.bytes_a_to_b);
    if (drop) return GTP_OK;
    InstCtx *ctx = (InstCtx *)addr->context_;
    sendto(ctx->sfd_send, pack, size, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_addr_len);
    return GTP_OK;
}

static std::vector<uint32_t> g_latency_us;
static pthread_mutex_t g_latency_lock = PTHREAD_MUTEX_INITIALIZER;

static std::unordered_map<uint32_t,int> g_seq_count;
static pthread_mutex_t g_seq_count_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t RecvFrameCbB(GtpHandler_p hdl, void *frame, uint32_t size, GtpAddr *addr) {
    (void)hdl; (void)addr;
    if (size >= sizeof(TestFrame)) {
        TestFrame *f = (TestFrame *)frame;
        if (f->magic == 0xDEADBEEF) {
            g_stats.recv_frames.fetch_add(1);

            uint64_t now = now_us();
            if (now >= f->send_ts_us) {
                uint32_t lat_us = (uint32_t)(now - f->send_ts_us);
                pthread_mutex_lock(&g_latency_lock);
                g_latency_us.push_back(lat_us);
                pthread_mutex_unlock(&g_latency_lock);
            }

            pthread_mutex_lock(&g_seq_count_lock);
            ++g_seq_count[f->seq];
            pthread_mutex_unlock(&g_seq_count_lock);
        }
    }
    return GTP_OK;
}

static uint32_t SendPackCbB(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    int drop = should_drop();
    classify_and_count((const uint8_t*)pack, size, drop, &g_stats.bytes_b_to_a);
    if (drop) return GTP_OK;
    InstCtx *ctx = (InstCtx *)addr->context_;
    sendto(ctx->sfd_send, pack, size, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_addr_len);
    return GTP_OK;
}

static uint32_t RecvFrameCbA(GtpHandler_p hdl, void *frame, uint32_t size, GtpAddr *addr) {
    (void)hdl; (void)frame; (void)size; (void)addr;
    return GTP_OK;
}

static void LogCb(uint32_t level, const char *fmt, ...) {
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
    fprintf(stderr, "[gtp-%s] %s", lv[level], buf);
}

static uint32_t LogLevelCb(void) { return 3; }

typedef struct RecvThreadArg {
    InstCtx         *inst;
    GtpHandler_p     gtp_hdl;
    pthread_mutex_t *lock;
} RecvThreadArg;

static void *recv_thread(void *arg) {
    RecvThreadArg *a   = (RecvThreadArg *)arg;
    InstCtx       *ctx = a->inst;
    static __thread uint8_t buf[65536];

    while (g_running) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t n = recvfrom(ctx->sfd_recv, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &peer_len);
        if (n <= 0) continue;

        pthread_mutex_lock(a->lock);
        uint32_t  mem_len  = 0;
        GtpAddr  *tran_addr = NULL;
        uint8_t  *pack_mem = GtpMallocPackMem(a->gtp_hdl, NULL, 0, &mem_len, (void **)&tran_addr, (uint32_t)n);
        if (!pack_mem) { pthread_mutex_unlock(a->lock); continue; }

        memcpy(pack_mem, buf, n);
        tran_addr->context_       = ctx;
        tran_addr->sfd_           = (uint32_t)ctx->sfd_send;
        tran_addr->stream_type_   = kRealTimeStream;
        tran_addr->enable_key_    = 0;
        tran_addr->self_addr_len_ = (uint32_t)sizeof(ctx->self_addr);
        memcpy(tran_addr->self_addr_, &ctx->self_addr, sizeof(ctx->self_addr));
        tran_addr->sock_addr_len_ = (uint32_t)peer_len;
        memcpy(tran_addr->sock_addr_, &peer, peer_len);

        uint32_t ret = GtpPacketReceive(a->gtp_hdl, pack_mem, (uint32_t)n, tran_addr);
        if (GTP_OK != ret) {
            GtpFreePackMem(a->gtp_hdl, pack_mem);
        }
        pthread_mutex_unlock(a->lock);
    }
    return NULL;
}

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
    struct timeval tv = {0, 200000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    inet_pton(AF_INET, LOOPBACK_IP, &out_addr->sin_addr);
    return fd;
}

/* 从 GetAlgorithmParam() 的文本输出里挖字段值，形如 " key= 123" 或 " key=123" */
static long extract_field(const char *buf, const char *key) {
    const char *p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == '=' || *p == ' ') p++;
    return atol(p);
}

static double percentile(std::vector<uint32_t> &sorted_v, double pct) {
    if (sorted_v.empty()) return 0.0;
    size_t idx = (size_t)(pct / 100.0 * (double)(sorted_v.size() - 1));
    return (double)sorted_v[idx];
}

int main(int argc, char *argv[]) {
    if (argc >= 2) g_loss_pct     = atoi(argv[1]);
    if (argc >= 3) g_pps          = atoi(argv[2]);
    if (argc >= 4) g_duration     = atoi(argv[3]);
    if (argc >= 5) g_book_id      = atoi(argv[4]);
    if (argc >= 6) g_outage_pct   = atoi(argv[5]);
    if (argc >= 7) g_outage_dur_s = atoi(argv[6]);
    if (argc >= 8) g_outage_at_s  = atoi(argv[7]);
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
    srand(seed);

    (void)g_book_id; /* book id 由库自适应选择，这里仅用于报告展示 */

    if (GTP_OK != InsLoadGtpModule()) {
        fprintf(stderr, "InsLoadGtpModule failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    g_inst_a.name = "A(sender)";
    g_inst_b.name = "B(recver)";
    g_inst_a.sfd_recv = make_udp_socket(BASE_PORT_A, &g_inst_a.self_addr);
    g_inst_b.sfd_recv = make_udp_socket(BASE_PORT_B, &g_inst_b.self_addr);
    g_inst_a.sfd_send = g_inst_a.sfd_recv;
    g_inst_b.sfd_send = g_inst_b.sfd_recv;
    g_inst_a.self_addr_len = sizeof(g_inst_a.self_addr);
    g_inst_b.self_addr_len = sizeof(g_inst_b.self_addr);

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
    if (INVALID_GTP_HANDLER == g_inst_a.gtp_hdl || INVALID_GTP_HANDLER == g_inst_b.gtp_hdl) {
        fprintf(stderr, "CreateGtpInstance failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    RecvThreadArg ra = {&g_inst_a, g_inst_a.gtp_hdl, &g_lock_a};
    RecvThreadArg rb = {&g_inst_b, g_inst_b.gtp_hdl, &g_lock_b};
    TimerArg      ta = {g_inst_a.gtp_hdl, g_inst_b.gtp_hdl, &g_lock_a, &g_lock_b};

    pthread_t tid_ra, tid_rb, tid_timer;
    pthread_create(&tid_ra,    NULL, recv_thread, &ra);
    pthread_create(&tid_rb,    NULL, recv_thread, &rb);
    pthread_create(&tid_timer, NULL, timer_thread, &ta);

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
            uint8_t  *mem = GtpMallocPackMem(g_inst_a.gtp_hdl, NULL, 0, &mem_len, (void **)&taddr, sizeof(f));
            if (mem) {
                memcpy(mem, &f, sizeof(f));
                taddr->context_       = &g_inst_a;
                taddr->sfd_           = (uint32_t)g_inst_a.sfd_send;
                taddr->stream_type_   = kRealTimeStream;
                taddr->enable_key_    = 0;
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

    /* 尾部排空：给在途的ARQ/FEC恢复一点时间落地，避免把仍在补的帧误计成永久丢失 */
    usleep(300000);

    g_running = 0;
    pthread_join(tid_ra,    NULL);
    pthread_join(tid_rb,    NULL);
    pthread_join(tid_timer, NULL);

    uint8_t alg_buf_a[4096] = {0};
    uint8_t alg_buf_b[4096] = {0};
    uint8_t ip_a[64], ip_b[64];
    inet_ntop(AF_INET, &g_inst_a.self_addr.sin_addr, (char*)ip_a, sizeof(ip_a));
    inet_ntop(AF_INET, &g_inst_b.self_addr.sin_addr, (char*)ip_b, sizeof(ip_b));
    GetAlgorithmParam(g_inst_a.gtp_hdl, ip_a, ip_b, alg_buf_a, sizeof(alg_buf_a));
    GetAlgorithmParam(g_inst_b.gtp_hdl, ip_b, ip_a, alg_buf_b, sizeof(alg_buf_b));

    uint32_t sent  = g_stats.sent_frames.load();
    uint32_t recvd = g_stats.recv_frames.load();
    uint32_t anom  = g_stats.fec_anomaly.load();

    uint32_t unique_seq = 0, dup_total = 0;
    for (auto &kv : g_seq_count) {
        unique_seq += 1;
        if (kv.second > 1) dup_total += (uint32_t)(kv.second - 1);
    }

    std::vector<uint32_t> lat_sorted;
    {
        pthread_mutex_lock(&g_latency_lock);
        lat_sorted = g_latency_us;
        pthread_mutex_unlock(&g_latency_lock);
    }
    std::sort(lat_sorted.begin(), lat_sorted.end());
    double p50 = percentile(lat_sorted, 50);
    double p95 = percentile(lat_sorted, 95);
    double p99 = percentile(lat_sorted, 99);
    double lat_max = lat_sorted.empty() ? 0.0 : (double)lat_sorted.back();
    double lat_avg = 0.0;
    for (auto v : lat_sorted) lat_avg += v;
    if (!lat_sorted.empty()) lat_avg /= (double)lat_sorted.size();

    double duration_s = (double)g_duration;
    double kbps_a2b = (double)g_stats.bytes_a_to_b.load() * 8.0 / 1000.0 / duration_s;
    double kbps_b2a = (double)g_stats.bytes_b_to_a.load() * 8.0 / 1000.0 / duration_s;

    /* ---- 从 A(发送端，ARQ 主战场) 抠 ARQ/boost 相关计数 ---- */
    long a_book_id       = extract_field((const char*)alg_buf_a, "book_id=");
    long rto_resend      = extract_field((const char*)alg_buf_a, "rto_resend_counter=");
    long ack_resend      = extract_field((const char*)alg_buf_a, "ack_resend_counter=");
    long boost_resend    = extract_field((const char*)alg_buf_a, "boost_resend_counter=");
    long ack_err         = extract_field((const char*)alg_buf_a, "ack_err_counter=");
    long stale_skip      = extract_field((const char*)alg_buf_a, "stale_retran_skip_counter=");

    /* ---- 从 B(接收端，FEC解码+重排窗口主战场) 抠 FEC/重排相关计数 ---- */
    long fec_success     = extract_field((const char*)alg_buf_b, "fec_res_sucess_sum=");
    long fec_repeat       = extract_field((const char*)alg_buf_b, "fec_res_repeat_sum=");
    long fec_to_app       = extract_field((const char*)alg_buf_b, "fec_res_to_app_sum=");
    long fec_failed        = extract_field((const char*)alg_buf_b, "fec_res_failed_sum=");
    long late_rescue        = extract_field((const char*)alg_buf_b, "late_rescue=");
    long giveup_drop        = extract_field((const char*)alg_buf_b, "giveup_drop=");
    long b_book_id           = extract_field((const char*)alg_buf_b, "book_id=");

    long total_arq_attempts = rto_resend + ack_resend;
    long total_retran_all   = total_arq_attempts + boost_resend;

    float frame_loss_pct = (sent && recvd <= sent) ? (float)(sent - recvd) * 100.0f / (float)sent : 0.0f;
    float fec_recovery_ratio = (sent > 0) ? (float)fec_to_app * 100.0f / (float)sent : 0.0f;

    printf("\n===================== 场景: loss=%d%% pps=%d dur=%ds", g_loss_pct, g_pps, g_duration);
    if (g_outage_pct > 0) {
        printf(" + 突发(loss=%d%% dur=%ds at=%ds)", g_outage_pct, g_outage_dur_s, g_outage_at_s);
    }
    printf(" =====================\n");

    printf("[帧统计] 发送=%u 接收=%u 去重后=%u 重复投递=%u 帧丢失率=%.2f%%\n",
           sent, recvd, unique_seq, dup_total, frame_loss_pct);
    printf("[FEC]    book_id(send/recv)=%ld/%ld  恢复成功(matrix)=%ld  实际投递给应用=%ld  失败=%ld  重复(matrix内)=%ld  FEC恢复占发送帧比例=%.2f%%\n",
           a_book_id, b_book_id, fec_success, fec_to_app, fec_failed, fec_repeat, fec_recovery_ratio);
    printf("[ARQ]    rto重传=%ld  ack/nack quick-resend=%ld  合计尝试=%ld  彻底失败(ack_err)=%ld  因太旧被跳过(stale_skip)=%ld\n",
           rto_resend, ack_resend, total_arq_attempts, ack_err, stale_skip);
    printf("[boost]  boost副本发送=%ld  占全部重传(含boost)比例=%.2f%%\n",
           boost_resend, (total_retran_all > 0) ? (100.0 * boost_resend / total_retran_all) : 0.0);
    printf("[重排窗] late_rescue(2sn内迟到仍投递)=%ld  giveup_drop(超出sn容忍度丢弃)=%ld\n",
           late_rescue, giveup_drop);
    printf("[网络层] DATA/FEC包: 送达=%u 丢弃=%u(%.1f%%)   ACK/NACK包: 送达=%u 丢弃=%u(%.1f%%)\n",
           g_stats.net_ok_data_fec.load(), g_stats.net_drop_data_fec.load(),
           (g_stats.net_ok_data_fec.load()+g_stats.net_drop_data_fec.load()) ?
               100.0*g_stats.net_drop_data_fec.load()/(g_stats.net_ok_data_fec.load()+g_stats.net_drop_data_fec.load()) : 0.0,
           g_stats.net_ok_ack_nack.load(), g_stats.net_drop_ack_nack.load(),
           (g_stats.net_ok_ack_nack.load()+g_stats.net_drop_ack_nack.load()) ?
               100.0*g_stats.net_drop_ack_nack.load()/(g_stats.net_ok_ack_nack.load()+g_stats.net_drop_ack_nack.load()) : 0.0);
    printf("[延迟us] n=%zu avg=%.0f p50=%.0f p95=%.0f p99=%.0f max=%.0f\n",
           lat_sorted.size(), lat_avg, p50, p95, p99, lat_max);
    printf("[带宽]   A->B(数据方向)=%.1f kbps   B->A(反馈方向)=%.1f kbps  合计=%.1f kbps\n",
           kbps_a2b, kbps_b2a, kbps_a2b + kbps_b2a);
    if (anom > 0) {
        printf("[警告]   fec_anomaly=%u (期望0)\n", anom);
    }

    printf("REPORT,%d,%d,%d,%d,%.2f,%.2f,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%.0f,%.0f,%.0f,%.1f,%.1f,%u\n",
           g_loss_pct, g_pps, g_duration, g_outage_pct,
           frame_loss_pct, fec_recovery_ratio,
           rto_resend, ack_resend, ack_err, stale_skip, boost_resend,
           fec_success, fec_to_app, late_rescue, giveup_drop,
           p50, p95, p99, kbps_a2b, kbps_b2a, anom);

    DeleteGtpInstance(g_inst_a.gtp_hdl);
    DeleteGtpInstance(g_inst_b.gtp_hdl);
    close(g_inst_a.sfd_recv);
    close(g_inst_b.sfd_recv);
    RmLoadGtpModule();
    return 0;
}
