/*
 * traffic_breakdown_test.cpp
 *
 * 单场景流量分类工具：基于 metrics_matrix_test.cpp 的双实例 loopback 骨架，
 * 按 wire 上的 pack_type_ 把发送字节数分成 DATA/FEC/ACK/NACK/RTT 六类，再借助
 * PrintHarqParam() 暴露的 rto_resend_counter/ack_resend_counter/boost_resend_counter
 * (ai_repair_sum_)，把 DATA 类目进一步拆成"首发"/"ARQ重传(RTO+NACK快速重传)"/"boost".
 *
 * 用法：
 *   ./traffic_breakdown_test <loss_pct> <pps> <seconds>
 *   ./traffic_breakdown_test 5 128 12
 *
 * 输出：人类可读的分类占比 + 一行 "REPORT,loss,pps,dur,total_bytes,data_orig_pct,
 * arq_pct,boost_pct,fec_pct,ack_pct,nack_pct,other_pct,book_id" 供外部脚本聚合。
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

#define GTP_RAW_PACK_TYPE(buf)  (((const uint8_t*)(buf))[2] & 0x07u)
enum {
    kPtData = 0x01, kPtAck = 0x02, kPtFec = 0x03, kPtRttSet = 0x04, kPtRttRes = 0x05, kPtNack = 0x06
};

#define LOOPBACK_IP     "127.0.0.1"
#define BASE_PORT_A     19201
#define BASE_PORT_B     19202
#define TIMER_PERIOD_US 10000

typedef struct TestFrame {
    uint32_t magic;
    uint32_t seq;
    uint64_t send_ts_us;
    char     payload[64];
} TestFrame;

struct TypeBytes {
    std::atomic<uint64_t> data{0};
    std::atomic<uint64_t> fec{0};
    std::atomic<uint64_t> ack{0};
    std::atomic<uint64_t> nack{0};
    std::atomic<uint64_t> rtt{0};
    std::atomic<uint64_t> other{0};
    std::atomic<uint64_t> data_pkts{0};
};

struct Stats {
    std::atomic<uint32_t> sent_frames{0};
    std::atomic<uint32_t> recv_frames{0};
    TypeBytes tb;   /* 两个方向合计 */
};

static Stats g_stats;

static int g_loss_pct  = 20;
static int g_pps       = 50;
static int g_duration  = 10;
static volatile int g_running = 1;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

static int should_drop(void) {
    return (rand() % 100) < g_loss_pct;
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

static void classify_bytes(const uint8_t *buf, uint32_t size) {
    uint8_t pt = GTP_RAW_PACK_TYPE(buf);
    switch (pt) {
        case kPtData:   g_stats.tb.data.fetch_add(size); g_stats.tb.data_pkts.fetch_add(1); break;
        case kPtFec:    g_stats.tb.fec.fetch_add(size);  break;
        case kPtAck:    g_stats.tb.ack.fetch_add(size);  break;
        case kPtNack:   g_stats.tb.nack.fetch_add(size); break;
        case kPtRttSet:
        case kPtRttRes: g_stats.tb.rtt.fetch_add(size);  break;
        default:        g_stats.tb.other.fetch_add(size); break;
    }
}

static uint32_t SendPackCbA(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    classify_bytes((const uint8_t*)pack, size);
    if (should_drop()) return GTP_OK;
    InstCtx *ctx = (InstCtx *)addr->context_;
    sendto(ctx->sfd_send, pack, size, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_addr_len);
    return GTP_OK;
}

static uint32_t SendPackCbB(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    classify_bytes((const uint8_t*)pack, size);
    if (should_drop()) return GTP_OK;
    InstCtx *ctx = (InstCtx *)addr->context_;
    sendto(ctx->sfd_send, pack, size, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_addr_len);
    return GTP_OK;
}

static uint32_t RecvFrameCbB(GtpHandler_p hdl, void *frame, uint32_t size, GtpAddr *addr) {
    (void)hdl; (void)addr;
    if (size >= sizeof(TestFrame)) {
        TestFrame *f = (TestFrame *)frame;
        if (f->magic == 0xDEADBEEF) {
            g_stats.recv_frames.fetch_add(1);
        }
    }
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

static long extract_field(const char *buf, const char *key) {
    const char *p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == '=' || *p == ' ') p++;
    return atol(p);
}

int main(int argc, char *argv[]) {
    if (argc >= 2) g_loss_pct  = atoi(argv[1]);
    if (argc >= 3) g_pps       = atoi(argv[2]);
    if (argc >= 4) g_duration  = atoi(argv[3]);
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
    srand(seed);

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

    uint64_t test_start_us = now_us();
    uint64_t interval_us   = 1000000ULL / (uint64_t)g_pps;
    uint64_t deadline      = test_start_us + (uint64_t)g_duration * 1000000ULL;
    uint64_t next_send     = test_start_us;
    uint32_t seq           = 0;

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

    usleep(300000);

    g_running = 0;
    pthread_join(tid_ra,    NULL);
    pthread_join(tid_rb,    NULL);
    pthread_join(tid_timer, NULL);

    uint8_t alg_buf_a[4096] = {0};
    uint8_t ip_a[64], ip_b[64];
    inet_ntop(AF_INET, &g_inst_a.self_addr.sin_addr, (char*)ip_a, sizeof(ip_a));
    inet_ntop(AF_INET, &g_inst_b.self_addr.sin_addr, (char*)ip_b, sizeof(ip_b));
    GetAlgorithmParam(g_inst_a.gtp_hdl, ip_a, ip_b, alg_buf_a, sizeof(alg_buf_a));

    long a_book_id    = extract_field((const char*)alg_buf_a, "book_id=");
    long rto_resend   = extract_field((const char*)alg_buf_a, "rto_resend_counter=");
    long ack_resend   = extract_field((const char*)alg_buf_a, "ack_resend_counter=");
    long boost_resend = extract_field((const char*)alg_buf_a, "boost_resend_counter=");

    uint64_t data_bytes  = g_stats.tb.data.load();
    uint64_t data_pkts   = g_stats.tb.data_pkts.load();
    uint64_t fec_bytes   = g_stats.tb.fec.load();
    uint64_t ack_bytes   = g_stats.tb.ack.load();
    uint64_t nack_bytes  = g_stats.tb.nack.load();
    uint64_t rtt_bytes   = g_stats.tb.rtt.load();
    uint64_t other_bytes = g_stats.tb.other.load();
    uint64_t total_bytes = data_bytes + fec_bytes + ack_bytes + nack_bytes + rtt_bytes + other_bytes;

    double avg_data_pkt = (data_pkts > 0) ? (double)data_bytes / (double)data_pkts : 0.0;

    long retran_pkts = (rto_resend > 0 ? rto_resend : 0) + (ack_resend > 0 ? ack_resend : 0);
    long boost_pkts  = (boost_resend > 0 ? boost_resend : 0);
    uint32_t sent_frames = g_stats.sent_frames.load();

    /* data_pkts 里可能因为尾部排空/边界效应跟 sent_frames+retran+boost 对不上，用比例而不是硬拆分 */
    double orig_bytes  = avg_data_pkt * (double)sent_frames;
    double arq_bytes_d = avg_data_pkt * (double)retran_pkts;
    double boost_bytes_d = avg_data_pkt * (double)boost_pkts;
    /* 归一化，让三者之和精确等于 data_bytes（wire 上实际统计值最准） */
    double split_sum = orig_bytes + arq_bytes_d + boost_bytes_d;
    double norm = (split_sum > 0) ? (double)data_bytes / split_sum : 0.0;
    orig_bytes    *= norm;
    arq_bytes_d   *= norm;
    boost_bytes_d *= norm;

    double pct_orig  = total_bytes ? 100.0 * orig_bytes    / (double)total_bytes : 0.0;
    double pct_arq   = total_bytes ? 100.0 * arq_bytes_d   / (double)total_bytes : 0.0;
    double pct_boost = total_bytes ? 100.0 * boost_bytes_d / (double)total_bytes : 0.0;
    double pct_fec   = total_bytes ? 100.0 * (double)fec_bytes   / (double)total_bytes : 0.0;
    double pct_ack   = total_bytes ? 100.0 * (double)ack_bytes   / (double)total_bytes : 0.0;
    double pct_nack  = total_bytes ? 100.0 * (double)nack_bytes  / (double)total_bytes : 0.0;
    double pct_other = total_bytes ? 100.0 * (double)(rtt_bytes + other_bytes) / (double)total_bytes : 0.0;

    double duration_s = (double)g_duration;
    double kbps_total = (double)total_bytes * 8.0 / 1000.0 / duration_s;

    printf("\n===== loss=%d%% pps=%d dur=%ds book_id=%ld =====\n", g_loss_pct, g_pps, g_duration, a_book_id);
    printf("发送帧=%u 接收帧=%u  rto_resend=%ld ack_resend=%ld boost_resend=%ld\n",
           sent_frames, g_stats.recv_frames.load(), rto_resend, ack_resend, boost_resend);
    printf("总流量=%.1f KB (%.1f kbps)  DATA平均包长=%.0fB\n",
           total_bytes / 1000.0, kbps_total, avg_data_pkt);
    printf("  DATA-首发   %6.1f%%  (%.1f KB)\n", pct_orig,  orig_bytes/1000.0);
    printf("  DATA-ARQ重传 %5.1f%%  (%.1f KB)   [rto+ack quick-resend]\n", pct_arq, arq_bytes_d/1000.0);
    printf("  DATA-boost  %6.1f%%  (%.1f KB)\n", pct_boost, boost_bytes_d/1000.0);
    printf("  FEC         %6.1f%%  (%.1f KB)\n", pct_fec,   fec_bytes/1000.0);
    printf("  ACK         %6.1f%%  (%.1f KB)\n", pct_ack,   ack_bytes/1000.0);
    printf("  NACK        %6.1f%%  (%.1f KB)\n", pct_nack,  nack_bytes/1000.0);
    printf("  其他(RTT等)  %5.1f%%  (%.1f KB)\n", pct_other, (rtt_bytes+other_bytes)/1000.0);

    printf("REPORT,%d,%d,%d,%llu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%ld\n",
           g_loss_pct, g_pps, g_duration, (unsigned long long)total_bytes,
           pct_orig, pct_arq, pct_boost, pct_fec, pct_ack, pct_nack, pct_other, a_book_id);

    DeleteGtpInstance(g_inst_a.gtp_hdl);
    DeleteGtpInstance(g_inst_b.gtp_hdl);
    close(g_inst_a.sfd_recv);
    close(g_inst_b.sfd_recv);
    RmLoadGtpModule();
    return 0;
}
