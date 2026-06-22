#ifndef _GOOD_TP_SESSION_H_
#define _GOOD_TP_SESSION_H_
/*********************************************************************************************************************

                                  Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_session.h
  Version  : Initial
  Author   : Albort.Feng
  Function : Realizes the linker session function header file.
  Modify record:
  1.Date   : August 28, 2023
    Author : Albort.Feng
    Content: New creates file.

*********************************************************************************************************************/
#include "goodtp_arq.h"
#include "goodtp_fec2.h"
#include "goodtp_factorcalulation.h"
#include "goodtp_reorder_window.h"
#include "goodtp_mem_pool.h"
#include "goodtp_comstruct.h"
#include "goodtp_macrodefine.h"
#include "goodtp.h"
#include "slidwin.h"

#include "tranmempool.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(4)

#pragma pack()

class GtpSession {
 public:
    GtpSession(const goodtp_sock &sfd, const u8 dst_sock_addr[], const u32 &dst_sock_addr_size,
               const u8 slf_sock_addr[], const u32 &slf_sock_addr_size, const u8 &stream_type, const u8 &smooth_jiter,
               const GtpHandler &gtp_hdl, GtpMemPool *arq_node_mem_pool, const GtpCallBackParam &cb, const u64 &ts_us,
               const u64 &ttl, TranMemPool &pack_mem_pool, GtpHandler_p app_gtp_hdl, const GtpFec2Mode *fec_code_book,
               void *context = NULL, const u32 &mode = (u32)(GtpSessionMode::kSender));

    GtpSession(GtpAddr *tran_addr, const GtpHandler &gtp_hdl, GtpMemPool *arq_node_mem_pool, const GtpCallBackParam &cb,
               const u64 &ts_us, const u64 &ttl, TranMemPool &pack_mem_pool, GtpHandler_p app_gtp_hdl,
               const GtpFec2Mode *fec_code_book, const u32 &mode = (u32)(GtpSessionMode::kSender));

    ~GtpSession();

    u32 Init(const u64 &ts_us, u8 *win_cache, const u32 &pack_sn = 0, const u32 &sort_sn = 0);

    static void* operator new(size_t n, void *psp_mem);
    static void operator delete(void *psp_mem, void *placement_mem);
    static void operator delete(void *psp_mem);

    static u32 PackRetransmit(void *session, ArqNode *arq_node, GtpAddr *tran_addr, const u64 &cur_ts_us);

    static u32 Fec2RestoreFrameReceive(void *session, GtpHandler gtp_hdl, GtpPacket *pack, GtpAddr *tran_addr,
                                       u32 *fec_res_sucess_stat, u32 *fec_res_failed_stat, u32 *fec_res_repeat_stat);

    static void ChgForceFec2Flag(void *session, const u32 &flag);
    
    u32 FramePrepHandler(void *frame, const u32 &frame_size, GtpPacket **out_pack, u32 *out_pack_size,
                    const u32 &hash, const u32 &user_id, const u32 &first_pack_sn = 0, const u32 &resend_num = 0);

    u32 FramePrepHandlerWithSelfVer(void *frame, const u32 &frame_size, GtpPacket **out_pack, u32 *out_pack_size,
                    const u32 &hash, const u32 &user_id, const u32 &first_pack_sn = 0, const u32 &resend_num = 0);

    u32 FramePrepHandlerWithPeerVer(void *frame, const u32 &frame_size, GtpPacket **out_pack, u32 *out_pack_size,
                    const u32 &hash, const u32 &user_id, const u32 &first_pack_sn = 0, const u32 &resend_num = 0);

    u32 FramePrepHandlerWith01(void *frame, const u32 &frame_size, GtpPacket **out_pack, u32 *out_pack_size,
                    const u32 &hash, const u32 &user_id, const u32 &first_pack_sn = 0, const u32 &resend_num = 0);

    u32 FramePostHandler(GtpAddr *tran_addr, GtpPacket *pack, const u32 &pack_size, const u32 &payload_size,
                       const u32 &entry_arq_flag, const u32 &first_pack_sn = 0,
                       const u32 &edge_pack_flag = GTP_YES, const u32 &fec_encoded_flag = GTP_NO);
    u32 FrameFecEncodeHandler(GtpPacket *pack);

    u32 PackPrepHandler(GtpPacket *pack, const u32 &size, u8 **out_frame, u32 *out_frame_size);
    u32 PackPostHandler(GtpPacket *pack, const u32 &size, GtpAddr *tran_addr,
                        const u32 &edge_pack_flag = GTP_YES);
    u32 DeliverFrameInOrder(GtpHandler_p gtp_hdl, const u32 &first_sn, const u8 *frame, const u32 &frame_size,
                            GtpAddr *tran_addr);
    u32 FlushRealtimeReorder(const u64 &ts_us, GtpHandler_p gtp_hdl);

    void TimerHandler(const u64 &ts_us, ConsumeTime *wheel_consume = NULL);
    void SetSessionStableTimeSize(const u64 &session_ttl_us);

    u32 ProcAckSnBitMap(const u8 ack_sn_bitmap[], const u32 &bitmap_sz, const u32 &head_sn,
                        const u32 &tail_sn, const u32 &rto_sn);

    void SecondTimerHandler(const u64 &cur_ts_us, ConsumeTime *wheel_consume = NULL);
    void AddNetStat(GtpPacket *pack, const u32 &pack_size);
    void CalcSessionQualityBfDead(const u64 &cur_ts_us);

    u32 GetQuality(const GtpNetQualityPosEnU32 &pos, GtpLinkQuality *quality);
    u32 WinBitMap(u8 *out_str, const u32 &mem_size, u32 *str_size);
    u32 PrintAlgorithmParam(u8 *out_str, const u32 &mem_size);

    inline NetQuality GetNetStatus(void) {
        return ((NetQuality)(net_quality_));
    }

    inline u32 GetSendBusinessPps(void) const {
        return (0 != pb_dt_.send_stat_.first_frame_pps_)
            ? pb_dt_.send_stat_.first_frame_pps_
            : pb_dt_.send_stat_.data_pack_pps_;
    }

    inline u8 GetStreamQos(const GtpNetQualityPosEnU32 &pos = kReceiverQuality) {
        #if (1 == AUTO_FEC_MACRO)
        if (kReceiverQuality == pos) {
            return STREAM_QOS_WITH_FEC;
        }

        #if (2 == APPLICATION_TYPE)
        if (TURN_OFF_FEC == CalcGameFecQos()) {
            return TURN_OFF_FEC;
        }
        return STREAM_QOS_WITH_FEC;
        #endif

        if (GTP_NO == rpt_snd_qualit_) {
            return STREAM_QOS_WITH_FEC;
        }

        #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
        if (500 > pb_dt_.send_stat_.data_pack_pps_) {
            return STREAM_QOS_WITH_FEC;  // SD vedio.
        }
        #endif

        if (((u8)(NetQuality::kNetQualityGood)) == net_quality_) {
            return TURN_OFF_FEC;
        }

        #endif

        return STREAM_QOS_WITH_FEC;
    }

    u32 AppendSessionAtrribute(u8 *out_str, const u32 &mem_size, u32 *str_size);

    void CalcMaxFramePeriod(const u64 &now_ts);
    u32 CalcLossStd(const u32 &loss, u32 *out_avg_loss);
    u8  CalcGameFecPolicy(void) const;
    u8  CalcGameFecBookId(void) const;
    u8  CalcGameFecQos(void) const;

    void UpdateSelfIp(const goodtp_sock &sfd);
    void UpdateSelfIp(u8 sock_addr[], const u32 &sock_addr_len);
    void UpdatePeerIp(u8 sock_addr[], const u32 &sock_addr_len);

PRIVATE:
    void SendSetRecvRttPacket(void);
    void SendRttTestResPacket(const u64 &ts_us);
    void ResetSession(const u32 &cur_sn, const u32 &sort_sn, const u32 &chg_status_flag);
    void RttHandler(const u32 &rtt_us);
    u32  CalcHeaderSize(const u32 &hash, const u32 &user_id, const u32 &resend_num);
    u32  CalcHeaderSizeVersion01(const u32 &hash, const u32 &user_id, const u32 &resend_num);
    u32  PrintHarqParam(u8 *out_str, const u32 &mem_size);
    u32  DeliverFrameNow(GtpHandler_p gtp_hdl, const u8 *frame, const u32 &frame_size, GtpAddr *tran_addr);
    u32  CalcRealtimeReorderWaitUs() const;
    u32  CalcRealtimeReorderMaxCacheNum() const;

 public:
    const GtpCallBackParam &cb_;

    u64 create_ts_us_;
    u64 last_active_ts_us_;
    u64 measure_rtt_ts_us_;
    u64 gen_new_rtt_ts_us_;
    u64 com_cache_ts_us_;
    u64 at_com_cache_ts_us_;
    u64 recv_idle_calc_loss_ts_us;
    u64 recv_idle_calc_period_us_;
    u64 recv_last_sync_ts_us_;
    u64 last_send_ts_us_;
    u64 last_gen_loss_ts_us_;
    u64 last_feedback_nack_ts_us_;
    u64 last_recv_data_pack_ts_us_;
    u32 recv_max_data_sn_;
    u32 rmv_close_alg_ts_us_;
    u32 rmv_close_alg_period_us_;
    u8 new_gap_detected_;

    slid_win_hdl data_win_s_;
    slid_win_hdl data_win_r_;
    slid_win_hdl filter_win_;   // filt the repeat packet for receiving slid window.

    FactorCalculation fac_;

    SessionPublicData pb_dt_;

    u32 last_sn_;
    u32 sort_sn_;

    GtpHandler_p app_gtp_hdl_;

    GtpArq arq_;

    GtpFec2 fec2_obj_;

    u8 self_bin_ip_[GTP_MAX_BIN_IP_SZ];
    u8 peer_bin_ip_[GTP_MAX_BIN_IP_SZ];

    u16 self_bin_port_;   // network sequence.
    u16 peer_bin_port_;   // network sequence.

    u32 ai_learn_sn_delta_;

    u32 ack_sn_;
    u32 min_session_stability_us_;

    u32 report_quality_flag_s_;
    f32 send_down_loss_;

    u32 send_session_stat_:4;  // GtpSessStat
    u32 recv_session_stat_:4;  // GtpSessStat
    u32 mode_:2;               // GtpSessionMode
    u32 self_ip_family_:1;     // 0:ipv4, 1:ipv6
    u32 peer_ip_family_:1;     // 0:ipv4, 1:ipv6
    u32 cache_loss_ovt_:1;     // 0:the cache_loss_ isn't overturn, 1:the cache_loss_ has been overturned.
    u32 max_frm_prd_updt_flg_:1;
    u32 net_quality_:2;
    u32 rpt_snd_qualit_:1;     // GTP_NO: hasn't reported sending quality, GTP_YES: has reported sending quality.
    u32 session_health_:3;
    u32 bit_rsv_:4;
    u32 cur_cache_loss_idx_:8;

    u32 test_rtt_period_us_;

    u32 loss_sum_;
    u32 self_session_ttl_us_;

    u32 cache_loss_[MAX_CACHE_LOSS_NUM];

    TranMemPool &pack_mem_pool_;

    u32 pack_sn_;

    RealtimeReorderWindow realtime_reorder_win_;

PRIVATE:
    u32 rtt_us_;

    u32 max_peak_frame_period_us_;
    u32 slid_win_size_;

    u64 second_ts_us_;

    u32 next_calc_factor1_ts_us_;
    u32 restore_factor1_tm_span_us_;

    u32 last_max_frame_period_us_;
    u32 now_max_frame_period_us_;

    u8 *win_mem_s_;
    u8 *win_mem_r_;
    u8 *filter_mem_;

    GtpMemPool *arq_node_mem_pool_;
};

#ifdef __cplusplus
}
#endif
#endif
