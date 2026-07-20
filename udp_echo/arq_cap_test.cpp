/*
 * arq_cap_test.cpp
 *
 * 验证：ARQ per-session节点数硬上限(MAX_ARQ_NODE_PER_SESSION，goodtp_arq.cpp)是否真的把
 * 单个session的未确认ARQ节点数钳死在上限，而不是无限增长。
 *
 * 复现场景：单个GoodTp实例，只发不收——send_pack_cb_直接丢弃（不真正投递），且全程不调用
 * PeriodGtpTimer()（不触发CheckRtoRetran()的RTO清理路径），单线程、无竞态、完全确定性地
 * 隔离出"peer完全静默不回包"这一种情况下admission-time硬上限本身的效果，不跟RTO超时清理
 * 或其它并发因素混在一起。
 *
 * 用kRealTimeStream（游戏模式默认）是关键：LinkQualityCallback()对realtime流的本地"down"
 * 方向粗估（未确认包比例，正是这个场景下唯一能算出来的信号，因为从来没收到过任何ACK/NACK）
 * 明确标记为不可信(trusted_policy_loss=NO)，不会触发alg_top_switch_关闭算法——这保证了
 * PacketEntryList()在整个测试期间都会被正常调用，不会被"链路太差整体关算法"这个独立机制
 * 提前掐断，测的就是硬上限本身。
 *
 * 用法：./arq_cap_test [发送包数，默认3000]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bitlinker.h"

typedef struct TestFrame {
    uint32_t magic;
    uint32_t seq;
    char     payload[64];
} TestFrame;

#define LOOPBACK_IP  "127.0.0.1"
#define SELF_PORT    19301
#define PEER_PORT    19302

/* 模拟"peer完全静默"：packet直接丢弃，不调用sendto，从不产生任何返回流量 */
static uint32_t SendPackCbDrop(GtpHandler_p hdl, void *pack, uint32_t size, GtpAddr *addr) {
    (void)hdl; (void)pack; (void)size; (void)addr;
    return GTP_OK;
}

static uint32_t RecvFrameCbNoop(GtpHandler_p hdl, void *frame, uint32_t size, GtpAddr *addr) {
    (void)hdl; (void)frame; (void)size; (void)addr;
    return GTP_OK;
}

static void LogCb(uint32_t level, const char *fmt, ...) {
    if (level > 4) return;   /* 放行到warning，好看到cap触发时的告警日志 */
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static uint32_t LogLevelCb(void) { return 3; /* kGtpLogLevelError -- cap eviction warnings are expected
                                                  and would otherwise flood stdout once per eviction */ }

static long extract_u32_field(const char *buf, const char *key) {
    const char *p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == '=' || *p == ' ') p++;
    return atol(p);
}

int main(int argc, char *argv[]) {
    int total_sends = 3000;
    if (argc >= 2) total_sends = atoi(argv[1]);

    if (GTP_OK != InsLoadGtpModule()) {
        fprintf(stderr, "InsLoadGtpModule failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in self_addr;
    memset(&self_addr, 0, sizeof(self_addr));
    self_addr.sin_family      = AF_INET;
    self_addr.sin_port        = htons(SELF_PORT);
    inet_pton(AF_INET, LOOPBACK_IP, &self_addr.sin_addr);
    if (bind(sfd, (struct sockaddr*)&self_addr, sizeof(self_addr)) < 0) {
        perror("bind"); return 1;
    }

    struct sockaddr_in peer_addr;
    memset(&peer_addr, 0, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port   = htons(PEER_PORT);
    inet_pton(AF_INET, LOOPBACK_IP, &peer_addr.sin_addr);

    GtpCallBackParam cb = {SendPackCbDrop, RecvFrameCbNoop, NULL, LogCb, LogLevelCb, NULL};

    MemPoolConfig mem_cfg;
    memset(&mem_cfg, 0, sizeof(mem_cfg));
    mem_cfg.m_256bytes_num_ = 4096;
    mem_cfg.m_512bytes_num_ = 4096;
    mem_cfg.m_1k_num_       = 512;
    mem_cfg.m_1_5k_num_     = 512;
    mem_cfg.m_4k_num_       = 128;
    mem_cfg.m_8k_num_       = 64;
    mem_cfg.m_64k_num_      = 16;

    GtpHandler_p hdl = CreateGtpInstance(1, 10000000, &cb, &mem_cfg);
    if (INVALID_GTP_HANDLER == hdl) {
        fprintf(stderr, "CreateGtpInstance failed: %s\n", LastGtpErrorInfo());
        return 1;
    }

    printf("=== arq_cap_test: %d sends, peer completely silent, no PeriodGtpTimer ===\n", total_sends);

    int ok_sends = 0;
    for (int i = 0; i < total_sends; ++i) {
        TestFrame f;
        f.magic = 0xDEADBEEF;
        f.seq   = (uint32_t)i;
        snprintf(f.payload, sizeof(f.payload), "seq=%d", i);

        uint32_t  mem_len = 0;
        GtpAddr  *taddr   = NULL;
        uint8_t  *mem = GtpMallocPackMem(hdl, NULL, 0, &mem_len, (void**)&taddr, sizeof(f));
        if (!mem) {
            fprintf(stderr, "GtpMallocPackMem failed at i=%d: %s\n", i, LastGtpErrorInfo());
            continue;
        }
        memcpy(mem, &f, sizeof(f));
        taddr->context_       = NULL;
        taddr->sfd_           = (uint32_t)sfd;
        taddr->stream_type_   = kRealTimeStream;
        taddr->enable_key_    = 0;
        taddr->self_addr_len_ = (uint32_t)sizeof(self_addr);
        memcpy(taddr->self_addr_, &self_addr, sizeof(self_addr));
        taddr->sock_addr_len_ = (uint32_t)sizeof(peer_addr);
        memcpy(taddr->sock_addr_, &peer_addr, sizeof(peer_addr));

        uint32_t ret = GtpFrameSend(hdl, mem, sizeof(f), taddr, 0, 0);
        if (GTP_OK == ret) {
            ok_sends += 1;
        } else {
            GtpFreePackMem(hdl, mem);
        }
    }

    printf("GtpFrameSend succeeded %d/%d times\n\n", ok_sends, total_sends);

    uint8_t alg_buf[4096] = {0};
    uint8_t self_ip[64], peer_ip[64];
    inet_ntop(AF_INET, &self_addr.sin_addr, (char*)self_ip, sizeof(self_ip));
    inet_ntop(AF_INET, &peer_addr.sin_addr, (char*)peer_ip, sizeof(peer_ip));
    GetAlgorithmParam(hdl, self_ip, peer_ip, alg_buf, sizeof(alg_buf));

    long harq_node_num   = extract_u32_field((const char*)alg_buf, "harq_node_num=");
    long cap_evict        = extract_u32_field((const char*)alg_buf, "cap_evict_counter=");
    long ack_err           = extract_u32_field((const char*)alg_buf, "ack_err_counter=");
    long rto_resend         = extract_u32_field((const char*)alg_buf, "rto_resend_counter=");

    uint8_t pool_buf[4096] = {0};
    GetPackMemPoolStatus(hdl, pool_buf, sizeof(pool_buf));

    printf("harq_node_num(session的arq_list_.node_num_) = %ld\n", harq_node_num);
    printf("cap_evict_counter(触发硬上限驱逐次数)         = %ld\n", cap_evict);
    printf("ack_err_counter(RTO超时彻底失败)              = %ld  (期望0, 因为没调PeriodGtpTimer)\n", ack_err);
    printf("rto_resend_counter(RTO重传)                   = %ld  (期望0, 同上)\n", rto_resend);
    printf("\n--- pack memory pool status ---\n%s\n", pool_buf);

    printf("========== 判定 ==========\n");
    int pass = 1;

    /* 核心断言：无论发了多少个包，单session的ARQ在途节点数绝不超过硬上限。这是本次修复
     * 要验证的核心不变量——改动前这里会随total_sends线性增长，没有上限。 */
    const long kExpectedCap = 1024;   /* 需要和goodtp_arq.cpp里的MAX_ARQ_NODE_PER_SESSION保持一致 */
    if (harq_node_num > kExpectedCap) {
        printf("[FAIL] harq_node_num=%ld 超过预期硬上限%ld，per-session cap没有生效！\n",
               harq_node_num, kExpectedCap);
        pass = 0;
    } else {
        printf("[PASS] harq_node_num=%ld 未超过硬上限%ld\n", harq_node_num, kExpectedCap);
    }

    if (total_sends > (int)kExpectedCap) {
        if (0 >= cap_evict) {
            printf("[FAIL] 发送量(%d)已远超硬上限(%ld)，但cap_evict_counter=%ld，说明驱逐机制根本没触发\n",
                   total_sends, kExpectedCap, cap_evict);
            pass = 0;
        } else {
            printf("[PASS] cap_evict_counter=%ld > 0，驱逐机制确实被触发\n", cap_evict);
        }

        if (harq_node_num != kExpectedCap) {
            printf("[FAIL] 发送量远超硬上限时，稳态node_num应该正好钉在%ld，实际=%ld\n",
                   kExpectedCap, harq_node_num);
            pass = 0;
        } else {
            printf("[PASS] 稳态node_num正好钉在硬上限%ld\n", kExpectedCap);
        }
    }

    /* 确认这条路径没有跟RTO清理混在一起干扰判断（没调PeriodGtpTimer，RTO相关计数应为0） */
    if ((0 != ack_err) || (0 != rto_resend)) {
        printf("[FAIL] ack_err_counter/rto_resend_counter应为0（未调用PeriodGtpTimer），实际=%ld/%ld"\
               "，说明观测到的node_num上限可能是RTO清理导致而非cap机制\n", ack_err, rto_resend);
        pass = 0;
    } else {
        printf("[PASS] ack_err_counter=rto_resend_counter=0，确认硬上限效果与RTO清理无关，是cap机制独立生效\n");
    }

    printf("\n整体：%s\n", pass ? "PASS" : "FAIL");

    DeleteGtpInstance(hdl);
    close(sfd);
    RmLoadGtpModule();
    return pass ? 0 : 1;
}
