/*********************************************************************************************************************

                               Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_session.cpp
  Version  : Initial
  Author   : Albort.Feng
  Function : Realizes the linker session function code file.
  Modify record:
  1.Date   : August 28, 2023
    Author : Albort.Feng
    Content: New creates file.

*********************************************************************************************************************/
#include "goodtp_session.h"
#include "goodtp_mgr.h"
#include "goodtp_comstruct.h"
#include "goodtp_macrodefine.h"
#include "goodtp.h"

#include <stdlib.h>
#include <stdio.h>
#include <cerrno>
#include <memory.h>
#include <string.h>
#include <string>

#if (_WIN32 || _WIN64)
#include <WS2tcpip.h>
#include <WinSock2.h>
#include <Windows.h>
#endif

#if (__linux__ || __APPLE__)
#include <netdb.h>
#include <arpa/inet.h>

#ifdef __linux__
#include <linux/tcp.h>
#endif

#ifdef __APPLE__
#include <sys/socket.h>
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline u8* GtpAlignSessionMem(u8 *ptr) {
    const size_t align_size = sizeof(void*);
    const size_t addr       = (size_t)ptr;
    return (u8*)((addr + align_size - 1) & (~(align_size - 1)));
}

#ifdef _SELFDEBUG
enum GtpFeedbackDebugReason {
    kGtpFeedbackDebugUnknown = 0,
    kGtpFeedbackDebugDataEntry,
    kGtpFeedbackDebugFecRestore,
    kGtpFeedbackDebugGapTimer,
    kGtpFeedbackDebugIdleTimer,
    kGtpFeedbackDebugSenderTimer,
    kGtpFeedbackDebugAckBitmap
};

static inline const char* GtpFeedbackDebugReasonName(const u8 &reason) {
    switch (reason) {
        case kGtpFeedbackDebugDataEntry:
            return "data_entry";
        case kGtpFeedbackDebugFecRestore:
            return "fec_restore";
        case kGtpFeedbackDebugGapTimer:
            return "gap_timer";
        case kGtpFeedbackDebugIdleTimer:
            return "idle_timer";
        case kGtpFeedbackDebugSenderTimer:
            return "sender_timer";
        case kGtpFeedbackDebugAckBitmap:
            return "ack_bitmap";
        default:
            return "unknown";
    }
}

static inline const char* GtpPackTypeName(const u8 &pack_type) {
    switch (pack_type) {
        case ((u8)(GtpPackType::kGtpDataPackType)):
            return "DATA";
        case ((u8)(GtpPackType::kGtpAckPackType)):
            return "ACK";
        case ((u8)(GtpPackType::kGtpFecPackType)):
            return "FEC";
        case ((u8)(GtpPackType::kGtpSetRecvRttPackType)):
            return "RTT_SET";
        case ((u8)(GtpPackType::kGtpRttTstResPackType)):
            return "RTT_RES";
        case ((u8)(GtpPackType::kGtpNackPackType)):
            return "NACK";
        default:
            return "UNKNOWN";
    }
}
#endif

typedef struct _FecMlParam {
    u8 mode_;
    u8 h_flag_;
    u8 v_flag_;
    u8 uphill_flag_;
    u8 downhill_flag_;
    u8 h_power_;
    u8 v_power_;
    u8 rsv_;
}FecMlParam;

#if (2 == APPLICATION_TYPE)
C3PTP_STATIC u32 CalcGameFecTableLossIndex(const f32 &loss) {
    static const f32 loss_std[] = {
        2.0f, 10.0f, 25.0f, 50.0f, 100.0f
    };

    for (u32 i = 0; (sizeof(loss_std) / sizeof(loss_std[0])) > i; ++i) {
        if (loss_std[i] > loss) {
            return i;
        }
    }
    return (u32)(sizeof(loss_std) / sizeof(loss_std[0]));
}

C3PTP_STATIC u32 CalcGameFecTablePpsIndex(const u32 &pps) {
    static const u32 pps_std[] = {
        20, 50, 80, 110, 140, 170
    };

    for (u32 i = 0; (sizeof(pps_std) / sizeof(pps_std[0])) > i; ++i) {
        if (pps_std[i] > pps) {
            return i;
        }
    }
    return (u32)(sizeof(pps_std) / sizeof(pps_std[0]));
}
#endif

void LinkQualityCallback(slid_win_hdl win_hdl, void *cntxt_hdl, const u32 &rtt_us, const u32 &jitter_us,
    const f32 &loss, const DiscardDirectU32 &loss_dir, const u32 &pre_congest_rank, const u32 &rto_us,
    const u32 &pps, const u32 &loss_num) {
    GtpSession *session = static_cast<GtpSession *>(cntxt_hdl);

    if (win_hdl == session->filter_win_) {
        return;
    }

    u8 tmp_buf[2048];

    GtpLinkQuality &link_quality = (*((GtpLinkQuality*)tmp_buf));

    link_quality.rsv_ = 0;

    if (win_hdl == session->data_win_s_) {
        session->rpt_snd_qualit_ = GTP_YES;

        u16 line_pos  = 0;
        u16 row_pos   = 0;
        u32 first_bad = GTP_NO;

        f32 max_loss_thresheld = (f32)MAX_REALTIME_LOSS_THRESHLD;
        f32 rmv_loss_thresheld = (f32)RMV_REALTIME_LOSS_THRESHLD;

        if ((MIN_SESSION_STABLE_TIME_US << 1) > (session->last_active_ts_us_ - session->create_ts_us_)) {
            session->report_quality_flag_s_ = GTP_YES;
            return;
        }

        if (kReliableStream == session->pb_dt_.tran_addr_.stream_type_) {
            max_loss_thresheld = (f32)MAX_RELIABLE_LOSS_THRESHLD;
            rmv_loss_thresheld = (f32)RMV_RELIABLE_LOSS_THRESHLD;
        }

        if (max_loss_thresheld <= loss) {
            if (GTP_OFF != session->pb_dt_.alg_top_switch_) {
                session->pb_dt_.alg_top_switch_ = GTP_OFF;

                GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "%s:%u<-->%s:%u(key=%llu) "\
                       "turn off algorithm(stream_type=%s std_loss=%5.2f%% cur_loss=%5.2f%% dir=%s)\r\n",
                       session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_),
                       session->pb_dt_.peer_ip_, (u32)(session->pb_dt_.peer_port_),
                       (unsigned long long)(session->pb_dt_.tran_addr_.stream_key_),
                       ((kRealTimeStream == session->pb_dt_.tran_addr_.stream_type_) ? "real_time" : "reliable"),
                       max_loss_thresheld, loss, ((kUpLinkerLoss == loss_dir) ? "up" : "down"));
            }

            // Base cooldown 400-660ms (was 1500-2548ms): a wifi micro-stall typically clears
            // in a few hundred ms, and the real re-enable gate below now also requires an
            // actually-observed low-loss window (rmv_loss_thresheld), so this floor only needs
            // to absorb single-window noise, not stand in as the sole safety margin.
            session->rmv_close_alg_period_us_ = 400000 + (session->last_active_ts_us_ & 0x0003FFFF);
            session->rmv_close_alg_ts_us_     = ((u32)(session->last_active_ts_us_ & 0x00000000FFFFFFFF))
                                              + session->rmv_close_alg_period_us_;

            goto proc_sender_network_quality_pos_;
        }

        if (GTP_ON == session->pb_dt_.alg_top_switch_) {
            goto proc_sender_network_quality_pos_;
        }

        // Only extend the timer if loss is still at or above the shutdown threshold.
        // In the 15-25% range let the timer expire naturally so the algorithm can re-enable.
        if (max_loss_thresheld <= loss) {
            session->rmv_close_alg_ts_us_ = ((u32)(session->last_active_ts_us_ & 0x00000000FFFFFFFF))
                                          + session->rmv_close_alg_period_us_;
            goto proc_sender_network_quality_pos_;
        }

        // Re-enable needs both: cooldown elapsed AND the current window actually back under
        // rmv_loss_thresheld. Previously this only checked the timer, so a link still sitting
        // at 15-25% loss right when the timer expired would get re-enabled anyway; now a
        // genuinely-still-bad link keeps getting re-evaluated instead of flapping back on early.
        if ((session->rmv_close_alg_ts_us_ > ((u32)(session->last_active_ts_us_ & 0x00000000FFFFFFFF)))
         || (rmv_loss_thresheld <= loss)) {
            goto proc_sender_network_quality_pos_;
        }

        session->pb_dt_.alg_top_switch_ = GTP_ON;

        GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "%s:%u<-->%s:%u(key=%llu) turn on "\
               "algorithm(stream_type=%s std_loss=%5.2f%% cur_loss=%5.2f%% dir=%s)\r\n",
               session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_),
               session->pb_dt_.peer_ip_, (u32)(session->pb_dt_.peer_port_),
               (unsigned long long)(session->pb_dt_.tran_addr_.stream_key_),
               ((kRealTimeStream == session->pb_dt_.tran_addr_.stream_type_) ? "real_time" : "reliable"),
               rmv_loss_thresheld, loss, ((kUpLinkerLoss == loss_dir) ? "up" : "down"));

proc_sender_network_quality_pos_:
        session->pb_dt_.UpdatedMaxLoss(loss);

        if (FLOAT_ZERO >= session->pb_dt_.max_send_loss_per_s_) {
            if (((u8)(NetQuality::kNetQualityBad)) == session->net_quality_) {
                if ((session->last_gen_loss_ts_us_ + MIN_ZERO_LOSS_EXIST_US) <= session->last_active_ts_us_) {
                    session->net_quality_          = ((u8)(NetQuality::kNetQualityGood));
                    session->arq_.max_boost_times_ = 0;
                    session->arq_.boost_switch_    = GTP_OFF;

                    #if (1 == ENABLE_FEC)
                    #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
                    u32 fec2_encode_book_id[] = {
                        4,  // pps <  150
                        4,  // pps <  300
                        4,  // pps <  500  SD vedio
                        1,  // pps <  1200 HD vedio
                        1   // pps >= 1200 2K/4K/8K
                    };

                    u32 std_pps[] = {150, 300, 500, 1200};
                    #endif

                    #if (2 == APPLICATION_TYPE)
                    session->fec2_obj_.ChangeFecMode(session->CalcGameFecBookId());
                    #else
                    if (std_pps[0] > session->pb_dt_.send_stat_.data_pack_pps_) {
                        line_pos = 0;
                        goto fec_mode_to_default_pos_;
                    }

                    if (std_pps[1] > session->pb_dt_.send_stat_.data_pack_pps_) {
                        line_pos = 1;
                        goto fec_mode_to_default_pos_;
                    }

                    if (std_pps[2] > session->pb_dt_.send_stat_.data_pack_pps_) {
                        line_pos = 2;
                        goto fec_mode_to_default_pos_;
                    }

                    if (std_pps[3] > session->pb_dt_.send_stat_.data_pack_pps_) {
                        line_pos = 3;
                        goto fec_mode_to_default_pos_;
                    }
                    line_pos = 4;

fec_mode_to_default_pos_:
                    session->fec2_obj_.ChangeFecMode(fec2_encode_book_id[line_pos]);
                    #endif
                    #endif
                }
            } else {
                if (GTP_ON == session->arq_.boost_switch_) {
                    session->arq_.max_boost_times_ = 0;
                    session->arq_.boost_switch_    = GTP_OFF;
                }
            }

            goto sender_qualiti_proc_start_pos_;
        }

        if (((u8)(NetQuality::kNetQualityGood)) == session->net_quality_) {
            first_bad             = GTP_YES;
            session->net_quality_ = ((u8)(NetQuality::kNetQualityBad));
        }

        if (GTP_ON != session->arq_.boost_switch_) {
            session->arq_.boost_switch_ = GTP_ON;
        }

        session->last_gen_loss_ts_us_ = session->last_active_ts_us_;

        {
        #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
        // boost number, continue discard packet.
        u32 boost_alg_param[][6] = {
            // loss < 1% loss < 10% loss < 20% loss < 35% loss < 50% loss >= 50%
            {1,          1,         2,         3,         2,         2},  // pps < 10    test speed
            {0,          0,         1,         1,         1,         1},  // pps < 500   SD vedio
            {0,          0,         1,         1,         1,         1},  // pps < 1200  HD vedio
            {0,          0,         1,         1,         1,         1},  // pps < 2000  2K vedio
            {0,          0,         0,         1,         1,         1},  // pps < 5000  4K vedio
            {0,          0,         0,         1,         1,         1},  // pps >= 5000 8K vedio
        };

        u32 std_pps[][5] = {
            {10, 500, 1200, 2000, 5000},  // realtime stream.
            {10, 250, 1200, 2000, 5000}   // reliable stream.
        };
        #endif

        #if (2 == APPLICATION_TYPE)
        // boost number, continue discard packet.
        // col1 (2-10% loss): 1 for all pps — book4 covers burst; ARQ boost adds proactive clone on backlog.
        u32 boost_alg_param[][6] = {
            // loss < 1% loss < 10% loss < 20% loss < 35% loss < 50% loss >= 50%
            {0,          1,         2,         0,         0,         0},  // pps < 12  low-pps game
            {0,          1,         1,         0,         0,         0},  // pps < 30  phone game
            {0,          1,         1,         0,         0,         0},  // pps < 50  phone game
            {0,          1,         1,         0,         0,         0},  // pps < 90  low-rate pc game
            {0,          1,         1,         0,         0,         0},  // pps < 130 low-rate pc game
            {0,          1,         1,         0,         0,         0},  // pps < 170 medium-rate pc game
            {0,          1,         1,         0,         0,         0},  // pps < 210 medium-rate pc game
            {0,          1,         1,         0,         0,         0},  // pps < 250 high-tail-latency pc game
            {0,          1,         1,         0,         0,         0},  // pps < 330 high-rate pc game
            {0,          1,         1,         0,         0,         0},  // pps >= 330 high-rate pc game
        };

        u32 boost_std_pps[][9] = {
            {12, 30, 50, 90, 130, 170, 210, 250, 330},  // realtime stream.
            {12, 30, 50, 90, 130, 170, 210, 250, 330}   // reliable stream.
        };

        u32 std_pps[][5] = {
            {12, 30, 50, 170, 250},  // realtime stream.
            {12, 30, 50, 170, 250}   // reliable stream.
        };
        #endif

        u32 std_pps_line = 0;

        if ((kReliableStream == session->pb_dt_.tran_addr_.stream_type_) && (0x01 < session->pb_dt_.peer_version_)) {
            std_pps_line = 1;
        }

        #if (2 == APPLICATION_TYPE)
        for (line_pos = 0;
             (sizeof(boost_std_pps[std_pps_line]) / sizeof(boost_std_pps[std_pps_line][0])) > line_pos;
             ++line_pos) {
            if (boost_std_pps[std_pps_line][line_pos] > session->pb_dt_.send_stat_.data_pack_pps_) {
                goto boost_alg_row_start_pos_;
            }
        }
        #else
        if (std_pps[std_pps_line][0] > session->pb_dt_.send_stat_.data_pack_pps_) {
            line_pos = 0;
            goto boost_alg_row_start_pos_;
        }

        if (std_pps[std_pps_line][1] > session->pb_dt_.send_stat_.data_pack_pps_) {
            line_pos = 1;
            goto boost_alg_row_start_pos_;
        }

        if (std_pps[std_pps_line][2] > session->pb_dt_.send_stat_.data_pack_pps_) {
            line_pos = 2;
            goto boost_alg_row_start_pos_;
        }

        if (std_pps[std_pps_line][3] > session->pb_dt_.send_stat_.data_pack_pps_) {
            line_pos = 3;
            goto boost_alg_row_start_pos_;
        }

        if (std_pps[std_pps_line][4] > session->pb_dt_.send_stat_.data_pack_pps_) {
            line_pos = 4;
            goto boost_alg_row_start_pos_;
        }
        line_pos = 5;
        #endif

boost_alg_row_start_pos_:
        // delta_loss=0.5%
        if (1.500001 > session->pb_dt_.max_send_loss_per_s_) {
            row_pos = 0;
            goto boost_alg_row_end_pos_;
        }

        // delta_loss=2.0%
        if (12.000001 > session->pb_dt_.max_send_loss_per_s_) {
            row_pos = 1;
            goto boost_alg_row_end_pos_;
        }

        // delta_loss=3.0%
        if (23.000001 > session->pb_dt_.max_send_loss_per_s_) {
            row_pos = 2;
            goto boost_alg_row_end_pos_;
        }

        // delta_loss=3.0%
        if (38.000001 > session->pb_dt_.max_send_loss_per_s_) {
            row_pos = 3;
            goto boost_alg_row_end_pos_;
        }

        // delta_loss=4.0%
        if (54.000001 > session->pb_dt_.max_send_loss_per_s_) {
            row_pos = 4;
            goto boost_alg_row_end_pos_;
        }

        row_pos = 5;

boost_alg_row_end_pos_:
        session->arq_.max_boost_times_ = (u8)boost_alg_param[line_pos][row_pos];
        if (GTP_YES == first_bad) {
            session->arq_.max_boost_times_ += 1;
        }

        // adjust fec algorithm parameter.
        line_pos = 0;
        row_pos  = 0;

        #if (1 == ENABLE_FEC)
        #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
        // mode,h_flag,v_flag, uphill_flag, downhill_flag, h_power, v_power
        u32 fec2_ml_book_id1[][4] = {
            // loss < 2.0%  loss < 10.0%  loss < 50.0%  loss >= 50.0%
            {4,             4,            4,            4},  // pps <  10   test speed
            {5,             4,            4,            4},  // pps <  500  SD vedio
            {1,             1,            4,            4},  // pps <  1200 HD vedio
            {1,             1,            4,            4},  // pps <  2000 2K
            {1,             1,            4,            4},  // pps <  5000 4K
            {1,             1,            4,            4}   // pps >= 5000 8K
        };
        u32 fec2_ml_book_id2[][4] = {
            // loss < 2.0%  loss < 10.0%  loss < 50.0%  loss >= 50.0%
            {4,             4,            4,            4},  // pps <  10   test speed
            {4,             4,            4,            4},  // pps <  500  SD vedio
            {4,             4,            4,            4},  // pps <  1200 HD vedio
            {4,             4,            4,            4},  // pps <  2000 2K
            {4,             4,            4,            4},  // pps <  5000 4K
            {4,             4,            4,            4}   // pps >= 5000 8K
        };
        #endif

        #if (2 == APPLICATION_TYPE)
        session->fec2_obj_.ChangeFecMode(session->CalcGameFecBookId());
        #endif

        #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
        if (std_pps[std_pps_line][0] > session->pb_dt_.send_stat_.data_pack_pps_) {
            line_pos = 0;
            goto fec_ml_row_start_pos_;
        }

        if (std_pps[std_pps_line][1] > session->pb_dt_.send_stat_.data_pack_pps_) {
            line_pos = 1;
            goto fec_ml_row_start_pos_;
        }

        if (std_pps[std_pps_line][2] > session->pb_dt_.send_stat_.data_pack_pps_) {
            line_pos = 2;
            goto fec_ml_row_start_pos_;
        }

        if (std_pps[std_pps_line][3] > session->pb_dt_.send_stat_.data_pack_pps_) {
            line_pos = 3;
            goto fec_ml_row_start_pos_;
        }

        if (std_pps[std_pps_line][4] > session->pb_dt_.send_stat_.data_pack_pps_) {
            line_pos = 4;
            goto fec_ml_row_start_pos_;
        }
        line_pos = 5;

fec_ml_row_start_pos_:
        if (2.000001 > session->pb_dt_.max_send_loss_per_s_) {
            row_pos = 0;
            goto fec_ml_end_pos_;
        }

        if (10.000001 > session->pb_dt_.max_send_loss_per_s_) {
            row_pos = 1;
            goto fec_ml_end_pos_;
        }

        if (50.000001 > session->pb_dt_.max_send_loss_per_s_) {
            row_pos = 2;
            goto fec_ml_end_pos_;
        }
        row_pos = 3;

fec_ml_end_pos_:
        if ((kReliableStream == session->pb_dt_.tran_addr_.stream_type_)
         && (0x01 < session->pb_dt_.peer_version_)) {
            session->fec2_obj_.ChangeFecMode(fec2_ml_book_id2[line_pos][row_pos]);
        } else {
            session->fec2_obj_.ChangeFecMode(fec2_ml_book_id1[line_pos][row_pos]);
        }
        #endif

        #endif
        }
sender_qualiti_proc_start_pos_:
        {
        u32 loss_std = 0;
        u32 avg_loss = 0;

        loss_std = session->CalcLossStd((u32)(loss * 1000), &avg_loss);

        // 0.1% >= avg_loss_ or loss is stability, normal rtt testing.
        if ((100 >= avg_loss) || (MIN_START_QUICK_RTT_STD >= loss_std)) {
            if (TEST_RTT_PERIOD_US != session->test_rtt_period_us_) {
                session->test_rtt_period_us_ = TEST_RTT_PERIOD_US;
                #ifdef _SELFDEBUG
                GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelInfo, "normal rtt test "\
                       "%s:%u<-->%s:%u(%s) avg_loss=%u loss_std=%u.\r\n", session->pb_dt_.self_ip_,
                       (u32)(session->pb_dt_.self_port_), session->pb_dt_.peer_ip_,
                       (u32)(session->pb_dt_.peer_port_),
                       ((((u8)(GtpSessionMode::kSender)) == session->mode_) ? "sender":"receiver"),
                       avg_loss, loss_std);
                #endif
            }
            goto report_net_quality_cont_pos_;
        }

        if (TEST_RTT_PERIOD_US == session->test_rtt_period_us_) {
            session->measure_rtt_ts_us_ -= TEST_RTT_PERIOD_US;  // measure rtt at once.
            session->test_rtt_period_us_ = (TEST_RTT_PERIOD_US >> 2) + (TEST_RTT_PERIOD_US >> 3);

            #ifdef _SELFDEBUG
            GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelInfo, "quick rtt test %s:%u<-->%s:%u(%s) "\
                   "avg_loss=%u loss_std=%u.\r\n", session->pb_dt_.self_ip_,
                   (u32)(session->pb_dt_.self_port_), session->pb_dt_.peer_ip_,
                   (u32)(session->pb_dt_.peer_port_),
                   ((((u8)(GtpSessionMode::kSender)) == session->mode_) ? "sender":"receiver"),
                   avg_loss, loss_std);
            #endif
        }
        }

report_net_quality_cont_pos_:
        if (kDownLinkerLoss == loss_dir) {
            session->send_down_loss_ = 100.0;   // enhance response nack/ack by handly.
        }

        session->report_quality_flag_s_ = GTP_YES;

        link_quality.report_pos_       = kSenderQuality;
        link_quality.total_frame_num_  = session->pb_dt_.send_stat_.first_frame_sum_;
        link_quality.total_pack_num_   = session->pb_dt_.send_stat_.data_pack_sum_;
        link_quality.total_ack_num_    = session->pb_dt_.recv_stat_.ack_sum_;
        link_quality.total_retran_num_ = session->pb_dt_.send_stat_.retran_frame_sum_;
        link_quality.data_bitrate_bps_ = session->pb_dt_.send_stat_.data_bitrate_bps_;
        link_quality.net_bitrate_bps_  = session->pb_dt_.send_stat_.net_bitrate_bps_;
        link_quality.data_pack_pps_    = session->pb_dt_.send_stat_.data_pack_pps_;
        link_quality.net_pack_pps_     = session->pb_dt_.send_stat_.net_pack_pps_;

        link_quality.linker_key_.direction_ = 1;

        session->arq_.GetStat(&(link_quality.total_ack_loss_num_), &(link_quality.total_rto_loss_num_),
                              &(link_quality.total_ack_err_num_), &(link_quality.total_ai_repair_num_));

        session->arq_.AdjustRtoTimeout(rto_us, rtt_us);

        #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
        u32 max_retran_std[][6] = {
            // 25000us >= rtt, 50000us >= rtt, 100000us >= rtt, 150000us >= rtt, 250000us >= rtt, 250000us < rtt
            {5, 5, 5, 5, 5, 5},  // 10.000001 >= loss
            {5, 5, 5, 5, 5, 5},  // 20.000001 >= loss
            {5, 5, 5, 5, 5, 5},  // 30.000001 >= loss
            {5, 5, 5, 5, 5, 5},  // 40.000001 >= loss
            {5, 5, 5, 5, 5, 5},  // 50.000001 >= loss
            {5, 5, 5, 5, 5, 5},  // 60.000001 >= loss
            {5, 5, 5, 5, 5, 5},  // 70.000001 >= loss
            {5, 5, 5, 5, 5, 5}   // 70.000001 <  loss
        };
        #endif

        #if (2 == APPLICATION_TYPE)
        u32 max_retran_std[][6] = {
            // 25000us >= rtt, 50000us >= rtt, 100000us >= rtt, 150000us >= rtt, 250000us >= rtt, 250000us < rtt
            {5, 5, 5, 3, 2, 2},  // 10.000001 >= loss  (50-100ms RTT: 4→5 retrans for CS2 burst recovery)
            {5, 4, 4, 3, 3, 2},  // 20.000001 >= loss  (150-250ms: 2→3, >250ms: 1→2)
            {5, 4, 4, 3, 2, 1},  // 30.000001 >= loss  (100-150ms: 2→3, 150-250ms: 1→2)
            {5, 4, 3, 2, 1, 1},  // 40.000001 >= loss
            {4, 3, 2, 1, 1, 1},  // 50.000001 >= loss
            {3, 2, 1, 0, 0, 0},  // 60.000001 >= loss
            {1, 1, 0, 0, 0, 0},  // 70.000001 >= loss
            {0, 0, 0, 0, 0, 0}   // 70.000001 <  loss
        };
        #endif

        u32 std_index;

        if (25000 >= rtt_us) {
            std_index = 0;
            goto adjust_max_retran_time_pos_;
        }

        if (50000 >= rtt_us) {
            std_index = 1;
            goto adjust_max_retran_time_pos_;
        }

        if (100000 >= rtt_us) {
            std_index = 2;
            goto adjust_max_retran_time_pos_;
        }

        if (150000 >= rtt_us) {
            std_index = 3;
            goto adjust_max_retran_time_pos_;
        }

        if (250000 >= rtt_us) {
            std_index = 4;
            goto adjust_max_retran_time_pos_;
        }

        std_index = 5;

adjust_max_retran_time_pos_:
        session->arq_.AdjustSWinCurrentLossRate(loss, (u32)loss_dir);

        if (10.000001 >= loss) {
            session->arq_.AdjustRetranTimes(max_retran_std[0][std_index]);
            goto goodtp_continue_rpt_quality_pos_;
        }

        if (20.000001 >= loss) {
            session->arq_.AdjustRetranTimes(max_retran_std[1][std_index]);
            goto goodtp_continue_rpt_quality_pos_;
        }

        if (30.000001 >= loss) {
            session->arq_.AdjustRetranTimes(max_retran_std[2][std_index]);
            goto goodtp_continue_rpt_quality_pos_;
        }

        if (40.000001 >= loss) {
            session->arq_.AdjustRetranTimes(max_retran_std[3][std_index]);
            goto goodtp_continue_rpt_quality_pos_;
        }

        if (50.000001 >= loss) {
            session->arq_.AdjustRetranTimes(max_retran_std[4][std_index]);
            goto goodtp_continue_rpt_quality_pos_;
        }

        if (60.000001 >= loss) {
            session->arq_.AdjustRetranTimes(max_retran_std[5][std_index]);
            goto goodtp_continue_rpt_quality_pos_;
        }

        if (70.000001 >= loss) {
            session->arq_.AdjustRetranTimes(max_retran_std[6][std_index]);
            goto goodtp_continue_rpt_quality_pos_;
        }

        session->arq_.AdjustRetranTimes(max_retran_std[7][std_index]);

        goto goodtp_continue_rpt_quality_pos_;
    }

    if (win_hdl == session->data_win_r_) {
        session->arq_.AdjustRWinCurrentLossRate(loss, (u32)loss_dir);

        link_quality.report_pos_         = kReceiverQuality;
        link_quality.total_frame_num_    = session->pb_dt_.recv_stat_.first_frame_sum_;
        link_quality.total_pack_num_     = session->pb_dt_.recv_stat_.data_pack_sum_;
        link_quality.total_ack_num_      = session->pb_dt_.send_stat_.ack_sum_;
        link_quality.total_retran_num_   = session->pb_dt_.recv_stat_.retran_frame_sum_;
        link_quality.data_bitrate_bps_   = session->pb_dt_.recv_stat_.data_bitrate_bps_;
        link_quality.net_bitrate_bps_    = session->pb_dt_.recv_stat_.net_bitrate_bps_;
        link_quality.data_pack_pps_      = session->pb_dt_.recv_stat_.data_pack_pps_;
        link_quality.net_pack_pps_       = session->pb_dt_.recv_stat_.net_pack_pps_;
        link_quality.total_ack_loss_num_ = 0;
        link_quality.total_rto_loss_num_ = 0;
        link_quality.total_ack_err_num_  = 0;
        link_quality.total_ai_repair_num_ = 0;

        link_quality.linker_key_.direction_ = 2;

        goto goodtp_continue_rpt_quality_pos_;
    }

    GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
           "unknown slid window's handler(cur_win_hdl=%p send_win_hdl=%p recv_win_hdl=%p).\r\n",
           win_hdl, session->data_win_s_, session->data_win_r_);

goodtp_continue_rpt_quality_pos_:
    #ifdef _SELFDEBUG
    u64 delta = GtpSysTimestampUs() - session->create_ts_us_;
    GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
           "session rtt=%uus loss=%5.2f delta=%lluus.\r\n", rtt_us, loss, delta);
    #endif

    memcpy(link_quality.self_bin_ip_, session->self_bin_ip_, GTP_MAX_BIN_IP_SZ);
    memcpy(link_quality.peer_bin_ip_, session->peer_bin_ip_, GTP_MAX_BIN_IP_SZ);

    link_quality.self_ip_          = &(session->pb_dt_.self_ip_[0]);
    link_quality.peer_ip_          = &(session->pb_dt_.peer_ip_[0]);
    link_quality.self_bin_port_    = session->self_bin_port_;
    link_quality.peer_bin_port_    = session->peer_bin_port_;
    link_quality.self_port_        = session->pb_dt_.self_port_;
    link_quality.peer_port_        = session->pb_dt_.peer_port_;
    link_quality.self_ip_family_   = (u8)(session->self_ip_family_);
    link_quality.peer_ip_family_   = (u8)(session->peer_ip_family_);
    link_quality.rtt_ms_           = (u16)(rtt_us / 1000);
    link_quality.rtt_jitter_ms_    = (u16)(jitter_us / 1000);
    link_quality.loss_             = loss;
    link_quality.pre_congest_rank_ = pre_congest_rank;
    link_quality.bitrage_chg_k_    = 0;
    link_quality.loss_direct_      = loss_dir;
    link_quality.context_          = session->pb_dt_.tran_addr_.context_;

    link_quality.linker_key_.sfd_ = session->pb_dt_.tran_addr_.sfd_;
    link_quality.linker_key_.self_socket_addr_len_ = session->pb_dt_.tran_addr_.self_addr_len_;
    link_quality.linker_key_.peer_socket_addr_len_ = session->pb_dt_.tran_addr_.sock_addr_len_;

    memcpy(link_quality.linker_key_.self_socket_addr_, session->pb_dt_.tran_addr_.self_addr_,
           session->pb_dt_.tran_addr_.self_addr_len_);
    memcpy(link_quality.linker_key_.peer_socket_addr_, session->pb_dt_.tran_addr_.sock_addr_,
           session->pb_dt_.tran_addr_.sock_addr_len_);

    u32 nret = GTP_OK;

    if (NULL != session->cb_.report_link_quality_cb_) {
        #if (1 == ENABLE_MD_PERF_CHECK)
        u64 tmp_us = GtpSysTimestampUs();
        nret = session->cb_.report_link_quality_cb_(GtpHdlIntToPointer(session->pb_dt_.gtp_hdl_),
                                                    &link_quality, 1);
        tmp_us = GtpSysTimestampUs() - tmp_us;
        session->pb_dt_.send_consume_.ts_us_ += tmp_us;
        session->pb_dt_.recv_consume_.ts_us_ += tmp_us;
        #else
        nret = session->cb_.report_link_quality_cb_(GtpHdlIntToPointer(session->pb_dt_.gtp_hdl_),
                                                    &link_quality, 1);
        #endif
    }

    if (((u32)GTP_ERR) == nret) {
        // it means turning on tracking running time.
        session->pb_dt_.tran_addr_.timestamp_ = GtpSysTimestampUs();
        goto link_quality_continue_pos_;
    }

    if (GTP_OK != nret) {
        GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "call rpt_quality_cb_() failed(0x%08x).\r\n", nret);
    }

link_quality_continue_pos_:
    #ifdef _SELFDEBUG
    if (0.000001 < loss) {
        GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelInfo, "%s:%u<-->%s:%u owner=%s pos=%s "\
               "rtt=%ums jitter=%ums loss=%5.2f%% dir=%s.\r\n", session->pb_dt_.self_ip_,
               (u32)(session->pb_dt_.self_port_), session->pb_dt_.peer_ip_,
               (u32)(session->pb_dt_.peer_port_),
               ((((u8)(GtpSessionMode::kSender)) == session->mode_) ? "sender":"receiver"),
               ((kSenderQuality == link_quality.report_pos_) ? "sending":"receiving"),
               (u32)(link_quality.rtt_ms_), (u32)(link_quality.rtt_jitter_ms_), loss,
               ((kUpLinkerLoss == loss_dir) ? "up":"down"));
    }
    #endif

    return;
}

void WinSnBitmapCallback(slid_win_hdl win_hdl, void *cntxt_hdl, const void *bit_map, const u32 &ack_nack) {
    GtpSession *session = static_cast<GtpSession *>(cntxt_hdl);
    if (win_hdl != session->data_win_r_) {
        return;
    }

    u64 now_ts_us = GtpSysTimestampUs();

    u8 *chg_len_zone = NULL;

    u8 tmp_buf[4096];
    enum {
        kMaxFilteredNackNum = (sizeof(tmp_buf) - sizeof(GtpPacket) - sizeof(GtpNackPacketLoad) - sizeof(u64))
                            / sizeof(u16)
    };
    u16 filtered_nack[kMaxFilteredNackNum];

    GtpPacket *pack = (GtpPacket*)tmp_buf;

    u32 pack_size      = 0;
    u16 tail_sn_offset = 0;
    u16 rto_sn_offset  = 0;

    pack->goodtp_ver_      = CalcRightGtpVer(GTP_VERSION, session->pb_dt_.peer_version_);
    pack->header_offset_   = (u8)sizeof(GtpPacket);
    pack->has_loss_flag_   = GTP_NO;
    pack->cache_us_flag_   = GTP_NO;
    pack->pack_type_       = (u8)(GtpPackType::kGtpNackPackType);
    pack->pack_sn_         = session->ack_sn_;
    pack->has_check_flag_  = GTP_NO;
    pack->has_ts_flag_     = GTP_NO;
    pack->has_rtt_flag_    = GTP_NO;
    pack->init_flag_       = (u8)(((u8)(GtpSessStat::kRunning) == session->recv_session_stat_) ? GTP_NO : GTP_YES);
    pack->repeat_counter_  = 0;
    pack->is_qos_flg_      = GTP_NO;
    pack->share_zone_      = 0;
    pack->has_chg_zone_    = GTP_NO;
    pack->stream_type_     = kRealTimeStream;
    GtpWriteStreamKeyHeader(pack, session->pb_dt_.tran_addr_.stream_key_);

    if (kNackType == ack_nack) {
        const NackData *nack_data = (NackData*)bit_map;
        const u16 *nack_offset = NULL;
        u32 send_nack_num = (u32)nack_data->nack_num_;

        #if (2 == APPLICATION_TYPE)
        send_nack_num = session->FilterRealtimeNackOffsets(nack_data, filtered_nack,
                                                           (u32)kMaxFilteredNackNum, now_ts_us);
        if (0 == send_nack_num) {
            #ifdef _SELFDEBUG
            GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
                   "[win_hdl=%p]%s:%u<-->%s:%u skip empty nack head_sn=%u tail_sn=%u rto_sn=%u "
                   "nack_num=%u.\r\n",
                   win_hdl, session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_),
                   session->pb_dt_.peer_ip_, (u32)(session->pb_dt_.peer_port_),
                   nack_data->head_sn_, nack_data->tail_sn_, nack_data->rto_sn_, (u32)nack_data->nack_num_);
            #endif
            return;
        }
        nack_offset = filtered_nack;
        #else
        nack_offset = filtered_nack;
        send_nack_num = session->FilterRealtimeNackOffsets(nack_data, filtered_nack,
                                                           (u32)kMaxFilteredNackNum, now_ts_us);
        #endif

        if (nack_data->head_sn_ <= nack_data->tail_sn_) {
            tail_sn_offset = (u16)(nack_data->tail_sn_ - nack_data->head_sn_);
        } else {
            tail_sn_offset = (u16)(nack_data->tail_sn_ + (0xFFFFFFFF - nack_data->head_sn_) + 1);
        }

        if (nack_data->head_sn_ <= nack_data->rto_sn_) {
            rto_sn_offset = (u16)(nack_data->rto_sn_ - nack_data->head_sn_);
        } else {
            rto_sn_offset = (u16)(nack_data->rto_sn_ + (0xFFFFFFFF - nack_data->head_sn_) + 1);
        }

        pack_size = pack->header_offset_ + sizeof(GtpNackPacketLoad) + (send_nack_num << 1);

        GtpNackPacketLoad *nack_pack = (GtpNackPacketLoad*)(((u8*)pack) + pack->header_offset_);

        nack_pack->nack_num_       = (u16)send_nack_num;
        nack_pack->tail_sn_offset_ = tail_sn_offset;
        nack_pack->rto_sn_offset_  = rto_sn_offset;
        nack_pack->head_sn_        = nack_data->head_sn_;
        nack_pack->recv_loss_      = nack_data->recv_loss_;

        chg_len_zone = ((u8*)nack_pack) + sizeof(GtpNackPacketLoad);

        if (0 != session->com_cache_ts_us_) {
            if (now_ts_us >= session->at_com_cache_ts_us_) {
                now_ts_us -= session->at_com_cache_ts_us_;
            } else {
                now_ts_us = session->at_com_cache_ts_us_ - now_ts_us;
            }

            pack->cache_us_flag_      = GTP_YES;
            GtpWriteU64Unaligned(chg_len_zone, session->com_cache_ts_us_ + now_ts_us);
            chg_len_zone             += sizeof(u64);
            pack_size                += sizeof(u64);

            session->com_cache_ts_us_    = 0;
            session->at_com_cache_ts_us_ = 0;
        }

        pack->pack_size_ = (u16)pack_size;

        if (3 <= send_nack_num) {
            session->nack_burst_detected_ = 1;
        }

        memcpy(chg_len_zone, nack_offset, send_nack_num << 1);

        #ifdef _SELFDEBUG
        GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
               "[win_hdl=%p]%s:%u<-->%s:%u(nack) head_sn=%u tail_sn=%u rto_sn=%u nack_num=%u.\r\n", win_hdl,
               session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_),
               session->pb_dt_.peer_ip_, (u32)(session->pb_dt_.peer_port_),
               nack_pack->head_sn_, nack_data->tail_sn_, nack_data->rto_sn_, nack_pack->nack_num_);
        #endif

        goto session_send_ack_nack_pos_;
    }

    {
    const ReceivedSnBitMap *ack_data = (ReceivedSnBitMap*)bit_map;

    if (ack_data->head_sn_ <= ack_data->tail_sn_) {
        tail_sn_offset = (u16)(ack_data->tail_sn_ - ack_data->head_sn_);
    } else {
        tail_sn_offset = (u16)(ack_data->tail_sn_ + (0xFFFFFFFF - ack_data->head_sn_) + 1);
    }

    if (ack_data->head_sn_ <= ack_data->rto_sn_) {
        rto_sn_offset = (u16)(ack_data->rto_sn_ - ack_data->head_sn_);
    } else {
        rto_sn_offset = (u16)(ack_data->rto_sn_ + (0xFFFFFFFF - ack_data->head_sn_) + 1);
    }

    pack_size = pack->header_offset_ + sizeof(GtpAckPacketLoad) + ack_data->mem_size_;

    GtpAckPacketLoad *ack = (GtpAckPacketLoad*)(((u8*)pack) + pack->header_offset_);

    pack->pack_type_     = (u8)(GtpPackType::kGtpAckPackType);
    ack->sn_size_        = (u16)(ack_data->mem_size_);
    ack->tail_sn_offset_ = tail_sn_offset;
    ack->rto_sn_offset_  = rto_sn_offset;
    ack->head_sn_        = ack_data->head_sn_;
    ack->recv_loss_      = ack_data->recv_loss_;

    chg_len_zone = ((u8*)ack) + sizeof(GtpAckPacketLoad);

    if (0 != session->com_cache_ts_us_) {
        if (now_ts_us >= session->at_com_cache_ts_us_) {
            now_ts_us -= session->at_com_cache_ts_us_;
        } else {
            now_ts_us = session->at_com_cache_ts_us_ - now_ts_us;
        }

        pack->cache_us_flag_  = GTP_YES;
        GtpWriteU64Unaligned(chg_len_zone, session->com_cache_ts_us_ + now_ts_us);
        chg_len_zone         += sizeof(u64);
        pack_size            += sizeof(u64);

        session->com_cache_ts_us_    = 0;
        session->at_com_cache_ts_us_ = 0;

        #ifdef _SELFDEBUG
        GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
               "[win_hdl=%p]%s:%u<-->%s:%u(ack) head_sn=%u tail_sn=%u rto_sn=%u.\r\n", win_hdl,
               session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_),
               session->pb_dt_.peer_ip_, (u32)(session->pb_dt_.peer_port_),
               ack->head_sn_, ack_data->tail_sn_, ack_data->rto_sn_);
        #endif
    }

    pack->pack_size_ = (u16)pack_size;

    memcpy(chg_len_zone, ack_data->sn_bit_map_, ack_data->mem_size_);
    }

session_send_ack_nack_pos_:
    session->ack_sn_ += 1;

    GtpHeaderNewToOld(pack, session->pb_dt_.peer_version_);

    u32 result = GTP_OK;
    #if (1 == ENABLE_MD_PERF_CHECK)
    {
    u64 app_consume_us = GtpSysTimestampUs();
    result = session->cb_.send_pack_cb_(GtpHdlIntToPointer(session->pb_dt_.gtp_hdl_),
                                        pack, pack_size, &(session->pb_dt_.tran_addr_));
    app_consume_us = GtpSysTimestampUs() - app_consume_us;
    session->pb_dt_.app_send_consume_.CacheConsumeTime(app_consume_us);

    session->pb_dt_.send_consume_.ts_us_ += app_consume_us;
    session->pb_dt_.recv_consume_.ts_us_ += app_consume_us;
    }
    #else
    result = session->cb_.send_pack_cb_(GtpHdlIntToPointer(session->pb_dt_.gtp_hdl_),
                                        pack, pack_size, &(session->pb_dt_.tran_addr_));
    #endif

    if (GTP_OK != result) {
        GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "call send_pack_cb_() failed(0x%08x)\r\n", result);
        return;
    }

    if (((u8)(GtpPackType::kGtpAckPackType)) == pack->pack_type_) {
        session->pb_dt_.send_stat_.ack_sum_ += 1;
    } else if (((u8)(GtpPackType::kGtpNackPackType)) == pack->pack_type_) {
        session->pb_dt_.send_stat_.nack_sum_ += 1;
    }
    session->pb_dt_.send_stat_.net_pack_sum_    += 1;
    session->pb_dt_.send_stat_.net_bitrate_sum_ += pack_size;

    #ifdef _SELFDEBUG
    if (((u8)(GtpPackType::kGtpAckPackType)) == pack->pack_type_) {
        const GtpAckPacketLoad *ack = (const GtpAckPacketLoad*)(((const u8*)pack) + pack->header_offset_);
        GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
               "%s:%u<-->%s:%u send ack reason=%s pack_sn=%u head_sn=%u tail_sn=%u rto_sn=%u sn_size=%u "
               "recv_loss=%u cache_us_flag=%u key=%llu.\r\n",
               session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_),
               session->pb_dt_.peer_ip_, (u32)(session->pb_dt_.peer_port_),
               GtpFeedbackDebugReasonName(session->debug_feedback_reason_), pack->pack_sn_, ack->head_sn_,
               ack->head_sn_ + ack->tail_sn_offset_, ack->head_sn_ + ack->rto_sn_offset_,
               (u32)(ack->sn_size_), ack->recv_loss_, (u32)(pack->cache_us_flag_),
               (unsigned long long)session->pb_dt_.tran_addr_.stream_key_);
    } else if (((u8)(GtpPackType::kGtpNackPackType)) == pack->pack_type_) {
        const GtpNackPacketLoad *nack = (const GtpNackPacketLoad*)(((const u8*)pack) + pack->header_offset_);
        GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
               "%s:%u<-->%s:%u send nack reason=%s pack_sn=%u head_sn=%u tail_sn=%u rto_sn=%u nack_num=%u "
               "recv_loss=%u cache_us_flag=%u key=%llu.\r\n",
               session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_),
               session->pb_dt_.peer_ip_, (u32)(session->pb_dt_.peer_port_),
               GtpFeedbackDebugReasonName(session->debug_feedback_reason_), pack->pack_sn_, nack->head_sn_,
               nack->head_sn_ + nack->tail_sn_offset_, nack->head_sn_ + nack->rto_sn_offset_,
               (u32)(nack->nack_num_), nack->recv_loss_, (u32)(pack->cache_us_flag_),
               (unsigned long long)session->pb_dt_.tran_addr_.stream_key_);
    }
    #endif

    session->last_feedback_nack_ts_us_    = now_ts_us;

    return;
}

GtpSession::GtpSession(const goodtp_sock &sfd, const u8 dst_sock_addr[], const u32 &dst_sock_addr_size,
               const u8 slf_sock_addr[], const u32 &slf_sock_addr_size, const u8 &stream_type, const u8 &smooth_jiter,
               const GtpHandler &gtp_hdl, GtpMemPool *arq_node_mem_pool, const GtpCallBackParam &cb, const u64 &ts_us,
               const u64 &ttl, TranMemPool &pack_mem_pool, GtpHandler_p app_gtp_hdl, const GtpFec2Mode *fec_code_book,
               void *context, const u32 &mode):
    cb_(cb),
    create_ts_us_(ts_us),
    last_active_ts_us_(ts_us),
    measure_rtt_ts_us_(ts_us),
    gen_new_rtt_ts_us_(0),
    com_cache_ts_us_(0),
    at_com_cache_ts_us_(0),
    second_ts_us_(ts_us + 1000000),
    recv_idle_calc_loss_ts_us(0xFFFFFFFFFFFFFFFF),
    next_calc_factor1_ts_us_((u32)((ts_us + DEFAULT_RTT_US) & 0x00000000FFFFFFFF)),
    restore_factor1_tm_span_us_(DEFAULT_RTT_US),
    last_max_frame_period_us_(DEFAULT_MAX_FRAME_PERIOD_US),
    now_max_frame_period_us_(0),
    send_down_loss_(-1.0),
    recv_idle_calc_period_us_(DEFAULT_HANDLER_CALC_PERIOD_US),
    recv_last_sync_ts_us_(ts_us),
    last_send_ts_us_(ts_us),
    last_gen_loss_ts_us_(ts_us),
    last_feedback_nack_ts_us_(ts_us),
    last_recv_data_pack_ts_us_(ts_us),
    recv_max_data_sn_(0),
    rmv_close_alg_ts_us_(0),
    nack_burst_detected_(0),
    new_gap_detected_(0),
    gap_nack_hold_ticks_(0),
    elevated_loss_streak_(0),
    #ifdef _SELFDEBUG
    debug_feedback_reason_(kGtpFeedbackDebugUnknown),
    #endif
    min_session_stability_us_((u32)ttl),
    report_quality_flag_s_(GTP_NO),
    mode_((u8)mode),
    cache_loss_ovt_(GTP_NO),
    max_frm_prd_updt_flg_(GTP_NO),
    net_quality_((u8)(NetQuality::kNetQualityGood)),
    rpt_snd_qualit_(GTP_NO),
    session_health_((u8)(SessionHealthState::kHealthNoraml)),
    cur_cache_loss_idx_(0),
    test_rtt_period_us_(TEST_RTT_PERIOD_US),
    loss_sum_(0),
    self_session_ttl_us_((u32)ttl),
    self_bin_port_(0),
    peer_bin_port_(0),
    data_win_s_(NULL),
    data_win_r_(NULL),
    pack_sn_(0),
    ack_sn_(0),
    fac_(),
    rtt_us_(0),
    max_peak_frame_period_us_(0),
    ai_learn_sn_delta_(DEF_MIX_QUINTUPLET_THRESHOLD),
    pb_dt_(gtp_hdl, ts_us),
    last_sn_(0),
    sort_sn_(0),
    app_gtp_hdl_(app_gtp_hdl),
    slid_win_size_(SlidwinInstanceSize()),
    arq_(DEFAULT_RTO_TIMEOUT_US, MAX_RETRAN_PACKET_TIMES, cb, pack_mem_pool),
    fec2_obj_(GtpSendPackCallBack, cb.write_log_cb_, pack_mem_pool,
              (pFec2RestroreReceive)(GtpSession::Fec2RestoreFrameReceive), fec_code_book),
    arq_node_mem_pool_(arq_node_mem_pool),
    pack_mem_pool_(pack_mem_pool),
    realtime_reorder_win_(),
    realtime_reorder_next_deliver_ts_us_(0),
    realtime_reorder_late_rescue_(0) {
    win_mem_s_  = GtpAlignSessionMem(((u8*)this) + sizeof(GtpSession));
    win_mem_r_  = GtpAlignSessionMem(win_mem_s_ + SlidwinInstanceSize());
    filter_mem_ = GtpAlignSessionMem(win_mem_r_ + SlidwinInstanceSize());

    pb_dt_.tran_addr_.context_       = context;
    pb_dt_.tran_addr_.sfd_           = (u32)sfd;
    pb_dt_.tran_addr_.sock_addr_len_ = dst_sock_addr_size;
    pb_dt_.tran_addr_.self_addr_len_ = slf_sock_addr_size;
    pb_dt_.tran_addr_.stream_type_   = stream_type;
    pb_dt_.tran_addr_.smooth_jitter_ = smooth_jiter;

    memcpy(pb_dt_.tran_addr_.sock_addr_, dst_sock_addr, dst_sock_addr_size);
    memcpy(pb_dt_.tran_addr_.self_addr_, slf_sock_addr, slf_sock_addr_size);

    memset(self_bin_ip_, 0x00, GTP_MAX_BIN_IP_SZ);
    memset(peer_bin_ip_, 0x00, GTP_MAX_BIN_IP_SZ);
    memset(cache_loss_, 0x00, sizeof(cache_loss_));

    send_session_stat_ = (u8)(GtpSessStat::kInitReq);
    recv_session_stat_ = (u8)(GtpSessStat::kInitRes);
}

GtpSession::GtpSession(GtpAddr *tran_addr, const GtpHandler &gtp_hdl, GtpMemPool *arq_node_mem_pool,
                       const GtpCallBackParam &cb, const u64 &ts_us, const u64 &ttl, TranMemPool &pack_mem_pool,
                       GtpHandler_p app_gtp_hdl, const GtpFec2Mode *fec_code_book, const u32 &mode) :
    cb_(cb),
    create_ts_us_(ts_us),
    last_active_ts_us_(ts_us),
    measure_rtt_ts_us_(ts_us),
    gen_new_rtt_ts_us_(0),
    com_cache_ts_us_(0),
    at_com_cache_ts_us_(0),
    second_ts_us_(ts_us + 1000000),
    recv_idle_calc_loss_ts_us(0xFFFFFFFFFFFFFFFF),
    next_calc_factor1_ts_us_((u32)((ts_us + DEFAULT_RTT_US) & 0x00000000FFFFFFFF)),
    restore_factor1_tm_span_us_(DEFAULT_RTT_US),
    last_max_frame_period_us_(DEFAULT_MAX_FRAME_PERIOD_US),
    now_max_frame_period_us_(0),
    send_down_loss_(-1.0),
    recv_idle_calc_period_us_(DEFAULT_HANDLER_CALC_PERIOD_US),
    recv_last_sync_ts_us_(ts_us),
    last_send_ts_us_(ts_us),
    last_gen_loss_ts_us_(ts_us),
    last_feedback_nack_ts_us_(ts_us),
    last_recv_data_pack_ts_us_(ts_us),
    recv_max_data_sn_(0),
    rmv_close_alg_ts_us_(0),
    nack_burst_detected_(0),
    new_gap_detected_(0),
    gap_nack_hold_ticks_(0),
    elevated_loss_streak_(0),
    #ifdef _SELFDEBUG
    debug_feedback_reason_(kGtpFeedbackDebugUnknown),
    #endif
    min_session_stability_us_((u32)ttl),
    report_quality_flag_s_(GTP_NO),
    mode_((u8)mode),
    cache_loss_ovt_(GTP_NO),
    max_frm_prd_updt_flg_(GTP_NO),
    net_quality_((u8)(NetQuality::kNetQualityGood)),
    rpt_snd_qualit_(GTP_NO),
    session_health_((u8)(SessionHealthState::kHealthNoraml)),
    cur_cache_loss_idx_(0),
    test_rtt_period_us_(TEST_RTT_PERIOD_US),
    loss_sum_(0),
    self_session_ttl_us_((u32)ttl),
    self_bin_port_(0),
    peer_bin_port_(0),
    data_win_s_(NULL),
    data_win_r_(NULL),
    pack_sn_(0),
    ack_sn_(0),
    fac_(),
    rtt_us_(0),
    max_peak_frame_period_us_(0),
    ai_learn_sn_delta_(DEF_MIX_QUINTUPLET_THRESHOLD),
    pb_dt_(gtp_hdl, ts_us),
    last_sn_(0),
    sort_sn_(0),
    app_gtp_hdl_(app_gtp_hdl),
    slid_win_size_(SlidwinInstanceSize()),
    arq_(DEFAULT_RTO_TIMEOUT_US, MAX_RETRAN_PACKET_TIMES, cb, pack_mem_pool),
    fec2_obj_(GtpSendPackCallBack, cb.write_log_cb_, pack_mem_pool,
              (pFec2RestroreReceive)(GtpSession::Fec2RestoreFrameReceive), fec_code_book),
    arq_node_mem_pool_(arq_node_mem_pool),
    pack_mem_pool_(pack_mem_pool),
    realtime_reorder_win_(),
    realtime_reorder_next_deliver_ts_us_(0),
    realtime_reorder_late_rescue_(0) {
    win_mem_s_  = GtpAlignSessionMem(((u8*)this) + sizeof(GtpSession));
    win_mem_r_  = GtpAlignSessionMem(win_mem_s_ + SlidwinInstanceSize());
    filter_mem_ = GtpAlignSessionMem(win_mem_r_ + SlidwinInstanceSize());

    memcpy(&(pb_dt_.tran_addr_), tran_addr, sizeof(GtpAddr));

    pb_dt_.tran_addr_.stream_type_   = 0;
    pb_dt_.tran_addr_.smooth_jitter_ = 0;

    memset(self_bin_ip_, 0x00, GTP_MAX_BIN_IP_SZ);
    memset(peer_bin_ip_, 0x00, GTP_MAX_BIN_IP_SZ);
    memset(cache_loss_, 0x00, sizeof(cache_loss_));

    send_session_stat_ = (u8)(GtpSessStat::kInitReq);
    recv_session_stat_ = (u8)(GtpSessStat::kInitRes);
}

GtpSession::~GtpSession() {
    if (NULL != data_win_s_) {
        DeleteSlidWin(data_win_s_);
        data_win_s_ = NULL;
    }

    if (NULL != data_win_r_) {
        DeleteSlidWin(data_win_r_);
        data_win_r_ = NULL;
    }

    if (NULL != filter_win_) {
        DeleteSlidWin(filter_win_);
        filter_win_ = NULL;
    }
}

void* GtpSession::operator new(size_t n, void *psp_mem) {
    if (NULL == psp_mem) {
        throw "no input memory pool!";
    }

    GtpMemPool *mem_pool = (GtpMemPool*)psp_mem;

    u8 *session_mem = mem_pool->MallocItem();
    if (NULL == session_mem) {
        throw "memory pool is overflow!";
    }

    if (sizeof(u64) == sizeof(void*)) {
        GtpWriteU64Unaligned(session_mem, (u64)psp_mem);
    } else {
        GtpWriteU32Unaligned(session_mem, (u32)((u64)psp_mem));
    }
    session_mem += sizeof(void*);

    return session_mem;
}

void GtpSession::operator delete(void *psp_mem, void *placement_mem) {
    if (NULL != psp_mem) {
        GtpSession::operator delete(psp_mem);
    }
}

void GtpSession::operator delete(void *psp_mem) {
    u8 *save_mem_pool_ptr = ((u8*)psp_mem) - sizeof(void*);

    GtpMemPool *mem_pool;

    if (sizeof(u64) == sizeof(void*)) {
        mem_pool = (GtpMemPool*)(GtpReadU64Unaligned(save_mem_pool_ptr));
    } else {
        mem_pool = (GtpMemPool*)((u64)(GtpReadU32Unaligned(save_mem_pool_ptr)));
    }

    mem_pool->FreeItem(save_mem_pool_ptr);

    return;
}

u32 GtpSession::Init(const u64 &ts_us, u8 *win_cache, const u32 &pack_sn, const u32 &next_out_sn,
                     const u32 &sort_sn_valid) {
    pb_dt_.session_ = this;
    last_sn_ = pack_sn;

    if (0 == pb_dt_.tran_addr_.self_addr_len_) {
        UpdateSelfIp(pb_dt_.tran_addr_.sfd_);
    } else {
        UpdateSelfIp(&(pb_dt_.tran_addr_.self_addr_[0]), pb_dt_.tran_addr_.self_addr_len_);
    }

    UpdatePeerIp(&(pb_dt_.tran_addr_.sock_addr_[0]), pb_dt_.tran_addr_.sock_addr_len_);

    data_win_s_ = CreateSlidWin(LinkQualityCallback, WinSnBitmapCallback, cb_.write_log_cb_,
                                g_goodtp_inst_mgr.cur_log_level_cb_, 0, ts_us, (min_session_stability_us_ >> 6),
                                (u32)kSendSlidWinMode, win_mem_s_, this, win_cache);
    if (NULL == data_win_s_) {
        RETURN_ERR(kGtpSessionMd, kCrtSlidWinFailed);
    }

    data_win_r_ = CreateSlidWin(LinkQualityCallback, WinSnBitmapCallback, cb_.write_log_cb_,
                                g_goodtp_inst_mgr.cur_log_level_cb_, pack_sn, ts_us, (min_session_stability_us_ >> 6),
                                (u32)kRecvSlidWinMode, win_mem_r_, this, win_cache);
    if (NULL == data_win_r_) {
        RETURN_ERR(kGtpSessionMd, kCrtSlidWinFailed);
    }

    const u32 filter_border_sn = (GTP_YES == sort_sn_valid) ? next_out_sn : pack_sn;
    filter_win_ = CreateSlidWin(LinkQualityCallback, WinSnBitmapCallback, NULL, NULL, filter_border_sn, ts_us,
                                (min_session_stability_us_ >> 6), (u32)kRecvSlidWinMode, filter_mem_,
                                this, win_cache, 1);
    if (NULL == filter_win_) {
        RETURN_ERR(kGtpSessionMd, kCrtSlidWinFailed);
    }

    if (GTP_OK != arq_.Init(arq_node_mem_pool_, GtpSendPackCallBack, (pPackRetran)(GtpSession::PackRetransmit),
                            &pb_dt_)) {
        RETURN_ERR(kGtpSessionMd, kGtpArqInitFailed);
    }

    #if (1 == ENABLE_FEC)
    if (GTP_OK != fec2_obj_.Init(&pb_dt_)) {
        RETURN_ERR(kGtpSessionMd, kGtpFec2InitFailed);
    }
    #endif

    u8 version[256] = {0};

    GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "created new session%s:%u<--->%s:%u(key=%llu) "\
           "create=%lluus last_active=%lluus ttl=%uus(pid=%d tid=%u ver=%s).\r\n", pb_dt_.self_ip_,
           (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
           (unsigned long long)(pb_dt_.tran_addr_.stream_key_), (unsigned long long)create_ts_us_,
           (unsigned long long)last_active_ts_us_, self_session_ttl_us_, (i32)GtpGetProcessId(),
           (i32)GtpGetThreadId(), GetGtpVersion(&(version[0])));

    return GTP_OK;
}

u32 GtpSession::PackRetransmit(void *session, ArqNode *arq_node, GtpAddr *tran_addr, const u64 &cur_ts_us) {
    GtpSession *cur_session = (GtpSession*)session;

    if (0 != cur_ts_us) {
        arq_node->last_send_ts_us_ = cur_ts_us;
        tran_addr->timestamp_      = cur_ts_us;
    } else {
        arq_node->last_send_ts_us_ = GtpSysTimestampUs();
        tran_addr->timestamp_      = arq_node->last_send_ts_us_;
    }

    cur_session->last_active_ts_us_ = tran_addr->timestamp_;

    GtpPacket *org_pack    = (GtpPacket *)(arq_node->pack_);
    GtpPacket *retran_pack = NULL;

    u32 pack_size  = 0;
    u32 nret       = GTP_OK;
    u8 *frame      = arq_node->pack_ + arq_node->payload_offset_;
    u32 frame_size = (u32)(arq_node->pack_len_ - arq_node->payload_offset_);
    u32 hash       = 0;
    u32 user_id    = 0;
    u32 move_pos   = sizeof(GtpPacket);

    if (0 < ((u8)(org_pack->repeat_counter_))) {
        move_pos += sizeof(u32);
    }

    if (0 != org_pack->has_check_flag_) {
        hash    = GtpReadU32Unaligned((u8 *)org_pack + move_pos);
        user_id = GtpReadU32Unaligned((u8 *)org_pack + move_pos + sizeof(u32));
    }

    arq_node->retran_counter_ += 1;

    move_pos = arq_node->retran_counter_;
    move_pos = GtpLimit(1, 7, move_pos);

    nret = cur_session->FramePrepHandler(frame, frame_size, &retran_pack, &pack_size, hash, user_id,
                                         arq_node->first_pack_sn_, move_pos);
    if (GTP_OK != nret) {
        GtpLog(cur_session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "calling session's FramePrepHandler() failed(0x%08x)\r\n", nret);
        return nret;
    }

    arq_node->pack_           = (u8*)retran_pack;
    arq_node->tran_addr_      = (u8*)tran_addr;
    arq_node->pack_len_       = (u16)pack_size;
    arq_node->payload_offset_ = retran_pack->header_offset_;
    arq_node->pack_sn_        = retran_pack->pack_sn_;

    #if (1 == ENABLE_TRAN_MEM_CHECK)
    nret = GtpCheckPackMemOutBoundry(retran_pack, tran_addr);
    if (GTP_OK != nret) {
        GtpLog(cur_session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "memory has been crossed "\
               "between tran address memory and packet memory(pack_addr=%p tran_addr=%p resend_num=%u).\r\n",
               retran_pack, tran_addr, (u32)(arq_node->retran_counter_));
        goto harq_resend_end_pos_;
    }

    nret = GtpTranAddrIsValid(tran_addr);
    if (GTP_OK != nret) {
        GtpLog(cur_session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "Goodtp transport address "\
               "memory is invalid(0x%08x tran_addr=%p dst_addr_len=%u src_addr_len=%u dst_mem_addr=%p "\
               "src_mem_addr=%p).\r\n", nret, tran_addr,
               ((NULL != tran_addr) ? tran_addr->sock_addr_len_ : 0),
               ((NULL != tran_addr) ? tran_addr->self_addr_len_ : 0),
               ((NULL != tran_addr) ? tran_addr->sock_addr_ : NULL),
               ((NULL != tran_addr) ? tran_addr->self_addr_ : NULL));
         goto harq_resend_end_pos_;
    }
    #endif

    GtpHeaderNewToOld(retran_pack, cur_session->pb_dt_.peer_version_);
    #if (1 == ENABLE_MD_PERF_CHECK)
    {
    u64 app_consume_us = GtpSysTimestampUs();

    nret = cur_session->cb_.send_pack_cb_(GtpHdlIntToPointer(cur_session->pb_dt_.gtp_hdl_),
                                          retran_pack, pack_size, tran_addr);
    app_consume_us = GtpSysTimestampUs() - app_consume_us;
    cur_session->pb_dt_.app_send_consume_.CacheConsumeTime(app_consume_us);

    cur_session->pb_dt_.send_consume_.ts_us_ += app_consume_us;
    cur_session->pb_dt_.recv_consume_.ts_us_ += app_consume_us;
    }
    #else
    nret = cur_session->cb_.send_pack_cb_(GtpHdlIntToPointer(cur_session->pb_dt_.gtp_hdl_),
                                          retran_pack, pack_size, tran_addr);
    #endif
    GtpHeaderOldToNew(retran_pack);

    if (GTP_OK != nret) {
        GtpLog(cur_session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "calling send_pack_cb_() failed(0x%08x)\r\n", nret);
        return nret;
    }

#if (1 == ENABLE_TRAN_MEM_CHECK)
harq_resend_end_pos_:
#endif
    nret = cur_session->FramePostHandler(tran_addr, retran_pack, pack_size, (pack_size - retran_pack->header_offset_),
                                         GTP_NO, arq_node->first_pack_sn_, GTP_YES);
    if (GTP_OK != nret) {
        GtpLog(cur_session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "calling session's FramePostHandler() failed(0x%08x)\r\n", nret);
        return nret;
    }

    return GTP_OK;
}

u32 GtpSession::Fec2RestoreFrameReceive(void *session, GtpHandler gtp_hdl, GtpPacket *pack, GtpAddr *tran_addr,
                                   u32 *fec_res_sucess_stat, u32 *fec_res_failed_stat, u32 *fec_res_repeat_stat) {
    GtpSession *s_obj = static_cast<GtpSession *>(session);

    u32 first_sn  = 0;
    u32 ret_value = GTP_OK;

    ret_value = GtpCheckPacketInvalid(pack, (u32)(pack->pack_size_));
    if (GTP_OK != ret_value) {
        GtpLog(s_obj->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "%s:%u---->%s:%u fec2 restore an invalid "\
               "packet(ver=0x%02x offset=%u loss_flag=%u check_flag=%u ts_flag=%u rtt_flag=%u repeat_num=%u "\
               "type=0x%02x pack_size=%u in_size=%u pack_sn=0x%08x nret=0x%08x tid=%d).\r\n",
               s_obj->pb_dt_.peer_ip_, (u32)(s_obj->pb_dt_.peer_port_),
               s_obj->pb_dt_.self_ip_, (u32)(s_obj->pb_dt_.self_port_),
               (u32)(pack->goodtp_ver_), (u32)(pack->header_offset_), (u32)(pack->has_loss_flag_),
               (u32)(pack->has_check_flag_), (u32)(pack->has_ts_flag_), (u32)(pack->has_rtt_flag_),
               (u32)(pack->repeat_counter_), (u32)(pack->pack_type_), (u32)(pack->pack_size_),
               (pack->pack_size_), pack->pack_sn_, ret_value, (i32)GtpGetThreadId());

        (*fec_res_failed_stat) += 1;

        return ret_value;
    }

    (*fec_res_sucess_stat) += 1;

    if ((0x02 <= pack->goodtp_ver_) && (GTP_YES == pack->has_chg_zone_)) {
        const ChangeZone *chg_zone = GtpGetPacketChangeZone(pack);
        if (NULL != chg_zone) {
            first_sn = chg_zone->sort_sn_;
        }
    } else {
        if (0 == pack->repeat_counter_) {
            first_sn = pack->pack_sn_;
        } else {
            first_sn = GtpReadU32Unaligned(((u8*)pack) + sizeof(GtpPacket));
        }
    }

    #ifdef _SELFDEBUG
    GtpLog(s_obj->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
           "%s:%u---->%s:%u fec restored data packet(pack_sn=%u first_sn=%u sort_sn_valid=%u repeat_num=%u "
           "header_offset=%u pack_size=%u payload_size=%u stream_type=%u key=%llu).\r\n",
           s_obj->pb_dt_.peer_ip_, (u32)(s_obj->pb_dt_.peer_port_),
           s_obj->pb_dt_.self_ip_, (u32)(s_obj->pb_dt_.self_port_), pack->pack_sn_, first_sn,
           (u32)(((0x02 <= pack->goodtp_ver_) && (GTP_YES == pack->has_chg_zone_)) ? GTP_YES : GTP_NO),
           (u32)(pack->repeat_counter_), (u32)(pack->header_offset_), (u32)(pack->pack_size_),
           (u32)(pack->pack_size_ - pack->header_offset_), (u32)(pack->stream_type_),
           (unsigned long long)s_obj->pb_dt_.tran_addr_.stream_key_);
    #endif

    ret_value = RepeatPacketFilter(s_obj->filter_win_, first_sn, s_obj->last_active_ts_us_);
    if (GTP_YES == ret_value) {
        (*fec_res_repeat_stat) += 1;

        #ifdef _SELFDEBUG
        GtpLog(s_obj->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
               "%s:%u---->%s:%u fec restored packet, it's repeat packet(sn=%u ret=%u).\r\n",
               s_obj->pb_dt_.peer_ip_, (u32)(s_obj->pb_dt_.peer_port_),
               s_obj->pb_dt_.self_ip_, (u32)(s_obj->pb_dt_.self_port_),
               first_sn, ret_value);
        #endif
        return GTP_OK;
    }

    ret_value = SnEntrySlidWin(s_obj->filter_win_, first_sn, s_obj->last_active_ts_us_);
    if (GTP_OK != ret_value) {
        GtpLog(s_obj->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
               "%s:%u---->%s:%u Call SnEntrySlidWin() failed(0x%08x) first_sn=%u.\r\n",
               s_obj->pb_dt_.peer_ip_, (u32)(s_obj->pb_dt_.peer_port_),
               s_obj->pb_dt_.self_ip_, (u32)(s_obj->pb_dt_.self_port_),
               ret_value, first_sn);
        (*fec_res_repeat_stat) += 1;
        return GTP_OK;
    }

    #ifdef _SELFDEBUG
    s_obj->debug_feedback_reason_ = kGtpFeedbackDebugFecRestore;
    #endif
    ret_value = SnEntrySlidWin(s_obj->data_win_r_, pack->pack_sn_, s_obj->last_active_ts_us_);
    if (GTP_OK != ret_value) {
        GtpLog(s_obj->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
               "%s:%u---->%s:%u Call SnEntrySlidWin(data_win_r_) failed(0x%08x) pack_sn=%u first_sn=%u.\r\n",
               s_obj->pb_dt_.peer_ip_, (u32)(s_obj->pb_dt_.peer_port_),
               s_obj->pb_dt_.self_ip_, (u32)(s_obj->pb_dt_.self_port_),
               ret_value, pack->pack_sn_, first_sn);
        (*fec_res_repeat_stat) += 1;
        return GTP_OK;
    } else {
        #ifdef _SELFDEBUG
        s_obj->debug_feedback_reason_ = kGtpFeedbackDebugFecRestore;
        #endif
        ret_value = CalcQualityByHandler(s_obj->data_win_r_, s_obj->last_active_ts_us_, GTP_YES);
        if (GTP_OK != ret_value) {
            GtpLog(s_obj->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                   "%s:%u---->%s:%u Call CalcQualityByHandler(data_win_r_) failed(0x%08x %s) after fec restore "
                   "pack_sn=%u first_sn=%u.\r\n",
                   s_obj->pb_dt_.peer_ip_, (u32)(s_obj->pb_dt_.peer_port_),
                   s_obj->pb_dt_.self_ip_, (u32)(s_obj->pb_dt_.self_port_),
                   ret_value, WinErrorInfo(s_obj->data_win_r_, ret_value), pack->pack_sn_, first_sn);
        }
    }

    u8 *frame = (u8*)pack;
    u32 size  = (u32)(pack->pack_size_);

    frame += pack->header_offset_;
    size  -= pack->header_offset_;

    #if (1 == ENABLE_FRAME_COPY_OUT)
    u32 mem_spec = kMemSpec1Dot5k;

    /* the pack's memory has been cached to fec receiving buffer,
       so needing a new memory for handing in frame. */
    u8 *out_mem = NULL;
    GtpAddr *out_addr = NULL;
    
    mem_spec = GtpPackSizeToMemSpec(size);
    out_mem  = s_obj->pack_mem_pool_.MallocTranBuf(NULL, 0, &ret_value, (void**)(&out_addr), (BufSizeType)mem_spec);
    if (NULL == out_mem) {
        const string &err_info = s_obj->pack_mem_pool_.Error();
        GtpLog(s_obj->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "Call MallocTranBuf() failed(%s).\r\n",
               err_info.c_str());

        const string &stat_info = s_obj->pack_mem_pool_.TranMemPoolStatInfo();
        GtpLog(s_obj->cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelWarning, "%s\r\n", stat_info.c_str());

        return GTP_OK;
    }

    memcpy(out_mem, frame, size);
    memcpy(out_addr, tran_addr, sizeof(GtpAddr));

    frame     = out_mem;
    tran_addr = out_addr;
    #endif

    ret_value = s_obj->DeliverFrameInOrder(GtpHdlIntToPointer(gtp_hdl), first_sn, frame, size, tran_addr);

    #if (1 == ENABLE_FRAME_COPY_OUT)
    s_obj->pack_mem_pool_.FreeTranBuf(out_mem);
    out_mem = NULL;
    #endif

    if (GTP_OK != ret_value) {
        GtpLog(s_obj->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "%s:%u<-->%s:%u hand on fec restore frame failed(0x%08x) when calling receive_frame_cb().\r\n",
               s_obj->pb_dt_.self_ip_, (u32)(s_obj->pb_dt_.self_port_),
               s_obj->pb_dt_.peer_ip_, (u32)(s_obj->pb_dt_.peer_port_), ret_value);
    }

    return ret_value;
}

u32 GtpSession::FramePrepHandler(void *frame, const u32 &frame_size, GtpPacket **out_pack, u32 *out_pack_size,
                         const u32 &hash, const u32 &user_id, const u32 &first_pack_sn, const u32 &resend_num) {
    if ((0xFF == pb_dt_.peer_version_) || (GTP_VERSION == pb_dt_.peer_version_)) {
        return FramePrepHandlerWithSelfVer(frame, frame_size, out_pack, out_pack_size, hash, user_id,
                                           first_pack_sn, resend_num);
    }

    return FramePrepHandlerWithPeerVer(frame, frame_size, out_pack, out_pack_size, hash, user_id,
                                       first_pack_sn, resend_num);
}

u32 GtpSession::CalcHeaderSize(const u32 &hash, const u32 &user_id, const u32 &resend_num) {
    u32 hdr_size = 0;

    if (-1.0 < send_down_loss_) {
        hdr_size += sizeof(send_down_loss_);
    }

    if (measure_rtt_ts_us_ <= last_active_ts_us_) {
        hdr_size += sizeof(last_active_ts_us_);
    }

    if (0 != rtt_us_) {
        hdr_size += sizeof(rtt_us_);
    }

    if ((0 != user_id) || (0 != hash)) {
        hdr_size  += sizeof(user_id);
        hdr_size  += sizeof(hash);
    }

    if (0 < resend_num) {
        hdr_size += sizeof(u32);
    }

    if (((kReliableStream == pb_dt_.tran_addr_.stream_type_) || (kRealTimeStream == pb_dt_.tran_addr_.stream_type_))
     && (0 == resend_num)) {
        hdr_size += sizeof(ChangeZone);
    }

    hdr_size += sizeof(GtpPacket);

    return hdr_size;
}

u32 GtpSession::CalcHeaderSizeVersion01(const u32 &hash, const u32 &user_id, const u32 &resend_num) {
    u32 hdr_size = 0;

    if (-1.0 < send_down_loss_) {
        hdr_size += sizeof(send_down_loss_);
    }

    if (measure_rtt_ts_us_ <= last_active_ts_us_) {
        hdr_size += sizeof(last_active_ts_us_);
    }

    if (0 != rtt_us_) {
        hdr_size += sizeof(rtt_us_);
    }

    if ((0 != user_id) || (0 != hash)) {
        hdr_size  += sizeof(user_id);
        hdr_size  += sizeof(hash);
    }

    if (0 < resend_num) {
        hdr_size += sizeof(u32);
    }

    hdr_size += sizeof(GtpPacket);

    return hdr_size;
}

u32 GtpSession::FramePrepHandlerWithSelfVer(void *frame, const u32 &frame_size, GtpPacket **out_pack,
                                            u32 *out_pack_size, const u32 &hash, const u32 &user_id,
                                            const u32 &first_pack_sn, const u32 &resend_num) {
    u32 hdr_size  = 0;
    u16 has_check = GTP_NO;
    u16 has_chg   = GTP_NO;
    u16 has_ts    = GTP_NO;
    u16 padding   = 0;
    u16 has_rtt   = GTP_NO;
    u16 has_loss  = GTP_NO;
    #ifdef _SELFDEBUG
    u32 dbg_sort_sn = 0;
    #endif

    u8 *move_pos = (u8*)frame;

    hdr_size = CalcHeaderSize(hash, user_id, resend_num);
    move_pos -= hdr_size;

    // padding for 8 or 4 bytes align.
    if (8 == sizeof(void*)) {
        padding = (u16)(AddressToUint(move_pos) & 0x0000000000000007);
    } else {
        padding = (u16)(AddressToUint(move_pos) & 0x00000003);
    }

    hdr_size = 0;
    move_pos = (u8*)frame;

    if (((kReliableStream == pb_dt_.tran_addr_.stream_type_) || (kRealTimeStream == pb_dt_.tran_addr_.stream_type_))
     && (0 == resend_num)) {
        has_chg   = GTP_YES;
        hdr_size += sizeof(ChangeZone);
        move_pos -= sizeof(ChangeZone);

        ChangeZone *chg_zone = (ChangeZone*)move_pos;

        chg_zone->sort_sn_      = sort_sn_;
        chg_zone->senter_ts_us_ = 0;
        #ifdef _SELFDEBUG
        dbg_sort_sn             = sort_sn_;
        #endif

        chg_zone->senter_ts_us_ = (u32)(GtpSysTimestampUs() & 0x00000000FFFFFFFF);

        sort_sn_ += 1;
    }

    if (0 != padding) {
        hdr_size += padding;
        move_pos -= padding;

        memset(move_pos, 0x00, padding);
    }

    if (-1.0 < send_down_loss_) {
        has_loss  = GTP_YES;
        move_pos -= sizeof(send_down_loss_);
        hdr_size += sizeof(send_down_loss_);

        GtpWriteF32Unaligned(move_pos, send_down_loss_);
        send_down_loss_   = -1.0;
    }

    if (measure_rtt_ts_us_ <= last_active_ts_us_) {
        has_ts    = GTP_YES;
        move_pos -= sizeof(last_active_ts_us_);
        hdr_size += sizeof(last_active_ts_us_);

        if (0 == resend_num) {
            GtpWriteU64Unaligned(move_pos, last_active_ts_us_);  // last_active_ts_us_ is too old when retransporting.
        } else {
            GtpWriteU64Unaligned(move_pos, GtpSysTimestampUs());
        }

        measure_rtt_ts_us_ = last_active_ts_us_ + ((u64)test_rtt_period_us_);   // current session is sending.
    }

    if (0 != rtt_us_) {
        #ifdef _SELFDEBUG
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug, "%s:%u<-->%s:%u send to receive windows's "\
               "rrt=%lluus.\r\n", pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
               pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), (unsigned long long)rtt_us_);
        #endif
        has_rtt   = GTP_YES;
        move_pos -= sizeof(rtt_us_);
        hdr_size += sizeof(rtt_us_);

        GtpWriteU32Unaligned(move_pos, rtt_us_);
        rtt_us_            = 0;
    }

    if ((0 != user_id) || (0 != hash)) {
        has_check  = GTP_YES;
        move_pos  -= sizeof(user_id);
        hdr_size  += sizeof(user_id);

        GtpWriteU32Unaligned(move_pos, user_id);

        move_pos  -= sizeof(hash);
        hdr_size  += sizeof(hash);

        GtpWriteU32Unaligned(move_pos, hash);
    }

    if (0 < resend_num) {
        move_pos -= sizeof(first_pack_sn);
        hdr_size += sizeof(first_pack_sn);

        GtpWriteU32Unaligned(move_pos, first_pack_sn);
    }

    move_pos -= sizeof(GtpPacket);
    hdr_size += sizeof(GtpPacket);

    if (64 <= hdr_size) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "%s:%u<-->%s:%u packet's header is too long("\
               "header_offset=%u peer_version=0x%02x resend_num=%u frame_size=%u pack_size=%u.\r\n",
               pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
               hdr_size, (u32)(pb_dt_.peer_version_), resend_num, frame_size, *out_pack_size);
        RETURN_ERR(kGtpSessionMd, kGtpPackLengthErr);
    }

    if (0x1FFF < (hdr_size + frame_size)) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "%s:%u<-->%s:%u packet is too long("\
               "header_offset=%u peer_version=0x%02x resend_num=%u frame_size=%u pack_size=%u).\r\n",
               pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
               hdr_size, (u32)(pb_dt_.peer_version_), resend_num, frame_size, hdr_size + frame_size);
        RETURN_ERR(kGtpSessionMd, kGtpPackLengthErr);
    }

    GtpPacket *pack = (GtpPacket*)move_pos;

    pack->goodtp_ver_     = (u8)GTP_VERSION;
    pack->header_offset_  = (u8)hdr_size;
    pack->has_loss_flag_  = (u8)has_loss;
    pack->pack_type_      = (u8)(GtpPackType::kGtpDataPackType);
    pack->pack_size_      = (u16)(hdr_size + frame_size);
    pack->pack_sn_        = pack_sn_;
    pack->has_check_flag_ = (u8)has_check;
    pack->has_ts_flag_    = (u8)has_ts;
    pack->has_rtt_flag_   = (u8)has_rtt;
    pack->init_flag_      = (u8)((((u8)(GtpSessStat::kRunning)) == send_session_stat_) ? GTP_NO : GTP_YES);
    pack->repeat_counter_ = (u8)(resend_num & 0x00000007);
    pack->cache_us_flag_  = GTP_NO;
    pack->is_qos_flg_     = GTP_YES;
    pack->share_zone_     = pb_dt_.tran_addr_.qos_;
    pack->has_chg_zone_   = (u8)has_chg;
    pack->stream_type_    = pb_dt_.tran_addr_.stream_type_;

    #ifdef _SELFDEBUG
    GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
           "%s:%u<-->%s:%u send data packet(pack_sn=%u sort_sn=%u sort_sn_valid=%u repeat_num=%u "
           "first_sn=%u header_offset=%u pack_size=%u payload_size=%u loss_flag=%u check_flag=%u "
           "ts_flag=%u rtt_flag=%u qos=%u stream_type=%u key=%llu).\r\n",
           pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
           pack->pack_sn_, dbg_sort_sn, (u32)has_chg, (u32)(pack->repeat_counter_), first_pack_sn,
           (u32)(pack->header_offset_), (u32)(pack->pack_size_), frame_size, (u32)(pack->has_loss_flag_),
           (u32)(pack->has_check_flag_), (u32)(pack->has_ts_flag_), (u32)(pack->has_rtt_flag_),
           (u32)(pack->share_zone_), (u32)(pack->stream_type_),
           (unsigned long long)pb_dt_.tran_addr_.stream_key_);
    #endif

    pack_sn_ += 1;

    *out_pack      = pack;
    *out_pack_size = (u32)(pack->pack_size_);

    return GTP_OK;
}

u32 GtpSession::FramePrepHandlerWith01(void *frame, const u32 &frame_size, GtpPacket **out_pack,
                                       u32 *out_pack_size, const u32 &hash, const u32 &user_id,
                                       const u32 &first_pack_sn, const u32 &resend_num) {
    u32 hdr_size  = 0;
    u32 has_check = GTP_NO;
    u16 has_ts    = GTP_NO;
    u16 padding   = 0;
    u16 has_rtt   = GTP_NO;
    u16 has_loss  = GTP_NO;

    u8 *move_pos = (u8*)frame;

    hdr_size = CalcHeaderSizeVersion01(hash, user_id, resend_num);
    move_pos -= hdr_size;

    // padding for 8 or 4 bytes align.
    if (8 == sizeof(void*)) {
        padding = (u16)(AddressToUint(move_pos) & 0x0000000000000007);
    } else {
        padding = (u16)(AddressToUint(move_pos) & 0x00000003);
    }

    hdr_size = 0;
    move_pos = (u8*)frame;

    if (0 != padding) {
        hdr_size += padding;
        move_pos -= padding;

        memset(move_pos, 0x00, padding);
    }

    if (-1.0 < send_down_loss_) {
        has_loss  = GTP_YES;
        move_pos -= sizeof(send_down_loss_);
        hdr_size += sizeof(send_down_loss_);

        GtpWriteF32Unaligned(move_pos, send_down_loss_);
        send_down_loss_   = -1.0;
    }

    if (measure_rtt_ts_us_ <= last_active_ts_us_) {
        has_ts    = GTP_YES;
        move_pos -= sizeof(last_active_ts_us_);
        hdr_size += sizeof(last_active_ts_us_);

        if (0 == resend_num) {
            GtpWriteU64Unaligned(move_pos, last_active_ts_us_);  // last_active_ts_us_ is too old when retransporting.
        } else {
            GtpWriteU64Unaligned(move_pos, GtpSysTimestampUs());
        }

        measure_rtt_ts_us_ = last_active_ts_us_ + ((u64)test_rtt_period_us_);   // current session is sending.
    }

    if (0 != rtt_us_) {
        #ifdef _SELFDEBUG
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug, "%s:%u<-->%s:%u send to receive windows's "\
               "rrt=%lluus.\r\n", pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
               pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), (unsigned long long)rtt_us_);
        #endif
        has_rtt   = GTP_YES;
        move_pos -= sizeof(rtt_us_);
        hdr_size += sizeof(rtt_us_);

        GtpWriteU32Unaligned(move_pos, rtt_us_);
        rtt_us_            = 0;
    }

    if ((0 != user_id) || (0 != hash)) {
        has_check  = GTP_YES;
        move_pos  -= sizeof(user_id);
        hdr_size  += sizeof(user_id);

        GtpWriteU32Unaligned(move_pos, user_id);

        move_pos  -= sizeof(hash);
        hdr_size  += sizeof(hash);

        GtpWriteU32Unaligned(move_pos, hash);
    }

    if (0 < resend_num) {
        move_pos -= sizeof(first_pack_sn);
        hdr_size += sizeof(first_pack_sn);

        GtpWriteU32Unaligned(move_pos, first_pack_sn);
    }

    move_pos -= sizeof(GtpPacket);
    hdr_size += sizeof(GtpPacket);

    if (64 <= hdr_size) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "%s:%u<-->%s:%u packet's header is too long("\
               "header_offset=%u peer_version=0x%02x resend_num=%u frame_size=%u pack_size=%u.\r\n",
               pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
               hdr_size, (u32)(pb_dt_.peer_version_), resend_num, frame_size, *out_pack_size);
        RETURN_ERR(kGtpSessionMd, kGtpPackLengthErr);
    }

    if (0x0FFF < (hdr_size + frame_size)) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "%s:%u<-->%s:%u packet is too long("\
               "header_offset=%u peer_version=0x%02x resend_num=%u frame_size=%u pack_size=%u).\r\n",
               pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
               hdr_size, (u32)(pb_dt_.peer_version_), resend_num, frame_size, hdr_size + frame_size);
        RETURN_ERR(kGtpSessionMd, kGtpPackLengthErr);
    }

    GtpPacket *pack = (GtpPacket*)move_pos;

    pack->goodtp_ver_      = (u8)0x01;
    pack->header_offset_   = (u8)hdr_size;
    pack->has_loss_flag_   = (u8)has_loss;
    pack->pack_type_       = (u8)(GtpPackType::kGtpDataPackType);
    pack->pack_size_       = (u16)(hdr_size + frame_size);
    pack->pack_sn_         = pack_sn_;
    pack->has_check_flag_  = (u8)has_check;
    pack->has_ts_flag_     = (u8)has_ts;
    pack->has_rtt_flag_    = (u8)has_rtt;
    pack->init_flag_       = (u8)((((u8)(GtpSessStat::kRunning)) == send_session_stat_) ? GTP_NO : GTP_YES);
    pack->repeat_counter_  = (u8)(resend_num & 0x00000007);
    pack->cache_us_flag_   = GTP_NO;
    pack->is_qos_flg_      = GTP_YES;
    pack->share_zone_      = pb_dt_.tran_addr_.qos_;
    pack->has_chg_zone_    = GTP_NO;
    pack->stream_type_     = kRealTimeStream;

    pack_sn_ += 1;

    *out_pack      = pack;
    *out_pack_size = (u32)(pack->pack_size_);

    return GTP_OK;
}

u32 GtpSession::FilterRealtimeNackOffsets(const NackData *nack_data, u16 *out_nack, const u32 &max_nack_num,
                                           const u64 &ts_us) const {
    if ((NULL == nack_data) || (NULL == out_nack) || (0 == max_nack_num)) {
        return 0;
    }

    const u32 nack_num = (u32)nack_data->nack_num_;
    if (0 == nack_num) {
        return 0;
    }

    const u32 copy_num = (nack_num < max_nack_num) ? nack_num : max_nack_num;
    for (u32 i = 0; i < copy_num; ++i) {
        out_nack[i] = nack_data->nack_[i].sn_offset_;
    }
    (void)ts_us;
    return copy_num;
}

u32 GtpSession::FramePrepHandlerWithPeerVer(void *frame, const u32 &frame_size, GtpPacket **out_pack,
                                            u32 *out_pack_size, const u32 &hash, const u32 &user_id,
                                            const u32 &first_pack_sn, const u32 &resend_num) {
    u32 nret = GTP_OK;

    switch (pb_dt_.peer_version_) {
    case 0x01: {
        nret = FramePrepHandlerWith01(frame, frame_size, out_pack, out_pack_size, hash, user_id,
                                      first_pack_sn, resend_num);
        break;
    }

    case 0x02: {
        nret = FramePrepHandlerWithSelfVer(frame, frame_size, out_pack, out_pack_size, hash, user_id,
                                           first_pack_sn, resend_num);
        break;
    }

    default: {
        nret = GEN_ERR(kGtpSessionMd, kUnknownGtpVerErr);
    }
    }

    return nret;
}

u32 GtpSession::FrameFecEncodeHandler(GtpPacket *pack) {
    u32 run_result = GTP_OK;

    #if (1 == ENABLE_FEC)
    if ((0x00 != pb_dt_.peer_version_) && (GTP_ON == pb_dt_.alg_top_switch_)
     && (0 == ((u8)(pack->repeat_counter_)))
     && ((u8)STREAM_QOS_WITH_FEC == GetStreamQos(kSenderQuality))) {
        run_result = fec2_obj_.Encode(pack, last_active_ts_us_);
        if (GTP_OK != run_result) {
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "%s:%u<-->%s:%u(new fec) calling Encode() "
                   "failed(0x%08x).\r\n", pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
                   pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), run_result);
        }
    }
    #endif

    return run_result;
}

u32 GtpSession::FramePostHandler(GtpAddr *tran_addr, GtpPacket *pack, const u32 &pack_size,
             const u32 &payload_size, const u32 &entry_arq_flag, const u32 &first_pack_sn,
             const u32 &edge_pack_flag, const u32 &fec_encoded_flag) {
    pb_dt_.send_stat_.net_pack_sum_    += 1;
    pb_dt_.send_stat_.net_bitrate_sum_ += pack_size;
    pb_dt_.send_stat_.data_pack_sum_    += 1;
    pb_dt_.send_stat_.data_bitrate_sum_ += pack_size;

    if (0 == ((u8)(pack->repeat_counter_))) {
        pb_dt_.send_stat_.first_frame_sum_  += 1;
    } else {
        pb_dt_.send_stat_.retran_frame_sum_ += 1;
    }

    u32 run_result = GTP_OK;

    // step1: FEC encode.
    if (GTP_YES != fec_encoded_flag) {
        run_result = FrameFecEncodeHandler(pack);
    }

    // step2: process send slid window.
    run_result = SnTsEntrySlidWin(data_win_s_, pack->pack_sn_, last_active_ts_us_);
    if (GTP_OK != run_result) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "%s:%u<-->%s:%u calling SnTsEntrySlidWin() "\
               "failed(0x%08x).\r\n", pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
               pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), run_result);
    }

    if (GTP_NO == entry_arq_flag) {
        return GTP_OK;
    }

    // step3: process arq.
    #if (1 == ENABLE_ARQ)
    if (GTP_ON == pb_dt_.alg_top_switch_) {
        arq_.PacketEntryList(pack, tran_addr, first_pack_sn, last_active_ts_us_);
    }
    #endif

    return GTP_OK;
}

u32 GtpSession::PackPrepHandler(GtpPacket *pack, const u32 &size, u8 **out_frame, u32 *out_frame_size) {
    u32 nret         = GTP_OK;
    u32 frame_size   = size;
    u8 *frame_header = (u8*)pack;
    u32 first_sn     = 0;

    *out_frame      = NULL;
    *out_frame_size = 0;

    switch (pack->pack_type_) {
        case ((u8)(GtpPackType::kGtpDataPackType)): {  // bussiness receiving end.
            if ((GTP_YES == pack->is_qos_flg_) && (pb_dt_.tran_addr_.qos_ != pack->share_zone_)) {
                pb_dt_.tran_addr_.qos_ = pack->share_zone_;
            }

            recv_idle_calc_loss_ts_us = last_active_ts_us_;

            if (GTP_YES == pack->init_flag_) {
                if (((u8)(GtpSessStat::kRunning)) == recv_session_stat_) {
                    u32 sort_sn = 0;
                    u32 sort_sn_valid = GTP_NO;
                    if (0x02 <= pack->goodtp_ver_) {
                        if (GTP_YES == pack->has_chg_zone_) {
                            const ChangeZone *chg_zone = GtpGetPacketChangeZone(pack);
                            if (NULL != chg_zone) {
                                sort_sn = chg_zone->sort_sn_;
                                sort_sn_valid = GTP_YES;
                            }
                        }
                    }

                    u32 sync_session = GTP_NO;
                    if ((recv_last_sync_ts_us_ + MIN_SYNC_PERIOD_US) <= last_active_ts_us_) {
                        sync_session = GTP_YES;
                    } else if ((0 == ((u8)(pack->repeat_counter_))) && (last_sn_ > pack->pack_sn_)
                            && (GTP_YES != CheckIntTurnOver(last_sn_, pack->pack_sn_,
                                                            MAX_RCV_PACK_SN_UPDATE_STEP))) {
                        const u32 delta_sn = last_sn_ - pack->pack_sn_;
                        if (ai_learn_sn_delta_ <= delta_sn) {
                            sync_session = GTP_YES;
                        }
                    }

                    if (GTP_YES == sync_session) {
                        ResetSession(pack->pack_sn_, sort_sn, sort_sn_valid, GTP_YES);
                    }
                }

                goto filter_repeat_start_pos_;
            }

            if (((u8)(GtpSessStat::kRunning)) != recv_session_stat_) {
                recv_session_stat_    = (u8)(GtpSessStat::kRunning);
                recv_last_sync_ts_us_ = last_active_ts_us_;
            }

filter_repeat_start_pos_:
            if (MAX_KEEPALIVE_TIME_LEN_US > (last_active_ts_us_ - last_recv_data_pack_ts_us_)) {
                if (0 != ((u8)(pack->repeat_counter_))) {
                    if ((last_sn_ <= pack->pack_sn_)
                     || (GTP_YES == CheckIntTurnOver(last_sn_, pack->pack_sn_, MAX_RCV_PACK_SN_UPDATE_STEP))) {
                        last_sn_ = pack->pack_sn_;
                    }
                } else {
                    u32 delta_sn = 0;
                    if (last_sn_ <= pack->pack_sn_) {
                        delta_sn = pack->pack_sn_ - last_sn_;
                        goto check_mix_break_pos_;
                    }

                    if (GTP_YES == CheckIntTurnOver(last_sn_, pack->pack_sn_, MAX_RCV_PACK_SN_UPDATE_STEP)) {
                        goto update_last_sn_pos_;
                    }

                    delta_sn = last_sn_ - pack->pack_sn_;

check_mix_break_pos_:
                    if (ai_learn_sn_delta_ <= delta_sn) {
                        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "%s:%u---->%s:%u(sfd=%u) quintuple "\
                               "may be mixed or session may be break(ai_learn_sn_delta=%u delta_sn=%u last_sn=%u "\
                               "now_sn=%u repeat_num=%u).\r\n", pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
                               pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), pb_dt_.tran_addr_.sfd_, ai_learn_sn_delta_,
                               delta_sn, last_sn_, pack->pack_sn_, (u32)(pack->repeat_counter_));

                        u32 sort_sn = 0;
                        u32 sort_sn_valid = GTP_NO;
                        if (0x02 <= pack->goodtp_ver_) {
                            if (GTP_YES == pack->has_chg_zone_) {
                                const ChangeZone *chg_zone = GtpGetPacketChangeZone(pack);
                                if (NULL != chg_zone) {
                                    sort_sn = chg_zone->sort_sn_;
                                    sort_sn_valid = GTP_YES;
                                }
                            }
                        }

                        // isn't mixed quintuple, force sync the slid window.
                        ResetSession(pack->pack_sn_, sort_sn, sort_sn_valid, GTP_NO);
                    }

update_last_sn_pos_:
                    last_sn_ = pack->pack_sn_;
                }
            } else if (0 == ((u8)(pack->repeat_counter_))) {
                last_sn_ = pack->pack_sn_;
            }

            last_recv_data_pack_ts_us_ = last_active_ts_us_;

            if ((0x02 <= pack->goodtp_ver_) && (GTP_YES == pack->has_chg_zone_)) {
                const ChangeZone *chg_zone = GtpGetPacketChangeZone(pack);
                if (NULL != chg_zone) {
                    first_sn = chg_zone->sort_sn_;
                }
            } else {
                if (0x00 == pack->goodtp_ver_) {
                    first_sn = GtpReadU32Unaligned(((u8*)pack) + pack->header_offset_);
                    goto filter_repeat_judge_pos_;
                }

                if (0 == pack->repeat_counter_) {
                    first_sn = pack->pack_sn_;
                } else {
                    first_sn = GtpReadU32Unaligned(((u8*)pack) + sizeof(GtpPacket));
                }
            }

filter_repeat_judge_pos_:
            #ifdef _SELFDEBUG
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
                   "%s:%u---->%s:%u receive data packet(pack_sn=%u first_sn=%u sort_sn_valid=%u repeat_num=%u "
                   "header_offset=%u pack_size=%u payload_size=%u loss_flag=%u check_flag=%u "
                   "ts_flag=%u rtt_flag=%u qos=%u stream_type=%u key=%llu).\r\n",
                   pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
                   pack->pack_sn_, first_sn,
                   (u32)(((0x02 <= pack->goodtp_ver_) && (GTP_YES == pack->has_chg_zone_)) ? GTP_YES : GTP_NO),
                   (u32)(pack->repeat_counter_), (u32)(pack->header_offset_), (u32)(pack->pack_size_),
                   (u32)(frame_size - pack->header_offset_), (u32)(pack->has_loss_flag_),
                   (u32)(pack->has_check_flag_), (u32)(pack->has_ts_flag_), (u32)(pack->has_rtt_flag_),
                   (u32)(pack->share_zone_), (u32)(pack->stream_type_),
                   (unsigned long long)pb_dt_.tran_addr_.stream_key_);
            #endif

            nret = RepeatPacketFilter(filter_win_, first_sn, last_active_ts_us_);
            if (GTP_YES == nret) {
                #if (((__linux__ || __APPLE__) && (_SELFDEBUG)) || (_WIN32 || _WIN64))
                GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelInfo,
                       "%s:%u---->%s:%u repeat packet(pack_sn=%u first_sn=%u ret=%u).\r\n",
                       pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), pb_dt_.self_ip_,
                       (u32)(pb_dt_.self_port_), pack->pack_sn_, first_sn, nret);
                #endif
                nret = GEN_ERR(kGtpSessionMd, kDuplicatePackErr);
                break;
            }

            nret = SnEntrySlidWin(filter_win_, first_sn, last_active_ts_us_);
            if (GTP_OK != nret) {
                GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                       "%s:%u---->%s:%u Call SnEntrySlidWin() failed(0x%08x) first_sn=%u.\r\n",
                       pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), pb_dt_.self_ip_,
                       (u32)(pb_dt_.self_port_), nret, first_sn);
            }

            frame_header += pack->header_offset_;
            frame_size   -= pack->header_offset_;

            *out_frame      = frame_header;
            *out_frame_size = frame_size;

            break;
        }

        case ((u8)(GtpPackType::kGtpAckPackType)): {
            if (GTP_YES == pack->init_flag_) {
                if (((u8)(GtpSessStat::kRunning)) != send_session_stat_) {
                    send_session_stat_ = (u8)(GtpSessStat::kRunning);
                }
            }
            break;
        }

        case ((u8)(GtpPackType::kGtpFecPackType)): {
            break;
        }

        case ((u8)(GtpPackType::kGtpSetRecvRttPackType)): {
            if ((GTP_YES != pack->init_flag_) && (((u8)(GtpSessStat::kRunning)) != recv_session_stat_)) {
                recv_session_stat_    = (u8)(GtpSessStat::kRunning);
                recv_last_sync_ts_us_ = GtpSysTimestampUs();
            }

            break;
        }

        case ((u8)(GtpPackType::kGtpRttTstResPackType)): {
            if (GTP_YES == pack->init_flag_) {
                if (((u8)(GtpSessStat::kRunning)) != send_session_stat_) {
                    send_session_stat_ = (u8)(GtpSessStat::kRunning);
                }
            }
            break;
        }

        case ((u8)(GtpPackType::kGtpNackPackType)): {
            if (GTP_YES == pack->init_flag_) {
                if (((u8)(GtpSessStat::kRunning)) != send_session_stat_) {
                    send_session_stat_ = (u8)(GtpSessStat::kRunning);
                }
            }
            break;
        }

        default: {
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "%s:%u---->%s:%u unknown packet "\
                   "type(0x%02x).\r\n", pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
                   pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), (u32)(pack->pack_type_));
            nret = GEN_ERR(kGtpMgrMd, kUnknownPackTypeErr);
        }
    }

    return nret;
}

u32 GtpSession::PackPostHandler(GtpPacket *pack, const u32 &size, GtpAddr *tran_addr, const u32 &edge_pack_flag) {
    pb_dt_.recv_stat_.net_pack_sum_    += 1;
    pb_dt_.recv_stat_.net_bitrate_sum_ += size;

    u32 run_result  = GTP_OK;
    u32 return_code = GTP_OK;

    #ifdef _SELFDEBUG
    if (((u8)(GtpPackType::kGtpDataPackType)) != pack->pack_type_) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
               "%s:%u---->%s:%u receive control packet(type=%s/%u pack_sn=%u repeat_num=%u "
               "header_offset=%u pack_size=%u loss_flag=%u check_flag=%u ts_flag=%u rtt_flag=%u "
               "stream_type=%u key=%llu).\r\n",
               pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
               GtpPackTypeName(pack->pack_type_), (u32)(pack->pack_type_), pack->pack_sn_,
               (u32)(pack->repeat_counter_), (u32)(pack->header_offset_), size, (u32)(pack->has_loss_flag_),
               (u32)(pack->has_check_flag_), (u32)(pack->has_ts_flag_), (u32)(pack->has_rtt_flag_),
               (u32)(pack->stream_type_), (unsigned long long)pb_dt_.tran_addr_.stream_key_);
    }
    #endif

    switch (pack->pack_type_) {
        case ((u8)(GtpPackType::kGtpDataPackType)): {  // bussiness receiving end.
            pb_dt_.recv_stat_.data_pack_sum_    += 1;
            pb_dt_.recv_stat_.data_bitrate_sum_ += size;  // It doesn't include FEC.

            if (0 == ((u8)(pack->repeat_counter_))) {
                pb_dt_.recv_stat_.first_frame_sum_  += 1;
            } else {
                pb_dt_.recv_stat_.retran_frame_sum_ += 1;
            }

            #if (1 == ENABLE_FEC)
            #ifndef _UTTEST
            if ((0x00 != pb_dt_.peer_version_) && ((u8)STREAM_QOS_WITH_FEC == GetStreamQos())) {
                run_result = fec2_obj_.CacheDataPack(pack, last_active_ts_us_);
                if (GTP_OK != run_result) {
                    GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                           "new fec: calling CacheDataPack() failed(0x%08x).\r\n", run_result);
                }
            }
            #endif
            #endif

            // u32 hash     = 0;
            // u32 user_id  = 0;
            // u32 first_sn = 0;
            u32 rtt_us    = 0;
            f32 send_loss = 0.0;

            u8 *move = ((u8*)pack) + sizeof(GtpPacket);

            if (0 != ((u8)(pack->repeat_counter_))) {
                // first_sn = *((u32*)move);
                move += sizeof(u32);
            }

            if (GTP_YES == pack->has_check_flag_) {
                // hash = *((u32*)move);
                move += sizeof(u32);

                // user_id = *((u32*)move);
                move += sizeof(u32);
            }

            if (GTP_YES == pack->has_rtt_flag_) {
                rtt_us = GtpReadU32Unaligned(move);
                move  += sizeof(u32);

                restore_factor1_tm_span_us_ = rtt_us;

                run_result = SetLinkerRttUs(data_win_r_, rtt_us, last_active_ts_us_);
                if (GTP_OK != run_result) {
                    GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                           "calling SetLinkerRttUs() failed(0x%08x).\r\n", run_result);
                }

                RttHandler(rtt_us);

                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
                       "%s:%u<-->%s:%u set receive windows's rrt=%uus(pack_type=%u).\r\n",
                       pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_,
                       (u32)(pb_dt_.peer_port_), rtt_us, (u32)(pack->pack_type_));
                #endif
            }

            if (GTP_YES == pack->has_ts_flag_) {
                com_cache_ts_us_    = GtpReadU64Unaligned(move);  // using for testing RTT.
                at_com_cache_ts_us_ = last_active_ts_us_;

                pb_dt_.CalcSysClockSync(com_cache_ts_us_, at_com_cache_ts_us_);

                move += sizeof(u64);
            }

            if (GTP_YES == pack->has_loss_flag_) {
                send_loss = GtpReadF32Unaligned(move);

                f32 factor = fac_.Factor1(send_loss, last_active_ts_us_);
                run_result = AdjustOptimizeLossFactor(data_win_r_, factor, last_active_ts_us_);
                if (GTP_OK != run_result) {
                    GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                           "calling AdjustOptimizeLossFactor() failed(0x%08x).\r\n", run_result);
                }

                next_calc_factor1_ts_us_ = ((u32)(last_active_ts_us_ & 0x00000000FFFFFFFF))
                                         + restore_factor1_tm_span_us_;

                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug, "[adjust]%s:%u<-->%s:%u factor=%f "\
                       "loss=%5.2f%%.\r\n", pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
                       pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), factor, send_loss);
                #endif

                move += sizeof(f32);
            }

            // current session is receiving.
            #ifdef _SELFDEBUG
            debug_feedback_reason_ = kGtpFeedbackDebugDataEntry;
            #endif
            run_result = SnEntrySlidWin(data_win_r_, pack->pack_sn_, last_active_ts_us_);
            if (GTP_OK != run_result) {
                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                       "calling SnEntrySlidWin() failed(0x%08x).\r\n", run_result);
                #endif
            }

            #if (2 == APPLICATION_TYPE)
            if ((0 != recv_max_data_sn_) && (1 < ((i32)(pack->pack_sn_ - recv_max_data_sn_)))) {
                gap_nack_hold_ticks_ = 2;
            }

            if ((0 == recv_max_data_sn_) || (0 < ((i32)(pack->pack_sn_ - recv_max_data_sn_)))) {
                recv_max_data_sn_ = pack->pack_sn_;
            }
            #endif

            break;
        }

        case ((u8)(GtpPackType::kGtpAckPackType)): {
            pb_dt_.recv_stat_.ack_sum_ += 1;

            GtpAckPacketLoad *ack_load = (GtpAckPacketLoad*)(((u8*)pack) + pack->header_offset_);

            u8 *chg_len_zone = ((u8*)ack_load) + sizeof(GtpAckPacketLoad);
            u64 cache_us     = 0;
            u64 now_us       = GtpSysTimestampUs();
            u32 rtt_us       = 0;
            u32 tail_sn      = 0;
            u32 rto_sn       = 0;

            tail_sn = ack_load->head_sn_ + ack_load->tail_sn_offset_;
            rto_sn  = ack_load->head_sn_ + ack_load->rto_sn_offset_;

            #ifdef _SELFDEBUG
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
                   "%s:%u---->%s:%u receive ack packet(head_sn=%u tail_sn=%u rto_sn=%u sn_size=%u "
                   "recv_loss=%u cache_us_flag=%u).\r\n",
                   pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
                   ack_load->head_sn_, tail_sn, rto_sn, (u32)(ack_load->sn_size_), ack_load->recv_loss_,
                   (u32)(pack->cache_us_flag_));
            #endif

            if (GTP_YES == pack->cache_us_flag_) {
                cache_us = GtpReadU64Unaligned(chg_len_zone);

                if (cache_us <= now_us) {
                    rtt_us = (u32)(now_us - cache_us);
                } else {
                    rtt_us = (u32)(cache_us - now_us);
                }

                rtt_us_ = rtt_us;

                gen_new_rtt_ts_us_ = now_us;

                RttHandler(rtt_us);

                chg_len_zone += sizeof(u64);
            }

            run_result = SetLinkerLoss(data_win_s_, ack_load->recv_loss_, now_us, rtt_us);
            if (GTP_OK != run_result) {
                GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                       "calling SetLinkerLoss() failed(0x%08x).\r\n", run_result);
                if (GTP_OK == return_code) {
                    return_code = run_result;
                }
            }

            run_result = ProcAckSnBitMap(chg_len_zone, ack_load->sn_size_, ack_load->head_sn_, tail_sn, rto_sn);
            #if (1 == ENABLE_ARQ)
            arq_.ProcAck(chg_len_zone, ack_load->sn_size_, ack_load->head_sn_, tail_sn, rto_sn,
                         ack_load->recv_loss_);
            #endif

            break;
        }

        case ((u8)(GtpPackType::kGtpFecPackType)): {
            #ifdef _SELFDEBUG
            Fec2CodePack *fec_pack = (Fec2CodePack*)pack;
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
                   "%s:%u---->%s:%u receive fec packet(pack_sn=%u book_id=%u encode_dir=%s/%u "
                   "encode_pos=%u encode_bitmap=0x%02x code_len=%u pack_size=%u key=%llu).\r\n",
                   pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
                   fec_pack->pack_sn_, (u32)(fec_pack->code_book_id_),
                   Fec2CodeDirToStr(fec_pack->fec_encode_dir_), (u32)(fec_pack->fec_encode_dir_),
                   (u32)(fec_pack->fec_encode_pos_), (u32)(fec_pack->encode_bit_map_),
                   (u32)(fec_pack->code_len_), size, (unsigned long long)pb_dt_.tran_addr_.stream_key_);
            #endif

            #if (1 == ENABLE_FEC)
            if ((0x00 != pb_dt_.peer_version_) && ((u8)STREAM_QOS_WITH_FEC == GetStreamQos())) {
                run_result = fec2_obj_.Decode((Fec2CodePack*)pack, last_active_ts_us_);
                if (GTP_OK != run_result) {
                    GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                           "new fec: calling Decode() failed(0x%08x).\r\n", run_result);
                    if (GTP_OK == return_code) {
                        return_code = run_result;
                    }
                }
            }
            #endif

            break;
        }

        case ((u8)(GtpPackType::kGtpSetRecvRttPackType)): {
            if (GTP_YES == pack->has_rtt_flag_) {
                u32 rtt_us  = 0;

                u8 *move = ((u8*)pack) + sizeof(GtpPacket);

                if (0 != ((u8)(pack->repeat_counter_))) {
                    move += sizeof(u32);
                }

                if (GTP_YES == pack->has_check_flag_) {
                    move += sizeof(u64);
                }

                rtt_us = GtpReadU32Unaligned(move);
                move  += sizeof(u32);

                restore_factor1_tm_span_us_ = rtt_us;

                run_result = SetLinkerRttUs(data_win_r_, rtt_us, last_active_ts_us_);
                if (GTP_OK != run_result) {
                    GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                           "calling SetLinkerRttUs() failed(0x%08x).\r\n", run_result);
                    if (GTP_OK == return_code) {
                        return_code = run_result;
                    }
                }

                RttHandler(rtt_us);

                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug, "%s:%u<-->%s:%u set receive windows's "\
                       "rrt=%uus, last_recv_ts=%lluus idle_calc_period=%lluus.\r\n", pb_dt_.self_ip_,
                       (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), rtt_us,
                       (unsigned long long)recv_idle_calc_loss_ts_us, (unsigned long long)recv_idle_calc_period_us_);
                #endif
            }

            break;
        }

        case ((u8)(GtpPackType::kGtpRttTstResPackType)): {
            u8 *move = ((u8*)pack) + sizeof(GtpPacket);

            if (0 != ((u8)(pack->repeat_counter_))) {
                move += sizeof(u32);
            }

            if (GTP_YES == pack->has_check_flag_) {
                move += sizeof(u64);
            }

            if (GTP_YES == pack->has_rtt_flag_) {
                move  += sizeof(u32);
            }

            if (GTP_YES == pack->has_ts_flag_) {
                u64 ts_us  = GtpReadU64Unaligned(move);
                u64 now_us = GtpSysTimestampUs();

                if (ts_us <= now_us) {
                    rtt_us_ = (u32)(now_us - ts_us);
                } else {
                    rtt_us_ = (u32)(ts_us - now_us);
                }

                run_result = SetLinkerRttUs(data_win_s_, rtt_us_, last_active_ts_us_);
                if (GTP_OK != run_result) {
                    GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                           "calling SetLinkerRttUs() failed(0x%08x).\r\n", run_result);
                    if (GTP_OK == return_code) {
                        return_code = run_result;
                    }
                }

                gen_new_rtt_ts_us_ = now_us;

                RttHandler(rtt_us_);

                move += sizeof(u64);
            }

            break;
        }

        case ((u8)(GtpPackType::kGtpNackPackType)): {
            pb_dt_.recv_stat_.nack_sum_ += 1;

            GtpNackPacketLoad *nack_load = (GtpNackPacketLoad*)(((u8*)pack) + pack->header_offset_);

            u8 *chg_len_zone = ((u8*)nack_load) + sizeof(GtpNackPacketLoad);
            u64 cache_us     = 0;
            u64 now_us       = GtpSysTimestampUs();
            u32 rtt_us       = 0;
            u32 tail_sn      = 0;
            u32 rto_sn       = 0;

            tail_sn = nack_load->head_sn_ + nack_load->tail_sn_offset_;
            rto_sn  = nack_load->head_sn_ + nack_load->rto_sn_offset_;

            #ifdef _SELFDEBUG
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
                   "%s:%u---->%s:%u receive nack packet(head_sn=%u tail_sn=%u rto_sn=%u nack_num=%u "
                   "recv_loss=%u cache_us_flag=%u).\r\n",
                   pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
                   nack_load->head_sn_, tail_sn, rto_sn, (u32)(nack_load->nack_num_), nack_load->recv_loss_,
                   (u32)(pack->cache_us_flag_));
            #endif

            if (GTP_YES == pack->cache_us_flag_) {
                cache_us = GtpReadU64Unaligned(chg_len_zone);

                if (cache_us <= now_us) {
                    rtt_us = (u32)(now_us - cache_us);
                } else {
                    rtt_us = (u32)(cache_us - now_us);
                }
                rtt_us_ = rtt_us;

                gen_new_rtt_ts_us_ = now_us;

                RttHandler(rtt_us);

                chg_len_zone += sizeof(u64);
            }

            run_result = SetLinkerLoss(data_win_s_, nack_load->recv_loss_, now_us, rtt_us);
            if (GTP_OK != run_result) {
                GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                       "calling SetLinkerLoss() failed(0x%08x).\r\n", run_result);
                if (GTP_OK == return_code) {
                    return_code = run_result;
                }
            }

            u32 head_sn  = nack_load->head_sn_;
            u32 rcv_loss = nack_load->recv_loss_;
            u32 nack_num = (u32)(nack_load->nack_num_);
            u32 nack_mem = sizeof(NackData) + (nack_num * sizeof(Nack));
            std::vector<u64> nack_cache((nack_mem + sizeof(u64) - 1) / sizeof(u64));
            NackData *nack_data = (NackData*)(&(nack_cache[0]));

            nack_data->head_sn_     = head_sn;
            nack_data->tail_sn_     = tail_sn;
            nack_data->recv_loss_   = rcv_loss;
            nack_data->rto_sn_      = rto_sn;
            nack_data->cache_ts_us_ = 0;
            nack_data->nack_num_    = (u16)nack_num;
            memcpy(nack_data->nack_, chg_len_zone, nack_num * sizeof(Nack));

            run_result = NackSnEntrySlidWin(data_win_s_, nack_data, last_active_ts_us_);
            if (GTP_OK != run_result) {
                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug, "[win_hdl=%p]%s:%u<-->%s:%u nack sn is "\
                       "invalid.\r\n", data_win_s_, pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
                       pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_));
                #endif
                break;
            }

            #if (1 == ENABLE_ARQ)
            arq_.ProcNack((u16*)nack_data->nack_, (u32)(nack_data->nack_num_), nack_data->head_sn_, nack_data->tail_sn_,
                          nack_data->rto_sn_);
            #endif

            break;
        }

        default: {
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "%s:%u---->%s:%u unknown packet "\
                   "type(0x%02x).\r\n", pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
                   pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), (u32)(pack->pack_type_));
            return_code = GEN_ERR(kGtpMgrMd, kUnknownPackTypeErr);

            break;
        }
    }

    return return_code;
}

u32 GtpSession::CalcRealtimeReorderBaseIntervalUs() const {
    const u32 avg_interval_us = realtime_reorder_win_.AvgIntervalUs();

    u32 pps = pb_dt_.recv_stat_.data_pack_pps_;
    if (pps < pb_dt_.send_stat_.data_pack_pps_) {
        pps = pb_dt_.send_stat_.data_pack_pps_;
    }

    if (0 == pps) {
        return avg_interval_us;
    }

    u32 pps_interval_us = 1000000U / pps;
    if (0 == pps_interval_us) {
        pps_interval_us = 1;
    }

    if ((0 != avg_interval_us) && (avg_interval_us < pps_interval_us)) {
        return avg_interval_us;
    }

    return pps_interval_us;
}

u32 GtpSession::CalcRealtimeReorderDeliverIntervalUs(const u64 &ts_us, const u32 &cache_count,
                                                     const u32 &wait_us) const {
    const u32 base_interval_us = CalcRealtimeReorderBaseIntervalUs();
    if (0 == base_interval_us) {
        return 0;
    }

    const u64 oldest_age_us = realtime_reorder_win_.OldestAgeUs(ts_us);
    const u32 age_packets = (u32)(oldest_age_us / base_interval_us);
    u32 pressure = cache_count + age_packets;
    if ((0 != wait_us) && (oldest_age_us > wait_us)) {
        pressure += (u32)(((oldest_age_us - wait_us) / base_interval_us) * 4ULL) + 4U;
    }

    u32 accel_q8 = 256U + (pressure * 256U) / 3U;
    if (3072U < accel_q8) {
        accel_q8 = 3072U;
    }

    u32 deliver_interval_us = (u32)(((u64)base_interval_us * 256ULL) / accel_q8);
    return (0 == deliver_interval_us) ? 1 : deliver_interval_us;
}

u32 GtpSession::CalcRealtimeReorderWaitUs() const {
    u32 wait_us = realtime_reorder_win_.AvgIntervalUs();
    u32 pps = pb_dt_.recv_stat_.data_pack_pps_;
    if (pps < pb_dt_.send_stat_.data_pack_pps_) {
        pps = pb_dt_.send_stat_.data_pack_pps_;
    }

    if ((0 == wait_us) && (0 != pps)) {
        wait_us = 750000U / pps;
        if (0 == wait_us) {
            wait_us = 1;
        }
    }

    if (0 == wait_us) {
        return 0;
    }

    #if (2 == APPLICATION_TYPE)
    if (0 != pps) {
        const u32 interval_us = 1000000U / pps;
        u32 book_scale_num = 1;
        u32 book_scale_den = 1;

        switch (fec2_obj_.RecvFecBookId()) {
            case 0:
            case 1:
            case 3:
                book_scale_num = 2;
                break;
            case 4:
            case 6:
            case 7:
                book_scale_num = 3;
                book_scale_den = 2;
                break;
            case 2:
                book_scale_num = 2;
                break;
            case 5:
            default:
                break;
        }

        u32 fec_wait_us = 0;
        if (0 != interval_us) {
            const u64 density_ref_us = 8000ULL;
            const u64 density_wait_us = (density_ref_us * density_ref_us * book_scale_num)
                                      / (((u64)interval_us) * book_scale_den);
            fec_wait_us = (24000ULL < density_wait_us) ? 24000U : (u32)density_wait_us;
        }

        if (wait_us < fec_wait_us) {
            wait_us = fec_wait_us;
        }
    }
    #endif

    u32 max_wait_us = 30000;
    if (170 <= pps) {
        max_wait_us = 24000;
    } else if (110 <= pps) {
        max_wait_us = 20000;
    } else if (50 <= pps) {
        max_wait_us = 20000;
    }

    /* 上面这几档固定上限只是按 FEC 补齐节奏估的，跟真实 RTT 无关。真实 WiFi/公网下 ARQ 的
       NACK+重传往返经常超过 20-30ms，等待窗口不够长会频繁触发 skip-ahead，转化成不必要的乱序
       交付。这里用持续更新的 RTT 测量值（pb_dt_.rtt_us_，见 RttHandler()）把上限适度抬高，
       覆盖大约一次 NACK+重传往返；loopback/局域网 rtt_us_ 很小，这段基本不生效。 */
    if (0 != pb_dt_.rtt_us_) {
        const u32 rtt_based_wait_us = (pb_dt_.rtt_us_ * 3) / 2;
        if (max_wait_us < rtt_based_wait_us) {
            max_wait_us = rtt_based_wait_us;
        }
    }
    const u32 kAbsoluteWaitCeilingUs = 60000;
    if (max_wait_us > kAbsoluteWaitCeilingUs) {
        max_wait_us = kAbsoluteWaitCeilingUs;
    }

    if (max_wait_us < wait_us) {
        return max_wait_us;
    }

    return wait_us;
}

u32 GtpSession::CalcRealtimeReorderMaxCacheNum() const {
    u32 pps = pb_dt_.recv_stat_.data_pack_pps_;
    if (pps < pb_dt_.send_stat_.data_pack_pps_) {
        pps = pb_dt_.send_stat_.data_pack_pps_;
    }

    if (120 <= pps) {
        return 96;
    }

    if (50 <= pps) {
        return 64;
    }

    return 32;
}

u32 GtpSession::DeliverFrameNow(GtpHandler_p gtp_hdl, const u8 *frame, const u32 &frame_size, GtpAddr *tran_addr) {
    u32 ret_value = GTP_OK;

    #if (1 == ENABLE_MD_PERF_CHECK)
    {
    u64 back_recv_tm = pb_dt_.recv_consume_.ts_us_;
    u64 bkup_send_tm = pb_dt_.send_consume_.ts_us_;
    u64 app_consume_us = GtpSysTimestampUs();

    ret_value = cb_.receive_frame_cb_(gtp_hdl, (void*)frame, frame_size, tran_addr);
    app_consume_us = GtpSysTimestampUs() - app_consume_us;
    pb_dt_.app_recv_consume_.CacheConsumeTime(app_consume_us);

    pb_dt_.send_consume_.ts_us_ = bkup_send_tm + app_consume_us;
    pb_dt_.recv_consume_.ts_us_ = back_recv_tm + app_consume_us;
    }
    #else
    ret_value = cb_.receive_frame_cb_(gtp_hdl, (void*)frame, frame_size, tran_addr);
    #endif

    return ret_value;
}

u32 GtpSession::FlushRealtimeReorder(const u64 &ts_us, GtpHandler_p gtp_hdl) {
    u32 ret_value = GTP_OK;
    const u32 max_cache_num = CalcRealtimeReorderMaxCacheNum();
    const u32 wait_us       = CalcRealtimeReorderWaitUs();

    RealtimeReorderWindow::Frame frame;
    while (0 != realtime_reorder_win_.Count()) {
        const u32 cache_count_before = realtime_reorder_win_.Count();
        const u32 expect_sn_before = realtime_reorder_win_.ExpectSn();
        const bool expected_ready = realtime_reorder_win_.HasExpectedFrame();
        const u64 oldest_age_us_before = realtime_reorder_win_.OldestAgeUs(ts_us);
        const u32 deliver_interval_us = CalcRealtimeReorderDeliverIntervalUs(ts_us, cache_count_before, wait_us);
        if ((0 != deliver_interval_us) && expected_ready) {
            if (0 == realtime_reorder_next_deliver_ts_us_) {
                realtime_reorder_next_deliver_ts_us_ = ts_us;
            }

            if ((0 != wait_us) && (oldest_age_us_before > wait_us)
             && (realtime_reorder_next_deliver_ts_us_ > ts_us)) {
                realtime_reorder_next_deliver_ts_us_ = ts_us;
            }

            if (ts_us < realtime_reorder_next_deliver_ts_us_) {
                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
                       "%s:%u<-->%s:%u realtime reorder waits for pacing(expect_sn=%u expect_cached=%u "
                       "cache_num=%u wait=%uus interval=%uus now=%lluus next=%lluus oldest_age=%lluus).\r\n",
                       pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
                       expect_sn_before, (u32)expected_ready, cache_count_before, wait_us, deliver_interval_us,
                       (unsigned long long)ts_us, (unsigned long long)realtime_reorder_next_deliver_ts_us_,
                       (unsigned long long)oldest_age_us_before);
                #endif
                break;
            }
        }

        if (!realtime_reorder_win_.PopReady(ts_us, max_cache_num, wait_us, &frame)) {
            #ifdef _SELFDEBUG
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
                   "%s:%u<-->%s:%u realtime reorder waits for gap(expect_sn=%u expect_cached=%u "
                   "cache_num=%u wait=%uus now=%lluus oldest_age=%lluus).\r\n",
                   pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
                   expect_sn_before, (u32)expected_ready, cache_count_before, wait_us, (unsigned long long)ts_us,
                   (unsigned long long)oldest_age_us_before);
            #endif
            break;
        }

        const u32 cache_count_after = realtime_reorder_win_.Count();
        const u32 skipped_gap = (expect_sn_before == frame.sn_) ? 0 : (frame.sn_ - expect_sn_before);
        #ifdef _SELFDEBUG
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
               "%s:%u<-->%s:%u realtime reorder delivered packet(sn=%u expect_sn=%u expect_cached=%u "
               "skipped_num=%u cache_before=%u cache_after=%u wait=%uus interval=%uus oldest_age=%lluus).\r\n",
               pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
               frame.sn_, expect_sn_before, (u32)expected_ready, skipped_gap, cache_count_before, cache_count_after,
               wait_us, deliver_interval_us,
               (unsigned long long)realtime_reorder_win_.OldestAgeUs(ts_us));
        #endif

        ret_value = DeliverFrameNow(gtp_hdl, frame.frame_.data(), (u32)frame.frame_.size(), &frame.tran_addr_);
        if (GTP_OK != ret_value) {
            return ret_value;
        }

        if ((0 != deliver_interval_us) && (0 != realtime_reorder_win_.Count())) {
            if (realtime_reorder_next_deliver_ts_us_ < ts_us) {
                realtime_reorder_next_deliver_ts_us_ = ts_us;
            }
            realtime_reorder_next_deliver_ts_us_ += deliver_interval_us;
        } else {
            realtime_reorder_next_deliver_ts_us_ = 0;
        }
    }

    return ret_value;
}

u32 GtpSession::DeliverFrameInOrder(GtpHandler_p gtp_hdl, const u32 &first_sn, const u8 *frame,
                                    const u32 &frame_size, GtpAddr *tran_addr) {
    if ((kRealTimeStream != pb_dt_.tran_addr_.stream_type_) || (NULL == frame) || (0 == frame_size)) {
        return DeliverFrameNow(gtp_hdl, frame, frame_size, tran_addr);
    }

    const u64 ts_us = last_active_ts_us_;
    RealtimeReorderWindow::PushResult push_result =
        realtime_reorder_win_.Push(first_sn, frame, frame_size, *tran_addr, ts_us);
    if ((RealtimeReorderWindow::kPushDirect == push_result)
     || (RealtimeReorderWindow::kPushStaleDeliver == push_result)) {
        if (RealtimeReorderWindow::kPushStaleDeliver == push_result) {
            realtime_reorder_late_rescue_ += 1;
        }

        #ifdef _SELFDEBUG
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
               "%s:%u<-->%s:%u realtime reorder %s packet(sn=%u expect_sn=%u cache_num=%u now=%lluus).\r\n",
               pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
               (RealtimeReorderWindow::kPushDirect == push_result) ? "directly delivered" :
               "late-rescue delivered(out-of-order)",
               first_sn, realtime_reorder_win_.ExpectSn(), realtime_reorder_win_.Count(),
               (unsigned long long)ts_us);
        #endif
        u32 ret_value = DeliverFrameNow(gtp_hdl, frame, frame_size, tran_addr);
        if (GTP_OK != ret_value) {
            return ret_value;
        }
        return FlushRealtimeReorder(ts_us, gtp_hdl);
    }

    #ifdef _SELFDEBUG
    GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
           "%s:%u<-->%s:%u realtime reorder %s packet(sn=%u expect_sn=%u cache_num=%u now=%lluus).\r\n",
           pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
           (RealtimeReorderWindow::kPushCached == push_result) ? "cached" : "drop",
           first_sn, realtime_reorder_win_.ExpectSn(), realtime_reorder_win_.Count(), (unsigned long long)ts_us);
    #endif

    return FlushRealtimeReorder(ts_us, gtp_hdl);
}

void GtpSession::TimerHandler(const u64 &ts_us, ConsumeTime *wheel_consume) {
    pb_dt_.tran_addr_.timestamp_ = ts_us;

    #ifdef _SELFDEBUG
    debug_feedback_reason_ = kGtpFeedbackDebugUnknown;
    #endif

    (void)FlushRealtimeReorder(ts_us, app_gtp_hdl_);

    #if (1 == ENABLE_ARQ)
    if (NULL != arq_.arq_list_.head_) {
        arq_.CheckRtoRetran(ts_us);
    }
    #endif

    SecondTimerHandler(ts_us, wheel_consume);

    u32 nret;

    if (GTP_NO == report_quality_flag_s_) {
        u64 delta = ts_us - create_ts_us_;
        if ((DEFAULT_RTO_TIMEOUT_US - (DEFAULT_RTO_TIMEOUT_US >> 4)) > delta) {
            goto timer_handler_continue_pos_;
        }

        #ifdef _SELFDEBUG
        debug_feedback_reason_ = kGtpFeedbackDebugSenderTimer;
        #endif
        nret = CalcQualityByHandler(data_win_s_, ts_us, GTP_NO);
        if (GTP_OK != nret) {
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                   "calling CalcQualityByHandler() failed(0x%08x %s).\r\n",
                   nret, WinErrorInfo(data_win_s_, nret));
        }

        report_quality_flag_s_ = GTP_YES;

        #ifdef _SELFDEBUG
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelInfo, "[win_hdl=%p]%s:%u<-->%s:%u force calc "\
               "sender's quality(create=%lluus current=%lluus delta=%lluus).\r\n", data_win_s_, pb_dt_.self_ip_,
               (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
               (unsigned long long)create_ts_us_, (unsigned long long)ts_us, (unsigned long long)delta);
        #endif
    }

timer_handler_continue_pos_:
    {
        const u64 rtt_probe_interval = (recv_idle_calc_period_us_ > MIN_RTT_EXIST_TM_SZ_US)
                                       ? recv_idle_calc_period_us_ : MIN_RTT_EXIST_TM_SZ_US;
        if ((GTP_ON == pb_dt_.alg_top_switch_) && (0 != rtt_us_) && ((gen_new_rtt_ts_us_ + rtt_probe_interval) <= ts_us)) {
            SendSetRecvRttPacket();
        }
    }

    #if (2 == APPLICATION_TYPE)
    if ((GTP_ON == pb_dt_.alg_top_switch_) && (0 < gap_nack_hold_ticks_)) {
        gap_nack_hold_ticks_ -= 1;
        if (0 == gap_nack_hold_ticks_) {
            new_gap_detected_ = 3;
        }
    }
    if ((GTP_ON == pb_dt_.alg_top_switch_) && (0 < new_gap_detected_)) {
        new_gap_detected_ -= 1;
        #ifdef _SELFDEBUG
        debug_feedback_reason_ = kGtpFeedbackDebugGapTimer;
        #endif
        nret = CalcQualityByHandler(data_win_r_, ts_us, GTP_YES);
        if (GTP_OK != nret) {
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                   "calling CalcQualityByHandler() failed(0x%08x %s).\r\n",
                   nret, WinErrorInfo(data_win_r_, nret));
        }

        if (0xFFFFFFFFFFFFFFFF != recv_idle_calc_loss_ts_us) {
            recv_idle_calc_loss_ts_us = ts_us;
        }
    }
    #endif

    if ((GTP_ON == pb_dt_.alg_top_switch_)
     && (0xFFFFFFFFFFFFFFFF != recv_idle_calc_loss_ts_us)
     && ((recv_idle_calc_loss_ts_us + recv_idle_calc_period_us_) <= ts_us)) {
        const u32 burst_flag = nack_burst_detected_;
        nack_burst_detected_ = 0;

        if ((GTP_NO == burst_flag) && (MAX_FEEDBACK_NACK_PERIOD_US >= (ts_us - last_feedback_nack_ts_us_))) {
            #ifdef _SELFDEBUG
            debug_feedback_reason_ = kGtpFeedbackDebugIdleTimer;
            #endif
            nret = CalcQualityByHandler(data_win_r_, ts_us, GTP_NO);
        } else {
            #ifdef _SELFDEBUG
            debug_feedback_reason_ = kGtpFeedbackDebugIdleTimer;
            #endif
            nret = CalcQualityByHandler(data_win_r_, ts_us, GTP_YES);
        }

        if (GTP_OK != nret) {
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                   "calling CalcQualityByHandler() failed(0x%08x %s).\r\n",
                   nret, WinErrorInfo(data_win_r_, nret));
        }

        #ifdef _SELFDEBUG
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
               "[win_hdl=%p]%s:%u<-->%s:%u recv_idle_calc_ts=%lluus cur_ts=%lluus span=%lluus.\r\n", data_win_r_,
               pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_,
               (u32)(pb_dt_.peer_port_), (unsigned long long)recv_idle_calc_loss_ts_us,
               (unsigned long long)ts_us, (unsigned long long)recv_idle_calc_period_us_);
        #endif

        recv_idle_calc_loss_ts_us = ts_us;
    }

    if (next_calc_factor1_ts_us_ <= ((u32)(ts_us & 0x00000000FFFFFFFF))) {
        f32 factor = fac_.Factor1(0.0, ts_us);
        nret = AdjustOptimizeLossFactor(data_win_r_, factor, ts_us);
        if (GTP_OK != nret) {
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                   "calling AdjustOptimizeLossFactor() failed(0x%08x).\r\n", nret);
        }

        next_calc_factor1_ts_us_ = ((u32)(ts_us & 0x00000000FFFFFFFF)) + restore_factor1_tm_span_us_;
    }

    // clear fec expired resource.
    #if (1 == ENABLE_FEC)
    fec2_obj_.ClearResource(arq_.r_cur_loss_rate_, ts_us);
    #endif

    return;
}

u32 GtpSession::ProcAckSnBitMap(const u8 ack_sn_bitmap[], const u32 &bitmap_sz, const u32 &head_sn,
                                  const u32 &tail_sn, const u32 &rto_sn) {
    u32 int_sz      = 0;
    u32 outloop     = 0;
    u32 inloop      = 0;
    u32 nret        = GTP_OK;
    u32 ack_sn      = head_sn;
    u8  bit8_value  = 1;

    if (8 == sizeof(void*)) {  // 64 bit system.
        const u64 *pu64sn_bitmap = (const u64*)ack_sn_bitmap;

        int_sz = bitmap_sz >> 3;

        while (int_sz > outloop) {
            nret = BlockU64SnEntrySlidWin(data_win_s_, (*pu64sn_bitmap), ack_sn, last_active_ts_us_, GTP_NO);
            if (GTP_OK != nret) {
                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
                       "calling BlockU64SnEntrySlidWin() failed(0x%08x) sn=%u.\r\n", nret, ack_sn);
                #endif
            }

            ack_sn += 64;

            if (head_sn <= tail_sn) {
                if (ack_sn < (ack_sn - 64)) {
                    goto proc_ack_exit_pos_;
                } else if (tail_sn < ack_sn) {
                    goto proc_ack_exit_pos_;
                }
            } else {
                if (ack_sn < (ack_sn - 64)) {
                    if (tail_sn < ack_sn) {
                        goto proc_ack_exit_pos_;
                    }
                } else if ((tail_sn < ack_sn) && (head_sn > ack_sn)) {
                    goto proc_ack_exit_pos_;
                }
            }

            pu64sn_bitmap += 1;
            outloop       += 1;
        }

        if (0 == (bitmap_sz & 0x00000007)) {
            CalcQualityByHandler(data_win_s_, last_active_ts_us_, GTP_NO);
            goto proc_ack_exit_pos_;
        }

        outloop = (bitmap_sz & 0xFFFFFFF8);
    } else {  // 32 bit system.
        const u32 *pu32sn_bitmap = (const u32*)ack_sn_bitmap;

        int_sz = bitmap_sz >> 2;

        while (int_sz > outloop) {
            nret = BlockU32SnEntrySlidWin(data_win_s_, (*pu32sn_bitmap), ack_sn, last_active_ts_us_, GTP_NO);
            if (GTP_OK != nret) {
                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
                       "calling BlockU32SnEntrySlidWin() failed(0x%08x) sn=%u.\r\n", nret, ack_sn);
                #endif
            }

            ack_sn += 32;
            if (head_sn <= tail_sn) {
                if (ack_sn < (ack_sn - 32)) {
                    goto proc_ack_exit_pos_;
                } else if (tail_sn < ack_sn) {
                    goto proc_ack_exit_pos_;
                }
            } else {
                if (ack_sn < (ack_sn - 32)) {
                    if (tail_sn < ack_sn) {
                        goto proc_ack_exit_pos_;
                    }
                } else if ((tail_sn < ack_sn) && (head_sn > ack_sn)) {
                    goto proc_ack_exit_pos_;
                }
            }

            pu32sn_bitmap += 1;
            outloop       += 1;
        }

        if (0 == (bitmap_sz & 0x00000003)) {
            CalcQualityByHandler(data_win_s_, last_active_ts_us_, GTP_NO);
            goto proc_ack_exit_pos_;
        }

        outloop = (bitmap_sz & 0xFFFFFFFC);
    }

    while (bitmap_sz > outloop) {
        bit8_value = 1;
        inloop     = 0;

        while (8 > inloop) {
            if (0 != (ack_sn_bitmap[outloop] & bit8_value)) {
                nret = SnEntrySlidWin(data_win_s_, ack_sn, last_active_ts_us_);
                if (GTP_OK != nret) {
                    #ifdef _SELFDEBUG
                    GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
                           "calling SnEntrySlidWin() failed(0x%08x) sn=%u.\r\n", nret, ack_sn);
                    #endif
                }
            }

            bit8_value <<= 1;
            inloop      += 1;
            ack_sn      += 1;

            if (head_sn <= tail_sn) {
                if (ack_sn < ack_sn - 1) {
                    goto proc_ack_exit_pos_;
                } else if (tail_sn < ack_sn) {
                    goto proc_ack_exit_pos_;
                }
            } else if (tail_sn < ack_sn && head_sn > ack_sn) {
                goto proc_ack_exit_pos_;
            }
        }

        outloop += 1;
    }

proc_ack_exit_pos_:
    return GTP_OK;
}

void GtpSession::SecondTimerHandler(const u64 &cur_ts_us, ConsumeTime *wheel_consume) {
    if (second_ts_us_ > cur_ts_us) {
        return;
    }

    if (NULL != wheel_consume) {
        wheel_consume->CalcAvgConsume();
    }

    second_ts_us_ = cur_ts_us + 1000000;

    pb_dt_.SecondTimerHandler(cur_ts_us);

    #if (2 == APPLICATION_TYPE)
    // pb_dt_.max_send_loss_per_s_ now holds the peak loss seen in the second that just
    // ended. A single isolated burst only pushes this past 10% for the one second it
    // happened in; a genuinely degrading link keeps doing it every second.
    if (10.0f <= pb_dt_.max_send_loss_per_s_) {
        if (250 > elevated_loss_streak_) {
            elevated_loss_streak_ += 1;
        }
    } else {
        elevated_loss_streak_ = 0;
    }
    #endif

    ai_learn_sn_delta_ = pb_dt_.recv_stat_.data_pack_pps_ + (pb_dt_.recv_stat_.data_pack_pps_ >> 1);
    if (MIN_MIX_QUINTUPLET_THRESHOLD > ai_learn_sn_delta_) {
        ai_learn_sn_delta_ = MIN_MIX_QUINTUPLET_THRESHOLD;
    }

    if ((GTP_YES == max_frm_prd_updt_flg_) && (last_max_frame_period_us_ != now_max_frame_period_us_)) {
        last_max_frame_period_us_ = now_max_frame_period_us_;
        now_max_frame_period_us_  = 0;
        max_frm_prd_updt_flg_     = GTP_NO;

        if (max_peak_frame_period_us_ < last_max_frame_period_us_) {
            max_peak_frame_period_us_ = last_max_frame_period_us_;
        }

        if (MIN_FRAME_PERIOD_US > last_max_frame_period_us_) {
            last_max_frame_period_us_ = MIN_FRAME_PERIOD_US;
        }

        #if (1 == ENABLE_FEC)
        // TODO(Albert.feng) :: set the frame's period to fec2.
        #endif
    }

    return;
}

void GtpSession::AddNetStat(GtpPacket *pack, const u32 &pack_size) {
    pb_dt_.send_stat_.net_pack_sum_    += 1;
    pb_dt_.send_stat_.net_bitrate_sum_ += pack_size;

    return;
}

void GtpSession::CalcSessionQualityBfDead(const u64 &cur_ts_us) {
    u32 nret = CalcQualityByHandler(data_win_s_, cur_ts_us, GTP_NO);
    if (GTP_OK != nret) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "calling CalcQualityByHandler() failed(0x%08x %s).\r\n", nret, WinErrorInfo(data_win_s_, nret));
    }

    nret = CalcQualityByHandler(data_win_r_, cur_ts_us, GTP_NO);
    if (GTP_OK != nret) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "calling CalcQualityByHandler() failed(0x%08x %s).\r\n", nret, WinErrorInfo(data_win_r_, nret));
    }

    #ifdef _SELFDEBUG
    GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelInfo,
           "%s:%u<-->%s:%u mode=%s create=%lluus last_active=%lluus current=%lluus ttl=%uus delta=%lluus dead by "\
           "handler.\r\n", pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_,
           (u32)(pb_dt_.peer_port_),
           ((((u32)(GtpSessionMode::kSender)) == mode_) ? "send":"receive"), (unsigned long long)create_ts_us_,
           (unsigned long long)last_active_ts_us_, (unsigned long long)cur_ts_us, self_session_ttl_us_,
           (unsigned long long) (cur_ts_us - last_active_ts_us_));
    #endif

    return;
}

u32 GtpSession::GetQuality(const GtpNetQualityPosEnU32 &pos, GtpLinkQuality *quality) {
    u32 rrt_us    = 0;
    u32 jitter_us = 0;

    u32 rto_us    = 0;
    u32 nret      = GTP_OK;

    quality->self_ip_       = &(pb_dt_.self_ip_[0]);
    quality->peer_ip_       = &(pb_dt_.peer_ip_[0]);
    quality->self_port_     = pb_dt_.self_port_;
    quality->peer_port_     = pb_dt_.peer_port_;
    quality->bitrage_chg_k_ = 0;
    quality->rsv_           = 0;
    quality->context_       = pb_dt_.tran_addr_.context_;

    if (kSenderQuality == pos) {
        quality->report_pos_       = kSenderQuality;
        quality->data_bitrate_bps_ = pb_dt_.send_stat_.data_bitrate_bps_;
        quality->net_bitrate_bps_  = pb_dt_.send_stat_.net_bitrate_bps_;
        quality->net_pack_pps_     = pb_dt_.send_stat_.net_pack_pps_;
        quality->total_frame_num_  = pb_dt_.send_stat_.first_frame_sum_;
        quality->total_pack_num_   = pb_dt_.send_stat_.data_pack_sum_;
        quality->total_ack_num_    = pb_dt_.recv_stat_.ack_sum_;
        quality->total_retran_num_ = pb_dt_.send_stat_.retran_frame_sum_;

        arq_.GetStat(&(quality->total_ack_loss_num_), &(quality->total_rto_loss_num_),
                     &(quality->total_ack_err_num_), &(quality->total_ai_repair_num_));

        nret = ObtainNetworkQuality(data_win_s_, &(quality->loss_), &rrt_us, &jitter_us, &(quality->pre_congest_rank_),
                                    &rto_us, &(quality->data_pack_pps_), &(quality->loss_direct_));

        quality->data_pack_pps_ = pb_dt_.send_stat_.data_pack_pps_;
        quality->rtt_ms_        = (u16)(rrt_us / 1000);
        quality->rtt_jitter_ms_ = (u16)(jitter_us / 1000);
    } else {
        quality->report_pos_       = kReceiverQuality;
        quality->data_bitrate_bps_ = pb_dt_.recv_stat_.data_bitrate_bps_;
        quality->net_bitrate_bps_  = pb_dt_.recv_stat_.net_bitrate_bps_;
        quality->net_pack_pps_     = pb_dt_.recv_stat_.net_pack_pps_;
        quality->total_frame_num_  = pb_dt_.recv_stat_.first_frame_sum_;
        quality->total_pack_num_   = pb_dt_.recv_stat_.data_pack_sum_;
        quality->total_ack_num_    = pb_dt_.send_stat_.ack_sum_;
        quality->total_retran_num_ = pb_dt_.recv_stat_.retran_frame_sum_;

        arq_.GetStat(&(quality->total_ack_loss_num_), &(quality->total_rto_loss_num_),
                     &(quality->total_ack_err_num_), &(quality->total_ai_repair_num_));

        nret = ObtainNetworkQuality(data_win_r_, &(quality->loss_), &rrt_us, &jitter_us, &(quality->pre_congest_rank_),
                                    &rto_us, &(quality->data_pack_pps_), &(quality->loss_direct_));

        quality->data_pack_pps_ = pb_dt_.recv_stat_.data_pack_pps_;
        quality->rtt_ms_        = (u16)(rrt_us / 1000);
        quality->rtt_jitter_ms_ = (u16)(jitter_us / 1000);
    }

    return nret;
}

u32 GtpSession::AppendSessionAtrribute(u8 *out_str, const u32 &mem_size, u32 *str_size) {
    u8 *wrt_pos  = out_str;
    u32 empty_sz = mem_size;

    u32 wrt_num  = (u32)snprintf((char*)wrt_pos, empty_sz,
                                 "session(%s:%u<-->%s:%u mode=%s send_status=%u recv_status=%u):\r\n"\
                                 "recv_idle_calc_loss_ts_us= %lluus\r\n"\
                                 "recv_idle_calc_period_us_= %lluus\r\n"\
                                 "           session_ttl_us= %uus\r\n"\
                                 "              rto_timeout= %uus\r\n"\
                                 "       max_frame_period/s= %uus\r\n"\
                                 "    max_peak_frame_period= %uus\r\n"\
                                 "       swin_cur_loss_rate= %.2f%%\r\n"\
                                 "       rwin_cur_loss_rate= %.2f%%\r\n"\
                                 "            swin_loss_dir= %s\r\n"\
                                 "            rwin_loss_dir= %s\r\n"\
                                 "         max_retran_times= %u\r\n"\
                                 "          ack_err_counter= %u\r\n"\
                                 "       ack_resend_counter= %u\r\n"\
                                 "       rto_resend_counter= %u\r\n"\
                                 "     boost_resend_counter= %u", pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
                                 pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
                                 ((((u8)(GtpSessionMode::kSender)) == mode_) ? "sender":"receiver"),
                                 (u32)send_session_stat_, (u32)recv_session_stat_,
                                 (unsigned long long)recv_idle_calc_loss_ts_us,
                                 (unsigned long long)recv_idle_calc_period_us_,
                                 self_session_ttl_us_, arq_.rto_timeout_us_,
                                 last_max_frame_period_us_, max_peak_frame_period_us_, arq_.s_cur_loss_rate_,
                                 arq_.r_cur_loss_rate_, LossDirToStr(arq_.s_loss_dir_),
                                 LossDirToStr(arq_.r_loss_dir_), (u32)(arq_.max_retran_times_),
                                 arq_.ack_err_counter_, arq_.ack_resend_counter_, arq_.rto_resend_counter_,
                                 arq_.arq_list_.ai_repair_sum_);

    if (((u32)wrt_num) >= empty_sz) {
        RETURN_ERR(kGtpSessionMd, kMemNotEnoughErr);
    }

    *str_size = wrt_num;

    return GTP_OK;
}

void GtpSession::CalcMaxFramePeriod(const u64 &now_ts) {
    u32 period_us = (u32)(now_ts - last_send_ts_us_);
    if (now_max_frame_period_us_ < period_us) {
        now_max_frame_period_us_ = period_us;
        if (GTP_YES != max_frm_prd_updt_flg_) {
            max_frm_prd_updt_flg_ = GTP_YES;
        }
    }

    return;
}

u32 GtpSession::CalcLossStd(const u32 &loss, u32 *out_avg_loss) {
    u32 std_sum  = 0;
    u32 loop     = 0;
    u32 delta    = 0;
    u32 avg_loss = 0;

    if (GTP_YES == cache_loss_ovt_) {
        loss_sum_ += loss;
        loss_sum_ -= cache_loss_[cur_cache_loss_idx_];
        avg_loss   = (loss_sum_ >> 5);

        cache_loss_[cur_cache_loss_idx_] = loss;
        cur_cache_loss_idx_ += 1;

        for (loop = 0; MAX_CACHE_LOSS_NUM > loop; ++loop) {
            if (avg_loss <= cache_loss_[loop]) {
                delta = cache_loss_[loop] - avg_loss;
            } else {
                delta = avg_loss - cache_loss_[loop];
            }

            std_sum += (delta * delta);
        }

        std_sum >>= 5;
    } else {
        cache_loss_[cur_cache_loss_idx_] = loss;
        cur_cache_loss_idx_ += 1;

        loss_sum_ += loss;
        avg_loss   = loss_sum_ / ((u32)cur_cache_loss_idx_);

        for (loop = 0; ((u32)cur_cache_loss_idx_) > loop; ++loop) {
            if (avg_loss <= cache_loss_[loop]) {
                delta = cache_loss_[loop] - avg_loss;
            } else {
                delta = avg_loss - cache_loss_[loop];
            }

            std_sum += (delta * delta);
        }

        std_sum /= ((u32)cur_cache_loss_idx_);
    }

    if ((GTP_NO == cache_loss_ovt_) && (MAX_CACHE_LOSS_NUM <= cur_cache_loss_idx_)) {
        cache_loss_ovt_ = GTP_YES;
    }

    cur_cache_loss_idx_ &= 0x1F;

    *out_avg_loss = avg_loss;

    return std_sum;
}

u8 GtpSession::CalcGameFecPolicy(void) const {
    #if (2 == APPLICATION_TYPE)
    const u32 policy_pps = GetSendBusinessPps();

    static const u8 game_fec_policy_table[][7] = {
        // pps:  <20  20-49 50-79 80-109 110-139 140-169 170+
        {0xFF,  0xFF,   5,    5,     5,      5,      5},  // loss < 2%  (pps<50: FEC off; pps>=50: book5)
        {  5,     5,    5,    5,     5,      5,      5},  // 2% <= loss < 10%
        {  4,     4,    4,    4,     4,      4,      4},  // 10% <= loss < 25%
        {0xFF,  0xFF, 0xFF, 0xFF,  0xFF,   0xFF,   0xFF},  // 25% <= loss < 50%  (not handled)
        {0xFF,  0xFF, 0xFF, 0xFF,  0xFF,   0xFF,   0xFF},  // 50% <= loss < 100%  (not handled)
        {0xFF,  0xFF, 0xFF, 0xFF,  0xFF,   0xFF,   0xFF}   // loss >= 100%  (not handled)
    };

    f32 policy_loss = pb_dt_.game_fec_policy_loss_;
    if (policy_loss < pb_dt_.max_send_loss_per_s_) {
        policy_loss = pb_dt_.max_send_loss_per_s_;
    }
    if ((50 <= policy_pps) && ((create_ts_us_ + 2000000) > last_active_ts_us_) && (1.0f > policy_loss)) {
        policy_loss = 1.0f;
    }
    if ((0.100001f < policy_loss) && (1.0f > policy_loss)) {
        policy_loss = 1.0f;
    }

    u32 loss_idx = CalcGameFecTableLossIndex(policy_loss);
    const u32 pps_idx  = CalcGameFecTablePpsIndex(policy_pps);

    // The 10-25% row jumps to book4 (2x2, double the parity overhead of book5). Only take
    // that jump once the elevated loss has actually persisted across 2+ seconds; an isolated
    // sub-second burst is long since recovered by ARQ before this redundancy could ever help,
    // so escalating for it is pure wasted bandwidth. Sustained bad links still escalate
    // normally after the second consecutive high-loss second.
    if ((2 == loss_idx) && (2 > elevated_loss_streak_)) {
        loss_idx = 1;
    }

    return game_fec_policy_table[loss_idx][pps_idx];
    #else
    return DEFAULT_FEC2_BOOK_ID;
    #endif
}

u8 GtpSession::CalcGameFecBookId(void) const {
    #if (2 == APPLICATION_TYPE)
    u8 policy = CalcGameFecPolicy();

    if (0xFF == policy) {
        return DEFAULT_FEC2_BOOK_ID;
    }
    return policy;
    #else
    return DEFAULT_FEC2_BOOK_ID;
    #endif
}

u8 GtpSession::CalcGameFecQos(void) const {
    #if (2 == APPLICATION_TYPE)
    if (0xFF == CalcGameFecPolicy()) {
        return TURN_OFF_FEC;
    }
    return STREAM_QOS_WITH_FEC;
    #else
    return STREAM_QOS_WITH_FEC;
    #endif
}

void GtpSession::UpdateSelfIp(const goodtp_sock &sfd) {
    u32 sock_addr_size = sizeof(sockaddr_in6);
    u8  sock_addr[GTP_SOCK_ADDR_SZ] = {0};

    u32 nret = GtpGetSelfSockAddr(sfd, &(sock_addr[0]), sock_addr_size);
    if (GTP_OK != nret) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "Call GtpGetSelfSockAddr() failed(nret=0x%08x "\
               "pid=%d tid=%d)\r\n", nret, (i32)GtpGetProcessId(), (i32)GtpGetThreadId());
        return;
    }

    nret = GtpSockAddrToStrAddr(&(sock_addr[0]), &(pb_dt_.self_ip_[0]), GTP_MAX_STR_IP_SZ,
                                &(pb_dt_.self_port_));
    if (GTP_OK != nret) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "Call GtpSockAddrToStrAddr() failed(nret=0x%08x "\
               "pid=%d tid=%d)\r\n", nret, (i32)GtpGetProcessId(), (i32)GtpGetThreadId());
        return;
    }

    if (AF_INET == ((struct sockaddr*)sock_addr)->sa_family) {
        struct sockaddr_in *saddr = (struct sockaddr_in*)sock_addr;

        memcpy(self_bin_ip_, (void*)(&(saddr->sin_addr)), IPV4_BIN_IP_SIZE);

        self_bin_port_  = saddr->sin_port;
        self_ip_family_ = kGtpIpv4;
    } else {
        struct sockaddr_in6 *saddr = (struct sockaddr_in6*)sock_addr;

        memcpy(self_bin_ip_, (void*)(&(saddr->sin6_addr)), IPV6_BIN_IP_SIZE);

        self_bin_port_  = saddr->sin6_port;
        self_ip_family_ = kGtpIpv6;
    }

    return;
}

void GtpSession::UpdateSelfIp(u8 sock_addr[], const u32 &sock_addr_len) {
    u32 nret = GtpSockAddrToStrAddr(&(sock_addr[0]), &(pb_dt_.self_ip_[0]), GTP_MAX_STR_IP_SZ,
                                &(pb_dt_.self_port_));
    if (GTP_OK != nret) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "Call GtpSockAddrToStrAddr() failed(nret=0x%08x "\
               "pid=%d tid=%d)\r\n", nret, (i32)GtpGetProcessId(), (i32)GtpGetThreadId());
        return;
    }

    if (AF_INET == ((struct sockaddr*)sock_addr)->sa_family) {
        struct sockaddr_in *saddr = (struct sockaddr_in*)sock_addr;

        memcpy(self_bin_ip_, (void*)(&(saddr->sin_addr)), IPV4_BIN_IP_SIZE);

        self_bin_port_  = saddr->sin_port;
        self_ip_family_ = kGtpIpv4;
    } else {
        struct sockaddr_in6 *saddr = (struct sockaddr_in6*)sock_addr;

        memcpy(self_bin_ip_, (void*)(&(saddr->sin6_addr)), IPV6_BIN_IP_SIZE);

        self_bin_port_  = saddr->sin6_port;
        self_ip_family_ = kGtpIpv6;
    }

    return;
}

void GtpSession::UpdatePeerIp(u8 sock_addr[], const u32 &sock_addr_len) {
    u32 nret = GtpSockAddrToStrAddr(&(sock_addr[0]), &(pb_dt_.peer_ip_[0]), GTP_MAX_STR_IP_SZ, &(pb_dt_.peer_port_));
    if (GTP_OK != nret) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "Call GtpSockAddrToStrAddr() failed(nret=0x%08x "\
               "pid=%d tid=%d)\r\n", nret, (i32)GtpGetProcessId(), (i32)GtpGetThreadId());
        return;
    }

    if (AF_INET == ((struct sockaddr*)sock_addr)->sa_family) {
        struct sockaddr_in *saddr = (struct sockaddr_in*)sock_addr;

        memcpy(peer_bin_ip_, (void*)(&(saddr->sin_addr)), IPV4_BIN_IP_SIZE);

        peer_bin_port_  = saddr->sin_port;
        peer_ip_family_ = kGtpIpv4;
    } else {
        struct sockaddr_in6 *saddr = (struct sockaddr_in6*)sock_addr;

        memcpy(peer_bin_ip_, (void*)(&(saddr->sin6_addr)), IPV6_BIN_IP_SIZE);

        peer_bin_port_  = saddr->sin6_port;
        peer_ip_family_ = kGtpIpv6;
    }

    return;
}

u32 GtpSession::WinBitMap(u8 *out_str, const u32 &mem_size, u32 *str_size) {
    u8 *wrt_pos = out_str;
    u32 free_sz = mem_size;
    u32 str_len = 0;
    u32 wrt_num = 0;

    u32 run_result = AppendSessionAtrribute(wrt_pos, free_sz, &wrt_num);
    if (GTP_OK != run_result) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "calling AppendSessionAtrribute() failed(0x%08x).\r\n", run_result);
        return run_result;
    }

    wrt_pos += wrt_num;
    free_sz -= wrt_num;
    str_len += wrt_num;
    wrt_num  = 0;

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\nsend win: ");
    str_len += wrt_num;

    if (((u32)wrt_num) >= free_sz) {
        RETURN_ERR(kGtpSessionMd, kMemNotEnoughErr);
    }

    wrt_pos += wrt_num;
    free_sz -= wrt_num;
    wrt_num  = 0;

    run_result = PrintWinBitMap(data_win_s_, wrt_pos, free_sz, &wrt_num);
    if (GTP_OK != run_result) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "calling PrintWinBitMap() failed(0x%08x %s) for send win.\r\n", run_result,
               WinErrorInfo(data_win_r_, run_result));
        return run_result;
    }
    str_len += wrt_num;

    if (((u32)wrt_num) >= free_sz) {
        RETURN_ERR(kGtpSessionMd, kMemNotEnoughErr);
    }

    wrt_pos += wrt_num;
    free_sz -= wrt_num;
    wrt_num  = 0;

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\nrecv win: ");

    str_len += wrt_num;
    if (wrt_num >= free_sz) {
        RETURN_ERR(kGtpSessionMd, kMemNotEnoughErr);
    }

    wrt_pos += wrt_num;
    free_sz -= wrt_num;
    wrt_num  = 0;

    run_result = PrintWinBitMap(data_win_r_, wrt_pos, free_sz, &wrt_num);
    if (GTP_OK != run_result) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "calling PrintWinBitMap() failed(0x%08x %s) for send win.\r\n", run_result,
               WinErrorInfo(data_win_r_, run_result));
        return run_result;
    }
    str_len += wrt_num;

    *str_size = str_len;

    return GTP_OK;
}

u32 GtpSession::PrintHarqParam(u8 *out_str, const u32 &mem_size) {
    u8 *wrt_pos = out_str;
    u32 free_sz = mem_size;
    u32 str_len = 0;
    u32 wrt_num = 0;

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\nharq parameter:");
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n alg_top_switch= %s",
                            ((GTP_ON == pb_dt_.alg_top_switch_) ? "On" : "Off"));
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n    net_quality= %s",
                            ((((u8)(NetQuality::kNetQualityBad)) == net_quality_) ? "bad" : "good"));
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n    stream_type= %s",
                            ((kRealTimeStream == pb_dt_.tran_addr_.stream_type_) ? "Real time" : "reliable"));
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n  smooth_jitter= %s",
                            ((GTP_NO == pb_dt_.tran_addr_.smooth_jitter_) ? "No" : "Yes"));
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n disorder_delta= %u", ai_learn_sn_delta_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n max_retran_num= %u", (u32)(arq_.max_retran_times_));
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n  max_boost_num= %u", (u32)(arq_.max_boost_times_));
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n  harq_node_num= %u", arq_.arq_list_.node_num_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n rto_resend_counter= %u", arq_.rto_resend_counter_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n ack_resend_counter= %u", arq_.ack_resend_counter_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n boost_resend_counter= %u", arq_.arq_list_.ai_repair_sum_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    return str_len;
}

u32 GtpSession::PrintAlgorithmParam(u8 *out_str, const u32 &mem_size) {
    u8 *wrt_pos = out_str;
    u32 free_sz = mem_size;
    u32 str_len = 0;
    u32 wrt_num = 0;

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "%s:%u<-->%s:%u pos=%s s_state=%s r_state=%s s_loss=%.2f%%(%s) "\
                            "max_s_loss=%.2f%% r_loss=%.2f%%(%s) sdt_pps=%u snt_pps=%u rdt_pps=%u rnt_pps=%u:",
                            pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
                            ((((u8)(GtpSessionMode::kSender)) == mode_) ? "send":"receive"),
                            LinkStatToStr(send_session_stat_), LinkStatToStr(recv_session_stat_),
                            arq_.s_cur_loss_rate_, LossDirToStr(arq_.s_loss_dir_), pb_dt_.max_send_loss_per_s_,
                            arq_.r_cur_loss_rate_, LossDirToStr(arq_.r_loss_dir_), pb_dt_.send_stat_.data_pack_pps_,
                            pb_dt_.send_stat_.net_pack_pps_, pb_dt_.recv_stat_.data_pack_pps_,
                            pb_dt_.recv_stat_.net_pack_pps_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = PrintHarqParam(wrt_pos, free_sz);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n");
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = fec2_obj_.PrintFec2Param(wrt_pos, free_sz);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n");
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz,
                            "\r\n realtime_reorder: rtt=%uus late_rescue=%llu",
                            pb_dt_.rtt_us_, (unsigned long long)realtime_reorder_late_rescue_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n%s=%lluus\r\n%s=%lluus\r\n%s=%lluus\r\n%s=%lluus",
                            pb_dt_.send_consume_.name_, (unsigned long long)pb_dt_.send_consume_.avg_consume_time_,
                            pb_dt_.recv_consume_.name_, (unsigned long long)pb_dt_.recv_consume_.avg_consume_time_,
                            pb_dt_.app_send_consume_.name_, (unsigned long long)pb_dt_.app_send_consume_.avg_consume_time_,
                            pb_dt_.app_recv_consume_.name_, (unsigned long long)pb_dt_.app_recv_consume_.avg_consume_time_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    return str_len;
}

void GtpSession::SendSetRecvRttPacket(void) {
    u8 tmp_buf[1024];
    GtpPacket *pack = (GtpPacket*)tmp_buf;

    pack->goodtp_ver_     = CalcRightGtpVer(GTP_VERSION, pb_dt_.peer_version_);
    pack->header_offset_  = (u8)sizeof(GtpPacket);
    pack->cache_us_flag_  = GTP_NO;
    pack->has_loss_flag_  = GTP_NO;
    pack->pack_type_      = (u8)(GtpPackType::kGtpSetRecvRttPackType);
    pack->pack_size_      = (u16)sizeof(GtpPacket);
    pack->pack_sn_        = 0;         // the sn is invalid in set receiving rtt packet.
    pack->has_check_flag_ = GTP_NO;
    pack->has_ts_flag_    = GTP_NO;
    pack->has_rtt_flag_   = GTP_YES;
    pack->init_flag_      = (u8)((((u8)(GtpSessStat::kRunning)) == send_session_stat_) ? GTP_NO : GTP_YES);
    pack->repeat_counter_ = 0;
    pack->is_qos_flg_     = GTP_NO;
    pack->share_zone_     = 0;
    pack->has_chg_zone_   = GTP_NO;
    pack->stream_type_    = kRealTimeStream;

    GtpWriteStreamKeyHeader(pack, pb_dt_.tran_addr_.stream_key_);
    GtpWriteU32Unaligned(((u8*)(pack)) + pack->header_offset_, rtt_us_);
    pack->header_offset_ += sizeof(u32);
    pack->pack_size_      = (u16)pack->header_offset_;

    #ifdef _SELFDEBUG
    GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug, "%s:%u<-->%s:%u send to receive windows's rrt=%uus.\r\n",
           pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_,
           (u32)(pb_dt_.peer_port_), rtt_us_);
    #endif

    GtpHeaderNewToOld(pack, pb_dt_.peer_version_);

    u32 result = GTP_OK;

    #if (1 == ENABLE_MD_PERF_CHECK)
    {
    u64 app_consume_us = GtpSysTimestampUs();
    result = cb_.send_pack_cb_(GtpHdlIntToPointer(pb_dt_.gtp_hdl_), pack,
                               (u32)(pack->pack_size_), &(pb_dt_.tran_addr_));
    app_consume_us = GtpSysTimestampUs() - app_consume_us;
    pb_dt_.app_send_consume_.CacheConsumeTime(app_consume_us);
    pb_dt_.send_consume_.ts_us_ += app_consume_us;
    pb_dt_.recv_consume_.ts_us_ += app_consume_us;
    }
    #else
    result = cb_.send_pack_cb_(GtpHdlIntToPointer(pb_dt_.gtp_hdl_), pack,
                               (u32)(pack->pack_size_), &(pb_dt_.tran_addr_));
    #endif

    if (GTP_OK != result) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "call send_pack_cb_() failed(0x%08x)\r\n", result);
        return;
    }

    rtt_us_ = 0;

    pb_dt_.send_stat_.net_pack_sum_    += 1;
    pb_dt_.send_stat_.net_bitrate_sum_ += ((u32)(pack->pack_size_));

    return;
}

void GtpSession::SendRttTestResPacket(const u64 &ts_us) {
    u8 tmp_buf[1024];
    GtpPacket *pack = (GtpPacket*)tmp_buf;

    pack->goodtp_ver_     = CalcRightGtpVer(GTP_VERSION, pb_dt_.peer_version_);
    pack->header_offset_  = (u8)sizeof(GtpPacket);
    pack->cache_us_flag_  = GTP_NO;
    pack->has_loss_flag_  = GTP_NO;
    pack->pack_type_      = (u8)(GtpPackType::kGtpRttTstResPackType);
    pack->pack_size_      = (u16)sizeof(GtpPacket);
    pack->pack_sn_        = 0;
    pack->has_check_flag_ = GTP_NO;
    pack->has_ts_flag_    = GTP_YES;
    pack->has_rtt_flag_   = GTP_NO;
    pack->init_flag_      = (u8)(((u8)(GtpSessStat::kRunning) == recv_session_stat_) ? GTP_NO : GTP_YES);
    pack->repeat_counter_ = 0;
    pack->is_qos_flg_     = GTP_NO;
    pack->share_zone_     = 0;
    pack->has_chg_zone_   = GTP_NO;
    pack->stream_type_    = kRealTimeStream;

    GtpWriteStreamKeyHeader(pack, pb_dt_.tran_addr_.stream_key_);
    GtpWriteU64Unaligned(((u8*)(pack)) + pack->header_offset_, ts_us);
    pack->header_offset_ += sizeof(u64);
    pack->pack_size_      = (u16)pack->header_offset_;

    GtpHeaderNewToOld(pack, pb_dt_.peer_version_);

    u32 result = GTP_OK;

    #if (1 == ENABLE_MD_PERF_CHECK)
    {
    u64 app_consume_us = GtpSysTimestampUs();
    result = cb_.send_pack_cb_(GtpHdlIntToPointer(pb_dt_.gtp_hdl_), pack,
                               (u32)(pack->pack_size_), &(pb_dt_.tran_addr_));
    app_consume_us = GtpSysTimestampUs() - app_consume_us;
    pb_dt_.app_send_consume_.CacheConsumeTime(app_consume_us);

    pb_dt_.send_consume_.ts_us_ += app_consume_us;
    pb_dt_.recv_consume_.ts_us_ += app_consume_us;
    }
    #else
    result = cb_.send_pack_cb_(GtpHdlIntToPointer(pb_dt_.gtp_hdl_), pack,
                               (u32)(pack->pack_size_), &(pb_dt_.tran_addr_));
    #endif

    if (GTP_OK != result) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "call send_pack_cb_() failed(0x%08x)\r\n", result);
        return;
    }

    pb_dt_.send_stat_.net_pack_sum_    += 1;
    pb_dt_.send_stat_.net_bitrate_sum_ += ((u32)(pack->pack_size_));

    return;
}

void GtpSession::ResetSession(const u32 &cur_sn, const u32 &sort_sn, const u32 &sort_sn_valid,
                              const u32 &chg_status_flag) {
    ack_sn_ = 0;
    last_sn_ = cur_sn;

    if (GTP_YES == chg_status_flag) {
        recv_session_stat_ = (u8)(GtpSessStat::kInitRes);
    }

    #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
    u32 l_border_sn = cur_sn & 0xFFFFFF80;
    u32 filter_l_border_sn = (GTP_YES == sort_sn_valid) ? (sort_sn & 0xFFFFFF80) : l_border_sn;
    #endif

    #if (2 == APPLICATION_TYPE)
    u32 l_border_sn = cur_sn & 0xFFFFFFC0;
    u32 filter_l_border_sn = (GTP_YES == sort_sn_valid) ? (sort_sn & 0xFFFFFFC0) : l_border_sn;
    #endif

    ResetSlidWin(data_win_r_, l_border_sn, last_active_ts_us_);
    ResetSlidWin(filter_win_, filter_l_border_sn, last_active_ts_us_);
    realtime_reorder_win_.Reset();
    realtime_reorder_next_deliver_ts_us_ = 0;

    if (GTP_YES == chg_status_flag) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "the peer sending has been recreated, "\
               "now reset the receiving win(%p)'s left border sn to %u and the filter win(%p)'s left border sn "
               "to %u, sort_sn=%u valid=%u\r\n",
               data_win_r_, l_border_sn, filter_win_, filter_l_border_sn, sort_sn, sort_sn_valid);
    } else {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "the network has been restored, "\
               "now sync the receiving win(%p)'s left border sn to %u and the filter win(%p)'s left border sn "
               "to %u, sort_sn=%u valid=%u\r\n",
               data_win_r_, l_border_sn, filter_win_, filter_l_border_sn, sort_sn, sort_sn_valid);
    }

    return;
}

void GtpSession::RttHandler(const u32 &rtt_us) {
    recv_idle_calc_period_us_ = (rtt_us >> 1);

    if (MAX_HANDLER_CALC_PERIOD_US < recv_idle_calc_period_us_) {
        recv_idle_calc_period_us_ = MAX_HANDLER_CALC_PERIOD_US;
    } else if (MIN_HANDLER_CALC_PERIOD_US > recv_idle_calc_period_us_) {
        recv_idle_calc_period_us_ = MIN_HANDLER_CALC_PERIOD_US;
    }

    // PPS floor applied after RTT clamp so low-PPS sessions don't send ACK faster than inter-packet interval.
    const u32 cur_pps = GetSendBusinessPps();
    if (0 < cur_pps) {
        const u64 pps_floor_us = 1000000ULL / cur_pps;
        if (pps_floor_us > recv_idle_calc_period_us_) {
            recv_idle_calc_period_us_ = pps_floor_us;
        }
    }

    self_session_ttl_us_ = ((rtt_us << 2) + rtt_us);
    if (min_session_stability_us_ > self_session_ttl_us_) {
        self_session_ttl_us_ = min_session_stability_us_;
    }

    if (min_session_stability_us_ < self_session_ttl_us_) {
        min_session_stability_us_ = self_session_ttl_us_;
    }

    if (10.000001 <= arq_.r_cur_loss_rate_) {
        self_session_ttl_us_ += (self_session_ttl_us_ >> 2);
    }

    pb_dt_.rtt_us_ = rtt_us;

    return;
}

#ifdef __cplusplus
}
#endif
