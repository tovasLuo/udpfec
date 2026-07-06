#include "slidwin_self.h"
#include "slidwin.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>

#include <unordered_map>
#include <string>

#if (__linux__ || __APPLE__)
#include <sys/time.h>
#endif

#if (_WIN32 || _WIN64)
#include <time.h>
#include <process.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")
#endif

typedef enum _WinLogLevelEnum {
    kWinLogLevelEmerg   = 0x00,
    kWinLogLevelAlert   = 0x01,
    kWinLogLevelCrit    = 0x02,
    kWinLogLevelError   = 0x03,
    kWinLogLevelWarning = 0x04,
    kWinLogLevelNotice  = 0x05,
    kWinLogLevelInfo    = 0x06,
    kWinLogLevelDebug   = 0x07,

    kWinLogLevelButt
}WinLogLevelEnum;

static inline u16 ReadNackOffsetUnaligned(const u16 nack_offset[], const u32 &pos) {
    u16 offset = 0;
    memcpy(&offset, ((const u8*)nack_offset) + (pos * sizeof(u16)), sizeof(offset));
    return offset;
}

static inline u8* SlidWinAlignCache(u8 *ptr) {
    const size_t align_size = sizeof(void*);
    const size_t addr       = (size_t)ptr;
    return (u8*)((addr + align_size - 1) & (~(align_size - 1)));
}

static inline u32 SlidWinCtz64(const u64 &value) {
#if defined(__GNUC__) || defined(__clang__)
    return (u32)__builtin_ctzll(value);
#else
    u32 count = 0;
    u64 tmp = value;
    while (0 == (tmp & 1)) {
        tmp >>= 1;
        count += 1;
    }
    return count;
#endif
}

static inline void FillNackOffsets(u16 *dst, const u32 &dst_pos, const u16 &start_offset, const u32 &count) {
    for (u32 idx = 0; count > idx; ++idx) {
        dst[dst_pos + idx] = (u16)(start_offset + idx);
    }
}

static inline void BuildAckSnBitmap(const u64 src_bitmap[], const u32 &start_pos, const u32 &bit_count, u8 *dst) {
    const u32 dst_u64_num = (bit_count + 63) >> 6;
    const u32 block_mask  = SLID_WIN_U64_BUF_SZ - 1;

    for (u32 idx = 0; dst_u64_num > idx; ++idx) {
        const u32 src_pos   = (start_pos + (idx << 6)) & WIN_POS_MASK;
        const u32 src_block = src_pos >> 6;
        const u32 src_shift = src_pos & 0x0000003F;
        u64 value = src_bitmap[src_block] >> src_shift;

        if (0 != src_shift) {
            value |= (src_bitmap[(src_block + 1) & block_mask] << (64 - src_shift));
        }

        const u32 valid_bits = bit_count - (idx << 6);
        if (64 > valid_bits) {
            value &= ((((u64)1) << valid_bits) - 1);
        }

        memcpy(dst + (idx << 3), &value, sizeof(value));
    }
}

#define ClearMemory(u16_mem_header, u16_mem_size, begin_pos, end_pos, last_pos_clear_flag) {\
    u32 move_pos = begin_pos;\
    do {\
        *(u16_mem_header + move_pos) = 0;\
        move_pos += 1;\
        if (u16_mem_size <= move_pos) {\
            move_pos = 0;\
        }\
    } while(move_pos != end_pos);\
    if (SELF_YES == last_pos_clear_flag) {\
        *(u16_mem_header + move_pos) = 0;\
    }\
}

#define AddJitter(jitter_sum, cur_jitter) {\
    if (0 > (cur_jitter)) {\
        (jitter_sum) += ((u32)(0 - (cur_jitter)));\
    } else {\
        (jitter_sum) += ((u32)(cur_jitter));\
    }\
}

#define DecJitter(jitter_sum, cur_jitter) {\
    if (0 > (cur_jitter)) {\
        (jitter_sum) -= ((u32)(0 - (cur_jitter)));\
    } else {\
        (jitter_sum) -= ((u32)(cur_jitter));\
    }\
}

#define SetCongestRank(last_jitter, jitter_threshold, congest, rank, create_us, cur_us, min_stable_us) {\
    if  (((jitter_threshold) <= (last_jitter)) && ((min_stable_us) <= ((u32)((cur_us) - (create_us))))) {\
        (congest) = (rank);\
    }\
}

#define ClrCongestRank(congest, rank) {\
     (congest) = (rank);\
}

#define SnPosToSpan(l_border_pos, cur_sn_pos) (((l_border_pos) <= (cur_sn_pos)) ? ((cur_sn_pos) - (l_border_pos))\
                                             : ((WIN_BUF_SIZE - (l_border_pos)) + (cur_sn_pos)))

#ifdef _UTTEST
#include <iostream>
#endif

using namespace std;

typedef struct _WinContext {
    explicit _WinContext(const u64 &current_ts_us): crt_ts_us_(current_ts_us) {
        memset(last_error_, 0x00, sizeof(last_error_));
    }

    u64 crt_ts_us_;
    u8  last_error_[MAX_ERR_INFO_SZ];
}WinContext;

#define BASE_YEARS         (1970)
#define ONE_YEAR_SECONDS   (31536000)
#define ONE_DAY_SECONDS    (86400)
#define ONE_HOUR_SECONDS   (3600)
#define ONE_MINUTE_SECONDS (60)

#define NO_TWO_MONTH_SECONDS (2419200)
#define LARG_MONTH_SECONDS   (2678400)
#define SMALL_MONTH_SECONDS  (2592000)

#define MONTH_NUM_PER_YEAR   (12)

typedef struct _SelfDate {
    u32 year_;
    u32 month_;
    u32 day_;
    u32 hour_;
    u32 minute_;
    u32 second_;
    u32 ms_;
}SelfDate;

#ifdef __cplusplus
extern "C" {
#endif

static unordered_map<SlidWin*, WinContext> g_win_hdl_mgr(1024);

static char g_com_error[MAX_ERR_INFO_SZ] = {0};

#if (_WIN32 || _WIN64)
void SlidWinGettimeofday(struct timeval *tp, void *tzp)
{
    time_t     clock;
    struct tm  tm;
    SYSTEMTIME wtm;

    GetLocalTime(&wtm);

    tm.tm_year  = wtm.wYear - 1900;
    tm.tm_mon   = wtm.wMonth - 1;
    tm.tm_mday  = wtm.wDay;
    tm.tm_hour  = wtm.wHour;
    tm.tm_min   = wtm.wMinute;
    tm.tm_sec   = wtm.wSecond;
    tm.tm_isdst = -1;

    clock = mktime(&tm);

    tp->tv_sec  = (long)clock;
    tp->tv_usec = wtm.wMilliseconds * 1000;

    return;
}

#endif

void SlidWinGetSysDateTime(SelfDate *date_time) {
    {
    time_t tms;
    struct tm stdate;

    time(&tms);

    #if (_WIN32 || _WIN64)
    localtime_s(&stdate, &tms);
    #endif

    #if (__linux__ || __APPLE__)
    localtime_r(&tms, &stdate);
    #endif

    date_time->year_   = stdate.tm_year + 1900;
    date_time->month_  = stdate.tm_mon  + 1;
    date_time->day_    = stdate.tm_mday;
    date_time->hour_   = stdate.tm_hour;
    date_time->minute_ = stdate.tm_min;
    date_time->second_ = stdate.tm_sec;
    }

    {
    struct timeval sys;
    #if (__linux__ || __APPLE__)
    (void)gettimeofday(&sys, NULL);
    #endif

    #if (_WIN32 || _WIN64)
    (void)SlidWinGettimeofday(&sys, NULL);
    #endif

    date_time->ms_ = sys.tv_usec / 1000;
    }

    return;
}

u8* WinDelFileNamePath(const u8 *pFileName) {
    i32 nLoop = 0;

    #if (__linux__ || __APPLE__)
    u8 dirSeperater = '/';
    #endif

    #if (_WIN32 || _WIN64)
    u8 dirSeperater = '\\';
    #endif

    for (nLoop = ((i32)(strlen((char *)pFileName)));  0 < nLoop; --nLoop) {
        if (dirSeperater  == pFileName[nLoop]) {
            nLoop += 1;
            break;
        }
    }

    return (u8*)(pFileName + nLoop);
}

#define WinLog(log_level, mode, fmt, ...) {\
    if ((NULL != CurLogLevelFunc_) && (NULL != LogOutFunc_) && (CurLogLevelFunc_() >= log_level)) {\
        const char *lvl_nm[] = {"Emerg", "Alert", "Critical", "Error", "Warning", "Notice", "Info", "Debug"}; \
        SelfDate self_date; \
        SlidWinGetSysDateTime(&self_date);\
        LogOutFunc_(log_level, "[%s]%04u.%02u.%02u_%02u:%02u:%02u:%03u (%s:%d)[SlidWin_%s]:" fmt, lvl_nm[log_level], \
           self_date.year_, self_date.month_, self_date.day_, self_date.hour_, self_date.minute_, self_date.second_,\
           self_date.ms_, (char*)WinDelFileNamePath((const u8*)__FILE__), __LINE__,\
           ((kSendSlidWinMode == (mode)) ? "send":"recv"), ##__VA_ARGS__);\
    }\
}

void* SlidWin::operator new(size_t n, void *psp_mem) {
    if (NULL == psp_mem) {
        throw "don't input slid window memory!";
    }

    return psp_mem;
}

void SlidWin::operator delete(void *psp_mem, void *placement_mem) {
}

void SlidWin::operator delete(void *psp_mem) {
}

void SlidWin::operator delete(void *psp_mem, size_t n) {
}

SlidWin::SlidWin(const u32 &current_sn, const u64 &ts_us, const u32 &min_stable_us, const u32 &mode,
          pNetQualityReportCallBack qaulity_func, pReportSnBitMapCallBack bitmap_func, pLogOutCallBack log_out_func,
          pGetCurLogLevel cur_log_level_func, void *context, u8 *cache, const u32 &filter_flag):
    created_ts_us_(ts_us),
    min_stable_us_(min_stable_us),
    slid_win_mode_((u8)mode),
    filter_win_flag_(filter_flag),
    reported_quality_(SELF_NO),
    RptNetworkQualityFunc_(qaulity_func),
    RptRecvedSnBitMapFunc_(bitmap_func),
    LogOutFunc_(log_out_func),
    CurLogLevelFunc_(cur_log_level_func),
    context_hdl_(context),
    cache_(cache),
    bit_map_update_(SELF_NO),
    force_calc_num_(0) {
    ResetWin(current_sn, ts_us);
    return;
}

SlidWin::~SlidWin() {
    return;
}

u32 SlidWin::ResetWin(const u32 &current_sn, const u64 &ts_us) {
    l_border_pos_          = 0;
    r_border_pos_          = MIN_WIN_SZ;
    l_border_sn_           = current_sn;
    r_border_sn_           = current_sn + MIN_WIN_SZ;
    cur_win_size_          = MIN_WIN_SZ;
    current_ts_us_         = ts_us;
    cur_rtt_cache_pos_     = 0;
    cache_rtt_ring_flg_    = SELF_NO;
    loss_                  = 0;
    feedback_first_head_sn_ = current_sn;
    feedback_last_head_sn_  = current_sn;
    feedback_loss_total_pack_num_ = 0;
    cur_rtt_us_            = 0;
    jitter_us_             = 0;
    jiiter_sum_            = 0;
    recv_out_r_counter_    = 0;
    no_calc_loss_sn_num_   = 0;
    last_calc_loss_ts_us_  = ts_us;
    calc_pps_ts_us_        = ts_us + 1000000;
    last_calc_pps_sn_num_  = 0;
    min_calc_loss_sn_num_  = MIN_CALC_LOSS_SN_NUM;
    min_calc_loss_span_us_ = DEFAULT_CALC_LOSS_SPAN_US;
    lst_fdbk_loss_ts_us_   = 0;
    current_sn_num_        = 0;
    cur_cache_pps_pos_     = 0;
    cache_pps_ring_flg_    = SELF_NO;
    pps_sum_               = 0;
    max_sn_pos_            = 0;
    max_sn_                = current_sn;
    max_sn_span_           = 0;
    avg_pps_               = 0;
    lst_rpt_quality_ts_us_ = ts_us;
    last_recv_ts_us        = ts_us;
    if (kSendSlidWinMode == slid_win_mode_) {
        loss_direction_    = kUpLinkerLoss;
    } else {
        loss_direction_    = kDownLinkerLoss;
    }
    rto_sn_num_            = DEFAULT_DISORDER_NUM;
    back_rto_sn_num_       = DEFAULT_DISORDER_NUM;
    disorder_counter_      = 0;
    order_counter_         = 0;
    max_rcv_sn_pos_        = 0;
    bit_map_update_        = SELF_NO;
    quality_move_count_    = 0;

    if (kSendSlidWinMode == slid_win_mode_) {
        rto_ts_us_         = DEFAULT_RTO_TS_US;
    } else {
        rto_ts_us_         = MAX_DISORDER_BUF_US;
    }

    feedback_factor_       = 1.0;
    feedback_loss_span_us_ = MAX_FEEDBACK_SPAN_US;
    recv_rto_dec_k_        = DEFAULT_RECV_RTO_DEC_K;
    disconn_filter_        = 0;
    rtt_dt_sum_            = 0.0;
    avg_rtt_dt_            = 0.0;
    fdbk_loss_             = 0;
    feedback_head_valid_   = SELF_NO;
    feedback_loss_trusted_ = SELF_NO;
    last_cache_rtt_ts_us_  = ts_us;
    jitter_arith_sum_      = 0;
    congest_rank_          = kUnknownGenCongest;
    last_jitter_           = 0;
    loss_num_              = 0;
    expect_next_recv_sn_   = current_sn;
    calc_loss_num_ts_us_   = ts_us;
    loss_num_in_10s_       = 0;
    loss_total_pack_num_   = 0;
    last_ts_us_            = 0;
    learn_rto_sn_std_      = DEFAULT_DISORDER_NUM;
    disorder_threshold_    = MIN_LEARN_THRESHOLD;

    memset(sn_bit_map_, 0x00, (MIN_WIN_SZ >> 3));  // (MIN_WIN_SZ >> 6) << 3
    // memset(sn_ts_ms_, 0x00, (MIN_WIN_SZ << 3));
    memset(jitter_cache_buf_, 0x00, sizeof(jitter_cache_buf_));
    memset(pps_cache_buf_, 0x00, sizeof(pps_cache_buf_));

    rtt_dt_buf_[0] = 0.0;

    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p] cur_ts=%lu last_calc_ts=%lu create_ts=%lu\r\n", this,
           ts_us, last_calc_loss_ts_us_, created_ts_us_);
    #endif

    return SELF_SUCESS;
}

u32 SlidWin::BlockSnEntryWin(const u64 &sn_bit_map, const u32 &begin_sn, const u64 &ts_us, const u32 &calc_loss_flag) {
    if (kSendSlidWinMode != slid_win_mode_) {
        return SELF_SUCESS;
    }

    u32 cur_sn_pos  = 0;
    u32 cur_sn_span = 0;

    if (l_border_sn_ <= begin_sn) {
        cur_sn_span = begin_sn - l_border_sn_;
    } else {
        cur_sn_span = (0xFFFFFFFF - l_border_sn_) + begin_sn + 1;
    }

    if (max_sn_span_ < cur_sn_span) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]ack invalid, l_pos=%u l_sn=%u r_pos=%u r_sn=%u "\
               "max_sn=%u max_pos=%u ack: begin_sn=%u bit_map=0x%016lx.\r\n", this, l_border_pos_, l_border_sn_,
               r_border_pos_, r_border_sn_, max_sn_, max_sn_pos_, begin_sn, sn_bit_map);
        #endif
        return (u32)(SlidWinErrorCode::kSnIsInvalid);
    }

    cur_sn_pos      = (l_border_pos_ + cur_sn_span) & WIN_POS_MASK;
    max_rcv_sn_pos_ = (cur_sn_pos + 63) & WIN_POS_MASK;

    force_calc_num_ = 0;

    current_ts_us_  = ts_us;
    last_recv_ts_us = ts_us;

    i32 nrun_result = IsMoveWin(cur_sn_pos);
    if (0 > nrun_result) {
        ResetWin(begin_sn, ts_us);
        cur_sn_pos = 0;
        goto u64_block_sn_full_one_pos_;
    }

    if (0 < nrun_result) {
        MoveWin(nrun_result, ts_us);
    }

u64_block_sn_full_one_pos_:
    if (0xFFFFFFFFFFFFFFFF == sn_bit_map) {
        u32 block_pos = cur_sn_pos >> 6;
        sn_bit_map_[block_pos] = sn_bit_map;
        no_calc_loss_sn_num_  += 64;

        goto u64_block_sn_calc_loss_pos_;
    }

    {
    u64 bit_1_value = 1;
    u32 nloop       = 0;

    while (64 > nloop) {
        if (0 != (sn_bit_map & bit_1_value)) {
            SnEntryWin(begin_sn + nloop, ts_us, calc_loss_flag);
        }
        nloop += 1;
    }
    goto block_64_exit_pos_;
    }

u64_block_sn_calc_loss_pos_:
    if (SELF_NO == calc_loss_flag) {
        goto block_64_exit_pos_;
    }

    CalcSndLoss(ts_us);

    if (1000000 <= (ts_us - lst_rpt_quality_ts_us_)) {
        if ((lst_fdbk_loss_ts_us_ + rto_ts_us_) >= ts_us) {
            RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                                   (f32)(((f32)fdbk_loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kUpLinkerLoss,
                                   (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);
            report_loss_    = fdbk_loss_;
            loss_direction_ = kUpLinkerLoss;
        } else {
            RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                                   (f32)(((f32)loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kDownLinkerLoss,
                                   (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);
            report_loss_    = loss_;
            loss_direction_ = kDownLinkerLoss;
        }

        reported_quality_      = SELF_YES;
        lst_rpt_quality_ts_us_ = ts_us;
        loss_num_              = 0;
    }

block_64_exit_pos_:
    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "block sn: Jitter=%ums\r\n", jitter_us_/1000);
    #endif

    return SELF_SUCESS;
}

u32 SlidWin::BlockSnEntryWin(const u32 &sn_bit_map, const u32 &begin_sn, const u64 &ts_us, const u32 &calc_loss_flag) {
    if (kSendSlidWinMode != slid_win_mode_) {
        return SELF_SUCESS;
    }

    u32 cur_sn_pos  = 0;
    u32 cur_sn_span = 0;

    if (l_border_sn_ <= begin_sn) {
        cur_sn_span = begin_sn - l_border_sn_;
    } else {
        cur_sn_span = (0xFFFFFFFF - l_border_sn_) + begin_sn + 1;
    }

    if (max_sn_span_ < cur_sn_span) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]ack invalid, l_pos=%u l_sn=%u r_pos=%u r_sn=%u "\
               "max_sn=%u max_pos=%u ack: begin_sn=%u bit_map=0x%016x.\r\n", this, l_border_pos_, l_border_sn_,
               r_border_pos_, r_border_sn_, max_sn_, max_sn_pos_, begin_sn, sn_bit_map);
        #endif
        return (u32)(SlidWinErrorCode::kSnIsInvalid);
    }

    cur_sn_pos      = (l_border_pos_ + cur_sn_span) & WIN_POS_MASK;
    max_rcv_sn_pos_ = (cur_sn_pos + 31) & WIN_POS_MASK;

    force_calc_num_ = 0;

    current_ts_us_  = ts_us;
    last_recv_ts_us = ts_us;

    i32 nrun_result = IsMoveWin(cur_sn_pos);
    if (0 > nrun_result) {
        ResetWin(begin_sn, ts_us);
        cur_sn_pos = 0;
        goto u32_block_sn_full_one_pos_;
    }

    if (0 < nrun_result) {
        MoveWin(nrun_result, ts_us);
    }

u32_block_sn_full_one_pos_:
    if (0xFFFFFFFF == sn_bit_map) {
        u32 block_pos = cur_sn_pos >> 6;
        u64 bit_value = 0xFFFFFFFF00000000;  // default high 32 bit of u64.

        if (0 == (cur_sn_pos & 0x0000003F)) {
            // current 32 bit block located in low 32 bit of u64.
            bit_value = 0x00000000FFFFFFFF;
        }

        sn_bit_map_[block_pos] |= bit_value;
        no_calc_loss_sn_num_  += 32;

        goto u32_block_sn_calc_loss_pos_;
    }

    {
    u32 bit_1_value = 1;
    u32 nloop       = 0;

    while (32 > nloop) {
        if (0 != (sn_bit_map & bit_1_value)) {
            SnEntryWin(begin_sn + nloop, ts_us, calc_loss_flag);
        }
        nloop += 1;
    }
    goto block_32_exit_pos_;
    }

u32_block_sn_calc_loss_pos_:
    if (SELF_NO == calc_loss_flag) {
        goto block_32_exit_pos_;
    }

    CalcSndLoss(ts_us);

    if (1000000 <= (ts_us - lst_rpt_quality_ts_us_)) {
        if ((lst_fdbk_loss_ts_us_ + rto_ts_us_) >= ts_us) {
            RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                                   (f32)(((f32)fdbk_loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kUpLinkerLoss,
                                   (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);
            report_loss_    = fdbk_loss_;
            loss_direction_ = kUpLinkerLoss;
        } else {
            RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                                   (f32)(((f32)loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kDownLinkerLoss,
                                   (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);
            report_loss_    = loss_;
            loss_direction_ = kDownLinkerLoss;
        }

        reported_quality_      = SELF_YES;
        lst_rpt_quality_ts_us_ = ts_us;
        loss_num_              = 0;
    }

block_32_exit_pos_:
    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "block sn: Jitter=%ums\r\n", jitter_us_/1000);
    #endif

    return SELF_SUCESS;
}

u32 SlidWin::SnEntryWin(const u32 &sn, const u64 &ts_us, const u32 &calc_loss_flag) {
    u32 cur_sn_pos  = 0;
    i32 nrun_result = 0;

    nrun_result = SnIsValid(sn);
    if (SELF_SUCESS != nrun_result) {
        if (kRecvSlidWinMode == slid_win_mode_) {
            WinLog(kWinLogLevelWarning, slid_win_mode_, "[win_hdl=%p]Invalid sn cur_sn=%u l_sn=%u r_sn=%u l_pos=%u "\
                   "r_pos=%u win_size=%u\r\n", this, sn, l_border_sn_, r_border_sn_, l_border_pos_, r_border_pos_,
                   cur_win_size_);

            if (SELF_SN_OUT_R_ERR == nrun_result) {
                recv_out_r_counter_ += 1;
                if (MAX_RESET_RECV_WIN_THRSHLD <= recv_out_r_counter_) {
                    const u32 reset_sn = sn & RESET_RECV_WIN_SN_MASK;
                    WinLog(kWinLogLevelWarning, slid_win_mode_, "[win_hdl=%p]receiving win async to sending win, "\
                           "now reset receiving win's left border sn to %u\r\n", this, reset_sn);
                    goto sn_entry_reset_win_pos_;
                }
            }
        }

        return (u32)(SlidWinErrorCode::kSnIsInvalid);
    }

    if (0 != recv_out_r_counter_) {
        recv_out_r_counter_ = 0;
    }

    current_ts_us_  = ts_us;
    last_recv_ts_us = ts_us;

    if (l_border_sn_ <= sn) {
        cur_sn_pos = (sn - l_border_sn_) + l_border_pos_;
    } else {
        // the sn has been turned over.
        cur_sn_pos = ((0xFFFFFFFF - l_border_sn_) + sn + 1) + l_border_pos_;
    }
    cur_sn_pos &= WIN_POS_MASK;  // avoid the window turn over.

    no_calc_loss_sn_num_ += 1;

    if (kRecvSlidWinMode == slid_win_mode_) {
        bit_map_update_  = SELF_YES;
        current_sn_num_ += 1;

        CalcPps(ts_us);
    }

    nrun_result = IsMoveWin(cur_sn_pos);
    if (0 > nrun_result) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "now reset window(l_pos=%u r_pos=%u win_size=%u cur_sn_pos=%u)\r\n",
               l_border_pos_, r_border_pos_, cur_win_size_, cur_sn_pos);
        #endif
        goto sn_entry_reset_win_pos_;
    }

    if (0 < nrun_result) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "now move window(l_pos=%u r_pos=%u win_size=%u cur_sn_pos=%u "\
               "move_step=%d)\r\n", l_border_pos_, r_border_pos_, cur_win_size_, cur_sn_pos, nrun_result);
        #endif
        MoveWin(nrun_result, ts_us);
    }

    force_calc_num_ = 0;

    {
    u32 block_pos      = cur_sn_pos >> 6;
    u32 bit_map_offset = cur_sn_pos & 0x0000003F;
    u64 bit_one_value  = 0x0000000000000001;
    u32 force_flag     = SELF_NO;
    
    sn_bit_map_[block_pos] |= (bit_one_value << bit_map_offset);

    if (kRecvSlidWinMode == slid_win_mode_) {
        force_flag = CheckRealExistLoss(sn, ts_us);  // quick ack/nack

        sn_ts_ms_[cur_sn_pos] = TsUsU64ToU16Ms(ts_us);
        UpdateMaxSnPos(cur_sn_pos, sn);
    } else {
        max_rcv_sn_pos_ = cur_sn_pos;
    }

    if (SELF_NO == calc_loss_flag) {
        return SELF_SUCESS;
    }

    if (kRecvSlidWinMode == slid_win_mode_) {
        CalcRcvLoss(ts_us, force_flag);
    } else {
        CalcSndLoss(ts_us, force_flag);
    }

    if (1000000 <= (ts_us - lst_rpt_quality_ts_us_)) {
        if (((kSendSlidWinMode == slid_win_mode_) && ((lst_fdbk_loss_ts_us_ + rto_ts_us_) >= ts_us))
         || ((kRecvSlidWinMode == slid_win_mode_) && ((lst_fdbk_loss_ts_us_ + (cur_rtt_us_ << 1)) >= ts_us))) {
            RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                                   (f32)(((f32)fdbk_loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kUpLinkerLoss,
                                   (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);
            report_loss_    = fdbk_loss_;
            loss_direction_ = kUpLinkerLoss;
        } else {
            RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                                   (f32)(((f32)loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kDownLinkerLoss,
                                   (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);
            report_loss_    = loss_;
            loss_direction_ = kDownLinkerLoss;
        }

        reported_quality_      = SELF_YES;
        lst_rpt_quality_ts_us_ = ts_us;
        loss_num_              = 0;
    }
    }

    return SELF_SUCESS;

sn_entry_reset_win_pos_:
    ResetWin(sn & RESET_RECV_WIN_SN_MASK, ts_us);

    no_calc_loss_sn_num_ += 1;

    // the left border is 0 when reset window.
    sn_bit_map_[0] |= 0x0000000000000001;

    if (kRecvSlidWinMode == slid_win_mode_) {
        sn_ts_ms_[cur_sn_pos] = TsUsU64ToU16Ms(ts_us);
    }

    bit_map_update_ = SELF_YES;

    return RESET_WIN_RETURN;
}

u32 SlidWin::CheckNackIsValid(const u32 &head_sn, const u32 &tail_sn, u32 *comb_head_sn, u32 *comb_tail_sn) {
    *comb_head_sn = head_sn;
    *comb_tail_sn = tail_sn;

    if (l_border_sn_ <= max_sn_) {
        if (l_border_sn_ > tail_sn) {
            return SELF_ERROR;
        }

        if (head_sn <= tail_sn) {
            if (max_sn_ < head_sn) {
                return SELF_ERROR;
            }

            if (l_border_sn_ > head_sn) {
                *comb_head_sn = l_border_sn_;
            }
            if (max_sn_ < tail_sn) {
                *comb_tail_sn = max_sn_;
            }
            return SELF_SUCESS;
        }

        if ((max_sn_ < head_sn) || (l_border_sn_ > head_sn)) {
            *comb_head_sn = l_border_sn_;
        }
        if (tail_sn > max_sn_) {
            *comb_tail_sn = max_sn_;
        }
        return SELF_SUCESS;
    }

    if (head_sn <= tail_sn) {
        if ((max_sn_ < head_sn) && (l_border_sn_ > tail_sn)) {
            return SELF_ERROR;
        }

        if ((max_sn_ < head_sn) && (l_border_sn_ > head_sn)) {
            *comb_head_sn = l_border_sn_;
        }
        if ((max_sn_ < tail_sn) && (l_border_sn_ > tail_sn)) {
            *comb_tail_sn = max_sn_;
        }
        return SELF_SUCESS;
    }

    if (l_border_sn_ > head_sn) {
        *comb_head_sn = l_border_sn_;
    }

    if (max_sn_ < tail_sn) {
        *comb_tail_sn = max_sn_;
    }
    return SELF_SUCESS;
}

u32 SlidWin::CalcFilterWinReserveSize(void) {
    #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
    u32 std_reserve_size[][4] = {
        // RTT < 50ms  RTT < 100ms  RTT < 150ms  RTT > 150ms
        {512,          640,         768,         1024},   // pps < 1000
        {5120,         6144,        7168,        8192},   // pps < 5000
        {6144,         7168,        8192,        9216},   // pps < 10000
        {7168,         8192,        9216,        10240},  // pps < 20000
        {9216,         10240,       11264,       12288},  // pps < 32000
        {10240,        11264,       12288,       14336}   // pps >= 32000
    };
    u32 std_pps[] = {1000, 5000, 10000, 20000, 32000};
    #endif

    #if (2 == APPLICATION_TYPE)
    u32 std_reserve_size[][4] = {
        // RTT < 50ms  RTT < 100ms  RTT < 150ms  RTT > 150ms
        {256,          256,         256,         256},  // pps < 50
        {256,          256,         256,         256},  // pps < 100
        {320,          320,         320,         320},  // pps < 150
        {384,          384,         384,         384},  // pps < 200
        {448,          448,         448,         448},  // pps < 300
        {512,          512,         512,         512}   // pps >= 300
    };
    u32 std_pps[] = {50, 100, 150, 200, 300};
    #endif

    u16 row_id    = 0;
    u16 column_id = 0;

    if (std_pps[0] > avg_pps_) {
        row_id = 0;
        goto calc_filter_rsv_choice_cow_pos_;
    }

    if (std_pps[1] > avg_pps_) {
        row_id = 1;
        goto calc_filter_rsv_choice_cow_pos_;
    }

    if (std_pps[2] > avg_pps_) {
        row_id = 2;
        goto calc_filter_rsv_choice_cow_pos_;
    }

    if (std_pps[3] > avg_pps_) {
        row_id = 3;
        goto calc_filter_rsv_choice_cow_pos_;
    }

    if (std_pps[4] > avg_pps_) {
        row_id = 4;
        goto calc_filter_rsv_choice_cow_pos_;
    }

    row_id = 5;

calc_filter_rsv_choice_cow_pos_:
    if (50000> cur_rtt_us_) {
        column_id = 0;
        goto calc_filter_reserve_exit_pos_;
    }

    if (100000> cur_rtt_us_) {
        column_id = 1;
        goto calc_filter_reserve_exit_pos_;
    }

    if (150000> cur_rtt_us_) {
        column_id = 2;
        goto calc_filter_reserve_exit_pos_;
    }

    column_id = 3;

calc_filter_reserve_exit_pos_:
    return std_reserve_size[row_id][column_id];
}

u32 SlidWin::CalcTranWinReserveSize(void) {
    #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
    u32 std_reserve_size[][4] = {
        // RTT < 50ms  RTT < 100ms  RTT < 150ms  RTT > 150ms
        {512,          640,         768,         1024},   // pps < 500
        {1536,         1792,        2048,        2560},   // pps < 2000
        {5120,         5632,        6144,        7168},   // pps < 5000
        {6144,         7168,        7168,        9216},   // pps < 10000
        {6400,         6400,        7424,        10240},  // pps < 20000
        {6656,         6656,        7680,        11264},  // pps < 30000
        {6912,         6912,        7936,        12288},  // pps < 40000
        {7168,         7168,        8192,        14336}   // pps >= 40000
    };
    u32 std_pps[] = {500, 2000, 5000, 10000, 20000, 30000, 40000};
    #endif

    #if (2 == APPLICATION_TYPE)
    u32 std_reserve_size[][4] = {
        // RTT < 50ms  RTT < 100ms  RTT < 150ms  RTT > 150ms
        {128,          128,         192,         192},  // pps < 50
        {192,          192,         192,         256},  // pps < 100
        {192,          256,         256,         320},  // pps < 150
        {256,          256,         320,         384},  // pps < 200
        {320,          320,         384,         448},  // pps < 300
        {448,          448,         448,         512},  // pps < 400
        {512,          512,         512,         640},  // pps < 500
        {640,          640,         640,         768}   // pps >= 500
    };
    u32 std_pps[] = {50, 100, 150, 200, 300, 400, 500};
    #endif

    u16 row_id    = 0;
    u16 column_id = 0;

    if (std_pps[0] > avg_pps_) {
        row_id = 0;
        goto calc_tran_rsv_choice_cow_pos_;
    }

    if (std_pps[1] > avg_pps_) {
        row_id = 1;
        goto calc_tran_rsv_choice_cow_pos_;
    }

    if (std_pps[2] > avg_pps_) {
        row_id = 2;
        goto calc_tran_rsv_choice_cow_pos_;
    }

    if (std_pps[3] > avg_pps_) {
        row_id = 3;
        goto calc_tran_rsv_choice_cow_pos_;
    }

    if (std_pps[4] > avg_pps_) {
        row_id = 4;
        goto calc_tran_rsv_choice_cow_pos_;
    }

    if (std_pps[5] > avg_pps_) {
        row_id = 5;
        goto calc_tran_rsv_choice_cow_pos_;
    }

    if (std_pps[6] > avg_pps_) {
        row_id = 6;
        goto calc_tran_rsv_choice_cow_pos_;
    }
    row_id = 7;

calc_tran_rsv_choice_cow_pos_:
    if (50000> cur_rtt_us_) {
        column_id = 0;
        goto calc_tran_reserve_exit_pos_;
    }

    if (100000> cur_rtt_us_) {
        column_id = 1;
        goto calc_tran_reserve_exit_pos_;
    }

    if (150000> cur_rtt_us_) {
        column_id = 2;
        goto calc_tran_reserve_exit_pos_;
    }

    column_id = 3;

calc_tran_reserve_exit_pos_:
    return std_reserve_size[row_id][column_id];
}

u32 SlidWin::NackSnOffsetEntryWin(const u32 &head_sn, const u32 &tail_sn, const u32 &recv_loss, const u32 &rto_sn,
                                  const u16 nack_offset[], const u32 &nack_num, const u64 &ts_us) {
    if (kSendSlidWinMode != slid_win_mode_) {
        return SELF_SUCESS;
    }

    u32 head_pos      = 0;
    u32 tail_pos      = 0;
    u32 head_span     = 0;
    u32 tail_span     = 0;
    u32 block_loop    = 0;
    u32 end_block_pos = 0;
    u32 move_pos      = 0;
    u64 bit_value     = 1;
    u32 block_pos     = 0;
    u32 bit_pos       = 0;
    u32 nack_sn       = 0;
    u32 sn_span       = 0;

    u32 comb_head_sn  = 0;
    u32 comb_tail_sn  = 0;

    (void)recv_loss;
    (void)rto_sn;

    u32 nret = CheckNackIsValid(head_sn, tail_sn, &comb_head_sn, &comb_tail_sn);
    if (SELF_SUCESS != nret) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]nack invalid, l_pos=%u l_sn=%u r_pos=%u r_sn=%u "\
               "max_sn=%u max_pos=%u nack: head_sn=%u tail_sn=%u.\r\n", this, l_border_pos_, l_border_sn_,
               r_border_pos_, r_border_sn_, max_sn_, max_sn_pos_, head_sn, tail_sn);
        #endif
        return (u32)(SlidWinErrorCode::kInvalidNackSn);
    }

    if (l_border_sn_ <= comb_head_sn) {
        head_span = comb_head_sn - l_border_sn_;
    } else {
        head_span = (0xFFFFFFFF - l_border_sn_) + comb_head_sn + 1;
    }

    if (l_border_sn_ <= comb_tail_sn) {
        tail_span = comb_tail_sn - l_border_sn_;
    } else {
        tail_span = (0xFFFFFFFF - l_border_sn_) + comb_tail_sn + 1;
    }

    head_pos = (l_border_pos_ + head_span) & WIN_POS_MASK;
    tail_pos = (l_border_pos_ + tail_span) & WIN_POS_MASK;

    max_rcv_sn_pos_ = tail_pos;

    block_loop    = head_pos >> 6;
    end_block_pos = tail_pos >> 6;

    while (end_block_pos != block_loop) {
        sn_bit_map_[block_loop] = 0xFFFFFFFFFFFFFFFF;
        no_calc_loss_sn_num_   += 64;

        block_loop += 1;
        if (SLID_WIN_U64_BUF_SZ <= block_loop) {
            block_loop = 0;
        }
    }

    move_pos = tail_pos & 0x0000003F;
    if (0x0000003F == move_pos) {
        sn_bit_map_[end_block_pos] = 0xFFFFFFFFFFFFFFFF;
        no_calc_loss_sn_num_      += 64;
    } else {
        bit_value = 1;
        bit_pos   = 0;
        move_pos += 1;

        no_calc_loss_sn_num_ += move_pos;

        do {
            sn_bit_map_[end_block_pos] |= bit_value;

            bit_value <<= 1;
            bit_pos    += 1;
        } while (move_pos >= bit_pos);
    }

    move_pos = head_pos & 0x0000003F;
    if (0 != move_pos) {
        block_loop = head_pos >> 6;
        if (0 != block_loop) {
            block_loop -= 1;
        } else {
            block_loop  = SLID_WIN_U64_BUF_SZ - 1;
        }

        bit_value   = 1;
        bit_value <<= move_pos;

        no_calc_loss_sn_num_ += (64 - move_pos);

        do {
            sn_bit_map_[block_loop] |= bit_value;

            bit_value <<= 1;
            move_pos  += 1;
        } while (64 > move_pos);
    }

    if (0 == nack_num) {
        goto nack_exit_pos_;
    }

    block_loop = 0;
    do {
        nack_sn = head_sn + ((u32)ReadNackOffsetUnaligned(nack_offset, block_loop));

        if (l_border_sn_ <= nack_sn) {
            sn_span = nack_sn - l_border_sn_;
        } else {
            sn_span = nack_sn + (0xFFFFFFFF - l_border_sn_) + 1;
        }

        if (max_sn_span_ < sn_span) {
            // invalid nack sn
            goto nack_next_pos_;
        }

        move_pos    = (sn_span + l_border_pos_) & WIN_POS_MASK;
        block_pos   = move_pos >> 6;
        bit_value   = 1;
        bit_value <<= (move_pos & 0x0000003F);

        sn_bit_map_[block_pos] &= (~bit_value);

nack_next_pos_:
        block_loop += 1;
    } while (nack_num > block_loop);

nack_exit_pos_:
    CalcSndLoss(ts_us);

    return SELF_SUCESS;
}

u32 SlidWin::NackSnEntryWin(const NackData *nack, const u64 &ts_us) {
    return NackSnOffsetEntryWin(nack->head_sn_, nack->tail_sn_, nack->recv_loss_, nack->rto_sn_,
                                (const u16*)nack->nack_, (u32)nack->nack_num_, ts_us);
}

u32 SlidWin::SnTsEntryWin(const u32 &sn, const u64 &ts_us) {
    if (kSendSlidWinMode != slid_win_mode_) {
        return SELF_SUCESS;
    }

    u32 cur_sn_pos  = 0;
    i32 nrun_result = 0;

    if (SELF_SUCESS != SnIsValid(sn)) {
        // the span between l_border_sn and current sn must be less than window's size.
        WinLog(kWinLogLevelWarning, slid_win_mode_, "[win_hdl=%p]Invalid sn cur_sn=%u l_sn=%u r_sn=%u l_pos=%u r_pos=%u "\
               "win_size=%u\r\n", this, sn, l_border_sn_, r_border_sn_, l_border_pos_, r_border_pos_, cur_win_size_);
        return (u32)(SlidWinErrorCode::kSnIsInvalid);
    }

    current_ts_us_ = ts_us;

    if (l_border_sn_ <= sn) {
        cur_sn_pos = (sn - l_border_sn_) + l_border_pos_;
    } else {
        // sn has been turned over.
        cur_sn_pos = ((0xFFFFFFFF - l_border_sn_) + sn + 1) + l_border_pos_;
    }
    cur_sn_pos &= WIN_POS_MASK;  // avoid the window overturn.

    if ((last_recv_ts_us + feedback_loss_span_us_) <= ts_us) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "calc loss by SnTsEntryWin() drive.\r\n");
        #endif
        CalcSndLoss(ts_us);
    }

    current_sn_num_ += 1;
    CalcPps(ts_us);

    nrun_result = IsMoveWin(cur_sn_pos);
    if (0 > nrun_result) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "now reset window(l_pos=%u r_pos=%u win_size=%u cur_sn_pos=%u)\r\n",
               l_border_pos_, r_border_pos_, cur_win_size_, cur_sn_pos);
        #endif
        goto sn_ts_entry_reset_win_pos_;
    }

    if (0 < nrun_result) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "now move window(l_pos=%u r_pos=%u win_size=%u cur_sn_pos=%u "\
               "move_step=%d)\r\n", l_border_pos_, r_border_pos_, cur_win_size_, cur_sn_pos, nrun_result);
        #endif
        MoveWin(nrun_result, ts_us);
    }

    bit_map_update_ = SELF_YES;
    force_calc_num_ = 0;
    max_rcv_sn_pos_ = cur_sn_pos;

    sn_ts_ms_[cur_sn_pos] = TsUsU64ToU16Ms(ts_us);

    UpdateMaxSnPos(cur_sn_pos, sn);

    if (1000000 <= (ts_us - lst_rpt_quality_ts_us_)) {
        if ((lst_fdbk_loss_ts_us_ + rto_ts_us_) >= ts_us) {
            RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                                   (f32)(((f32)fdbk_loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kUpLinkerLoss,
                                   (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);
            report_loss_    = fdbk_loss_;
            loss_direction_ = kUpLinkerLoss;
        } else {
            RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                                   (f32)(((f32)loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kDownLinkerLoss,
                                   (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);
            report_loss_    = loss_;
            loss_direction_ = kDownLinkerLoss;
        }

        reported_quality_      = SELF_YES;
        lst_rpt_quality_ts_us_ = ts_us;
        loss_num_              = 0;
    }

    return SELF_SUCESS;

sn_ts_entry_reset_win_pos_:
    ResetWin(sn, ts_us);

    // the left border is 0 when reset window.
    sn_ts_ms_[0]    = TsUsU64ToU16Ms(ts_us);
    bit_map_update_ = SELF_YES;

    return RESET_WIN_RETURN;
}

u32 SlidWin::ObtainNetworkQuality(f32 *loss, u32 *rtt_us, u32 *jitter_us, u32 *congest_rank, u32 *rto_us, u32 *pps,
                                  u32 *discard_dir) const {
    *rtt_us       = cur_rtt_us_;
    *jitter_us    = jitter_us_;
    *congest_rank = (u32)congest_rank_;
    *rto_us       = rto_ts_us_;
    *pps          = avg_pps_;
    *discard_dir  = (u32)loss_direction_;
    *loss         = (f32)(((f32)report_loss_) / EXPAND_LOSS_FACTOR_FLOAT);

    return SELF_SUCESS;
}

u32 SlidWin::ObtainLossCalcWindow(u32 *loss_num, u32 *total_pack_num) const {
    if ((NULL == loss_num) || (NULL == total_pack_num)) {
        return (u32)(SlidWinErrorCode::kInputParamIsNull);
    }

    *loss_num       = (u32)loss_num_;
    *total_pack_num = loss_total_pack_num_;

    return SELF_SUCESS;
}

void SlidWin::SetRttUs(const u32 &rtt_us, const u64 &ts_us, const u32 &report_flag) {
    i32 jitter    = 0;
    u32 chg_delta = 0;

    old_rtt_us_ = cur_rtt_us_;
    cur_rtt_us_ = rtt_us;

    if (rtt_us >= old_rtt_us_) {
        jitter = (i32)(rtt_us - old_rtt_us_);
    } else {
        jitter = (i32)(old_rtt_us_ - rtt_us);
        jitter = 0 - jitter;
    }

    if (SELF_YES == cache_rtt_ring_flg_) {
        jitter_arith_sum_ -= jitter_cache_buf_[cur_rtt_cache_pos_];

        DecJitter(jiiter_sum_, jitter_cache_buf_[cur_rtt_cache_pos_]);

        rtt_dt_sum_ -= rtt_dt_buf_[cur_rtt_cache_pos_];

        rtt_dt_buf_[cur_rtt_cache_pos_] = ((f32)jitter) / ((f32)(ts_us - last_cache_rtt_ts_us_));

        rtt_dt_sum_ += rtt_dt_buf_[cur_rtt_cache_pos_];
        avg_rtt_dt_  = rtt_dt_sum_ / (MAX_RTT_CACHE_SZ - 1);

        last_cache_rtt_ts_us_ = ts_us;
    } else {
        if (0 != cur_rtt_cache_pos_) {
            rtt_dt_buf_[cur_rtt_cache_pos_] = ((f32)jitter) / ((f32)(ts_us - last_cache_rtt_ts_us_));

            rtt_dt_sum_ += rtt_dt_buf_[cur_rtt_cache_pos_];
            avg_rtt_dt_  = rtt_dt_sum_ / cur_rtt_cache_pos_;
        }

        last_cache_rtt_ts_us_ = ts_us;
    }

    jitter_cache_buf_[cur_rtt_cache_pos_] = jitter;

    cur_rtt_cache_pos_ += 1;
    cur_rtt_cache_pos_ &= 0x0000000F;
    if ((SELF_YES != cache_rtt_ring_flg_) && (0 == cur_rtt_cache_pos_)) {
        cache_rtt_ring_flg_ = SELF_YES;
    }

    jitter_arith_sum_ += jitter;
    last_jitter_       = jitter;

    AddJitter(jiiter_sum_, jitter);

    u32 backup_rtt    = old_rtt_us_;
    u32 backup_jitter = jitter_us_;

    if (SELF_YES == cache_rtt_ring_flg_) {
        jitter_us_ = (jiiter_sum_ >> 4);
    } else {
        jitter_us_ = jiiter_sum_ / cur_rtt_cache_pos_;
    }

    #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
    u8 rto_sn_std_by_rtt[][7] = {
    // <20ms, <50ms, <100ms, <150ms, <200ms, <250ms, >250ms (rtt)
        {1,     2,     2,      3,      3,      4,      4},   // pps <  500
        {2,     2,     3,      3,      3,      5,      5},   // pps <  2000
        {3,     3,     3,      4,      4,      5,      6},   // pps <  5000
        {3,     4,     4,      5,      5,      6,      7},   // pps <  10000
        {4,     5,     5,      6,      6,      7,      8},   // pps <  20000
        {5,     6,     6,      7,      7,      8,      9},   // pps <  30000
        {6,     7,     7,      8,      8,      9,      10},  // pps <  40000
        {7,     8,     8,      9,      9,      10,     11}   // pps >= 40000
    };
    #endif

    #if (2 == APPLICATION_TYPE)
    u8 rto_sn_std_by_rtt[][7] = {
    // <20ms, <50ms, <100ms, <150ms, <200ms, <250ms, >250ms (rtt)
        {1,     1,     1,      1,      1,      2,      3},   // pps <  50
        {1,     1,     1,      1,      2,      3,      3},   // pps <  100
        {1,     1,     1,      2,      2,      3,      3},   // pps <  150
        {1,     1,     1,      2,      2,      3,      4},   // pps <  200
        {1,     1,     2,      2,      2,      3,      4},   // pps <  300
        {1,     1,     2,      2,      3,      3,      4},   // pps <  400
        {2,     2,     2,      3,      3,      4,      4},   // pps <  500
        {2,     2,     3,      3,      3,      5,      5}    // pps >= 500
    };
    #endif

    u8 *std_pos = NULL;

    // the pps is more greater, ack is more quick, it protects network becoming bad.
    if (500 > avg_pps_) {
        std_pos = &(rto_sn_std_by_rtt[0][0]);
        goto adjust_rto_sn_std_by_rtt_pos_;
    }

    if (2000 > avg_pps_) {
        std_pos = &(rto_sn_std_by_rtt[1][0]);
        goto adjust_rto_sn_std_by_rtt_pos_;
    }

    if (5000 > avg_pps_) {
        std_pos = &(rto_sn_std_by_rtt[2][0]);
        goto adjust_rto_sn_std_by_rtt_pos_;
    }

    if (10000 > avg_pps_) {
        std_pos = &(rto_sn_std_by_rtt[3][0]);
        goto adjust_rto_sn_std_by_rtt_pos_;
    }

    if (20000 > avg_pps_) {
        std_pos = &(rto_sn_std_by_rtt[4][0]);
        goto adjust_rto_sn_std_by_rtt_pos_;
    }

    if (30000 > avg_pps_) {
        std_pos = &(rto_sn_std_by_rtt[5][0]);
        goto adjust_rto_sn_std_by_rtt_pos_;
    }

    if (40000 > avg_pps_) {
        std_pos = &(rto_sn_std_by_rtt[6][0]);
        goto adjust_rto_sn_std_by_rtt_pos_;
    }

    std_pos = &(rto_sn_std_by_rtt[7][0]);

adjust_rto_sn_std_by_rtt_pos_:
    if (20000 > cur_rtt_us_) {
        learn_rto_sn_std_ = *(std_pos + 0);
        goto update_rto_sn_std_end_pos_;
    }

    if (50000 > cur_rtt_us_) {
        learn_rto_sn_std_ = *(std_pos + 1);
        goto update_rto_sn_std_end_pos_;
    }

    if (100000 > cur_rtt_us_) {
        learn_rto_sn_std_ = *(std_pos + 2);
        goto update_rto_sn_std_end_pos_;
    }

    if (150000 > cur_rtt_us_) {
        learn_rto_sn_std_ = *(std_pos + 3);
        goto update_rto_sn_std_end_pos_;
    }

    if (200000 > cur_rtt_us_) {
        learn_rto_sn_std_ = *(std_pos + 4);
        goto update_rto_sn_std_end_pos_;
    }

    if (250000 > cur_rtt_us_) {
        learn_rto_sn_std_ = *(std_pos + 5);
        goto update_rto_sn_std_end_pos_;
    }

    learn_rto_sn_std_ = *(std_pos + 6);

update_rto_sn_std_end_pos_:

    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "calced jitter: Jitter=%ums\r\n", jitter_us_/1000);
    #endif

    if ((15.000001 <= (((f32)jitter * 100.0f) / ((f32)cur_rtt_us_)))
     && (10.000001 <= (((f32)jitter_us_ * 100.0f) / ((f32)cur_rtt_us_)))) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "single_jitter=%d rtt=%u\r\n", jitter, cur_rtt_us_);
        #endif
        SetCongestRank(jitter, 1000, congest_rank_, kMustBeGenCongest, created_ts_us_, ts_us, min_stable_us_);
    } else {
        ForcastCongest(ts_us);
    }

    if (kRecvSlidWinMode == slid_win_mode_) {
        // min calc loss period is 0.5*RTT for receiving mode.
        min_calc_loss_span_us_ = cur_rtt_us_ >> 1;
        min_calc_loss_span_us_ = (u32)(((f32)min_calc_loss_span_us_) * feedback_factor_);

        if (MIN_RCV_CALC_LOSS_SPAN_US > min_calc_loss_span_us_) {
            min_calc_loss_span_us_ = MIN_RCV_CALC_LOSS_SPAN_US;
        } else if (MAX_RCV_CALC_LOSS_SPAN_US < min_calc_loss_span_us_) {
            min_calc_loss_span_us_ = MAX_RCV_CALC_LOSS_SPAN_US;
        }
    } else {
        // min calc loss period is 1.25*RTT for sending mode.
        min_calc_loss_span_us_ = cur_rtt_us_ + (cur_rtt_us_ >> 2);
        min_calc_loss_span_us_ = (u32)(((f32)min_calc_loss_span_us_) * feedback_factor_);

        if (MIN_SND_CALC_LOSS_SPAN_US > min_calc_loss_span_us_) {
            min_calc_loss_span_us_ = MIN_SND_CALC_LOSS_SPAN_US;
        } else if (MAX_SND_CALC_LOSS_SPAN_US < min_calc_loss_span_us_) {
            min_calc_loss_span_us_ = MAX_SND_CALC_LOSS_SPAN_US;
        }
    }

    if (kRecvSlidWinMode == slid_win_mode_) {
        // Low-PPS game streams need a slightly wider disorder window because
        // packet spacing dominates short RTT fractions.
        if (LOW_PPS_DISORDER_THRESHOLD >= avg_pps_) {
            rto_ts_us_ = (cur_rtt_us_ >> 2);
        } else if ((DEFAULT_RECV_RTO_DEC_K != recv_rto_dec_k_) && (0 != recv_rto_dec_k_)) {
            rto_ts_us_ = (u32)(((f32)cur_rtt_us_) / ((f32)recv_rto_dec_k_));
        } else {
            rto_ts_us_ = (cur_rtt_us_ >> 3);
        }

        rto_ts_us_ += jitter_us_;

        if (LOW_PPS_DISORDER_THRESHOLD >= avg_pps_) {
            if (MAX_LOW_PPS_DISORDER_BUF_US < rto_ts_us_) {
                rto_ts_us_ = MAX_LOW_PPS_DISORDER_BUF_US;
                goto set_rtt_continue_pos_;
            }

            if (MIN_LOW_PPS_DISORDER_BUF_US > rto_ts_us_) {
                rto_ts_us_ = MIN_LOW_PPS_DISORDER_BUF_US;
            }
        } else {
            if (MAX_DISORDER_BUF_US < rto_ts_us_) {
                rto_ts_us_ = MAX_DISORDER_BUF_US;
                goto set_rtt_continue_pos_;
            }

            if (MIN_DISORDER_BUF_US > rto_ts_us_) {
                rto_ts_us_ = MIN_DISORDER_BUF_US;
            }
        }

        goto set_rtt_continue_pos_;
    }

    // send mode: RTO = 1.75*RTT.
    rto_ts_us_ = cur_rtt_us_ + jitter_us_;
    rto_ts_us_ = rto_ts_us_ + (rto_ts_us_ >> 1) + (rto_ts_us_ >> 2);
    if (MIN_RTO_TS_US > rto_ts_us_) {
        rto_ts_us_ = MIN_RTO_TS_US;
    }

set_rtt_continue_pos_:
    // (rto_ts_us_ >> 2) to enhance stability.
    feedback_loss_span_us_ = rto_ts_us_ + (rto_ts_us_ >> 1);
    feedback_loss_span_us_ = SlidWinLimit(MIN_FEEDBACK_SPAN_US, MAX_FEEDBACK_SPAN_US, feedback_loss_span_us_);

    if (kSendSlidWinMode == slid_win_mode_) {
        if (kDownLinkerLoss != report_loss_) {
            rto_ts_us_ = feedback_loss_span_us_;
        } else {
            #ifdef _SELFDEBUG
            WinLog(kWinLogLevelWarning, slid_win_mode_, "[win_hdl=%p]send rto can't equal feedback(rto=%uus "\
                   "rpt_loss=%u loss_dir=%s \r\n", this, rto_ts_us_, report_loss_,
                   ((kDownLinkerLoss == loss_direction_) ? "down" : "up"));
            #endif
        }
    }

    if (SELF_NO == report_flag) {
        return;
    }

    if (old_rtt_us_ <= cur_rtt_us_) {
        chg_delta = cur_rtt_us_ - old_rtt_us_;
    } else {
        chg_delta = old_rtt_us_ - cur_rtt_us_;
    }

    if (0 == old_rtt_us_) {
        goto rtt_change_report_pos_;
    }

    backup_rtt <<= 3;
    backup_rtt >>= 7;

    if ((0 != backup_rtt) && (chg_delta >= backup_rtt)) {
rtt_change_report_pos_:
        if (kRecvSlidWinMode == slid_win_mode_) {
            // the changed value >= 6.25%, report the last network quality.
            RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                                   (f32)(((f32)loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kDownLinkerLoss,
                                   (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);
            report_loss_    = loss_;
            loss_direction_ = kDownLinkerLoss;
            loss_num_       = 0;
            goto set_rtt_chg_exit_pos_;
        }

        if ((lst_fdbk_loss_ts_us_ + rto_ts_us_) >= ts_us) {
            // the changed value >= 6.25%, report the last network quality.
            RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                                   (f32)(((f32)fdbk_loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kUpLinkerLoss,
                                   (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);
            report_loss_    = fdbk_loss_;
            loss_direction_ = kUpLinkerLoss;
            loss_num_       = 0;
        } else {
            RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                                   (f32)(((f32)loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kDownLinkerLoss,
                                   (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);
            report_loss_    = loss_;
            loss_direction_ = kDownLinkerLoss;
            loss_num_       = 0;
        }

set_rtt_chg_exit_pos_:
        reported_quality_      = SELF_YES;
        lst_rpt_quality_ts_us_ = ts_us;
        return;
    }

    if (backup_jitter <= jitter_us_) {
        chg_delta = jitter_us_ - backup_jitter;
    } else {
        chg_delta = backup_jitter - jitter_us_;
    }

    backup_jitter <<= 4;
    backup_jitter >>= 7;

    if ((kMaybeGenCongest < congest_rank_) || ((0 != backup_jitter) && (chg_delta >= backup_jitter))) {
        if (kRecvSlidWinMode == slid_win_mode_) {
            // the changed value >= 6.25%, report the last network quality.
            RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                                   (f32)(((f32)loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kDownLinkerLoss,
                                   (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);
            report_loss_    = loss_;
            loss_direction_ = kDownLinkerLoss;
            loss_num_       = 0;
            goto set_rtt_jit_chg_exit_pos_;
        }

        // the changed value >= 6.25%, report the last network quality.
        if ((lst_fdbk_loss_ts_us_ + rto_ts_us_) >= ts_us) {
            RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                                   (f32)(((f32)fdbk_loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kUpLinkerLoss,
                                   (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);
            report_loss_    = fdbk_loss_;
            loss_direction_ = kUpLinkerLoss;
            loss_num_       = 0;
            goto set_rtt_jit_chg_exit_pos_;
        }

        RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                               (f32)(((f32)loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kDownLinkerLoss,
                               (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);
        report_loss_    = loss_;
        loss_direction_ = kDownLinkerLoss;
        loss_num_       = 0;

set_rtt_jit_chg_exit_pos_:
        reported_quality_      = SELF_YES;
        lst_rpt_quality_ts_us_ = ts_us;
        return;
    }

    return;
}

void SlidWin::SetFeedBackLoss(const u32 &loss, const u64 &ts_us, const u64 &rtt_us) {
    SetFeedBackLossEx(loss, ts_us, rtt_us, 0, 0);
}

void SlidWin::SetFeedBackLossEx(const u32 &loss, const u64 &ts_us, const u64 &rtt_us,
                                const u32 &sample_total_pack_num, const u32 &head_sn) {
    u32 delta_loss = 0;

    fdbk_loss_           = loss;
    lst_fdbk_loss_ts_us_ = ts_us;
    feedback_loss_total_pack_num_ = sample_total_pack_num;
    feedback_loss_trusted_ = SELF_NO;

    if (0 == sample_total_pack_num) {
        feedback_loss_trusted_ = SELF_YES;
    } else {
        if (SELF_YES != feedback_head_valid_) {
            feedback_first_head_sn_ = head_sn;
            feedback_head_valid_ = SELF_YES;
        } else if (0 > ((i32)(head_sn - feedback_last_head_sn_))) {
            feedback_first_head_sn_ = head_sn;
        }

        feedback_last_head_sn_ = head_sn;

        if ((32 <= sample_total_pack_num) && (head_sn != feedback_first_head_sn_)) {
            feedback_loss_trusted_ = SELF_YES;
        }
    }

    if (0 != rtt_us) {
        SetRttUs((u32)rtt_us, ts_us, SELF_YES);

        if (ts_us == lst_rpt_quality_ts_us_) {
            return;
        }
    }

    if (SELF_YES != feedback_loss_trusted_) {
        return;
    }

    if (SELF_NO == reported_quality_) {
        goto report_network_quality_pos_;
    }

    if (((12800 > report_loss_) && (12800 <= loss))
     || ((12800 <= report_loss_) && (12800 > loss))
     || ((0 != report_loss_) && (0 == loss))
     || ((0 == report_loss_) && (0 != loss))) {
        goto report_network_quality_pos_;
    }

    if (report_loss_ < loss) {
        delta_loss = loss - report_loss_;
    } else {
        delta_loss = report_loss_ - loss;
    }

    if (EXPNAD_LOSS_PACTOR_INT > delta_loss) {
        return;
    }

report_network_quality_pos_:
    RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_,
                           (f32)(((f32)fdbk_loss_) / EXPAND_LOSS_FACTOR_FLOAT), (u32)kUpLinkerLoss,
                           (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);

    reported_quality_      = SELF_YES;
    lst_rpt_quality_ts_us_ = ts_us;
    report_loss_           = fdbk_loss_;
    loss_direction_        = kUpLinkerLoss;
    loss_num_              = 0;

    return;
}

u32 SlidWin::AdjustFeedbackFactor(const f32 &k, const u64 &ts_us) {
    if (kSendSlidWinMode == slid_win_mode_) {
        return (u32)(SlidWinErrorCode::kNoSupportSetParam);
    }

    if (MAX_FEEDBACK_FACTOR <= k) {
        feedback_factor_ = MAX_FEEDBACK_FACTOR;
        goto fdbc_take_effect_pos_;
    }

    if (MIN_FEEDBACK_FACTOR >= k) {
        feedback_factor_ = (f32)MIN_FEEDBACK_FACTOR;
        goto fdbc_take_effect_pos_;
    }

    feedback_factor_ = k;

fdbc_take_effect_pos_:
    min_calc_loss_sn_num_ = (u32)(((f32)min_calc_loss_sn_num_) * feedback_factor_);
    if (MIN_CALC_LOSS_SN_NUM > min_calc_loss_sn_num_) {
        min_calc_loss_sn_num_ = MIN_CALC_LOSS_SN_NUM;
    }

    min_calc_loss_span_us_ = (u32)(((f32)min_calc_loss_span_us_) * feedback_factor_);

    if (kRecvSlidWinMode == slid_win_mode_) {
        if (MIN_RCV_CALC_LOSS_SPAN_US > min_calc_loss_span_us_) {
            min_calc_loss_span_us_ = MIN_RCV_CALC_LOSS_SPAN_US;
        } else if (MAX_RCV_CALC_LOSS_SPAN_US < min_calc_loss_span_us_) {
            min_calc_loss_span_us_ = MAX_RCV_CALC_LOSS_SPAN_US;
        }
    } else {
        if (MIN_SND_CALC_LOSS_SPAN_US > min_calc_loss_span_us_) {
            min_calc_loss_span_us_ = MIN_SND_CALC_LOSS_SPAN_US;
        } else if (MAX_SND_CALC_LOSS_SPAN_US < min_calc_loss_span_us_) {
            min_calc_loss_span_us_ = MAX_SND_CALC_LOSS_SPAN_US;
        }
    }

    return SELF_SUCESS;
}

u32 SlidWin::SetRecvRtoDecFactor(const u32 &k, const u64 &ts_us) {
    if (kSendSlidWinMode == slid_win_mode_) {
        return (u32)(SlidWinErrorCode::kNoSupportSetParam);
    }

    if (MIN_RECV_RTO_DEC_K > k) {
        recv_rto_dec_k_ = MIN_RECV_RTO_DEC_K;
        goto rto_dec_take_effect_pos_;
    }

    if (MAX_RECV_RTO_DEC_K < k) {
        recv_rto_dec_k_ = MAX_RECV_RTO_DEC_K;
        goto rto_dec_take_effect_pos_;
    }

    recv_rto_dec_k_ = k;

rto_dec_take_effect_pos_:
    // default receive mode: RTO = 0.125*RTT
    if (0 != recv_rto_dec_k_) {
        rto_ts_us_ = (u32)(((f32)cur_rtt_us_) / ((f32)recv_rto_dec_k_));
    } else {
        rto_ts_us_ = (cur_rtt_us_ >> 3);
    }

    rto_ts_us_ += jitter_us_;

    if (MAX_DISORDER_BUF_US < rto_ts_us_) {
        rto_ts_us_ = MAX_DISORDER_BUF_US;
        goto exist_pos_;
    }

    if (MIN_DISORDER_BUF_US > rto_ts_us_) {
        rto_ts_us_ = MIN_DISORDER_BUF_US;
    }

exist_pos_:
    return SELF_SUCESS;
}

u32 SlidWin::UpdateMaxSnPos(const u32 &current_sn_pos, const u32 &sn) {
    u32 cur_span = 0;
    u32 max_span = 0;

    if (l_border_pos_ <= current_sn_pos) {
        cur_span = current_sn_pos - l_border_pos_;
    } else {
        cur_span = (WIN_BUF_SIZE - l_border_pos_) + current_sn_pos;
    }

    if (cur_win_size_ < cur_span) {
        // current_sn_pos is invalid.
        return SELF_ERROR;
    }

    if (l_border_pos_ <= max_sn_pos_) {
        max_span = max_sn_pos_ - l_border_pos_;
    } else {
        max_span = (WIN_BUF_SIZE - l_border_pos_) + max_sn_pos_;
    }

    if (max_span <= cur_span) {
        max_sn_pos_          = current_sn_pos;
        max_sn_              = sn;
        max_sn_span_         = cur_span;
        expect_next_recv_sn_ = max_sn_ + 1;
    }

    return SELF_SUCESS;
}

//@return: 0: no move window, positive integer: move the window step number, negative integer: reset window.
i32 SlidWin::IsMoveWin(const u32 &cur_sn_pos, const u64 &ts_us) const {
    u32 cur_span = 0;
    u32 mv_step  = 0;

    if (l_border_pos_ <= cur_sn_pos) {
        cur_span = cur_sn_pos - l_border_pos_;
    } else {
        cur_span = (WIN_BUF_SIZE - l_border_pos_) + cur_sn_pos;
    }

    if ((cur_win_size_ >> 1) > cur_span) {
        // the current sn pos lie in slid window.
        return SELF_NO_MOVE;
    }

    u32 mid_pos = l_border_pos_ + (cur_win_size_ >> 1);
    mid_pos &= WIN_POS_MASK;

    if (mid_pos <= cur_sn_pos) {
        mv_step = cur_sn_pos - mid_pos;
    } else {
        mv_step = (WIN_BUF_SIZE - mid_pos) + cur_sn_pos;
    }

    mv_step &= 0xFFFFFFC0;
    mv_step += MIN_MOVE_STEP;
    if (MAX_MOVE_STEP < mv_step) {
        return SELF_RESET_WIN;  // need to reset the window.
    }

    return (i32)mv_step;
}

u32 SlidWin::SnIsValid(const u32 &cur_sn) const {
    u32 sn_span = 0;

    if (l_border_sn_ <= cur_sn) {
        sn_span = cur_sn - l_border_sn_;
    } else {
        sn_span = cur_sn + (0xFFFFFFFF - l_border_sn_) + 1;
    }

    if (cur_win_size_ < sn_span) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]Invalid sn cur_sn=%u l_sn=%u r_sn=%u l_pos=%u r_pos=%u "\
               "win_size=%u\r\n", this, cur_sn, l_border_sn_, r_border_sn_, l_border_pos_, r_border_pos_, cur_win_size_);
        #endif

        if (l_border_sn_ <= r_border_sn_) {
            if (l_border_sn_ > cur_sn) {
                 return SELF_SN_OUT_L_ERR;
            }

            if (r_border_sn_ < cur_sn) {
                return SELF_SN_OUT_R_ERR;
            }

            return SELF_ERROR;
        }

        u32 delta_to_r = 0;
        u32 delta_to_l = 0;

        if (r_border_sn_ < cur_sn) {
            delta_to_r = cur_sn - r_border_sn_;
        } else {
            delta_to_r = 0;
        }

        if (l_border_sn_ > cur_sn) {
            delta_to_l = l_border_sn_ - cur_sn;
        }

        if ((0 != delta_to_r) && (delta_to_r < delta_to_l)) {
            return SELF_SN_OUT_R_ERR;
        }

        if ((0 != delta_to_l) && (delta_to_l < delta_to_r)) {
            return SELF_SN_OUT_L_ERR;
        }

        return SELF_ERROR;
    }

    return SELF_SUCESS;
}

void SlidWin::MoveWin(const u32 &move_step, const u64 &cur_ts_us) {
    if (MIN_RSV_WIN_BIT_SZ >= move_step) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "move the window step less than min(%u>%u).\r\n",
               MIN_RSV_WIN_BIT_SZ, move_step);
        #endif
        return;
    }

    u32 reserve_size    = 0;
    u32 valid_move_step = 0;

    if (SELF_YES == filter_win_flag_) {
        reserve_size = CalcFilterWinReserveSize();
        goto calc_real_move_step_pos_;
    }

    reserve_size = CalcTranWinReserveSize();

calc_real_move_step_pos_:
    if (reserve_size >= move_step) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "move the window step less than dynamic std(%u>%u).\r\n",
               reserve_size, move_step);
        #endif
        return;
    }

    valid_move_step  = move_step - reserve_size;
    valid_move_step &= MIN_MOVE_STEP_BIT_MASK;
    if (0 == valid_move_step) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "move the window step is too small(%u %u) after reserving.\r\n",
               valid_move_step, move_step);
        #endif
        return;
    }

    u32 new_r_border_pos = r_border_pos_ + valid_move_step;
    u32 new_l_border_pos = l_border_pos_ + valid_move_step;

    new_r_border_pos &= WIN_POS_MASK;  // ring buffer, the buffer's length is 65536, avoid turn over
    new_l_border_pos &= WIN_POS_MASK;  // ring buffer, the buffer's length is 65536, avoid turn over

    u32 old_r_border_pos = r_border_pos_;
    #ifdef _SELFDEBUG
    u32 old_l_border_pos = l_border_pos_;
    #endif

    r_border_pos_ = new_r_border_pos;
    l_border_pos_ = new_l_border_pos;
    r_border_sn_ += valid_move_step;
    l_border_sn_ += valid_move_step;
    if (0xFFFFFFFF > quality_move_count_) {
        quality_move_count_ += 1;
    }

    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "\r\nmove %u step\r\nmoved windows(l_border_pos=%u l_border_block=%u "\
           "l_border_sn=%u r_border_pos=%u r_border_block=%u r_border_sn=%u win_size=%u)\r\n",
           valid_move_step, l_border_pos_, l_border_pos_ >> 6, l_border_sn_, r_border_pos_, r_border_pos_ >> 6,
           r_border_sn_, cur_win_size_);
    #endif

    // step1: clear sn timestamp memory.
    u32 begin_pos = old_r_border_pos;

    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "move sn time window(l_border_pos=%u-->%u, r_border_pos=%u-->%u).\r\n",
           old_l_border_pos, l_border_pos_, old_r_border_pos, r_border_pos_);
    #endif

    if (new_r_border_pos >= begin_pos) {
        memset(&(sn_ts_ms_[begin_pos]), 0x00, (new_r_border_pos - begin_pos) << 1);
    } else {
        ClearMemory(&(sn_ts_ms_[0]), WIN_BUF_SIZE, begin_pos, new_r_border_pos, SELF_YES);
    }

    // step2: clear send and receive flag bitmap memory.
    new_r_border_pos >>= 6;
    if (SLID_WIN_U64_BUF_SZ <= new_r_border_pos) {
        new_r_border_pos = 0;
    }

    new_l_border_pos >>= 6;
    if (SLID_WIN_U64_BUF_SZ <= new_l_border_pos) {
        new_l_border_pos = 0;
    }

    begin_pos  = (old_r_border_pos >> 6);
    if (SLID_WIN_U64_BUF_SZ <= begin_pos) {
        begin_pos = 0;
    }

    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "move sn bitmap window(l_border_block=%u-->%u, "\
           "r_border_block=%u-->%u).\r\n\n", (old_l_border_pos >> 6), new_l_border_pos,
           (old_r_border_pos >> 6), new_r_border_pos);
    #endif
 
    if (new_r_border_pos >= begin_pos) {
        memset(&(sn_bit_map_[begin_pos]), 0x00, (new_r_border_pos - begin_pos) << 3);
    } else {
        ClearMemory(&(sn_bit_map_[0]), SLID_WIN_U64_BUF_SZ, begin_pos, new_r_border_pos, SELF_YES);
    }

    if (l_border_pos_ <= r_border_pos_) {
        cur_win_size_ = r_border_pos_ - l_border_pos_;
    } else {
        cur_win_size_ = (WIN_BUF_SIZE - l_border_pos_) + r_border_pos_;
    }

    return;
}

u32 SlidWin::GetRtoTmoutPos(const i32 &begin_pos, const i32 &end_pos, const u16 &rto_ts_ms,
                            const u16 &cur_ts_ms) const {
    if (begin_pos >= end_pos) {
        return begin_pos;
    }

    u16 delta_ms = CalcDeltaTsMs(cur_ts_ms, sn_ts_ms_[begin_pos]);
    if (rto_ts_ms >= delta_ms) {
        return ((u32)begin_pos);
    }

    delta_ms = CalcDeltaTsMs(cur_ts_ms, sn_ts_ms_[end_pos]);
    if (rto_ts_ms <= delta_ms) {
        return ((u32)end_pos);
    }

    i32 mid_pos = ((end_pos - begin_pos) >> 1) + begin_pos;

    delta_ms = CalcDeltaTsMs(cur_ts_ms, sn_ts_ms_[mid_pos]);
    if (rto_ts_ms == delta_ms) {
        return ((u32)mid_pos);
    }

    if (rto_ts_ms < delta_ms) {
       return GetRtoTmoutPos(begin_pos + 1, mid_pos - 1, rto_ts_ms, cur_ts_ms);
    }

    return GetRtoTmoutPos(mid_pos + 1, end_pos - 1, rto_ts_ms, cur_ts_ms);
}

u32 SlidWin::GetRtoTmoutPos(const i32 &begin_pos, const i32 &end_pos, const i32 &max_sn_pos) const {
    if (begin_pos >= end_pos) {
        return begin_pos;
    }

    i32 delta_sn = 0;

    if (begin_pos <= max_sn_pos) {
        delta_sn = max_sn_pos - begin_pos;
    } else {
        delta_sn = max_sn_pos + (WIN_POS_MASK - begin_pos);
    }

    if (rto_sn_num_ >= ((u16)delta_sn)) {
        return begin_pos;
    }

    if (end_pos <= max_sn_pos) {
        delta_sn = max_sn_pos - end_pos;
    } else {
        delta_sn = max_sn_pos + (WIN_POS_MASK - end_pos);
    }

    if (rto_sn_num_ <= ((u16)delta_sn)) {
        return end_pos;
    }

    i32 mid_pos = ((end_pos - begin_pos) >> 1) + begin_pos;

    if (mid_pos <= max_sn_pos) {
        delta_sn = max_sn_pos - mid_pos;
    } else {
        delta_sn = max_sn_pos + (WIN_POS_MASK - mid_pos);
    }

    if (rto_sn_num_ == ((u16)delta_sn)) {
        return mid_pos;
    }

    if (rto_sn_num_ < ((u16)delta_sn)) {
        return GetRtoTmoutPos(begin_pos + 1, mid_pos - 1, max_sn_pos);
    }

    return GetRtoTmoutPos(mid_pos + 1, end_pos - 1, max_sn_pos);
}

void SlidWin::CalcSndLoss(const u64 &ts_us, const u32 &force_calc) {
    if (SELF_NO == bit_map_update_) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]cur_ts=%luus create_ts=%luus bitmap_updated=%u\r\n",
               this, ts_us, created_ts_us_, bit_map_update_);
        #endif
        return;
    }

    if (min_calc_loss_sn_num_ > no_calc_loss_sn_num_) {
        u64 delta_us = ts_us - last_calc_loss_ts_us_;

        if (last_calc_loss_ts_us_ > ts_us) {
            delta_us = last_calc_loss_ts_us_ - ts_us;
        }

        if (min_calc_loss_span_us_ > delta_us) {
            if (SELF_NO == force_calc) {
                return;
            }

            delta_us = ts_us - created_ts_us_;
            if (created_ts_us_ > ts_us) {
                delta_us = created_ts_us_ - ts_us;
            }

            if ((min_calc_loss_span_us_ >> 1) > delta_us) {
                return;
            }
        }
    }

    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]cur_ts=%lu last_calc_ts=%lu create_ts=%lu\r\n", this,
           ts_us, last_calc_loss_ts_us_, created_ts_us_);
    #endif

    no_calc_loss_sn_num_  = 0;
    last_calc_loss_ts_us_ = ts_us;

    u64 bit_mask       = 0;
    u32 block_loop     = l_border_pos_ >> 6;
    u32 move_pos       = l_border_pos_;
    u32 loss_pack_num  = 0;
    u32 bit_loop       = 0;
    u32 total_pack_num = 0;
    u32 tmp_max_sn_pos = max_rcv_sn_pos_;
    u32 block_head_pos = l_border_pos_;
    u32 block_tail_pos = block_head_pos + 63;
    u32 max_sn_span    = 0;
    u32 cur_sn_span    = 0;
    u32 min_rto        = cur_rtt_us_ + (cur_rtt_us_ >> 2);
    u32 com_delta      = 0;
    u16 rto_ts_ms      = TsUsU32ToU16Ms(rto_ts_us_);
    u16 ts_ms          = TsUsU64ToU16Ms(ts_us);
    u16 delta_ms       = 0xFFFF;
    u16 break_flag     = SELF_NO;

    f32 report_loss    = 0.0;

    if (MIN_RTO_TS_US > min_rto) {
        min_rto = MIN_RTO_TS_US;
    }

    if (l_border_pos_ <= tmp_max_sn_pos) {
        max_sn_span = tmp_max_sn_pos - l_border_pos_;
    } else {
        max_sn_span = tmp_max_sn_pos + (WIN_BUF_SIZE - l_border_pos_);
    }

    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]Begin to calc loss:\r\nl_pos=%u l_sn=%u r_pos=%u r_sn=%u "\
           "win_size=%u max_sn_pos=%u max_sn=%u max_rcv_sn_pos=%u max_rcv_sn=%u max_rcv_sn_span=%u "\
           "l_block_start=%u\r\n min_calc_sn_span=%u min_calc_tm_span=%ums rto_ts=%ums force_calc=%u\r\n",
           this, l_border_pos_, l_border_sn_, r_border_pos_, r_border_sn_, cur_win_size_, max_sn_pos_, max_sn_,
           max_rcv_sn_pos_, l_border_sn_ + max_sn_span, max_sn_span, block_loop, min_calc_loss_sn_num_,
           min_calc_loss_span_us_ / 1000, rto_ts_us_/1000, force_calc);
    #endif

    break_flag = SELF_NO;

    do {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]bit_map=0x%016lx block_loop=%u move_pos=%u\r\n", this,
               sn_bit_map_[block_loop], block_loop, move_pos);
        #endif

        if ((block_head_pos <= tmp_max_sn_pos) && (tmp_max_sn_pos < block_tail_pos)) {
            // not integrate sn bitmap block.
            goto calc_bit_discard_pos_;
        }

        if (0xFFFFFFFFFFFFFFFF == sn_bit_map_[block_loop]) {
            move_pos += 64;
            move_pos &= WIN_POS_MASK;        // ring buffer, the buffer's length is 65536

            goto next_block_loop_pos_;
        }

        if (0 == sn_bit_map_[block_loop]) {
            goto calc_bit_discard_pos_;
        }

calc_bit_discard_pos_:
        bit_mask = 1;
        bit_loop = 0;

        while ((64 > bit_loop) && (cur_sn_span <= max_sn_span)) {
            if (0 != (sn_bit_map_[block_loop] & bit_mask)) {
                const u64 block_bits = sn_bit_map_[block_loop] >> bit_loop;
                u32 skip_bits = (0 == ~block_bits) ? (64 - bit_loop) : SlidWinCtz64(~block_bits);
                if (skip_bits > (64 - bit_loop)) {
                    skip_bits = 64 - bit_loop;
                }

                if (cur_sn_span + skip_bits > max_sn_span + 1) {
                    skip_bits = max_sn_span + 1 - cur_sn_span;
                }

                move_pos += skip_bits;
                bit_loop += skip_bits;
                if (64 > bit_loop) {
                    bit_mask <<= skip_bits;
                } else {
                    bit_mask = 0;
                }
                move_pos &= WIN_POS_MASK;       // ring buffer, the buffer's length is 65536
                cur_sn_span = SnPosToSpan(l_border_pos_, move_pos);
                continue;
            }

            if (0 == (sn_bit_map_[block_loop] & bit_mask)) {
                delta_ms = CalcDeltaTsMs(ts_ms, sn_ts_ms_[move_pos]);
                if (rto_ts_ms > delta_ms) {
                    // temp avoid loss = 0.0% for disconnect linker by iptables.
                    break_flag = SELF_YES;
                    break;
                }

                loss_pack_num += 1;
                goto calc_bit_discard_next_pos_;
            }

calc_bit_discard_next_pos_:
            move_pos  += 1;
            bit_loop  += 1;
            bit_mask <<= 1;
            move_pos  &= WIN_POS_MASK;       // ring buffer, the buffer's length is 65536

            cur_sn_span = SnPosToSpan(l_border_pos_, move_pos);
        }

        if ((cur_sn_span > max_sn_span) || (SELF_YES == break_flag)) {
            break;
        }

next_block_loop_pos_:
        block_head_pos += 64;
        block_head_pos &= WIN_POS_MASK;  // ring buffer, the buffer's length is 65536
        block_tail_pos  = block_head_pos + 63;
        block_tail_pos &= WIN_POS_MASK;  // ring buffer, the buffer's length is 65536
        block_loop     += 1;
        if (SLID_WIN_U64_BUF_SZ <= block_loop) {
            block_loop = 0;
        }

        cur_sn_span = SnPosToSpan(l_border_pos_, move_pos);
    } while (cur_sn_span <= max_sn_span);

    #ifdef _SELFDEBUG
    delta_ms = CalcDeltaTsMs(ts_ms, sn_ts_ms_[move_pos]);
    WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]move_pos=%u l_border_pos_=%u move_delta_span=%ums\r\n",
           this, move_pos, l_border_pos_, delta_ms);
    #endif

    if (l_border_pos_ <= move_pos) {
        total_pack_num = move_pos - l_border_pos_;
    } else {
        total_pack_num = (WIN_BUF_SIZE - l_border_pos_) + move_pos;
    }

    loss_num_            = (u16)loss_pack_num;
    loss_total_pack_num_ = total_pack_num;

    if ((calc_loss_num_ts_us_ + 10000000) <= ts_us) {
        calc_loss_num_ts_us_ = ts_us;
        loss_num_in_10s_     = 0;
    }

    if (0 == loss_pack_num) {
        loss_ = 0;
    } else {
        loss_ = ((loss_pack_num * 100) << 7) / total_pack_num;  // expand 128 multiple.
        if (12800 < loss_) {
            loss_ = 12800;
        }

        loss_num_in_10s_ += loss_pack_num;
    }

    report_loss = (f32)(((f32)loss_) / EXPAND_LOSS_FACTOR_FLOAT);

    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]loss_pack_num=%u total_pack_num=%u loss=%5.2f%%\r\n", this,
           loss_pack_num, total_pack_num, report_loss);
    #endif

    MoveWin(total_pack_num, ts_us);

    if (SELF_NO == reported_quality_) {
        goto calc_send_loss_report_pos_;
    }

    if (11520 <= loss_) {      // 90.0% <= loss
        disconn_filter_ += 1;  // filter the ack discard to generate 100% loss.
    } else {
        disconn_filter_  = 0;
    }

    if (((12800 > report_loss_) && (12800 <= loss_) && (3 <= disconn_filter_))
     || ((0 != report_loss_) && (0 == loss_))) {
        goto calc_send_adjust_loss_pos_;
    }

    if (report_loss_ <= loss_) {
        com_delta = loss_ - report_loss_;
    } else {
        com_delta = report_loss_ - loss_;
    }

    if (EXPNAD_LOSS_PACTOR_INT > com_delta) {
        return;
    }

calc_send_adjust_loss_pos_:
    report_loss_    = loss_;
    loss_direction_ = kDownLinkerLoss;

    if ((lst_fdbk_loss_ts_us_ + feedback_loss_span_us_) > ts_us) {
        if (fdbk_loss_ <= loss_) {
            com_delta = loss_ - fdbk_loss_;
        } else {
            com_delta = fdbk_loss_ - loss_;
        }

        if (10240 <= com_delta) {  // delta loss >= 80.0%, report loss by sender calcing.
            goto calc_send_loss_adjust_rto_pos_;
        }

        report_loss     = (f32)(((f32)fdbk_loss_) / EXPAND_LOSS_FACTOR_FLOAT);
        report_loss_    = fdbk_loss_;
        loss_direction_ = kUpLinkerLoss;

        goto calc_send_loss_report_pos_;
    }

calc_send_loss_adjust_rto_pos_:
    if (fdbk_loss_ <= loss_) {
        WinLog(kWinLogLevelInfo, slid_win_mode_, "[win_hdl=%p]auto adjust rto(rcv_loss=%u snd_loss=%u cur_rto=%uus"\
               " norecv_loss_delta=%uus) force_flag=%u\r\n", this, fdbk_loss_, loss_, rto_ts_us_,
               (u32)(ts_us - lst_fdbk_loss_ts_us_), force_calc);

        rto_ts_us_ -= ((rto_ts_us_ >> 2) + (rto_ts_us_ >> 3));  // enable quick resent.
        if (rto_ts_us_ < min_rto) {
            rto_ts_us_ = min_rto;  // min_rto=1.25*RTT.
        }
    }

calc_send_loss_report_pos_:
    RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_, report_loss, (u32)loss_direction_,
                           (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);

    reported_quality_      = SELF_YES;
    lst_rpt_quality_ts_us_ = ts_us;
    loss_num_              = 0;

    return;
}

void SlidWin::CalcRcvLoss(const u64 &ts_us, const u32 &force_calc) {
    if (SELF_NO == bit_map_update_) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]cur_ts=%luus create_ts=%luus bitmap_updated=%u "\
               "force_flag=%u\r\n", this, ts_us, created_ts_us_, (u32)bit_map_update_, force_calc);
        #endif
        return;
    }

    if (min_calc_loss_sn_num_ > no_calc_loss_sn_num_) {
        #if (2 == APPLICATION_TYPE)
        if ((0 == loss_) && (SELF_NO == force_calc)) {
            return;
        }
        #endif

        u64 delta_us = ts_us - last_calc_loss_ts_us_;

        if (last_calc_loss_ts_us_ > ts_us) {
            delta_us = last_calc_loss_ts_us_ - ts_us;
        }

        if ((min_calc_loss_span_us_ > delta_us) && (SELF_NO == force_calc)) {
            return;
        }
    }

    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]cur_ts=%lu last_calc_ts=%lu create_ts=%lu\r\n", this,
           ts_us, last_calc_loss_ts_us_, created_ts_us_);
    #endif

    no_calc_loss_sn_num_  = 0;
    last_calc_loss_ts_us_ = ts_us;

    u32 block_loop     = l_border_pos_ >> 6;
    u32 move_pos       = l_border_pos_;
    u32 loss_pack_num  = 0;
    u32 bit_loop       = 0;
    u32 total_pack_num = 0;
    u32 stop_loop_flag = SELF_NO;
    u64 bit_mask       = 0;
    u16 *lss_sn_offset = NULL;
    u32 tmp_max_sn_pos = ((max_sn_pos_ + 1) & WIN_POS_MASK);
    u32 rto_tmout_pos  = l_border_pos_;
    u32 cach_pos       = 0;
    u32 move_win_step  = 0;
    u32 block_1_pos    = l_border_pos_;
    u32 block_head_pos = l_border_pos_;
    u32 block_tail_pos = block_head_pos + 63;
    u16 rto_ts_ms      = TsUsU32ToU16Ms(rto_ts_us_);
    u16 ts_ms          = TsUsU64ToU16Ms(ts_us);
    u16 delta_ms       = 0xFFFF;
    u16 empty_hole_flg = SELF_NO;
    u32 max_sn_span    = 0;
    u32 cur_sn_span    = 0;
    u16 sv_loss_sn_pos = 0;
    u16 delta_pos      = 0;

    f32 report_loss    = 0.0;

    if (SELF_YES != filter_win_flag_) {
        lss_sn_offset = (u16*)(&(cache_[sizeof(NackData)]));
    }

    if (0 == rto_ts_ms) {
        rto_ts_ms = 1;
    }

    if (l_border_pos_ <= tmp_max_sn_pos) {
        max_sn_span = tmp_max_sn_pos - l_border_pos_;
    } else {
        max_sn_span = tmp_max_sn_pos + (WIN_BUF_SIZE - l_border_pos_);
    }

    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]Begin to calc loss:\r\nl_pos=%u l_sn=%u r_pos=%u r_sn=%u "\
           "win_size=%u max_sn_pos=%u max_sn=%u l_block_start=%u max_sn_span=%u\r\nmin_calc_sn_span=%u "\
           "min_calc_tm_span=%uus rto_ts=%ums rto_sn_num=%u bck_rto_sn_num=%u force_calc=%u\r\n", this,
           l_border_pos_, l_border_sn_, r_border_pos_, r_border_sn_, cur_win_size_, max_sn_pos_, max_sn_, block_loop,
           max_sn_span, min_calc_loss_sn_num_, min_calc_loss_span_us_, (u32)rto_ts_ms, (u32)rto_sn_num_,
           (u32)back_rto_sn_num_, force_calc);
    #endif

    do {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]bit_map=0x%016lx block_loop=%u move_pos=%u\r\n", this,
               sn_bit_map_[block_loop], block_loop, move_pos);
        #endif

        if ((block_head_pos <= tmp_max_sn_pos) && (tmp_max_sn_pos < block_tail_pos)) {
            // not integrate sn bitmap block.
            goto calc_bit_discard_pos_;
        }

        if (0xFFFFFFFFFFFFFFFF == sn_bit_map_[block_loop]) {
            if (SELF_NO == empty_hole_flg) {
                block_1_pos += 64;
                block_1_pos &= WIN_POS_MASK;
            }

            move_pos += 64;
            move_pos &= WIN_POS_MASK;

            cach_pos = block_head_pos;

            if (0 == loss_pack_num) {
                delta_ms = CalcDeltaTsMs(ts_ms, sn_ts_ms_[block_tail_pos]);
                rto_tmout_pos = block_tail_pos;

                goto next_block_loop_pos_;
            }

            delta_ms = CalcDeltaTsMs(ts_ms, sn_ts_ms_[cach_pos]);
            if (rto_ts_ms > delta_ms) {
                goto next_block_loop_pos_;
            }

            delta_ms = CalcDeltaTsMs(ts_ms, sn_ts_ms_[block_tail_pos]);
            if (rto_ts_ms <= delta_ms) {
                rto_tmout_pos = block_tail_pos;
                goto next_block_loop_pos_;
            }

            rto_tmout_pos = GetRtoTmoutPos((i32)cach_pos, (i32)block_tail_pos, rto_ts_ms, ts_ms);
            goto next_block_loop_pos_;
        }

        empty_hole_flg = SELF_YES;

        if (0 == sn_bit_map_[block_loop]) {
            if (rto_ts_ms >= delta_ms) {
                break; // rto timer isn't timeout.
            }

            rto_tmout_pos = GetRtoTmoutPos((i32)block_head_pos, (i32)block_tail_pos, (i32)tmp_max_sn_pos);
            if (block_head_pos == rto_tmout_pos) {
                break;
            }

            if (block_tail_pos == rto_tmout_pos) {
                cach_pos       = 64;
            } else {
                stop_loop_flag = SELF_YES;
                cach_pos       = rto_tmout_pos - block_head_pos + 1;
            }

            // fill nack.
            if (l_border_pos_ <= move_pos) {
                delta_pos = move_pos - l_border_pos_;
            } else {
                delta_pos = move_pos + (WIN_POS_MASK - l_border_pos_) + 1;
            }

            if (MAX_SAVE_LOSS_SN_NUM > sv_loss_sn_pos) {
                const u32 save_num = ((u32)(MAX_SAVE_LOSS_SN_NUM - sv_loss_sn_pos) < cach_pos) ?
                                     (u32)(MAX_SAVE_LOSS_SN_NUM - sv_loss_sn_pos) : cach_pos;
                if ((SELF_YES != filter_win_flag_) && (0 != save_num)) {
                    FillNackOffsets(lss_sn_offset, sv_loss_sn_pos, delta_pos, save_num);
                }

                sv_loss_sn_pos = (u16)(sv_loss_sn_pos + save_num);
            }

            loss_pack_num  += cach_pos;
            move_pos       += cach_pos;
            move_pos       &= WIN_POS_MASK;

            if (SELF_YES == stop_loop_flag) {
                break;
            }

            goto next_block_loop_pos_;
        }

calc_bit_discard_pos_:
        if (move_pos <= tmp_max_sn_pos) {
            delta_pos = (u16)(tmp_max_sn_pos - move_pos);
        } else {
            delta_pos = (u16)(tmp_max_sn_pos + (WIN_POS_MASK - move_pos));
        }

        if ((0 != loss_pack_num) && ((rto_ts_ms >= delta_ms) || (rto_sn_num_ >= delta_pos))) {
            break;
        }

        bit_mask = 1;
        bit_loop = 0;

        // while ((64 > bit_loop) && (tmp_max_sn_pos != move_pos)) {
        while ((64 > bit_loop) && (cur_sn_span < max_sn_span)) {
            if (0 != (sn_bit_map_[block_loop] & bit_mask)) {
                const u64 block_bits = sn_bit_map_[block_loop] >> bit_loop;
                u32 skip_bits = (0 == ~block_bits) ? (64 - bit_loop) : SlidWinCtz64(~block_bits);
                if (skip_bits > (64 - bit_loop)) {
                    skip_bits = 64 - bit_loop;
                }

                if (cur_sn_span + skip_bits > max_sn_span) {
                    skip_bits = max_sn_span - cur_sn_span;
                }

                const u32 last_rcv_pos = (move_pos + skip_bits - 1) & WIN_POS_MASK;
                delta_ms = CalcDeltaTsMs(ts_ms, sn_ts_ms_[last_rcv_pos]);
                rto_tmout_pos = last_rcv_pos;  // if not loss packet, rto_sn == max_sn.

                move_pos += skip_bits;
                bit_loop += skip_bits;
                if (64 > bit_loop) {
                    bit_mask <<= skip_bits;
                } else {
                    bit_mask = 0;
                }
                move_pos &= WIN_POS_MASK;

                if (l_border_pos_ <= move_pos) {
                    cur_sn_span = move_pos - l_border_pos_;
                } else {
                    cur_sn_span = move_pos + (WIN_BUF_SIZE - l_border_pos_);
                }
                continue;
            }

            if (0 == (sn_bit_map_[block_loop] & bit_mask)) {
                if (move_pos <= tmp_max_sn_pos) {
                    delta_pos = (u16)(tmp_max_sn_pos - move_pos);
                } else {
                    delta_pos = (u16)(tmp_max_sn_pos + (WIN_POS_MASK - move_pos));
                }

                #ifdef _SELFDEBUG
                WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]move_pos=%u rto_delta_tm=%ums "\
                       "rto_delta_sn=%u\r\n", this, move_pos, (u32)delta_ms, (u32)delta_pos);
                #endif

                if (rto_ts_ms >= delta_ms) {
                    if (rto_sn_num_ >= delta_pos) {
                        stop_loop_flag = SELF_YES;
                        break;
                    }

                    goto bit_loss_continue_pos_;
                }

                if (rto_sn_num_ >= delta_pos) {
                    stop_loop_flag = SELF_YES;
                    break;
                }

bit_loss_continue_pos_:
                rto_tmout_pos = move_pos;

                if (l_border_pos_ <= move_pos) {
                    delta_pos = move_pos - l_border_pos_;
                } else {
                    delta_pos = move_pos + (WIN_POS_MASK - l_border_pos_) + 1;
                }

                if (MAX_SAVE_LOSS_SN_NUM > sv_loss_sn_pos) {
                    if (SELF_YES != filter_win_flag_) {
                        lss_sn_offset[sv_loss_sn_pos] = delta_pos;
                    }
                    sv_loss_sn_pos += 1;
                }

                loss_pack_num += 1;

                goto calc_bit_discard_next_pos_;
            }

            delta_ms = CalcDeltaTsMs(ts_ms, sn_ts_ms_[move_pos]);

            rto_tmout_pos = move_pos;  // if not loss packet, rto_sn == max_sn.

calc_bit_discard_next_pos_:
            move_pos  += 1;
            bit_loop  += 1;
            bit_mask <<= 1;
            move_pos  &= WIN_POS_MASK;

            if (l_border_pos_ <= move_pos) {
                cur_sn_span = move_pos - l_border_pos_;
            } else {
                cur_sn_span = move_pos + (WIN_BUF_SIZE - l_border_pos_);
            }
        }

        if (SELF_YES == stop_loop_flag) {
            break;
        }

next_block_loop_pos_:
        block_head_pos += 64;
        block_head_pos &= WIN_POS_MASK;
        block_tail_pos  = block_head_pos + 63;
        block_tail_pos &= WIN_POS_MASK;  // ring buffer, the buffer's length is 65536
        block_loop     += 1;
        if (SLID_WIN_U64_BUF_SZ <= block_loop) {
            block_loop = 0;
        }
        move_pos       &= WIN_POS_MASK;  // ring buffer, the buffer's length is 65536

        if (l_border_pos_ <= move_pos) {
            cur_sn_span = move_pos - l_border_pos_;
        } else {
            cur_sn_span = move_pos + (WIN_BUF_SIZE - l_border_pos_);
        }
    } while (cur_sn_span < max_sn_span);

    if ((cur_sn_span >= max_sn_span) && (0 == loss_pack_num)) {
        order_counter_   += 1;
        disorder_counter_ = 0;
        goto adjust_rto_sn_num_pos_;
    }

    disorder_counter_ += 1;
    order_counter_     = 0;

adjust_rto_sn_num_pos_:
    if (disorder_threshold_ <= order_counter_) {
        if (((u16)learn_rto_sn_std_) < rto_sn_num_) {
            rto_sn_num_ -= 1;
        }

        order_counter_ = 0;

        goto adjust_max_move_pos_;
    }

    if ((disorder_threshold_ >> 1) <= disorder_counter_) {
        u32 max_sn_block_id = (max_sn_pos_ >> 6);
        u32 pre_block_id    = max_sn_block_id - 1;

        if (0 == pre_block_id) {
            pre_block_id  = SLID_WIN_U64_BUF_SZ - 1;
        }

        if ((pre_block_id == block_1_pos) || (max_sn_block_id == block_1_pos)) {
            if (back_rto_sn_num_ > rto_sn_num_) {
                rto_sn_num_ += 1;
            } else {
                rto_sn_num_ = (u16)back_rto_sn_num_;
            }

            disorder_counter_ = 0;
        } else {
            if (1 < rto_sn_num_) {
                rto_sn_num_ -= 1;
            }
        }
    }

adjust_max_move_pos_:
    if (tmp_max_sn_pos == move_pos) {
        if (0 != move_pos) {
            move_pos -= 1;
        } else {
            move_pos  = WIN_POS_MASK;
        }
    }

    #ifdef _SELFDEBUG
    delta_ms = CalcDeltaTsMs(ts_ms, sn_ts_ms_[move_pos]);
    WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]move_pos=%u rto_pos=%u l_border_pos_=%u "\
           "move_delta_span=%ums calc_span=%ums rto=%ums order_num=%u disorder_num=%u\r\n", this, move_pos,
           rto_tmout_pos, l_border_pos_, delta_ms, min_calc_loss_span_us_/1000, (u32)rto_ts_ms, order_counter_,
           disorder_counter_);
    #endif
 
    total_pack_num = rto_tmout_pos - l_border_pos_ + 1;
    if (rto_tmout_pos < l_border_pos_) {
        total_pack_num = rto_tmout_pos + WIN_BUF_SIZE - l_border_pos_ + 1;
    }

    loss_num_            = (u16)loss_pack_num;
    loss_total_pack_num_ = total_pack_num;

    if ((calc_loss_num_ts_us_ + 10000000) <= ts_us) {
        calc_loss_num_ts_us_ = ts_us;
        loss_num_in_10s_     = 0;
    }

    if (0 == loss_pack_num) {
        loss_ = 0;
    } else {
        loss_ = ((loss_pack_num * 100) << 7) / total_pack_num;  // expand 128 multiple.
        if (12800 < loss_) {
            loss_ = 12800;
        }

        loss_num_in_10s_ += loss_pack_num;
    }

    report_loss = (f32)(((f32)loss_) / EXPAND_LOSS_FACTOR_FLOAT);

    // quick resend.
    if (0.000001 > report_loss) {
        if (1 < recv_rto_dec_k_) {
            recv_rto_dec_k_ -= 1;
            goto calc_rcv_loss_rsp_ack_nack_pos_;
        }

        recv_rto_dec_k_ = 1;
        goto calc_rcv_loss_rsp_ack_nack_pos_;
    }

    if (10.000001 > report_loss) {
        recv_rto_dec_k_ += 2;
        goto calc_rcv_loss_rsp_ack_nack_pos_;
    }

    if (15.000001 > report_loss) {
        recv_rto_dec_k_ += 4;
        goto calc_rcv_loss_rsp_ack_nack_pos_;
    }

    if (20.000001 > report_loss) {
        recv_rto_dec_k_ += 6;
        goto calc_rcv_loss_rsp_ack_nack_pos_;
    }

    if (30.000001 > report_loss) {
        recv_rto_dec_k_ += 8;
        goto calc_rcv_loss_rsp_ack_nack_pos_;
    }

    if (40.000001 > report_loss) {
        recv_rto_dec_k_ += 10;
        goto calc_rcv_loss_rsp_ack_nack_pos_;
    }

    if (50.000001 > report_loss) {
        recv_rto_dec_k_ += 15;
        goto calc_rcv_loss_rsp_ack_nack_pos_;
    }

    recv_rto_dec_k_ += 20;

calc_rcv_loss_rsp_ack_nack_pos_:
    recv_rto_dec_k_ = (MAX_RECV_RTO_DEC_K < recv_rto_dec_k_) ? MAX_RECV_RTO_DEC_K : recv_rto_dec_k_;

    if (SELF_YES == filter_win_flag_) {
        goto calc_rcv_loss_move_win_pos_;
    }

    if ((0 < loss_pack_num) &&
        ((2.000001 > report_loss) ||
        #if (2 == APPLICATION_TYPE)
         ((MAX_FEEDBACK_NACK_SN_NUM << 1) > loss_pack_num)
        #else
         (MAX_FEEDBACK_NACK_SN_NUM > loss_pack_num)
        #endif
        )) {
        NackData *nack_sn_bitmap = (NackData*)cache_;

        nack_sn_bitmap->head_sn_     = l_border_sn_;
        nack_sn_bitmap->tail_sn_     = max_sn_;
        nack_sn_bitmap->recv_loss_   = loss_;
        nack_sn_bitmap->nack_num_    = (u16)loss_pack_num;
        nack_sn_bitmap->cache_ts_us_ = ts_us;

        if (l_border_pos_ <= rto_tmout_pos) {
            nack_sn_bitmap->rto_sn_  = l_border_sn_ + (rto_tmout_pos - l_border_pos_);
        } else {
            nack_sn_bitmap->rto_sn_  = l_border_sn_ + (rto_tmout_pos + (WIN_BUF_SIZE -  l_border_pos_));
        }

        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelInfo, slid_win_mode_, "[win_hdl=%p] responce nack head_sn=%u tail_sn=%u "\
               "rto_sn=%u nack_num=%u loss=%5.2f%%\r\n", this, nack_sn_bitmap->head_sn_,
               nack_sn_bitmap->tail_sn_, nack_sn_bitmap->rto_sn_, (u32)(nack_sn_bitmap->nack_num_),
               report_loss);
        #endif

        RptRecvedSnBitMapFunc_(this, context_hdl_, nack_sn_bitmap, (u32)kNackType);

        goto calc_rcv_loss_move_win_pos_;
    }

    {
    ReceivedSnBitMap *ack_sn_bitmap = (ReceivedSnBitMap*)SlidWinAlignCache(cache_);

    ack_sn_bitmap->sn_bit_map_  = &(cache_[sizeof(ReceivedSnBitMap) + sizeof(u64)]);
    ack_sn_bitmap->head_sn_     = l_border_sn_;
    ack_sn_bitmap->tail_sn_     = max_sn_;
    ack_sn_bitmap->recv_loss_   = loss_;
    ack_sn_bitmap->cache_ts_us_ = ts_us;

    if (l_border_pos_ <= rto_tmout_pos) {
        ack_sn_bitmap->rto_sn_ = l_border_sn_ + (rto_tmout_pos - l_border_pos_);
    } else {
        ack_sn_bitmap->rto_sn_ = l_border_sn_ + (rto_tmout_pos + (WIN_BUF_SIZE -  l_border_pos_));
    }

    {
    u32 ack_span = 0;
    u8* tmp_mem  = ((u8*)ack_sn_bitmap) + sizeof(ReceivedSnBitMap) + sizeof(u64);

    if (l_border_pos_ <= max_sn_pos_) {
        ack_span = max_sn_pos_ - l_border_pos_;
    } else {
        ack_span = max_sn_pos_ + (WIN_BUF_SIZE - l_border_pos_);
    }

    ack_sn_bitmap->mem_size_   = (ack_span >> 3) + 8;
    ack_sn_bitmap->sn_bit_map_ = tmp_mem;
    memset(tmp_mem, 0x00, ack_sn_bitmap->mem_size_);

    BuildAckSnBitmap(sn_bit_map_, l_border_pos_, ack_span + 1, tmp_mem);
    }

    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p] responce ack head_sn=%u tail_sn=%u rto_sn=%u "\
           "loss=%5.2f%% bitmap=0x%llu\r\n", this, ack_sn_bitmap->head_sn_,
           ack_sn_bitmap->tail_sn_, ack_sn_bitmap->rto_sn_, report_loss, *((const u64*)(ack_sn_bitmap->sn_bit_map_)));
    #endif

    RptRecvedSnBitMapFunc_(this, context_hdl_, ack_sn_bitmap, (u32)kAckType);
    }

calc_rcv_loss_move_win_pos_:
    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]rto_tmout_pos=%u block_1_pos=%u l_border_pos=%u "\
           "max_sn_pos=%u l_border_sn=%u r_border_sn=%u max_sn=%u rto_ts_us=%lluus rtt=%lluus jitter=%lluus\r\n", this,
           rto_tmout_pos, block_1_pos, l_border_pos_, max_sn_pos_, l_border_sn_, r_border_sn_, max_sn_, rto_ts_us_,
           cur_rtt_us_, jitter_us_);
    #endif

    if (l_border_pos_ <= block_1_pos) {
        cach_pos = block_1_pos - l_border_pos_ + 1;
    } else {
        cach_pos = block_1_pos + (WIN_BUF_SIZE - l_border_pos_) + 1;
    }

    if (l_border_pos_ <= rto_tmout_pos) {
        move_win_step = rto_tmout_pos - l_border_pos_ + 1;
    } else {
        move_win_step = rto_tmout_pos + (WIN_BUF_SIZE - l_border_pos_) + 1;
    }

    if (move_win_step < cach_pos) {
        move_win_step = cach_pos;
    }

    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]loss_pack_num=%u total_pack_num=%u loss=%0.2f%%\r\n", this,
           loss_pack_num, total_pack_num, report_loss);
    #endif

    MoveWin(move_win_step, ts_us);

    if ((32 > total_pack_num) || (0 == quality_move_count_)) {
        return;
    }

    if (SELF_NO == reported_quality_) {
        goto calc_rcv_loss_report_quality_pos_;
    }

    if (((12800 > report_loss_) && (12800 <= loss_))
     || ((0 != report_loss_) && (0 == loss_))) {
        goto calc_rcv_loss_report_quality_pos_;
    }

    if (report_loss_ <= loss_) {
        cach_pos = loss_ - report_loss_;
    } else {
        cach_pos = report_loss_ - loss_;
    }
    if (EXPNAD_LOSS_PACTOR_INT > cach_pos) {
        return;
    }

calc_rcv_loss_report_quality_pos_:
    RptNetworkQualityFunc_(this, context_hdl_, cur_rtt_us_, jitter_us_, report_loss, (u32)loss_direction_,
                           (u32)congest_rank_, rto_ts_us_, avg_pps_, (u32)loss_num_);

    reported_quality_      = SELF_YES;
    lst_rpt_quality_ts_us_ = ts_us;
    loss_num_              = 0;
    report_loss_           = loss_;
    loss_direction_        = kDownLinkerLoss;

    return;
}

void SlidWin::CalcPps(const u64 &ts_us) {
    if (calc_pps_ts_us_ > ts_us) {
        return;
    }

    if (last_calc_pps_sn_num_ <= current_sn_num_) {
        min_calc_loss_sn_num_ = current_sn_num_ - last_calc_pps_sn_num_;
    } else {
        min_calc_loss_sn_num_ = current_sn_num_ + 0xFFFFFFFF - last_calc_pps_sn_num_ + 1;
    }

    pps_sum_ += min_calc_loss_sn_num_;
    pps_sum_ -= pps_cache_buf_[cur_cache_pps_pos_];

    pps_cache_buf_[cur_cache_pps_pos_] = min_calc_loss_sn_num_;

    cur_cache_pps_pos_ += 1;
    cur_cache_pps_pos_ &= 0x0003;
    if (0 == cur_cache_pps_pos_) {
        cache_pps_ring_flg_ = SELF_YES;
    }

    if (SELF_YES == cache_pps_ring_flg_) {
        min_calc_loss_sn_num_ = pps_sum_ >> 2;
    } else {
        min_calc_loss_sn_num_ = pps_sum_ / cur_cache_pps_pos_;
    }

    avg_pps_ = min_calc_loss_sn_num_;

    disorder_threshold_ = (u16)(avg_pps_ >> 6);
    if (MIN_LEARN_THRESHOLD > disorder_threshold_) {
        disorder_threshold_ = MIN_LEARN_THRESHOLD;
    }

    #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
    u8 std_rto_sn[][7] = {
        ///<20ms, <50ms, <100ms, <150ms, <200ms, <250ms, >250ms (rtt)
        {2,     3,     4,      5,      6,      7,      9},   // pps <  500
        {3,     4,     5,      6,      7,      9,      11},  // pps <  2000
        {4,     5,     6,      8,      10,     12,     14},  // pps <  5000
        {6,     7,     8,      10,     12,     14,     16},  // pps <  10000
        {7,     8,     9,      10,     14,     16,     18},  // pps <  20000
        {8,     9,     12,     14,     16,     18,     20},  // pps <  30000
        {9,     10,    14,     16,     18,     20,     22},  // pps <  40000
        {12,    14,    16,     18,     20,     22,     24}   // pps >= 40000
    };
    #endif

    #if (2 == APPLICATION_TYPE)
    u8 std_rto_sn[][7] = {
    // <20ms, <50ms, <100ms, <150ms, <200ms, <250ms, >250ms (rtt)
        {1,     1,     2,      2,      2,      3,      3},   // pps <  50
        {1,     2,     2,      2,      2,      3,      4},   // pps <  100
        {1,     2,     2,      2,      3,      3,      4},   // pps <  150
        {1,     2,     2,      2,      3,      4,      4},   // pps <  200
        {1,     2,     2,      3,      3,      4,      4},   // pps <  300
        {1,     2,     2,      3,      3,      4,      4},   // pps <  400
        {1,     2,     2,      3,      3,      4,      4},   // pps <  500
        {1,     2,     3,      3,      3,      5,      5}    // pps >= 500
    };
    #endif

    u8 *std_pos = NULL;

    #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
    // the pps is more greater, ack is more quick, it protects network becoming bad.
    if (500 > avg_pps_) {
        min_calc_loss_sn_num_ >>= 3;  // 12.5%
        std_pos = &(std_rto_sn[0][0]);
        goto adjust_rto_sn_by_rtt_pos_;
    }

    if (2000 > avg_pps_) {
        min_calc_loss_sn_num_ >>= 3;  // 12.5%
        std_pos = &(std_rto_sn[1][0]);
        goto adjust_rto_sn_by_rtt_pos_;
    }

    if (5000 > avg_pps_) {
        min_calc_loss_sn_num_ >>= 4;  // 6.25%
        std_pos = &(std_rto_sn[2][0]);
        goto adjust_rto_sn_by_rtt_pos_;
    }

    if (10000 > avg_pps_) {
        min_calc_loss_sn_num_ >>= 4;  // 6.25%
        std_pos = &(std_rto_sn[3][0]);
        goto adjust_rto_sn_by_rtt_pos_;
    }

    if (20000 > avg_pps_) {
        min_calc_loss_sn_num_ >>= 5;  // 3.125%
        std_pos = &(std_rto_sn[4][0]);
        goto adjust_rto_sn_by_rtt_pos_;
    }

    if (30000 > avg_pps_) {
        min_calc_loss_sn_num_ >>= 5;  // 3.125%
        std_pos = &(std_rto_sn[5][0]);
        goto adjust_rto_sn_by_rtt_pos_;
    }

    if (40000 > avg_pps_) {
        min_calc_loss_sn_num_ >>= 6;  // 1.5625%
        std_pos = &(std_rto_sn[6][0]);
        goto adjust_rto_sn_by_rtt_pos_;
    }

    min_calc_loss_sn_num_ >>= 6;  // 1.5625%
    std_pos = &(std_rto_sn[7][0]);
    #endif

    
    #if (2 == APPLICATION_TYPE)
    // the pps is more greater, ack is more quick, it protects network becoming bad.
    if (50 > avg_pps_) {
        min_calc_loss_sn_num_ >>= 5;  // 3.125%
        std_pos = &(std_rto_sn[0][0]);
        goto adjust_rto_sn_by_rtt_pos_;
    }

    if (100 > avg_pps_) {
        min_calc_loss_sn_num_ >>= 5;  // 3.125% 
        std_pos = &(std_rto_sn[1][0]);
        goto adjust_rto_sn_by_rtt_pos_;
    }

    if (150 > avg_pps_) {
        min_calc_loss_sn_num_ >>= 4;  // 6.25%
        std_pos = &(std_rto_sn[2][0]);
        goto adjust_rto_sn_by_rtt_pos_;
    }

    if (200 > avg_pps_) {
        min_calc_loss_sn_num_ >>= 4;  // 6.25%
        std_pos = &(std_rto_sn[3][0]);
        goto adjust_rto_sn_by_rtt_pos_;
    }

    if (300 > avg_pps_) {
        min_calc_loss_sn_num_ >>= 4;  // 6.25%
        std_pos = &(std_rto_sn[4][0]);
        goto adjust_rto_sn_by_rtt_pos_;
    }

    if (400 > avg_pps_) {
        min_calc_loss_sn_num_ >>= 4;  // 6.25%
        std_pos = &(std_rto_sn[5][0]);
        goto adjust_rto_sn_by_rtt_pos_;
    }

    if (500 > avg_pps_) {
        min_calc_loss_sn_num_ >>= 5;  // 3.125%
        std_pos = &(std_rto_sn[6][0]);
        goto adjust_rto_sn_by_rtt_pos_;
    }

    min_calc_loss_sn_num_ >>= 6;  // 1.5625%
    std_pos = &(std_rto_sn[7][0]);
    #endif

adjust_rto_sn_by_rtt_pos_:
    if (20000 > cur_rtt_us_) {
        back_rto_sn_num_ = *(std_pos + 0);
        goto correct_pos_;
    }

    if (50000 > cur_rtt_us_) {
        back_rto_sn_num_ = *(std_pos + 1);
        goto correct_pos_;
    }

    if (100000 > cur_rtt_us_) {
        back_rto_sn_num_ = *(std_pos + 2);
        goto correct_pos_;
    }

    if (150000 > cur_rtt_us_) {
        back_rto_sn_num_ = *(std_pos + 3);
        goto correct_pos_;
    }

    if (200000 > cur_rtt_us_) {
        back_rto_sn_num_ = *(std_pos + 4);
        goto correct_pos_;
    }

    if (250000 > cur_rtt_us_) {
        back_rto_sn_num_ = *(std_pos + 5);
        goto correct_pos_;
    }

    back_rto_sn_num_ = *(std_pos + 6);

correct_pos_:
    min_calc_loss_sn_num_ = (u32)(((f32)min_calc_loss_sn_num_) * feedback_factor_);

    if (MIN_CALC_LOSS_SN_NUM > min_calc_loss_sn_num_) {
        min_calc_loss_sn_num_ = MIN_CALC_LOSS_SN_NUM;
    }

    last_calc_pps_sn_num_ = current_sn_num_;
    calc_pps_ts_us_       = ts_us + 1000000;

    return;
}

u32 SlidWin::CheckIsRepeatPacketSn(const u32 &cur_sn, const u64 &ts_us) {
    if (kRecvSlidWinMode != slid_win_mode_) {
        return ISNT_RECEIVE_WIN;
    }

    u32 max_l_sn_span = 0;
    u32 cur_l_sn_span = 0;

    if (l_border_sn_ <= cur_sn) {
        cur_l_sn_span = cur_sn - l_border_sn_;
    } else {
        cur_l_sn_span = cur_sn + (0xFFFFFFFF - l_border_sn_) + 1;
    }

    // current is out of the left border.
    if (cur_win_size_ <= cur_l_sn_span) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelInfo, slid_win_mode_, "[win_hdl=%p] cur_sn=%u is repeat(l_border_sn=%u r_border_sn=%u "\
               "max_sn=%u win_size=%u).\r\n", this, cur_sn, l_border_sn_, r_border_sn_, max_sn_, cur_win_size_);
        #endif
        return IS_REPEAT_SN;
    }

    if (l_border_sn_ <= max_sn_) {
        max_l_sn_span = max_sn_ - l_border_sn_;
    } else {
        max_l_sn_span = max_sn_ + (0xFFFFFFFF - l_border_sn_) + 1;
    }

    // new max sn, it's usully expect next sn.
    if (max_l_sn_span < cur_l_sn_span) {
        return ISNT_REPEAT_SN;
    }

    u32 cur_sn_pos = 0;

    if (l_border_sn_ <= cur_sn) {
        // the sn hasn't been overturned.
        cur_sn_pos = l_border_pos_ + cur_sn - l_border_sn_;
    } else {
        cur_sn_pos = l_border_pos_ + cur_sn + 0xFFFFFFFF - l_border_sn_ + 1;
    }

    cur_sn_pos &= WIN_POS_MASK;  // avoid the window overturn.

    u32 block_pos      = cur_sn_pos >> 6;
    u32 bit_map_offset = cur_sn_pos & 0x0000003F;
    u64 bit_one_value  = 0x0000000000000001;

    if (0 != (sn_bit_map_[block_pos] & (bit_one_value << bit_map_offset))) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelInfo, slid_win_mode_, "[win_hdl=%p]cur_ts=%luus create_ts=%luus bitmap_updated=%u\r\n",
               this, ts_us, created_ts_us_, bit_map_update_);
        #endif
        return IS_REPEAT_SN;
    }

    return ISNT_REPEAT_SN;
}

u32 SlidWin::CalcQualityByHandler(const u64 &ts_us, const u32 &must_calc_flag) {
    if (SELF_YES == must_calc_flag) {
        goto calc_quality_by_handler_start_pos_;
    }

    if (SELF_NO == bit_map_update_) {
        return SELF_SUCESS;
    }

    if (MAX_FORCE_NUM_BITMAP_UN_CHG <= force_calc_num_) {
        return SELF_SUCESS;
    }

    force_calc_num_ += 1;

    if (min_stable_us_ > ((u32)(ts_us - current_ts_us_))) {
        return SELF_SUCESS;
    }

calc_quality_by_handler_start_pos_:
    CalcPps(ts_us);

    if (kRecvSlidWinMode == slid_win_mode_) {
        CalcRcvLoss(ts_us, SELF_YES);
    } else {
        CalcSndLoss(ts_us, SELF_YES);
    }

    return SELF_SUCESS;
}

void SlidWin::ForcastCongest(const u64 &ts_us) {
    f32 ratio   = (((f32)last_jitter_ * 100.0f)) / ((f32)cur_rtt_us_);
    f32 correct = (((f32)jitter_us_ * 100.0f)) / ((f32)cur_rtt_us_);

    f32 back_ratio  = ratio_cach_;
    f32 delta_ratio = ratio - ratio_cach_;

    f32 delta_threshold_cfg[] = {
        10.000001f,   // 0us < rtt <= 50000us
        7.000001f,    // 50000us < rtt <= 100000us
        5.000001f,    // 100000us < rtt <= 150000us
        3.200001f,    // 150000us < rtt <= 200000us
        1.800001f     // 200000us < rtt
    };

    f32 correct_threshold_cfg[] = {
        3.000001f,   // 0us < rtt <= 50000us
        2.200001f,   // 50000us < rtt <= 100000us
        1.100001f,   // 100000us < rtt <= 150000us
        0.359999f,   // 150000us < rtt <= 200000us
        0.249999f    // 200000us < rtt
    };

    i32 last_jitter_threshold_cfg[] = {
        1000,   // 0us < rtt <= 50000us
        3000,   // 50000us < rtt <= 100000us
        5300,   // 100000us < rtt <= 150000us
        8000,   // 150000us < rtt <= 200000us
        10600   // 200000us < rtt
    };

    f32 delta_threshold       = 0.0;
    f32 correct_threshold     = 0.0;
    i32 last_jitter_threshold = 0;

    if (50000 > cur_rtt_us_) {
        delta_threshold       = delta_threshold_cfg[0];
        correct_threshold     = correct_threshold_cfg[0];
        last_jitter_threshold = last_jitter_threshold_cfg[0];

        goto congest_rank_calc_pos_;
    }

    if (100000 > cur_rtt_us_) {
        delta_threshold   = delta_threshold_cfg[1];
        correct_threshold = correct_threshold_cfg[1];
        last_jitter_threshold = last_jitter_threshold_cfg[1];

        goto congest_rank_calc_pos_;
    }

    if (150000 > cur_rtt_us_) {
        delta_threshold   = delta_threshold_cfg[2];
        correct_threshold = correct_threshold_cfg[2];
        last_jitter_threshold = last_jitter_threshold_cfg[2];

        goto congest_rank_calc_pos_;
    }

    if (200000 > cur_rtt_us_) {
        delta_threshold   = delta_threshold_cfg[3];
        correct_threshold = correct_threshold_cfg[3];
        last_jitter_threshold = last_jitter_threshold_cfg[3];

        goto congest_rank_calc_pos_;
    }

    delta_threshold   = delta_threshold_cfg[4];
    correct_threshold = correct_threshold_cfg[4];
    last_jitter_threshold = last_jitter_threshold_cfg[4];

congest_rank_calc_pos_:
    ratio_cach_ = ratio;

    if ((0.000001 > avg_rtt_dt_) && (0 > last_jitter_)) {
        congest_rank_ = kNoGenCongest;
        goto calc_congest_exit_pos_;
    }

    if (0.04 <= avg_rtt_dt_) {
        SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kMustBeGenCongest,
                       created_ts_us_, ts_us, min_stable_us_);
        goto calc_congest_exit_pos_;

    }

    if (0.010001 <= avg_rtt_dt_) {
        if (15.000001 <= ratio) {
            SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kMustBeGenCongest,
                           created_ts_us_, ts_us, min_stable_us_);
            goto calc_congest_exit_pos_;
        }

        if ((0.000001 > back_ratio) && (0.000001 < ratio)) {
            SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kMustBeGenCongest,
                           created_ts_us_, ts_us, min_stable_us_);
            goto calc_congest_exit_pos_;
        }

        if (4.000001 < delta_ratio) {
            SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kMustBeGenCongest,
                           created_ts_us_, ts_us, min_stable_us_);
            goto calc_congest_exit_pos_;
        }

        if (5.000001 <= ratio) {
            SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kWillBeGenCongest,
                           created_ts_us_, ts_us, min_stable_us_);
            goto calc_congest_exit_pos_;
        }

        if (1.000001 <= ratio) {
            SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kMaybeGenCongest,
                           created_ts_us_, ts_us, min_stable_us_);
            goto calc_congest_exit_pos_;
        }

        congest_rank_ = kNoGenCongest;
        goto calc_congest_exit_pos_;
    }

    if (0.000001 <= avg_rtt_dt_) {
        if (35.000001 <= ratio) {
            if (correct_threshold < correct) {
                SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kMustBeGenCongest,
                               created_ts_us_, ts_us, min_stable_us_);
            } else {
                congest_rank_ = kNoGenCongest;
            }
            goto calc_congest_exit_pos_;
        }

        if ((0.000001 > back_ratio) && (0.000001 < ratio) && (delta_threshold < delta_ratio)) {
            if (correct_threshold < correct) {
                SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kMustBeGenCongest,
                               created_ts_us_, ts_us, min_stable_us_);
            } else {
                congest_rank_ = kNoGenCongest;
            }
            goto calc_congest_exit_pos_;
        }

        if (25.000001 < delta_ratio) {
            if (correct_threshold < correct) {
                SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kMustBeGenCongest,
                               created_ts_us_, ts_us, min_stable_us_);
            } else {
                congest_rank_ = kNoGenCongest;
            }
            goto calc_congest_exit_pos_;
        }

        if (15.000001 <= ratio) {
            if (correct_threshold < correct) {
                SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kWillBeGenCongest,
                               created_ts_us_, ts_us, min_stable_us_);
            } else {
                congest_rank_ = kNoGenCongest;
            }
            goto calc_congest_exit_pos_;
        }

        if (10.000001 <= ratio) {
            if (correct_threshold < correct) {
                SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kMaybeGenCongest,
                               created_ts_us_, ts_us, min_stable_us_);
            } else {
                congest_rank_ = kNoGenCongest;
            }
            goto calc_congest_exit_pos_;
        }

        if (delta_threshold < delta_ratio) {
            if (correct_threshold < correct) {
                SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kMustBeGenCongest,
                               created_ts_us_, ts_us, min_stable_us_);
            } else {
                congest_rank_ = kNoGenCongest;
            }
            goto calc_congest_exit_pos_;
        }

        congest_rank_ = kNoGenCongest;
        goto calc_congest_exit_pos_;
    }

    // negatives
    if (15.000001 >= ratio) {
        congest_rank_ = kNoGenCongest;
        goto calc_congest_exit_pos_;
    }

    if (30.000001 >= ratio) {
        SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kMaybeGenCongest, created_ts_us_,
                       ts_us, min_stable_us_);
        goto calc_congest_exit_pos_;
    }

    if (45.000001 >= ratio) {
        SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kWillBeGenCongest, created_ts_us_,
                       ts_us, min_stable_us_);
        goto calc_congest_exit_pos_;
    }

    if (correct_threshold < correct) {
        SetCongestRank(last_jitter_, last_jitter_threshold, congest_rank_, kMustBeGenCongest, created_ts_us_,
                       ts_us, min_stable_us_);
    } else {
        congest_rank_ = kNoGenCongest;
    }

calc_congest_exit_pos_:
    #ifdef _SELFDEBUG
    WinLog(kWinLogLevelDebug, slid_win_mode_, "lst_jit=%d rtt=%u bak_ratio=%f cur_ratio=%f delta=%f dt=%f "\
           "cong_rank_=%u correct=%f\r\n", last_jitter_, cur_rtt_us_, back_ratio, ratio, delta_ratio, avg_rtt_dt_,
           (u32)congest_rank_, correct);
    #endif
    return;
}

u32 SlidWin::CheckRealExistLoss(const u32 &cur_sn, const u64 &cur_ts) {
    if (0 == last_ts_us_) {
        #ifdef _SELFDEBUG
        WinLog(kWinLogLevelDebug, slid_win_mode_, "[win_hdl=%p]cur_ts=%lu last_calc_ts=%lu create_ts=%lu cur_sn=%u\r\n",
               this, cur_ts, last_calc_loss_ts_us_, created_ts_us_, cur_sn);
        #endif

        last_calc_loss_ts_us_ = cur_ts;
        last_ts_us_ = (u32)(cur_ts & 0x00000000FFFFFFFF);

        if ((created_ts_us_ + 50000) <= cur_ts) {
            created_ts_us_ = cur_ts;  // sender session's receive slid window.
        }

        return SELF_NO;  // first sn.
    }

    last_ts_us_ = (u32)(cur_ts & 0x00000000FFFFFFFF);

    if (expect_next_recv_sn_ == cur_sn) {
        return SELF_NO;
    }

    u32 std_index = 0;
    u32 temp_span = 0;

    if (expect_next_recv_sn_ < cur_sn) {
        if ((1000 > expect_next_recv_sn_) && (0x7FFFFFFF < cur_sn)) {
            temp_span = expect_next_recv_sn_ + (0xFFFFFFFF - cur_sn) + 1;  // expect next sn is turn over.
        } else {
            temp_span = cur_sn - expect_next_recv_sn_;
        }
        goto check_disorder_or_loss_pos_;
    }

    if ((1000 > cur_sn) && (0x7FFFFFFF < expect_next_recv_sn_)) {
        temp_span = cur_sn + (0xFFFFFFFF - expect_next_recv_sn_) + 1;      // current sn is turn over.
        goto check_disorder_or_loss_pos_;
    }
    temp_span = expect_next_recv_sn_ - cur_sn;

check_disorder_or_loss_pos_:
    if (200 > avg_pps_) {
        if (0 != temp_span) {
            return SELF_YES;
        }
        return SELF_NO;
    }

    if (500 > avg_pps_) {
        if (1 < temp_span) {
            return SELF_YES;
        }
        return SELF_NO;
    }

    if (1000 > avg_pps_) {
        if (1 < temp_span) {
            return SELF_YES;
        }
        return SELF_NO;
    }

    u32 disorder_std[][2] = {
        //2000 > pps, 2000 <= pps, 
        {0,           1},  //   10000us >= RTT(10ms>=RTT)
        {1,           3},  //   40000us >= RTT(40ms>=RTT)
        {1,           5},  //   80000us >= RTT(80ms>=RTT)
        {2,           7},  //  200000us >= RTT(200ms>=RTT)
        {3,           9},  //  300000us >= RTT(300ms>=RTT)
        {5,           11}  //  300000us <  RTT(300ms<RTT)
    };

    if (2000 <= avg_pps_) {
        std_index = 1;
    }

    if (10000 >= cur_rtt_us_) {  //  10000us >= RTT
        if (disorder_std[0][std_index] < temp_span) {
            return SELF_YES;
        }
        return SELF_NO;
    }

    if (40000 >= cur_rtt_us_) {  //  40000us >= RTT
        if (disorder_std[1][std_index] < temp_span) {
            return SELF_YES;
        }
        return SELF_NO;
    }

    if (80000 >= cur_rtt_us_) {  //  80000us >= RTT
        if (disorder_std[2][std_index] < temp_span) {
            return SELF_YES;
        }
        return SELF_NO;
    }

    if (200000 >= cur_rtt_us_) {  //  200000us >= RTT
        if (disorder_std[3][std_index] < temp_span) {
            return SELF_YES;
        }
        return SELF_NO;
    }

    if (300000 >= cur_rtt_us_) {  //  300000us >= RTT
        if (disorder_std[4][std_index] < temp_span) {
            return SELF_YES;
        }
        return SELF_NO;
    }

    //  300000us < RTT
    if (disorder_std[5][std_index] < temp_span) {
        return SELF_YES;
    }
    return SELF_NO;
}

u32 SlidWin::PrintBitMap(u8 *out_str, const u32 &mem_size) {
    u32 block_loop = l_border_pos_ >> 6;
    u32 end_block  = max_sn_pos_ >> 6;

    u32 end_span   = 0;
    u32 cur_span   = 0;

    if (block_loop <= end_block) {
        end_span = end_block - block_loop;
    } else {
        end_span = end_block + (SLID_WIN_U64_BUF_SZ - block_loop);
    }

    u8 *wrt_pos = out_str;
    i32 free_sz = (i32)mem_size;
    i32 wrt_num = snprintf((char*)wrt_pos, free_sz, "span_num=%u span_tm=%ums rto_tm=%ums rto_num=%u "
                           "learn=%u lss_in10s=%u a=%.1f b=%u max_recv_pos=%u h_sn|t_sn=%u|%u bitmap=",
                           min_calc_loss_sn_num_, min_calc_loss_span_us_/1000, rto_ts_us_/1000, (u32)rto_sn_num_,
                           (u32)disorder_threshold_, loss_num_in_10s_, feedback_factor_, recv_rto_dec_k_,
                           (u32)max_rcv_sn_pos_, l_border_sn_, max_sn_);

    if (free_sz <= wrt_num) {
        return ((u32)wrt_num);
    }

    free_sz -= wrt_num;
    wrt_pos += wrt_num;

    u32 writed_sz = (u32)wrt_num;

    do {
        wrt_num = snprintf((char*)wrt_pos, free_sz, "0x%016llx ", (unsigned long long)sn_bit_map_[block_loop]);

        if (free_sz <= wrt_num) {
            writed_sz += ((u32)wrt_num);
            break;
        }

        free_sz   -= wrt_num;
        wrt_pos   += ((u32)wrt_num);
        writed_sz += ((u32)wrt_num);

        block_loop += 1;
        if (SLID_WIN_U64_BUF_SZ <= block_loop) {
            block_loop = 0;
        }

        cur_span += 1;
    } while (end_span >= cur_span);

    return writed_sz;
}

/*****************************************************************************************************************
Name     : RepeatPacketFilter
Function : checks current sn is duplicate packet's sn.
In param : const slid_win_hdl &win_hdl
           const u32 &sn
           const u64 &ts_us
Out param: void
Return   : u32  // 0: isn't repeat packet, 1: is repeat packet, others: the win_hdl isn't receiving slid window.

Mdf history  :
1.Date       : 2023.09.19
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 RepeatPacketFilter(const slid_win_hdl &win_hdl, const u32 &sn, const u64 &ts_us) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    g_com_error[0] = '\0';

    return ((SlidWin*)win_hdl)->CheckIsRepeatPacketSn(sn, ts_us);
}

/*****************************************************************************************************************
Name     : SnEntrySlidWin
Function : push a received sn to slid window.
In param : const slid_win_hdl &win_hdl
           const u32 &sn
           const u64 &ts_us
Out param: void
Return   : u32  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 SnEntrySlidWin(const slid_win_hdl &win_hdl, const u32 &sn, const u64 &ts_us) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    g_com_error[0] = '\0';

    return ((SlidWin*)win_hdl)->SnEntryWin(sn, ts_us);
}

/*****************************************************************************************************************
Name     : NackSnEntrySlidWin
Function : push series nack sn to slid window.
In param : const slid_win_hdl &win_hdl
           const NackData *nack
           const u64 &ts_us
Out param: void
Return   : u32  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2023.12.15
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 NackSnEntrySlidWin(const slid_win_hdl &win_hdl, const NackData *nack, const u64 &ts_us) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    g_com_error[0] = '\0';

    return ((SlidWin*)win_hdl)->NackSnEntryWin(nack, ts_us);
}

u32 NackSnOffsetEntrySlidWin(const slid_win_hdl &win_hdl, const u32 &head_sn, const u32 &tail_sn,
                             const u32 &recv_loss, const u32 &rto_sn, const u16 nack_offset[],
                             const u32 &nack_num, const u64 &ts_us) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    g_com_error[0] = '\0';

    return ((SlidWin*)win_hdl)->NackSnOffsetEntryWin(head_sn, tail_sn, recv_loss, rto_sn, nack_offset, nack_num, ts_us);
}


/*****************************************************************************************************************
Name     : BlockU64SnEntrySlidWin
Function : push a block received sn to slid window.
In param : const slid_win_hdl &win_hdl
           const u64 &sn_bit_map
           const u32 &begin_sn
           const u64 &ts_us
           const u32 &calc_loss_flag
Out param: void
Return   : u32  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2023.10.11
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 BlockU64SnEntrySlidWin(const slid_win_hdl &win_hdl, const u64 &sn_bit_map, const u32 &begin_sn, const u64 &ts_us,
                           const u32 &calc_loss_flag) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    g_com_error[0] = '\0';

    return ((SlidWin*)win_hdl)->BlockSnEntryWin(sn_bit_map, begin_sn, ts_us, calc_loss_flag);
}

/*****************************************************************************************************************
Name     : BlockU32SnEntrySlidWins
Function : push a block received sn to slid window.
In param : const slid_win_hdl &win_hdl
           const u32 &sn_bit_map
           const u32 &begin_sn
           const u64 &ts_us
           const u32 &calc_loss_flag
Out param: void
Return   : u32  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2023.10.11
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 BlockU32SnEntrySlidWin(const slid_win_hdl &win_hdl, const u32 &sn_bit_map, const u32 &begin_sn, const u64 &ts_us,
                           const u32 &calc_loss_flag) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    g_com_error[0] = '\0';

    return ((SlidWin*)win_hdl)->BlockSnEntryWin(sn_bit_map, begin_sn, ts_us, calc_loss_flag);
}

/*****************************************************************************************************************
Name     : SnTsEntrySlidWin
Function : push a new sn's timestamp to slid window.
In param : const slid_win_hdl &win_hdl
           const u32 &sn
           const u64 &ts_us
Out param: void
Return   : u32  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 SnTsEntrySlidWin(const slid_win_hdl &win_hdl, const u32 &sn, const u64 &ts_us) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    g_com_error[0] = '\0';

    return ((SlidWin*)win_hdl)->SnTsEntryWin(sn, ts_us);
}

/*****************************************************************************************************************
Name     : CalcQualityByHandler
Function : calc network quality by handler for long time no packet.
In param : const slid_win_hdl &win_hdl
           const u64 &ts_us
           const u32 &must_calc_flag  // 0: whether calc by slid window's status, 1: must calc.
Out param: void
Return   : u32   // 0:SUCESS, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.07.06
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 CalcQualityByHandler(const slid_win_hdl &win_hdl, const u64 &ts_us, const u32 &must_calc_flag) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    g_com_error[0] = '\0';

    return ((SlidWin*)win_hdl)->CalcQualityByHandler(ts_us, must_calc_flag);
}

/*****************************************************************************************************************
Name     : SetLinkerRttUs
Function : setting a linker rrt value, unit: us.
In param : const slid_win_hdl &win_hdl
           const u32 &rtt_us
           const u64 &ts_us
Out param: void
Return   : u32   // 0:SUCESS, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.07.06
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 SetLinkerRttUs(const slid_win_hdl &win_hdl, const u32 &rtt_us, const u64 &ts_us) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    ((SlidWin*)win_hdl)->SetRttUs(rtt_us, ts_us);

    return SELF_SUCESS;
}

/*****************************************************************************************************************
Name     : SetLinkerLoss
Function : setting a linker loss coming from receiver.
In param : const slid_win_hdl &win_hdl
           const u32 &loss
           const u64 &ts_us
           const u32 &rtt_us
Out param: void
Return   : u32   // 0:SUCESS, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.07.08
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 SetLinkerLoss(const slid_win_hdl &win_hdl, const u32 &loss, const u64 &ts_us, const u32 &rtt_us) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    ((SlidWin*)win_hdl)->SetFeedBackLoss(loss, ts_us, rtt_us);

    return SELF_SUCESS;
}

u32 SetLinkerLossEx(const slid_win_hdl &win_hdl, const u32 &loss, const u64 &ts_us, const u32 &rtt_us,
                    const u32 &sample_total_pack_num, const u32 &head_sn) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    ((SlidWin*)win_hdl)->SetFeedBackLossEx(loss, ts_us, rtt_us, sample_total_pack_num, head_sn);

    return SELF_SUCESS;
}

/*****************************************************************************************************************
Name     : AdjustOptimizeLossFactor
Function : when occurs single director loss, calling this function to adjust optimizing factor to decress loss.
In param : const slid_win_hdl &win_hdl
           const float &factor
           const u64 &ts_us
Out param: void
Return   : u32   // 0:SUCESS, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.08.08
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 AdjustOptimizeLossFactor(const slid_win_hdl &win_hdl, const float &factor, const u64 &ts_us) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    return ((SlidWin*)win_hdl)->AdjustFeedbackFactor(factor, ts_us);
}

/*****************************************************************************************************************
Name     : SetReceiveRtoDecFactor
Function : for rtc application, call this function to optimize retransmission performance, The larger the factor,
           the better the performance, while maybe large variation in packet loss rate.
In param : const slid_win_hdl &win_hdl
           const u32 &factor   // range[1, 30]
           const u64 &ts_us
Out param: void
Return   : u32   // 0:SUCESS, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.09.08
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 SetReceiveRtoDecFactor(const slid_win_hdl &win_hdl, const u32 &factor, const u64 &ts_us) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    return ((SlidWin*)win_hdl)->SetRecvRtoDecFactor(factor, ts_us);
}

/*****************************************************************************************************************
Name     : ResetSlidWin
Function : reset the slid window.
In param : const slid_win_hdl &win_hdl
           const u32 &current_sns
           const u64 &ts_us
Out param: void
Return   : u32  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 ResetSlidWin(const slid_win_hdl &win_hdl, const u32 &current_sn, const u64 &ts_us) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    g_com_error[0] = '\0';

    ((SlidWin*)win_hdl)->ResetWin(current_sn, ts_us);

    return SELF_SUCESS;
}

/*****************************************************************************************************************
Name     : CreateSlidWin
Function : create a new slid window
In param : pNetQualityReportCallBack qaulity_func  // when rtt or jitter or loss has been changed, call this to report.
           pReportSnBitMapCallBack bitmap_func     // when move window, report received sn's bitmap.
           pLogOutCallBack log_out_func
           pGetCurLogLevel cur_log_level_func
           const u32 &current_sn                   // first sn, normal is 0.
           const u64 &current_ts_us                // current system timestamp, unit is us.
           const u32 &min_stable_us                // min start to calc loss time length.
           const u32 &mode                         // SlidWinMode
           void *instance_mem                      // ensure the memory enough, call SlidwinInstanceSize() to get the
                                                   // the slid window bytes size, the slidwin module don't ensure free
                                                   // this memory even if called the DeleteSlidWin().
           void *cur_context_hdl                   // current context handler
           u8 *cache                               // at least 1.5k bytes.
           const u32 &filter_flag                  // 0: transport slid window, 1: filter slid window
Out param: void
Return   : slid_win_hdl  // NULL: failed, , the others: sucess.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
slid_win_hdl CreateSlidWin(pNetQualityReportCallBack qaulity_func, pReportSnBitMapCallBack bitmap_func,
                           pLogOutCallBack log_out_func, pGetCurLogLevel cur_log_level_func, const u32 &current_sn,
                           const u64 &current_ts_us, const u32 &min_stable_us, const u32 &mode, void *instance_mem,
                           void *cur_context_hdl, u8 *cache, const u32 &filter_flag) {
    if ((NULL == qaulity_func) || (NULL == bitmap_func) || (kSlidWinModeButt <= mode) || (NULL == instance_mem)
     || (NULL == cache)) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "input paramter is invalid");
        return NULL;
    }

    u32 filter = ((0 == filter_flag) ? SELF_NO : SELF_YES);

    SlidWin *instance = new (instance_mem)SlidWin(current_sn, current_ts_us, min_stable_us, mode, qaulity_func, 
                                        bitmap_func, log_out_func, cur_log_level_func, cur_context_hdl, cache, filter);
    if (nullptr == instance) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "malloc memory failed");
        return NULL;
    }

    g_com_error[0] = '\0';

    #if (1 == ENABLE_SECURE_PROTECT)
    g_win_hdl_mgr.emplace(instance, WinContext(current_ts_us));
    #endif

    return (slid_win_hdl)instance;
}

/*****************************************************************************************************************
Name     : DeleteSlidWin
Function : delete a slid window instance, which has been creaded.
In param : const slid_win_hdl &win_hdl
Out param: void
Return   : void*   // NULL:failed, call WinErrorInfo() to get the error information,
                   // the others: sucess, it's the current slid window instance memory address.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
void* DeleteSlidWin(const slid_win_hdl &win_hdl) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the %p isn't slid window handler", win_hdl);
        return NULL;
    }

    g_win_hdl_mgr.erase(itr);
    #endif

    g_com_error[0] = '\0';

    delete ((SlidWin*)win_hdl);

    return win_hdl;
}

/*****************************************************************************************************************
Name     : SelfWinErrorInfo
Function : get the slid window error information.
In param : const slid_win_hdl &win_hdl
           const u32 &error_code        // 0: current last error information,
                                        // the others: the error information for the error code.
Out param: void
Return   : const char*  // error information, max 256 chars.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
#if (__linux__ || __APPLE__)
const char* SelfWinErrorInfo(const slid_win_hdl &win_hdl, const u32 &error_code) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the %p isn't slid window handler", win_hdl);
        return &(g_com_error[0]);
    }
    #endif

    if (('\0' != g_com_error[0]) && (SELF_SUCESS == error_code)) {
        return &(g_com_error[0]);
    }

    if (0 == error_code) {
        #if (1 == ENABLE_SECURE_PROTECT)
        return (const char*)(itr->second.last_error_);
        #else
        return "";
        #endif
    }

    if (((u32)SlidWinErrorCode::kErrorcodeButt) <= (error_code & 0x0000FFFF)) {
        return "The error code don't belong to slid window";
    }

    // the error code convert to error information.
    const char *error_info[] = {
        "no any error",
        "no enough free memory",
        "invalid slid window handler",
        "input paramter is invalid",
        "current sn is invalid",
        "setting no supported in send mode for the current parameter",
        "nack packet's head or tail sn is invalid"
    };

    return error_info[error_code & 0x0000FFFF];
}
#endif

#if (_WIN32 || _WIN64)
const char* SelfWinErrorInfo(const slid_win_hdl &win_hdl, const u8* err_code) {
    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the %p isn't slid window handler", win_hdl);
        return &(g_com_error[0]);
    }
    #endif

    u32 error_code = 0;
    if ('-' != err_code[0])
    {
        sscanf((char*)err_code,"%d", &error_code);
    }

    if (('\0' != g_com_error[0]) && (SELF_SUCESS == error_code)) {
        return &(g_com_error[0]);
    }

    if (0 == error_code) {
        #if (1 == ENABLE_SECURE_PROTECT)
        return (const char*)(itr->second.last_error_);
        #else
        return "";
        #endif
    }

    if (((u32)SlidWinErrorCode::kErrorcodeButt) <= (error_code & 0x0000FFFF)) {
        return "The error code don't belong to slid window";
    }

    // the error code convert to error information.
    const char *error_info[] = {
        "no any error",
        "no enough free memory",
        "invalid slid window handler",
        "input paramter is invalid",
        "current sn is invalid",
        "setting no supported in send mode for the current parameter",
        "nack packet's head or tail sn is invalid"
    };

    return error_info[error_code & 0x0000FFFF];
}
#endif

/*****************************************************************************************************************
Name     : ObtainNetworkQuality
Function : actively get the network quality.
In param : const slid_win_hdl &win_hdl
           f32 *loss
           u32 *rtt_us
           u32 *jitter_us
           u32 *congest_rank
           u32 *rto_us
           u32 *pps
           u32 *discard_dir
Out param: void
Return   : u32  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 ObtainNetworkQuality(const slid_win_hdl &win_hdl, f32 *loss, u32 *rtt_us, u32 *jitter_us, u32 *congest_rank,
                         u32 *rto_us, u32 *pps, u32 *discard_dir) {
    #ifdef _SELFDEBUG
    if ((NULL == loss) || (NULL == rtt_us) || (NULL == jitter_us) || (NULL == congest_rank) || (NULL == rto_us)
     || (NULL == pps) || (NULL == discard_dir)) {
        return (u32)(SlidWinErrorCode::kInputParamIsNull);
    }
    #endif

    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    return ((SlidWin*)win_hdl)->ObtainNetworkQuality(loss, rtt_us, jitter_us, congest_rank, rto_us, pps, discard_dir);
}

u32 ObtainLossCalcWindow(const slid_win_hdl &win_hdl, u32 *loss_num, u32 *total_pack_num) {
    #ifdef _SELFDEBUG
    if ((NULL == loss_num) || (NULL == total_pack_num)) {
        return (u32)(SlidWinErrorCode::kInputParamIsNull);
    }
    #endif

    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    return ((SlidWin*)win_hdl)->ObtainLossCalcWindow(loss_num, total_pack_num);
}

/*****************************************************************************************************************
Name     : SlidwinInstanceSize
Function : Obtain the slid window bytes size.
In param : void
Out param: void
Return   : u32  // the slid window bytes number.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 SlidwinInstanceSize(void) {
    return ((u32)(sizeof(SlidWin) + MIN_MEM_GAP_SZ));
}

/*****************************************************************************************************************
Name     : PrintWinBitMap
Function : print the slid window's bitmap into the out_str memory.
In param : const slid_win_hdl &win_hdl
           u8 *out_str
           const u32 &mem_size   // it need be enough large.
Out param: u8 *out_str
           u32 *str_size
Return   : u32  // the slid window bytes number.

Mdf history  :
1.Date       : 2024.01.09
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
u32 PrintWinBitMap(const slid_win_hdl &win_hdl, u8 *out_str, const u32 &mem_size, u32 *str_size) {
    #ifdef _SELFDEBUG
    if ((NULL == out_str) || (512 > mem_size)) {
        return (u32)(SlidWinErrorCode::kInputParamIsNull);
    }
    #endif

    #if (1 == ENABLE_SECURE_PROTECT)
    unordered_map<SlidWin*, WinContext>::const_iterator itr = g_win_hdl_mgr.find((SlidWin*)win_hdl);
    if (g_win_hdl_mgr.end() == itr) {
        snprintf(g_com_error, MAX_ERR_INFO_SZ, "the 0x%p isn't slid window handler", win_hdl);
        return (u32)(SlidWinErrorCode::kInvalidSlidWinHdl);
    }
    #endif

    *str_size = ((SlidWin*)win_hdl)->PrintBitMap(out_str, mem_size);

    return SELF_SUCESS;
}

u8* GetSlidWinVersion(u8 *pout_version) {
    char version_type[64] = {0};

    if (NULL == pout_version) {
        return NULL;
    }

    #ifdef _SELFDEBUG
    snprintf(&(version_type[0]), sizeof(version_type), "_debug");
    #else
    snprintf(&(version_type[0]), sizeof(version_type), "_release");
    #endif

    #if (_WIN32 || _WIN64)
    #pragma warning(disable:4996)
    sprintf((char*)pout_version, "%s_%s  %s%s", // NOLINT
            __DATE__, __TIME__, VBUILDVERSION, &(version_type[0]));
    #pragma warning(default:)
    #endif

    #if (__linux__ || __APPLE__)
    sprintf((char*)pout_version, "%s_%s  %s%s", // NOLINT
            __DATE__, __TIME__, VBUILDVERSION, &(version_type[0]));
    #endif

    return pout_version;
}

u8* GetSlidWinPureVer(u8 *pout_version) {
    char version_type[64] = {0};

    if (NULL == pout_version) {
        return NULL;
    }

    #ifdef _SELFDEBUG
    snprintf(&(version_type[0]), sizeof(version_type), "_debug");
    #else
    snprintf(&(version_type[0]), sizeof(version_type), "_release");
    #endif

    #if (_WIN32 || _WIN64)
    #pragma warning(disable:4996)
    sprintf((char*)pout_version, "%s%s", VBUILDVERSION, &(version_type[0])); // NOLINT
    #pragma warning(default:)
    #endif

    #if (__linux__ || __APPLE__)
    sprintf((char*)pout_version, "%s%s", VBUILDVERSION, &(version_type[0]));  // NOLINT
    #endif

    return pout_version;
}

#ifdef _UTTEST
SlidWin& HandlerToObject(const slid_win_hdl &win_hdl) {
    return *((SlidWin*)win_hdl);
}
#endif

#ifdef __cplusplus
}
#endif

