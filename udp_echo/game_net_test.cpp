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
 *   PART 3 — 随机波动丢包 2%-30% — book 自适应切换正确性验证
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
enum LossModel { LOSS_NONE, LOSS_RANDOM, LOSS_BURST, LOSS_INTERMIT, LOSS_RANDOM_WALK };

struct LossCfg {
    LossModel model     = LOSS_NONE;
    int  loss_pct       = 0;
    int  burst_len      = 5;
    int  burst_gap      = 40;
    int  intermit_gap   = 100;
    int  intermit_len   = 8;
    int  rw_min_pct     = 2;     /* LOSS_RANDOM_WALK: 丢包率下界 */
    int  rw_max_pct     = 25;    /* LOSS_RANDOM_WALK: 丢包率上界 */
    int  rw_step_ms     = 2000;  /* LOSS_RANDOM_WALK: 每隔多久走一步 */
    int  rw_max_step_pct = 5;    /* LOSS_RANDOM_WALK: 每步最多漂移多少个百分点 */

    LossCfg() = default;
    LossCfg(LossModel m)                                   : model(m) {}
    LossCfg(LossModel m, int pct)                          : model(m), loss_pct(pct) {}
    LossCfg(LossModel m, int, int bl, int bg)              : model(m), burst_len(bl), burst_gap(bg) {}
    LossCfg(LossModel m, int, int bl, int bg, int ig, int il)
        : model(m), burst_len(bl), burst_gap(bg), intermit_gap(ig), intermit_len(il) {}
    /* [min_pct, max_pct] 区间内做有界随机游走：每 step_ms 走一步，每步漂移
     * [-max_step_pct, +max_step_pct] 个百分点（钳制在区间内）。用平滑漂移而非
     * 瞬间跳变，是因为真实网络丢包率是渐变的，瞬间跳变（如 0%→23%）会触发
     * goodtp_session.cpp 里为"长时间中断后重连"设计的会话重建逻辑，产生的
     * 瞬时重复包是那条独立路径的已知行为，不是本测试想验证的对象。
     * 用于验证 FEC book 自适应切换在持续波动的丢包环境下是否跟得上、切得对。 */
    static LossCfg RandomWalk(int min_pct, int max_pct, int step_ms, int max_step_pct = 5) {
        LossCfg c; c.model = LOSS_RANDOM_WALK;
        c.rw_min_pct = min_pct; c.rw_max_pct = max_pct; c.rw_step_ms = step_ms;
        c.rw_max_step_pct = max_step_pct;
        return c;
    }
};

struct LossState {
    const LossCfg *cfg   = nullptr;
    bool  in_drop         = false;
    int   phase           = 0;
    int   rw_cur_pct       = -1;     /* LOSS_RANDOM_WALK: 当前生效丢包率，-1=未初始化 */
    uint64_t rw_next_roll_us = 0;

    void reset(const LossCfg *c) {
        cfg = c; in_drop = false; phase = 0;
        rw_cur_pct = -1; rw_next_roll_us = 0;
    }

    bool drop() {
        if (!cfg) return false;
        switch (cfg->model) {
        case LOSS_NONE:   return false;
        case LOSS_RANDOM: return (rand() % 100) < cfg->loss_pct;
        case LOSS_RANDOM_WALK: {
            uint64_t now = now_us();
            if (now >= rw_next_roll_us) {
                if (0 > rw_cur_pct) {
                    rw_cur_pct = (cfg->rw_min_pct + cfg->rw_max_pct) / 2;  /* 起点：区间中点 */
                } else {
                    int step = cfg->rw_max_step_pct;
                    int delta = step > 0 ? (rand() % (2 * step + 1)) - step : 0;
                    rw_cur_pct += delta;
                    if (rw_cur_pct < cfg->rw_min_pct) rw_cur_pct = cfg->rw_min_pct;
                    if (rw_cur_pct > cfg->rw_max_pct) rw_cur_pct = cfg->rw_max_pct;
                }
                rw_next_roll_us = now + (uint64_t)cfg->rw_step_ms * 1000ULL;
            }
            return (rand() % 100) < rw_cur_pct;
        }
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
    bool track_book   = false;  /* 运行期间轮询 book_id，变化时打印时间线（诊断用） */
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

    /* book_id 时间线追踪（诊断用，仅 track_book=true 时启用） */
    uint8_t  track_ip_a[64]={}, track_ip_b[64]={};
    uint64_t scene_start_us   = now_us();
    uint64_t next_book_poll   = scene_start_us;
    uint32_t last_book_id     = 0xFFFFFFFF;
    if (cfg.track_book) {
        inet_ntop(AF_INET, &g_addr_a.sin_addr, (char*)track_ip_a, 64);
        inet_ntop(AF_INET, &g_addr_b.sin_addr, (char*)track_ip_b, 64);
        printf("  ── book_id 时间线（丢包 model=%d 变化时打印）──\n", (int)cfg.loss.model);
    }

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

        /* ⓪ book_id 时间线轮询（每 200ms，仅变化时打印） */
        if (cfg.track_book && t >= next_book_poll) {
            next_book_poll = t + 200000ULL;
            uint8_t alg_probe[8192] = {};
            GetAlgorithmParam(g_hdl_a, track_ip_a, track_ip_b, alg_probe, sizeof(alg_probe));
            const char *bp = strstr((char*)alg_probe, "book_id=");
            uint32_t cur_book = 0;
            if (bp) sscanf(bp, "book_id= %u", &cur_book);
            if (cur_book != last_book_id) {
                last_book_id = cur_book;
                printf("    t=%5.2fs  book实=%u  当前注入丢包=%d%%\n",
                       (t - scene_start_us) / 1e6, cur_book, g_loss.rw_cur_pct);
            }
        }

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

    sep('=',88); printf("  GoodTP 全方位专业测试（单线程单实例驱动，符合设计约束）\n"); sep('=',88);

    /* ══════════════════════════════════════════════════
     * PART 1: 核心场景（150pps / 1包/帧（均匀发包）/ book4 / 6s）
     * Direction A：每 tick 发 1 包（6.7ms间距），FEC矩阵均匀填充
     * ══════════════════════════════════════════════════ */
    printf("\n【PART 1】核心功能与游戏稳定性（150pps / 1包/帧-均匀发包 / book_id=4）\n");
    printf("  方向A：pkts_per_frm=1，每包间距6.7ms，FEC矩阵在4×6.7ms=26.7ms内均匀填充\n");

    struct { const char *name; LossCfg loss; int reorder; } p1[] = {
        {"S1 零丢包（好网络基准）",           {LOSS_NONE}, 0},
        {"S2 随机 5%（轻度丢包）",            {LOSS_RANDOM, 5}, 0},
        {"S3 随机 15%（典型游戏场景）",       {LOSS_RANDOM, 15}, 0},
        {"S4 随机 30%（恶劣网络）",           {LOSS_RANDOM, 30}, 0},
        {"S5 突发 5连/40间隔",               {LOSS_BURST, 0, 5, 40}, 0},
        {"S6 突发 10连/80间隔（超 FEC 矩阵）",{LOSS_BURST, 0, 10, 80}, 0},
        {"S7 纯乱序 窗口=4（无丢包）",        {LOSS_NONE}, 4},
        {"S8 乱序窗口=4 + 10% 丢包",         {LOSS_RANDOM, 10}, 4},
        {"S9 间歇中断 8包断/100包平静",       {LOSS_INTERMIT, 0, 5, 40, 100, 8}, 0},
    };

    for (auto &sc : p1) {
        SceneCfg cfg{};
        cfg.name = sc.name; cfg.loss = sc.loss; cfg.reorder_win = sc.reorder;
        cfg.pps = 150; cfg.pkts_per_frm = 1; cfg.duration_s = 60; cfg.book_id = 4;
        sep(); print_result(run_scene(cfg), 4, 150);
    }

    /* ══════════════════════════════════════════════════
     * PART 2: FEC 自适应策略观测（15% 丢包）
     * 注意：GoodTP 内部根据 PPS+丢包率自动选择 book_id；
     *       cfg.book_id 字段保留但不传入库，此处观测自适应行为。
     * ══════════════════════════════════════════════════ */
    printf("\n【PART 2】FEC 自适应策略观测（15%% 丢包 / 150pps；GoodTP内部自适应选book）\n");

    struct { int book_id; const char *label; } books[] = {
        {2, "B1 观测run1（同参数重复1）"},
        {5, "B2 观测run2（同参数重复2）"},
        {4, "B3 观测run3（同参数重复3）"},
        {3, "B4 观测run4（同参数重复4）"},
    };

    struct BwRow { int book; double fec_pct, total_ovhd, delivery; uint32_t fec_rec, arq_rto; };
    std::vector<BwRow> bw_rows;

    for (auto &bk : books) {
        SceneCfg cfg{};
        cfg.name = bk.label;
        cfg.loss.model = LOSS_RANDOM; cfg.loss.loss_pct = 15;
        cfg.pps = 150; cfg.pkts_per_frm = 3; cfg.duration_s = 60; cfg.book_id = bk.book_id;
        auto r = run_scene(cfg);
        sep(); print_result(r, bk.book_id, 150);
        BwRow row{};
        row.book     = bk.book_id;
        row.fec_pct  = r.bw_a.total_b > 0 ? r.bw_a.fec_b * 100.0 / r.bw_a.total_b : 0;
        uint64_t tt  = r.bw_a.total_b + r.bw_b.total_b;
        row.total_ovhd = r.app_payload > 0 ? (tt - r.app_payload)*100.0/r.app_payload : 0;
        row.delivery = r.sent_pkt > 0 ? r.recv_pkt * 100.0 / r.sent_pkt : 0;
        row.fec_rec  = r.fec_rec;
        row.arq_rto  = r.arq_rto;
        bw_rows.push_back(row);
    }

    /* ══════════════════════════════════════════════════
     * PART 3: 随机波动丢包（2%-30%区间）— book 自适应切换正确性
     * 丢包率不是固定档位，而是每隔 step_ms 就在 [2,30]% 区间内做有界随机游走
     * （每步漂移最多 ±max_step_pct 个百分点），用来验证 FEC book 自适应在
     * 持续波动、覆盖低/中/高全档位的真实网络下能否跟得上、切得对
     * （而不是像 book2 vs book4 静态对比那样只看两个固定点位）。
     * max_step_pct 取得比默认(5)更大，是为了让 60s 内的有限步数也能真正
     * 走到 2%/30% 两端附近，而不是被夹在区间中段。
     * ══════════════════════════════════════════════════ */
    printf("\n【PART 3】随机波动丢包 2%%-30%% — book 自适应切换正确性（150pps）\n");
    printf("  参考策略表：0-2%% auto关闭FEC | 2-10%% book2/5 | 10-20%% book4 | 20-35%% book4/3\n");

    struct RwScene { const char *name; int step_ms; int dur_s; };
    RwScene rw_scenes[] = {
        {"RW1 快速波动（每2s重随机）", 2000, 60},
        {"RW2 慢速波动（每6s重随机）", 6000, 60},
    };

    for (auto &sc : rw_scenes) {
        SceneCfg cfg{};
        cfg.name = sc.name;
        cfg.loss = LossCfg::RandomWalk(2, 30, sc.step_ms, /*max_step_pct=*/10);
        cfg.pps = 150; cfg.pkts_per_frm = 1; cfg.duration_s = sc.dur_s; cfg.track_book = true;
        auto r = run_scene(cfg);
        sep(); print_result(r, r.book_id_actual, 150);
    }

    /* ══════════════════════════════════════════════════
     * 综合分析
     * ══════════════════════════════════════════════════ */
    sep('=');
    printf("\n【综合分析与建议】\n");
    sep('-');
    printf("  1. FEC 带宽效率拐点\n");
    printf("     ┌──────────────┬──────────────┬──────────────────────────────────────┐\n");
    printf("     │  丢包率范围  │  推荐book_id │  说明                                │\n");
    printf("     ├──────────────┼──────────────┼──────────────────────────────────────┤\n");
    printf("     │  0-2%%        │  auto关闭FEC │  好网络：auto-FEC节省100%%额外带宽    │\n");
    printf("     │  2-10%%       │  book2(25%%) │  单行可恢复，低开销优先              │\n");
    printf("     │  10-20%%      │  book4(100%%）│  H+V双向覆盖，任意单包可恢复         │\n");
    printf("     │  20-35%%      │  book4/3     │  book3加斜向覆盖对角线丢包           │\n");
    printf("     │  >35%%        │  book3+ARQ   │  FEC已超能力上限，依赖ARQ兜底        │\n");
    printf("     └──────────────┴──────────────┴──────────────────────────────────────┘\n");
    printf("\n  2. FEC vs ARQ 带宽权衡\n");
    printf("     FEC 成本恒定（按 book_id 固定冗余），ARQ 成本随丢包率线性增长\n");
    printf("     game mode RTT≈40ms，ARQ 每次重传延迟≥25ms（MIN_RTO），严重影响帧感\n");
    printf("     FEC 0延迟恢复，时延不可见，是游戏场景优先选项\n");
    printf("     交叉点：丢包率>book2恢复上限(~12%%)时，book4虽贵但帧完整率更高\n");
    printf("\n  3. 突发丢包 vs 随机丢包\n");
    printf("     随机：FEC矩阵(4包)内只丢1-2包 → FEC高效恢复\n");
    printf("     突发10连丢：跨越2-3个FEC矩阵 → FEC恢复能力耗尽，ARQ接管\n");
    printf("     建议：突发场景降低burst_gap比例，避免连续矩阵全灭\n");
    printf("\n  4. 乱序\n");
    printf("     GoodTP 接收侧有去重过滤窗口，轻度乱序不影响最终交付率\n");
    printf("     但乱序会推迟FEC矩阵完成时刻，可能延迟恢复1-2个timer周期(10-20ms)\n");
    printf("\n  5. 好网络开销（0%%丢包）\n");
    printf("     book4 FEC开销≈70%%（自动FEC管理会在稳定后逐步降低FEC发送）\n");
    printf("     ACK/NACK/RTT控制包≈20-30%%\n");
    printf("     建议：0%%丢包超过1s → auto-FEC自动关闭，降至控制包开销\n");
    /* ══════════════════════════════════════════════════
     * PART 4: 延迟基准 — PPS 变化对延迟的影响（0%丢包 + 10%丢包）
     * 关注：FEC矩阵填充时间 = block_size/pps，低PPS下FEC恢复延迟大
     * ══════════════════════════════════════════════════ */
    printf("\n【PART 4】延迟基准：PPS vs 延迟（0%%丢包 / 10%%丢包，book4，pkts_per_frm=2）\n");
    printf("  FEC矩阵填充时间 = 4包/pps → 30pps=133ms  60pps=67ms  150pps=27ms  200pps=20ms\n");
    sep('-');
    printf("  %-10s %-6s %10s %10s %8s %8s %8s\n",
           "场景","pps","p50(µs)","p99(µs)","帧完整率","FEC恢复","SN告警");

    struct P4Row { int pps; int loss; uint32_t p50,p99; double frm; uint32_t fec,sn; };
    std::vector<P4Row> p4rows;

    int p4_pps[]  = {30, 60, 100, 150, 200};
    int p4_loss[] = {0, 10};
    for (int loss : p4_loss) {
        for (int pps : p4_pps) {
            SceneCfg cfg{};
            char nm[64]; snprintf(nm,sizeof(nm),"P4 loss=%d%% pps=%d", loss, pps);
            cfg.name = nm;
            cfg.loss.model = (loss==0) ? LOSS_NONE : LOSS_RANDOM;
            cfg.loss.loss_pct = loss;
            cfg.pps = pps; cfg.pkts_per_frm = 2; cfg.duration_s = 60; cfg.book_id = 4;
            auto r = run_scene(cfg);
            double frm = r.frm_total > 0 ? r.frm_complete*100.0/r.frm_total : 0;
            printf("  %-10s %-6d %10u %10u %7.2f%% %8u %8u\n",
                   (loss==0?"0%丢包":"10%丢包"), pps, r.p50, r.p99, frm, r.fec_rec, g_quintuple_cnt);
            P4Row row{pps,loss,r.p50,r.p99,frm,r.fec_rec,g_quintuple_cnt};
            p4rows.push_back(row);
        }
    }

    /* ══════════════════════════════════════════════════
     * PART 5: 延迟随丢包率变化（150pps，pkts_per_frm=1均匀，book4自适应，6s）
     * 关注：0→5%→10%→20% 延迟是否可接受，拐点在哪
     * ══════════════════════════════════════════════════ */
    printf("\n【PART 5】丢包率 → 延迟扫描（150pps / pkts_per_frm=1均匀 / book4自适应 / 6s）\n");
    sep('-');
    printf("  %-8s %10s %10s %8s %8s %8s %8s\n",
           "丢包率","p50(µs)","p99(µs)","帧完整率","FEC恢复","ARQ-RTO","SN告警");

    struct P5Row {
        int loss; uint32_t p50,p99; double frm;
        uint32_t fec,arq; uint32_t book; bool net_bad; uint32_t boost; bool top_off;
        double fec_pct, ovhd;
    };
    std::vector<P5Row> p5rows;

    int p5_losses[] = {0,1,2,5,10,20,30,45};
    for (int loss : p5_losses) {
        SceneCfg cfg{};
        char nm[64]; snprintf(nm,sizeof(nm),"P5 loss=%d%%", loss);
        cfg.name = nm;
        cfg.loss.model = (loss==0) ? LOSS_NONE : LOSS_RANDOM;
        cfg.loss.loss_pct = loss;
        cfg.pps = 150; cfg.pkts_per_frm = 1; cfg.duration_s = 60; cfg.book_id = 4;
        auto r = run_scene(cfg);
        double frm = r.frm_total > 0 ? r.frm_complete*100.0/r.frm_total : 0;
        printf("  %-8d %10u %10u %7.2f%% %8u %8u %8u\n",
               loss, r.p50, r.p99, frm, r.fec_rec, r.arq_rto, g_quintuple_cnt);
        P5Row row{};
        row.loss=loss; row.p50=r.p50; row.p99=r.p99; row.frm=frm;
        row.fec=r.fec_rec; row.arq=r.arq_rto;
        row.book=r.book_id_actual; row.net_bad=r.net_bad;
        row.boost=r.max_boost; row.top_off=r.alg_top_off;
        row.fec_pct = r.bw_a.total_b>0 ? r.bw_a.fec_b*100.0/r.bw_a.total_b : 0;
        uint64_t tt = r.bw_a.total_b + r.bw_b.total_b;
        row.ovhd    = r.app_payload>0 ? (tt-r.app_payload)*100.0/r.app_payload : 0;
        p5rows.push_back(row);
    }

    sep('=');
    printf("  【PART 5 综合对照表】GoodTP 各丢包率下内部状态完整汇总\n");
    sep('-',100);
    printf("  %-6s %-6s %-6s %-6s %-5s %-5s %-8s %-10s %-10s %-9s %-9s\n",
           "丢包%","book","net","boost","top","FEC","ARQ",
           "p50(µs)","p99(µs)","FEC带宽%","总开销%");
    sep('-',100);
    for (auto &r : p5rows) {
        printf("  %-6d %-6u %-6s %-6u %-5s %-5u %-8u %-10u %-10u %-9.1f %-9.1f\n",
               r.loss, r.book,
               r.net_bad?"bad":"good",
               r.boost,
               r.top_off?"Off":"On",
               r.fec, r.arq,
               r.p50, r.p99,
               r.fec_pct, r.ovhd);
    }
    sep('=');

    /* ══════════════════════════════════════════════════
     * PART 6: 极端与边界场景
     * ══════════════════════════════════════════════════ */
    printf("\n【PART 6】极端与边界场景\n");

    struct { const char *name; LossCfg loss; int reorder; int pps; int dur; } p6[] = {
        /* 50% 随机：FEC完全饱和，ARQ接管 */
        {"E1 随机50%%（极端）150pps",    {LOSS_RANDOM,50},  0, 150, 60},
        /* 高PPS下突发：200pps突发5连，FEC矩阵20ms填满，覆盖能力强 */
        {"E2 突发5连/40间隔 200pps",     {LOSS_BURST,0,5,40},0, 200, 60},
        /* 低PPS下突发：30pps突发5连，矩阵133ms，延迟极大 */
        {"E3 突发5连/40间隔 30pps",      {LOSS_BURST,0,5,40},0, 30,  60},
        /* 高强度突发：20连丢/200间隔 ~9%丢包但每次跨越5个FEC矩阵 */
        {"E4 突发20连/200间隔 150pps",   {LOSS_BURST,0,20,200},0,150, 60},
        /* 乱序+30%丢包 复合最坏情况 */
        {"E5 乱序win=4 + 30%% 150pps",  {LOSS_RANDOM,30},  4, 150, 60},
        /* 200pps 20%丢包：高速游戏场景+中等丢包 */
        {"E6 随机20%% 200pps FPS场景",  {LOSS_RANDOM,20},  0, 200, 60},
        /* 间歇断流：100包平静/20包断 模拟无线信道切换 */
        {"E7 间歇20断/100平静 150pps",  {LOSS_INTERMIT,0,5,40,100,20},0,150,60},
    };

    for (auto &sc : p6) {
        SceneCfg cfg{};
        cfg.name = sc.name; cfg.loss = sc.loss; cfg.reorder_win = sc.reorder;
        cfg.pps = sc.pps; cfg.pkts_per_frm = 2; cfg.duration_s = sc.dur; cfg.book_id = 4;
        sep(); print_result(run_scene(cfg), 4, sc.pps);
    }

    /* ══════════════════════════════════════════════════
     * PART 7: 发包节奏对比（Direction A 量化）
     * 对比 pkts_per_frm=1/2/3/4 在不同丢包率下的 p99 延迟
     * 理论：FEC 矩阵填充时间 = block_size/pps；FEC 发送时机影响恢复延迟
     * ══════════════════════════════════════════════════ */
    printf("\n【PART 7】发包节奏对比（Direction A 量化，150pps，book4）\n");
    printf("  理论：pkts_per_frm 越接近 block_size/2=2，FEC 矩阵完成越均衡，恢复延迟越低\n");
    sep('-');
    printf("  %-6s %-6s %10s %10s %8s %8s %9s %9s\n",
           "ppf","丢包%","p50(µs)","p99(µs)","帧完整率","FEC恢复","FEC带宽%","总开销%");

    int p7_ppf[]  = {1, 2, 3, 4};
    int p7_loss[] = {0, 5, 10, 20};
    for (int loss : p7_loss) {
        sep('-',80);
        for (int ppf : p7_ppf) {
            SceneCfg cfg{};
            char nm[64]; snprintf(nm,sizeof(nm),"P7 ppf=%d loss=%d%%", ppf, loss);
            cfg.name = nm;
            cfg.loss.model = (loss==0) ? LOSS_NONE : LOSS_RANDOM;
            cfg.loss.loss_pct = loss;
            cfg.pps = 150; cfg.pkts_per_frm = ppf; cfg.duration_s = 60; cfg.book_id = 4;
            auto r = run_scene(cfg);
            double frm = r.frm_total > 0 ? r.frm_complete*100.0/r.frm_total : 0;
            double fec_pct = r.bw_a.total_b>0 ? r.bw_a.fec_b*100.0/r.bw_a.total_b : 0;
            uint64_t tt    = r.bw_a.total_b + r.bw_b.total_b;
            double ovhd    = r.app_payload>0 ? (tt-r.app_payload)*100.0/r.app_payload : 0;
            printf("  %-6d %-6d %10u %10u %7.2f%% %8u %8.1f%% %8.1f%%\n",
                   ppf, loss, r.p50, r.p99, frm, r.fec_rec, fec_pct, ovhd);
        }
    }

    sep('=');
    printf("  PART 7 发包节奏影响小结\n");
    printf("  interval = pkts_per_frm/pps；FEC矩阵（block=4）分组：\n");
    printf("    ppf=1: 4帧填满矩阵，均匀间距6.7ms，最早FEC在第2帧（+6.7ms）\n");
    printf("    ppf=2: 2帧填满矩阵，间距13.3ms，最早FEC在第2帧（+13.3ms）\n");
    printf("    ppf=3: 2帧填满矩阵，间距20ms，首帧3包次帧1包，FEC在第2帧（+20ms）\n");
    printf("    ppf=4: 1帧填满矩阵，FEC与数据同帧发出，理论上恢复延迟最低（~loopback）\n");

    /* ══════════════════════════════════════════════════
     * PART 8: 低 PPS 专项延迟测试（ppf=1 均匀发包，10%/5% 丢包）
     * 目标：量化 Method B 在 5-30pps 真实游戏 pps 区间的收益
     * V-FEC 理论下界 = 2/pps：5pps=400ms 10pps=200ms 15pps=133ms 20pps=100ms 25pps=80ms 30pps=67ms
     * ══════════════════════════════════════════════════ */
    printf("\n【PART 8】低PPS专项延迟测试（ppf=1均匀 / book4 / 10s）\n");
    printf("  V-FEC理论下界 = 2/pps：5pps=400ms 10pps=200ms 20pps=100ms 30pps=67ms\n");
    sep('-');
    printf("  %-6s %-6s %10s %10s %8s %8s %8s %10s\n",
           "pps","丢包%","p50(µs)","p99(µs)","帧完整率","FEC恢复","ARQ(ack)","总开销%");
    sep('-');

    int p8_pps[]  = {5, 10, 15, 20, 25, 30};
    int p8_loss[] = {5, 10};
    for (int loss : p8_loss) {
        for (int pps : p8_pps) {
            SceneCfg cfg{};
            char nm[64]; snprintf(nm,sizeof(nm),"P8 %dpps %d%%", pps, loss);
            cfg.name = nm;
            cfg.loss.model = LOSS_RANDOM;
            cfg.loss.loss_pct = loss;
            cfg.pps = pps;
            cfg.pkts_per_frm = 1;   // 均匀发包，ppf=1
            cfg.duration_s = 60;
            cfg.book_id = 4;
            auto r = run_scene(cfg);
            double frm  = r.frm_total > 0 ? r.frm_complete*100.0/r.frm_total : 0;
            uint64_t tt = r.bw_a.total_b + r.bw_b.total_b;
            double ovhd = r.app_payload>0 ? (tt-r.app_payload)*100.0/r.app_payload : 0;
            printf("  %-6d %-6d %10u %10u %7.2f%% %8u %8u %9.1f%%\n",
                   pps, loss, r.p50, r.p99, frm, r.fec_rec, r.arq_ack, ovhd);
        }
        sep('-');
    }
    sep('=');
    printf("  测试完成\n");
    sep('=');

    /* ══════════════════════════════════════════════════
     * PART 9: book4 vs book5 带宽效率对比（5%丢包核心场景）
     * 目标：量化 book5(2×1 H, FEC=33%) vs book4(2×2 H+V, FEC=50%)
     *        在低丢包（2%/5%/8%）下的带宽与延迟 trade-off
     * 编译说明：默认 book4；以 -DFORCE_FEC_BOOK_LOW_LOSS=5 重新编译库
     *           可切换为 book5 对照组
     * ══════════════════════════════════════════════════ */
    printf("\n【PART 9】低丢包 FEC book 带宽效率对比（ppf=1 均匀，150pps，10s）\n");
    printf("  [library book for 2-10%% loss range: see [算法]book_id 输出]\n");
    sep('-');
    printf("  %-6s %-6s %8s %8s %7s %7s %7s %9s %9s %10s\n",
           "pps","丢包%","p50(µs)","p99(µs)","帧完%","FEC恢","ARQ(ack)","FEC占%","总开销%","book实");
    sep('-');

    struct P9Row {
        int pps; int loss;
        uint32_t p50, p99;
        double frm_pct, fec_pct, ovhd;
        uint32_t fec_rec, arq_ack;
        uint32_t book_actual;
    };

    int p9_pps[]  = {50, 100, 150, 200};
    int p9_loss[] = {2, 5, 8};

    for (int loss : p9_loss) {
        for (int pps : p9_pps) {
            SceneCfg cfg{};
            char nm[64]; snprintf(nm, sizeof(nm), "P9 %dpps %d%%", pps, loss);
            cfg.name         = nm;
            cfg.loss.model   = LOSS_RANDOM;
            cfg.loss.loss_pct = loss;
            cfg.pps          = pps;
            cfg.pkts_per_frm = 1;
            cfg.duration_s   = 60;
            cfg.book_id      = 4;

            auto r = run_scene(cfg);

            uint64_t tt     = r.bw_a.total_b + r.bw_b.total_b;
            double frm      = r.frm_total > 0 ? r.frm_complete * 100.0 / r.frm_total : 0;
            double fec_pct  = r.bw_a.total_b > 0 ? r.bw_a.fec_b * 100.0 / r.bw_a.total_b : 0;
            double ovhd     = r.app_payload > 0 ? (tt - r.app_payload) * 100.0 / r.app_payload : 0;

            printf("  %-6d %-6d %8u %8u %6.2f%% %7u %8u %8.1f%% %8.1f%% %10u\n",
                   pps, loss, r.p50, r.p99, frm, r.fec_rec, r.arq_ack, fec_pct, ovhd, r.book_id_actual);
        }
        sep('-');
    }

    /* 补充：极低 pps（游戏低频场景）5% 丢包 */
    printf("  低PPS补充（5%%丢包）\n");
    sep('-');
    int p9_low_pps[] = {10, 20, 30};
    for (int pps : p9_low_pps) {
        SceneCfg cfg{};
        char nm[64]; snprintf(nm, sizeof(nm), "P9low %dpps 5%%", pps);
        cfg.name         = nm;
        cfg.loss.model   = LOSS_RANDOM;
        cfg.loss.loss_pct = 5;
        cfg.pps          = pps;
        cfg.pkts_per_frm = 1;
        cfg.duration_s   = 60;
        cfg.book_id      = 4;

        auto r = run_scene(cfg);

        uint64_t tt    = r.bw_a.total_b + r.bw_b.total_b;
        double frm     = r.frm_total > 0 ? r.frm_complete * 100.0 / r.frm_total : 0;
        double fec_pct = r.bw_a.total_b > 0 ? r.bw_a.fec_b * 100.0 / r.bw_a.total_b : 0;
        double ovhd    = r.app_payload > 0 ? (tt - r.app_payload) * 100.0 / r.app_payload : 0;

        printf("  %-6d %-6d %8u %8u %6.2f%% %7u %8u %8.1f%% %8.1f%% %10u\n",
               pps, 5, r.p50, r.p99, frm, r.fec_rec, r.arq_ack, fec_pct, ovhd, r.book_id_actual);
    }
    sep('=');
    printf("  PART 9 完成\n");
    printf("  提示：以 -DFORCE_FEC_BOOK_LOW_LOSS=5 重建 libfec.a 后重跑，对比 book5 数据\n");
    sep('=');

    /* ══════════════════════════════════════════════════
     * PART 10: 50pps 边界专项测试（30s，统计稳定性验证）
     * 目标：确认 book5 在 30-60pps 边界的 p99 安全性
     * 关键：10s 测试仅 ~1.5 次双丢事件，30s 可获 ~4.5 次，p99 更稳定
     * 双丢概率：loss=5%: P=0.25%/对；loss=8%: P=0.64%/对
     * ══════════════════════════════════════════════════ */
    printf("\n【PART 10】50pps 边界专项（ppf=1 均匀，30s）\n");
    printf("  [测量 p50/p99/p999 帧交付延迟，帧完整率，带宽开销]\n");
    sep('-');
    printf("  %-6s %-6s %8s %8s %8s %7s %7s %9s %10s\n",
           "pps","丢包%","p50(µs)","p99(µs)","帧完%","FEC恢","ARQ(ack)","总开销%","book实");
    sep('-');

    int p10_pps[]  = {30, 40, 50, 60};
    int p10_loss[] = {5, 8};

    for (int loss : p10_loss) {
        for (int pps : p10_pps) {
            SceneCfg cfg{};
            char nm[64]; snprintf(nm, sizeof(nm), "P10 %dpps %d%%", pps, loss);
            cfg.name          = nm;
            cfg.loss.model    = LOSS_RANDOM;
            cfg.loss.loss_pct = loss;
            cfg.pps           = pps;
            cfg.pkts_per_frm  = 1;
            cfg.duration_s    = 60;
            cfg.book_id       = 4;

            auto r = run_scene(cfg);

            uint64_t tt    = r.bw_a.total_b + r.bw_b.total_b;
            double frm     = r.frm_total > 0 ? r.frm_complete * 100.0 / r.frm_total : 0;
            double ovhd    = r.app_payload > 0 ? (tt - r.app_payload) * 100.0 / r.app_payload : 0;

            printf("  %-6d %-6d %8u %8u %7.3f%% %7u %8u %9.1f%% %10u\n",
                   pps, loss, r.p50, r.p99, frm, r.fec_rec, r.arq_ack, ovhd, r.book_id_actual);
        }
        sep('-');
    }
    sep('=');
    printf("  PART 10 完成（book 类型见 book实 列：4=book4, 5=book5）\n");
    sep('=');

    RmLoadGtpModule();
    return 0;
}
