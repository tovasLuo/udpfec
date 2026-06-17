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
#include "bitlinker.h"


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

void LinkQualityCallback(slid_win_hdl win_hdl, void *cntxt_hdl, const u32 &rtt_us, const u32 &jitter_us,
    const f32 &loss, const DiscardDirectU32 &loss_dir, const u32 &pre_congest_rank, const u32 &rto_us,
    const u32 &pps, const u32 &loss_num) {
    GtpSession *session = static_cast<GtpSession *>(cntxt_hdl);

    if (win_hdl == session->filter_win_) {
        return;
    }

    u8 tmp_buf[2048];

    GtpLinkQuality &link_quality = (*((GtpLinkQuality*)tmp_buf));

    memset(&link_quality, 0, sizeof(link_quality));

    if (win_hdl == session->data_win_s_) {
        session->rpt_snd_qualit_ = GTP_YES;

        u16 line_pos  = 0;
        u16 row_pos   = 0;
        u32 first_bad = GTP_NO;

        f32 max_loss_thresheld = (f32)MAX_REALTIME_LOSS_THRESHLD;
        f32 rmv_loss_thresheld = (f32)RMV_REALTIME_LOSS_THRESHLD;

        if ((MIN_SESSION_STABLE_TIME_US << 1) > (session->last_active_ts_us_ - session->create_ts_us_)) {
            goto proc_sender_network_quality_pos_;
        }

        if (kReliableStream <= session->pb_dt_.tran_addr_.stream_type_) {
            max_loss_thresheld = (f32)MAX_RELIABLE_LOSS_THRESHLD;
            rmv_loss_thresheld = (f32)RMV_RELIABLE_LOSS_THRESHLD;
        }

        if (max_loss_thresheld <= loss) {
            if (GTP_OFF != session->pb_dt_.alg_top_switch_) {
                session->pb_dt_.alg_top_switch_ = GTP_OFF;

                GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "%s:%u<-->%s:%u turn off "\
                       "algorithm(stream_type=%s std_loss=%5.2f%% cur_loss=%5.2f%% dir=%s)\r\n",
                       session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_),
                       session->pb_dt_.peer_ip_, (u32)(session->pb_dt_.peer_port_),
                       ((kReliableStream > session->pb_dt_.tran_addr_.stream_type_) ? "real_time" : "reliable"),
                       max_loss_thresheld, loss, ((kUpLinkerLoss == loss_dir) ? "up" : "down"));
            }

            session->rmv_close_alg_period_us_ = 1500000 + (session->last_active_ts_us_ & 0x00000000000FFFFF);
            session->rmv_close_alg_ts_us_     = ((u32)(session->last_active_ts_us_ & 0x00000000FFFFFFFF))
                                              + session->rmv_close_alg_period_us_;

            goto proc_sender_network_quality_pos_;
        }

        if (GTP_ON == session->pb_dt_.alg_top_switch_) {
            goto proc_sender_network_quality_pos_;
        }

        if (rmv_loss_thresheld < loss) {
            session->rmv_close_alg_ts_us_ = ((u32)(session->last_active_ts_us_ & 0x00000000FFFFFFFF))
                                          + session->rmv_close_alg_period_us_;
            goto proc_sender_network_quality_pos_;
        }

        if (session->rmv_close_alg_ts_us_ > ((u32)(session->last_active_ts_us_ & 0x00000000FFFFFFFF))) {
            goto proc_sender_network_quality_pos_;
        }

        session->pb_dt_.alg_top_switch_ = GTP_ON;

        GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "%s:%u<-->%s:%u turn on "\
               "algorithm(stream_type=%s std_loss=%5.2f%% cur_loss=%5.2f%% dir=%s)\r\n",
               session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_),
               session->pb_dt_.peer_ip_, (u32)(session->pb_dt_.peer_port_),
               ((kReliableStream > session->pb_dt_.tran_addr_.stream_type_) ? "real_time" : "reliable"),
               rmv_loss_thresheld, loss, ((kUpLinkerLoss == loss_dir) ? "up" : "down"));

proc_sender_network_quality_pos_:
        session->pb_dt_.UpdatedMaxLoss(loss);

        if (FLOAT_ZERO >= session->pb_dt_.max_send_loss_per_s_) {
            session->net_bad_pending_cnt_ = 0;
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
                    u32 fec2_encode_book_id[] = {
                        5,  // pps <  10  V-FEC arrives after gap timer; book5 saves ~50% FEC BW
                        5,  // pps <  50  same reasoning (covers ≤30pps; 31-49 borderline but fine)
                        4,  // pps <  100 pc game
                        4,  // pps <  150 pc game
                        4   // pps >= 150 pc game
                    };

                    u32 std_pps[] = {10, 50, 100, 150};
                    #endif

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
            session->net_bad_pending_cnt_++;
            if (session->net_bad_pending_cnt_ < 2) {
                session->last_gen_loss_ts_us_ = session->last_active_ts_us_;
                goto sender_qualiti_proc_start_pos_;
            }
            session->net_bad_pending_cnt_ = 0;
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
        u32 boost_alg_param[][6] = {
            // loss < 1% loss < 10% loss < 20% loss < 35% loss < 50% loss >= 50%
            {1,          1,         2,         3,         2,         2},  // pps < 10 test speed
            {0,          0,         1,         2,         1,         0},  // pps < 50 phone game
            {0,          0,         1,         2,         1,         0},  // pps < 100 phone&pc game
            {0,          0,         1,         2,         1,         0},  // pps < 170 pc game
            {0,          0,         1,         1,         1,         0},  // pps < 250 pc game
            {0,          0,         0,         1,         1,         0},  // pps >= 250 for pc game
        };

        u32 std_pps[][5] = {
            {10, 60, 100, 170, 250},  // realtime stream.
            {10, 50, 100, 170, 250}   // reliable stream.
        };
        #endif

        u32 std_pps_line = 0;

        if ((kReliableStream <= session->pb_dt_.tran_addr_.stream_type_) && (0x01 < session->pb_dt_.peer_version_)) {
            std_pps_line = 1;
        }

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
        // 5 columns: loss<2%  2-10%  10-25%  25-50%  >=50%
        // 25% crossover added: consistent with game mode data.
        u32 fec2_ml_book_id1[][5] = {
            // loss<2%  2-10%  10-25%  25-50%  >=50%
            {4,         4,     4,      4,      4},  // pps <  10   test speed
            {5,         4,     4,      2,      2},  // pps <  500  SD video
            {1,         1,     4,      2,      2},  // pps <  1200 HD video
            {1,         1,     4,      2,      2},  // pps <  2000 2K
            {1,         1,     4,      2,      2},  // pps <  5000 4K
            {1,         1,     4,      2,      2}   // pps >= 5000 8K
        };
        u32 fec2_ml_book_id2[][5] = {
            // loss<2%  2-10%  10-25%  25-50%  >=50%
            {4,         4,     4,      4,      4},  // pps <  10   test speed
            {4,         4,     4,      2,      2},  // pps <  500  SD video
            {4,         4,     4,      2,      2},  // pps <  1200 HD video
            {4,         4,     4,      2,      2},  // pps <  2000 2K
            {4,         4,     4,      2,      2},  // pps <  5000 4K
            {4,         4,     4,      2,      2}   // pps >= 5000 8K
        };
        #endif

        #if (2 == APPLICATION_TYPE)
        // 5 columns: loss<2%  2-10%  10-25%  25-50%  >=50%
        // Data source: game_net_test PART3 bandwidth sweep (book2 vs book4).
        //   <25% loss: book4(2×2 H+V) has lower total overhead than book2(4×1 H)
        //              test data: 15% loss → book4=177% vs book2=245% overhead
        //   >25% loss: book4 FEC pkts also get lost; extra parity buys little recovery
        //              but costs more ARQ retrans; book2 wins on total bandwidth.
        //              test data: 25% loss → book2=193% vs book4=292% overhead
        //
        // bad 状态 FEC book 选择（effective loss < 10%）：
        //   pps <  10: book4（2×2 H+V），低 pps 数据不足，保守选择
        //   pps >= 10: book5（2×1 H），节省 30-51pp 带宽开销，p99 ≈ book4±4%
        //   实测依据：PART10 30s 专项测试，30-60pps × 5%/8% 丢包全场景对比
        u32 fec2_ml_book_id1[][5] = {
            // loss<2%  2-10%  10-25%  25-50%  >=50%
            {5,         5,     4,      2,      2},  // pps <  10  book5: V-FEC arrives after gap timer
            {5,         5,     4,      2,      2},  // pps <  60  book5 (10-59pps 验证安全)
            {5,         5,     4,      2,      2},  // pps <  100
            {5,         5,     4,      2,      2},  // pps <  170
            {5,         5,     4,      2,      2},  // pps <  250
            {5,         5,     4,      2,      2}   // pps >= 250
        };
        u32 fec2_ml_book_id2[][5] = {
            // loss<2%  2-10%  10-25%  25-50%  >=50%
            {5,         5,     4,      2,      2},  // pps <  10  book5: V-FEC arrives after gap timer
            {5,         5,     4,      2,      2},  // pps <  60
            {5,         5,     4,      2,      2},  // pps <  100
            {5,         5,     4,      2,      2},  // pps <  170
            {5,         5,     4,      2,      2},  // pps <  250
            {5,         5,     4,      2,      2}   // pps >= 250
        };
        #endif

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
            row_pos = 0;   /* <2%:  book4, auto-FEC typically off */
            goto fec_ml_end_pos_;
        }

        if (10.000001 > session->pb_dt_.max_send_loss_per_s_) {
            row_pos = 1;   /* 2-10%: book4, H+V coverage, lower overhead */
            goto fec_ml_end_pos_;
        }

        if (25.000001 > session->pb_dt_.max_send_loss_per_s_) {
            /* hysteresis: if currently on book2, require loss < 23% to switch back to book4 */
            if ((2 == session->fec2_obj_.GetUsingBookId()) && (23.000001 <= session->pb_dt_.max_send_loss_per_s_)) {
                row_pos = 3;
            } else {
                row_pos = 2;   /* 10-25%: book4 still optimal (test: 177% vs 245%) */
            }
            goto fec_ml_end_pos_;
        }

        if (50.000001 > session->pb_dt_.max_send_loss_per_s_) {
            row_pos = 3;   /* 25-50%: book2 saves ~100ppts overhead (test: 193% vs 292%) */
            goto fec_ml_end_pos_;
        }
        row_pos = 4;       /* >=50%: book2, ARQ-dominant, minimal FEC overhead */

fec_ml_end_pos_:
        if ((kReliableStream <= session->pb_dt_.tran_addr_.stream_type_)
         && (0x01 < session->pb_dt_.peer_version_)) {
            session->fec2_obj_.ChangeFecMode(fec2_ml_book_id2[line_pos][row_pos]);
        } else {
            session->fec2_obj_.ChangeFecMode(fec2_ml_book_id1[line_pos][row_pos]);
        }

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
            {5, 5, 4, 3, 2, 2},  // 10.000001 >= loss
            {5, 4, 4, 3, 2, 1},  // 20.000001 >= loss
            {5, 4, 4, 2, 1, 1},  // 30.000001 >= loss
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
    link_quality.bitrage_chg_k_    = 0;
    link_quality.loss_direct_      = loss_dir;
    link_quality.context_          = session->pb_dt_.tran_addr_.context_;

    link_quality.linker_key_.sfd_        = session->pb_dt_.tran_addr_.sfd_;
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

    u64 now_ts_us  = GtpSysTimestampUs();
    u64 real_ts_us = now_ts_us;  // preserve before possible RTT-delta modification

    u8 *chg_len_zone = NULL;

    u8 tmp_buf[4096];

    GtpPacket *pack = (GtpPacket*)tmp_buf;

    u32 pack_size      = 0;
    u16 tail_sn_offset = 0;
    u16 rto_sn_offset  = 0;

    // when the session is identified by stream_key_, embed it right after the fixed header(under the
    // existing has_check_flag_), before the ack/nack-specific load. decode side already locates the load
    // via pack->header_offset_(see PackPrepHandler's kGtpAckPackType/kGtpNackPackType cases), so this is
    // transparent to that code.
    u16 has_check = GTP_NO;
    u8  hdr_off   = (u8)sizeof(GtpPacket);

    if (GTP_YES == session->pb_dt_.tran_addr_.enable_key_) {
        has_check = GTP_YES;
        u8 *key_ptr = ((u8*)pack) + sizeof(GtpPacket);
        *((u32*)(key_ptr))              = (u32)(session->pb_dt_.tran_addr_.stream_key_ >> 32);
        *((u32*)(key_ptr + sizeof(u32))) = (u32)(session->pb_dt_.tran_addr_.stream_key_);
        hdr_off = (u8)(sizeof(GtpPacket) + sizeof(u64));
    }

    pack->goodtp_ver_      = CalcRightGtpVer(GTP_VERSION, session->pb_dt_.peer_version_);
    pack->header_offset_   = hdr_off;
    pack->has_loss_flag_   = GTP_NO;
    pack->cache_us_flag_   = GTP_NO;
    pack->pack_type_       = (u8)(GtpPackType::kGtpNackPackType);
    pack->pack_sn_         = session->ack_sn_;
    pack->has_check_flag_  = (u8)has_check;
    pack->has_ts_flag_     = GTP_NO;
    pack->has_rtt_flag_    = GTP_NO;
    pack->init_flag_       = (u8)(((u8)(GtpSessStat::kRunning) == session->recv_session_stat_) ? GTP_NO : GTP_YES);
    pack->repeat_counter_  = 0;
    pack->is_qos_flg_      = GTP_NO;
    pack->share_zone_      = 0;

    if (kNackType == ack_nack) {
        const NackData *nack_data = (NackData*)bit_map;

        if (nack_data->head_sn_ <= nack_data->tail_sn_) {
            tail_sn_offset = (u16)(nack_data->tail_sn_ - nack_data->head_sn_);
        } else {
            tail_sn_offset = (u16)(nack_data->tail_sn_ + (0xFFFFFFFF - nack_data->head_sn_));
        }

        if (nack_data->head_sn_ <= nack_data->rto_sn_) {
            rto_sn_offset = (u16)(nack_data->rto_sn_ - nack_data->head_sn_);
        } else {
            rto_sn_offset = (u16)(nack_data->rto_sn_ + (0xFFFFFFFF - nack_data->head_sn_));
        }

        pack_size = ((u32)hdr_off) + sizeof(GtpNackPacketLoad) + (((u32)(nack_data->nack_num_)) << 1);

        GtpNackPacketLoad *nack_load = (GtpNackPacketLoad*)(((u8*)pack) + hdr_off);

        nack_load->nack_num_       = nack_data->nack_num_;
        nack_load->tail_sn_offset_ = tail_sn_offset;
        nack_load->rto_sn_offset_  = rto_sn_offset;
        nack_load->head_sn_        = nack_data->head_sn_;
        nack_load->recv_loss_      = nack_data->recv_loss_;

        chg_len_zone = ((u8*)nack_load) + sizeof(GtpNackPacketLoad);

        if (0 != session->com_cache_ts_us_) {
            if (now_ts_us >= session->at_com_cache_ts_us_) {
                now_ts_us -= session->at_com_cache_ts_us_;
            } else {
                now_ts_us = session->at_com_cache_ts_us_ - now_ts_us;
            }

            pack->cache_us_flag_  = GTP_YES;
            *((u64*)chg_len_zone) = session->com_cache_ts_us_ + now_ts_us;
            chg_len_zone          += sizeof(u64);
            pack_size              += sizeof(u64);

            session->com_cache_ts_us_    = 0;
            session->at_com_cache_ts_us_ = 0;
        }

        pack->pack_size_ = (u16)pack_size;

        if (3 <= nack_data->nack_num_) {
            session->nack_burst_detected_ = 1;
        }

        memcpy(chg_len_zone, nack_data->nack_, ((u32)(nack_data->nack_num_)) << 1);

        #ifdef _SELFDEBUG
        GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
               "[win_hdl=%p]%s:%u<-->%s:%u(nack) head_sn=%u tail_sn=%u rto_sn=%u nack_num=%u.\r\n", win_hdl,
               session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_),
               session->pb_dt_.peer_ip_, (u32)(session->pb_dt_.peer_port_),
               nack_load->head_sn_, nack_data->tail_sn_, nack_data->rto_sn_, nack_load->nack_num_);
        #endif

        goto session_send_ack_nack_pos_;
    }

    {
    const ReceivedSnBitMap *ack_data = (ReceivedSnBitMap*)bit_map;

    if (ack_data->head_sn_ <= ack_data->tail_sn_) {
        tail_sn_offset = (u16)(ack_data->tail_sn_ - ack_data->head_sn_);
    } else {
        tail_sn_offset = (u16)(ack_data->tail_sn_ + (0xFFFFFFFF - ack_data->head_sn_));
    }

    if (ack_data->head_sn_ <= ack_data->rto_sn_) {
        rto_sn_offset = (u16)(ack_data->rto_sn_ - ack_data->head_sn_);
    } else {
        rto_sn_offset = (u16)(ack_data->rto_sn_ + (0xFFFFFFFF - ack_data->head_sn_));
    }

    pack_size = ((u32)hdr_off) + sizeof(GtpAckPacketLoad) + ack_data->mem_size_;

    pack->pack_type_ = (u8)(GtpPackType::kGtpAckPackType);

    GtpAckPacketLoad *ack_load = (GtpAckPacketLoad*)(((u8*)pack) + hdr_off);

    ack_load->sn_size_        = (u16)(ack_data->mem_size_);
    ack_load->tail_sn_offset_ = tail_sn_offset;
    ack_load->rto_sn_offset_  = rto_sn_offset;
    ack_load->head_sn_        = ack_data->head_sn_;
    ack_load->recv_loss_      = ack_data->recv_loss_;

    chg_len_zone = ((u8*)ack_load) + sizeof(GtpAckPacketLoad);

    if (0 != session->com_cache_ts_us_) {
        if (now_ts_us >= session->at_com_cache_ts_us_) {
            now_ts_us -= session->at_com_cache_ts_us_;
        } else {
            now_ts_us = session->at_com_cache_ts_us_ - now_ts_us;
        }

        pack->cache_us_flag_  = GTP_YES;
        *((u64*)chg_len_zone) = session->com_cache_ts_us_ + now_ts_us;
        chg_len_zone          += sizeof(u64);
        pack_size              += sizeof(u64);

        session->com_cache_ts_us_    = 0;
        session->at_com_cache_ts_us_ = 0;

        #ifdef _SELFDEBUG
        GtpLog(session->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug,
               "[win_hdl=%p]%s:%u<-->%s:%u(ack) head_sn=%u tail_sn=%u rto_sn=%u.\r\n", win_hdl,
               session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_),
               session->pb_dt_.peer_ip_, (u32)(session->pb_dt_.peer_port_),
               ack_load->head_sn_, ack_data->tail_sn_, ack_data->rto_sn_);
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
    }

    session->pb_dt_.send_stat_.ack_sum_         += 1;
    session->pb_dt_.send_stat_.net_pack_sum_    += 1;
    session->pb_dt_.send_stat_.net_bitrate_sum_ += pack_size;

    session->last_feedback_nack_ts_us_ = real_ts_us;

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
    rmv_close_alg_ts_us_(0),
    min_session_stability_us_((u32)ttl),
    report_quality_flag_s_(GTP_NO),
    mode_((u8)mode),
    cache_loss_ovt_(GTP_NO),
    max_frm_prd_updt_flg_(GTP_NO),
    net_quality_((u8)(NetQuality::kNetQualityGood)),
    rpt_snd_qualit_(GTP_NO),
    session_health_((u8)(SessionHealthState::kHealthNoraml)),
    net_bad_pending_cnt_(0),
    nack_burst_detected_(0),
    new_gap_detected_(0),
    cur_cache_loss_idx_(0),
    test_rtt_period_us_(TEST_RTT_PERIOD_US),
    recv_max_data_sn_(0),
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
    pack_mem_pool_(pack_mem_pool) {
    win_mem_s_  = ((u8*)this) + sizeof(GtpSession);
    win_mem_r_  = win_mem_s_ + SlidwinInstanceSize();
    filter_mem_ = win_mem_r_ + SlidwinInstanceSize();

    pb_dt_.tran_addr_.context_       = context;
    pb_dt_.tran_addr_.sfd_           = (u32)sfd;
    pb_dt_.tran_addr_.sock_addr_len_ = dst_sock_addr_size;
    pb_dt_.tran_addr_.self_addr_len_ = slf_sock_addr_size;
    pb_dt_.tran_addr_.stream_type_   = stream_type;

    memcpy(pb_dt_.tran_addr_.sock_addr_, dst_sock_addr, dst_sock_addr_size);
    memcpy(pb_dt_.tran_addr_.self_addr_, slf_sock_addr, slf_sock_addr_size);

    memset(self_bin_ip_, 0x00, GTP_MAX_BIN_IP_SZ);
    memset(peer_bin_ip_, 0x00, GTP_MAX_BIN_IP_SZ);
    memset(cache_loss_, 0x00, sizeof(cache_loss_));
    memset(reorder_buf_, 0x00, sizeof(reorder_buf_));
    reorder_inited_       = 0;
    reorder_next_sn_      = 0;
    reorder_gap_since_us_ = 0;

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
    rmv_close_alg_ts_us_(0),
    min_session_stability_us_((u32)ttl),
    report_quality_flag_s_(GTP_NO),
    mode_((u8)mode),
    cache_loss_ovt_(GTP_NO),
    max_frm_prd_updt_flg_(GTP_NO),
    net_quality_((u8)(NetQuality::kNetQualityGood)),
    rpt_snd_qualit_(GTP_NO),
    session_health_((u8)(SessionHealthState::kHealthNoraml)),
    net_bad_pending_cnt_(0),
    nack_burst_detected_(0),
    new_gap_detected_(0),
    cur_cache_loss_idx_(0),
    test_rtt_period_us_(TEST_RTT_PERIOD_US),
    recv_max_data_sn_(0),
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
    pack_mem_pool_(pack_mem_pool) {
    win_mem_s_  = ((u8*)this) + sizeof(GtpSession);
    win_mem_r_  = win_mem_s_ + SlidwinInstanceSize();
    filter_mem_ = win_mem_r_ + SlidwinInstanceSize();

    memcpy(&(pb_dt_.tran_addr_), tran_addr, sizeof(GtpAddr));

    pb_dt_.tran_addr_.stream_type_   = 0;

    memset(self_bin_ip_, 0x00, GTP_MAX_BIN_IP_SZ);
    memset(peer_bin_ip_, 0x00, GTP_MAX_BIN_IP_SZ);
    memset(cache_loss_, 0x00, sizeof(cache_loss_));
    memset(reorder_buf_, 0x00, sizeof(reorder_buf_));
    reorder_inited_       = 0;
    reorder_next_sn_      = 0;
    reorder_gap_since_us_ = 0;

    send_session_stat_ = (u8)(GtpSessStat::kInitReq);
    recv_session_stat_ = (u8)(GtpSessStat::kInitRes);
}

GtpSession::~GtpSession() {
    ReorderClear();

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
        *((u64*)session_mem) = (u64)psp_mem;
    } else {
        *((u32*)session_mem) = (u32)((u64)psp_mem);
    }
    session_mem += sizeof(void*);

    return session_mem;
}

void GtpSession::operator delete(void *psp_mem) {
    u8 *save_mem_pool_ptr = ((u8*)psp_mem) - sizeof(void*);

    GtpMemPool *mem_pool;

    if (sizeof(u64) == sizeof(void*)) {
        mem_pool = (GtpMemPool*)(*((u64*)save_mem_pool_ptr));
    } else {
        mem_pool = (GtpMemPool*)((u64)(*((u32*)save_mem_pool_ptr)));
    }

    mem_pool->FreeItem(save_mem_pool_ptr);

    return;
}

void GtpSession::operator delete(void *psp_mem, void *psp_mem_for_new) {
    (void)psp_mem_for_new;
    GtpSession::operator delete(psp_mem);
}

u32 GtpSession::Init(const u64 &ts_us, u8 *win_cache, const u32 &pack_sn, const u32 &next_out_sn) {
    pb_dt_.session_ = this;

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

    filter_win_ = CreateSlidWin(LinkQualityCallback, WinSnBitmapCallback, NULL, NULL, pack_sn, ts_us,
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
        hash    = *((u32*)((u8 *)org_pack + move_pos));
        user_id = *((u32*)((u8 *)org_pack + move_pos + sizeof(u32)));
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

    ret_value = GtpCheckPacketInvalid(pack, (u32)(pack->pack_size_), NULL);
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

    if ((kReliableStream > s_obj->pb_dt_.tran_addr_.stream_type_) || (0x02 > pack->goodtp_ver_)
     || (GTP_NO == pack->has_chg_zone_)) {
        if (0 == pack->repeat_counter_) {
            first_sn = pack->pack_sn_;
        } else {
            first_sn = *((u32*)(((u8*)pack) + sizeof(GtpPacket)));
        }
    } else {
        first_sn = ((ChangeZone*)(((u8*)pack) + pack->header_offset_))->sort_sn_;
    }

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
        GtpLog(s_obj->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "%s:%u---->%s:%u Call SnEntrySlidWin() failed(0x%08x) first_sn=%u.\r\n",
               s_obj->pb_dt_.peer_ip_, (u32)(s_obj->pb_dt_.peer_port_),
               s_obj->pb_dt_.self_ip_, (u32)(s_obj->pb_dt_.self_port_),
               ret_value, first_sn);
    }

    u8 *frame = (u8*)pack;
    u32 size  = (u32)(pack->pack_size_);

    frame += pack->header_offset_;
    size  -= pack->header_offset_;

    // Route through per-session reorder buffer so the application always receives packets in SN order.
    // ReorderEnqueue delivers immediately if in-order, or buffers until the gap is filled or times out.
    s_obj->ReorderEnqueue(pack->pack_sn_, frame, size, tran_addr, s_obj->last_active_ts_us_);

    // Register the FEC-recovered SN in data_win_r_ so the next ACK bitmap includes it.
    // Without this, data_win_r_ has no record of the recovered packet, the ACK omits the SN,
    // and the sender's ARQ waits for RTO then retransmits a packet already delivered to the app.
    u32 win_ret = SnEntrySlidWin(s_obj->data_win_r_, pack->pack_sn_, s_obj->last_active_ts_us_);
    if (GTP_OK != win_ret) {
        GtpLog(s_obj->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning,
               "%s:%u<-->%s:%u fec restore: SnEntrySlidWin(data_win_r_) failed(0x%08x) pack_sn=%u.\r\n",
               s_obj->pb_dt_.self_ip_, (u32)(s_obj->pb_dt_.self_port_),
               s_obj->pb_dt_.peer_ip_, (u32)(s_obj->pb_dt_.peer_port_),
               win_ret, pack->pack_sn_);
    }

    return GTP_OK;
}

void GtpSession::ReorderClear() {
    for (u32 i = 0; i < REORDER_BUF_SIZE; i++) {
        if (NULL != reorder_buf_[i].frame) {
            pack_mem_pool_.FreeTranBuf(reorder_buf_[i].frame);
            reorder_buf_[i].frame = NULL;
        }
        reorder_buf_[i].placeholder_ = 0;
    }
    reorder_inited_       = 0;
    reorder_next_sn_      = 0;
    reorder_gap_since_us_ = 0;
}

void GtpSession::ReorderFlush(u64 ts_us) {
    while (true) {
        u32 idx = reorder_next_sn_ % REORDER_BUF_SIZE;
        ReorderSlot &slot = reorder_buf_[idx];
        if (NULL == slot.frame && 0 == slot.placeholder_) break;
        if (NULL != slot.frame) {
            cb_.receive_frame_cb_(app_gtp_hdl_, slot.frame, slot.frame_size, &slot.tran_addr);
            pack_mem_pool_.FreeTranBuf(slot.frame);
            slot.frame = NULL;
        }
        slot.placeholder_ = 0;
        reorder_next_sn_++;
    }

    bool found = false;
    for (u32 i = 0; i < REORDER_BUF_SIZE && !found; i++) {
        if (NULL != reorder_buf_[i].frame || 0 != reorder_buf_[i].placeholder_) found = true;
    }
    if (!found) {
        reorder_gap_since_us_ = 0;
    } else if (0 == reorder_gap_since_us_) {
        // A new gap remains after flush (e.g. second missing SN); restart timer.
        reorder_gap_since_us_ = ts_us;
    }
}

void GtpSession::ReorderEnqueue(u32 sn, u8 *frame, u32 frame_size,
                                 const GtpAddr *tran_addr, u64 ts_us,
                                 u32 first_sn_hint) {
    if (0 == reorder_inited_) {
        reorder_next_sn_ = sn;
        reorder_inited_  = 1;
    }

    i32 delta = (i32)(sn - reorder_next_sn_);

    if (delta < 0) {
        return;  // duplicate or late retransmit already passed
    }

    if (NULL == frame) {
        // Non-data SN (FEC packet, ACK, RTT probe): advance past it if in-order,
        // or mark as placeholder so ReorderFlush can skip it.
        if (0 == delta) {
            reorder_next_sn_++;
            reorder_gap_since_us_ = 0;
            ReorderFlush(ts_us);
        } else if ((u32)delta < REORDER_BUF_SIZE) {
            u32 idx = sn % REORDER_BUF_SIZE;
            // Don't overwrite a slot that already holds real recovered data (e.g. from FEC).
            // If the slot is empty, mark it as a placeholder so ReorderFlush can advance past it.
            if (NULL == reorder_buf_[idx].frame) {
                reorder_buf_[idx].placeholder_ = 1;
                if (0 == reorder_gap_since_us_) {
                    reorder_gap_since_us_ = ts_us;
                }
            }
        }
        return;
    }

    if (0 == delta) {
        // In-order data: deliver directly
        cb_.receive_frame_cb_(app_gtp_hdl_, frame, frame_size, (GtpAddr*)tran_addr);
        reorder_next_sn_++;
        reorder_gap_since_us_ = 0;
        ReorderFlush(ts_us);
        return;
    }

    // Out-of-order data: gap exists (delta > 0)
    if ((u32)delta >= REORDER_BUF_SIZE) {
        // ARQ retransmit fallback: if the caller supplied a first_sn_hint (logical SN) that
        // differs from sn (retransmit's new pack_sn_), land at the logical position instead
        // of flushing the entire window and skipping all buffered packets in between.
        if (UINT32_MAX != first_sn_hint && first_sn_hint != sn) {
            sn    = first_sn_hint;
            delta = (i32)(sn - reorder_next_sn_);
            if (delta < 0) {
                return;  // logical SN already delivered; discard
            }
            // Re-enter the normal buffering path with the corrected SN.
            // Fall through to the small-delta or delta==0 handling below.
            if (0 == delta) {
                cb_.receive_frame_cb_(app_gtp_hdl_, frame, frame_size, (GtpAddr*)tran_addr);
                reorder_next_sn_++;
                reorder_gap_since_us_ = 0;
                ReorderFlush(ts_us);
                return;
            }
            if ((u32)delta < REORDER_BUF_SIZE) {
                goto reorder_buffer_slot_;  // buffer at first_sn_hint's slot
            }
            // first_sn_hint is also a large gap — fall through to flush
        }
        // Gap too large: flush the buffered window first (preserves in-order delivery
        // for packets already buffered), then skip the large gap.
        for (u32 i = 0; i < REORDER_BUF_SIZE; i++) {
            u32 flush_sn = reorder_next_sn_ + i;
            u32 fidx     = flush_sn % REORDER_BUF_SIZE;
            if (NULL != reorder_buf_[fidx].frame) {
                cb_.receive_frame_cb_(app_gtp_hdl_, reorder_buf_[fidx].frame,
                                      reorder_buf_[fidx].frame_size, &reorder_buf_[fidx].tran_addr);
                pack_mem_pool_.FreeTranBuf(reorder_buf_[fidx].frame);
                reorder_buf_[fidx].frame = NULL;
            }
            reorder_buf_[fidx].placeholder_ = 0;
        }
        reorder_next_sn_      = sn;
        reorder_gap_since_us_ = 0;
        cb_.receive_frame_cb_(app_gtp_hdl_, frame, frame_size, (GtpAddr*)tran_addr);
        reorder_next_sn_++;
        return;
    }

reorder_buffer_slot_:

    u32 idx = sn % REORDER_BUF_SIZE;
    if (NULL != reorder_buf_[idx].frame) {
        pack_mem_pool_.FreeTranBuf(reorder_buf_[idx].frame);
        reorder_buf_[idx].frame = NULL;
    }

    u32 copy_size = 0;
    void *dummy_addr = NULL;
    u32 mem_spec = GtpPackSizeToMemSpec(frame_size);
    u8 *copy = pack_mem_pool_.MallocTranBuf(NULL, 0, &copy_size, &dummy_addr, (BufSizeType)mem_spec);
    if (NULL == copy) {
        // OOM: deliver directly to avoid stalling
        cb_.receive_frame_cb_(app_gtp_hdl_, frame, frame_size, (GtpAddr*)tran_addr);
        return;
    }

    memcpy(copy, frame, frame_size);
    reorder_buf_[idx].frame        = copy;
    reorder_buf_[idx].frame_size   = frame_size;
    reorder_buf_[idx].arrive_ts_us = ts_us;
    memcpy(&reorder_buf_[idx].tran_addr, tran_addr, sizeof(GtpAddr));

    if (0 == reorder_gap_since_us_) {
        reorder_gap_since_us_ = ts_us;
    }
}

u32 GtpSession::FramePrepHandler(void *frame, const u32 &frame_size, GtpPacket **out_pack, u32 *out_pack_size,
                         const u32 &hash, const u32 &user_id, const u32 &first_pack_sn, const u32 &resend_num) {
    u32 real_hash    = hash;
    u32 real_user_id = user_id;

    // when the session is identified by stream_key_, embed it into the existing hash_/user_id_ optional
    // field(carried under has_check_flag_) instead of the caller-supplied token/token_id, so the peer can
    // recover stream_key_ from the raw packet without a session lookup(see GtpCheckPacketInvalid()).
    if (GTP_YES == pb_dt_.tran_addr_.enable_key_) {
        real_hash    = (u32)(pb_dt_.tran_addr_.stream_key_ >> 32);
        real_user_id = (u32)(pb_dt_.tran_addr_.stream_key_ & 0x00000000FFFFFFFFULL);
    }

    if ((0xFF == pb_dt_.peer_version_) || (GTP_VERSION == pb_dt_.peer_version_)) {
        return FramePrepHandlerWithSelfVer(frame, frame_size, out_pack, out_pack_size, real_hash, real_user_id,
                                           first_pack_sn, resend_num);
    }

    return FramePrepHandlerWithPeerVer(frame, frame_size, out_pack, out_pack_size, real_hash, real_user_id,
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

    if ((kReliableStream <= pb_dt_.tran_addr_.stream_type_) && (0 == resend_num)) {
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

    if ((kReliableStream <= pb_dt_.tran_addr_.stream_type_) && (0 == resend_num)) {
        has_chg   = GTP_YES;
        hdr_size += sizeof(ChangeZone);
        move_pos -= sizeof(ChangeZone);

        ChangeZone *chg_zone = (ChangeZone*)move_pos;

        chg_zone->sort_sn_      = sort_sn_;
        chg_zone->senter_ts_us_ = 0;

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

        *((f32*)move_pos) = send_down_loss_;
        send_down_loss_   = -1.0;
    }

    if (measure_rtt_ts_us_ <= last_active_ts_us_) {
        has_ts    = GTP_YES;
        move_pos -= sizeof(last_active_ts_us_);
        hdr_size += sizeof(last_active_ts_us_);

        if (0 == resend_num) {
            *((u64 *)move_pos) = last_active_ts_us_;  // last_active_ts_us_ is too old when retransporting.
        } else {
            *((u64 *)move_pos) = GtpSysTimestampUs();
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

        *((u32 *)move_pos) = rtt_us_;
        rtt_us_            = 0;
    }

    if ((0 != user_id) || (0 != hash)) {
        has_check  = GTP_YES;
        move_pos  -= sizeof(user_id);
        hdr_size  += sizeof(user_id);

        *((u32 *)move_pos) = user_id;

        move_pos  -= sizeof(hash);
        hdr_size  += sizeof(hash);

        *((u32*)move_pos) = hash;
    }

    if (0 < resend_num) {
        move_pos -= sizeof(first_pack_sn);
        hdr_size += sizeof(first_pack_sn);

        *((u32 *)move_pos) = first_pack_sn;
    }

    move_pos -= sizeof(GtpPacket);
    hdr_size += sizeof(GtpPacket);

    if (64 <= hdr_size) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "%s:%u<-->%s:%u packet's header is too long("\
               "header_offset=%u peer_version=0x%02x resend_num=%u frame_size=%u pack_size=%u.\r\n",
               pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
               hdr_size, (u32)(pb_dt_.peer_version_), resend_num, frame_size, *out_pack_size);
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
    pack->stream_type_    = (kReliableStream <= pb_dt_.tran_addr_.stream_type_) ? 1 : 0;

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

        *((f32*)move_pos) = send_down_loss_;
        send_down_loss_   = -1.0;
    }

    if (measure_rtt_ts_us_ <= last_active_ts_us_) {
        has_ts    = GTP_YES;
        move_pos -= sizeof(last_active_ts_us_);
        hdr_size += sizeof(last_active_ts_us_);

        if (0 == resend_num) {
            *((u64 *)move_pos) = last_active_ts_us_;  // last_active_ts_us_ is too old when retransporting.
        } else {
            *((u64 *)move_pos) = GtpSysTimestampUs();
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

        *((u32 *)move_pos) = rtt_us_;
        rtt_us_            = 0;
    }

    if ((0 != user_id) || (0 != hash)) {
        has_check  = GTP_YES;
        move_pos  -= sizeof(user_id);
        hdr_size  += sizeof(user_id);

        *((u32 *)move_pos) = user_id;

        move_pos  -= sizeof(hash);
        hdr_size  += sizeof(hash);

        *((u32*)move_pos) = hash;
    }

    if (0 < resend_num) {
        move_pos -= sizeof(first_pack_sn);
        hdr_size += sizeof(first_pack_sn);

        *((u32 *)move_pos) = first_pack_sn;
    }

    move_pos -= sizeof(GtpPacket);
    hdr_size += sizeof(GtpPacket);

    if (64 <= hdr_size) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "%s:%u<-->%s:%u packet's header is too long("\
               "header_offset=%u peer_version=0x%02x resend_num=%u frame_size=%u pack_size=%u.\r\n",
               pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
               hdr_size, (u32)(pb_dt_.peer_version_), resend_num, frame_size, *out_pack_size);
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
    pack->stream_type_     = 0;

    pack_sn_ += 1;

    *out_pack      = pack;
    *out_pack_size = (u32)(pack->pack_size_);

    return GTP_OK;
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

u32 GtpSession::FramePostHandler(GtpAddr *tran_addr, GtpPacket *pack, const u32 &pack_size,
             const u32 &payload_size, const u32 &entry_arq_flag, const u32 &first_pack_sn, const u32 &edge_pack_flag) {
    pb_dt_.send_stat_.net_pack_sum_    += 1;
    pb_dt_.send_stat_.net_bitrate_sum_ += pack_size;

    if (0 == ((u8)(pack->repeat_counter_))) {
        pb_dt_.send_stat_.first_frame_sum_  += 1;
    } else {
        pb_dt_.send_stat_.retran_frame_sum_ += 1;
    }

    u32 run_result = GTP_OK;

    #if (1 == ENABLE_FEC)
    // step1: FEC encode.
    if ((0x00 != pb_dt_.peer_version_) && (GTP_ON == pb_dt_.alg_top_switch_)
     && ((u8)STREAM_QOS_WITH_FEC == GetStreamQos(kSenderQuality))) {
        run_result = fec2_obj_.Encode(pack, last_active_ts_us_);
        if (GTP_OK != run_result) {
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "%s:%u<-->%s:%u(new fec) calling Encode() "\
                   "failed(0x%08x).\r\n", pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
                   pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_), run_result);
        }
    }
    #endif

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
                if ((((u8)(GtpSessStat::kRunning)) == recv_session_stat_)
                 && ((recv_last_sync_ts_us_ + MIN_SYNC_PERIOD_US) <= last_active_ts_us_)) {
                    u32 sort_sn = 0;
                    if (0x02 <= pack->goodtp_ver_) {
                        if (GTP_YES == pack->has_chg_zone_) {
                            sort_sn = ((ChangeZone*)(((u8*)pack) + pack->header_offset_))->sort_sn_;
                        }
                    }

                    ResetSession(pack->pack_sn_, sort_sn, GTP_YES);
                }

                goto filter_repeat_start_pos_;
            }

            if (((u8)(GtpSessStat::kRunning)) != recv_session_stat_) {
                recv_session_stat_    = (u8)(GtpSessStat::kRunning);
                recv_last_sync_ts_us_ = last_active_ts_us_;
            }

filter_repeat_start_pos_:
            if (MAX_KEEPALIVE_TIME_LEN_US > (last_active_ts_us_ - last_recv_data_pack_ts_us_)) {
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
                    if (0x02 <= pack->goodtp_ver_) {
                        if (GTP_YES == pack->has_chg_zone_) {
                            sort_sn = ((ChangeZone*)(((u8*)pack) + pack->header_offset_))->sort_sn_;
                        }
                    }

                    // isn't mixed quintuple, force sync the slid window.
                    ResetSession(pack->pack_sn_, sort_sn, GTP_NO);
                }

update_last_sn_pos_:
                last_sn_ = pack->pack_sn_;
            }

            last_recv_data_pack_ts_us_ = last_active_ts_us_;

            if ((kReliableStream > pb_dt_.tran_addr_.stream_type_) || (0x02 > pack->goodtp_ver_)
             || (GTP_NO == pack->has_chg_zone_)) {
                if (0x00 == pack->goodtp_ver_) {
                    first_sn = *((u32*)(((u8*)pack) + pack->header_offset_));
                    goto filter_repeat_judge_pos_;
                }

                if (0 == pack->repeat_counter_) {
                    first_sn = pack->pack_sn_;
                } else {
                    first_sn = *((u32*)(((u8*)pack) + sizeof(GtpPacket)));
                }
            } else {
                first_sn = ((ChangeZone*)(((u8*)pack) + pack->header_offset_))->sort_sn_;
            }

filter_repeat_judge_pos_:
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
                rtt_us = *((u32*)move);
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
                com_cache_ts_us_    = *((u64*)move);  // using for testing RTT.
                at_com_cache_ts_us_ = last_active_ts_us_;

                pb_dt_.CalcSysClockSync(com_cache_ts_us_, at_com_cache_ts_us_);

                move += sizeof(u64);
            }

            if (GTP_YES == pack->has_loss_flag_) {
                send_loss = *((f32*)move);

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
            run_result = SnEntrySlidWin(data_win_r_, pack->pack_sn_, last_active_ts_us_);
            if (GTP_OK != run_result) {
                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                       "calling SnEntrySlidWin() failed(0x%08x).\r\n", run_result);
                #endif
            }

            // Method B: detect first new gap → start 2-shot NACK countdown (4ms apart)
            if (recv_max_data_sn_ != 0) {
                if ((i32)(pack->pack_sn_ - recv_max_data_sn_) > 1) {
                    new_gap_detected_ = 2;
                }
            }
            if (recv_max_data_sn_ == 0 || (i32)(pack->pack_sn_ - recv_max_data_sn_) > 0) {
                recv_max_data_sn_ = pack->pack_sn_;
            }

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

            if (GTP_YES == pack->cache_us_flag_) {
                cache_us = *((u64*)chg_len_zone);

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
            arq_.ProcAck(chg_len_zone, ack_load->sn_size_, ack_load->head_sn_, tail_sn, rto_sn);
            #endif

            pack_mem_pool_.FreeTranBuf((u8*)pack);

            break;
        }

        case ((u8)(GtpPackType::kGtpFecPackType)): {
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

            pack_mem_pool_.FreeTranBuf((u8*)pack);

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

                rtt_us = *((u32*)move);
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

            pack_mem_pool_.FreeTranBuf((u8*)pack);

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
                u64 ts_us  = *((u64*)move);
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

            pack_mem_pool_.FreeTranBuf((u8*)pack);

            break;
        }

        case ((u8)(GtpPackType::kGtpNackPackType)): {
            pb_dt_.recv_stat_.ack_sum_ += 1;  // TODO(Albert.feng) :: nack

            GtpNackPacketLoad *nack_load = (GtpNackPacketLoad*)(((u8*)pack) + pack->header_offset_);

            u8 *chg_len_zone = ((u8*)nack_load) + sizeof(GtpNackPacketLoad);
            u64 cache_us     = 0;
            u64 now_us       = GtpSysTimestampUs();
            u32 rtt_us       = 0;
            u32 tail_sn      = 0;
            u32 rto_sn       = 0;

            tail_sn = nack_load->head_sn_ + nack_load->tail_sn_offset_;
            rto_sn  = nack_load->head_sn_ + nack_load->rto_sn_offset_;

            if (GTP_YES == pack->cache_us_flag_) {
                cache_us = *((u64*)chg_len_zone);

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

            NackData *nack_data = (NackData*)(chg_len_zone - sizeof(NackData));

            {
            u32 head_sn  = nack_load->head_sn_;
            u32 rcv_loss = nack_load->recv_loss_;
            u32 nack_num = (u32)(nack_load->nack_num_);

            nack_data->head_sn_     = head_sn;
            nack_data->tail_sn_     = tail_sn;
            nack_data->recv_loss_   = rcv_loss;
            nack_data->rto_sn_      = rto_sn;
            nack_data->cache_ts_us_ = 0;
            nack_data->nack_num_    = (u16)nack_num;
            }

            run_result = NackSnEntrySlidWin(data_win_s_, nack_data, last_active_ts_us_);
            if (GTP_OK != run_result) {
                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug, "[win_hdl=%p]%s:%u<-->%s:%u nack sn is "\
                       "invalid.\r\n", data_win_s_, pb_dt_.self_ip_, (u32)(pb_dt_.self_port_),
                       pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_));
                #endif
            }

            #if (1 == ENABLE_ARQ)
            arq_.ProcNack((u16*)chg_len_zone, (u32)(nack_data->nack_num_), nack_data->head_sn_, nack_data->tail_sn_,
                          nack_data->rto_sn_);
            #endif

            pack_mem_pool_.FreeTranBuf((u8*)pack);

            break;
        }

        default: {
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "%s:%u---->%s:%u unknown packet "\
                   "type(0x%02x).\r\n", pb_dt_.peer_ip_, (u32)(pb_dt_.peer_port_),
                   pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), (u32)(pack->pack_type_));
            return_code = GEN_ERR(kGtpMgrMd, kUnknownPackTypeErr);

            pack_mem_pool_.FreeTranBuf((u8*)pack);
            break;
        }
    }

    return return_code;
}

void GtpSession::TimerHandler(const u64 &ts_us, ConsumeTime *wheel_consume) {
    pb_dt_.tran_addr_.timestamp_ = ts_us;

    #if (1 == ENABLE_ARQ)
    arq_.CheckRtoRetran(ts_us);
    #endif

    #if (1 == ENABLE_FEC)
    if ((u8)STREAM_QOS_WITH_FEC == GetStreamQos(kSenderQuality)) {
        // TODO(Albert.feng) :: send fec2 code at once when there isn't any new packet.
    }
    #endif

    SecondTimerHandler(ts_us, wheel_consume);

    u32 nret;

    if (GTP_NO == report_quality_flag_s_) {
        u64 delta = ts_us - create_ts_us_;
        if ((DEFAULT_RTO_TIMEOUT_US - (DEFAULT_RTO_TIMEOUT_US >> 4)) > delta) {
            goto timer_handler_continue_pos_;
        }

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
    if ((0 != rtt_us_) && ((gen_new_rtt_ts_us_ + MIN_RTT_EXIST_TM_SZ_US) <= ts_us)) {
        SendSetRecvRttPacket();
    }

    // Method B: 2-shot NACK for gap loss protection (tick N and N+4ms, independent drop probability)
    if (new_gap_detected_ > 0) {
        new_gap_detected_--;
        nret = CalcQualityByHandler(data_win_r_, ts_us, GTP_YES);
        if (GTP_OK != nret) {
            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
                   "new_gap CalcQualityByHandler() failed(0x%08x %s).\r\n",
                   nret, WinErrorInfo(data_win_r_, nret));
        }
        if (0xFFFFFFFFFFFFFFFF != recv_idle_calc_loss_ts_us) {
            recv_idle_calc_loss_ts_us = ts_us;
        }
    }

    if ((0xFFFFFFFFFFFFFFFF != recv_idle_calc_loss_ts_us)
     && ((recv_idle_calc_loss_ts_us + recv_idle_calc_period_us_) <= ts_us)) {
        {
        u32 burst_flag = nack_burst_detected_;
        nack_burst_detected_ = 0;
        if ((GTP_NO == burst_flag) &&
            (MAX_FEEDBACK_NACK_PERIOD_US >= (ts_us - last_feedback_nack_ts_us_))) {
            nret = CalcQualityByHandler(data_win_r_, ts_us, GTP_NO);
        } else {
            nret = CalcQualityByHandler(data_win_r_, ts_us, GTP_YES);
        }
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

    // Reorder gap timeout: skip a gap that hasn't been filled within 2×inter-packet interval.
    if (0 != reorder_gap_since_us_) {
        u32 pps = pb_dt_.recv_stat_.data_pack_pps_;
        if (0 == pps) pps = 1;
        u64 timeout_us = 2000000ULL / (u64)pps;    // 2×inter-packet: fast gap recovery
        if (timeout_us < 13000)  timeout_us = 13000;   // floor 13ms
        if (timeout_us > 500000) timeout_us = 500000;  // ceil 500ms

        if ((ts_us - reorder_gap_since_us_) >= timeout_us) {
            reorder_next_sn_++;
            reorder_gap_since_us_ = 0;
            ReorderFlush(ts_us);
        }
    }

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
    quality->bitrage_chg_k_         = 0;
    quality->context_               = pb_dt_.tran_addr_.context_;

    u32 tmp_congest_rank = 0;

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

        nret = ObtainNetworkQuality(data_win_s_, &(quality->loss_), &rrt_us, &jitter_us, &tmp_congest_rank,
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

        nret = ObtainNetworkQuality(data_win_r_, &(quality->loss_), &rrt_us, &jitter_us, &tmp_congest_rank,
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
                            ((kReliableStream > pb_dt_.tran_addr_.stream_type_) ? "Real time" : "reliable"));
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n  smooth_jitter= %s",
                            ((pb_dt_.tran_addr_.stream_type_ & 1) ? "Yes" : "No"));
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n disorder_delta= %u", ai_learn_sn_delta_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n max_retran_num= %u", (u32)(arq_.max_retran_times_));
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n  max_boost_num= %u", (u32)(arq_.max_boost_times_));
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n  harq_node_num= %u", arq_.arq_list_.node_num_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n ack_resend_counter= %u", arq_.ack_resend_counter_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n rto_resend_counter= %u", arq_.rto_resend_counter_);
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

    u16 has_check  = GTP_NO;
    u32 rtt_offset = (u32)sizeof(GtpPacket);

    if (GTP_YES == pb_dt_.tran_addr_.enable_key_) {
        has_check = GTP_YES;
        u8 *key_ptr = ((u8*)pack) + sizeof(GtpPacket);
        *((u32*)(key_ptr))              = (u32)(pb_dt_.tran_addr_.stream_key_ >> 32);
        *((u32*)(key_ptr + sizeof(u32))) = (u32)(pb_dt_.tran_addr_.stream_key_);
        rtt_offset += (u32)sizeof(u64);
    }

    u32 pack_size = rtt_offset + (u32)sizeof(u32);

    pack->goodtp_ver_     = CalcRightGtpVer(GTP_VERSION, pb_dt_.peer_version_);
    pack->header_offset_  = (u8)pack_size;
    pack->cache_us_flag_  = GTP_NO;
    pack->has_loss_flag_  = GTP_NO;
    pack->pack_type_      = (u8)(GtpPackType::kGtpSetRecvRttPackType);
    pack->pack_size_      = (u16)pack_size;
    pack->pack_sn_        = 0;         // the sn is invalid in set receiving rtt packet.
    pack->has_check_flag_ = (u8)has_check;
    pack->has_ts_flag_    = GTP_NO;
    pack->has_rtt_flag_   = GTP_YES;
    pack->init_flag_      = (u8)((((u8)(GtpSessStat::kRunning)) == send_session_stat_) ? GTP_NO : GTP_YES);
    pack->repeat_counter_ = 0;
    pack->is_qos_flg_     = GTP_NO;
    pack->share_zone_     = 0;

    *((u32*)(((u8*)(pack)) + rtt_offset)) = rtt_us_;

    #ifdef _SELFDEBUG
    GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug, "%s:%u<-->%s:%u send to receive windows's rrt=%uus.\r\n",
           pb_dt_.self_ip_, (u32)(pb_dt_.self_port_), pb_dt_.peer_ip_,
           (u32)(pb_dt_.peer_port_), rtt_us_);
    #endif

    rtt_us_ = 0;

    GtpHeaderNewToOld(pack, pb_dt_.peer_version_);

    u32 result = GTP_OK;

    #if (1 == ENABLE_MD_PERF_CHECK)
    {
    u64 app_consume_us = GtpSysTimestampUs();
    result = cb_.send_pack_cb_(GtpHdlIntToPointer(pb_dt_.gtp_hdl_), pack, pack_size, &(pb_dt_.tran_addr_));
    app_consume_us = GtpSysTimestampUs() - app_consume_us;
    pb_dt_.app_send_consume_.CacheConsumeTime(app_consume_us);
    pb_dt_.send_consume_.ts_us_ += app_consume_us;
    pb_dt_.recv_consume_.ts_us_ += app_consume_us;
    }
    #else
    result = cb_.send_pack_cb_(GtpHdlIntToPointer(pb_dt_.gtp_hdl_), pack, pack_size, &(pb_dt_.tran_addr_));
    #endif

    if (GTP_OK != result) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "call send_pack_cb_() failed(0x%08x)\r\n", result);
    }

    pb_dt_.send_stat_.net_pack_sum_    += 1;
    pb_dt_.send_stat_.net_bitrate_sum_ += pack_size;

    return;
}

void GtpSession::SendRttTestResPacket(const u64 &ts_us) {
    u8 tmp_buf[1024];
    GtpPacket *pack = (GtpPacket*)tmp_buf;

    u16 has_check = GTP_NO;
    u32 ts_offset = (u32)sizeof(GtpPacket);

    if (GTP_YES == pb_dt_.tran_addr_.enable_key_) {
        has_check = GTP_YES;
        u8 *key_ptr = ((u8*)pack) + sizeof(GtpPacket);
        *((u32*)(key_ptr))              = (u32)(pb_dt_.tran_addr_.stream_key_ >> 32);
        *((u32*)(key_ptr + sizeof(u32))) = (u32)(pb_dt_.tran_addr_.stream_key_);
        ts_offset += (u32)sizeof(u64);
    }

    u32 pack_size = ts_offset + (u32)sizeof(u64);

    pack->goodtp_ver_     = CalcRightGtpVer(GTP_VERSION, pb_dt_.peer_version_);
    pack->header_offset_  = (u8)pack_size;
    pack->cache_us_flag_  = GTP_NO;
    pack->has_loss_flag_  = GTP_NO;
    pack->pack_type_      = (u8)(GtpPackType::kGtpRttTstResPackType);
    pack->pack_size_      = (u16)pack_size;
    pack->pack_sn_        = 0;
    pack->has_check_flag_ = (u8)has_check;
    pack->has_ts_flag_    = GTP_YES;
    pack->has_rtt_flag_   = GTP_NO;
    pack->init_flag_      = (u8)(((u8)(GtpSessStat::kRunning) == recv_session_stat_) ? GTP_NO : GTP_YES);
    pack->repeat_counter_ = 0;
    pack->is_qos_flg_     = GTP_NO;
    pack->share_zone_     = 0;

    *((u64*)(((u8*)(pack)) + ts_offset)) = ts_us;

    GtpHeaderNewToOld(pack, pb_dt_.peer_version_);

    u32 result = GTP_OK;

    #if (1 == ENABLE_MD_PERF_CHECK)
    {
    u64 app_consume_us = GtpSysTimestampUs();
    result = cb_.send_pack_cb_(GtpHdlIntToPointer(pb_dt_.gtp_hdl_), pack, pack_size, &(pb_dt_.tran_addr_));
    app_consume_us = GtpSysTimestampUs() - app_consume_us;
    pb_dt_.app_send_consume_.CacheConsumeTime(app_consume_us);

    pb_dt_.send_consume_.ts_us_ += app_consume_us;
    pb_dt_.recv_consume_.ts_us_ += app_consume_us;
    }
    #else
    result = cb_.send_pack_cb_(GtpHdlIntToPointer(pb_dt_.gtp_hdl_), pack, pack_size, &(pb_dt_.tran_addr_));
    #endif

    if (GTP_OK != result) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError,
               "call send_pack_cb_() failed(0x%08x)\r\n", result);
    }

    pb_dt_.send_stat_.net_pack_sum_    += 1;
    pb_dt_.send_stat_.net_bitrate_sum_ += pack_size;

    return;
}

void GtpSession::ResetSession(const u32 &cur_sn, const u32 &sort_sn, const u32 &chg_status_flag) {
    ack_sn_ = 0;

    if (GTP_YES == chg_status_flag) {
        recv_session_stat_ = (u8)(GtpSessStat::kInitRes);
    }

    #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
    u32 l_border_sn = cur_sn & 0xFFFFFF80;
    #endif

    #if (2 == APPLICATION_TYPE)
    u32 l_border_sn = cur_sn & 0xFFFFFFC0;
    #endif

    ResetSlidWin(data_win_r_, l_border_sn, last_active_ts_us_);
    ResetSlidWin(filter_win_, l_border_sn, last_active_ts_us_);

    ReorderClear();

    if (GTP_YES == chg_status_flag) {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "the peer sending has been recreated, "\
               "now reset the receiving win(%p) and the filter win(%p) 's left border sn to %u, sort_sn=%u\r\n",
               data_win_r_, filter_win_, l_border_sn,  sort_sn);
    } else {
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "the network has been restored, "\
               "now sync the receiving win(%p) and the filter win(%p) 's left border sn to %u, sort_sn=%u\r\n",
               data_win_r_, filter_win_, l_border_sn, sort_sn);
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

