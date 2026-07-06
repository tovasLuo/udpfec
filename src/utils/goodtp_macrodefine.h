#ifndef _GOOD_TP_MACRO_DEFINE_H_
#define _GOOD_TP_MACRO_DEFINE_H_
/*********************************************************************************************************************

                           Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: macro_define.h
  Version  : Initial
  Author   : Albort.Feng
  Function : Defines the common macro for goodtp.
  Modify record:
  1.Date   : August 23, 2023
    Author : Albort.Feng
    Content: New creates file.

*********************************************************************************************************************/
#include <stdint.h>

// 0: common application, 1: vedio application, 2: game acceleration
#define APPLICATION_TYPE          (2)

#define ENABLE_CB_OUTLOG          (1)
#define ENABLE_UDP_LOG            (0)
#define ENABLE_INTERFACE_LOG      (0)
#define ENABLE_INPUT_PARAM_CHCK   (0)
#define ENABLE_INNER_VALID_CHCK   (1)
#define ENABLE_TRACE_CODE_FLAG    (0)
#define ENABLE_BOOST_AI_FLAG      (1)
#define ENABLE_ARQ_BOOST_FLAG     (1)
#define ENABLE_FRAME_COPY_OUT     (0)
#define ENABLE_ARQ                (1)
#define ENABLE_FEC                (1)
#define ENABLE_MD_PERF_CHECK      (0)
#define ENABLE_STACK_CHECK        (0)
#define ENABLE_TRAN_MEM_CHECK     (0)

#define SUPPORT_RELIABLE_TRAN     (0)

#define FLOAT_ZERO                ((float)0.000001)

// business macro
#define MAX_ERROR_INFO_LEN        (256)

#define MAX_GTP_INST_NUM          (64)

#define GTP_HANDLER_BASE          (100000)

#define GTP_VERSION               (0x02)

// 统一版本判断入口：以后升级协议、新增按版本区分的特性，一律用这个宏判断，
// 不要在各处手写魔数比较。
// GtpVersionAtLeast 是给"未来"特性门禁用的通用">="判断（比如新协议版本引入可选
// 字段、且新旧版本仍能互通时，用它判断"对端够不够新，能不能用这个新特性"）。
#define GtpVersionAtLeast(ver, min_ver)      ((u8)((ver) >= (min_ver)))

// 入口"能不能通话"用严格相等：目前不知道未来更高版本会强制新增什么必须理解的
// 字段，宁可拒绝也不要用当前版本的解析逻辑去误读一个更高版本的包。等真的引入
// 向前兼容的新版本时，再把这里放宽成 GtpVersionAtLeast(ver, MIN_ACCEPT_VER)。
#define GtpIsAcceptablePeerVersion(ver)       ((u8)((ver) == GTP_VERSION))

#define MIN_STACK_SIZE            (1024 << 9)

#define GtpLimit(min, max, value) ((((min) > (value)) ? (min) : (((max) < (value)) ? (max) : (value))))

#ifdef _UTTEST
#define PRIVATE        public
#define PROTECTED      public
#define C3PTP_STATIC
#else
#define PRIVATE        private
#define PROTECTED      protected
#define C3PTP_STATIC   static
#endif

#if (_WIN32 || _WIN64 || _SELFANDROID || __APPLE__)
#define REGISTER
#else
#define REGISTER register
#endif

#if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
#define MIN_SESSION_TTL_US         (600000)
#define SLID_WIN_SIZE              (16382)

#define DEFAULT_FEC2_BOOK_ID       (2)

#define DEFAULT_BOOST_TIMES        (3)
#define INIT_BOOST_TIMES           (3)

#define MAX_RELIABLE_LOSS_THRESHLD (90.00001)
#define MAX_REALTIME_LOSS_THRESHLD (70.00001)

#define RMV_RELIABLE_LOSS_THRESHLD (70.00001)
#define RMV_REALTIME_LOSS_THRESHLD (50.00001)

#define MAX_SUPPORT_PPS            (60000)

// cache size for fec receiving buffer.
#define MAX_FEC2_CACHE_CAPACITY    (256)
#define FEC2_CACHE_CAPACITY_MASK   ((MAX_FEC2_CACHE_CAPACITY - 1))

#define MIN_CLEAR_FEC_RECV_STEP    (16)
#define MAX_CLEAR_FEC_RECV_STEP    (240)

#define MAX_RSV_WIN_SIZE           (2176)

#if (_WIN32 || _WIN64 || _SELFANDROID || __APPLE__)
// client
#define ARQ_SPECS                  (7)

#define MAX_SESSION_NUM            (128)
#define MAX_PPS_PER_SESSION        (128)
#else
// server
#define ARQ_SPECS                  (11)

#define MAX_SESSION_NUM            (1024)
#define MAX_PPS_PER_SESSION        (128)
#endif
#endif

#if (2 == APPLICATION_TYPE)
#define MIN_SESSION_TTL_US         (4000000)
#define SLID_WIN_SIZE              (2048)

#define DEFAULT_FEC2_BOOK_ID       (5)

#define DEFAULT_BOOST_TIMES        (3)
#define INIT_BOOST_TIMES           (0)

#define MAX_RELIABLE_LOSS_THRESHLD (80.00001)
#define MAX_REALTIME_LOSS_THRESHLD (25.00001)

#define RMV_RELIABLE_LOSS_THRESHLD (60.00001)
#define RMV_REALTIME_LOSS_THRESHLD (15.00001)

#define MAX_SUPPORT_PPS            (20000)

// cache size for fec receiving buffer.
#define MAX_FEC2_CACHE_CAPACITY    (32)
#define FEC2_CACHE_CAPACITY_MASK   ((MAX_FEC2_CACHE_CAPACITY - 1))

#define MIN_CLEAR_FEC_RECV_STEP    (4)
#define MAX_CLEAR_FEC_RECV_STEP    (28)

#define MAX_RSV_WIN_SIZE           (640)

#if (_WIN32 || _WIN64 || _SELFANDROID || __APPLE__)
// client
#define ARQ_SPECS                  (5)

#define MAX_SESSION_NUM            (256)
#define MAX_PPS_PER_SESSION        (128)
#else
// server
#define ARQ_SPECS                  (11)

#define MAX_SESSION_NUM            (5120)
#define MAX_PPS_PER_SESSION        (128)
#endif
#endif

#define MAX_BOOST_TIMES_PER_ONCE     (7)

#define DEF_MIX_QUINTUPLET_THRESHOLD (2000)
#define MIN_MIX_QUINTUPLET_THRESHOLD (100)
#define MAX_RCV_PACK_SN_UPDATE_STEP  (100000)

#define MAX_DISORDER_SMOOTH_US         (300000)
#define MAX_RELIABLE_TRAN_DISORDER_US  (1000000)
#define MAX_RELIABLE_TRAN_DISORDER_SN  (4000)

#define MAX_ARQ_NODE_NUM        (((MAX_PPS_PER_SESSION << (ARQ_SPECS)) + (MAX_PPS_PER_SESSION << (ARQ_SPECS - 1))))

#define MAX_RETRAN_PACKET_TIMES (3)
#define DEFAULT_RTO_TIMEOUT_US  (100000)

#define MAX_KEEPALIVE_TIME_LEN_US (800000)

#define MIN_SESSION_STABLE_TIME_US     (500000)
#define DEFAULT_HANDLER_CALC_PERIOD_US (50000)
#define MAX_HANDLER_CALC_PERIOD_US     (75000)
#define MIN_HANDLER_CALC_PERIOD_US     (4000)

#define DEFAULT_MAX_FRAME_PERIOD_US    (50000)

#if (2 == APPLICATION_TYPE)
#define MAX_FEEDBACK_NACK_PERIOD_US    (50000)
#else
#define MAX_FEEDBACK_NACK_PERIOD_US    (110000)
#endif

#define MIN_FRAME_PERIOD_US     (30)

#define GTP_INST_COM_CACHE_SIZE (3072)

#define TEST_RTT_PERIOD_US      (100000)

#define IPV4_BIN_IP_SIZE        (4)
#define IPV6_BIN_IP_SIZE        (16)

#define FREE_C3BUF_NUM_PER      (512)

#define STREAM_QOS_WITH_FEC     (63)

#define DEFAULT_RTT_US          (500000)

#define MAX_CACHE_LOSS_NUM      (32)

#define MIN_START_QUICK_RTT_STD (200000)

#define MIN_RTO_US              (25000)
#define MAX_RTO_US              (500000)

#define MIN_SYNC_PERIOD_US      (MIN_SESSION_TTL_US)

#define TURN_OFF_FEC            (0)

#define MIN_ZERO_LOSS_EXIST_US  (1000000)

#define DEFAULT_BOOST_PERIOD_US (10000)
#define MIN_BOOST_PERIOD_US     (1000)
#define MAX_BOOST_PERIOD_US     (10000)
#define DEFAULT_BOOST_THRESHELD (5)

#define MIN_WHELL_PERIOD_US     (20000)

#define AUTO_FEC_MACRO          (1)

#define MIN_RTT_EXIST_TM_SZ_US  (35000)

#define MIN_ENHANCE_BOOST_PPS   (200)

// new fec
#define MAX_FEC2_MODE_BOOK_ID    (8)
#define MAX_VALID_FEC2_BOOK_ID   (7)

#define MAX_FEC2_MATRIX_H_SIZE   (4)
#define MAX_FEC2_MATRIX_V_SIZE   (MAX_FEC2_MATRIX_H_SIZE)
#define MAX_FEC2_HILL_SIZE       ((MAX_FEC2_MATRIX_H_SIZE) + (MAX_FEC2_MATRIX_H_SIZE) - 3)

#define INVALID_ENCODE_POS       (0xFF)

#ifdef _SELFDEBUG
#define goodtp_asserta(expression) {\
    if (expression) {            \
        return;                  \
    }                            \
}

#define goodtp_assertb(expression, retval) {\
    if (expression) {                    \
        return (retval);                 \
    }                                    \
}
#else
#define goodtp_asserta(expression) {\
}

#define goodtp_assertb(expression, retval) {\
}
#endif

enum class GtpPackType: uint8_t {
    kGtpDataPackType       = 0x01,
    kGtpAckPackType        = 0x02,
    kGtpFecPackType        = 0x03,
    kGtpSetRecvRttPackType = 0x04,
    kGtpRttTstResPackType  = 0x05,
    kGtpNackPackType       = 0x06,

    GtpPackTypeButt
};

enum class GtpSessionMode: uint32_t {
    kSender   = 1,
    kReceiver = 2
};

enum class GtpSessStat: uint8_t {
    kInitReq = 0,
    kInitRes = 1,
    kRunning = 2,

    kGtpSessionButt
};

enum class NetQuality: uint8_t {
    kNetQualityGood = 0,
    kNetQualityBad  = 1,

    kNetQualityButt
};

// new fec
enum class Fec2Mode: uint8_t {
    kBlock       = 0,
    kConvolution = 1,
    kFountain    = 2,

    kFec2ModeButt
};

enum class Fec2CodeDir: uint8_t {
    kHorizontal = 0,
    kVertical   = 1,
    kUpHill     = 2,
    kDownHill   = 3,

    kFec2CodeDirButt
};

enum class Fec2TryRestoreType: uint8_t {
    kFec2RestoreBoot = 0,
    kFec2RestoreHorizontal = 1,
    kFec2RestoreVertical   = 2,
    kFec2RestoreUpHill     = 3,
    kFec2RestoreDownHill   = 4,

    kFec2TryRestoreButt
};

enum class HarqReTranType: uint32_t {
    kNotRetranType   = 0,
    kRtoRetranType   = 1,
    kBoostRetranType = 2,
    kQuickRetranType = 3,

    kHarqRetranTypeButt
};

enum class CorrectionAct: uint8_t {
    kZeroAct = 0,
    kAddAct  = 1,
    kDecAct  = 2,

    kCorrectionActButt
};

enum class SessionHealthState: uint8_t {
    kHealthNoraml    = 0,
    kTimeOutDeleting = 1,
    kDeletingByApp   = 2,

    kSessionHealthButt
};

#define SessionHealthToStr(status) ((((uint8_t)(SessionHealthState::kHealthNoraml)) == ((uint8_t)(status))) ? "normal":\
                                   ((((uint8_t)(SessionHealthState::kTimeOutDeleting)) == ((uint8_t)(status))) ? "timeout":\
                                   ((((uint8_t)(SessionHealthState::kDeletingByApp)) == ((uint8_t)(status))) ? "appdelet":"unknown")))
                                    

#define LinkStatToStr(status)   ((((uint8_t)(GtpSessStat::kInitReq)) == ((uint8_t)(status))) ? "init req":\
                                ((((uint8_t)(GtpSessStat::kInitRes)) == ((uint8_t)(status))) ? "init res":\
                                ((((uint8_t)(GtpSessStat::kRunning)) == ((uint8_t)(status))) ? "running":"unknown")))

#define Fec2CodeDirToStr(fec_type) ((((uint8_t)(Fec2CodeDir::kHorizontal)) == ((uint8_t)(fec_type))) ? "horizontal":\
                                ((((uint8_t)(Fec2CodeDir::kVertical)) == ((uint8_t)(fec_type))) ? "vertical":\
                                ((((uint8_t)(Fec2CodeDir::kUpHill)) == ((uint8_t)(fec_type))) ? "uphill":"downhill")))

#define Fec2ModeToStr(fec_mode) ((((uint8_t)(Fec2Mode::kBlock)) == ((uint8_t)(fec_mode))) ? "block":\
                                ((((uint8_t)(Fec2Mode::kConvolution)) == ((uint8_t)(fec_mode))) ? "convolution":\
                                ((((uint8_t)(Fec2Mode::kFountain)) == ((uint8_t)(fec_mode))) ? "fountain":"unknown")))

#define CheckIntTurnOver(head_val, tail_val, std_val) ((((std_val) >= (0xFFFFFFFF - (head_val) + (tail_val))) ? \
                                                       GTP_YES : GTP_NO))

#define LossDirToStr(loss_dir)  (((0 == (loss_dir)) ? "up" : "down"))

#define PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos) {\
    (str_len) += (wrt_num);\
    if (((u32)(wrt_num)) >= (free_sz)) {\
        return (str_len);\
    }\
    (wrt_pos) += (wrt_num);\
    (free_sz) -= (wrt_num);\
    (wrt_num)  = 0;\
}

#define PrintAfterHandlerBreak(str_len, wrt_num, free_sz, wrt_pos) {\
    (str_len) += (wrt_num);\
    if (((u32)(wrt_num)) >= (free_sz)) {\
        break;\
    }\
    (wrt_pos) += (wrt_num);\
    (free_sz) -= (wrt_num);\
    (wrt_num)  = 0;\
}

#endif
