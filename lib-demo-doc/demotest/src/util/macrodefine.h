#ifndef _MACRO_DEF_H_
#define _MACRO_DEF_H_

#include "cos.h"

// 0: common application, 1: vedio application, 2: game acceleration
#define APPLICATION_TYPE     (2)

#define NEW_GOODTP_LIB       (1)

#define ENABLE_PATH_SWITCH   (0)

// 0: no any changing payload, 1: changing payload from 0 to 256, 2: large packet test.
#define LARGE_PACKET_TEST    (0)

#define MAX_LOG_SIZE         (1024 << 15)

#define ENABLE_OUT_CHECK     (1)

#define MIN_FEEDBACK_FACTOR  (0.1)
#define MAX_FEEDBACK_FACTOR  (1.9)

#define MIN_RECV_RTO_DEC_K   (1)
#define MAX_RECV_RTO_DEC_K   (30)

#define SIZE_64K             ((1024 << 6))
#define SIZE_512K            ((1024 << 10))
#define MAX_EPOLL_SIZE       (128)
#define MAX_EPOLL_EVENT_NUM  (64)
#define RCD_CONSUME_BUF_SIZE (512)

#define SERVER_RECORD_SWITCH (0)

#if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
#define BASE_PAYLOAD_SIZE    (4)
#endif

#if (2 == APPLICATION_TYPE)
#define BASE_PAYLOAD_SIZE    (128)
#endif

// 63: turn on fec, 0:turn off fec
#define FEC_SWITCH           (63)

#define GTP_STREAM_TYPE      (0)
#define GTP_REMOVE_JITTER    (0)

enum class moduleIdEnum:u32 {
    kModuleIdBase = cos_module_id_butt,
    kModuleBootId,
    kModuleOamId,
    kModuleEpollId
};

enum class rolerEnum:u32 {
    kClientRoler = 1,
    kServerRoler = 2
};

enum class switchName:u32 {
    kAllSwitch        = 0,
    kAckFeedLogSwitch = 1,
    kSockErrLogSwitch = 2,
    kWinErrLogSwitch  = 3,

    kInvalidSwitch
};

#endif

