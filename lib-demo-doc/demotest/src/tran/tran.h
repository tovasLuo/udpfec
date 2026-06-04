#ifndef _TRAN_H_
#define _TRAN_H_

#include "macrodefine.h"
#include "goodtp.h"
#include "slidwin.h"
#include "cos.h"

#ifdef __linux__
#include <netdb.h>
#include <arpa/inet.h>
#endif

#if (_WIN32 || _WIN64)
#include <WS2tcpip.h>
#include <WinSock2.h>
#include <Windows.h>
#endif

#include <vector>
#include <unordered_map>
#include <string.h>
#include <string>

using namespace std;

#define SOCKET_ADDR_SIZE (((((u32)sizeof(struct sockaddr_in6)) & 0xFFFFFFFC) + 4))

typedef struct _StatResult {
    _StatResult(void);

    void Clear(void);

    template <typename T1, typename T2>
    void DoStat(const T1 data_vec[], const T2 &data_num);

    const _StatResult  operator +(const _StatResult &old) const;
    const _StatResult& operator +=(const _StatResult &old);
    const _StatResult  operator /(const u32 &n) const;
    const _StatResult& operator /=(const u32 &n);

    u32 max_value_;
    u32 min_value_;
    u32 avg_value_;
    u32 std_value_;
    f32 one_std_rate_;
    f32 two_std_rate_;
    f32 three_std_rate_;
    f32 other_std_rate_;
}StatResult;

#ifdef __cplusplus
extern "C" {
#endif
void AppReportQuality(slid_win_hdl win_hdl, void *cur_cntxt_hdl, const u32 &avg_rtt_us, const u32 &jitter_us,
                      const f32 &loss, const DiscardDirectU32 &discard_dir, const u32 &pre_congest_rank,
                      const u32 &rto_us, const u32 &avg_pps, const u32 &loss_num);
void AppBitMapReceive(slid_win_hdl win_hdl, void *cur_cntxt_hdl,const void *bit_map, const u32 &ack_or_nack);

typedef struct _ {
    u32 last_bitmap_header_sn_;
    u32 last_bitmap_tail_sn_;

    u32 bitmap_size_;
    u32 last_sn_;

    u32 pps_;
    f32 loss_;

    slid_win_hdl win_hdl_;

    u64 bitmap_[512];

    u8  slid_win_mem_[SIZE_512K];
    u8  win_cache_[2048];
}AppSlidWinInfo;

typedef struct _LinkerQuality {
    _LinkerQuality(const GtpLinkQuality &linker_quality) {
        app_quality_.win_hdl_               = NULL;
        app_quality_.last_bitmap_header_sn_ = 0;
        app_quality_.last_bitmap_tail_sn_   = 0;
        app_quality_.bitmap_size_           = 0;
        app_quality_.last_sn_               = 0;
        app_quality_.pps_                   = 0;
        app_quality_.loss_                  = 0.0;

        memset(app_quality_.bitmap_, 0x00, sizeof(app_quality_.bitmap_));
        memset(app_quality_.slid_win_mem_, 0x00, SIZE_512K);

        rtt_ms_             = linker_quality.rtt_ms_;
        rtt_jitter_ms_      = linker_quality.rtt_jitter_ms_;
        data_bitrate_bps_   = linker_quality.data_bitrate_bps_;
        net_bitrate_bps_    = linker_quality.net_bitrate_bps_;
        data_pack_pps_      = linker_quality.data_pack_pps_;
        net_pack_pps_       = linker_quality.net_pack_pps_;
        loss_               = linker_quality.loss_;
        pre_congest_rank_   = linker_quality.pre_congest_rank_;
        bitrage_chg_k_      = linker_quality.bitrage_chg_k_;
        loss_direct_        = linker_quality.loss_direct_;
        report_pos_         = linker_quality.report_pos_;
        total_frame_num_    = linker_quality.total_frame_num_;
        total_pack_num_     = linker_quality.total_pack_num_;
        total_ack_num_      = linker_quality.total_ack_num_;
        total_retran_num_   = linker_quality.total_retran_num_;
        total_ack_loss_num_ = linker_quality.total_ack_loss_num_;
        total_rto_loss_num_ = linker_quality.total_rto_loss_num_;
        total_ack_err_num_  = linker_quality.total_ack_err_num_;

        cos_date_time_stru date;
        cos_getCurSysTime(&date);
        last_active_ts_us_ = date.timestamp_us;

        app_quality_.win_hdl_ = CreateSlidWin(AppReportQuality, AppBitMapReceive, cos_accept3rdLogCallBack,
                                              cos_getPrintLogLevel, 0, date.timestamp_us, 100000, kRecvSlidWinMode,
                                              app_quality_.slid_win_mem_, &(this->app_quality_),
                                              (u8*)app_quality_.win_cache_, 1);
        
    }

    _LinkerQuality(const u32 &sn) {
        app_quality_.win_hdl_               = NULL;
        app_quality_.last_bitmap_header_sn_ = 0;
        app_quality_.last_bitmap_tail_sn_   = 0;
        app_quality_.bitmap_size_           = 0;
        app_quality_.pps_                   = 0;
        app_quality_.loss_                  = 0.0;
        app_quality_.last_sn_               = sn;

        memset(app_quality_.bitmap_, 0x00, sizeof(app_quality_.bitmap_));
        memset(app_quality_.slid_win_mem_, 0x00, SIZE_512K);

        rtt_ms_             = 0;
        rtt_jitter_ms_      = 0;
        data_bitrate_bps_   = 0;
        net_bitrate_bps_    = 0;
        data_pack_pps_      = 0;
        net_pack_pps_       = 0;
        loss_               = 0.0;
        pre_congest_rank_   = 0;
        bitrage_chg_k_      = 0;
        loss_direct_        = 0;
        report_pos_         = 0;
        total_frame_num_    = 0;
        total_pack_num_     = 0;
        total_ack_num_      = 0;
        total_retran_num_   = 0;
        total_ack_loss_num_ = 0;
        total_rto_loss_num_ = 0;
        total_ack_err_num_  = 0;

        cos_date_time_stru date;
        cos_getCurSysTime(&date);
        last_active_ts_us_ = date.timestamp_us;

        app_quality_.win_hdl_ = CreateSlidWin(AppReportQuality, AppBitMapReceive, cos_accept3rdLogCallBack,
                                              cos_getPrintLogLevel, sn & 0xFFFFFFC0, date.timestamp_us, 100000,
                                              kRecvSlidWinMode, app_quality_.slid_win_mem_, &(this->app_quality_),
                                              (u8*)app_quality_.win_cache_, 1);
    }

    _LinkerQuality(const _LinkerQuality &old) {
        app_quality_.win_hdl_               = NULL;
        app_quality_.last_bitmap_header_sn_ = old.app_quality_.last_bitmap_header_sn_;
        app_quality_.last_bitmap_tail_sn_   = old.app_quality_.last_bitmap_tail_sn_;
        app_quality_.bitmap_size_           = old.app_quality_.bitmap_size_;
        app_quality_.pps_                   = old.app_quality_.pps_;
        app_quality_.loss_                  = old.app_quality_.loss_;
        app_quality_.last_sn_               = old.app_quality_.last_sn_;

        memset(app_quality_.bitmap_, 0x00, sizeof(app_quality_.bitmap_));
        memset(app_quality_.slid_win_mem_, 0x00, SIZE_512K);

        rtt_ms_             = old.rtt_ms_;
        rtt_jitter_ms_      = old.rtt_jitter_ms_;
        data_bitrate_bps_   = old.data_bitrate_bps_;
        net_bitrate_bps_    = old.net_bitrate_bps_;
        data_pack_pps_      = old.data_pack_pps_;
        net_pack_pps_       = old.net_pack_pps_;
        loss_               = old.loss_;
        pre_congest_rank_   = old.pre_congest_rank_;
        bitrage_chg_k_      = old.bitrage_chg_k_;
        loss_direct_        = old.loss_direct_;
        report_pos_         = old.report_pos_;
        total_frame_num_    = old.total_frame_num_;
        total_pack_num_     = old.total_pack_num_;
        total_ack_num_      = old.total_ack_num_;
        total_retran_num_   = old.total_retran_num_;
        total_ack_loss_num_ = old.total_ack_loss_num_;
        total_rto_loss_num_ = old.total_rto_loss_num_;
        total_ack_err_num_  = old.total_ack_err_num_;
        last_active_ts_us_  = old.last_active_ts_us_;

        app_quality_.win_hdl_ = CreateSlidWin(AppReportQuality, AppBitMapReceive, cos_accept3rdLogCallBack,
                                              cos_getPrintLogLevel, old.app_quality_.last_sn_ & 0xFFFFFFC0,
                                              old.last_active_ts_us_, 100000, kRecvSlidWinMode,
                                              app_quality_.slid_win_mem_, &(this->app_quality_),
                                              (u8*)app_quality_.win_cache_, 1);
    }

    ~_LinkerQuality() {
        if (NULL != app_quality_.win_hdl_) {
            DeleteSlidWin(app_quality_.win_hdl_);
            app_quality_.win_hdl_ = NULL;
        }
    }

    const _LinkerQuality& operator =(const _LinkerQuality &a) {
        if (NULL != app_quality_.win_hdl_) {
            DeleteSlidWin(app_quality_.win_hdl_);
            app_quality_.win_hdl_ = NULL;
        }

        app_quality_.last_bitmap_header_sn_ = a.app_quality_.last_bitmap_header_sn_;
        app_quality_.last_bitmap_tail_sn_   = a.app_quality_.last_bitmap_tail_sn_;
        app_quality_.bitmap_size_           = a.app_quality_.bitmap_size_;
        app_quality_.pps_                   = a.app_quality_.pps_;
        app_quality_.loss_                  = a.app_quality_.loss_;
        app_quality_.last_sn_               = a.app_quality_.last_sn_;

        memset(app_quality_.bitmap_, 0x00, sizeof(app_quality_.bitmap_));
        memset(app_quality_.slid_win_mem_, 0x00, SIZE_512K);

        rtt_ms_             = a.rtt_ms_;
        rtt_jitter_ms_      = a.rtt_jitter_ms_;
        data_bitrate_bps_   = a.data_bitrate_bps_;
        net_bitrate_bps_    = a.net_bitrate_bps_;
        data_pack_pps_      = a.data_pack_pps_;
        net_pack_pps_       = a.net_pack_pps_;
        loss_               = a.loss_;
        pre_congest_rank_   = a.pre_congest_rank_;
        bitrage_chg_k_      = a.bitrage_chg_k_;
        loss_direct_        = a.loss_direct_;
        report_pos_         = a.report_pos_;
        total_frame_num_    = a.total_frame_num_;
        total_pack_num_     = a.total_pack_num_;
        total_ack_num_      = a.total_ack_num_;
        total_retran_num_   = a.total_retran_num_;
        total_ack_loss_num_ = a.total_ack_loss_num_;
        total_rto_loss_num_ = a.total_rto_loss_num_;
        total_ack_err_num_  = a.total_ack_err_num_;
        last_active_ts_us_  = a.last_active_ts_us_;

        app_quality_.win_hdl_ = CreateSlidWin(AppReportQuality, AppBitMapReceive, cos_accept3rdLogCallBack,
                                              cos_getPrintLogLevel, a.app_quality_.last_sn_ & 0xFFFFFFC0,
                                              a.last_active_ts_us_, 100000, kRecvSlidWinMode,
                                              app_quality_.slid_win_mem_, &(this->app_quality_),
                                              (u8*)app_quality_.win_cache_, 1);

        return *this;
    }

    u16 rtt_ms_;
    u16 rtt_jitter_ms_;
    u32 data_bitrate_bps_; // It includes fec restoring packets, and it's deleted repetitive packets.
    u32 net_bitrate_bps_;  // It doesn't include fec restoring packets, but it includes repetitive packets.
    u32 data_pack_pps_;    // It includes fec restoring packets, and it's deleted repetitive packets, it's from
                           // slid window's stat.
    u32 net_pack_pps_;     // It doesn't include fec restoring packets, but it includes repetitive packets, it's
                           // from net card.

    f32 loss_;
    u32 pre_congest_rank_; // GtpPreCongestRankU32
    f32 bitrage_chg_k_;
    u32 loss_direct_;      // GtpLossDirectEnU32

    u32 report_pos_;
    u32 rsv_;
    u64 last_active_ts_us_;

    u32 total_frame_num_;
    u32 total_pack_num_;
    u32 total_ack_num_;
    u32 total_retran_num_;
    u32 total_ack_loss_num_;
    u32 total_rto_loss_num_;
    u32 total_ack_err_num_;
    u32 total_ai_repair_num_;

    AppSlidWinInfo app_quality_;
}LinkerQuality;

typedef struct _LinkerKey {
    _LinkerKey(const GtpLinkQuality &linker_quality) {
        memset(self_ip_, 0x00, GTP_MAX_STR_IP_SZ);
        memset(peer_ip_, 0x00, GTP_MAX_STR_IP_SZ);

        strcpy((char*)self_ip_, (char*)(linker_quality.self_ip_));
        strcpy((char*)peer_ip_, (char*)(linker_quality.peer_ip_));

        memcpy(self_bin_ip_, linker_quality.self_bin_ip_, GTP_MAX_BIN_IP_SZ);
        memcpy(peer_bin_ip_, linker_quality.peer_bin_ip_, GTP_MAX_BIN_IP_SZ);

        self_port_     = linker_quality.self_port_;
        peer_port_     = linker_quality.peer_port_;
        self_bin_port_ = linker_quality.self_bin_port_;
        peer_bin_port_ = linker_quality.peer_bin_port_;

        key_   = (((((u64)self_bin_port_) << 16) | ((u64)peer_bin_port_)) & 0x00000000FFFFFFFF);
        key_ <<= 32;

        u32 *ptr = (u32*)self_bin_ip_;
        u32 loop = 0;
        while ((GTP_MAX_BIN_IP_SZ >> 2) > loop) {
            key_ += (*ptr);
            ptr  += 1;
            loop += 1;
        }

        ptr  = (u32*)peer_bin_ip_;
        loop = 0;
        while ((GTP_MAX_BIN_IP_SZ >> 2) > loop) {
            key_ += (*ptr);
            ptr  += 1;
            loop += 1;
        }

        if (kSenderQuality == linker_quality.report_pos_) {
            key_ &= 0x7FFFFFFFFFFFFFFF;
        } else {
            key_ |= 0x8000000000000000;
        }
    }

    _LinkerKey(u8 self_ip[GTP_MAX_STR_IP_SZ], u8 peer_ip[GTP_MAX_STR_IP_SZ],
               const u16 &self_port, const u16 &peer_port, const u32 &report_pos) {
        memcpy(self_ip_, self_ip, GTP_MAX_STR_IP_SZ);
        memcpy(peer_ip_, peer_ip, GTP_MAX_STR_IP_SZ);

        memset(self_bin_ip_, 0x00, GTP_MAX_BIN_IP_SZ);
        memset(peer_bin_ip_, 0x00, GTP_MAX_BIN_IP_SZ);

        self_bin_port_ = htons(self_port);
        peer_bin_port_ = htons(peer_port);
        self_port_     = self_port;
        peer_port_     = peer_port;

        u32 ip_size = GTP_MAX_BIN_IP_SZ;

        cos_stringIpToBinIp(&(self_ip[0]),&(self_bin_ip_[0]), &ip_size);
        cos_stringIpToBinIp(&(peer_ip[0]),&(peer_bin_ip_[0]), &ip_size);

        key_   = (((((u64)self_bin_port_) << 16) | ((u64)peer_bin_port_)) & 0x00000000FFFFFFFF);
        key_ <<= 32;

        u32 *ptr = (u32*)self_bin_ip_;
        u32 loop = 0;
        while ((GTP_MAX_BIN_IP_SZ >> 2) > loop) {
            key_ += (*ptr);
            ptr  += 1;
            loop += 1;
        }

        ptr  = (u32*)peer_bin_ip_;
        loop = 0;
        while ((GTP_MAX_BIN_IP_SZ >> 2) > loop) {
            key_ += (*ptr);
            ptr  += 1;
            loop += 1;
        }

        if (kSenderQuality == report_pos) {
            key_ &= 0x7FFFFFFFFFFFFFFF;
        } else {
            key_ |= 0x8000000000000000;
        }
    }

    _LinkerKey(const _LinkerKey &old) {
        memcpy(self_ip_, old.self_ip_, GTP_MAX_STR_IP_SZ);
        memcpy(peer_ip_, old.peer_ip_, GTP_MAX_STR_IP_SZ);
        memcpy(self_bin_ip_, old.self_bin_ip_, GTP_MAX_BIN_IP_SZ);
        memcpy(peer_bin_ip_, old.peer_bin_ip_, GTP_MAX_BIN_IP_SZ);

        self_port_     = old.self_port_;
        peer_port_     = old.peer_port_;
        self_bin_port_ = old.self_bin_port_;
        peer_bin_port_ = old.peer_bin_port_;

        key_ = old.key_;
    }

    const _LinkerKey& operator =(const _LinkerKey &old) {
        memcpy(self_ip_, old.self_ip_, GTP_MAX_STR_IP_SZ);
        memcpy(peer_ip_, old.peer_ip_, GTP_MAX_STR_IP_SZ);
        memcpy(self_bin_ip_, old.self_bin_ip_, GTP_MAX_BIN_IP_SZ);
        memcpy(peer_bin_ip_, old.peer_bin_ip_, GTP_MAX_BIN_IP_SZ);

        self_port_     = old.self_port_;
        peer_port_     = old.peer_port_;
        self_bin_port_ = old.self_bin_port_;
        peer_bin_port_ = old.peer_bin_port_;

        key_ = old.key_;

        return *this;
    }

    bool operator ==(const _LinkerKey &a) const {
        return ((key_ == a.key_) && (self_bin_port_ == a.self_bin_port_) && (peer_bin_port_ == a.peer_bin_port_)
             && (0 == memcmp(self_bin_ip_, a.self_bin_ip_, GTP_MAX_BIN_IP_SZ))
             && (0 == memcmp(peer_bin_ip_, a.peer_bin_ip_, GTP_MAX_BIN_IP_SZ)));
    }

    u8  self_bin_ip_[GTP_MAX_BIN_IP_SZ];
    u8  peer_bin_ip_[GTP_MAX_BIN_IP_SZ];
    
    u8  self_ip_[GTP_MAX_STR_IP_SZ];
    u8  peer_ip_[GTP_MAX_STR_IP_SZ];

    u16 self_bin_port_;
    u16 peer_bin_port_;

    u16 self_port_;
    u16 peer_port_;

    u64 key_;
}LinkerKey;

typedef struct _LinkerKeyHash {
    u64 operator ()(const LinkerKey &a) const {
        return a.key_;
    }
}LinkerKeyHash;

typedef struct _ConsumeTime {
    _ConsumeTime();
    void clear(void);
    void update(const u16 &delta_tm);
    void doStat(void);
    const StatResult& getStatResult(void) const;

    u16 used_time_us_[RCD_CONSUME_BUF_SIZE];   // max recording the 512 item, loop back recording when overflow.
    u16 current_record_pos_;
    u16 current_record_num_;

    StatResult stat_result_;
}ConsumeTime;

class TranEpoll {
public:
    TranEpoll(const rolerEnum &roler, const u32 &send_pps, const u32 &thread_id, const vector<string> &listen_ip,
              const u32 &start_port, const u32 &end_port, const string &send_ip, FILE *pf,
              const u32 &auto_show = COS_NO);
    ~TranEpoll();

    u32 Running(const u32 &type);
    u32 Running(void);
    void Stop(void);

    void SaveLinkerQuality(const GtpLinkQuality &linker_quality);
    u32  AppSnEntrySlidWin(const u32 &sn, GtpAddr *tran_addr);
    void PrintLinkerQuality(const u32 &bit_map_flag);
    void CheckResource(void);
    void ShowLinkerQuality(const u32 &pos);
    void ShowSessionBitMap(const u8 *src_ip, const u8 *dst_ip);
    void ShowFecParameter(const u8 *src_ip, const u8 *dst_ip);
    u32  CreateGtpInst(void);

    static u32 ReceiveEntry(void *param);
    static u32 MeasureSpeedReceiveEnry(void *param);
    static u32 FileTranReceiveEntry(void *param);

    u32 epoll_thread_enable_flag_;   // COS_NO: terminate thread. COS_YES: running thread;
    u32 epoll_thread_runned_flag_;   // COS_NO: thread hasn't be running,
    u32 epoll_thread_exited_flag_;   // COS_NO: thread hasn't been existed, COS_YES: thread has been existed.
    u32 thread_id_;

    u64 *goodtp_tran_failed_dot_;

    slid_win_hdl win_hdl_;

    ConsumeTime sock_send_consume_;
    ConsumeTime sock_recv_consume_;
    ConsumeTime goodtp_send_consume_;
    ConsumeTime goodtp_recv_consume_;
    ConsumeTime goodtp_timer_consume_;

    cos_date_time_stru current_date_;

    u32 consume_switch_;
    u32 measure_pack_num_;

    u32 win_cache_[2048];

    unordered_map<LinkerKey, LinkerQuality, LinkerKeyHash> linker_quality_map_;

    GtpHandler_p goodtp_hdl_;

    u32 application_sn_;

    u32 proced_frame_flag_;

    u32 app_entry_win_switch_;

    u32 rtt_ms_;
    u32 too_late_counter_;

    rolerEnum roler_;

    u32 state_machin_;

    u32 single_test_flag_;
    u32 receved_data_flag_;

    u32 max_delay_ms_;
    u32 min_delay_ms_;

    u32 loss_frame_sum_;
    u32 expect_recv_sn_;

private:
    #ifdef __linux__
    i32 epoll_id_;
    #endif

    #if (_WIN32 || _WIN64)
    fd_set epoll_id_;
    #endif
    
    u32 auto_show_;

    cos_sock send_sfd_;

    vector<cos_sock> listen_sfd_;
    vector<string> listen_ip_;
    u32 start_port_;
    u32 end_port_;

    string send_ip_;

    u64 *udp_receive_failed_dot_;
    u64 *gtp_buf_malloc_failed_dot_;
    u64 *goodtp_rcv_pack_err_dot_;
    u64 *goodtp_snd_frame_err_dot_;
    u64 *goodtp_timer_failed_dot_;
    u64 *socket_send_failed_dot_;

    u64 sending_frame_ts_us_;
    u64 send_period_ts_us_;

    u64 call_goodtp_timer_ts_us_;

    u32 send_pps_;
    u32 server_sock_addr_size_;
    u8  server_sock_addr_[SOCKET_ADDR_SIZE];

    u32 backup_server_sock_addr_size_;
    u8  backup_server_sock_addr_[SOCKET_ADDR_SIZE];

    u32* server_addr_size_;
    u8*  server_addr_;

    u64 switch_server_ip_ts_us_;

    u64 check_resource_period_ts_us_;

    FILE *bin_pf_;

    GtpCallBackParam reg_cb_;
    MemPoolConfig mem_pool_cfg_;

    u8 cache_64K_[SIZE_64K];

    u8 slid_win_mem_[SIZE_512K];
};

u32  InitTran(const rolerEnum &roler);
void RemoveTran(void);

#ifdef __cplusplus
}
#endif

#endif
