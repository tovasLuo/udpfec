#ifndef _GOOD_TP_MGR_H_
#define _GOOD_TP_MGR_H_
/*********************************************************************************************************************

                               Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_mgr.h
  Version  : Initial
  Author   : Albort.Feng
  Function : Manage the goodtp module header file.
  Modify record:
  1.Date   : August 23, 2023
    Author : Albort.Feng
    Content: New creates file.

*********************************************************************************************************************/
#include "goodtp_mem_pool.h"
#include "goodtp_session.h"
#include "goodtp_comstruct.h"
#include "goodtp_macrodefine.h"
#include "goodtp.h"

#include "tranmempool.h"

#if (__linux__ || __APPLE__)
#include <pthread.h>

#ifdef __APPLE__
#include <os/lock.h>
#endif
#endif

#if (_WIN32 || _WIN64)
#include <WS2tcpip.h>
#include <winSock2.h>
#include <mswsock.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Kernel32.lib")
#endif

#include <stdio.h>
#include <unordered_map>
#include <string>

using namespace std;

#if ((_WIN32 || _WIN64) && (1 == ENABLE_UDP_LOG))
#ifdef _WIN64
typedef i64 gtp_sock;
#else
typedef i32 gtp_sock;
#endif

extern "C" void gtp_udpSend(gtp_sock nSockHdl, const void *data, u32 nLen);
#endif

#define RETURN_ERR(mderrbase, errno) {\
    return (((GtpGetSysId() << 24)&0xff000000)  \
          | (((((u32)mderrbase)) << 16)&0x00ff0000) \
          | ((((u32)errno)) & 0x0000ffff));         \
}

#define GEN_ERR(mderrbase, errno) (((GtpGetSysId() << 24)&0xff000000)   \
                                   | (((((u32)mderrbase)) << 16)&0x00ff0000)  \
                                   | ((((u32)errno)) & 0x0000ffff))

#ifndef _UTTEST
#if ((_WIN32 || _WIN64) && (1 == ENABLE_UDP_LOG))
#if (1 == ENABLE_CB_OUTLOG)
#define GtpLog(wrt_log_cb, module_id, log_level, fmt, ...) {\
    if ((NULL != g_goodtp_inst_mgr.cur_log_level_cb_) && (NULL != wrt_log_cb)\
     && (g_goodtp_inst_mgr.cur_log_level_cb_() >= (log_level))) {\
        const char *lvl_nm[] = {"Emerg", "Alert", "Critical", "Error", "Warning", "Notice", "Info", "Debug"}; \
        GtpDateTime data_time;\
        GtpSysDateTime(&data_time);\
        u8 buf[2048] = {0};\
        u32 str_len  = 0;\
        u32 level    = ((NULL != g_goodtp_inst_mgr.cur_log_level_cb_) ? g_goodtp_inst_mgr.cur_log_level_cb_() : 0);\
        str_len = (u32)snprintf((char*)buf, 2048, "[%s]%04u.%02u.%02u_%02u:%02u:%02u:%03u mid=0x%02x(%s:%d):" fmt, \
                             lvl_nm[log_level], data_time.ulYear, data_time.ulMonth, data_time.ulMday, \
                             data_time.ulHour, data_time.ulMin, data_time.ulSec, data_time.ulMSec, (u32)module_id, \
                             (char*)GtpDelFileNamePath((const u8*)__FILE__), __LINE__, ##__VA_ARGS__);\
        gtp_udpSend(g_goodtp_inst_mgr.udp_log_sfd_, &(buf[0]), str_len);\
        wrt_log_cb(log_level, "[%s]%04u.%02u.%02u_%02u:%02u:%02u:%03u mid=0x%02x(%s:%d):" fmt, lvl_nm[log_level], \
                   data_time.ulYear, data_time.ulMonth, data_time.ulMday, data_time.ulHour, data_time.ulMin, \
                   data_time.ulSec, data_time.ulMSec, (u32)module_id, \
                   (char*)GtpDelFileNamePath((const u8*)__FILE__), __LINE__, ##__VA_ARGS__);\
    }\
}
#else
#define GtpLog(wrt_log_cb, module_id, log_level, fmt, ...) {\
    if ((NULL != g_goodtp_inst_mgr.cur_log_level_cb_) && (NULL != wrt_log_cb)\
     && (g_goodtp_inst_mgr.cur_log_level_cb_() >= (log_level))) {\
        const char *lvl_nm[] = {"Emerg", "Alert", "Critical", "Error", "Warning", "Notice", "Info", "Debug"}; \
        GtpDateTime data_time;\
        GtpSysDateTime(&data_time);\
        u8 buf[2048] = {0};\
        u32 str_len  = 0;\
        u32 level    = ((NULL != g_goodtp_inst_mgr.cur_log_level_cb_) ? g_goodtp_inst_mgr.cur_log_level_cb_() : 0);\
        str_len = (u32)snprintf((char*)buf, 2048, "[%s]%04u.%02u.%02u_%02u:%02u:%02u:%03u mid=0x%02x(%s:%d):" fmt, \
                             lvl_nm[log_level], data_time.ulYear, data_time.ulMonth, data_time.ulMday, \
                             data_time.ulHour, data_time.ulMin, data_time.ulSec, data_time.ulMSec, (u32)module_id, \
                             (char*)GtpDelFileNamePath((const u8*)__FILE__), __LINE__, ##__VA_ARGS__);\
        gtp_udpSend(g_goodtp_inst_mgr.udp_log_sfd_, &(buf[0]), str_len);\
    }\
}
#endif
#else
#define GtpLog(wrt_log_cb, module_id, log_level, fmt, ...) {\
    if ((NULL != g_goodtp_inst_mgr.cur_log_level_cb_) &&(NULL != wrt_log_cb)\
     && (g_goodtp_inst_mgr.cur_log_level_cb_() >= (log_level))) {\
        const char *lvl_nm[] = {"Emerg", "Alert", "Critical", "Error", "Warning", "Notice", "Info", "Debug"}; \
        GtpDateTime data_time;\
        GtpSysDateTime(&data_time);\
        wrt_log_cb(log_level, "[%s]%04u.%02u.%02u_%02u:%02u:%02u:%03u mid=0x%02x(%s:%d):" fmt, lvl_nm[log_level], \
                   data_time.ulYear, data_time.ulMonth, data_time.ulMday, data_time.ulHour, data_time.ulMin, \
                   data_time.ulSec, data_time.ulMSec, (u32)module_id, \
                   (char*)GtpDelFileNamePath((const u8*)__FILE__), __LINE__, ##__VA_ARGS__);\
    }\
}
#endif
#else
extern "C" void UtLog(const u32 &log_level, const char *fmt, ...);

#define GtpLog(wrt_log_cb, module_id, log_level, fmt, ...) {\
    const char *lvl_nm[] = {"Emerg", "Alert", "Critical", "Error", "Warning", "Notice", "Info", "Debug"}; \
    GtpDateTime data_time;\
    GtpSysDateTime(&data_time);\
    UtLog(log_level, "[%s]%04u.%02u.%02u_%02u:%02u:%02u:%03u mid=0x%02x(%s:%d):" fmt, lvl_nm[log_level], \
          data_time.ulYear, data_time.ulMonth, data_time.ulMday, data_time.ulHour, data_time.ulMin, \
          data_time.ulSec, data_time.ulMSec, (u32)module_id, (char*)GtpDelFileNamePath((const u8*)__FILE__),\
          __LINE__, ##__VA_ARGS__);\
}
#endif

#if (_WIN32 || _WIN64)
#ifdef _WIN64
#define GtpHdlPointerToInt(gtp_hdl)  ((GtpHandler)((i64)(gtp_hdl)))
#define GtpHdlIntToPointer(gtp_hdl)  ((GtpHandler_p)((u64)gtp_hdl))
#define AddressToUint(address)       (((u64)(address)))
#else
#define GtpHdlPointerToInt(gtp_hdl)  ((GtpHandler)((i32)(gtp_hdl)))
#define GtpHdlIntToPointer(gtp_hdl)  ((GtpHandler_p)gtp_hdl)
#define AddressToUint(address)       (((u32)(address)))
#endif
#endif

#ifdef __linux__
#ifdef __i386__
#define GtpHdlPointerToInt(gtp_hdl)  ((GtpHandler)((i32)(gtp_hdl)))
#define GtpHdlIntToPointer(gtp_hdl)  ((GtpHandler_p)gtp_hdl)
#define AddressToUint(address)       (((u32)(address)))
#else
#define GtpHdlPointerToInt(gtp_hdl)  ((GtpHandler)((i64)(gtp_hdl)))
#define GtpHdlIntToPointer(gtp_hdl)  ((GtpHandler_p)((u64)gtp_hdl))
#define AddressToUint(address)       (((u64)(address)))
#endif
#endif

#ifdef __APPLE__
#if (arm64 == ARCHS_STANDARD)
#define GtpHdlPointerToInt(gtp_hdl)  ((GtpHandler)((i64)(gtp_hdl)))
#define GtpHdlIntToPointer(gtp_hdl)  ((GtpHandler_p)((u64)gtp_hdl))
#define AddressToUint(address)       (((u64)(address)))
#else
#define GtpHdlPointerToInt(gtp_hdl)  ((GtpHandler)((i32)(gtp_hdl)))
#define GtpHdlIntToPointer(gtp_hdl)  ((GtpHandler_p)gtp_hdl)
#define AddressToUint(address)       (((u32)(address)))
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

class GtpMemGuard {
 public:
    explicit GtpMemGuard(volatile void *spin_locker);
    ~GtpMemGuard();

PRIVATE:
    volatile void *spin_locker_;
};

class GoodTp {
 public:
    explicit GoodTp(const i32 &pos, const u32 &session_ttl_us, const GtpCallBackParam &reg_cb,
                    const MemPoolConfig &mem_pool_cfg);
    ~GoodTp();

    u32 Init(void);

    GtpHandler  GetSelfHandler(void);
    GtpSession* GetSession(GtpAddr *tran_addr, GtpHandler_p app_gtp_hdl,
                           const u32 &mode = (u32)(GtpSessionMode::kSender), const u32 &pack_sn = 0,
                           const u32 &sort_sn = 0, const u32 &sort_sn_valid = GTP_NO,
                           const u32 &create_session = GTP_YES);
    GtpSession* GetSession(const GtpSessionKey &session_key);
    GtpSession* GetSessionByAddr(GtpAddr *tran_addr);

    void CheckResourceActiveStatus(GtpHandler_p app_gtp_hdl);
    void SendArqCachedPackWhenDead(void);
    void DelSpsSession(GtpHandler_p app_gtp_hdl, GtpAddr *tran_addr);

    u32 ShowBitMap(const u8 *src_ip, const u8 *dst_ip, u8 *out_str, const u32 &mem_size);
    u32 ShowLinker(const u8 *matched_str, u8 *out_str, const u32 mem_size);
    u32 ShowAlgorithmParam(const u8 *src_ip, const u8 *dst_ip, u8 *out_str, const u32 &mem_size);
    u32 SessionNumber(void);

    void GetTranMemPoolStatus(u8 mem[], const u32 &mem_size);

    u32 CheckSingleThreadCalling(void);

    GtpSession* BuildNewSession(const GtpSessionKey &key, GtpAddr *tran_addr, GtpHandler_p app_gtp_hdl,
                                const u32 &mode, const u32 &pack_sn, const u32 &sort_sn,
                                const u32 &sort_sn_valid, const goodtp_sock &sfd);

PRIVATE:
    void DelSpsSessionByKey(GtpHandler_p app_gtp_hdl, const GtpSessionKey &link_key);

 public:
    u64 current_ts_us_;
    u64 last_wheel_running_ts_us_;

    GtpCallBackParam cb_;

    GtpMemPool arq_node_mem_pool_;

    TranMemPool  packet_mem_pool_;

    u32 m_3rd_malloc_num_;
    u32 m_3rd_free_num_;

    i32 handler_base_;   // using for check application the memory out of bounds.

PRIVATE:
    GtpHandler handler_;

    u64 session_ttl_us_;
    u64 second_ts_us_;

    GtpMemPool session_mem_pool_;
    u32 session_pool_num_;

    ConsumeTime wheel_consume_;

    u8 win_cache_[GTP_INST_COM_CACHE_SIZE];

    unordered_map<GtpSessionKey, GtpSession*, GtpSessionKeyHash> session_map_;

    /* 0: 4X4(h=1 v=1 uh=1 dh=1), 1: 4X4(h=1 v=1 uh=0 dh=0), 2: 4X1(h=1 v=0 uh=0 dh=0)
       3: 2X2(h=1 v=1 uh=1 dh=1), 4: 2X2(h=1 v=1 uh=0 dh=0), 5: 2X1(h=1 v=0 uh=0 dh=0) */
    GtpFec2Mode fec_mode_book_[MAX_FEC2_MODE_BOOK_ID];

    goodtp_tid init_tid_;
};

typedef struct _ErrorInfoMgr {
    goodtp_tid tid_;

    u8 error_info_[MAX_ERROR_INFO_LEN];
}ErrorInfoMgr;

typedef struct _GtpInstMgr {
    // using only during creating goodtp instance.
    #ifdef __linux__
    pthread_spinlock_t *spin_;
    #endif

    #ifdef __APPLE__
    os_unfair_lock *spin_;
    #endif

    #if (_WIN32 || _WIN64)
    u32 *spin_;
    #if (1 == ENABLE_UDP_LOG)
    gtp_sock udp_log_sfd_;
    #endif
    #endif

    ErrorInfoMgr *error_info_;

    // volatile only prevents compiler caching/reordering, not atomicity. inst_num_/runing_flag_ are
    // read-modify-written (|=, &=, -=) from whichever thread happens to be driving each GoodTp
    // instance -- and instances are explicitly designed to run concurrently from different threads
    // (see CLAUDE.md) -- with no lock protecting the shared updates. Confirmed as a genuine data
    // race with ThreadSanitizer. Fixed at the call sites with __sync_fetch_and_*() builtins rather
    // than switching these fields to std::atomic: this struct is memset() and returned by value in
    // InitGtpInstMgr(), which std::atomic's deleted copy/move constructor would break.
    volatile u32 inst_num_;
    u32 sys_id_;
    volatile u64 runing_flag_;  // bit = 1, it means that position goodtp instance is used, eg: bit0=1,
                                // the instance that located in goodtp_inst[0] is being used.

    pLogLevelCallBack cur_log_level_cb_;

    GoodTp *goodtp_inst_[MAX_GTP_INST_NUM];
}GtpInstMgr;

/*****************************************************************************************************************
Name     : GtpHandlerToObj
Function : Covert a goodtp handler to instance.
In param : GtpHandler hdl
Out param: void
Return   : GoodTp*    // NULL: failed, the others: goodtp instance address.

Mdf history  :
1.Date       : 2023.08.25
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
GoodTp* GtpHandlerToObj(GtpHandler hdl);

/*****************************************************************************************************************
Name     : GtpObjToHandler
Function : Covert a goodtp instance to handler.
In param : GtpHandler hdl
Out param: void
Return   : GtpHandler    // GTP_ERR: failed, the others: goodtp instance handler.

Mdf history  :
1.Date       : 2023.08.25
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
GtpHandler GtpObjToHandler(GoodTp *goodtp_obj);

u32 GtpGetSysId(void);
u64 GtpSysTimestampUs(void);
void GtpSysDateTime(GtpDateTime *date_time);
u32 GtpSockAddrInputIsValid(const u8 *sock_addr, const u32 &sock_addr_len, const u32 &allow_zero_len,
                            const u32 &len_err_code);
u32 GtpTranAddrSockInputIsValid(const GtpAddr *tran_addr);
u32 GtpLinkerKeySockInputIsValid(const GtpLinkerKey *linker_key);
u32 GtpSockAddrToStrAddr(void *sock_addr, u8 *out_ip, const u32 &mem_size, u16 *out_port);
u32 GtpSockAddrToBinAddr(void *sock_addr, u8 *out_ip, u16 *out_ip_size, u16 *out_port);
u32 GtpGetSelfSockAddr(const goodtp_sock &sfd, void *sock_addr, u32 addr_size);
u8* GtpDelFileNamePath(const u8 *pFileName);
u32 GtpSendPackCallBack(GtpHandler_p gtp_hdl, void *pack, u32 size, GtpAddr *tran_addr);
void LogBinaryData(const u8 *bin_data, const u32 &size, u8 *log_buf, const u32 &buf_size);
void GtpAddrToStrIpAndPort(GtpAddr *tran_addr, u8 self_ip[GTP_MAX_STR_IP_SZ],
                           u8 peer_ip[GTP_MAX_STR_IP_SZ], u16 *self_port, u16 *peer_port);

goodtp_tid GtpGetThreadId(void);
goodtp_pid GtpGetProcessId(void);
u32 GtpTranAddrIsValid(GtpAddr *tran_addr);
u32 GtpCheckPackMemOutBoundry(void *pack_mem, void *tran_addr);

extern "C" GtpInstMgr g_goodtp_inst_mgr;

#ifdef __cplusplus
}
#endif

#endif
