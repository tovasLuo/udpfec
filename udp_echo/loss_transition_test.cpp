/*
 * loss_transition_test.cpp — FEC book 自适应切换的滞回行为观测
 *
 * 背景：GtpSession::CalcGameFecPolicy() 对 10-25% 丢包这一档要求
 * elevated_loss_streak_ >= 2（即连续两个"整秒丢包>=10%"）才会从 book5
 * 升级到 book4；这个测试用一个随时间变化的丢包率日程（9% -> 11% -> 19% -> 4%），
 * 逐秒记录：实际注入丢包率、库内部测得的 max_s_loss、当前 book_id、
 * 以及该秒发送的帧最终是否完整送达（恢复率）、延迟分布。
 *
 * 单线程主循环（发送/接收/定时器都在一个线程里，避免多实例并发访问的已知
 * 竞争面），跟 game_net_test.cpp 的验证过的写法一致。
 *
 * 构建：cd udp_echo/build && cmake .. && make loss_transition_test
 * 运行：./loss_transition_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include <stdarg.h>
#include <algorithm>
#include <cstddef>
#include <vector>
#include <string>

#include "bitlinker.h"

#define LOOPBACK     "127.0.0.1"
#define PORT_A       19031
#define PORT_B       19032
#define TIMER_US     10000    /* 10 ms，跟 goodtp 定时器周期一致 */
#define POLL_ALG_US  100000   /* 100 ms 轮询一次算法内部状态，捕捉 book 切换的确切时刻 */

static uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

#define GAME_TEST_STREAM_KEY  0x1234567890ABCDEFULL
#define FRAME_MAGIC  0xC0DEBABE
#define PAYLOAD_SZ   188

struct GamePkt {
    uint32_t magic;
    uint32_t frame_id;
    uint32_t pkt_in_frame;
    uint32_t pkts_per_frame;
    uint64_t send_ts_us;
    uint8_t  pad[PAYLOAD_SZ];
    uint32_t crc;
};

static uint32_t pkt_crc(const GamePkt *p) {
    const uint32_t *w = (const uint32_t *)p;
    uint32_t v = 0;
    for (size_t i = 0; i < offsetof(GamePkt, crc) / sizeof(uint32_t); i++) v ^= w[i];
    return v;
}

/* ─────────── 丢包日程：随时间变化的丢包率 ─────────── */
struct LossPhase {
    const char *name;
    int loss_pct;
    int duration_s;
};

static LossPhase g_schedule[] = {
    {"P1 9%丢包(基线,预期book5)",        9, 15},
    {"P2 11%突破(应触发2s滞回,预期升book4)", 11, 15},
    {"P3 19%持续高丢包(streak应已建立)",  19, 15},
    {"P4 回落4%(观察降档反应)",           4, 15},
};
static const int kNumPhases = sizeof(g_schedule) / sizeof(g_schedule[0]);

static uint64_t g_test_start_us = 0;

/* 返回当前(相对测试开始)所在的 phase 下标；超出日程返回最后一个 phase */
static int phase_at(uint64_t elapsed_us) {
    uint64_t acc_s = 0;
    for (int i = 0; i < kNumPhases; i++) {
        acc_s += (uint64_t)g_schedule[i].duration_s;
        if (elapsed_us < acc_s * 1000000ULL) return i;
    }
    return kNumPhases - 1;
}

static int current_loss_pct() {
    return g_schedule[phase_at(now_us() - g_test_start_us)].loss_pct;
}

/* ─────────── 带宽/延迟统计（同 game_net_test.cpp 的口径） ─────────── */
#define GTP_TYPE_OF(buf) (((*(uint32_t*)(buf)) >> 16) & 0x7u)
#define T_DATA 1
#define T_FEC  3
#define T_ACK  2
#define T_NACK 6

struct BwStat {
    uint64_t data_b=0, fec_b=0, ack_b=0, retran_b=0, total_b=0;
    void add(const void *pack, uint32_t sz) {
        uint32_t t = GTP_TYPE_OF(pack);
        total_b += sz;
        if (t == T_DATA) {
            data_b += sz;
            const uint8_t *b = (const uint8_t*)pack;
            if ((b[8] >> 4) & 0x7) retran_b += sz;
        } else if (t == T_FEC)  { fec_b += sz; }
        else if  (t == T_ACK)  { ack_b += sz; }
    }
};
static BwStat g_bw_a, g_bw_b;

/* 按 phase 分桶的延迟样本 */
struct DelaySampler {
    std::vector<uint32_t> v;
    void add(uint32_t us) { v.push_back(us); }
    void p50p99(uint32_t &p50, uint32_t &p99) {
        if (v.empty()) { p50=p99=0; return; }
        std::sort(v.begin(), v.end());
        p50 = v[v.size()*50/100];
        p99 = v[v.size()*99/100];
    }
};
static DelaySampler g_ds_phase[kNumPhases];
static DelaySampler g_ds_all;

/* 按 phase 分桶的帧完整性 */
struct FrameRecord {
    uint32_t recv=0, total=0;
    uint32_t seen_mask=0;
    int phase = -1;
};
static std::vector<FrameRecord> g_frames;
static uint32_t g_frm_sent_by_phase[kNumPhases]  = {0};
static uint32_t g_frm_ok_by_phase[kNumPhases]    = {0};
static uint32_t g_pkt_sent_by_phase[kNumPhases]  = {0};
static uint32_t g_pkt_recv_by_phase[kNumPhases]  = {0};

static uint32_t g_sent_pkt=0, g_recv_pkt=0, g_dropped=0;
static uint32_t g_corruption=0, g_dup_pkt=0;
static uint32_t g_fec_anomaly=0;
static uint64_t g_app_payload = 0;

/* ─────────── book_id 切换时间线 ─────────── */
struct AlgSample {
    uint64_t elapsed_us;
    int      phase;
    uint32_t book_id;
    float    max_s_loss;
    bool     net_bad;
};
static std::vector<AlgSample> g_alg_timeline;
static uint32_t g_last_logged_book = 0xFFFFFFFF;

/* ─────────── socket / GoodTP 回调 ─────────── */
static int g_sfd_a=-1, g_sfd_b=-1;
static struct sockaddr_in g_addr_a, g_addr_b;
static GtpHandler_p g_hdl_a = INVALID_GTP_HANDLER;
static GtpHandler_p g_hdl_b = INVALID_GTP_HANDLER;

static uint32_t SendCbA(GtpHandler_p, void *pack, uint32_t sz, GtpAddr *) {
    g_bw_a.add(pack, sz);
    if ((rand() % 100) < current_loss_pct()) { g_dropped++; return GTP_OK; }
    sendto(g_sfd_a, pack, sz, 0, (struct sockaddr*)&g_addr_b, sizeof(g_addr_b));
    return GTP_OK;
}

static uint32_t RecvCbB(GtpHandler_p, void *frame, uint32_t sz, GtpAddr *) {
    if (sz < sizeof(GamePkt)) return GTP_OK;
    GamePkt *pkt = (GamePkt*)frame;
    if (pkt->magic != FRAME_MAGIC) return GTP_OK;
    if (pkt->crc != pkt_crc(pkt)) g_corruption++;

    g_recv_pkt++;

    uint32_t fid = pkt->frame_id;
    if (fid > 0 && fid <= (uint32_t)g_frames.size()) {
        auto &fr = g_frames[fid-1];
        if (fr.total == 0) fr.total = pkt->pkts_per_frame;
        uint32_t bit = pkt->pkt_in_frame;
        if (bit < 32) {
            if (fr.seen_mask & (1u << bit)) {
                g_dup_pkt++;
            } else {
                fr.seen_mask |= (1u << bit);
                if (++fr.recv == fr.total && fr.phase >= 0) g_frm_ok_by_phase[fr.phase]++;
            }
        }
        if (fr.phase >= 0) g_pkt_recv_by_phase[fr.phase]++;
    }

    uint64_t now = now_us();
    if (now > pkt->send_ts_us) {
        uint32_t delay = (uint32_t)(now - pkt->send_ts_us);
        g_ds_all.add(delay);
        int ph = phase_at(pkt->send_ts_us - g_test_start_us);
        if (ph >= 0 && ph < kNumPhases) g_ds_phase[ph].add(delay);
    }

    return GTP_OK;
}

static uint32_t SendCbB(GtpHandler_p, void *pack, uint32_t sz, GtpAddr *) {
    g_bw_b.add(pack, sz);
    if ((rand() % 100) < current_loss_pct()) { g_dropped++; return GTP_OK; }
    sendto(g_sfd_b, pack, sz, 0, (struct sockaddr*)&g_addr_a, sizeof(g_addr_a));
    return GTP_OK;
}

static uint32_t RecvCbA(GtpHandler_p, void *, uint32_t, GtpAddr *) { return GTP_OK; }

static void LogCb(uint32_t lv, const char *fmt, ...) {
    if (lv > 5) return;
    char buf[512];
    va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
    if (strstr(buf,"fec restore abnormal")) g_fec_anomaly++;
}
static uint32_t LogLvCb() { return 5; }

static void recv_and_deliver(int sfd, GtpHandler_p hdl, const struct sockaddr_in *self_addr) {
    static uint8_t buf[65536];
    for (int i = 0; i < 32; i++) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof(peer);
        ssize_t n = recvfrom(sfd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr*)&peer, &plen);
        if (n <= 0) break;

        uint32_t sz = (uint32_t)n;
        uint32_t msz = 0;
        GtpAddr *ta = nullptr;
        uint8_t *mem = GtpMallocPackMem(hdl, nullptr, 0, &msz, (void**)&ta, sz);
        if (!mem) continue;
        memcpy(mem, buf, sz);
        uint64_t sk = 0;
        GtpCheckPacketInvalid(mem, sz, &sk);
        ta->context_       = nullptr;
        ta->sfd_           = (uint32_t)sfd;
        ta->stream_type_   = kRealTimeStream;
        ta->enable_key_    = 1;
        ta->stream_key_    = sk ? sk : GAME_TEST_STREAM_KEY;
        ta->self_addr_len_ = sizeof(*self_addr);
        memcpy(ta->self_addr_, self_addr, sizeof(*self_addr));
        ta->sock_addr_len_ = (uint32_t)plen;
        memcpy(ta->sock_addr_, &peer, plen);
        if (GTP_OK != GtpPacketReceive(hdl, mem, sz, ta))
            GtpFreePackMem(hdl, mem);
    }
}

static int make_sock(uint16_t port, struct sockaddr_in *out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port   = htons(port);
    out->sin_addr.s_addr = INADDR_ANY;
    bind(fd, (struct sockaddr*)out, sizeof(*out));
    inet_pton(AF_INET, LOOPBACK, &out->sin_addr);
    return fd;
}

/* 从 GetAlgorithmParam 的诊断字符串里解析 book_id / net_quality / max_s_loss */
static void poll_alg_state(uint64_t elapsed_us) {
    static uint8_t alg[8192];
    uint8_t ip_a[64], ip_b[64];
    inet_ntop(AF_INET, &g_addr_a.sin_addr, (char*)ip_a, 64);
    inet_ntop(AF_INET, &g_addr_b.sin_addr, (char*)ip_b, 64);
    if (GTP_OK != GetAlgorithmParam(g_hdl_a, ip_a, ip_b, alg, sizeof(alg))) return;

    AlgSample s{};
    s.elapsed_us = elapsed_us;
    s.phase      = phase_at(elapsed_us);

    const char *p;
    if ((p = strstr((char*)alg, "book_id=")))   sscanf(p, "book_id= %u", &s.book_id);
    if ((p = strstr((char*)alg, "max_s_loss="))) sscanf(p, "max_s_loss= %f", &s.max_s_loss);
    if ((p = strstr((char*)alg, "net_quality="))) {
        char tmp[16]="good";
        sscanf(p, "net_quality= %15s", tmp);
        s.net_bad = (tmp[0]=='b');
    }

    /* 只在 book_id 真正变化时记录一条时间线事件，避免刷屏 */
    if (s.book_id != g_last_logged_book) {
        g_alg_timeline.push_back(s);
        g_last_logged_book = s.book_id;
    }
}

int main() {
    srand(12345);

    int pps          = 150;   /* 大部分游戏的典型 pps */
    int pkts_per_frm = 1;
    int total_dur_s  = 0;
    for (int i = 0; i < kNumPhases; i++) total_dur_s += g_schedule[i].duration_s;

    printf("═══════════════════════════════════════════════════════════════\n");
    printf(" FEC book 自适应切换滞回观测测试\n");
    printf(" pps=%d  pkts_per_frm=%d  总时长=%ds\n", pps, pkts_per_frm, total_dur_s);
    printf(" 丢包日程：\n");
    for (int i = 0; i < kNumPhases; i++) {
        printf("   %s -- %d%% 丢包，持续 %ds\n", g_schedule[i].name, g_schedule[i].loss_pct, g_schedule[i].duration_s);
    }
    printf("═══════════════════════════════════════════════════════════════\n\n");

    if (GTP_OK != InsLoadGtpModule()) {
        fprintf(stderr, "InsLoadGtpModule failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    int frames_per_sec = pps / pkts_per_frm;
    int max_frames = frames_per_sec * total_dur_s + 256;
    g_frames.assign(max_frames, FrameRecord{});

    g_sfd_a = make_sock(PORT_A, &g_addr_a);
    g_sfd_b = make_sock(PORT_B, &g_addr_b);
    struct sockaddr_in peer_b = g_addr_b; peer_b.sin_port = htons(PORT_B);
    struct sockaddr_in peer_a = g_addr_a; peer_a.sin_port = htons(PORT_A);
    memcpy(&g_addr_b, &peer_b, sizeof(peer_b));
    memcpy(&g_addr_a, &peer_a, sizeof(peer_a));

    GtpCallBackParam cb_a = {SendCbA, RecvCbA, nullptr, LogCb, LogLvCb, nullptr};
    GtpCallBackParam cb_b = {SendCbB, RecvCbB, nullptr, LogCb, LogLvCb, nullptr};
    MemPoolConfig mem{};
    mem.m_256bytes_num_ = 4096; mem.m_512bytes_num_ = 4096;
    mem.m_1k_num_ = 2048; mem.m_1_5k_num_ = 2048;
    mem.m_4k_num_ = 512;  mem.m_8k_num_   = 256;
    mem.m_64k_num_ = 32;

    g_hdl_a = CreateGtpInstance(1, 10000000, &cb_a, &mem);
    g_hdl_b = CreateGtpInstance(2, 10000000, &cb_b, &mem);
    if (INVALID_GTP_HANDLER == g_hdl_a || INVALID_GTP_HANDLER == g_hdl_b) {
        fprintf(stderr, "CreateGtpInstance failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    g_test_start_us = now_us();
    uint64_t interval_us  = 1000000ULL * (uint64_t)pkts_per_frm / (uint64_t)pps;
    uint64_t deadline     = g_test_start_us + (uint64_t)total_dur_s * 1000000ULL;
    uint64_t next_send    = g_test_start_us;
    uint64_t next_timer_a = g_test_start_us;
    uint64_t next_timer_b = g_test_start_us;
    uint64_t next_poll    = g_test_start_us;
    uint32_t frame_id     = 0;

    while (now_us() < deadline) {
        uint64_t t = now_us();

        if (t >= next_timer_a) { PeriodGtpTimer(g_hdl_a); next_timer_a = t + TIMER_US; }
        if (t >= next_timer_b) { PeriodGtpTimer(g_hdl_b); next_timer_b = t + TIMER_US; }

        recv_and_deliver(g_sfd_a, g_hdl_a, &g_addr_a);
        recv_and_deliver(g_sfd_b, g_hdl_b, &g_addr_b);

        if (t >= next_poll) {
            poll_alg_state(t - g_test_start_us);
            next_poll = t + POLL_ALG_US;
        }

        if (t >= next_send) {
            frame_id++;
            int ph = phase_at(t - g_test_start_us);
            if (frame_id <= (uint32_t)g_frames.size()) {
                g_frames[frame_id-1].total = (uint32_t)pkts_per_frm;
                g_frames[frame_id-1].phase = ph;
            }
            g_frm_sent_by_phase[ph]++;

            for (int p = 0; p < pkts_per_frm; p++) {
                GamePkt pkt{};
                pkt.magic          = FRAME_MAGIC;
                pkt.frame_id       = frame_id;
                pkt.pkt_in_frame   = (uint32_t)p;
                pkt.pkts_per_frame = (uint32_t)pkts_per_frm;
                pkt.send_ts_us     = now_us();
                for (size_t bi = 0; bi < sizeof(pkt.pad); bi++) pkt.pad[bi] = (uint8_t)(rand() & 0xFF);
                pkt.crc = pkt_crc(&pkt);

                uint32_t msz = 0;
                GtpAddr *ta  = nullptr;
                uint8_t *mem_ptr = GtpMallocPackMem(g_hdl_a, nullptr, 0, &msz, (void**)&ta, sizeof(pkt));
                if (!mem_ptr) continue;
                memcpy(mem_ptr, &pkt, sizeof(pkt));
                ta->context_       = nullptr;
                ta->sfd_           = (uint32_t)g_sfd_a;
                ta->stream_type_   = kRealTimeStream;
                ta->enable_key_    = 1;
                ta->stream_key_    = GAME_TEST_STREAM_KEY;
                ta->self_addr_len_ = sizeof(g_addr_a);
                memcpy(ta->self_addr_, &g_addr_a, sizeof(g_addr_a));
                ta->sock_addr_len_ = sizeof(g_addr_b);
                memcpy(ta->sock_addr_, &g_addr_b, sizeof(g_addr_b));

                uint32_t ret = GtpFrameSend(g_hdl_a, mem_ptr, sizeof(pkt), ta, 0, 0);
                if (GTP_OK == ret) {
                    g_sent_pkt++;
                    g_app_payload += sizeof(pkt);
                    g_pkt_sent_by_phase[ph]++;
                } else {
                    GtpFreePackMem(g_hdl_a, mem_ptr);
                }
            }
            next_send += interval_us;
        }

        usleep(500);
    }

    /* 尾部排水：再跑2秒让最后阶段的重传/FEC恢复到账 */
    uint64_t tail_end = now_us() + 2000000ULL;
    while (now_us() < tail_end) {
        PeriodGtpTimer(g_hdl_a);
        recv_and_deliver(g_sfd_a, g_hdl_a, &g_addr_a);
        PeriodGtpTimer(g_hdl_b);
        recv_and_deliver(g_sfd_b, g_hdl_b, &g_addr_b);
        usleep(500);
    }

    /* ─────────── 报告 ─────────── */
    printf("\n══════════════════ book_id 切换时间线 ══════════════════\n");
    printf("  %-8s %-8s %-8s %-10s %-6s\n", "t(s)", "phase", "book实", "max_s_loss%", "net");
    for (auto &s : g_alg_timeline) {
        int nominal_phase_start_s = 0;
        for (int i = 0; i < s.phase; i++) nominal_phase_start_s += g_schedule[i].duration_s;
        double lag_s = (double)s.elapsed_us/1e6 - nominal_phase_start_s;
        printf("  %-8.2f %-8s %-8u %-10.2f %-6s  (距本阶段开始 %+.2fs)\n",
               s.elapsed_us/1e6, g_schedule[s.phase].name, s.book_id, s.max_s_loss,
               s.net_bad?"bad":"good", lag_s);
    }

    printf("\n══════════════════ 分阶段统计 ══════════════════\n");
    printf("  %-42s %8s %8s %8s %8s %10s %10s\n",
           "阶段", "注入%", "包交付%", "帧完整%", "残留丢帧%", "p50(µs)", "p99(µs)");
    for (int i = 0; i < kNumPhases; i++) {
        double delivery = g_pkt_sent_by_phase[i] > 0 ?
            g_pkt_recv_by_phase[i] * 100.0 / g_pkt_sent_by_phase[i] : 0;
        double frm_ok = g_frm_sent_by_phase[i] > 0 ?
            g_frm_ok_by_phase[i] * 100.0 / g_frm_sent_by_phase[i] : 0;
        uint32_t p50, p99;
        g_ds_phase[i].p50p99(p50, p99);
        printf("  %-42s %7d%% %7.2f%% %7.2f%% %8.2f%% %10u %10u\n",
               g_schedule[i].name, g_schedule[i].loss_pct, delivery, frm_ok, 100.0-frm_ok, p50, p99);
    }

    uint32_t all_p50, all_p99;
    g_ds_all.p50p99(all_p50, all_p99);
    uint64_t tx_a = g_bw_a.total_b, tx_b = g_bw_b.total_b;
    double overhead = g_app_payload > 0 ? ((tx_a+tx_b) - g_app_payload) * 100.0 / g_app_payload : 0;
    double fec_pct  = tx_a > 0 ? g_bw_a.fec_b * 100.0 / tx_a : 0;

    printf("\n══════════════════ 全程汇总 ══════════════════\n");
    printf("  发送包=%u  接收包=%u  丢弃(模拟)=%u  CRC错误=%u  重复包=%u  FEC异常=%u\n",
           g_sent_pkt, g_recv_pkt, g_dropped, g_corruption, g_dup_pkt, g_fec_anomaly);
    printf("  全程 p50=%uµs p99=%uµs\n", all_p50, all_p99);
    printf("  带宽 A→B=%.0fKB FEC占%.1f%% 总开销%.1f%%\n", tx_a/1024.0, fec_pct, overhead);

    DeleteGtpInstance(g_hdl_a);
    DeleteGtpInstance(g_hdl_b);
    close(g_sfd_a); close(g_sfd_b);
    RmLoadGtpModule();

    return 0;
}
