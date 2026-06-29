/*********************************************************************************************************************

                         Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_arq.cpp
  Version  : Initial
  Author   : Albort.Feng
  Function : Realizes the arq function code file.
  Modify record:
  1.Date   : August 28, 2023
    Author : Albort.Feng
    Content: New creates file.

*********************************************************************************************************************/
#include "goodtp_arq.h"
#include "goodtp_mgr.h"
#include "goodtp_comstruct.h"
#include "goodtp_macrodefine.h"
#include "goodtp.h"
#include "goodtp_session.h"

#include <string>

using namespace std;

static inline u32 GtpArqSnSpan(const u32 &head_sn, const u32 &sn) {
    if (head_sn <= sn) {
        return sn - head_sn;
    }

    return sn + (0xFFFFFFFF - head_sn) + 1;
}

static inline u32 GtpArqAckedIn32Bitmap(const u32 *ack_bitmap, const u32 &bitmap_sz, const u32 &sn_span) {
    const u32 idx = sn_span >> 5;
    if (((idx + 1) * sizeof(u32)) > bitmap_sz) {
        return GTP_NO;
    }

    return (0 != (ack_bitmap[idx] & (0x01 << (sn_span & 0x0000001F)))) ? GTP_YES : GTP_NO;
}

static inline u32 GtpArqAckedIn64Bitmap(const u64 *ack_bitmap, const u32 &bitmap_sz, const u32 &sn_span) {
    const u32 idx = sn_span >> 6;
    const u64 bit_value = 1;
    if (((idx + 1) * sizeof(u64)) > bitmap_sz) {
        return GTP_NO;
    }

    return (0 != (ack_bitmap[idx] & (bit_value << (sn_span & 0x0000003F)))) ? GTP_YES : GTP_NO;
}

#define PopArqNode(list, cur_node) {\
    if ((cur_node) == (list).head_) {\
        /*pop head node*/ \
        (list).head_ = (list).head_->nxt_node_;\
        if (NULL != (list).head_) {\
            (list).head_->pre_node_ = (list).head_;\
        } else {\
            (list).tail_ = NULL;\
        }\
    } else if ((cur_node) == (list).tail_) {\
        /*pop tail node*/ \
        (list).tail_ = (list).tail_->pre_node_;\
        (list).tail_->nxt_node_ = NULL;\
    } else {\
        /*pop mid node*/ \
        (cur_node)->pre_node_->nxt_node_ = (cur_node)->nxt_node_;\
        (cur_node)->nxt_node_->pre_node_ = (cur_node)->pre_node_;\
    }\
    if (0 < (list).node_num_) {\
        (list).node_num_ -= 1;\
    }\
}

#define PushPack(list, node) {\
    (node)->nxt_node_ = NULL;\
    if (NULL == (list).head_) {\
        (node)->pre_node_ = (node);\
        (list).head_      = (node);\
    } else {\
        (list).tail_->nxt_node_ = (node);\
        (node)->pre_node_       = (list).tail_;\
    }\
    (list).tail_      = (node);\
    (list).node_num_ += 1;\
}

#ifdef __cplusplus
extern "C" {
#endif

GtpArq::GtpArq(const u32 &rto_timeout_us, const u32 &max_retran_times, const GtpCallBackParam &cb,
               TranMemPool &pack_mem_pool) :
    arq_packet_pool_(NULL),
    pb_dt_(NULL),
    rto_timeout_us_(rto_timeout_us),
    max_retran_times_((u8)max_retran_times),
    max_boost_times_(INIT_BOOST_TIMES),
    boost_switch_(GTP_ON),
    s_cur_loss_rate_(0.0),
    s_loss_dir_(0),
    r_loss_dir_(0),
    rto_resend_counter_(0),
    ack_resend_counter_(0),
    ack_err_counter_(0),
    send_pack_cb_(NULL),
    cb_(cb),
    pack_mem_pool_(pack_mem_pool) {
    arq_list_.head_          = NULL;
    arq_list_.tail_          = NULL;
    arq_list_.node_num_      = 0;
    arq_list_.ai_repair_sum_ = 0;
}

GtpArq::~GtpArq() {
}

u32 GtpArq::Init(GtpMemPool *arq_packet_pool, pSendPackCallBack send_pack_cb, pPackRetran retran_cb,
                 SessionPublicData *public_data) {
    arq_packet_pool_ = arq_packet_pool;
    send_pack_cb_    = send_pack_cb;
    retran_cb_       = retran_cb;
    pb_dt_           = public_data;

    return GTP_OK;
}

u32 GtpArq::PacketEntryList(GtpPacket *pack, GtpAddr *tran_addr, const u32 &first_pack_sn, const u64 &ts_us) {
    ArqNode *node = (ArqNode*)arq_packet_pool_->MallocItem();
    if (NULL == node) {
        const u8* arq_mem_status = arq_packet_pool_->GetMemPoolStatus();

        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelError, "%s:%u<-->%s:%u calling MallocItem() "\
               "failed for arq node(%s).\r\n", pb_dt_->self_ip_, (u32)(pb_dt_->self_port_), pb_dt_->peer_ip_,
               (u32)(pb_dt_->peer_port_), arq_mem_status);

        RETURN_ERR(kGtpArqMd, kCrtArqNodeFailed);
    }

    if (NULL == pack_mem_pool_.TranBufUseRefAddOne((u8*)pack)) {
        const string &err_info = pack_mem_pool_.Error();
        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelError, "%s:%u<-->%s:%u call TranBufUseRefAddOne() "
               "failed(%s pack=%p req_size=%u).\r\n",
               pb_dt_->self_ip_, (u32)(pb_dt_->self_port_), pb_dt_->peer_ip_,
               (u32)(pb_dt_->peer_port_), err_info.c_str(), pack, (u32)(pack->pack_size_));

        arq_packet_pool_->FreeItem(node);
        RETURN_ERR(kGtpArqMd, kMallocPackMemFailed);
    }

    node->last_send_ts_us_ = ts_us;
    node->pack_            = (u8*)pack;
    node->tran_addr_       = (u8*)tran_addr;
    node->pack_len_        = (u16)(pack->pack_size_);
    node->try_agin_flg_    = GTP_NO;
    node->payload_offset_  = (u8)(pack->header_offset_);
    node->retran_counter_  = (u8)(pack->repeat_counter_);
    node->failed_ack_      = GTP_NO;
    node->has_boost_node_  = GTP_NO;
    node->boost_node_flg_  = GTP_NO;
    node->boost_threshold_ = ((0 == max_boost_times_) ? DEFAULT_BOOST_TIMES :  max_boost_times_);
    node->pack_sn_         = pack->pack_sn_;
    node->first_pack_sn_   = first_pack_sn;

    PushPack(arq_list_, node);

    if ((GTP_OFF == boost_switch_) || (0 == max_boost_times_)) {
        return GTP_OK;
    }

    // Guard against boosting during quiet periods after an intermittent burst.
    // After PushPack, node_num_ includes the just-added original.
    // A small backlog means ACKs are flowing and the network is currently healthy.
    // Only clone when backlog >= 1+2*max_boost_times_, requiring at least
    // max_boost_times_ consecutive losses to build up before boosting kicks in.
    const u32 boost_backlog_thresh = 1u + 2u * (u32)max_boost_times_;
    if (arq_list_.node_num_ < boost_backlog_thresh) {
        return GTP_OK;
    }

    CloneBoostNode(node, ts_us);

    return GTP_OK;
}

void GtpArq::CheckRtoRetran(const u64 &cur_ts_us) {
    ArqNode *cur_head = arq_list_.head_;
    ArqNode *nxt_node = NULL;
    u32 ret           = GTP_OK;
    HarqReTranType retran_type = HarqReTranType::kNotRetranType;

    if (NULL == cur_head) {
        return;
    }

    if (GTP_OFF == pb_dt_->alg_top_switch_) {
        ClearArqList();
        return;
    }

    u64 rto_ts_us     = cur_ts_us - ((u64)rto_timeout_us_);
    u64 bst_ts_us     = cur_ts_us - ((u64)boost_period_us_);

    while ((NULL != cur_head) && (cur_ts_us > cur_head->last_send_ts_us_)) {
        nxt_node = cur_head->nxt_node_;

        if (GTP_YES == JudgeIsTranFailed(cur_head, rto_ts_us)) {
            ((GtpAddr*)(cur_head->tran_addr_))->loss_      = (u8)s_cur_loss_rate_;
            ((GtpAddr*)(cur_head->tran_addr_))->timestamp_ = cur_ts_us;

            ack_err_counter_ += 1;

            #ifdef _SELFDEBUG
            GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelNotice, "%s:%u<-->%s:%u retran failed, now cb out"\
                "(sn=%u first_sn=%u boost_node=%s retran_num=%u max_retran_num=%u max_boost_num=%u loss=%5.2f%%(%s)"\
                "rto_resend_num=%u boost_resend_num=%u ack_resend_num=%u ack_err_num=%u rto=%uus bst_period=%uus "\
                "reason=%s).\r\n", pb_dt_->self_ip_, (u32)(pb_dt_->self_port_),
                pb_dt_->peer_ip_, (u32)(pb_dt_->peer_port_), cur_head->pack_sn_,
                cur_head->first_pack_sn_, ((GTP_YES == cur_head->boost_node_flg_) ? "yes" : "no"),
                (u32)(cur_head->retran_counter_), (u32)max_retran_times_, (u32)max_boost_times_, s_cur_loss_rate_,
                ((0 == s_loss_dir_) ? "up":"down"), rto_resend_counter_, arq_list_.ai_repair_sum_,
                ack_resend_counter_, ack_err_counter_, rto_timeout_us_, boost_period_us_,
                ((GTP_YES == cur_head->failed_ack_) ? "ack_failed" : "timeout"));
            #endif

            PopArqNode(arq_list_, cur_head);

            TranFailedPostHandler(cur_head, cur_ts_us);

            goto harq_check_resend_next_pos_;
        }

        retran_type = JudgeCanRtoReSend(cur_head, rto_ts_us, bst_ts_us);
        if (HarqReTranType::kNotRetranType != retran_type) {
            PopArqNode(arq_list_, cur_head);

            ret = retran_cb_(pb_dt_->session_, cur_head, (GtpAddr*)(cur_head->tran_addr_), cur_ts_us);
            if (GTP_OK != ret) {
                GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelError,
                       "calling PackRetransmit() failed(0x%08x)\r\n", ret);
                PushPack(arq_list_, cur_head);
                goto harq_check_resend_next_pos_;
            }

            if (HarqReTranType::kBoostRetranType == retran_type) {
                arq_list_.ai_repair_sum_ += 1;

                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "%s:%u<-->%s:%u boost retraned packet"\
                       "(pack_sn=%u first_sn=%u rto=%uus)\r\n", pb_dt_->self_ip_,
                       (u32)(pb_dt_->self_port_), pb_dt_->peer_ip_, (u32)(pb_dt_->peer_port_),
                       cur_head->pack_sn_, cur_head->first_pack_sn_, rto_timeout_us_);
                #endif
            } else {
                rto_resend_counter_ += 1;

                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "%s:%u<-->%s:%u rto retraned packet"\
                       "(pack_sn=%u first_sn=%u rto=%uus)\r\n", (char*)(pb_dt_->self_ip_),
                       (u32)(pb_dt_->self_port_), (char*)(pb_dt_->peer_ip_), (u32)(pb_dt_->peer_port_),
                       cur_head->pack_sn_, cur_head->first_pack_sn_, rto_timeout_us_);
                #endif
            }

            PushPack(arq_list_, cur_head);
        }

harq_check_resend_next_pos_:
        cur_head = nxt_node;
    }

    return;
}

void GtpArq::AdjustSWinCurrentLossRate(const f32 &cur_loss_rate, const u32 &loss_dir) {
    s_cur_loss_rate_ = cur_loss_rate;
    s_loss_dir_      = (u8)loss_dir;
}

void GtpArq::AdjustRWinCurrentLossRate(const f32 &cur_loss_rate, const u32 &loss_dir) {
    r_cur_loss_rate_ = cur_loss_rate;
    r_loss_dir_      = (u8)loss_dir;
}

void GtpArq::AdjustRetranTimes(const u32 &max_retran_times) {
    max_retran_times_ = (u8)max_retran_times;
    #ifdef _SELFDEBUG
    GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelDebug, "adjust max retran times to %u\r\n", max_retran_times);
    #endif
}

void GtpArq::AdjustRtoTimeout(const u32 &rto_timeout_us, const u32 &rtt_us) {
    boost_period_us_ = (rtt_us >> 6);
    boost_period_us_ = GtpLimit(MIN_BOOST_PERIOD_US, MAX_BOOST_PERIOD_US, boost_period_us_);
    rto_timeout_us_  = GtpLimit(MIN_RTO_US, MAX_RTO_US, rto_timeout_us);

    return;
}

void GtpArq::ProcAck(const u8 ack_sn_bitmap[], const u32 &bitmap_sz, const u32 &head_sn,
                     const u32 &tail_sn, const u32 &rto_sn, const u32 &recv_loss) {
    #ifdef _SELFDEBUG
    GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "%s:%u<->%s:%u proc ack head_sn=%u tail_sn=%u "\
           "rto_sn=%u.\r\n", pb_dt_->self_ip_, (u32)(pb_dt_->self_port_),
           pb_dt_->peer_ip_, (u32)(pb_dt_->peer_port_), head_sn, tail_sn, rto_sn);

    if (NULL != arq_list_.head_) {
        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "%s:%u<->%s:%u arq_head_sn=%u arq_tail_sn=%u "\
               "arq_size=%u\r\n", pb_dt_->self_ip_, (u32)(pb_dt_->self_port_),
               pb_dt_->peer_ip_, (u32)(pb_dt_->peer_port_),
               arq_list_.head_->pack_sn_, arq_list_.tail_->pack_sn_, arq_list_.node_num_);
    } else {
        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "%s:%u<->%s:%u arq list is null.\r\n",
               pb_dt_->self_ip_, (u32)(pb_dt_->self_port_),
               pb_dt_->peer_ip_, (u32)(pb_dt_->peer_port_));
    }
    #endif

    if (8 == sizeof(void*)) {
        ProcArqIn64BitSys(ack_sn_bitmap, bitmap_sz, head_sn, tail_sn, rto_sn, recv_loss);
    } else {
        ProcArqIn32BitSys(ack_sn_bitmap, bitmap_sz, head_sn, tail_sn, rto_sn, recv_loss);
    }

    return;
}

void GtpArq::ProcArqIn32BitSys(const u8 ack_sn_bitmap[], const u32 &bitmap_sz, const u32 &head_sn, const u32 &tail_sn,
                               const u32 &rto_sn, const u32 &recv_loss) {
    if (NULL == arq_list_.head_) {
        return;
    }

    ArqNode *cur_node = arq_list_.head_;
    ArqNode *del_node = arq_list_.head_;

    u8 *gtp_pack_vec[FREE_C3BUF_NUM_PER];

    const u32 *ack_bitmap = (const u32*)ack_sn_bitmap;

    u32 run_result        = GTP_OK;
    u32 cach_pos          = 0;
    u32 head_tail_sn_span = 0;
    u32 head_curr_sn_span = 0;
    u32 head_rto_sn_span  = 0;
    u32 first_sn          = 0;

    if (head_sn <= tail_sn) {
        head_tail_sn_span = tail_sn - head_sn;
    } else {
        head_tail_sn_span = tail_sn + (0xFFFFFFFF - head_sn) + 1;
    }

    if (head_sn <= rto_sn) {
        head_rto_sn_span = rto_sn - head_sn;
    } else {
        head_rto_sn_span = rto_sn + (0xFFFFFFFF - head_sn) + 1;
    }

    #ifdef _SELFDEBUG
    GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "head_tail_sn_span=%u head_rto_sn_span=%u\r\n",
           head_tail_sn_span, head_rto_sn_span);
    #endif

    #if (1 == ENABLE_ARQ_BOOST_FLAG)
    u64 cur_ts_us = GtpSysTimestampUs();
    #endif

    do {
        head_curr_sn_span = GtpArqSnSpan(head_sn, cur_node->pack_sn_);

        if (cur_node->first_pack_sn_ != cur_node->pack_sn_) {
            const u32 head_first_sn_span = GtpArqSnSpan(head_sn, cur_node->first_pack_sn_);
            if ((head_tail_sn_span >= head_first_sn_span)
             && (GTP_YES == GtpArqAckedIn32Bitmap(ack_bitmap, bitmap_sz, head_first_sn_span))) {
                goto ack_32bit_del_node_pos_;
            }
        }

        #ifdef _SELFDEBUG
        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "cur_sn=%u cur_sn_span=%u\r\n",
               cur_node->pack_sn_, head_curr_sn_span);
        #endif

        if (head_tail_sn_span < head_curr_sn_span) {
            if (SLID_WIN_SIZE >= head_curr_sn_span) {
                break;
            }
            goto arq_32bit_quick_resend_pos_;
        }

        if (GTP_NO == GtpArqAckedIn32Bitmap(ack_bitmap, bitmap_sz, head_curr_sn_span)) {
            #if (2 != APPLICATION_TYPE)
            if (0 == recv_loss) {
                cur_node = cur_node->nxt_node_;
                goto arq_32bit_next_normal_pos_;
            }
            #endif

            if (head_rto_sn_span >= head_curr_sn_span) {
arq_32bit_quick_resend_pos_:
                // current list is harq list.
                if (GTP_YES == JudgeExhautResendNum(cur_node)) {
                    // this can't call frame_failed_cb_(), because this may dirty application env.
                    cur_node->failed_ack_ = GTP_YES;

                    #if (0 == SUPPORT_RELIABLE_TRAN)
                    goto ack_32bit_del_node_pos_;
                    #endif

                    cur_node = cur_node->nxt_node_;
                    goto arq_32bit_next_normal_pos_;
                }

                cur_node = cur_node->nxt_node_;

                PopArqNode(arq_list_, del_node);

                // quick resend.
                run_result = retran_cb_(pb_dt_->session_, del_node, (GtpAddr*)(del_node->tran_addr_), 0);
                if (GTP_OK != run_result) {
                    GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelError,
                           "calling PackRetransmit() failed(0x%08x)\r\n", run_result);
                    PushPack(arq_list_, del_node);
                    goto arq_32bit_next_normal_pos_;
                }

                ack_resend_counter_ += 1;

                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "quick retraned packet(source=ack_bitmap "
                       "pack_sn=%u first_sn=%u head_sn=%u tail_sn=%u rto_sn=%u cur_span=%u recv_loss=%u)\r\n",
                       del_node->pack_sn_, del_node->first_pack_sn_, head_sn, tail_sn, rto_sn,
                       head_curr_sn_span, recv_loss);
                #endif

                #if (1 == ENABLE_ARQ_BOOST_FLAG)
                if (GTP_NO == del_node->has_boost_node_) {
                    CloneBoostNode(del_node, cur_ts_us);

                    if (0 == max_boost_times_) {
                        max_boost_times_ = DEFAULT_BOOST_TIMES;
                    }
                }
                #endif

                PushPack(arq_list_, del_node);

                goto arq_32bit_next_normal_pos_;
            }

            cur_node = cur_node->nxt_node_;
            goto arq_32bit_next_normal_pos_;
        }

ack_32bit_del_node_pos_:
        first_sn = cur_node->first_pack_sn_;

        gtp_pack_vec[cach_pos] = cur_node->pack_;

        cach_pos += 1;

        if (FREE_C3BUF_NUM_PER <= cach_pos) {
            pack_mem_pool_.FreeTranBuf(gtp_pack_vec, cach_pos);
            cach_pos = 0;
        }

        cur_node = cur_node->nxt_node_;

        PopArqNode(arq_list_, del_node);
        arq_packet_pool_->FreeItem(del_node);
        DelNodeByFirstSn(first_sn, &cur_node);

arq_32bit_next_normal_pos_:
        del_node = cur_node;
    } while (NULL != cur_node);

    if (0 < cach_pos) {
        pack_mem_pool_.FreeTranBuf(gtp_pack_vec, cach_pos);
        cach_pos = 0;
    }

    return;
}

void GtpArq::ProcArqIn64BitSys(const u8 ack_sn_bitmap[], const u32 &bitmap_sz, const u32 &head_sn, const u32 &tail_sn,
                               const u32 &rto_sn, const u32 &recv_loss) {
    if (NULL == arq_list_.head_) {
        return;
    }

    ArqNode *cur_node = arq_list_.head_;
    ArqNode *del_node = arq_list_.head_;

    u8 *gtp_pack_vec[FREE_C3BUF_NUM_PER];

    const u64 *ack_bitmap = (const u64*)ack_sn_bitmap;

    u32 run_result = GTP_OK;
    u32 cach_pos   = 0;

    u32 head_tail_sn_span = 0;
    u32 head_curr_sn_span = 0;
    u32 head_rto_sn_span  = 0;
    u32 first_sn          = 0;

    if (head_sn <= tail_sn) {
        head_tail_sn_span = tail_sn - head_sn;
    } else {
        head_tail_sn_span = tail_sn + (0xFFFFFFFF - head_sn) + 1;
    }

    if (head_sn <= rto_sn) {
        head_rto_sn_span = rto_sn - head_sn;
    } else {
        head_rto_sn_span = rto_sn + (0xFFFFFFFF - head_sn) + 1;
    }

    #ifdef _SELFDEBUG
    GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "head_tail_sn_span=%u head_rto_sn_span=%u\r\n",
           head_tail_sn_span, head_rto_sn_span);
    #endif

    #if (1 == ENABLE_ARQ_BOOST_FLAG)
    u64 cur_ts_us = GtpSysTimestampUs();
    #endif

    do {
        head_curr_sn_span = GtpArqSnSpan(head_sn, cur_node->pack_sn_);

        if (cur_node->first_pack_sn_ != cur_node->pack_sn_) {
            const u32 head_first_sn_span = GtpArqSnSpan(head_sn, cur_node->first_pack_sn_);
            if ((head_tail_sn_span >= head_first_sn_span)
             && (GTP_YES == GtpArqAckedIn64Bitmap(ack_bitmap, bitmap_sz, head_first_sn_span))) {
                goto ack_64bit_del_node_pos_;
            }
        }

        #ifdef _SELFDEBUG
        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "cur_sn=%u cur_sn_span=%u\r\n",
               cur_node->pack_sn_, head_curr_sn_span);
        #endif

        if (head_tail_sn_span < head_curr_sn_span) {
            if (SLID_WIN_SIZE >= head_curr_sn_span) {
                break;
            }
            goto arq_64bit_quick_resend_pos_;
        }

        if (GTP_NO == GtpArqAckedIn64Bitmap(ack_bitmap, bitmap_sz, head_curr_sn_span)) {
            #if (2 != APPLICATION_TYPE)
            if (0 == recv_loss) {
                cur_node = cur_node->nxt_node_;
                goto arq_64bit_next_normal_pos_;
            }
            #endif

            if (head_rto_sn_span >= head_curr_sn_span) {
arq_64bit_quick_resend_pos_:
                // current list is harq list.
                if (GTP_YES == JudgeExhautResendNum(cur_node)) {
                    // this can't call frame_failed_cb_(), because this maybe pollute application env.
                    cur_node->failed_ack_ = GTP_YES;

                    #if (0 == SUPPORT_RELIABLE_TRAN)
                    goto ack_64bit_del_node_pos_;
                    #endif

                    cur_node = cur_node->nxt_node_;
                    goto arq_64bit_next_normal_pos_;
                }

                cur_node = cur_node->nxt_node_;

                PopArqNode(arq_list_, del_node);

                // quick resend.
                run_result = retran_cb_(pb_dt_->session_, del_node, (GtpAddr*)(del_node->tran_addr_), 0);
                if (GTP_OK != run_result) {
                    GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelError,
                           "calling PackRetransmit() failed(0x%08x)\r\n", run_result);
                    PushPack(arq_list_, del_node);
                    goto arq_64bit_next_normal_pos_;
                }

                ack_resend_counter_ += 1;

                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "quick retraned packet(source=ack_bitmap "
                       "pack_sn=%u first_sn=%u head_sn=%u tail_sn=%u rto_sn=%u cur_span=%u recv_loss=%u)\r\n",
                       del_node->pack_sn_, del_node->first_pack_sn_, head_sn, tail_sn, rto_sn,
                       head_curr_sn_span, recv_loss);
                #endif

                #if (1 == ENABLE_ARQ_BOOST_FLAG)
                if (GTP_NO == del_node->has_boost_node_) {
                    CloneBoostNode(del_node, cur_ts_us);

                    if (0 == max_boost_times_) {
                        max_boost_times_ = DEFAULT_BOOST_TIMES;
                    }
                }
                #endif

                PushPack(arq_list_, del_node);

                goto arq_64bit_next_normal_pos_;
            }

            cur_node = cur_node->nxt_node_;
            goto arq_64bit_next_normal_pos_;
        }

ack_64bit_del_node_pos_:
        first_sn = cur_node->first_pack_sn_;

        gtp_pack_vec[cach_pos] = cur_node->pack_;

        cach_pos += 1;

        if (FREE_C3BUF_NUM_PER <= cach_pos) {
            pack_mem_pool_.FreeTranBuf(gtp_pack_vec, cach_pos);
            cach_pos = 0;
        }

        cur_node = cur_node->nxt_node_;

        PopArqNode(arq_list_, del_node);
        arq_packet_pool_->FreeItem(del_node);
        DelNodeByFirstSn(first_sn, &cur_node);

arq_64bit_next_normal_pos_:
        del_node = cur_node;
    } while (NULL != cur_node);

    if (0 < cach_pos) {
        pack_mem_pool_.FreeTranBuf(gtp_pack_vec, cach_pos);
        cach_pos = 0;
    }

    return;
}

void GtpArq::ProcNack(const u16 nack_sn_offset[], const u32 &nack_num, const u32 &head_sn, const u32 &tail_sn,
                      const u32 &rto_sn) {
    #ifdef _SELFDEBUG
    GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "%s:%u<->%s:%u proc nack head_sn=%u tail_sn=%u "\
           "rto_sn=%u nack_num=%u.\r\n", pb_dt_->self_ip_, (u32)(pb_dt_->self_port_),
           pb_dt_->peer_ip_, (u32)(pb_dt_->peer_port_), head_sn, tail_sn, rto_sn, nack_num);

    if (NULL != arq_list_.head_) {
        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "%s:%u<->%s:%u arq_head_sn=%u arq_tail_sn=%u "\
               "arq_size=%u\r\n", pb_dt_->self_ip_, (u32)(pb_dt_->self_port_),
               pb_dt_->peer_ip_, (u32)(pb_dt_->peer_port_), arq_list_.head_->pack_sn_,
               arq_list_.tail_->pack_sn_, arq_list_.node_num_);
    } else {
        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "%s:%u<->%s:%u arq list is null.\r\n",
               pb_dt_->self_ip_, (u32)(pb_dt_->self_port_),
               pb_dt_->peer_ip_, (u32)(pb_dt_->peer_port_));
    }
    #endif

    if (0 == nack_num) {
        ProcNotLossNack(head_sn, rto_sn);
    } else {
        ProcHasLossNack((u16_p)nack_sn_offset, nack_num, head_sn, tail_sn, rto_sn);
    }

    return;
}

void GtpArq::ProcNotLossNack(const u32 &head_sn, const u32 &tail_sn) {
    if (NULL == arq_list_.head_) {
        return;
    }

    ArqNode *cur_node = arq_list_.head_;
    ArqNode *del_node = arq_list_.head_;

    u8 *gtp_pack_vec[FREE_C3BUF_NUM_PER];

    u32 run_result = GTP_OK;
    u32 cach_pos   = 0;

    u32 head_tail_sn_span = 0;
    u32 head_curr_sn_span = 0;
    u32 first_sn          = 0;

    if (head_sn <= tail_sn) {
        head_tail_sn_span = tail_sn - head_sn;
    } else {
        head_tail_sn_span = tail_sn + (0xFFFFFFFF - head_sn) + 1;
    }

    #ifdef _SELFDEBUG
    GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "head_tail_sn_span=%u max_retran_times=%u\r\n",
           head_tail_sn_span, (u32)max_retran_times_);
    #endif

    do {
        if (head_sn <= cur_node->pack_sn_) {
            head_curr_sn_span = cur_node->pack_sn_ - head_sn;
        } else {
            head_curr_sn_span = cur_node->pack_sn_ + (0xFFFFFFFF - head_sn) + 1;
        }

        #ifdef _SELFDEBUG
        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "cur_sn=%u cur_sn_span=%u retran_counter=%u\r\n",
               cur_node->pack_sn_, head_curr_sn_span, (u32)(cur_node->retran_counter_));
        #endif

        if (head_tail_sn_span < head_curr_sn_span) {
            if (SLID_WIN_SIZE >= head_curr_sn_span) {
                break;
            }

            // current list is harq list.
            #ifdef _SELFDEBUG
            GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "%s:%u<-->%s:%u current sn is too large, need "\
                   "be resended(retran_num=%u max_retran_num=%u no loss nack:head=%u tail=%u cur_sn=%u\r\n",
                   pb_dt_->self_ip_, (u32)(pb_dt_->self_port_), pb_dt_->peer_ip_,
                   (u32)(pb_dt_->peer_port_), (u32)(cur_node->retran_counter_), (u32)max_retran_times_,
                   head_sn, tail_sn, cur_node->pack_sn_);
            #endif

            if (GTP_YES == JudgeExhautResendNum(cur_node)) {
                // this can't call frame_failed_cb_(), because this maybe pollute application env.
                cur_node->failed_ack_ = GTP_YES;

                #if (0 == SUPPORT_RELIABLE_TRAN)
                goto nack_noloss_del_node_pos_;
                #endif

                cur_node = cur_node->nxt_node_;
                goto no_nack_next_normal_pos_;
            }

            cur_node = cur_node->nxt_node_;

            PopArqNode(arq_list_, del_node);

            // quick resend.
            run_result = retran_cb_(pb_dt_->session_, del_node, (GtpAddr*)(del_node->tran_addr_), 0);
            if (GTP_OK != run_result) {
                GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelError,
                       "calling PackRetransmit() failed(0x%08x)\r\n", run_result);
                PushPack(arq_list_, del_node);
                goto no_nack_next_normal_pos_;
            }

            ack_resend_counter_ += 1;

            #ifdef _SELFDEBUG
            GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "%s:%u<-->%s:%u retraned packet(sn=%u "\
                   "first_sn=%u)\r\n",pb_dt_->self_ip_, (u32)(pb_dt_->self_port_),
                   pb_dt_->peer_ip_, (u32)(pb_dt_->peer_port_), del_node->pack_sn_,
                   del_node->first_pack_sn_);
            #endif

            PushPack(arq_list_, del_node);

            goto no_nack_next_normal_pos_;
        }

#if (0 == SUPPORT_RELIABLE_TRAN)
nack_noloss_del_node_pos_:
#endif
        first_sn = cur_node->first_pack_sn_;

        gtp_pack_vec[cach_pos] = cur_node->pack_;

        cach_pos += 1;

        if (FREE_C3BUF_NUM_PER <= cach_pos) {
            pack_mem_pool_.FreeTranBuf(gtp_pack_vec, cach_pos);
            cach_pos = 0;
        }

        cur_node = cur_node->nxt_node_;

        PopArqNode(arq_list_, del_node);
        arq_packet_pool_->FreeItem(del_node);
        DelNodeByFirstSn(first_sn, &cur_node);

no_nack_next_normal_pos_:
        del_node = cur_node;
    } while (NULL != cur_node);

    if (0 < cach_pos) {
        pack_mem_pool_.FreeTranBuf(gtp_pack_vec, cach_pos);
        cach_pos = 0;
    }

    return;
}

void GtpArq::ProcHasLossNack(const u16_p &nack_sn_offset, const u32 &nack_num, const u32 &head_sn, const u32 &tail_sn,
                             const u32 &rto_sn) {
    if (NULL == arq_list_.head_) {
        return;
    }

    ArqNode *cur_node = arq_list_.head_;
    ArqNode *del_node = arq_list_.head_;

    u8 *c3buf_vec[FREE_C3BUF_NUM_PER];

    u32 run_result = GTP_OK;
    u32 cach_pos   = 0;

    u32 head_curr_sn_span = 0;
    u32 head_rto_sn_span  = 0;
    u32 first_sn          = 0;

    if (head_sn <= rto_sn) {
        head_rto_sn_span = rto_sn - head_sn;
    } else {
        head_rto_sn_span = rto_sn + (0xFFFFFFFF - head_sn) + 1;
    }

    #ifdef _SELFDEBUG
    GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "head_rto_sn_span=%u\r\n", head_rto_sn_span);

    for (u32 i = 0; nack_num > i; ++i) {
        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "nack_sn=%u\r\n", head_sn + ((u32)nack_sn_offset[i]));
    }
    #endif

    #if (1 == ENABLE_ARQ_BOOST_FLAG)
    u64 cur_ts_us = GtpSysTimestampUs();
    #endif

    do {
        if (head_sn <= cur_node->pack_sn_) {
            head_curr_sn_span = cur_node->pack_sn_ - head_sn;
        } else {
            head_curr_sn_span = cur_node->pack_sn_ + (0xFFFFFFFF - head_sn) + 1;
        }

        #ifdef _SELFDEBUG
        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "cur_sn=%u cur_sn_span=%u\r\n",
               cur_node->pack_sn_, head_curr_sn_span);
        #endif

        if (head_rto_sn_span < head_curr_sn_span) {
            if (SLID_WIN_SIZE >= head_curr_sn_span) {
                break;
            }
            goto nack_quick_resend_pos_;
        }

        if ((GTP_YES == CheckCurSnIsDiscard(head_sn, (u16_p)nack_sn_offset, nack_num, cur_node->pack_sn_))
         || ((cur_node->first_pack_sn_ != cur_node->pack_sn_)
          && (GTP_YES == CheckCurSnIsDiscard(head_sn, (u16_p)nack_sn_offset, nack_num, cur_node->first_pack_sn_)))) {
            if (head_rto_sn_span >= head_curr_sn_span) {
nack_quick_resend_pos_:
                // current list is harq list.
                if (GTP_YES == JudgeExhautResendNum(cur_node)) {
                    // this can't call frame_failed_cb_(), because this maybe pollute application env.
                    cur_node->failed_ack_ = GTP_YES;

                    #if (0 == SUPPORT_RELIABLE_TRAN)
                    goto nack_hasloss_del_node_pos_;
                    #endif

                    cur_node = cur_node->nxt_node_;
                    goto has_nack_next_normal_pos_;
                }

                cur_node = cur_node->nxt_node_;

                PopArqNode(arq_list_, del_node);

                // quick resend.
                run_result = retran_cb_(pb_dt_->session_, del_node, (GtpAddr*)(del_node->tran_addr_), 0);
                if (GTP_OK != run_result) {
                    GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelError,
                           "calling PackRetransmit() failed(0x%08x)\r\n", run_result);
                    PushPack(arq_list_, del_node);
                    goto has_nack_next_normal_pos_;
                }

                ack_resend_counter_ += 1;

                #ifdef _SELFDEBUG
                GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelInfo, "quick retraned packet(source=nack "
                       "pack_sn=%u first_sn=%u head_sn=%u tail_sn=%u rto_sn=%u cur_span=%u nack_num=%u)\r\n",
                       del_node->pack_sn_, del_node->first_pack_sn_, head_sn, tail_sn, rto_sn,
                       head_curr_sn_span, nack_num);
                #endif

                #if (1 == ENABLE_ARQ_BOOST_FLAG)
                if (GTP_NO == del_node->has_boost_node_) {
                    CloneBoostNode(del_node, cur_ts_us);

                    if (0 == max_boost_times_) {
                        max_boost_times_ = DEFAULT_BOOST_TIMES;
                    }
                }
                #endif

                PushPack(arq_list_, del_node);

                goto has_nack_next_normal_pos_;
            }

            cur_node = cur_node->nxt_node_;
            goto has_nack_next_normal_pos_;
        }
#if (0 == SUPPORT_RELIABLE_TRAN)
nack_hasloss_del_node_pos_:
#endif
        first_sn = cur_node->first_pack_sn_;

        c3buf_vec[cach_pos] = cur_node->pack_;

        cach_pos += 1;

        if (FREE_C3BUF_NUM_PER <= cach_pos) {
            pack_mem_pool_.FreeTranBuf(c3buf_vec, cach_pos);
            cach_pos = 0;
        }

        cur_node = cur_node->nxt_node_;

        PopArqNode(arq_list_, del_node);
        arq_packet_pool_->FreeItem(del_node);
        DelNodeByFirstSn(first_sn, &cur_node);

has_nack_next_normal_pos_:
        del_node = cur_node;
    } while (NULL != cur_node);

    if (0 < cach_pos) {
        pack_mem_pool_.FreeTranBuf(c3buf_vec, cach_pos);
        cach_pos = 0;
    }

    return;
}

void GtpArq::ClearArqList(void) {
    if (NULL == arq_list_.head_) {
        return;
    }

    ArqNode *cur_node = arq_list_.head_;
    ArqNode *nxt_node = cur_node->nxt_node_;

    do {
        #ifdef _SELFDEBUG
        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelDebug,
               "%s:%u<-->%s:%u closed algorithm, clear arq(sn=%u first_sn=%u retran_counter=%u "\
               "max_retran_times=%u loss=%5.2f%%(%s)).\r\n",
               pb_dt_->self_ip_, (u32)(pb_dt_->self_port_), pb_dt_->peer_ip_,
               (u32)(pb_dt_->peer_port_), cur_node->pack_sn_, cur_node->first_pack_sn_,
               (u32)(cur_node->retran_counter_), (u32)max_retran_times_, s_cur_loss_rate_,
               ((0 == s_loss_dir_) ? "up":"down"));
        #endif

        pack_mem_pool_.FreeTranBuf(cur_node->pack_);
        arq_packet_pool_->FreeItem(cur_node);

        cur_node = nxt_node;
        if (NULL != cur_node) {
            nxt_node = cur_node->nxt_node_;
        }
    } while (NULL != cur_node);

    arq_list_.head_     = NULL;
    arq_list_.tail_     = NULL;
    arq_list_.node_num_ = 0;

    return;
}

void GtpArq::PopAllPack(const u64 &ts_us) {
    if (NULL == arq_list_.head_) {
        return;
    }

    ArqNode *cur_node = arq_list_.head_;
    ArqNode *nxt_node = cur_node->nxt_node_;

    do {
        #ifdef _SELFDEBUG
        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelDebug,
               "%s:%u<-->%s:%u hasn't been ack when session dead, now cb out(sn=%u first_sn=%u retran_counter=%u "\
               "max_retran_times=%u loss=%5.2f%%(%s) rto_resend_num=%u ack_resend_num=%u boost_resend_num=%u).\r\n",
               pb_dt_->self_ip_, (u32)(pb_dt_->self_port_), pb_dt_->peer_ip_,
               (u32)(pb_dt_->peer_port_), cur_node->pack_sn_, cur_node->first_pack_sn_,
               (u32)(cur_node->retran_counter_), (u32)max_retran_times_, s_cur_loss_rate_,
               ((0 == s_loss_dir_) ? "up":"down"), rto_resend_counter_, ack_resend_counter_,
               arq_list_.ai_repair_sum_);
        #endif

        pack_mem_pool_.FreeTranBuf(cur_node->pack_);
        arq_packet_pool_->FreeItem(cur_node);

        cur_node = nxt_node;
        if (NULL != cur_node) {
            nxt_node = cur_node->nxt_node_;
        }
    } while (NULL != cur_node);

    arq_list_.head_     = NULL;
    arq_list_.tail_     = NULL;
    arq_list_.node_num_ = 0;

    return;
}

void GtpArq::GetStat(u32 *ack_resend_num, u32 *rto_resend_num, u32 *ack_err_num, u32 *boost_resend_num) {
    *ack_resend_num   = ack_resend_counter_;
    *rto_resend_num   = rto_resend_counter_;
    *ack_err_num      = ack_err_counter_;
    *boost_resend_num = arq_list_.ai_repair_sum_;
    return;
}

u32 GtpArq::CheckCurSnIsDiscard(const u32 &head_sn, const u16_p &nack_sn_offset, const u32 &nack_num,
                                const u32 &cur_sn) {
    u32 loop    = 0;
    u32 ret_val = GTP_NO;

    while (nack_num > loop) {
        if (cur_sn == (head_sn + ((u32)(nack_sn_offset[loop])))) {
            ret_val = GTP_YES;
            break;
        }

        loop += 1;
    }

    return ret_val;
}

// @return: GTP_NO(don't exhaut resend number), GTP_YES(has exhauted resend number)
u32 GtpArq::JudgeExhautResendNum(const ArqNode *node) {
    u8 max_resend_std = max_retran_times_;

    if (GTP_YES == node->boost_node_flg_) {
        max_resend_std = node->boost_threshold_;
    }

    if (max_resend_std <= node->retran_counter_) {
        return GTP_YES;
    }

    return GTP_NO;
}

/* @return: GTP_NO (unkown transporting sucessfully, maybe resend stilly),
            GTP_YES(transported failedly, can't transport continuely) */
u32 GtpArq::JudgeIsTranFailed(const ArqNode *node, const u64 &rto_ts_us) {
    if (GTP_YES == node->failed_ack_) {
        return GTP_YES;
    }

    if ((GTP_YES == JudgeExhautResendNum(node))
     && (rto_ts_us >= node->last_send_ts_us_)) {
        return GTP_YES;
    }

    return GTP_NO;
}

HarqReTranType GtpArq::JudgeCanRtoReSend(const ArqNode *node, const u64 &rto_ts_us, const u64 &bst_rto_ts_us) {
    if ((kRealTimeStream == pb_dt_->tran_addr_.stream_type_) && (0 == pb_dt_->recv_stat_.ack_sum_)) {
        return HarqReTranType::kNotRetranType;
    }

    if (GTP_YES == node->boost_node_flg_) {
        if ((kRealTimeStream == pb_dt_->tran_addr_.stream_type_) && (FLOAT_ZERO >= s_cur_loss_rate_)) {
            return HarqReTranType::kNotRetranType;
        }

        if ((node->boost_threshold_ > node->retran_counter_)
         && (bst_rto_ts_us >= node->last_send_ts_us_)) {
            return HarqReTranType::kBoostRetranType;
        }

        return HarqReTranType::kNotRetranType;
    }

    if ((kRealTimeStream == pb_dt_->tran_addr_.stream_type_) && (FLOAT_ZERO >= s_cur_loss_rate_)) {
        return HarqReTranType::kNotRetranType;
    }

    if ((max_retran_times_ > node->retran_counter_)
     && (rto_ts_us >= node->last_send_ts_us_)) {
        return HarqReTranType::kRtoRetranType;;
    }

    return HarqReTranType::kNotRetranType;
}

void GtpArq::CloneBoostNode(ArqNode *org_node, const u64 &ts_us) {
    #if (0 == ENABLE_BOOST_AI_FLAG)
    return;
    #endif

    u8 *pack_mem = NULL;
    u32 mem_size = 0;
    u32 mem_spec = GtpPackSizeToMemSpec(org_node->pack_len_);
    GtpAddr *gtp_addr = NULL;
    ArqNode *bst_node = NULL;

    pack_mem = pack_mem_pool_.MallocTranBuf(NULL, 0, &mem_size, (void**)(&gtp_addr), (BufSizeType)mem_spec);
    if (NULL == pack_mem) {
        const string &err_info = pack_mem_pool_.Error();
        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelError, "%s:%u<-->%s:%u call MallocTranBuf() "
               "failed(%s req_size=%u mem_spec=%u).\r\n",
               pb_dt_->self_ip_, (u32)(pb_dt_->self_port_), pb_dt_->peer_ip_,
               (u32)(pb_dt_->peer_port_), err_info.c_str(), (u32)(org_node->pack_len_), mem_spec);
        return;
    }

    bst_node = (ArqNode*)arq_packet_pool_->MallocItem();
    if (NULL == bst_node) {
        pack_mem_pool_.FreeTranBuf(pack_mem);

        const u8* arq_mem_status = arq_packet_pool_->GetMemPoolStatus();

        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelError, "%s:%u<-->%s:%u call MallocItem() "\
               "failed for arq node(%s).\r\n", pb_dt_->self_ip_, (u32)(pb_dt_->self_port_),
               pb_dt_->peer_ip_, (u32)(pb_dt_->peer_port_), arq_mem_status);
        return;
    }

    memcpy(pack_mem, org_node->pack_, (u32)(org_node->pack_len_));
    memcpy(gtp_addr, org_node->tran_addr_, sizeof(GtpAddr));

    bst_node->last_send_ts_us_ = ts_us - 1000000;  // next loop must be sended out.
    bst_node->pack_            = pack_mem;
    bst_node->tran_addr_       = (u8*)gtp_addr;
    bst_node->pack_len_        = (u16)(((GtpPacket*)pack_mem)->pack_size_);
    bst_node->try_agin_flg_    = GTP_YES;  // the boost node can't try again to retransport.
    bst_node->payload_offset_  = (u8)(((GtpPacket*)pack_mem)->header_offset_);
    bst_node->retran_counter_  = 0;
    bst_node->has_boost_node_  = GTP_YES;
    bst_node->boost_node_flg_  = GTP_YES;
    bst_node->boost_threshold_ = org_node->boost_threshold_;
    bst_node->failed_ack_      = GTP_NO;
    bst_node->pack_sn_         = ((GtpPacket*)pack_mem)->pack_sn_;
    bst_node->first_pack_sn_   = org_node->first_pack_sn_;

    PushPack(arq_list_, bst_node);

    org_node->has_boost_node_  = GTP_YES;

    return;
}

void GtpArq::TranFailedPostHandler(ArqNode *node, const u64 &cur_ts_us) {
    if ((kRealTimeStream == pb_dt_->tran_addr_.stream_type_) || (GTP_OFF == pb_dt_->alg_top_switch_)
     || (GTP_YES == node->boost_node_flg_) || (GTP_YES == node->try_agin_flg_)) {
        pack_mem_pool_.FreeTranBuf((u8*)(node->pack_));
        arq_packet_pool_->FreeItem(node);

        return;
    }

    node->retran_counter_ = 0;
    node->failed_ack_     = GTP_NO;
    node->boost_node_flg_ = GTP_NO;
    node->try_agin_flg_   = GTP_YES;

    u32 ret = retran_cb_(pb_dt_->session_, node, (GtpAddr*)(node->tran_addr_), cur_ts_us);
    if (GTP_OK != ret) {
        GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelError,
               "calling PackRetransmit() failed(0x%08x)\r\n", ret);
    }

    #ifdef _SELFDEBUG
    const ChangeZone *chg_zone = GtpGetPacketChangeZone((GtpPacket*)node->pack_);
    const u32 sort_sn = (NULL == chg_zone) ? 0 : chg_zone->sort_sn_;

    GtpLog(cb_.write_log_cb_, kGtpArqMd, kGtpLogLevelNotice, "%s:%u<-->%s:%u the packet resends failedly, now send again"\
           "because of reliable stream(pack_sn=%u first_sn=%u sort_sn=%u rto=%uus)\r\n", pb_dt_->self_ip_,
           (u32)(pb_dt_->self_port_), pb_dt_->peer_ip_, (u32)(pb_dt_->peer_port_),
           node->pack_sn_, node->first_pack_sn_, sort_sn, rto_timeout_us_);
    #endif

    if ((GTP_YES == node->has_boost_node_)
     && (MAX_BOOST_TIMES_PER_ONCE > node->boost_threshold_)) {
        node->boost_threshold_ += 1;
    }

    CloneBoostNode(node, cur_ts_us);

    PushPack(arq_list_, node);
}

void GtpArq::DelNodeByFirstSn(const u32 &first_sn, ArqNode **next_node) {
    ArqNode *cur_head = arq_list_.head_;
    ArqNode *nxt_node = NULL;

    while (NULL != cur_head) {
        nxt_node = cur_head->nxt_node_;

        if (first_sn == cur_head->first_pack_sn_) {
            if ((NULL != next_node) && ((*next_node) == cur_head)) {
                *next_node = nxt_node;
            }

            PopArqNode(arq_list_, cur_head);
            pack_mem_pool_.FreeTranBuf((u8*)(cur_head->pack_));
            arq_packet_pool_->FreeItem(cur_head);
        }

        cur_head = nxt_node;
    }

    return;
}

#ifdef __cplusplus
}
#endif
