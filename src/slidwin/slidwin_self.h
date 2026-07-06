#ifndef _SLID_WIN_SELF_H_
#define _SLID_WIN_SELF_H_

#include "slidwin.h"

#include <stdlib.h>

// 0: common application, 1: vedio application, 2: game acceleration
#define APPLICATION_TYPE  (2)

#define SELF_SUCESS       (0)
#define SELF_ERROR        (-1)

#define SELF_SN_OUT_L_ERR (0x10000001)
#define SELF_SN_OUT_R_ERR (0x20000002)

#define SELF_NO           (0)
#define SELF_YES          (1)
#define SELF_NO_MOVE      (0)
#define SELF_RESET_WIN    (-1)

#define COM_CACHE_SIZE              (1536)
#define NORMAL_WIN_RESERVE_SZ       (2048)

#define MAX_SAVE_LOSS_SN_NUM        ((((COM_CACHE_SIZE) - sizeof(NackData)) >> 1))

#define SlidWinLimit(min, max, value) ((((min) > (value)) ? (min) : (((max) < (value)) ? (max) : (value))))

#define CorrectWinRsvSize(org_size, pps) (((org_size) < NORMAL_WIN_RESERVE_SZ) ? (org_size) : \
                                           ((org_size) <= ((pps) >> 1) ? (org_size) : ((pps) >> 1)))

#if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
#define MAX_FEEDBACK_NACK_SN_NUM    (32)

#define WIN_SPECS                   (6)
#define WIN_POS_MASK                (0x0000FFFF)

#define MIN_CALC_LOSS_SN_NUM        (10)
#define MIN_LEARN_THRESHOLD         (10)

#define MAX_RESET_RECV_WIN_THRSHLD  (8)
#define RESET_RECV_WIN_SN_MASK      (0xFFFFFF80)

#define DEFAULT_DISORDER_NUM        (3)

#endif

#if (2 == APPLICATION_TYPE)
#define MAX_FEEDBACK_NACK_SN_NUM    (12)
#define MAX_GAME_FAST_NACK_SN_NUM   (64)

#define WIN_SPECS                   (3)
#define WIN_POS_MASK                (0x00001FFF)

#define MIN_CALC_LOSS_SN_NUM        (3)
#define MIN_LEARN_THRESHOLD         (3)

#define MAX_RESET_RECV_WIN_THRSHLD  (4)
#define RESET_RECV_WIN_SN_MASK      (0xFFFFFFC0)

#define DEFAULT_DISORDER_NUM        (2)

#endif

#define MIN_RSV_WIN_BIT_SZ          (64)

#define MIN_MOVE_STEP_BIT_MASK      (0x0000FFC0)

#define WIN_BUF_SIZE                ((1024 << WIN_SPECS))
#define SLID_WIN_U64_BUF_SZ         ((WIN_BUF_SIZE >> 6))
#define INVALID_WIN_POS             ((WIN_BUF_SIZE))

#define MIN_WIN_SZ                  ((WIN_BUF_SIZE >> 1))
#define MAX_WIN_SZ                  (((WIN_BUF_SIZE >> 1) + (WIN_BUF_SIZE >> 2)))
#define MIN_MOVE_STEP               (64)
#define MAX_MOVE_STEP               ((MAX_WIN_SZ >> 1))

#define MAX_RTT_CACHE_SZ            (16)
#define MIN_CALC_RTT_THRESHOLD      (8)

#define MAX_PPS_CACHE_SZ            (4)

#define MAX_ERR_INFO_SZ             (256)
#define MIN_MEM_GAP_SZ              (8)

#define MAX_SLID_WIN_OBJ_SIZE_1M    ((1024 << 10))

#define DEFAULT_CALC_LOSS_SPAN_US   (100000)
#define DEFAULT_RTO_TS_US           (450000)
#define MIN_SND_CALC_LOSS_SPAN_US   (8000)
#define MAX_SND_CALC_LOSS_SPAN_US   (110000)

#define MIN_RCV_CALC_LOSS_SPAN_US   (4000)
#define MAX_RCV_CALC_LOSS_SPAN_US   (55000)

#define MIN_FEEDBACK_SPAN_US        (350000)
#define MAX_FEEDBACK_SPAN_US        (800000)
#define MIN_RTO_TS_US               (25000)
#define MAX_DISORDER_BUF_US         (10000)
#define MIN_DISORDER_BUF_US         (1000)
#define LOW_PPS_DISORDER_THRESHOLD  (80)
#define MAX_LOW_PPS_DISORDER_BUF_US (15000)
#define MIN_LOW_PPS_DISORDER_BUF_US (2000)

#define MIN_FEEDBACK_FACTOR         (0.1)
#define MAX_FEEDBACK_FACTOR         (1.0)

#define EXPAND_LOSS_FACTOR_FLOAT    (128.0)
#define EXPNAD_LOSS_PACTOR_INT      (12)

#define DEFAULT_RECV_RTO_DEC_K      (8)
#define MIN_RECV_RTO_DEC_K          (1)
#define MAX_RECV_RTO_DEC_K          (30)

#define ENABLE_SECURE_PROTECT       (0)

#define ISNT_REPEAT_SN              (0)
#define IS_REPEAT_SN                (1)
#define ISNT_RECEIVE_WIN            (2)

#define MAX_FORCE_NUM_BITMAP_UN_CHG (3)

#ifdef _UTTEST
#define PRIVATE        public
#define PROTECTED      public
#else
#define PRIVATE        private
#define PROTECTED      protected
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef float    f32;
typedef double   f64;

typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

enum class SlidWinErrorCode: u32 {
    kUnknownError       = 0x00000000,  // no any error.
    kMallocMemoryFailed = 0x00000001,  // no enough free memory.
    kInvalidSlidWinHdl  = 0x00000002,  // invalid slid window handler.
    kInputParamIsNull   = 0x00000003,  // input parameter point is null.
    kSnIsInvalid        = 0x00000004,  // current sn is invalid.
    kNoSupportSetParam  = 0x00000005,  // setting no supported in send mode for the current parameter.
    kInvalidNackSn      = 0x00000006,  // nack packet's head or tail sn is invalid.

    kErrorcodeButt
};

#define TsUsU64ToU16Ms(ts_us) ((u16)(((ts_us) / 1000) & 0x000000000000FFFF))
#define TsUsU32ToU16Ms(ts_us) ((u16)(((ts_us) / 1000) & 0x0000FFFF))

#define CalcDeltaTsMs(now_ms, old_ms) (((now_ms) >= (old_ms)) ? ((now_ms) - (old_ms)) : (0xFFFF - (old_ms) + (now_ms)))

#ifdef __cplusplus
extern "C" {
#endif

class SlidWin {
public:
    static void* operator new(size_t n, void *psp_mem);
    static void operator delete(void *psp_mem, void *placement_mem);
    static void operator delete(void *psp_mem);
    static void operator delete(void *psp_mem, size_t n);

    SlidWin(const u32 &current_sn, const u64 &ts_us, const u32 &min_stable_us, const u32 &mode,
            pNetQualityReportCallBack qaulity_func, pReportSnBitMapCallBack bitmap_func, pLogOutCallBack log_out_func,
            pGetCurLogLevel cur_log_level_func, void *context, u8 *cache, const u32 &filter_flag);
    ~SlidWin();

    u32  ResetWin(const u32 &current_sn, const u64 &ts_us);
    u32  SnEntryWin(const u32 &sn, const u64 &ts_us, const u32 &calc_loss_flag = SELF_YES);
    u32  NackSnEntryWin(const NackData *nack, const u64 &ts_us);
    u32  NackSnOffsetEntryWin(const u32 &head_sn, const u32 &tail_sn, const u32 &recv_loss, const u32 &rto_sn,
                              const u16 nack_offset[], const u32 &nack_num, const u64 &ts_us);

    u32  BlockSnEntryWin(const u64 &sn_bit_map, const u32 &begin_sn, const u64 &ts_us, const u32 &calc_loss_flag);
    u32  BlockSnEntryWin(const u32 &sn_bit_map, const u32 &begin_sn, const u64 &ts_us, const u32 &calc_loss_flag);

    u32  SnTsEntryWin(const u32 &sn, const u64 &ts_us);

    u32  ObtainNetworkQuality(f32 *loss, u32 *rtt_us, u32 *jitter_us, u32 *congest_rank, u32 *rto_us, u32 *pps,
                              u32 *discard_dir) const;
    u32  ObtainLossCalcWindow(u32 *loss_num, u32 *total_pack_num) const;
    void SetRttUs(const u32 &rtt_us, const u64 &ts_us, const u32 &report_flag = SELF_YES);
    void SetFeedBackLoss(const u32 &loss, const u64 &ts_us, const u64 &rtt_us = 0);
    void SetFeedBackLossEx(const u32 &loss, const u64 &ts_us, const u64 &rtt_us,
                           const u32 &sample_total_pack_num, const u32 &head_sn);
    u32  AdjustFeedbackFactor(const f32 &k, const u64 &ts_us);
    u32  SetRecvRtoDecFactor(const u32 &k, const u64 &ts_us);
    u32  CheckIsRepeatPacketSn(const u32 &sn, const u64 &ts_us);
    u32  CalcQualityByHandler(const u64 &ts_us, const u32 &must_calc_flag);
    u32  PrintBitMap(u8 *out_str, const u32 &mem_size);

PRIVATE:
    void MoveWin(const u32 &move_step, const u64 &cur_ts_us = 0);
    void CalcRcvLoss(const u64 &ts_us, const u32 &force_calc = SELF_NO);
    void CalcSndLoss(const u64 &ts_us, const u32 &force_calc = SELF_NO);
    void CalcPps(const u64 &ts_us);
    void ForcastCongest(const u64 &ts_us);
    void AutoAdjustRtoK(const f32 &loss);
    i32  IsMoveWin(const u32 &cur_sn_pos, const u64 &ts_us = 0) const;
    u32  UpdateMaxSnPos(const u32 &current_sn_pos, const u32 &sn = 0);
    u32  SnIsValid(const u32 &cur_sn) const;
    u32  GetRtoTmoutPos(const i32 &begin_pos, const i32 &end_pos, const u16 &rto_ts_ms, const u16 &cur_ts_ms) const;
    u32  GetRtoTmoutPos(const i32 &begin_pos, const i32 &end_pos, const i32 &max_sn_pos) const;
    u32  CheckRealExistLoss(const u32 &cur_sn, const u64 &cur_ts);
    u32  CheckNackIsValid(const u32 &head_sn, const u32 &tail_sn, u32 *comb_head_sn, u32 *comb_tail_sn);
    u32  CalcFilterWinReserveSize(void);
    u32  CalcTranWinReserveSize(void);

PRIVATE:
    u64 created_ts_us_;
    u64 current_ts_us_;

    u32 l_border_pos_;
    u32 r_border_pos_;
    u32 l_border_sn_;
    u32 r_border_sn_;

    u64 sn_bit_map_[SLID_WIN_U64_BUF_SZ];
    u16 sn_ts_ms_[WIN_BUF_SIZE];

    u32 max_sn_span_;
    u32 cur_win_size_;

    u32 jiiter_sum_;
    u8  cache_rtt_ring_flg_;
    u8  cache_pps_ring_flg_;
    u8  slid_win_mode_:4;             // SlidWinMode
    u8  filter_win_flag_:4;           // SELF_YES: filter window, SELF_NO: transprot window.r
    u8  recv_out_r_counter_;

    u32 no_calc_loss_sn_num_;
    u32 old_rtt_us_;

    i32 jitter_cache_buf_[MAX_RTT_CACHE_SZ];
    f32 rtt_dt_buf_[MAX_RTT_CACHE_SZ];

    u64 last_cache_rtt_ts_us_;

    u32 last_calc_pps_sn_num_;
    u32 current_sn_num_;

    u16 cur_cache_pps_pos_;
    u16 cur_rtt_cache_pos_;
    u32 pps_sum_;

    u32 pps_cache_buf_[MAX_PPS_CACHE_SZ];

    u64 last_calc_loss_ts_us_;
    u64 calc_pps_ts_us_;

    u64 lst_rpt_quality_ts_us_;
    u64 lst_fdbk_loss_ts_us_;

    pNetQualityReportCallBack RptNetworkQualityFunc_;
    pReportSnBitMapCallBack   RptRecvedSnBitMapFunc_;
    pLogOutCallBack           LogOutFunc_;
    pGetCurLogLevel           CurLogLevelFunc_;

    u32 fdbk_loss_;  // feedback loss coming from receiver.
    u32 loss_;       // expanded the 128 multiple.
    u32 report_loss_;
    u32 feedback_first_head_sn_;
    u32 feedback_last_head_sn_;
    u32 feedback_loss_total_pack_num_;
    u32 cur_rtt_us_;

    u32 jitter_us_;
    u32 avg_pps_;
    u32 min_calc_loss_sn_num_;
    u32 min_calc_loss_span_us_;

    u32 rto_ts_us_;
    u32 feedback_loss_span_us_;
    u32 disconn_filter_;
    f32 avg_rtt_dt_;  // the average differential value for rtt during period.

    f32 rtt_dt_sum_;
    f32 feedback_factor_;
    u32 recv_rto_dec_k_;
    u32 max_sn_pos_;

    u32 max_sn_;
    u32 min_stable_us_;

    u32 reported_quality_;
    u32 quality_move_count_;
    u16 bit_map_update_;
    u16 force_calc_num_;

    u64 last_recv_ts_us;
    void *context_hdl_;

    u8 *cache_;

    u8  loss_direction_;  // DiscardDirectU32
    u8  congest_rank_;    // PreCongestRankU32
    u8  learn_rto_sn_std_;
    u8  back_rto_sn_num_;
    u8  feedback_head_valid_;
    u8  feedback_loss_trusted_;
    u8  rsv_quality_flag_[2];
    i32 jitter_arith_sum_;
    i32 last_jitter_;
    f32 ratio_cach_;

    u16 loss_num_;
    u16 rto_sn_num_;
    u16 disorder_counter_;
    u16 order_counter_;

    u16 disorder_threshold_;
    u16 max_rcv_sn_pos_;
    u32 expect_next_recv_sn_;

    u64 calc_loss_num_ts_us_;
    u32 loss_num_in_10s_;
    u32 loss_total_pack_num_;
    u32 last_ts_us_;
};

#ifdef _UTTEST
SlidWin& HandlerToObject(const slid_win_hdl &win_hdl);
#endif

#ifdef __cplusplus
}
#endif

#endif
