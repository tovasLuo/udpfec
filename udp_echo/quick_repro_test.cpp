/*
 * game_net_test.cpp — GoodTP 全方位专业测试
 *
 * 架构说明：
 *   每个 GoodTP 实例由且仅由一个线程驱动（严格遵守单线程约束）。
 *   本测试通过"单主循环"将 send/recv/timer 集中在同一线程，
 *   用 MSG_DONTWAIT 非阻塞收包，规避原有多线程竞争问题。
 *
 * 覆盖维度：
 *   PART 1 — 9 种丢包/乱序场景（pkts_per_frm=1 均匀发包，150pps）
 *   PART 2 — FEC 自适应策略观测（15% 丢包）
 *   PART 3 — 带宽效率扫描（book2 vs book4，0-35% loss）
 *   PART 7 — 发包节奏对比（pkts_per_frm=1/2/3/4，10% 丢包，量化改善）
 *
 * 关键指标：
 *   帧交付率 / 帧完整率（游戏稳定性）/ FEC恢复 / ARQ重传
 *   带宽开销分解（FEC% / 重传% / 控制%）
 *   端到端延迟（p50 / p99）
 *
 * 构建：
 *   cd udp_echo/build && cmake .. && make game_net_test
 * 运行：
 *   ./game_net_test
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
#include <atomic>
#include <cstddef>
#include <vector>

#include "bitlinker.h"

/* ─────────── 基础工具 ─────────── */
#define LOOPBACK     "127.0.0.1"
#define PORT_A       19021
#define PORT_B       19022
#define TIMER_US     10000   /* 10 ms */

static uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* ─────────── stream_key_ 测试常量 ─────────── */
#define GAME_TEST_STREAM_KEY  0x1234567890ABCDEFULL

/* ─────────── 帧格式 ─────────── */
#define FRAME_MAGIC  0xC0DEBABE
#define PAYLOAD_SZ   188   /* 192 - 4 bytes reserved for CRC */

struct GamePkt {
    uint32_t magic;
    uint32_t frame_id;
    uint32_t pkt_in_frame;
    uint32_t pkts_per_frame;
    uint64_t send_ts_us;
    uint8_t  pad[PAYLOAD_SZ];
    uint32_t crc;          /* XOR-checksum of all preceding bytes; detects FEC corruption */
};

/* Simple non-crypto checksum: XOR every 4-byte word preceding the crc field. */
static uint32_t pkt_crc(const GamePkt *p) {
    const uint32_t *w = (const uint32_t *)p;
    uint32_t v = 0;
    for (size_t i = 0; i < offsetof(GamePkt, crc) / sizeof(uint32_t); i++) v ^= w[i];
    return v;
}

/* ─────────── 丢包模型 ─────────── */
enum LossModel { LOSS_NONE, LOSS_RANDOM, LOSS_BURST, LOSS_INTERMIT };

struct LossCfg {
    LossModel model     = LOSS_NONE;
    int  loss_pct       = 0;
    int  burst_len      = 5;
    int  burst_gap      = 40;
    int  intermit_gap   = 100;
    int  intermit_len   = 8;

    LossCfg() = default;
    LossCfg(LossModel m)                                   : model(m) {}
    LossCfg(LossModel m, int pct)                          : model(m), loss_pct(pct) {}
    LossCfg(LossModel m, int, int bl, int bg)              : model(m), burst_len(bl), burst_gap(bg) {}
    LossCfg(LossModel m, int, int bl, int bg, int ig, int il)
        : model(m), burst_len(bl), burst_gap(bg), intermit_gap(ig), intermit_len(il) {}
};

struct LossState {
    const LossCfg *cfg = nullptr;
    bool  in_drop = false;
    int   phase   = 0;

    void reset(const LossCfg *c) {
        cfg = c; in_drop = false; phase = 0;
    }

    bool drop() {
        if (!cfg) return false;
        switch (cfg->model) {
        case LOSS_NONE:   return false;
        case LOSS_RANDOM: return (rand() % 100) < cfg->loss_pct;
        case LOSS_BURST:
        case LOSS_INTERMIT: {
            int total = in_drop ? (cfg->model == LOSS_BURST ? cfg->burst_len : cfg->intermit_len)
                                : (cfg->model == LOSS_BURST ? cfg->burst_gap  : cfg->intermit_gap);
            if (++phase >= total) { in_drop = !in_drop; phase = 0; }
            return in_drop;
        }
        }
        return false;
    }
};

/* ─────────── 简单乱序缓冲（原始 UDP 字节，收包方） ─────────── */
struct RawPkt {
    uint8_t  buf[65536];
    ssize_t  n;
    struct   sockaddr_storage peer;
    socklen_t peer_len;
};

struct ReorderBuf {
    int           window = 0;
    std::vector<RawPkt*> slots;

    void reset(int w) {
        for (auto *p : slots) delete p;
        slots.clear();
        window = w;
    }

    /* 返回应立即交付的包（乱序后）；push 获取所有权，可能返回多个 */
    std::vector<RawPkt*> push(RawPkt *p) {
        slots.push_back(p);
        if ((int)slots.size() >= window) {
            std::random_shuffle(slots.begin(), slots.end());
            std::vector<RawPkt*> out;
            out.swap(slots);
            return out;
        }
        return {};
    }

    std::vector<RawPkt*> flush() {
        std::random_shuffle(slots.begin(), slots.end());
        std::vector<RawPkt*> out;
        out.swap(slots);
        return out;
    }
};

/* ─────────── 带宽统计（同线程，无需原子） ─────────── */
#define GTP_TYPE_OF(buf) (((*(uint32_t*)(buf)) >> 16) & 0x7u)
#define T_DATA 1
#define T_FEC  3
#define T_ACK  2
#define T_NACK 6

struct BwStat {
    uint64_t data_b=0, fec_b=0, ack_b=0, retran_b=0, total_b=0;
    uint32_t data_p=0, fec_p=0, ack_p=0, nack_p=0, retran_p=0;

    void add(const void *pack, uint32_t sz) {
        uint32_t t = GTP_TYPE_OF(pack);
        total_b += sz;
        if (t == T_DATA) {
            data_b += sz; data_p++;
            const uint8_t *b = (const uint8_t*)pack;
            if ((b[8] >> 4) & 0x7) { retran_b += sz; retran_p++; }
        } else if (t == T_FEC)  { fec_b  += sz; fec_p++; }
        else if  (t == T_ACK)  { ack_b  += sz; ack_p++; }
        else if  (t == T_NACK) { nack_p++; }
    }
    void reset() { *this = BwStat{}; }
};

/* ─────────── 延迟采样 ─────────── */
struct DelaySampler {
    std::vector<uint32_t> v;
    void add(uint32_t us) { v.push_back(us); }
    void p50p99(uint32_t &p50, uint32_t &p99) {
        if (v.empty()) { p50=p99=0; return; }
        std::sort(v.begin(), v.end());
        p50 = v[v.size()*50/100];
        p99 = v[v.size()*99/100];
    }
    void reset() { v.clear(); }
};

/* ─────────── 帧完整性（无锁，同线程访问） ─────────── */
struct FrameRecord {
    uint32_t recv=0, total=0;
    uint32_t seen_mask=0;   /* bit i set => pkt_in_frame==i already delivered to app */
};
static std::vector<FrameRecord> g_frames;  /* indexed by frame_id-1 */

/* ─────────── 场景配置与结果 ─────────── */
struct SceneCfg {
    const char *name;
    LossCfg     loss;
    int  reorder_win  = 0;
    int  pps          = 150;
    int  pkts_per_frm = 3;
    int  duration_s   = 60;
    int  book_id      = 4;
};

struct SceneResult {
    const char *name    = nullptr;
    uint32_t sent_pkt=0, recv_pkt=0;
    uint32_t sent_frm=0, recv_frm=0;
    uint32_t frm_total=0, frm_complete=0;
    uint64_t app_payload=0;
    BwStat   bw_a, bw_b;
    uint32_t dropped=0;
    uint32_t fec_rec=0, arq_rto=0, arq_ack=0, arq_boost=0;
    uint32_t dup_pkt=0, corruption=0;
    uint32_t p50=0, p99=0;
    /* 算法内部状态（从 GetAlgorithmParam 解析） */
    uint32_t book_id_actual = 0;
    bool     net_bad        = false;
    uint32_t max_boost      = 0;
    bool     alg_top_off    = false;
};

/* ─────────── 全局两端状态（场景级别重置） ─────────── */
static int       g_sfd_a = -1, g_sfd_b = -1;
static struct    sockaddr_in g_addr_a, g_addr_b;
static GtpHandler_p g_hdl_a = INVALID_GTP_HANDLER;
static GtpHandler_p g_hdl_b = INVALID_GTP_HANDLER;

static LossState    g_loss;
static ReorderBuf   g_reorder;
static BwStat       g_bw_a, g_bw_b;
static DelaySampler g_ds;
static uint32_t     g_dropped = 0;
static uint32_t     g_sent_pkt = 0, g_recv_pkt = 0;
static uint32_t     g_sent_frm = 0, g_recv_frm = 0;
static uint64_t     g_app_payload = 0;
static uint32_t     g_fec_anomaly = 0;
static uint32_t     g_quintuple_cnt = 0;
static uint32_t     g_corruption = 0;  /* CRC mismatch after FEC/ARQ recovery */
static uint32_t     g_dup_pkt = 0;     /* same (frame_id,pkt_in_frame) delivered to app twice */

/* ─────────── GoodTP 回调 ─────────── */
static uint32_t SendCbA(GtpHandler_p, void *pack, uint32_t sz, GtpAddr *addr) {
    g_bw_a.add(pack, sz);
    if (g_loss.drop()) { g_dropped++; return GTP_OK; }
    sendto(g_sfd_a, pack, sz, 0, (struct sockaddr*)&g_addr_b, sizeof(g_addr_b));
    return GTP_OK;
}

static uint32_t RecvCbB(GtpHandler_p, void *frame, uint32_t sz, GtpAddr *) {
    if (sz < sizeof(GamePkt)) return GTP_OK;
    GamePkt *pkt = (GamePkt*)frame;
    if (pkt->magic != FRAME_MAGIC) return GTP_OK;

    if (pkt->crc != pkt_crc(pkt)) g_corruption++;

    g_recv_pkt++;

    /* frame completeness + duplicate-delivery detection */
    uint32_t fid = pkt->frame_id;
    if (fid > 0 && fid <= (uint32_t)g_frames.size()) {
        auto &fr = g_frames[fid-1];
        if (fr.total == 0) fr.total = pkt->pkts_per_frame;
        uint32_t bit = pkt->pkt_in_frame;
        if (bit < 32) {
            if (fr.seen_mask & (1u << bit)) {
                g_dup_pkt++;            /* app already received this exact packet once */
            } else {
                fr.seen_mask |= (1u << bit);
                if (++fr.recv == fr.total) g_recv_frm++;
            }
        }
    }

    uint64_t now = now_us();
    if (now > pkt->send_ts_us)
        g_ds.add((uint32_t)(now - pkt->send_ts_us));

    return GTP_OK;
}

static uint32_t SendCbB(GtpHandler_p, void *pack, uint32_t sz, GtpAddr *) {
    g_bw_b.add(pack, sz);
    /* B→A 方向（ACK/NACK/RTT）同样随机丢包，模拟真实双向网络丢包 */
    if (g_loss.drop()) { g_dropped++; return GTP_OK; }
    sendto(g_sfd_b, pack, sz, 0, (struct sockaddr*)&g_addr_a, sizeof(g_addr_a));
    return GTP_OK;
}

static uint32_t RecvCbA(GtpHandler_p, void *, uint32_t, GtpAddr *) { return GTP_OK; }

static void LogCb(uint32_t lv, const char *fmt, ...) {
    if (lv > 5) return;   /* Notice(5)+Error/Warn visible */
    char buf[512];
    va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
    if (strstr(buf,"fec restore abnormal")) g_fec_anomaly++;
    if (strstr(buf,"quintuple may be mixed")) {
        g_quintuple_cnt++;
        fprintf(stderr, "[SN-WARN] %s", buf);
    }
    /* emit error/warning/notice to stderr for post-analysis */
    if (lv <= 5) {
        const char *tag = (lv<=2)?"[CRIT]":(lv==3)?"[ERR]":(lv==4)?"[WARN]":"[NOTICE]";
        fprintf(stderr, "%s %s", tag, buf);
    }
}
static uint32_t LogLvCb() { return 5; }

/* ─────────── 非阻塞收包并递交给 GoodTP ─────────── */
static void recv_and_deliver(int sfd, GtpHandler_p hdl,
                              const struct sockaddr_in *self_addr,
                              ReorderBuf *rbuf) {
    static uint8_t buf[65536];
    for (int i = 0; i < 32; i++) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof(peer);
        ssize_t n = recvfrom(sfd, buf, sizeof(buf), MSG_DONTWAIT,
                              (struct sockaddr*)&peer, &plen);
        if (n <= 0) break;

        auto deliver = [&](const uint8_t *data, ssize_t dn,
                           const struct sockaddr_storage &p, socklen_t pl) {
            uint32_t sz = (uint32_t)dn;
            uint32_t msz = 0;
            GtpAddr *ta = nullptr;
            uint8_t *mem = GtpMallocPackMem(hdl, nullptr, 0, &msz, (void**)&ta, sz);
            if (!mem) return;
            memcpy(mem, data, sz);
            uint64_t sk = 0;
            GtpCheckPacketInvalid(mem, sz, &sk);
            ta->context_       = nullptr;
            ta->sfd_           = (uint32_t)sfd;
            ta->stream_type_   = kRealTimeStream;
            ta->enable_key_    = 1;
            ta->stream_key_    = sk ? sk : GAME_TEST_STREAM_KEY;
            ta->self_addr_len_ = sizeof(*self_addr);
            memcpy(ta->self_addr_, self_addr, sizeof(*self_addr));
            ta->sock_addr_len_ = (uint32_t)pl;
            memcpy(ta->sock_addr_, &p, pl);
            if (GTP_OK != GtpPacketReceive(hdl, mem, sz, ta))
                GtpFreePackMem(hdl, mem);
        };

        if (rbuf && rbuf->window > 1) {
            RawPkt *rp = new RawPkt();
            memcpy(rp->buf, buf, n);
            rp->n = n; rp->peer = peer; rp->peer_len = plen;
            auto out = rbuf->push(rp);
            for (auto *op : out) {
                deliver(op->buf, op->n, op->peer, op->peer_len);
                delete op;
            }
        } else {
            deliver(buf, n, peer, plen);
        }
    }
}

/* ─────────── socket 创建 ─────────── */
static int make_sock(uint16_t port, struct sockaddr_in *out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    /* 设置为非阻塞，recv_and_deliver 使用 MSG_DONTWAIT */
    memset(out, 0, sizeof(*out));
    out->sin_family      = AF_INET;
    out->sin_port        = htons(port);
    out->sin_addr.s_addr = INADDR_ANY;
    bind(fd, (struct sockaddr*)out, sizeof(*out));
    inet_pton(AF_INET, LOOPBACK, &out->sin_addr);
    return fd;
}

/* ─────────── 场景执行（单主循环，无额外线程） ─────────── */
SceneResult run_scene(const SceneCfg &cfg) {
    /* 初始化状态 */
    g_loss.reset(&cfg.loss);
    g_reorder.reset(cfg.reorder_win);
    g_bw_a.reset(); g_bw_b.reset(); g_ds.reset();
    g_dropped = 0; g_sent_pkt = 0; g_recv_pkt = 0;
    g_sent_frm = 0; g_recv_frm = 0; g_app_payload = 0;
    g_fec_anomaly = 0; g_quintuple_cnt = 0; g_corruption = 0; g_dup_pkt = 0;

    /* 预分配帧记录 */
    /* pps = 总包/秒；frame_interval = pkts_per_frm/pps；frames = pps/pkts_per_frm × duration */
    int frames_per_sec = cfg.pps / cfg.pkts_per_frm;
    int max_frames = frames_per_sec * cfg.duration_s + 256;
    g_frames.assign(max_frames, FrameRecord{});

    /* sockets */
    g_sfd_a = make_sock(PORT_A, &g_addr_a);
    g_sfd_b = make_sock(PORT_B, &g_addr_b);

    struct sockaddr_in peer_b = g_addr_b; peer_b.sin_port = htons(PORT_B);
    struct sockaddr_in peer_a = g_addr_a; peer_a.sin_port = htons(PORT_A);
    memcpy(&g_addr_b, &peer_b, sizeof(peer_b));
    memcpy(&g_addr_a, &peer_a, sizeof(peer_a));

    /* GoodTP 实例 */
    GtpCallBackParam cb_a = {SendCbA, RecvCbA, nullptr, LogCb, LogLvCb, nullptr};
    GtpCallBackParam cb_b = {SendCbB, RecvCbB, nullptr, LogCb, LogLvCb, nullptr};
    MemPoolConfig mem{};
    mem.m_256bytes_num_ = 4096; mem.m_512bytes_num_ = 4096;
    mem.m_1k_num_ = 2048; mem.m_1_5k_num_ = 2048;
    mem.m_4k_num_ = 512;  mem.m_8k_num_   = 256;
    mem.m_64k_num_ = 32;

    g_hdl_a = CreateGtpInstance(1, 10000000, &cb_a, &mem);
    g_hdl_b = CreateGtpInstance(2, 10000000, &cb_b, &mem);

    /* 主循环（单线程，严格遵守单线程约束） */
    /* frame_interval = pkts_per_frm / pps（例：3pkt/帧 ÷ 150pps = 20ms → 50fps） */
    uint64_t interval_us  = 1000000ULL * (uint64_t)cfg.pkts_per_frm / (uint64_t)cfg.pps;
    uint64_t deadline     = now_us() + (uint64_t)cfg.duration_s * 1000000ULL;
    uint64_t next_send    = now_us();
    uint64_t next_timer_a = now_us();
    uint64_t next_timer_b = now_us();
    uint32_t frame_id     = 0;

    while (now_us() < deadline) {
        uint64_t t = now_us();

        /* ① timer A（hdl_a 专属） */
        if (t >= next_timer_a) {
            PeriodGtpTimer(g_hdl_a);
            next_timer_a = t + TIMER_US;
        }

        /* ② timer B（hdl_b 专属） */
        if (t >= next_timer_b) {
            PeriodGtpTimer(g_hdl_b);
            next_timer_b = t + TIMER_US;
        }

        /* ③ 收 A 端包（ACK/NACK 来自 B） */
        recv_and_deliver(g_sfd_a, g_hdl_a, &g_addr_a, nullptr);

        /* ④ 收 B 端包（数据包来自 A） */
        recv_and_deliver(g_sfd_b, g_hdl_b, &g_addr_b,
                         (cfg.reorder_win > 1) ? &g_reorder : nullptr);

        /* ⑤ 发包 */
        if (t >= next_send) {
            frame_id++;
            if (frame_id <= (uint32_t)g_frames.size())
                g_frames[frame_id-1].total = (uint32_t)cfg.pkts_per_frm;

            for (int p = 0; p < cfg.pkts_per_frm; p++) {
                GamePkt pkt{};
                pkt.magic          = FRAME_MAGIC;
                pkt.frame_id       = frame_id;
                pkt.pkt_in_frame   = (uint32_t)p;
                pkt.pkts_per_frame = (uint32_t)cfg.pkts_per_frm;
                pkt.send_ts_us     = now_us();
                for (size_t bi = 0; bi < sizeof(pkt.pad); bi++)
                    pkt.pad[bi] = (uint8_t)(rand() & 0xFF);
                pkt.crc = pkt_crc(&pkt);

                uint32_t msz = 0;
                GtpAddr *ta  = nullptr;
                /* 每包独立分配，避免共享 TranBuf 的竞争风险 */
                uint8_t *mem_ptr = GtpMallocPackMem(g_hdl_a, nullptr, 0, &msz,
                                                     (void**)&ta, sizeof(pkt));
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
                } else {
                    GtpFreePackMem(g_hdl_a, mem_ptr);
                }
            }
            g_sent_frm++;
            next_send += interval_us;
        }

        usleep(500);   /* 500µs：让出 CPU 同时保证接收及时（每毫秒 2 次轮询） */
    }

    /* flush 乱序缓冲 */
    for (auto *p : g_reorder.flush()) {
        uint32_t sz = (uint32_t)p->n;
        uint32_t msz = 0;
        GtpAddr *ta = nullptr;
        uint8_t *mem = GtpMallocPackMem(g_hdl_b, nullptr, 0, &msz, (void**)&ta, sz);
        if (mem) {
            memcpy(mem, p->buf, sz);
            uint64_t fsk = 0;
            GtpCheckPacketInvalid(mem, sz, &fsk);
            ta->context_ = nullptr; ta->sfd_ = (uint32_t)g_sfd_b;
            ta->stream_type_ = kRealTimeStream;
            ta->enable_key_  = 1;
            ta->stream_key_  = fsk ? fsk : GAME_TEST_STREAM_KEY;
            ta->self_addr_len_ = sizeof(g_addr_b);
            memcpy(ta->self_addr_, &g_addr_b, sizeof(g_addr_b));
            ta->sock_addr_len_ = p->peer_len;
            memcpy(ta->sock_addr_, &p->peer, p->peer_len);
            if (GTP_OK != GtpPacketReceive(g_hdl_b, mem, sz, ta))
                GtpFreePackMem(g_hdl_b, mem);
        }
        delete p;
    }

    /* 再跑 500ms 让尾包到达 */
    uint64_t tail_end = now_us() + 500000ULL;
    while (now_us() < tail_end) {
        PeriodGtpTimer(g_hdl_a);
        recv_and_deliver(g_sfd_a, g_hdl_a, &g_addr_a, nullptr);
        PeriodGtpTimer(g_hdl_b);
        recv_and_deliver(g_sfd_b, g_hdl_b, &g_addr_b, nullptr);
        usleep(TIMER_US);
    }

    /* 算法统计 */
    SceneResult res{};
    res.name = cfg.name;

    uint8_t alg[8192]={}, alg_b[8192]={};
    uint8_t ip_a[64], ip_b[64];
    inet_ntop(AF_INET, &g_addr_a.sin_addr, (char*)ip_a, 64);
    inet_ntop(AF_INET, &g_addr_b.sin_addr, (char*)ip_b, 64);
    GetAlgorithmParam(g_hdl_a, ip_a, ip_b, alg, sizeof(alg));
    GetAlgorithmParam(g_hdl_b, ip_b, ip_a, alg_b, sizeof(alg_b));

    const char *p;
    if ((p = strstr((char*)alg, "rto_resend_counter")))   sscanf(p, "rto_resend_counter= %u",   &res.arq_rto);
    if ((p = strstr((char*)alg, "ack_resend_counter")))   sscanf(p, "ack_resend_counter= %u",   &res.arq_ack);
    if ((p = strstr((char*)alg, "boost_resend_counter"))) sscanf(p, "boost_resend_counter= %u", &res.arq_boost);
    if ((p = strstr((char*)alg_b,"fec_res_to_app_sum"))) sscanf(p, "fec_res_to_app_sum= %u",&res.fec_rec);
    /* 解析内部算法状态（发送端 A 的视角） */
    if ((p = strstr((char*)alg, "book_id="))) {
        sscanf(p, "book_id= %u", &res.book_id_actual);
    }
    if ((p = strstr((char*)alg, "net_quality="))) {
        char tmp[16]="good";
        sscanf(p, "net_quality= %15s", tmp);
        res.net_bad = (tmp[0]=='b');
    }
    if ((p = strstr((char*)alg, "max_boost_num=")))
        sscanf(p, "max_boost_num= %u", &res.max_boost);
    if ((p = strstr((char*)alg, "alg_top_switch="))) {
        char tmp[8]="On";
        sscanf(p, "alg_top_switch= %7s", tmp);
        res.alg_top_off = (tmp[0]=='O' && tmp[1]=='f');
    }

    res.sent_pkt     = g_sent_pkt;
    res.recv_pkt     = g_recv_pkt;
    res.sent_frm     = g_sent_frm;
    res.recv_frm     = g_recv_frm;
    res.frm_total    = frame_id;
    res.frm_complete = g_recv_frm;
    res.app_payload  = g_app_payload;
    res.dup_pkt      = g_dup_pkt;
    res.corruption   = g_corruption;
    /* unconditional alarm: catches duplicate/corrupted delivery to app even in
     * PART3/4/5/7-10, whose table printers don't surface dup_pkt/corruption. */
    if (g_dup_pkt > 0 || g_corruption > 0) {
        fprintf(stderr, "[DUP-ALARM] scene=\"%s\" dup_pkt=%u corruption=%u\n",
                cfg.name, g_dup_pkt, g_corruption);
    }
    res.bw_a         = g_bw_a;
    res.bw_b         = g_bw_b;
    res.dropped      = g_dropped;
    g_ds.p50p99(res.p50, res.p99);

    DeleteGtpInstance(g_hdl_a);
    DeleteGtpInstance(g_hdl_b);
    close(g_sfd_a); close(g_sfd_b);
    usleep(50000);

    return res;
}

/* ─────────── 输出 ─────────── */
static void sep(char c='-', int w=88) { for(int i=0;i<w;i++) putchar(c); putchar('\n'); }

static void print_result(const SceneResult &r, int book_id, int pps) {
    double delivery  = r.sent_pkt > 0 ? r.recv_pkt * 100.0 / r.sent_pkt : 0;
    double frm_succ  = r.frm_total > 0 ? r.frm_complete * 100.0 / r.frm_total : 0;
    double eff_loss  = r.sent_pkt > 0 ? (1.0 - (double)r.recv_pkt / r.sent_pkt) * 100.0 : 0;
    uint64_t tx_a    = r.bw_a.total_b;
    uint64_t tx_b    = r.bw_b.total_b;
    double fec_pct   = tx_a > 0 ? r.bw_a.fec_b * 100.0 / tx_a : 0;
    double retr_pct  = tx_a > 0 ? r.bw_a.retran_b * 100.0 / tx_a : 0;
    double ack_pct   = tx_a > 0 ? tx_b * 100.0 / tx_a : 0;
    double overhead  = r.app_payload > 0 ? ((tx_a+tx_b) - r.app_payload) * 100.0 / r.app_payload : 0;

    printf("  %-44s book=%d pps=%d\n", r.name, book_id, pps);
    printf("  丢包(模拟)%5.1f%% | 包交付率%6.2f%% | 帧完整率%6.2f%%\n",
           eff_loss, delivery, frm_succ);
    printf("  FEC恢复%4u  ARQ(rto)%4u  ARQ(ack)%4u  ARQ(boost)%4u  FEC异常%u  SN跳跃告警%u  CRC错误%u  重复包%u\n",
           r.fec_rec, r.arq_rto, r.arq_ack, r.arq_boost, g_fec_anomaly, g_quintuple_cnt, r.corruption, r.dup_pkt);
    if (r.dup_pkt > 0)
        printf("  *** 警告：检测到 %u 个重复包被上报给应用层 ***\n", r.dup_pkt);
    printf("  延迟 p50=%uµs  p99=%uµs\n", r.p50, r.p99);
    printf("  带宽 A→B%.0fKB FEC占%.1f%% 重传占%.1f%% B→A控制%.1f%% 总开销%.1f%%\n",
           tx_a/1024.0, fec_pct, retr_pct, ack_pct, overhead);
    printf("  [算法] book_id实=%u  net=%s  boost上限=%u  alg_top=%s\n",
           r.book_id_actual, r.net_bad?"bad ":"good", r.max_boost,
           r.alg_top_off?"Off":"On");

    const char *s;
    if (frm_succ>=99.5) s="★★★★★ 极好";
    else if (frm_succ>=98.0) s="★★★★  优";
    else if (frm_succ>=95.0) s="★★★   良（轻微卡顿）";
    else if (frm_succ>=90.0) s="★★    差（明显卡顿）";
    else                     s="★     很差（严重掉帧）";
    printf("  游戏稳定性：%s\n", s);
}

/* ─────────── 主函数 ─────────── */
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    srand((unsigned)time(nullptr));

    if (GTP_OK != InsLoadGtpModule()) {
        fprintf(stderr, "InsLoadGtpModule failed\n"); return 1;
    }

    sep('=',88); printf("  快速回归验证：卡头阻塞修复第三方案针对性场景\n"); sep('=',88);

    /* 目标场景：此前 HOL-blocking 修复暴露 dup_pkt 回归的最小集合
     *   S1  0%%丢包基准（此前 dup_pkt=7）
     *   S2  5%%丢包（guarded 版本下 dup_pkt 从190恶化到634）
     *   L15 15%%丢包 book4（此前 dup_pkt=4483）
     *   L25 25%%丢包 book4（此前 dup_pkt=2847）
     */
    struct { const char *name; LossCfg loss; int book; } scenes[] = {
        {"S1 零丢包基准",     {LOSS_NONE},        4},
        {"S2 随机5%丢包",     {LOSS_RANDOM, 5},   4},
        {"L15 随机15%丢包",   {LOSS_RANDOM, 15},  4},
        {"L25 随机25%丢包",   {LOSS_RANDOM, 25},  4},
    };

    uint32_t total_dup = 0;
    for (auto &sc : scenes) {
        SceneCfg cfg{};
        cfg.name = sc.name; cfg.loss = sc.loss;
        cfg.pps = 150; cfg.pkts_per_frm = 1; cfg.duration_s = 60; cfg.book_id = sc.book;
        auto r = run_scene(cfg);
        sep(); print_result(r, sc.book, 150);
        total_dup += r.dup_pkt;
    }

    sep('=');
    printf("  快速验证完成：累计 dup_pkt=%u\n", total_dup);
    sep('=');

    RmLoadGtpModule();
    return 0;
}
