/*
 * multi_session_test.cpp — 多会话 FEC + stream_key_ 集成测试（单进程 loopback）
 *
 * 测试目标：
 *   1. 5 个并发逻辑会话（enable_key_=1，stream_key_=0x1001..0x1005）
 *   2. server 端用 GtpCheckPacketInvalid 提取 stream_key_，实现正确 session 路由
 *   3. echo 帧带嵌入 stream_key_，client 端验证一致性
 *   4. 全 DEBUG 日志：任何 [ERROR] 行即表示有问题
 *   5. 10s 自动退出并打印统计（PASS/FAIL）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include <stdarg.h>

#include "bitlinker.h"

/* ── 配置 ── */
#define SERVER_PORT      9102          /* 避免与其他测试冲突 */
#define NUM_SESSIONS     5
#define SEND_INTERVAL_US 100000        /* 每个 session 100ms 发一帧 */
#define TIMER_PERIOD_US  10000         /* 10ms */
#define TEST_DURATION_S  10
#define RECV_BUF_SZ      65536
#define BASE_STREAM_KEY  0x1001ULL
#define FRAME_MAGIC      0xBEEFCAFEU

/* ── 帧格式 ── */
typedef struct {
    uint32_t magic;
    uint32_t sess_idx;     /* 0..NUM_SESSIONS-1 */
    uint32_t seq;
    uint64_t stream_key;   /* 嵌入发送时的 stream_key，用于接收端验证 */
    uint64_t send_ts_us;
    char     info[32];
} TestFrame;

/* ── 每逻辑 session 状态 ── */
typedef struct {
    uint64_t stream_key;
    uint32_t send_seq;
    uint32_t echo_seq;
    uint64_t next_send_ts;
} SessState;

/* ── 全局 ── */
static SessState    g_sess[NUM_SESSIONS];
static int          g_sfd_svr = -1;
static int          g_sfd_cli = -1;
static GtpHandler_p g_gtp_svr = INVALID_GTP_HANDLER;
static GtpHandler_p g_gtp_cli = INVALID_GTP_HANDLER;

static struct sockaddr_in g_svr_sa;     /* 127.0.0.1:SERVER_PORT */
static struct sockaddr_in g_cli_sa;     /* 127.0.0.1:<OS port>   */

static uint32_t g_error_cnt    = 0;
static uint32_t g_svr_frames   = 0;
static uint32_t g_cli_frames   = 0;
static uint32_t g_key_mismatch = 0;

/* ── 时间工具 ── */
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* ── 日志回调 ── */
static void CliLog(uint32_t lv, const char *fmt, ...) {
    static const char *t[] = {"EMERG","ALERT","CRIT","ERROR","WARN","NOTICE","INFO","DEBUG"};
    if (lv == kGtpLogLevelError) g_error_cnt++;
    printf("[CLI][%s] ", (lv < 8) ? t[lv] : "?");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
static uint32_t CliLogLv(void) { return 7; }

static void SvrLog(uint32_t lv, const char *fmt, ...) {
    static const char *t[] = {"EMERG","ALERT","CRIT","ERROR","WARN","NOTICE","INFO","DEBUG"};
    if (lv == kGtpLogLevelError) g_error_cnt++;
    printf("[SVR][%s] ", (lv < 8) ? t[lv] : "?");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
static uint32_t SvrLogLv(void) { return 7; }

/* ── server send callback ── */
static uint32_t SvrSendCb(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    ssize_t r = sendto(g_sfd_svr, pack, size, 0,
                       (struct sockaddr*)addr->sock_addr_, addr->sock_addr_len_);
    if (r != (ssize_t)size)
        fprintf(stderr, "[SVR] sendto err: %s\n", strerror(errno));
    return GTP_OK;
}

/* ── server receive callback: echo back with same stream_key ── */
static uint32_t SvrRecvCb(GtpHandler_p hdl, void *frame, uint32_t size, GtpAddr *tran_addr) {
    g_svr_frames++;
    if (size < sizeof(TestFrame)) {
        fprintf(stderr, "[SVR] frame too small: %u bytes\n", size);
        return GTP_OK;
    }
    TestFrame *tf = (TestFrame*)frame;
    printf("[SVR] recv sess=%u seq=%u key=0x%llx one_way=%llu us\n",
           tf->sess_idx, tf->seq,
           (unsigned long long)tf->stream_key,
           (unsigned long long)(now_us() - tf->send_ts_us));

    /* echo back：使用帧中嵌入的 stream_key */
    uint32_t mem_len = 0;
    GtpAddr *snd = NULL;
    uint8_t *mem = GtpMallocPackMem(hdl, NULL, 0, &mem_len, (void**)&snd, size);
    if (!mem) { fprintf(stderr, "[SVR] GtpMallocPackMem failed\n"); return GTP_OK; }
    memcpy(mem, frame, size);

    snd->context_      = NULL;
    snd->sfd_          = (uint32_t)g_sfd_svr;
    snd->stream_type_  = kReliableStream;
    snd->enable_key_   = 1;
    snd->stream_key_   = tf->stream_key;   /* 与 client 发送时对应的 session */
    snd->self_addr_len_= 0;
    snd->sock_addr_len_= tran_addr->sock_addr_len_;
    memcpy(snd->sock_addr_, tran_addr->sock_addr_, tran_addr->sock_addr_len_);

    uint32_t ret = GtpFrameSend(hdl, mem, size, snd, 0, 0);
    if (GTP_OK != ret) {
        fprintf(stderr, "[SVR] GtpFrameSend err 0x%x %s\n", ret, LastGtpErrorInfo());
        GtpFreePackMem(hdl, mem);
    }
    return GTP_OK;
}

/* ── client send callback ── */
static uint32_t CliSendCb(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl;
    ssize_t r = sendto(g_sfd_cli, pack, size, 0,
                       (struct sockaddr*)addr->sock_addr_, addr->sock_addr_len_);
    if (r != (ssize_t)size)
        fprintf(stderr, "[CLI] sendto err: %s\n", strerror(errno));
    return GTP_OK;
}

/* ── client receive callback: verify echo ── */
static uint32_t CliRecvCb(GtpHandler_p hdl, void *frame, uint32_t size, GtpAddr *tran_addr) {
    (void)hdl; (void)tran_addr;
    g_cli_frames++;
    if (size < sizeof(TestFrame)) {
        fprintf(stderr, "[CLI] echo too small: %u\n", size);
        return GTP_OK;
    }
    TestFrame *tf = (TestFrame*)frame;
    if (tf->magic != FRAME_MAGIC) {
        fprintf(stderr, "[CLI] magic mismatch: 0x%x\n", tf->magic);
        g_error_cnt++;
        return GTP_OK;
    }
    if (tf->sess_idx >= NUM_SESSIONS) {
        fprintf(stderr, "[CLI] sess_idx %u out of range\n", tf->sess_idx);
        g_error_cnt++;
        return GTP_OK;
    }
    uint64_t expected = BASE_STREAM_KEY + tf->sess_idx;
    if (tf->stream_key != expected) {
        fprintf(stderr, "[CLI] stream_key MISMATCH sess=%u expected=0x%llx got=0x%llx\n",
                tf->sess_idx,
                (unsigned long long)expected,
                (unsigned long long)tf->stream_key);
        g_key_mismatch++;
        g_error_cnt++;
    }
    uint64_t rtt = now_us() - tf->send_ts_us;
    printf("[CLI] echo  sess=%u seq=%u key=0x%llx rtt=%llu us %s\n",
           tf->sess_idx, tf->seq,
           (unsigned long long)tf->stream_key,
           (unsigned long long)rtt,
           (tf->stream_key == expected) ? "OK" : "KEY_ERR");
    g_sess[tf->sess_idx].echo_seq = tf->seq;
    return GTP_OK;
}

/* ── client 发一帧 ── */
static void SendFrame(uint32_t i) {
    SessState *s = &g_sess[i];
    TestFrame tf;
    memset(&tf, 0, sizeof(tf));
    tf.magic      = FRAME_MAGIC;
    tf.sess_idx   = i;
    tf.seq        = ++s->send_seq;
    tf.stream_key = s->stream_key;
    tf.send_ts_us = now_us();
    snprintf(tf.info, sizeof(tf.info), "s%u#%u", i, tf.seq);

    uint32_t mem_len = 0;
    GtpAddr *addr = NULL;
    uint8_t *mem = GtpMallocPackMem(g_gtp_cli, NULL, 0, &mem_len, (void**)&addr, sizeof(tf));
    if (!mem) { fprintf(stderr, "[CLI] GtpMallocPackMem failed sess=%u\n", i); return; }
    memcpy(mem, &tf, sizeof(tf));

    addr->context_      = NULL;
    addr->sfd_          = (uint32_t)g_sfd_cli;
    addr->stream_type_  = kReliableStream;
    addr->enable_key_   = 1;
    addr->stream_key_   = s->stream_key;
    addr->self_addr_len_= 0;
    addr->sock_addr_len_= (uint32_t)sizeof(g_svr_sa);
    memcpy(addr->sock_addr_, &g_svr_sa, sizeof(g_svr_sa));

    uint32_t ret = GtpFrameSend(g_gtp_cli, mem, sizeof(tf), addr, 0, 0);
    if (GTP_OK != ret) {
        fprintf(stderr, "[CLI] GtpFrameSend sess=%u err 0x%x %s\n", i, ret, LastGtpErrorInfo());
        GtpFreePackMem(g_gtp_cli, mem);
    } else {
        printf("[CLI] sent  sess=%u seq=%u key=0x%llx\n",
               i, tf.seq, (unsigned long long)s->stream_key);
    }
}

/* ── 从 socket 读包并送入 gtp ── */
static void DrainSocket(int sfd, GtpHandler_p gtp, int is_server) {
    static uint8_t raw[RECV_BUF_SZ];
    for (;;) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof(peer);
        ssize_t n = recvfrom(sfd, raw, sizeof(raw), MSG_DONTWAIT,
                             (struct sockaddr*)&peer, &plen);
        if (n <= 0) break;

        uint32_t mem_len = 0;
        GtpAddr *addr = NULL;
        uint8_t *mem = GtpMallocPackMem(gtp, NULL, 0, &mem_len, (void**)&addr, (uint32_t)n);
        if (!mem) {
            fprintf(stderr, "[%s] GtpMallocPackMem failed\n", is_server ? "SVR" : "CLI");
            continue;
        }
        memcpy(mem, raw, n);

        /* server 需要从包头提取 stream_key_ 供 session 路由 */
        uint64_t extracted_key = 0;
        if (is_server) {
            GtpCheckPacketInvalid(mem, (uint32_t)n, &extracted_key);
        }

        addr->context_      = NULL;
        addr->sfd_          = (uint32_t)sfd;
        addr->stream_type_  = kReliableStream;
        addr->enable_key_   = (is_server && extracted_key != 0) ? 1 : 0;
        addr->stream_key_   = extracted_key;
        addr->self_addr_len_= 0;
        addr->sock_addr_len_= (uint32_t)plen;
        memcpy(addr->sock_addr_, &peer, (uint32_t)plen);

        uint32_t ret = GtpPacketReceive(gtp, mem, (uint32_t)n, addr);
        if (GTP_OK != ret) {
            fprintf(stderr, "[%s] GtpPacketReceive err 0x%x\n",
                    is_server ? "SVR" : "CLI", ret);
            GtpFreePackMem(gtp, mem);
        }
    }
}

/* ── main ── */
int main(void) {
    printf("===== multi_session_test =====\n");
    printf("sessions=%d  stream_key=0x%llx..0x%llx  duration=%ds\n",
           NUM_SESSIONS,
           (unsigned long long)BASE_STREAM_KEY,
           (unsigned long long)(BASE_STREAM_KEY + NUM_SESSIONS - 1),
           TEST_DURATION_S);

    for (int i = 0; i < NUM_SESSIONS; i++) {
        g_sess[i].stream_key   = BASE_STREAM_KEY + (uint64_t)i;
        g_sess[i].send_seq     = 0;
        g_sess[i].echo_seq     = 0;
        /* 错开首包 20ms，避免同一时刻集中发包 */
        g_sess[i].next_send_ts = now_us() + (uint64_t)i * 20000;
    }

    if (GTP_OK != InsLoadGtpModule()) {
        fprintf(stderr, "InsLoadGtpModule failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    /* server socket */
    g_sfd_svr = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&g_svr_sa, 0, sizeof(g_svr_sa));
    g_svr_sa.sin_family      = AF_INET;
    g_svr_sa.sin_port        = htons(SERVER_PORT);
    g_svr_sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    { int reuse = 1; setsockopt(g_sfd_svr, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)); }
    if (bind(g_sfd_svr, (struct sockaddr*)&g_svr_sa, sizeof(g_svr_sa)) < 0) {
        perror("bind server"); return 1;
    }

    /* client socket */
    g_sfd_cli = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in cli_bind;
    memset(&cli_bind, 0, sizeof(cli_bind));
    cli_bind.sin_family      = AF_INET;
    cli_bind.sin_port        = 0;
    cli_bind.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(g_sfd_cli, (struct sockaddr*)&cli_bind, sizeof(cli_bind)) < 0) {
        perror("bind client"); return 1;
    }
    socklen_t cli_sa_len = sizeof(g_cli_sa);
    getsockname(g_sfd_cli, (struct sockaddr*)&g_cli_sa, &cli_sa_len);
    printf("client port: %u   server port: %u\n",
           ntohs(g_cli_sa.sin_port), SERVER_PORT);

    /* gtp instances */
    MemPoolConfig mem_cfg;
    memset(&mem_cfg, 0, sizeof(mem_cfg));
    mem_cfg.m_256bytes_num_ = 128;
    mem_cfg.m_512bytes_num_ = 128;
    mem_cfg.m_1k_num_       = 256;
    mem_cfg.m_1_5k_num_     = 256;
    mem_cfg.m_4k_num_       = 64;
    mem_cfg.m_8k_num_       = 32;
    mem_cfg.m_64k_num_      = 8;

    GtpCallBackParam svr_cb, cli_cb;
    memset(&svr_cb, 0, sizeof(svr_cb));
    svr_cb.send_pack_cb_     = SvrSendCb;
    svr_cb.receive_frame_cb_ = SvrRecvCb;
    svr_cb.write_log_cb_     = SvrLog;
    svr_cb.cur_log_level_cb_ = SvrLogLv;

    memset(&cli_cb, 0, sizeof(cli_cb));
    cli_cb.send_pack_cb_     = CliSendCb;
    cli_cb.receive_frame_cb_ = CliRecvCb;
    cli_cb.write_log_cb_     = CliLog;
    cli_cb.cur_log_level_cb_ = CliLogLv;

    g_gtp_svr = CreateGtpInstance(1, 30000000, &svr_cb, &mem_cfg);
    if (INVALID_GTP_HANDLER == g_gtp_svr) {
        fprintf(stderr, "CreateGtpInstance svr failed: %s\n", LastGtpErrorInfo());
        return 1;
    }
    g_gtp_cli = CreateGtpInstance(2, 30000000, &cli_cb, &mem_cfg);
    if (INVALID_GTP_HANDLER == g_gtp_cli) {
        fprintf(stderr, "CreateGtpInstance cli failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    uint8_t ver[256] = {0};
    GetGtpVersion(ver);
    printf("goodtp version: %s\n", (char*)ver);
    printf("--- test running ---\n");

    uint64_t end_ts     = now_us() + (uint64_t)TEST_DURATION_S * 1000000ULL;
    uint64_t next_timer = now_us();

    while (now_us() < end_ts) {
        DrainSocket(g_sfd_svr, g_gtp_svr, 1);
        DrainSocket(g_sfd_cli, g_gtp_cli, 0);

        uint64_t cur = now_us();
        if (cur >= next_timer) {
            PeriodGtpTimer(g_gtp_svr);
            PeriodGtpTimer(g_gtp_cli);
            next_timer = cur + TIMER_PERIOD_US;
        }

        cur = now_us();
        for (int i = 0; i < NUM_SESSIONS; i++) {
            if (cur >= g_sess[i].next_send_ts) {
                SendFrame((uint32_t)i);
                g_sess[i].next_send_ts = cur + SEND_INTERVAL_US;
            }
        }

        usleep(1000);
    }

    /* 等待在途包到达 */
    usleep(300000);
    DrainSocket(g_sfd_svr, g_gtp_svr, 1);
    DrainSocket(g_sfd_cli, g_gtp_cli, 0);
    PeriodGtpTimer(g_gtp_svr);
    PeriodGtpTimer(g_gtp_cli);
    DrainSocket(g_sfd_svr, g_gtp_svr, 1);
    DrainSocket(g_sfd_cli, g_gtp_cli, 0);

    /* 打印各 session 的 FEC 状态 */
    {
        uint8_t info[4096] = {0};
        ShowTotalLinker(g_gtp_svr, NULL, info, sizeof(info));
        printf("\n[SVR linkers]\n%s\n", (char*)info);
        memset(info, 0, sizeof(info));
        ShowTotalLinker(g_gtp_cli, NULL, info, sizeof(info));
        printf("[CLI linkers]\n%s\n", (char*)info);
    }

    printf("\n========== TEST SUMMARY ==========\n");
    printf("server frames received : %u\n", g_svr_frames);
    printf("client echoes received : %u\n", g_cli_frames);
    printf("stream_key mismatches  : %u\n", g_key_mismatch);
    printf("ERROR log count        : %u\n", g_error_cnt);
    printf("\nper-session:\n");
    for (int i = 0; i < NUM_SESSIONS; i++) {
        SessState *s = &g_sess[i];
        printf("  [%d] key=0x%llx  sent=%-3u  echoed=%-3u  %s\n",
               i, (unsigned long long)s->stream_key, s->send_seq, s->echo_seq,
               (s->send_seq > 0 && s->echo_seq == 0) ? "WARN:no_echo" : "");
    }

    int pass = (g_error_cnt == 0 && g_key_mismatch == 0);
    printf("\n[%s]\n", pass ? "PASS" : "FAIL");

    DeleteGtpInstance(g_gtp_cli);
    DeleteGtpInstance(g_gtp_svr);
    close(g_sfd_cli);
    close(g_sfd_svr);
    RmLoadGtpModule();
    return pass ? 0 : 1;
}
