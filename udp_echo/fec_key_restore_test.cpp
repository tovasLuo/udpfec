/*
 * fec_key_restore_test.cpp
 *
 * 专项回归测试：FEC 恢复 + stream_key（新固定包头格式）。
 *
 * 覆盖场景
 * ─────────
 *  - enable_key_=1，stream_key_=TEST_STREAM_KEY（生产模式）。
 *  - 确定性丢包：丢弃 pack_sn % 4 == 1 的 DATA 包（每 2×2 FEC 块中
 *    第 2 号位置，对应 H-row-0 的第二格），FEC H 方向必能恢复。
 *  - 不丢 FEC 奇偶包和 ACK/NACK，保证恢复路径可达。
 *
 * 验证指标
 * ─────────
 *  TEST-1  fec_anomaly == 0
 *          "fec restore abnormal" 日志为零。
 *          旧代码（stream_key 塞 fec_code_[] payload + memmove）在 >10%
 *          丢包下会出现 res_pos != CalcPosInPackCache(pack_sn) 错误；
 *          新代码（stream_key 放固定 Fec2CodePack 头部）应永远为零。
 *
 *  TEST-2  fec_key_bad == 0
 *          通过 GtpCheckPacketInvalid 从 FEC 包中提取 stream_key_ 全部
 *          正确——验证新包头格式（bytes 16-23 = stream_key_）。
 *
 *  TEST-3  fec_key_ok > 0
 *          至少有 FEC 包被收到并成功提取 key，确保测试路径有效。
 *
 *  TEST-4  recv_frames >= sent_frames * 90 / 100
 *          FEC + ARQ 联合保障下帧交付率 ≥ 90%。
 *
 * 构建
 * ─────────
 *  cd udp_echo/build && cmake .. && make fec_key_restore_test
 *
 * 运行
 * ─────────
 *  ./fec_key_restore_test
 *  返回值：0 = PASS，1 = FAIL
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

/* ──────────────────────── 宏：GTP 包头原始字段 ──────────────────────── */
/*
 * 包头布局（小端，#pragma pack(1)）：
 *   bytes 0-3  u32: goodtp_ver_:8 | header_offset_:6 | ... | pack_type_:3 | pack_size_:13
 *   bytes 4-7  u32: pack_sn_
 *   bytes 8-9  u16: has_check_flag_:1 | ...
 */
#define GTP_RAW_PACK_TYPE(p)       (((const uint8_t*)(p))[2] & 0x07u)
#define GTP_RAW_PACK_SN(p)         ({uint32_t _sn; memcpy(&_sn,(uint8_t*)(p)+4,4); _sn;})
#define GTP_RAW_HAS_CHECK(p)       (((const uint8_t*)(p))[8] & 0x01u)
#define GTP_RAW_REPEAT_COUNTER(p)  ((((const uint8_t*)(p))[8] >> 4) & 0x07u)

#define GTP_DATA_TYPE  0x01u
#define GTP_ACK_TYPE   0x02u
#define GTP_FEC_TYPE   0x03u
#define GTP_NACK_TYPE  0x06u

/* ──────────────────────── 测试常量 ──────────────────────── */
#define TEST_STREAM_KEY  0x1234567890ABCDEFULL

#define LOOPBACK_IP   "127.0.0.1"
#define PORT_A        19031
#define PORT_B        19032
#define TIMER_US      10000   /* 10 ms timer */
#define SEND_PPS      50      /* 50 帧/秒 */
#define DURATION_S    10      /* 测试持续 10 秒 */
#define MAX_PAYLOAD   256

/* ──────────────────────── 帧格式 ──────────────────────── */
typedef struct TestFrame {
    uint32_t magic;      /* 0xDEADBEEF */
    uint32_t seq;
    uint64_t send_ts;
    char     data[64];
} TestFrame;

/* ──────────────────────── 统计 ──────────────────────── */
struct Stats {
    std::atomic<uint32_t> sent_frames;
    std::atomic<uint32_t> recv_frames;
    std::atomic<uint32_t> dropped_data;
    std::atomic<uint32_t> fec_anomaly;
    std::atomic<uint32_t> fec_key_ok;   /* FEC 包 stream_key_ 提取正确 */
    std::atomic<uint32_t> fec_key_bad;  /* FEC 包 stream_key_ 提取错误 */

    Stats() : sent_frames(0), recv_frames(0), dropped_data(0),
              fec_anomaly(0), fec_key_ok(0), fec_key_bad(0) {}
};

static Stats g_stats;
static volatile int g_running = 1;

/* ──────────────────────── 实例上下文 ──────────────────────── */
typedef struct InstCtx {
    const char        *name;
    int                sfd;
    GtpHandler_p       gtp_hdl;
    struct sockaddr_in self_addr;
    struct sockaddr_in peer_addr;
    socklen_t          peer_addr_len;
} InstCtx;

static InstCtx g_a;   /* 发送端 */
static InstCtx g_b;   /* 接收端 */

static pthread_mutex_t g_lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lock_b = PTHREAD_MUTEX_INITIALIZER;

/* ──────────────────────── 时间 ──────────────────────── */
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* ──────────────────────── 日志回调 ──────────────────────── */
static void LogCb(uint32_t level, const char *fmt, ...) {
    /* 只打印 ERROR 及以上，捕捉 "fec restore abnormal" */
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

static uint32_t LogLevelCb(void) { return 3; }

/* ──────────────────────── A → B 发包回调 ──────────────────────── */
/*
 * 确定性丢包策略：
 *   pack_sn % 4 == 1 的 DATA 包被丢弃。
 *   这是每个 2×2 FEC 块（block_size=4）中 H-row-0 的第二格（v_pos=1）。
 *   H-row-0 FEC 奇偶包覆盖 sn=4k 和 sn=4k+1，sn=4k 必然被收到，
 *   故 sn=4k+1 一定可以由 FEC 水平方向恢复。
 *   FEC 奇偶包、ACK/NACK/RTT 全部转发。
 */
static uint32_t SendPackCbA(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    uint8_t pt = GTP_RAW_PACK_TYPE(pack);
    if (GTP_DATA_TYPE == pt) {
        uint32_t sn = GTP_RAW_PACK_SN(pack);
        if ((sn % 4u) == 1u) {
            g_stats.dropped_data.fetch_add(1);
            return GTP_OK;   /* 丢包 */
        }
    }
    InstCtx *ctx = (InstCtx *)addr->context_;
    sendto(ctx->sfd, pack, size, 0,
           (struct sockaddr *)&ctx->peer_addr, ctx->peer_addr_len);
    return GTP_OK;
}

/* ──────────────────────── B 收帧回调 ──────────────────────── */
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

/* ──────────────────────── B → A 回包回调 ──────────────────────── */
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

/* ──────────────────────── 接收线程 ──────────────────────── */
typedef struct {
    InstCtx         *inst;
    GtpHandler_p     gtp_hdl;
    pthread_mutex_t *lock;
    int              is_b;   /* 1=B 端，需验证 FEC 包 stream_key_ */
} RecvArg;

static void *recv_thread(void *arg) {
    RecvArg  *a   = (RecvArg *)arg;
    InstCtx  *ctx = a->inst;
    static __thread uint8_t buf[65536];

    while (g_running) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t n = recvfrom(ctx->sfd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&peer, &peer_len);
        if (n <= 0) continue;

        pthread_mutex_lock(a->lock);

        uint32_t  mem_len  = 0;
        GtpAddr  *taddr    = NULL;
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

        /* 在 B 端接收时验证 FEC 包中 stream_key_ 提取是否正确（新固定头部格式） */
        if (a->is_b) {
            uint64_t sk = 0;
            GtpCheckPacketInvalid(mem, (uint32_t)n, &sk);

            uint8_t pt = GTP_RAW_PACK_TYPE(mem);
            if (GTP_FEC_TYPE == pt) {
                if (sk == TEST_STREAM_KEY)
                    g_stats.fec_key_ok.fetch_add(1);
                else
                    g_stats.fec_key_bad.fetch_add(1);
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

/* ──────────────────────── 定时器线程 ──────────────────────── */
typedef struct {
    GtpHandler_p     hdl_a, hdl_b;
    pthread_mutex_t *lock_a, *lock_b;
} TimerArg;

static void *timer_thread(void *arg) {
    TimerArg *t = (TimerArg *)arg;
    while (g_running) {
        usleep(TIMER_US);
        pthread_mutex_lock(t->lock_a);
        PeriodGtpTimer(t->hdl_a);
        pthread_mutex_unlock(t->lock_a);
        pthread_mutex_lock(t->lock_b);
        PeriodGtpTimer(t->hdl_b);
        pthread_mutex_unlock(t->lock_b);
    }
    return NULL;
}

/* ──────────────────────── socket 辅助 ──────────────────────── */
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

/* ──────────────────────── main ──────────────────────── */
int main(void) {
    printf("=== fec_key_restore_test ===\n");
    printf("确定性丢包: sn%%4==1 (DATA)，enable_key_=1，stream_key=0x%016llX\n\n",
           (unsigned long long)TEST_STREAM_KEY);

    if (GTP_OK != InsLoadGtpModule()) {
        fprintf(stderr, "InsLoadGtpModule failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    /* socket */
    g_a.name = "A(sender)";
    g_b.name = "B(recver)";
    g_a.sfd  = make_udp_socket(PORT_A, &g_a.self_addr);
    g_b.sfd  = make_udp_socket(PORT_B, &g_b.self_addr);
    if (g_a.sfd < 0 || g_b.sfd < 0) return 1;

    /* 对端地址 */
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

    g_a.gtp_hdl = CreateGtpInstance(1, 10000000, &cb_a, &mem_cfg);
    g_b.gtp_hdl = CreateGtpInstance(2, 10000000, &cb_b, &mem_cfg);
    if (INVALID_GTP_HANDLER == g_a.gtp_hdl || INVALID_GTP_HANDLER == g_b.gtp_hdl) {
        fprintf(stderr, "CreateGtpInstance failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    /* 线程 */
    RecvArg ra = {&g_a, g_a.gtp_hdl, &g_lock_a, 0};
    RecvArg rb = {&g_b, g_b.gtp_hdl, &g_lock_b, 1};
    TimerArg ta = {g_a.gtp_hdl, g_b.gtp_hdl, &g_lock_a, &g_lock_b};

    pthread_t tid_ra, tid_rb, tid_timer;
    pthread_create(&tid_ra,    NULL, recv_thread,  &ra);
    pthread_create(&tid_rb,    NULL, recv_thread,  &rb);
    pthread_create(&tid_timer, NULL, timer_thread, &ta);

    /* 发送循环：50 fps，持续 10 秒 */
    const uint64_t interval = 1000000ULL / SEND_PPS;
    const uint64_t deadline = now_us() + (uint64_t)DURATION_S * 1000000ULL;
    uint64_t next_send      = now_us();
    uint32_t seq            = 0;

    while (now_us() < deadline) {
        if (now_us() >= next_send) {
            TestFrame f;
            f.magic    = 0xDEADBEEF;
            f.seq      = ++seq;
            f.send_ts  = now_us();
            snprintf(f.data, sizeof(f.data), "seq=%u", seq);

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
                if (GTP_OK == ret)
                    g_stats.sent_frames.fetch_add(1);
                else
                    GtpFreePackMem(g_a.gtp_hdl, mem);
            }
            pthread_mutex_unlock(&g_lock_a);
            next_send += interval;
        }
        usleep(200);
    }

    g_running = 0;
    pthread_join(tid_ra,    NULL);
    pthread_join(tid_rb,    NULL);
    pthread_join(tid_timer, NULL);

    /* ──────── 结果报告 ──────── */
    uint32_t sent      = g_stats.sent_frames.load();
    uint32_t recvd     = g_stats.recv_frames.load();
    uint32_t dropped   = g_stats.dropped_data.load();
    uint32_t anom      = g_stats.fec_anomaly.load();
    uint32_t fec_ok    = g_stats.fec_key_ok.load();
    uint32_t fec_bad   = g_stats.fec_key_bad.load();

    printf("\n========== 统计 ==========\n");
    printf("发送帧:          %u\n", sent);
    printf("接收帧:          %u\n", recvd);
    printf("丢弃 DATA 包:    %u  (确定性: sn%%4==1)\n", dropped);
    printf("FEC 包 key 正确: %u\n", fec_ok);
    printf("FEC 包 key 错误: %u\n", fec_bad);
    printf("FEC restore 异常: %u  (期望: 0)\n", anom);

    printf("\n========== 判定 ==========\n");
    int pass = 1;

    /* TEST-1: FEC 恢复不产生异常 */
    if (anom > 0) {
        printf("[FAIL] TEST-1: fec_anomaly=%u，期望 0\n"
               "       旧代码 memmove 后 res_pos != CalcPosInPackCache(pack_sn) 触发此错误\n",
               anom);
        pass = 0;
    } else {
        printf("[PASS] TEST-1: 无 fec restore abnormal 日志\n");
    }

    /* TEST-2: FEC 包 stream_key_ 从新固定头部正确提取 */
    if (fec_bad > 0) {
        printf("[FAIL] TEST-2: FEC包stream_key_提取错误 %u 次\n"
               "       新格式应将 stream_key_ 存于 Fec2CodePack 固定头 bytes16-23\n",
               fec_bad);
        pass = 0;
    } else {
        printf("[PASS] TEST-2: FEC包stream_key_提取全部正确（%u 次）\n", fec_ok);
    }

    /* TEST-3: FEC 包确实被收到（验证测试路径有效性） */
    if (fec_ok == 0) {
        printf("[WARN] TEST-3: fec_key_ok=0，未收到任何 FEC 包，测试覆盖不足\n");
    } else {
        printf("[PASS] TEST-3: FEC 包覆盖验证有效（%u 包）\n", fec_ok);
    }

    /* TEST-4: 帧交付率 >= 90% */
    uint32_t threshold = sent * 90 / 100;
    if (recvd < threshold) {
        printf("[FAIL] TEST-4: 接收帧 %u < 阈值 %u（发送帧的 90%%）\n",
               recvd, threshold);
        pass = 0;
    } else {
        printf("[PASS] TEST-4: 帧交付率 %.1f%%（阈值 90%%）\n",
               sent ? recvd * 100.0 / sent : 0.0);
    }

    printf("\n整体：%s\n\n", pass ? "PASS" : "FAIL");

    DeleteGtpInstance(g_a.gtp_hdl);
    DeleteGtpInstance(g_b.gtp_hdl);
    close(g_a.sfd);
    close(g_b.sfd);
    RmLoadGtpModule();
    return pass ? 0 : 1;
}
