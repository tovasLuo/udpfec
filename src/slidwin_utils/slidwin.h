#ifndef _SLID_WIN_H_
#define _SLID_WIN_H_

#include <limits.h>
#include <stdint.h>

/*****************************************************************************************************************
Name     : WinErrorInfo
Function : get a slid window's the lastest error information.
In param : a  // slid window's handler, it's created by CreateSlidWin() function.
           b  // error code, when b = 0, it's means get the common error information.
Out param: void
Return   : const char*  // error infortion string.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
#if (_WIN32 || _WIN64)
#define WinErrorInfo(a, b) SelfWinErrorInfo(a, (uint8_t*)#b##"-")
#endif

#if (__linux__ || __APPLE__)
#define SelfWinErrorInfo1(a)   SelfWinErrorInfo(a, 0)
#define SelfWinErrorInfo2(a,b) SelfWinErrorInfo(a, b)

#define ERR_GET_2TH_ARG(arg1, arg2, arg3,...) arg3
#define ERR_FUN_MACRO_CHOOSER(...) ERR_GET_2TH_ARG(__VA_ARGS__, SelfWinErrorInfo2, SelfWinErrorInfo1, )

#define WinErrorInfo(...) ERR_FUN_MACRO_CHOOSER(__VA_ARGS__)(__VA_ARGS__)
#endif

#define RESET_WIN_RETURN  (0xA5A55A5A)

typedef enum _SlidWinMode {
    kSendSlidWinMode = 0,
    kRecvSlidWinMode = 1,

    kSlidWinModeButt
}SlidWinMode;
typedef uint32_t SlidWinModeU32;

typedef enum _DiscardDirect {
    kUpLinkerLoss   = 0,
    kDownLinkerLoss = 1,

    kDiscardDirectButt
}DiscardDirect;
typedef uint32_t DiscardDirectU32;

typedef enum _AckNackType {
    kAckType  = 0,
    kNackType = 1,

    kAckNackButt
}AckNackType;
typedef uint32_t AckNackTypeU32;

typedef enum _PreCongestRank {
    kNoGenCongest      = 0,
    kMaybeGenCongest   = 1,
    kWillBeGenCongest  = 2,
    kMustBeGenCongest  = 3,
    kUnknownGenCongest = 4,

    kPreCongestRankButt
}PreCongestRank;
typedef uint32_t PreCongestRankU32;

typedef void* slid_win_hdl;

typedef struct _ReceivedSnBitMap {
    uint32_t head_sn_;
    uint32_t tail_sn_;
    uint32_t recv_loss_;     // expanded 128
    uint32_t mem_size_;
    uint32_t rto_sn_;        // less or equal rto_sn_ empty bitmap's sn will need to be resend at once.
    uint64_t cache_ts_us_;
    const uint8_t *sn_bit_map_;
}ReceivedSnBitMap;

#pragma pack(1)
typedef struct _Nack {
    uint16_t sn_offset_;
}Nack;

#if (_WIN32 || _WIN64)
#pragma warning(disable:4200)
#endif
typedef struct _NackData {
    uint32_t head_sn_;
    uint32_t tail_sn_;
    uint32_t recv_loss_;     // expanded 128
    uint32_t rto_sn_;
    uint64_t cache_ts_us_;

    uint16_t nack_num_;
    Nack nack_[0];
}NackData;
#if (_WIN32 || _WIN64)
#pragma warning(default:)
#endif

#pragma pack()

typedef void (*pNetQualityReportCallBack)(slid_win_hdl win_hdl, void *cur_cntxt_hdl, const uint32_t &avg_rtt_us,
                                    const uint32_t &jitter_us, const float &loss, const DiscardDirectU32 &discard_dir,
                                    const uint32_t &pre_congest_rank, const uint32_t &rto_us, const uint32_t &avg_pps,
                                    const uint32_t &loss_num);

typedef void (*pReportSnBitMapCallBack)(slid_win_hdl win_hdl, void *cur_cntxt_hdl, const void* bit_map_data,
                                        const uint32_t &ack_or_nack);

typedef void (*pLogOutCallBack)(uint32_t log_level, const char *fmt, ...);
typedef uint32_t (*pGetCurLogLevel)(void);

#ifdef __cplusplus
extern "C" {
#endif
/*****************************************************************************************************************
Name     : RepeatPacketFilter
Function : checks current sn is duplicate packet's sn.
In param : const slid_win_hdl &win_hdl
           const uint32_t &sn
           const u64 &ts_us
Out param: void
Return   : uint32_t  // 0: isn't repeat packet, 1: is repeat packet, others: the win_hdl isn't receiving slid window.

Mdf history  :
1.Date       : 2023.09.19
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t RepeatPacketFilter(const slid_win_hdl &win_hdl, const uint32_t &sn, const uint64_t &ts_us);

/*****************************************************************************************************************
Name     : SnEntrySlidWin
Function : push a received sn to slid window.
In param : const slid_win_hdl &win_hdl
           const uint32_t &sn
           const uint64_t &ts_us
Out param: void
Return   : uint32_t  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t SnEntrySlidWin(const slid_win_hdl &win_hdl, const uint32_t &sn, const uint64_t &ts_us);

/*****************************************************************************************************************
Name     : NackSnEntrySlidWin
Function : push series nack sn to slid window.
In param : const slid_win_hdl &win_hdl
           const NackData *nack
           const uint64_t &ts_us
Out param: void
Return   : uint32_t  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2023.12.15
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t NackSnEntrySlidWin(const slid_win_hdl &win_hdl, const NackData *nack, const uint64_t &ts_us);

/*****************************************************************************************************************
Name     : MarkSnAbandonedSlidWin
Function : tell a receive slid window to stop generating NACK candidates for 'sn' -- the sender's own
           realtime-stream retry budget is already known to be exhausted for it upstream. No-op if sn has
           already arrived or has fallen outside the current window.
In param : const slid_win_hdl &win_hdl
           const uint32_t &sn
Out param: void
Return   : uint32_t  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2026.07.06
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t MarkSnAbandonedSlidWin(const slid_win_hdl &win_hdl, const uint32_t &sn);

uint32_t NackSnOffsetEntrySlidWin(const slid_win_hdl &win_hdl, const uint32_t &head_sn,
                                  const uint32_t &tail_sn, const uint32_t &recv_loss,
                                  const uint32_t &rto_sn, const uint16_t nack_offset[],
                                  const uint32_t &nack_num, const uint64_t &ts_us);

/*****************************************************************************************************************
Name     : BlockU64SnEntrySlidWin
Function : push a block received sn to slid window.
In param : const slid_win_hdl &win_hdl
           const uint64_t &sn_bit_map
           const uint32_t &begin_sn
           const uint64_t &ts_us
           const uint32_t &calc_loss_flag
Out param: void
Return   : uint32_t  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2023.10.11
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t BlockU64SnEntrySlidWin(const slid_win_hdl &win_hdl, const uint64_t &sn_bit_map, const uint32_t &begin_sn,
                                const uint64_t &ts_us, const uint32_t &calc_loss_flag);

/*****************************************************************************************************************
Name     : BlockU32SnEntrySlidWins
Function : push a block received sn to slid window.
In param : const slid_win_hdl &win_hdl
           const uint32_t &sn_bit_map
           const uint32_t &begin_sn
           const uint64_t &ts_us
           const uint32_t &calc_loss_flag
Out param: void
Return   : uint32_t  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2023.10.11
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t BlockU32SnEntrySlidWin(const slid_win_hdl &win_hdl, const uint32_t &sn_bit_map, const uint32_t &begin_sn,
                                const uint64_t &ts_us, const uint32_t &calc_loss_flag);

/*****************************************************************************************************************
Name     : SnTsEntrySlidWin
Function : push a new sn's timestamp to slid window.
In param : const slid_win_hdl &win_hdl
           const uint32_t &sn
           const uint64_t &ts_us
Out param: void
Return   : uint32_t  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.
                     // RESET_WIN_RETURN: it means window has been reset.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t SnTsEntrySlidWin(const slid_win_hdl &win_hdl, const uint32_t &sn, const uint64_t &ts_us);

/*****************************************************************************************************************
Name     : ResetSlidWin
Function : reset the slid window.
In param : const slid_win_hdl &win_hdl
           const uint32_t &current_sn
           const uint64_t &ts_us
Out param: void
Return   : uint32_t  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t ResetSlidWin(const slid_win_hdl &win_hdl, const uint32_t &current_sn, const uint64_t &ts_us);

/*****************************************************************************************************************
Name     : CalcQualityByHandler
Function : calc network quality by handler for long time no packet.
In param : const slid_win_hdl &win_hdl
           const uint64_t &ts_us
           const uint32_t &must_calc_flag  // 0: whether calc by slid window's status, 1: must calc.
Out param: void
Return   : uint32_t   // 0:SUCESS, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.07.06
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t CalcQualityByHandler(const slid_win_hdl &win_hdl, const uint64_t &ts_us, const uint32_t &must_calc_flag);

/*****************************************************************************************************************
Name     : CreateSlidWin
Function : create a new slid window
In param : pNetQualityReportCallBack qaulity_func  // when rtt or jitter or loss has been changed, call this to report.
           pReportSnBitMapCallBack bitmap_func     // when move window, report received sn's bitmap.
           pLogOutCallBack log_out_func
           pGetCurLogLevel cur_log_level_func
           const uint32_t &current_sn              // first sn, normal is 0.
           const uint64_t &current_ts_us           // current system timestamp, unit is us.
           const uint32_t &min_stable_us           // min start to calc loss time length.
           const uint32_t &mode                    // SlidWinMode
           void *instance_mem                      // ensure the memory enough, call SlidwinInstanceSize() to get the
                                                   // the slid window bytes size, the slidwin module don't ensure free
                                                   // this memory even if called the DeleteSlidWin().
           void *cur_context_hdl                   // current context handler
           u8 *cache                               // at least 1.5k bytes.
           const uint32_t &filter_flag             // 0: transport slid window, 1: filter slid window
Out param: void
Return   : slid_win_hdl  // NULL: failed, , the others: sucess.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
slid_win_hdl CreateSlidWin(pNetQualityReportCallBack qaulity_func, pReportSnBitMapCallBack bitmap_func,
                      pLogOutCallBack log_out_func, pGetCurLogLevel cur_log_level_func, const uint32_t &current_sn,
                      const uint64_t &current_ts_us, const uint32_t &min_stable_us, const uint32_t &mode,
                      void *instance_mem, void *cur_context_hdl, uint8_t *cache, const uint32_t &filter_flag = 0);

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
void* DeleteSlidWin(const slid_win_hdl &win_hdl);

/*****************************************************************************************************************
Name     : SetLinkerRttUs
Function : setting a linker rrt value, unit: us.
In param : const slid_win_hdl &win_hdl
           const uint32_t &rtt_us
           const uint64_t &ts_us
Out param: void
Return   : uint32_t   // 0:SUCESS, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.07.06
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t SetLinkerRttUs(const slid_win_hdl &win_hdl, const uint32_t &rtt_us, const uint64_t &ts_us);

/*****************************************************************************************************************
Name     : SetLinkerLoss
Function : setting a linker loss coming from receiver.
In param : const slid_win_hdl &win_hdl
           const uint32_t &loss
           const uint64_t &ts_us
           const uint32_t &rtt_us
Out param: void
Return   : uint32_t   // 0:SUCESS, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.07.08
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t SetLinkerLoss(const slid_win_hdl &win_hdl, const uint32_t &loss, const uint64_t &ts_us,
                       const uint32_t &rtt_us);

uint32_t SetLinkerLossEx(const slid_win_hdl &win_hdl, const uint32_t &loss, const uint64_t &ts_us,
                         const uint32_t &rtt_us, const uint32_t &sample_total_pack_num,
                         const uint32_t &head_sn);

/*****************************************************************************************************************
Name     : AdjustOptimizeLossFactor
Function : when occurs single director loss, calling this function to adjust optimizing factor to decress loss.
In param : const slid_win_hdl &win_hdl
           const float &factor    // range[0.1, 1.9]
           const uint64_t &ts_us
Out param: void
Return   : uint32_t   // 0:SUCESS, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.08.08
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t AdjustOptimizeLossFactor(const slid_win_hdl &win_hdl, const float &factor, const uint64_t &ts_us);

/*****************************************************************************************************************
Name     : SetReceiveRtoDecFactor
Function : for rtc application, call this function to optimize retransmission performance, The larger the factor,
           the better the performance, while maybe large variation in packet loss rate.
In param : const slid_win_hdl &win_hdl
           const uint32_t &factor   // range[1, 30]
           const uint64_t &ts_us
Out param: void
Return   : uint32_t   // 0:SUCESS, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.09.08
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t SetReceiveRtoDecFactor(const slid_win_hdl &win_hdl, const uint32_t &factor, const uint64_t &ts_us);

/*****************************************************************************************************************
Name     : SelfWinErrorInfo
Function : get the slid window error information.
In param : const slid_win_hdl &win_hdl
           const uint32_t &error_code   // 0: current last error information,
                                        // the others: the error information for the error code.
Out param: void
Return   : const char*  // error information, max 256 chars.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
#if (__linux__ || __APPLE__)
const char* SelfWinErrorInfo(const slid_win_hdl &win_hdl, const uint32_t &error_code);
#endif
#if (_WIN32 || _WIN64)
const char* SelfWinErrorInfo(const slid_win_hdl &win_hdl, const uint8_t* err_code);
#endif

/*****************************************************************************************************************
Name     : ObtainNetworkQuality
Function : actively get the network quality.
In param : const slid_win_hdl &win_hdl
           float *loss
           uint32_t *rtt_us
           uint32_t *jitter_us
           uint32_t *pre_congest_rank
           uint32_t *rto_us
           uint32_t *pps
           uint32_t *discard_dir
Out param: void
Return   : uint32_t  // 0: sucess, the others: failed, call WinErrorInfo() to get the error information.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t ObtainNetworkQuality(const slid_win_hdl &win_hdl, float *loss, uint32_t *rtt_us, uint32_t *jitter_us,
                              uint32_t *pre_congest_rank, uint32_t *rto_us, uint32_t *pps, uint32_t *discard_dir);

uint32_t ObtainLossCalcWindow(const slid_win_hdl &win_hdl, uint32_t *loss_num, uint32_t *total_pack_num);

/*****************************************************************************************************************
Name     : SlidwinInstanceSize
Function : Obtain the slid window bytes size.
In param : void
Out param: void
Return   : uint32_t  // the slid window bytes number.

Mdf history  :
1.Date       : 2020.04.28
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t SlidwinInstanceSize(void);

/*****************************************************************************************************************
Name     : GetSlidWinVersion
Function : Obtain the slid window library version.
In param : uint8_t *out_version
Out param: uint8_t *out_version
Return   : uint8_t*  // the slid window lib's version.

Mdf history  :
1.Date       : 2023.11.01
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint8_t* GetSlidWinVersion(uint8_t *out_version);

/*****************************************************************************************************************
Name     : GetSlidWinPureVer
Function : Obtain the slid window library pure version.
In param : uint8_t *out_version
Out param: uint8_t *out_version
Return   : uint8_t*  // the slid window lib's pure version.

Mdf history  :
1.Date       : 2024.03.29
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint8_t* GetSlidWinPureVer(uint8_t *pout_version);

/*****************************************************************************************************************
Name     : PrintWinBitMap
Function : print the slid window's bitmap into the out_str memory.
In param : const slid_win_hdl &win_hdl
           uint8_t *out_str
           const u32 &mem_size   // it need be enough large.
Out param: uint8_t *out_str
           uint32_t *str_size
Return   : uint32_t  // the slid window bytes number.

Mdf history  :
1.Date       : 2024.01.09
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
uint32_t PrintWinBitMap(const slid_win_hdl &win_hdl, uint8_t *out_str, const uint32_t &mem_size, uint32_t *str_size);

#ifdef __cplusplus
}
#endif

#endif
