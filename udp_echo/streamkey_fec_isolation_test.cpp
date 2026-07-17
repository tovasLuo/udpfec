/*
 * streamkey_fec_isolation_test.cpp
 *
 * 验证："带 stream_key 的 FEC 包会不会污染矩阵起始序号，导致 FEC 恢复实际失效，
 * 但因为 ARQ 兜底所以最终应用层数据仍然正确、常规测试看不出来"这个猜测。
 *
 * 方法：把 B→A 方向（ACK/NACK/RTT 反馈）完全阻断（100%丢弃），这样 A 端永远收不到
 * 任何反馈，ARQ 的 quick-resend/RTO 重传全部失效（ProcAck/ProcNack 从未被调用，
 * CheckRtoRetran 里 kRealTimeStream 分支在 recv_stat_.ack_sum_==0 时直接不重传——
 * 见 goodtp_arq.cpp JudgeCanRtoReSend 开头那个 early return）。这样应用层最终收到
 * 的帧，只可能来自两个来源：直接送达 或 FEC 矩阵恢复，不存在"FEC 没恢复但 ARQ 兜底"
 * 这条掩盖路径。
 *
 * 同一个 RNG seed 分别跑 use_key=0 和 use_key=1 两遍（丢包位置完全一致），对比帧丢失率：
 * 如果 stream_key 破坏了 FEC 矩阵起始序号，use_key=1 应该表现出明显更高的帧丢失率
 * （FEC 该恢复的没恢复），而 use_key=0 作为基线应该不受影响。
 *
 * 用法：./streamkey_fec_isolation_test <loss_pct> <pps> <seconds> [book_id] [seed] [warmup_s] [burst_len]
 *   burst_len=1(默认)：独立随机丢包；burst_len>1：Gilbert-Elliott连续丢包，平均连续丢N个包。
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
#include <unordered_map>

#include "bitlinker.h"

#define LOOPBACK_IP     "127.0.0.1"
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
    std::atomic<uint32_t> fec_sn_anomaly{0};   /* FEC包pack_sn_(矩阵起始序号)看起来被污染 */
};

static Stats g_stats;

static int g_loss_pct = 20;
static int g_pps      = 100;
static int g_duration = 15;
static int g_book_id  = 4;
static int g_use_key  = 0;
static unsigned g_seed = 1;
static int g_warmup_s = 0;   /* 默认0=从t=0起就阻断反馈，彻底锁死RTO(见SendPackCbB注释)；
                                 传>0则牺牲隔离纯度换book4，仅用于对比 */
static volatile int g_running = 1;
static uint64_t g_measure_start_us = 0;  /* 预热结束时间点；早于此的发送/接收不计入统计 */

#define TEST_STREAM_KEY  0x1234567890ABCDEFULL

/* pack_type_ 在第一个u32的bits[16..18]，pack_sn_是紧接着的第二个u32(固定偏移，不受
 * header_offset_/stream_key影响)。直接读裸字节，不走库的任何解析路径，作为独立于
 * 库内部记账的"地面真相"检查。 */
static inline uint32_t raw_pack_type(const uint8_t *buf) { return buf[2] & 0x07u; }
#define RAW_FEC_PACK_TYPE 0x03u
static inline uint32_t raw_pack_sn(const uint8_t *buf) {
    uint32_t sn = 0;
    memcpy(&sn, buf + 4, sizeof(sn));
    return sn;
}

/* TEST_STREAM_KEY = 0x1234567890ABCDEF 拆成 hash(低32位,先写)/user_id(高32位,后写)。
 * 正常 pack_sn_ 在这个测试规模下应该在几千以内；如果 FEC 包的 pack_sn_ 撞上这两个
 *值(或字节错位后的中间值)，且明显超出正常 pack_sn_ 应有的量级，判定为异常。 */
static const uint32_t kKeyHashHalf   = (uint32_t)(TEST_STREAM_KEY & 0xFFFFFFFFULL);        /* 0x90ABCDEF */
static const uint32_t kKeyUserIdHalf = (uint32_t)((TEST_STREAM_KEY >> 32) & 0xFFFFFFFFULL); /* 0x12345678 */
static std::atomic<uint32_t> g_max_data_pack_sn{0};

static void check_fec_pack_sn(const uint8_t *buf, uint32_t size, const char *dir) {
    if ((size < 8) || (RAW_FEC_PACK_TYPE != raw_pack_type(buf))) {
        return;
    }
    uint32_t sn = raw_pack_sn(buf);
    uint32_t plausible_ceiling = g_max_data_pack_sn.load() + 1000;
    static const uint64_t kKey = TEST_STREAM_KEY;
    bool looks_like_key = (sn == kKeyHashHalf) || (sn == kKeyUserIdHalf)
                         || (0 == memcmp(&sn, ((const uint8_t*)&kKey) + 2, 4))  /* 中间4字节切片 */
                         || (0 == memcmp(&sn, ((const uint8_t*)&kKey) + 1, 4));
    bool out_of_range = (sn > plausible_ceiling) && (sn > 100000);
    if (looks_like_key || out_of_range) {
        g_stats.fec_sn_anomaly.fetch_add(1);
        fprintf(stderr, "[FEC-SN-ANOMALY] dir=%s pack_sn_=0x%08x(%u) plausible_ceiling=%u "
                "key_hash_half=0x%08x key_userid_half=0x%08x looks_like_key=%d out_of_range=%d\n",
                dir, sn, sn, plausible_ceiling, kKeyHashHalf, kKeyUserIdHalf,
                (int)looks_like_key, (int)out_of_range);
    }
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

typedef struct InstCtx {
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

/* A->B（DATA/FEC）：用独立的确定性 RNG 流，两次运行(use_key=0/1) 用同一个 seed 保证
 * 丢包位置完全一致，只有 use_key 这一个变量在变。
 *
 * 两种丢包模型：
 * - 独立随机（g_burst_len<=1）：每个包独立按 g_loss_pct 概率丢，块内同时丢2个包的概率
 *   很低，对 book4(2x2) 这种双轴冗余基本是"送分题"。
 * - 连续丢包/Gilbert-Elliott 两状态模型（g_burst_len>1）：GOOD/BAD 两态马尔可夫链，
 *   进入 BAD 态后连续丢包，平均连续丢包长度 = g_burst_len，整体丢包率仍收敛到
 *   g_loss_pct。真实 WiFi/拥塞场景的丢包基本是这种连续掉一串，而不是独立稀疏丢单包——
 *   连续丢掉同一个 2x2 矩阵块里的 2 个包（同一行或同一列）会让那一根轴的 FEC 完全
 *   救不回来，只能指望另一根轴，是比独立随机丢包严格更狠的压力测试。 */
static unsigned g_rng_state;
static int g_burst_len = 1;   /* 平均连续丢包长度；1=独立随机丢包 */
static int g_burst_in_loss_state = 0;

static double rng_uniform(void) {
    g_rng_state = g_rng_state * 1103515245u + 12345u;
    /* (state>>16) 只有16位熵(0-65535)，之前直接对1000000取模等于没取模，把返回值钳死在
     * [0, 0.0655) —— 造成任何 loss_pct>=7% 的阈值比较 (rng<threshold) 恒真，独立随机丢包
     * 模式(burst_len<=1)因此变成了100%丢包。改成对24位量级(state>>8, 0-16777215)取模，
     * 覆盖满 [0,1) 区间。 */
    return (double)((g_rng_state >> 8) % 1000000u) / 1000000.0;
}

static int should_drop_a2b(void) {
    if (g_burst_len <= 1) {
        return rng_uniform() < ((double)g_loss_pct / 100.0);
    }

    /* 两状态马尔可夫链：P(exit_bad)=1/burst_len 给出几何分布的平均连续丢包长度；
     * P(enter_bad) 反解使稳态丢包率 = g_loss_pct/100。 */
    const double loss_frac = (double)g_loss_pct / 100.0;
    const double p_exit    = 1.0 / (double)g_burst_len;
    const double p_enter   = (1.0 >= loss_frac) ? (p_exit * loss_frac / (1.0 - loss_frac)) : 1.0;

    if (g_burst_in_loss_state) {
        if (rng_uniform() < p_exit) {
            g_burst_in_loss_state = 0;
        }
        return 1;
    }

    if (rng_uniform() < p_enter) {
        g_burst_in_loss_state = 1;
        return 1;
    }
    return 0;
}

static uint32_t SendPackCbA(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    const uint8_t *raw = (const uint8_t*)pack;
    if ((size >= 8) && (0x01u == raw_pack_type(raw))) {  /* DATA包：记录当前pack_sn_量级作为地面真相 */
        uint32_t sn = raw_pack_sn(raw);
        uint32_t prev = g_max_data_pack_sn.load();
        if (sn > prev) g_max_data_pack_sn.store(sn);
    }
    check_fec_pack_sn(raw, size, "send(A构造出的原始包,丢包判定前)");

    if (should_drop_a2b()) return GTP_OK;
    InstCtx *ctx = (InstCtx *)addr->context_;
    sendto(ctx->sfd_send, pack, size, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_addr_len);
    return GTP_OK;
}

/* B->A（ACK/NACK/RTT 反馈）：g_warmup_s==0(默认)时从第0秒起就100%阻断——这样A的
 * recv_stat_.ack_sum_永远是0，JudgeCanRtoReSend()开头那个"kRealTimeStream且ack_sum_==0
 * 直接kNotRetranType"的早退会锁死RTO重传，才是真正干净的"只剩FEC"隔离。
 *
 * 早期版本用非0预热期换book4（quick-resend/RTO都需要反馈才能触发，以为切完book就能
 * 安全阻断），但漏想了一点：ack_sum_是历史累计值，预热期哪怕只成功收到一次ACK，
 * ack_sum_就再也回不到0了——RTO这个早退门槛从此永久失效，测量期虽然收不到新反馈，
 * 但本地"未确认即当丢"的估计(s_cur_loss_rate_，不需要任何反馈也会自然走高)照样能让
 * RTO重传和boost继续开火，混进了FEC之外的救回来源，把对比结果稀释了(第一轮矩阵测试
 * 就是在这个状态下跑的，看到的"帧丢失率"其实是"FEC+盲发RTO+boost"的合力，不是纯FEC)。
 * 默认改回0预热，代价是拿不到book4(没反馈学不到该切哪个book，停留在初始默认book，
 * 实测是book5)——但book5同样要走start_pack_sn_这套记账，且是单轴，没有H+V双轴互相
 * 掩盖的问题，反而更适合暴露"矩阵起始序号被污染"这类bug。 */
static uint32_t SendPackCbB(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    if (now_us() >= g_measure_start_us) {
        return GTP_OK;
    }
    InstCtx *ctx = (InstCtx *)addr->context_;
    sendto(ctx->sfd_send, pack, size, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_addr_len);
    return GTP_OK;
}

static std::unordered_map<uint32_t,int> g_seq_count;
static pthread_mutex_t g_seq_count_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t RecvFrameCbB(GtpHandler_p hdl, void *frame, uint32_t size, GtpAddr *addr) {
    (void)hdl; (void)addr;
    if (size >= sizeof(TestFrame)) {
        TestFrame *f = (TestFrame *)frame;
        if ((f->magic == 0xDEADBEEF) && (f->send_ts_us >= g_measure_start_us)) {
            g_stats.recv_frames.fetch_add(1);
            pthread_mutex_lock(&g_seq_count_lock);
            ++g_seq_count[f->seq];
            pthread_mutex_unlock(&g_seq_count_lock);
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
    if (strstr(buf, "fec restore abnormal")) g_stats.fec_anomaly.fetch_add(1);
    fprintf(stderr, "[gtp] %s", buf);
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
        check_fec_pack_sn(buf, (uint32_t)n, "recv(线路上收到的原始字节,库处理前)");
        pthread_mutex_lock(a->lock);
        uint32_t  mem_len  = 0;
        GtpAddr  *tran_addr = NULL;
        uint8_t  *pack_mem = GtpMallocPackMem(a->gtp_hdl, NULL, 0, &mem_len, (void **)&tran_addr, (uint32_t)n);
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
            tran_addr->enable_key_ = 1;
            tran_addr->stream_key_ = sk;
        } else {
            tran_addr->enable_key_ = 0;
        }

        uint32_t ret = GtpPacketReceive(a->gtp_hdl, pack_mem, (uint32_t)n, tran_addr);
        if (GTP_OK != ret) GtpFreePackMem(a->gtp_hdl, pack_mem);
        pthread_mutex_unlock(a->lock);
    }
    return NULL;
}

typedef struct TimerArg {
    GtpHandler_p     hdl_a, hdl_b;
    pthread_mutex_t *lock_a, *lock_b;
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

/* 从 GetAlgorithmParam() 的文本输出里挖字段值，形如 " key= 123" */
static long extract_field(const char *buf, const char *key) {
    const char *p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == '=' || *p == ' ') p++;
    return atol(p);
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
    if (bind(fd, (struct sockaddr *)out_addr, sizeof(*out_addr)) < 0) { perror("bind"); close(fd); return -1; }
    struct timeval tv = {0, 200000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    inet_pton(AF_INET, LOOPBACK_IP, &out_addr->sin_addr);
    return fd;
}

static int run_once(uint16_t port_a, uint16_t port_b) {
    g_stats.sent_frames = 0;
    g_stats.recv_frames = 0;
    g_stats.fec_anomaly = 0;
    g_stats.fec_sn_anomaly = 0;
    g_max_data_pack_sn = 0;
    g_seq_count.clear();
    g_rng_state = g_seed;
    g_burst_in_loss_state = 0;
    g_running = 1;

    if (GTP_OK != InsLoadGtpModule()) {
        fprintf(stderr, "InsLoadGtpModule failed: %s\n", LastGtpErrorInfo());
        return -1;
    }

    g_inst_a.sfd_recv = make_udp_socket(port_a, &g_inst_a.self_addr);
    g_inst_b.sfd_recv = make_udp_socket(port_b, &g_inst_b.self_addr);
    g_inst_a.sfd_send = g_inst_a.sfd_recv;
    g_inst_b.sfd_send = g_inst_b.sfd_recv;
    g_inst_a.self_addr_len = sizeof(g_inst_a.self_addr);
    g_inst_b.self_addr_len = sizeof(g_inst_b.self_addr);

    memset(&g_inst_a.peer_addr, 0, sizeof(g_inst_a.peer_addr));
    g_inst_a.peer_addr.sin_family = AF_INET;
    g_inst_a.peer_addr.sin_port   = htons(port_b);
    inet_pton(AF_INET, LOOPBACK_IP, &g_inst_a.peer_addr.sin_addr);
    g_inst_a.peer_addr_len = sizeof(g_inst_a.peer_addr);

    memset(&g_inst_b.peer_addr, 0, sizeof(g_inst_b.peer_addr));
    g_inst_b.peer_addr.sin_family = AF_INET;
    g_inst_b.peer_addr.sin_port   = htons(port_a);
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
        return -1;
    }

    RecvThreadArg ra = {&g_inst_a, g_inst_a.gtp_hdl, &g_lock_a};
    RecvThreadArg rb = {&g_inst_b, g_inst_b.gtp_hdl, &g_lock_b};
    TimerArg      ta = {g_inst_a.gtp_hdl, g_inst_b.gtp_hdl, &g_lock_a, &g_lock_b};

    pthread_t tid_ra, tid_rb, tid_timer;
    pthread_create(&tid_ra,    NULL, recv_thread, &ra);
    pthread_create(&tid_rb,    NULL, recv_thread, &rb);
    pthread_create(&tid_timer, NULL, timer_thread, &ta);

    uint64_t start_us    = now_us();
    uint64_t interval_us = 1000000ULL / (uint64_t)g_pps;
    g_measure_start_us   = start_us + (uint64_t)g_warmup_s * 1000000ULL;
    uint64_t deadline    = g_measure_start_us + (uint64_t)g_duration * 1000000ULL;
    uint64_t next_send   = start_us;
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
                taddr->enable_key_    = (uint32_t)g_use_key;
                taddr->stream_key_    = g_use_key ? TEST_STREAM_KEY : 0;
                taddr->self_addr_len_ = (uint32_t)sizeof(g_inst_a.self_addr);
                memcpy(taddr->self_addr_, &g_inst_a.self_addr, sizeof(g_inst_a.self_addr));
                taddr->sock_addr_len_ = (uint32_t)sizeof(g_inst_a.peer_addr);
                memcpy(taddr->sock_addr_, &g_inst_a.peer_addr, sizeof(g_inst_a.peer_addr));

                uint32_t ret = GtpFrameSend(g_inst_a.gtp_hdl, mem, sizeof(f), taddr, 0, 0);
                if (GTP_OK == ret) {
                    if (f.send_ts_us >= g_measure_start_us) g_stats.sent_frames.fetch_add(1);
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

    uint32_t sent  = g_stats.sent_frames.load();
    uint32_t recvd = g_stats.recv_frames.load();
    uint32_t anom  = g_stats.fec_anomaly.load();
    uint32_t sn_anom = g_stats.fec_sn_anomaly.load();
    uint32_t unique_seq = 0, dup_total = 0;
    for (auto &kv : g_seq_count) {
        unique_seq += 1;
        if (kv.second > 1) dup_total += (uint32_t)(kv.second - 1);
    }

    uint8_t alg_buf_a[4096] = {0};
    uint8_t alg_buf_b[4096] = {0};
    uint8_t ip_a[64], ip_b[64];
    inet_ntop(AF_INET, &g_inst_a.self_addr.sin_addr, (char*)ip_a, sizeof(ip_a));
    inet_ntop(AF_INET, &g_inst_b.self_addr.sin_addr, (char*)ip_b, sizeof(ip_b));
    GetAlgorithmParam(g_inst_a.gtp_hdl, ip_a, ip_b, alg_buf_a, sizeof(alg_buf_a));
    GetAlgorithmParam(g_inst_b.gtp_hdl, ip_b, ip_a, alg_buf_b, sizeof(alg_buf_b));

    float frame_loss_pct = (sent && recvd <= sent) ? (float)(sent - recvd) * 100.0f / (float)sent : -1.0f;

    long a_rto   = extract_field((const char*)alg_buf_a, "rto_resend_counter=");
    long a_ack   = extract_field((const char*)alg_buf_a, "ack_resend_counter=");
    long a_boost = extract_field((const char*)alg_buf_a, "boost_resend_counter=");
    long a_book  = extract_field((const char*)alg_buf_a, "book_id=");
    long b_fec_to_app = extract_field((const char*)alg_buf_b, "fec_res_to_app_sum=");

    printf("use_key=%d  sent=%u recvd=%u unique=%u dup=%u anomaly=%u fec_sn_anomaly=%u  帧丢失率=%.2f%%\n",
           g_use_key, sent, recvd, unique_seq, dup_total, anom, sn_anom, frame_loss_pct);
    printf("  [隔离校验] A侧book=%ld rto_resend=%ld ack_resend=%ld boost_resend=%ld"
           " (三个应接近0才是真正的'纯FEC'隔离)  B侧fec_res_to_app=%ld\n",
           a_book, a_rto, a_ack, a_boost, b_fec_to_app);
    printf("--- A(sender/encoder)算法参数 ---\n%s\n", alg_buf_a);
    printf("--- B(recver)算法参数 ---\n%s\n", alg_buf_b);

    DeleteGtpInstance(g_inst_a.gtp_hdl);
    DeleteGtpInstance(g_inst_b.gtp_hdl);
    close(g_inst_a.sfd_recv);
    close(g_inst_b.sfd_recv);
    RmLoadGtpModule();
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc >= 2) g_loss_pct  = atoi(argv[1]);
    if (argc >= 3) g_pps       = atoi(argv[2]);
    if (argc >= 4) g_duration  = atoi(argv[3]);
    if (argc >= 5) g_book_id   = atoi(argv[4]);
    if (argc >= 6) g_seed      = (unsigned)atoi(argv[5]);
    if (argc >= 7) g_warmup_s  = atoi(argv[6]);
    if (argc >= 8) g_burst_len = atoi(argv[7]);
    (void)g_book_id;

    printf("=== streamkey_fec_isolation_test === loss=%d%% pps=%d warmup=%ds measure_dur=%ds seed=%u "
           "burst_len=%d(平均连续丢包长度,1=独立随机) (预热期B->A放行让book切到4，测量期100%%阻断只剩FEC)\n\n",
           g_loss_pct, g_pps, g_warmup_s, g_duration, g_seed, g_burst_len);

    printf(">>> Round 1: use_key=0 (baseline)\n");
    g_use_key = 0;
    run_once(19201, 19202);

    printf("\n>>> Round 2: use_key=1 (同样的丢包序列)\n");
    g_use_key = 1;
    run_once(19203, 19204);

    return 0;
}
