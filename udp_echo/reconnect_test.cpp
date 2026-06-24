/*
 * reconnect_test.cpp
 *
 * Regression tests for two production bugs fixed in commit 510015c:
 *
 * TEST 1 — Session rebuild after DelGtpLinker (Bug 1, goodtp_mgr.cpp)
 *   A sends to B with stream_key_. Server calls DelGtpLinker. A continues sending.
 *   Expected: B rebuilds session (no "call GetSession() failed"), B receives new frames.
 *
 * TEST 2 — FEC state flushed on SN jump (Bug 2/3, goodtp_session.cpp)
 *   A sends ~120 frames to B with FEC active (stream_key_ mode). A is deleted and
 *   replaced with A' (fresh GoodTP instance, SN starts from 0). A' sends to B.
 *   Expected: no "fec restore abnormal" or "Unknown fec encode type" errors after reset.
 *
 * Usage:
 *   ./reconnect_test
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

#define LOOPBACK_IP  "127.0.0.1"
#define PORT_T1_A    19020
#define PORT_T1_B    19021
#define PORT_T2_A    19022
#define PORT_T2_B    19023
#define TIMER_US     10000   /* 10 ms */
#define SEND_PPS     50      /* frames/sec */
#define TEST_KEY     0x1234567890ABCDEFULL

typedef struct { uint32_t magic; uint32_t seq; uint64_t ts; } TestFrame;

/* ── global counters (reset between test phases) ── */
static std::atomic<int> g_get_session_failed(0);
static std::atomic<int> g_fec_restore_abnormal(0);
static std::atomic<int> g_unknown_fec_type(0);
static std::atomic<int> g_recv_b(0);
static volatile int     g_running = 0;

/* ── instance context ── */
typedef struct {
    int              sfd;
    GtpHandler_p     hdl;
    struct sockaddr_in peer_addr;
    socklen_t          peer_addr_len;
    struct sockaddr_in self_addr;
} Ctx;

static Ctx g_a, g_b;
static pthread_mutex_t g_la = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lb = PTHREAD_MUTEX_INITIALIZER;

/* ── log callback ── */
static void log_cb(uint32_t level, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (level <= 3) {
        if (strstr(buf, "call GetSession() failed"))  g_get_session_failed.fetch_add(1);
        if (strstr(buf, "fec restore abnormal"))       g_fec_restore_abnormal.fetch_add(1);
        if (strstr(buf, "Unknown fec encode type"))    g_unknown_fec_type.fetch_add(1);
        static const char *lv[] = {"EMRG","ALRT","CRIT","ERR"};
        printf("[gtp-%s] %s", lv[level], buf);
    }
}
static uint32_t log_level_cb(void) { return 3; }

/* ── callbacks for A ── */
static uint32_t send_cb_a(GtpHandler_p h, void *p, uint32_t sz, GtpAddr *addr) {
    (void)h;
    Ctx *c = (Ctx *)addr->context_;
    sendto(c->sfd, p, sz, 0, (struct sockaddr *)&c->peer_addr, c->peer_addr_len);
    return GTP_OK;
}
static uint32_t recv_cb_a(GtpHandler_p h, void *f, uint32_t s, GtpAddr *a) {
    (void)h;(void)f;(void)s;(void)a; return GTP_OK;
}

/* ── callbacks for B ── */
static uint32_t send_cb_b(GtpHandler_p h, void *p, uint32_t sz, GtpAddr *addr) {
    (void)h;
    Ctx *c = (Ctx *)addr->context_;
    sendto(c->sfd, p, sz, 0, (struct sockaddr *)&c->peer_addr, c->peer_addr_len);
    return GTP_OK;
}
static uint32_t recv_cb_b(GtpHandler_p h, void *frame, uint32_t sz, GtpAddr *a) {
    (void)h;(void)a;
    if (sz >= sizeof(TestFrame) && ((TestFrame*)frame)->magic == 0xDEADBEEF)
        g_recv_b.fetch_add(1);
    return GTP_OK;
}

/* ── helpers ── */
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

static int make_socket(uint16_t port, struct sockaddr_in *out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port   = htons(port);
    out->sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)out, sizeof(*out)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    struct timeval tv = {0, 200000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    inet_pton(AF_INET, LOOPBACK_IP, &out->sin_addr);
    return fd;
}

static MemPoolConfig default_pool(void) {
    MemPoolConfig c; memset(&c, 0, sizeof(c));
    c.m_256bytes_num_ = 1024; c.m_512bytes_num_ = 1024;
    c.m_1k_num_ = 512;  c.m_1_5k_num_ = 512;
    c.m_4k_num_ = 128;  c.m_8k_num_   = 64; c.m_64k_num_ = 16;
    return c;
}

/* ── recv thread (processes B's incoming UDP) ── */
typedef struct { Ctx *ctx; pthread_mutex_t *lock; } RecvArg;

static void *recv_thread(void *arg) {
    RecvArg *ra = (RecvArg *)arg;
    static __thread uint8_t buf[65536];
    while (g_running) {
        struct sockaddr_storage peer; socklen_t plen = sizeof(peer);
        ssize_t n = recvfrom(ra->ctx->sfd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&peer, &plen);
        if (n <= 0) continue;
        pthread_mutex_lock(ra->lock);
        uint32_t ml = 0; GtpAddr *ta = NULL;
        uint8_t *pm = GtpMallocPackMem(ra->ctx->hdl, NULL, 0, &ml,
                                        (void **)&ta, (uint32_t)n);
        if (!pm) { pthread_mutex_unlock(ra->lock); continue; }
        memcpy(pm, buf, n);
        ta->context_       = ra->ctx;
        ta->sfd_           = (uint32_t)ra->ctx->sfd;
        ta->stream_type_   = kRealTimeStream;
        ta->self_addr_len_ = (uint32_t)sizeof(ra->ctx->self_addr);
        memcpy(ta->self_addr_, &ra->ctx->self_addr, sizeof(ra->ctx->self_addr));
        ta->sock_addr_len_ = (uint32_t)plen;
        memcpy(ta->sock_addr_, &peer, plen);
        /* extract stream_key_ from packet (mirrors socks5udpf's recv path) */
        uint64_t sk = 0;
        GtpCheckPacketInvalid(pm, (uint32_t)n, &sk);
        ta->enable_key_ = (sk != 0) ? 1 : 0;
        ta->stream_key_ = sk;
        uint32_t ret = GtpPacketReceive(ra->ctx->hdl, pm, (uint32_t)n, ta);
        if (GTP_OK != ret) GtpFreePackMem(ra->ctx->hdl, pm);
        pthread_mutex_unlock(ra->lock);
    }
    return NULL;
}

/* ── timer thread ── */
typedef struct { GtpHandler_p ha, hb; pthread_mutex_t *la, *lb; } TimerArg;

static void *timer_thread(void *arg) {
    TimerArg *ta = (TimerArg *)arg;
    while (g_running) {
        usleep(TIMER_US);
        pthread_mutex_lock(ta->la); PeriodGtpTimer(ta->ha); pthread_mutex_unlock(ta->la);
        pthread_mutex_lock(ta->lb); PeriodGtpTimer(ta->hb); pthread_mutex_unlock(ta->lb);
    }
    return NULL;
}

/* ── send n frames from A to B at SEND_PPS ── */
static void send_frames(int n) {
    static uint32_t seq = 0;
    uint64_t interval = 1000000ULL / SEND_PPS;
    uint64_t next = now_us();
    for (int i = 0; i < n; ) {
        if (now_us() >= next) {
            pthread_mutex_lock(&g_la);
            uint32_t ml = 0; GtpAddr *ta = NULL;
            uint8_t *pm = GtpMallocPackMem(g_a.hdl, NULL, 0, &ml,
                                            (void **)&ta, sizeof(TestFrame));
            if (pm) {
                TestFrame f = {0xDEADBEEF, ++seq, now_us()};
                memcpy(pm, &f, sizeof(f));
                ta->context_       = &g_a;
                ta->sfd_           = (uint32_t)g_a.sfd;
                ta->stream_type_   = kRealTimeStream;
                ta->enable_key_    = 1;
                ta->stream_key_    = TEST_KEY;
                ta->self_addr_len_ = (uint32_t)sizeof(g_a.self_addr);
                memcpy(ta->self_addr_, &g_a.self_addr, sizeof(g_a.self_addr));
                ta->sock_addr_len_ = (uint32_t)sizeof(g_a.peer_addr);
                memcpy(ta->sock_addr_, &g_a.peer_addr, sizeof(g_a.peer_addr));
                uint32_t ret = GtpFrameSend(g_a.hdl, pm, sizeof(TestFrame), ta, 0, 0);
                if (GTP_OK != ret) GtpFreePackMem(g_a.hdl, pm);
                i++;
            }
            pthread_mutex_unlock(&g_la);
            next += interval;
        }
        usleep(500);
    }
}

/* ── wait up to timeout_ms for B to receive at least want frames ── */
static int wait_recv(int want, int timeout_ms) {
    uint64_t deadline = now_us() + (uint64_t)timeout_ms * 1000ULL;
    while (now_us() < deadline) {
        if (g_recv_b.load() >= want) return 1;
        usleep(10000);
    }
    return g_recv_b.load() >= want;
}

/* ════════════════════════════════════════════════════════════════
 * TEST 1: DelGtpLinker + reconnect → no GetSession failure
 * ════════════════════════════════════════════════════════════════ */
static int test_reconnect_after_del(void) {
    printf("\n=== TEST 1: DelGtpLinker + reconnect ===\n");
    int ok = 1;

    /* sockets */
    g_a.sfd = make_socket(PORT_T1_A, &g_a.self_addr);
    g_b.sfd = make_socket(PORT_T1_B, &g_b.self_addr);
    if (g_a.sfd < 0 || g_b.sfd < 0) return 0;

    /* peer addresses */
    g_a.peer_addr.sin_family = AF_INET;
    g_a.peer_addr.sin_port   = htons(PORT_T1_B);
    inet_pton(AF_INET, LOOPBACK_IP, &g_a.peer_addr.sin_addr);
    g_a.peer_addr_len = sizeof(g_a.peer_addr);

    g_b.peer_addr.sin_family = AF_INET;
    g_b.peer_addr.sin_port   = htons(PORT_T1_A);
    inet_pton(AF_INET, LOOPBACK_IP, &g_b.peer_addr.sin_addr);
    g_b.peer_addr_len = sizeof(g_b.peer_addr);

    /* GoodTP instances */
    MemPoolConfig mc = default_pool();
    GtpCallBackParam cba = {send_cb_a, recv_cb_a, NULL, log_cb, log_level_cb, NULL};
    GtpCallBackParam cbb = {send_cb_b, recv_cb_b, NULL, log_cb, log_level_cb, NULL};
    g_a.hdl = CreateGtpInstance(10, 10000000, &cba, &mc);
    g_b.hdl = CreateGtpInstance(11, 10000000, &cbb, &mc);
    if (INVALID_GTP_HANDLER == g_a.hdl || INVALID_GTP_HANDLER == g_b.hdl) {
        fprintf(stderr, "CreateGtpInstance failed: %s\n", LastGtpErrorInfo());
        return 0;
    }

    /* threads */
    RecvArg rb = {&g_b, &g_lb};
    TimerArg ta = {g_a.hdl, g_b.hdl, &g_la, &g_lb};
    pthread_t tid_rb, tid_timer;
    g_running = 1;
    pthread_create(&tid_rb,    NULL, recv_thread, &rb);
    pthread_create(&tid_timer, NULL, timer_thread, &ta);

    /* phase 1: establish session, send 40 frames */
    printf("  Phase 1: send 40 frames A→B ...\n");
    g_recv_b.store(0);
    g_get_session_failed.store(0);
    send_frames(40);
    wait_recv(10, 3000);
    printf("  Phase 1 done: B received %d frames\n", g_recv_b.load());

    /* call DelGtpLinker on B (simulates socks5udpf del_fec_session) */
    printf("  Calling DelGtpLinker on B with stream_key_=0x%llX ...\n",
           (unsigned long long)TEST_KEY);
    GtpAddr del_addr;
    memset(&del_addr, 0, sizeof(del_addr));
    del_addr.enable_key_ = 1;
    del_addr.stream_key_ = TEST_KEY;
    del_addr.sfd_        = (uint32_t)g_b.sfd;
    pthread_mutex_lock(&g_lb);
    DelGtpLinker(g_b.hdl, &del_addr);
    pthread_mutex_unlock(&g_lb);

    /* phase 2: A continues sending after Del; B must rebuild session */
    printf("  Phase 2: send 40 more frames A→B (after Del) ...\n");
    g_recv_b.store(0);
    g_get_session_failed.store(0);
    send_frames(40);
    wait_recv(10, 3000);
    int recv2 = g_recv_b.load();
    int failed = g_get_session_failed.load();
    printf("  Phase 2 done: B received %d frames, GetSession failures=%d\n", recv2, failed);

    g_running = 0;
    pthread_join(tid_rb, NULL);
    pthread_join(tid_timer, NULL);

    DeleteGtpInstance(g_a.hdl);
    DeleteGtpInstance(g_b.hdl);
    close(g_a.sfd);
    close(g_b.sfd);

    /* verdict */
    if (failed > 0) {
        printf("  [FAIL] GetSession failed %d times after Del+reconnect\n", failed);
        ok = 0;
    } else {
        printf("  [PASS] No GetSession failures after DelGtpLinker\n");
    }
    if (recv2 == 0) {
        printf("  [FAIL] B received 0 frames in Phase 2 (session not rebuilt)\n");
        ok = 0;
    } else {
        printf("  [PASS] B received %d frames after reconnect\n", recv2);
    }
    return ok;
}

/* ════════════════════════════════════════════════════════════════
 * TEST 2: SN jump (simulated client restart) → FEC state flushed
 * ════════════════════════════════════════════════════════════════ */
static int test_fec_clean_after_sn_jump(void) {
    printf("\n=== TEST 2: SN jump → FEC state clean ===\n");
    int ok = 1;

    /* sockets */
    g_a.sfd = make_socket(PORT_T2_A, &g_a.self_addr);
    g_b.sfd = make_socket(PORT_T2_B, &g_b.self_addr);
    if (g_a.sfd < 0 || g_b.sfd < 0) return 0;

    g_a.peer_addr.sin_family = AF_INET;
    g_a.peer_addr.sin_port   = htons(PORT_T2_B);
    inet_pton(AF_INET, LOOPBACK_IP, &g_a.peer_addr.sin_addr);
    g_a.peer_addr_len = sizeof(g_a.peer_addr);

    g_b.peer_addr.sin_family = AF_INET;
    g_b.peer_addr.sin_port   = htons(PORT_T2_A);
    inet_pton(AF_INET, LOOPBACK_IP, &g_b.peer_addr.sin_addr);
    g_b.peer_addr_len = sizeof(g_b.peer_addr);

    MemPoolConfig mc = default_pool();
    GtpCallBackParam cba = {send_cb_a, recv_cb_a, NULL, log_cb, log_level_cb, NULL};
    GtpCallBackParam cbb = {send_cb_b, recv_cb_b, NULL, log_cb, log_level_cb, NULL};
    g_b.hdl = CreateGtpInstance(21, 10000000, &cbb, &mc);
    if (INVALID_GTP_HANDLER == g_b.hdl) {
        fprintf(stderr, "CreateGtpInstance(B) failed\n"); return 0;
    }

    /* ── phase 1: A (original) sends 120 frames, building FEC matrices on B ── */
    g_a.hdl = CreateGtpInstance(20, 10000000, &cba, &mc);
    if (INVALID_GTP_HANDLER == g_a.hdl) {
        fprintf(stderr, "CreateGtpInstance(A) failed\n"); return 0;
    }

    RecvArg rb = {&g_b, &g_lb};
    TimerArg ta_arg = {g_a.hdl, g_b.hdl, &g_la, &g_lb};
    pthread_t tid_rb, tid_timer;
    g_running = 1;
    pthread_create(&tid_rb,    NULL, recv_thread, &rb);
    pthread_create(&tid_timer, NULL, timer_thread, &ta_arg);

    printf("  Phase 1: A (SN≈0→119) sends 120 frames to B (FEC book4, no loss) ...\n");
    g_recv_b.store(0);
    g_fec_restore_abnormal.store(0);
    g_unknown_fec_type.store(0);
    send_frames(120);
    wait_recv(60, 5000);
    printf("  Phase 1 done: B received %d frames, FEC errors=%d+%d\n",
           g_recv_b.load(), g_fec_restore_abnormal.load(), g_unknown_fec_type.load());

    /* ── stop threads, delete A, create A' (fresh instance, SN restarts at 0) ── */
    g_running = 0;
    pthread_join(tid_rb, NULL);
    pthread_join(tid_timer, NULL);

    pthread_mutex_lock(&g_la);
    DeleteGtpInstance(g_a.hdl);
    g_a.hdl = INVALID_GTP_HANDLER;
    pthread_mutex_unlock(&g_la);

    /* A' is a new GoodTP instance on the same socket/address */
    g_a.hdl = CreateGtpInstance(22, 10000000, &cba, &mc);
    if (INVALID_GTP_HANDLER == g_a.hdl) {
        fprintf(stderr, "CreateGtpInstance(A') failed\n");
        DeleteGtpInstance(g_b.hdl);
        close(g_a.sfd); close(g_b.sfd);
        return 0;
    }

    /* ── phase 2: A' (SN starts at 0) sends to B → triggers ResetSession on B ── */
    /* Reset counters: only errors from this phase count */
    g_fec_restore_abnormal.store(0);
    g_unknown_fec_type.store(0);
    g_recv_b.store(0);

    TimerArg ta_arg2 = {g_a.hdl, g_b.hdl, &g_la, &g_lb};
    g_running = 1;
    pthread_create(&tid_rb,    NULL, recv_thread, &rb);
    pthread_create(&tid_timer, NULL, timer_thread, &ta_arg2);

    printf("  Phase 2: A' (SN starts at 0) sends 80 frames to B ...\n");
    send_frames(80);
    wait_recv(20, 5000);
    int recv2     = g_recv_b.load();
    int fec_abn   = g_fec_restore_abnormal.load();
    int fec_unk   = g_unknown_fec_type.load();
    printf("  Phase 2 done: B received %d frames, fec_restore_abnormal=%d, "
           "unknown_fec_type=%d\n", recv2, fec_abn, fec_unk);

    g_running = 0;
    pthread_join(tid_rb, NULL);
    pthread_join(tid_timer, NULL);

    DeleteGtpInstance(g_a.hdl);
    DeleteGtpInstance(g_b.hdl);
    close(g_a.sfd);
    close(g_b.sfd);

    /* verdict */
    if (fec_abn > 0) {
        printf("  [FAIL] fec_restore_abnormal=%d after SN jump (FEC state not flushed)\n", fec_abn);
        ok = 0;
    } else {
        printf("  [PASS] No fec_restore_abnormal after SN jump\n");
    }
    if (fec_unk > 0) {
        printf("  [FAIL] Unknown fec encode type appeared %d times after SN jump\n", fec_unk);
        ok = 0;
    } else {
        printf("  [PASS] No unknown FEC encode type errors after SN jump\n");
    }
    if (recv2 == 0) {
        printf("  [WARN] B received 0 frames from A' (ResetSession may not have allowed delivery)\n");
    } else {
        printf("  [PASS] B received %d frames from A' after reset\n", recv2);
    }
    return ok;
}

/* ════════════════════════════════════════════════════════════════
 * main
 * ════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("=== reconnect_test ===\n");
    printf("Verifying Bug 1 (GetSession after Del) and Bug 2/3 (FEC flush on SN jump)\n");

    if (GTP_OK != InsLoadGtpModule()) {
        fprintf(stderr, "InsLoadGtpModule failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    int p1 = test_reconnect_after_del();
    int p2 = test_fec_clean_after_sn_jump();

    RmLoadGtpModule();

    printf("\n========== 汇总 ==========\n");
    printf("TEST 1 (DelGtpLinker+reconnect): %s\n", p1 ? "PASS" : "FAIL");
    printf("TEST 2 (SN jump FEC flush):      %s\n", p2 ? "PASS" : "FAIL");
    printf("整体: %s\n", (p1 && p2) ? "PASS" : "FAIL");

    return (p1 && p2) ? 0 : 1;
}
