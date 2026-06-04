#ifndef _ERROR_CODE_H_
#define _ERROR_CODE_H_
#include "cos.h"

enum class C3coreErrorCodeEnum:u32 {
    kC3coreErrorCodeBase = cos_error_code_butt,
    kInvalidIpErr        = 0x00008001,
    kDistributeTestIdErr = 0x00008002,
    kNewEpollTranObjErr  = 0x00008003,
    kSetSocketReuseErr   = 0x00008004,
    kEpollIdIsExistErr   = 0x00008005,
    kSysAbnormalErr      = 0x00008006,
    kCrtGtpInstErr     = 0x00008007,
    kCrtC3MemPoolErr     = 0x00008008,
    kCrtRunningDotErr    = 0x00008009,
    kCrtSlidWinErr       = 0x0000800a,
    kIpFormatErr         = 0x0000800b
};

#endif

