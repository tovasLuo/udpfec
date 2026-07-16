#ifndef _GOOD_TP_ARQ_H_
#define _GOOD_TP_ARQ_H_
/*********************************************************************************************************************

                             Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_arq.h
  Version  : Initial
  Author   : Albort.Feng
  Function : Realizes the arq function header file.
  Modify record:
  1.Date   : August 28, 2023
    Author : Albort.Feng
    Content: New creates file.

*********************************************************************************************************************/
#include "goodtp_comstruct.h"
#include "goodtp_macrodefine.h"
#include "goodtp_mem_pool.h"
#include "goodtp.h"

#include "tranmempool.h"

#define USING_4K_ARRAY              (1)
#define USING_8K_ARRAY              (0)

#if (USING_4K_ARRAY)
#define ARQ_NODE_ARRAY_SIZE         (0x0200)
#define ARQ_NODE_ARRAY_MIN_POS_MASK (0xFFFFFF80)
#define ARQ_NODE_ARRAY_POS_MASK     (0x01FF)
#define SEQ_NO_POS_MASK             (0xFFFF)
#define RIGHT_SHIFT_BIT_COUNT       (7)
#elif (USING_8K_ARRAY)
#define ARQ_NODE_ARRAY_SIZE         (0x0400)
#define ARQ_NODE_ARRAY_MIN_POS_MASK (0xFFFFFFC0)
#define ARQ_NODE_ARRAY_POS_MASK     (0x03FF)
#define SEQ_NO_POS_MASK             (0xFFFF)
#define RIGHT_SHIFT_BIT_COUNT       (6)
#else
#define ARQ_NODE_ARRAY_SIZE         (0x010000)
#define ARQ_NODE_ARRAY_MIN_POS_MASK (0xFFFFFFFF)
#define ARQ_NODE_ARRAY_POS_MASK     (0xFFFF)
#define SEQ_NO_POS_MASK             (0xFFFF)
#define RIGHT_SHIFT_BIT_COUNT       (0)
#endif

typedef u32 (*pPackRetran)(void*, ArqNode*, GtpAddr*, const u64&);

#ifdef __cplusplus
extern "C" {
#endif

class GtpArq {
 public:
    typedef u16* u16_p;

    GtpArq(const u32 &rto_timeout_us, const u32 &max_retran_times, const GtpCallBackParam &cb,
           TranMemPool &pack_mem_pool);
    ~GtpArq();

    u32 Init(GtpMemPool *arq_packet_pool, pSendPackCallBack send_pack_cb, pPackRetran retran_cb,
             SessionPublicData *public_data);

    u32 PacketEntryList(GtpPacket *pack, GtpAddr *tran_addr, const u32 &first_pack_sn, const u64 &ts_us);

    void CheckRtoRetran(const u64 &cur_ts_us);

    void AdjustSWinCurrentLossRate(const f32 &cur_loss_rate, const u32 &loss_dir);
    void AdjustRWinCurrentLossRate(const f32 &cur_loss_rate, const u32 &loss_dir);

    void AdjustRetranTimes(const u32 &max_retran_times);

    void AdjustRtoTimeout(const u32 &rto_timeout_us, const u32 &rtt_us);

    void ProcAck(const u8 ack_sn_bitmap[], const u32 &bitmap_sz, const u32 &head_sn, const u32 &tail_sn,
                 const u32 &rto_sn, const u32 &recv_loss);

    void ProcNack(const u16 nack_sn_offset[], const u32 &nack_num, const u32 &head_sn, const u32 &tail_sn,
                  const u32 &rto_sn);

    void PopAllPack(const u64 &ts_us);

    void GetStat(u32 *ack_resend_num, u32 *rto_resend_num, u32 *ack_err_num, u32 *boost_resend_num);

PRIVATE:
    
    void ProcArqIn32BitSys(const u8 ack_sn_bitmap[], const u32 &bitmap_sz, const u32 &head_sn, const u32 &tail_sn,
                           const u32 &rto_sn, const u32 &recv_loss);

    void ProcArqIn64BitSys(const u8 ack_sn_bitmap[], const u32 &bitmap_sz, const u32 &head_sn, const u32 &tail_sn,
                           const u32 &rto_sn, const u32 &recv_loss);

    void ProcNotLossNack(const u32 &head_sn, const u32 &tail_sn);

    void ProcHasLossNack(const u16_p &nack_sn_offset, const u32 &nack_num, const u32 &head_sn, const u32 &tail_sn,
                         const u32 &rto_sn);

    u32  CheckCurSnSpanIsDiscard(const u64 nack_offset_bitmap[], const u16_p &nack_sn_offset,
                                  const u32 &nack_num, const u32 &cur_sn_span);

    u32  JudgeExhautResendNum(const ArqNode *node);
    u32  JudgeIsTranFailed(const ArqNode *node, const u64 &rto_ts_us);
    HarqReTranType JudgeCanRtoReSend(const ArqNode *node, const u64 &rto_ts_us, const u64 &bst_rto_ts_us);

    void ClearArqList(void);
    void CloneBoostNode(ArqNode *node, const u64 &ts_us);
    void TranFailedPostHandler(ArqNode *node, const u64 &cur_ts_us);
    void DelNodeByFirstSn(const u32 &first_sn, ArqNode **next_node);
    u32  ShouldTriggerBoost(void) const;
    u32  IsRetranTooStale(const ArqNode *node, const u32 &head_sn) const;

 public:
    u32 rto_timeout_us_;
    u32 boost_period_us_;

    f32 s_cur_loss_rate_;     // send slid window's loss
    f32 r_cur_loss_rate_;     // recv slid window's loss

    u32 ack_err_counter_;

    u32 s_loss_dir_:1;        // send slid window's loss direction.
    u32 r_loss_dir_:1;        // recv slid window's loss direction.
    u32 boost_switch_:1;
    u32 bit_rsv_:13;
    u32 max_retran_times_:8;
    u32 max_boost_times_:8;

    u32 rto_resend_counter_;
    u32 ack_resend_counter_;

    // Retransmissions skipped by IsRetranTooStale() -- distinct from ack_err_counter_ (which
    // also fires for genuinely exhausted retry counts): this one is "never even attempted because
    // it would've been dropped on arrival anyway", not "attempted and gave up".
    u32 stale_retran_skip_counter_;

    // Latest receiver window low edge (head_sn from ProcAck()/ProcNack()), kept around so
    // CheckRtoRetran()'s RTO-driven path -- which has no ACK/NACK of its own to read a fresh
    // head_sn from -- can still apply the same staleness check the quick-resend paths apply
    // inline. See IsRetranTooStale().
    u32  last_known_head_sn_;
    u32  has_known_head_sn_:1;

    ArqList arq_list_;

PRIVATE:
    pSendPackCallBack send_pack_cb_;

    const GtpCallBackParam &cb_;

    pPackRetran retran_cb_;

    GtpMemPool *arq_packet_pool_;

    SessionPublicData *pb_dt_;

    TranMemPool &pack_mem_pool_;
};

#ifdef __cplusplus
}
#endif

#endif
