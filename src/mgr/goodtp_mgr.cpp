/*********************************************************************************************************************

                               Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_mgr.cpp
  Version  : Initial
  Author   : Albort.Feng
  Function : Manage the goodtp module realizeing code file.
  Modify record:
  1.Date   : August 23, 2023
    Author : Albort.Feng
    Content: New creates file.

*********************************************************************************************************************/

#include "goodtp_mgr.h"
#include "goodtp_mem_pool.h"
#include "goodtp_session.h"
#include "goodtp_comstruct.h"
#include "goodtp_macrodefine.h"
#include "bitlinker.h"
#include "slidwin.h"

#include <stdlib.h>
#include <memory.h>
#include <stdio.h>
#include <errno.h>

#if (__linux__ || __APPLE__)
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

#include <netdb.h>
#include <arpa/inet.h>

#ifdef __linux__
#include <linux/tcp.h>
#endif

#ifdef __APPLE__
#include <sys/socket.h>
#include <os/lock.h>
#endif
#endif

#if (_WIN32 || _WIN64)
#include <time.h>

#include <WS2tcpip.h>
#include <WinSock2.h>
#include <Windows.h>

#pragma comment(lib, "ws2_32.lib")
#endif

#ifdef __cplusplus
extern "C" {
#endif

i32 g_goodtp_handler_base    = 1;

/* Positional aggregate init (not designated init) so this compiles under C++11/14/17;
 * MSVC only allows designated initializers with /std:c++20. Field order must track
 * the _GtpInstMgr declaration in goodtp_mgr.h: spin_ [, udp_log_sfd_], error_info_,
 * inst_num_, sys_id_, runing_flag_ — remaining trailing members are zero-initialized. */
#ifdef __linux__
GtpInstMgr g_goodtp_inst_mgr = {NULL, NULL, 1, 1, 1};
#endif

#ifdef __APPLE__
GtpInstMgr g_goodtp_inst_mgr = {NULL, NULL, 1, 1, 1};
#endif

#if (_WIN32 || _WIN64)
#if (1 == ENABLE_UDP_LOG)
GtpInstMgr g_goodtp_inst_mgr = {NULL, -1, NULL, 1, 1, 1};

#else
GtpInstMgr g_goodtp_inst_mgr = {NULL, NULL, 1, 1, 1};
#endif
#endif

#if ((_WIN32 || _WIN64) && (1 == ENABLE_UDP_LOG))
void gtp_udpSend(gtp_sock nSockHdl, const void *data, u32 nLen) {
    u32 nRunRslt = 0;
    u32 nAddrLen = 0;

    struct sockaddr_in srvAddr;

    nAddrLen = sizeof(srvAddr);

    memset(&srvAddr, 0x00, sizeof(srvAddr));

    int addrLen = sizeof(struct sockaddr_in);
    WSAStringToAddressA((char*)"127.0.0.1", AF_INET, NULL, (struct sockaddr*)&srvAddr, &addrLen);

    srvAddr.sin_family = AF_INET;
    srvAddr.sin_port   = htons(10000);

    sendto(nSockHdl, (char*)data, nLen, 0, (struct sockaddr*)&srvAddr, nAddrLen);

    return;
}

void gtp_bindSock(gtp_sock nSockHdl, const char *IPDotAddr) {
    u32 nAddrLen = 0;

    struct sockaddr_in srvAddr;

    nAddrLen = sizeof(srvAddr);

    memset(&srvAddr, 0, sizeof(srvAddr));

    WSAStringToAddressA((char*)IPDotAddr, AF_INET, NULL, (struct sockaddr*)&srvAddr, (int*)&nAddrLen);

    srvAddr.sin_family  = AF_INET;
    srvAddr.sin_port    = htons(0);

    bind(nSockHdl, (struct sockaddr*)&srvAddr, nAddrLen);

    return;
}

void gtp_setSockNoBlock(gtp_sock nSockHdl) {
    i32 nComVar      = 0;
    u32 nSockBufSize = (1024 << 11);

    unsigned long imode = 1;

    BOOL bDontLinger = FALSE;
    setsockopt(nSockHdl, SOL_SOCKET, SO_DONTLINGER, (char*)&bDontLinger, sizeof(BOOL));

    nComVar = ioctlsocket(nSockHdl, FIONBIO, (u_long*)&imode);
    if (0 != nComVar) {
        return;
    }

    setsockopt(nSockHdl, SOL_SOCKET, SO_RCVBUF, (const char*)&nSockBufSize, sizeof(nSockBufSize));

    setsockopt(nSockHdl, SOL_SOCKET, SO_SNDBUF, (const char*)&nSockBufSize, sizeof(nSockBufSize));

    return;
}

gtp_sock gtp_openSock(void) {
    gtp_sock nSockHdl = ((gtp_sock)-1);
    u32 nOrgIpTp      = 0;
    u32 nOrgSockTp    = 0;

    unsigned long imode = 1;
    i32 nComVar         = 0;

    nOrgIpTp   = AF_INET;
    nOrgSockTp = SOCK_DGRAM;

    nSockHdl = (gtp_sock)socket(nOrgIpTp, nOrgSockTp, 0);

    if (INVALID_SOCKET == nSockHdl) {
        return ((gtp_sock)-1);
    }

    gtp_setSockNoBlock(nSockHdl);

    gtp_bindSock(nSockHdl, "127.0.0.1");

    return nSockHdl;
}
#endif

goodtp_tid GtpGetThreadId(void) {
    #ifdef __linux__
    return pthread_self();
    #endif

    #if (_WIN32 || _WIN64)
    return GetCurrentThreadId();
    #endif

    #ifdef __APPLE__
    return (goodtp_tid)((u64)pthread_self());
    #endif
}

goodtp_pid GtpGetProcessId(void) {
    #ifdef __linux__
    return (goodtp_pid)getpid();
    #endif

    #if (_WIN32 || _WIN64)
    return (goodtp_pid)GetCurrentProcessId();
    #endif

    #ifdef __APPLE__
    return (goodtp_pid) 1;
    #endif
}

ErrorInfoMgr* GtpGetIdleErrorCache(ErrorInfoMgr cache[], const i32 &first, const i32 &last, const goodtp_tid &tid) {
    if (first > last) {
        return NULL;
    }

    if (GTP_INVALID == cache[first].tid_) {
        cache[first].tid_ = tid;
        return &(cache[first]);
    }

    if (GTP_INVALID == cache[last].tid_) {
        cache[last].tid_ = tid;
        return &(cache[last]);
    }

    i32 middle = first + ((i32)((((u32)last) - ((u32)first)) > 1));
    if (GTP_INVALID == cache[middle].tid_) {
        cache[middle].tid_ = tid;
        return &(cache[middle]);
    }

    ErrorInfoMgr *err_info_mgr = GtpGetIdleErrorCache(cache, middle + 1, last - 1, tid);
    if (NULL != err_info_mgr) {
        return err_info_mgr;
    }

    return GtpGetIdleErrorCache(cache, first + 1, middle - 1, tid);
}

ErrorInfoMgr* GtpGetSelfErrorCache(ErrorInfoMgr cache[], const i32 &first, const i32 &last, const goodtp_tid &tid) {
    if (first > last) {
        return NULL;
    }

    if (tid == cache[first].tid_) {
        return &(cache[first]);
    }

    if (tid == cache[last].tid_) {
        return &(cache[last]);
    }

    i32 middle = first + ((i32)((((u32)last) - ((u32)first)) > 1));
    if (tid == cache[middle].tid_) {
        return &(cache[middle]);
    }

    ErrorInfoMgr *err_info_mgr = GtpGetSelfErrorCache(cache, middle + 1, last - 1, tid);
    if (NULL != err_info_mgr) {
        return err_info_mgr;
    }

    return GtpGetSelfErrorCache(cache, first + 1, middle - 1, tid);
}

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
GoodTp* GtpHandlerToObj(GtpHandler hdl) {
    if ((g_goodtp_handler_base > hdl) || ((g_goodtp_handler_base + MAX_GTP_INST_NUM) <= hdl)) {
        return NULL;
    }

    i32 pos = hdl - g_goodtp_handler_base;

    if ((NULL != g_goodtp_inst_mgr.goodtp_inst_[pos])
     && (g_goodtp_handler_base != g_goodtp_inst_mgr.goodtp_inst_[pos]->handler_base_)) {
        return NULL;
    }

    u64 running_bit = 1;

    g_goodtp_inst_mgr.runing_flag_ |= (running_bit << pos);

    return g_goodtp_inst_mgr.goodtp_inst_[pos];
}

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
GtpHandler GtpObjToHandler(GoodTp *goodtp_obj) {
    GtpHandler hdl = goodtp_obj->GetSelfHandler();

    if ((g_goodtp_handler_base > hdl) || ((g_goodtp_handler_base + MAX_GTP_INST_NUM) <= hdl)) {
        return GTP_ERR;
    }

    i32 pos = hdl - g_goodtp_handler_base;
    if ((g_goodtp_inst_mgr.goodtp_inst_[pos] != goodtp_obj) || (g_goodtp_handler_base != goodtp_obj->handler_base_)) {
        return GTP_ERR;
    }

    return hdl;
}

/*****************************************************************************************************************
Name     : FindGtpMemIdlePos
Function : find a unusing positioon quickly.
In param : void *mem[]
           const i32 &first
           const i32 &last
Out param: void
Return   : i32    // -1: don't find, the others: unusing position index.

Mdf history  :
1.Date       : 2023.08.25
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
i32 FindGtpMemIdlePos(void *mem[], const i32 &first, const i32 &last) {
    if (first > last) {
        return -1;
    }

    if (NULL == mem[first]) {
        return first;
    }

    if (NULL == mem[last]) {
        return last;
    }

    i32 pos = first + ((last - first) >> 1);
    if (NULL == mem[pos]) {
        return pos;
    }

    pos = FindGtpMemIdlePos(mem, first + 1, pos - 1);
    if (-1 < pos) {
        return pos;
    }

    return FindGtpMemIdlePos(mem, pos + 1, last - 1);
}

/*****************************************************************************************************************
Name     : CreateGtpInstance
Function : creates a goodtp instance, while a single goodtp can't support multi thread using.
In param : u32 system_id
           uint32_t session_ttl_us
           GtpCallBackParam *reg_cb
           MemPoolConfig *mem_pool_cfg
Out param: void
Return   : GtpHandler_p // INVALID_GTP_HANDLER: failed and call LastGtpErrorInfo() to obtain the detailed information,
                        // the others: sucess,it's the current goodtp instance handler.

Mdf history  :
1.Date       : 2023.08.23
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
GtpHandler_p CreateGtpInstance(u32 system_id, uint32_t session_ttl_us, GtpCallBackParam *reg_cb,
                               MemPoolConfig *mem_pool_cfg) {
    GtpHandler gtp_hdl = GTP_ERR;
    u32 nret = GTP_OK;
    i32 pos  = 0;

    GoodTp *goodtp_obj = NULL;

    GtpMemGuard guard(g_goodtp_inst_mgr.spin_);

    if (NULL == reg_cb) {
        goodtp_tid tid = GtpGetThreadId();
        ErrorInfoMgr *err_info_mem = GtpGetSelfErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1,
                                                            tid);
        if (NULL == err_info_mem) {
            err_info_mem = GtpGetIdleErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1, tid);
        }

        if (NULL != err_info_mem) {
            snprintf((char*)(err_info_mem->error_info_), sizeof(err_info_mem->error_info_),
                     "the input parameter is null(reg_cb=%p)", reg_cb);
        }

        goto create_gtp_instance_exit_pos_;
    }

    #ifndef _UTTEST
    if ((NULL == reg_cb->send_pack_cb_) || (NULL == reg_cb->receive_frame_cb_)) {
        goodtp_tid tid = GtpGetThreadId();
        ErrorInfoMgr *err_info_mem = GtpGetSelfErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1,
                                                            tid);
        if (NULL == err_info_mem) {
            err_info_mem = GtpGetIdleErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1, tid);
        }

        if (NULL != err_info_mem) {
            snprintf((char*)(err_info_mem->error_info_), sizeof(err_info_mem->error_info_),
                     "the must callback interface is null(%p %p %p %p %p)", reg_cb->send_pack_cb_,
                     reg_cb->receive_frame_cb_, reg_cb->report_link_quality_cb_, reg_cb->write_log_cb_,
                     reg_cb->cur_log_level_cb_);
        }

        goto create_gtp_instance_exit_pos_;
    }
    #endif

    if (NULL == g_goodtp_inst_mgr.cur_log_level_cb_) {
        g_goodtp_inst_mgr.cur_log_level_cb_ = reg_cb->cur_log_level_cb_;
    }

    if (MAX_GTP_INST_NUM <= g_goodtp_inst_mgr.inst_num_) {
        goodtp_tid tid = GtpGetThreadId();
        ErrorInfoMgr *err_info_mem = GtpGetSelfErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1,
                                                            tid);
        if (NULL == err_info_mem) {
            err_info_mem = GtpGetIdleErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1, tid);
        }

        if (NULL != err_info_mem) {
            snprintf((char*)(err_info_mem->error_info_), sizeof(err_info_mem->error_info_),
                     "the goodtp instance has been upper limit(%u)", MAX_GTP_INST_NUM);
        }

        goto create_gtp_instance_exit_pos_;
    }

    pos = FindGtpMemIdlePos((void **)(&(g_goodtp_inst_mgr.goodtp_inst_[0])), 0, MAX_GTP_INST_NUM - 1);
    if (-1 == pos) {
        goodtp_tid tid = GtpGetThreadId();
        ErrorInfoMgr *err_info_mem = GtpGetSelfErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1,
                                                            tid);
        if (NULL == err_info_mem) {
            err_info_mem = GtpGetIdleErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1, tid);
        }

        if (NULL != err_info_mem) {
            snprintf((char*)(err_info_mem->error_info_), sizeof(err_info_mem->error_info_),
                     "there hasn't enough cache for goodtp instance(find_pos=%d inst_num=%u)",
                     pos, g_goodtp_inst_mgr.inst_num_);
        }

        goto create_gtp_instance_exit_pos_;
    }

    #if (1 == ENABLE_FEC)
    #if (_WIN32 || _WIN64 || _SELFANDROID || __APPLE__)
    mem_pool_cfg->m_1_5k_num_ += 2048;
    #else
    mem_pool_cfg->m_1_5k_num_ += (1024 << 6);
    #endif
    #endif

    goodtp_obj = new GoodTp(pos, session_ttl_us, *reg_cb, *mem_pool_cfg);
    if (NULL == goodtp_obj) {
        goodtp_tid tid = GtpGetThreadId();
        ErrorInfoMgr *err_info_mem = GtpGetSelfErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1,
                                                            tid);
        if (NULL == err_info_mem) {
            err_info_mem = GtpGetIdleErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1, tid);
        }

        if (NULL != err_info_mem) {
            snprintf((char*)(err_info_mem->error_info_), sizeof(err_info_mem->error_info_),
                     "new goodtp object failed(errorcode= %u(%s))", errno, strerror(errno));
        }

        goto create_gtp_instance_exit_pos_;
    }

    nret = goodtp_obj->Init();
    if (GTP_OK != nret) {
        delete goodtp_obj;

        goodtp_tid tid = GtpGetThreadId();
        ErrorInfoMgr *err_info_mem = GtpGetSelfErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1,
                                                            tid);
        if (NULL == err_info_mem) {
            err_info_mem = GtpGetIdleErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1, tid);
        }

        if (NULL != err_info_mem) {
            snprintf((char*)(err_info_mem->error_info_), sizeof(err_info_mem->error_info_),
                     "call goodtp.init() failed(0x%08x)", nret);
        }

        goto create_gtp_instance_exit_pos_;
    }

    g_goodtp_inst_mgr.goodtp_inst_[pos] = goodtp_obj;

    g_goodtp_inst_mgr.inst_num_ += 1;
    g_goodtp_inst_mgr.sys_id_    = system_id;

    gtp_hdl = pos + g_goodtp_handler_base;

create_gtp_instance_exit_pos_:
    #if (1 == ENABLE_TRACE_CODE_FLAG)
    GtpLog(goodtp_obj->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "Created No.%u goodtp instance.\r\n",
           pos);
    #endif

    #if (1 == ENABLE_FEC)
    #if (_WIN32 || _WIN64 || _SELFANDROID || __APPLE__)
    mem_pool_cfg->m_1_5k_num_ -= 2048;
    #else
    mem_pool_cfg->m_1_5k_num_ -= (1024 << 6);
    #endif
    #endif

    #if (_WIN32 || _WIN64)
    #ifdef _WIN64
    return ((GtpHandler_p)((u64)gtp_hdl));
    #else
    return ((GtpHandler_p)gtp_hdl);
    #endif
    #endif

    #ifdef __linux__
    #ifdef __i386__
    return ((GtpHandler_p)gtp_hdl);
    #else
    return ((GtpHandler_p)((u64)gtp_hdl));
    #endif
    #endif

    #ifdef __APPLE__
    #if (arm64 == ARCHS_STANDARD)
    return ((GtpHandler_p)((u64)gtp_hdl));
    #else
    return ((GtpHandler_p)gtp_hdl);
    #endif
    #endif
}

/*****************************************************************************************************************
Name     : DeleteGtpInstance
Function : Deletes a goodtp instance, it's a secure function.
In param : GtpHandler_p gtp_hdl
Out param: void
Return   : u32    // GTP_OK: sucess,
                  // the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2023.08.24
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 DeleteGtpInstance(GtpHandler_p gtp_hdl) {
    GtpMemGuard guard(g_goodtp_inst_mgr.spin_);

    GoodTp *gtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));
    if (NULL == gtp_obj) {
        goodtp_tid tid = GtpGetThreadId();
        ErrorInfoMgr *err_info_mem = GtpGetSelfErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1,
                                                            tid);
        if (NULL == err_info_mem) {
            err_info_mem = GtpGetIdleErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1, tid);
        }

        if (NULL != err_info_mem) {
            snprintf((char*)(err_info_mem->error_info_), sizeof(err_info_mem->error_info_),
                     "it isn't goodtp handler(%d)", (GtpHandler)((i64)gtp_hdl));
        }

        RETURN_ERR(kGtpMgrMd, kInvalidGtpHandler);
    }

    i32 pos       = ((GtpHandler)((i64)gtp_hdl)) - g_goodtp_handler_base;
    u64 using_bit = 1;

    #if (1 == ENABLE_TRACE_CODE_FLAG)
    GtpLog(gtp_obj->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "Delete No.%d goodtp instance.\r\n", pos);
    #endif

    gtp_obj->SendArqCachedPackWhenDead();

    g_goodtp_inst_mgr.runing_flag_ &= (~(using_bit << pos));

    if (0 < g_goodtp_inst_mgr.inst_num_) {
        g_goodtp_inst_mgr.inst_num_ -= 1;
    }

    if (0 == g_goodtp_inst_mgr.inst_num_) {
        g_goodtp_inst_mgr.cur_log_level_cb_ = NULL;
    }

    g_goodtp_inst_mgr.goodtp_inst_[pos] = NULL;

    delete gtp_obj;

    return GTP_OK;
}

/*****************************************************************************************************************
Name     : InsLoadGtpModule
Function : When boot the current system, it must be insload the goodtp module only one before calling any the others
           goodtp interface, calling this function but it don't create any goodtp transporing instance.
In param : void
Out param: void
Return   : u32    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2023.08.24
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 InsLoadGtpModule(void) {
    g_goodtp_inst_mgr.inst_num_    = 0;
    g_goodtp_inst_mgr.sys_id_      = 0;
    g_goodtp_inst_mgr.runing_flag_ = 0;

    g_goodtp_inst_mgr.error_info_ = (ErrorInfoMgr*)malloc(sizeof(ErrorInfoMgr) * MAX_GTP_INST_NUM);
    if (NULL == g_goodtp_inst_mgr.error_info_) {
        RETURN_ERR(kGtpMgrMd, kDistributeMemFail);
    }

    #if (_WIN32 || _WIN64)
    g_goodtp_inst_mgr.spin_ = (u32*)malloc(sizeof(u32));
    if (NULL == g_goodtp_inst_mgr.spin_) {
        RETURN_ERR(kGtpMgrMd, kDistributeMemFail);
    }

    *(g_goodtp_inst_mgr.spin_) = 0;

    #if (1 == ENABLE_UDP_LOG)
    g_goodtp_inst_mgr.udp_log_sfd_ = gtp_openSock();
    #endif
    #endif

    #ifdef __linux__
    g_goodtp_inst_mgr.spin_ = (pthread_spinlock_t*)malloc(sizeof(pthread_spinlock_t));
    if (NULL == g_goodtp_inst_mgr.spin_) {
        RETURN_ERR(kGtpMgrMd, kDistributeMemFail);
    }

    pthread_spin_init(g_goodtp_inst_mgr.spin_, PTHREAD_PROCESS_PRIVATE);
    #endif

    #ifdef __APPLE__
    g_goodtp_inst_mgr.spin_ = (os_unfair_lock*)malloc(sizeof(os_unfair_lock));
    if (NULL == g_goodtp_inst_mgr.spin_) {
        RETURN_ERR(kGtpMgrMd, kDistributeMemFail);
    }

    *(g_goodtp_inst_mgr.spin_) = OS_UNFAIR_LOCK_INIT;
    #endif

    u32 nloop = 0;

    do {
        g_goodtp_inst_mgr.goodtp_inst_[nloop]     = NULL;
        g_goodtp_inst_mgr.error_info_[nloop].tid_ = GTP_INVALID;
        nloop += 1;
    } while (MAX_GTP_INST_NUM > nloop);

    g_goodtp_handler_base = (i32)(((GtpSysTimestampUs() & 0x00000000FFFFFFFF) % GTP_HANDLER_BASE)
                          + GTP_HANDLER_BASE);

    return GTP_OK;
}

/*****************************************************************************************************************
Name     : RmLoadGtpModule
Function : When poweroff the current system, it must remove load goodtp module only one, calling this function it
           delete all the existing goodtp instances automatically.
In param : void
Out param: void
Return   : u32    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2023.08.24
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 RmLoadGtpModule(void) {
    g_goodtp_inst_mgr.inst_num_ = 0;

    u64 current_ts_us       = GtpSysTimestampUs();
    u64 exit_deadline_ts_us = current_ts_us + 100000;

    while ((0 != g_goodtp_inst_mgr.runing_flag_) && (exit_deadline_ts_us > current_ts_us)) {
        #if (__linux__ || __APPLE__)
        usleep(1000);
        #endif

        #if (_WIN32 || _WIN64)
        Sleep(1);
        #endif
        current_ts_us = GtpSysTimestampUs();
    }

    u32 nloop = 0;

    do {
        if (NULL != g_goodtp_inst_mgr.goodtp_inst_[nloop]) {
            // delete the goodtp instance, while can't call DeleteGtpInstance() to free.
            delete g_goodtp_inst_mgr.goodtp_inst_[nloop];
            g_goodtp_inst_mgr.goodtp_inst_[nloop] = NULL;
        }

        nloop += 1;
    } while (MAX_GTP_INST_NUM > nloop);

    if (NULL != g_goodtp_inst_mgr.spin_) {
        #if (_WIN32 || _WIN64)
        InterlockedExchange((u32*)(g_goodtp_inst_mgr.spin_), 0);
        #endif

        #ifdef __linux__
        pthread_spin_destroy(g_goodtp_inst_mgr.spin_);
        #endif

        #ifdef __APPLE__
        #endif

        free((void*)(g_goodtp_inst_mgr.spin_));
        g_goodtp_inst_mgr.spin_ = NULL;
    }

    if (NULL != g_goodtp_inst_mgr.error_info_) {
        free(g_goodtp_inst_mgr.error_info_);
        g_goodtp_inst_mgr.error_info_ = NULL;
    }

    g_goodtp_inst_mgr.inst_num_    = 0;
    g_goodtp_inst_mgr.sys_id_      = 0;
    g_goodtp_inst_mgr.runing_flag_ = 0;

    return GTP_OK;
}

/*****************************************************************************************************************
Name     : LastGtpErrorInfo
Function : Get goodtp module last commont error information when creating goodtp instance faile, when call c3tpt
           interface failedly, it shall be call this function to get last error information, otherwise this error
           information maybe loss, and a same error information can get only one.
In param : void
Out param: void
Return   : const char*

Mdf history  :
1.Date       : 2023.09.14
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
const char* LastGtpErrorInfo(void) {
    goodtp_tid tid = GtpGetThreadId();
    ErrorInfoMgr *err_info_mem = GtpGetSelfErrorCache(g_goodtp_inst_mgr.error_info_, 0, MAX_GTP_INST_NUM - 1, tid);
    if (NULL != err_info_mem) {
        err_info_mem->tid_ = GTP_INVALID;
        return (char*)(err_info_mem->error_info_);
    }

    return "not find any error info";
}

/*****************************************************************************************************************
Name     : GtpAddrToHostAddr
Function : Converts goodtp address to host address and port.
In param : GtpAddr *goodtp_addr
Out param: u8 out_self_ip[]    // memory's size is 64 bytes at least.
           u8 out_peer_ip[]    // memory's size is 64 bytes at least.
           u16 *out_self_port
           u16 *out_peer_port
Return   : u32  // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2023.11.07
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 GtpAddrToHostAddr(GtpAddr *goodtp_addr, u8 out_self_ip[], u8 out_peer_ip[],
                        u16 *out_self_port, u16 *out_peer_port) {
    if (NULL == goodtp_addr) {
        RETURN_ERR(kGtpMgrMd, kInvalidInPutParam);
    }

    u32 nret = GTP_OK;

    if ((NULL != out_self_ip) && (NULL != out_self_port)) {
        u32 sock_addr_size = sizeof(sockaddr_in6);
        u8  sock_addr[GTP_SOCK_ADDR_SZ];

        nret = GtpGetSelfSockAddr(goodtp_addr->sfd_, &(sock_addr[0]), sock_addr_size);
        if (GTP_OK != nret) {
            return nret;
        }

        nret = GtpSockAddrToStrAddr(&(sock_addr[0]), out_self_ip, GTP_MAX_STR_IP_SZ, out_self_port);
        if (GTP_OK != nret) {
            return nret;
        }
    }

    if ((NULL != out_peer_ip) && (NULL != out_peer_port)) {
        nret = GtpSockAddrToStrAddr(&(goodtp_addr->sock_addr_[0]), out_peer_ip, GTP_MAX_STR_IP_SZ, out_peer_port);
        if (GTP_OK != nret) {
            return nret;
        }
    }

    return GTP_OK;
}

/*****************************************************************************************************************
Name     : GetGtpVersion
Function : Get GoodTp libary's version.
In param : u8 *pout_ver   // this memory must be 256 bytes at least.
Out param: u8 *pout_ver
Return   : u8*

Mdf history  :
1.Date       : 2023.11.01
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u8* GetGtpVersion(u8 *pout_ver) {
    char version_type[64] = {0};
    u8 slid_win_ver[128]  = {0};

    if (NULL == pout_ver) {
        return NULL;
    }

    #ifdef _SELFDEBUG
    snprintf(&(version_type[0]), sizeof(version_type), "_debug");
    #else
    snprintf(&(version_type[0]), sizeof(version_type), "_release");
    #endif

    sprintf((char*)pout_ver, "%s_%s  %s%s  slid_window(%s)\r\n", __DATE__, __TIME__, VBUILDVERSION,
            &(version_type[0]), GetSlidWinVersion(&(slid_win_ver[0])));

    return pout_ver;
}

/*****************************************************************************************************************
Name     : GetGtpPureVer
Function : Get GoodTp libary's pure version.
In param : u8 *pout_ver   // this memory must be 256 bytes at least.
Out param: u8 *pout_ver
Return   : u8*

Mdf history  :
1.Date       : 2024.03.29
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u8* GetGtpPureVer(u8 *pout_ver) {
    char version_type[64] = {0};

    if (NULL == pout_ver) {
        return NULL;
    }

    #ifdef _SELFDEBUG
    snprintf(&(version_type[0]), sizeof(version_type), "_debug");
    #else
    snprintf(&(version_type[0]), sizeof(version_type), "_release");
    #endif

    sprintf((char*)pout_ver, "%s%s", VBUILDVERSION, &(version_type[0]));

    return pout_ver;
}

#if (_WIN32 || _WIN64)
void GtpGettimeofday(struct timeval *tp, void *tzp) {
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

    tp->tv_sec  = (u32)clock;
    tp->tv_usec = wtm.wMilliseconds * 1000;

    return;
}
#endif

u64 GtpSysTimestampUs(void) {
    struct timeval sys;

    #if (__linux__ || __APPLE__)
    (void)gettimeofday(&sys, NULL);
    #endif

    #if (_WIN32 || _WIN64)
    (void)GtpGettimeofday(&sys, NULL);
    #endif

    return ((((u64)1000000) * ((u64)(sys.tv_sec))) + ((u64)(sys.tv_usec)));
}

void GtpSysDateTime(GtpDateTime *date_time) {
    goodtp_asserta(NULL == date_time);

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

    date_time->ulYear  = stdate.tm_year + 1900;
    date_time->ulMonth = stdate.tm_mon  + 1;
    date_time->ulMday  = stdate.tm_mday;
    date_time->ulHour  = stdate.tm_hour;
    date_time->ulMin   = stdate.tm_min;
    date_time->ulSec   = stdate.tm_sec;
    date_time->ulYDay  = stdate.tm_yday;
    }

    {
    struct timeval sys;
    #if (__linux__ || __APPLE__)
    (void)gettimeofday(&sys, NULL);
    #endif

    #if (_WIN32 | _WIN64)
    (void)GtpGettimeofday(&sys, NULL);
    #endif

    date_time->timestamp_us = (((u64)1000000) * ((u64)(sys.tv_sec))) + ((u64)(sys.tv_usec));
    date_time->ulMSec       = sys.tv_usec / 1000;
    }

    return;
}

u32 GtpGetSysId(void) {
    return g_goodtp_inst_mgr.sys_id_;
}

u32 GtpSockAddrToStrAddr(void *sock_addr, u8 *out_ip, const u32 &mem_size, u16 *out_port) {
    if (AF_INET == ((struct sockaddr*)sock_addr)->sa_family) {  // IPV4
        if (NULL != out_port) {
            *out_port = (u32)htons(((struct sockaddr_in *)(sock_addr))->sin_port);
        }

        if (NULL != out_ip) {
            memset(out_ip, 0x00, mem_size);

            #if (_WIN32 || _WIN64)
            size_t addrLen = sizeof(struct sockaddr_in);
            WSAAddressToStringA((struct sockaddr*)sock_addr, (DWORD)addrLen, NULL,
                                (char *)out_ip, (LPDWORD)(&mem_size));

            for (i32 nLoopVar = (i32)strlen((char*)out_ip); 0 < nLoopVar; --nLoopVar) {
                if (':' == out_ip[nLoopVar]) {
                    out_ip[nLoopVar] = 0;
                    break;
                }
            }
            #endif

            #if (__linux__ || __APPLE__)
            inet_ntop(((struct sockaddr_in*)(sock_addr))->sin_family, &(((struct sockaddr_in*)(sock_addr))->sin_addr),
                      (char *)out_ip, mem_size);
            #endif
        }
    } else {  // IPV6
        if (NULL != out_port) {
            *out_port = (u32)htons(((struct sockaddr_in6 *)(sock_addr))->sin6_port);
        }

        if (NULL != out_ip) {
            memset(out_ip, 0x00, mem_size);

            #if (_WIN32 || _WIN64)
            size_t addrLen = sizeof(struct sockaddr_in6);
            WSAAddressToStringA((struct sockaddr*)sock_addr, (DWORD)addrLen, NULL,
                                (char *)out_ip, (LPDWORD)(&mem_size));

            for (i32 nLoopVar = (i32)strlen((char *)out_ip); 0 < nLoopVar; --nLoopVar) {
                if (':' == out_ip[nLoopVar]) {
                    out_ip[nLoopVar] = 0;
                    break;
                }
            }
            #endif

            #if (__linux__ || __APPLE__)
            inet_ntop(((struct sockaddr_in6*)(sock_addr))->sin6_family,
                      &(((struct sockaddr_in6*)(sock_addr))->sin6_addr), (char *)out_ip, mem_size);
            #endif
        }
    }

    return GTP_OK;
}

u32 GtpSockAddrToBinAddr(void *sock_addr, u8 *out_ip, u16 *out_ip_size, u16 *out_port) {
    struct sockaddr *saddr = (struct sockaddr*)sock_addr;

    if (AF_INET == saddr->sa_family) {
        // IPV4
        struct sockaddr_in *v4SockAddr = (struct sockaddr_in*)sock_addr;

        memcpy(out_ip, &(v4SockAddr->sin_addr), IPV4_BIN_IP_SIZE);

        if (NULL != out_ip_size) {
            *out_ip_size = IPV4_BIN_IP_SIZE;
        }

        *out_port = v4SockAddr->sin_port;
    } else {
        // IPV6
        struct sockaddr_in6 *v6SockAddr = (struct sockaddr_in6*)sock_addr;

        memcpy(out_ip, &(v6SockAddr->sin6_addr), IPV6_BIN_IP_SIZE);

        if (NULL != out_ip_size) {
            *out_ip_size = IPV6_BIN_IP_SIZE;
        }

        *out_port = v6SockAddr->sin6_port;
    }

    return GTP_OK;
}


u32 GtpGetSelfSockAddr(const goodtp_sock &sfd, void *sock_addr, u32 addr_size) {
    u32 nRunRslt = GTP_OK;

    memset(sock_addr, 0x00, addr_size);

    #if (__linux__ || _SELFANDROID || __APPLE__)
    #ifdef _SELFANDROID
    nRunRslt = getsockname(sfd, (struct sockaddr *)sock_addr, (socklen_t*)&addr_size);
    #else
    nRunRslt = getsockname(sfd, (struct sockaddr *)sock_addr, &addr_size);
    #endif
    #endif

    #if (_WIN32 || _WIN64)
    nRunRslt = getsockname(sfd, (struct sockaddr *)sock_addr, (i32*)(&addr_size));
    #endif

    if (GTP_OK != nRunRslt) {
        RETURN_ERR(kGtpMgrMd, kGetSockAddrFailed);
    }

    return GTP_OK;
}

void GtpAddrToStrIpAndPort(GtpAddr *tran_addr, u8 self_ip[GTP_MAX_STR_IP_SZ], u8 peer_ip[GTP_MAX_STR_IP_SZ],
                           u16 *self_port, u16 *peer_port) {
    u32 sock_addr_size = sizeof(sockaddr_in6);
    u8  sock_addr[GTP_SOCK_ADDR_SZ] = {0};

    if (0 == tran_addr->self_addr_len_) {
        GtpGetSelfSockAddr(tran_addr->sfd_, &(sock_addr[0]), sock_addr_size);
        GtpSockAddrToStrAddr(&(sock_addr[0]), &(self_ip[0]), GTP_MAX_STR_IP_SZ, self_port);
    } else {
        GtpSockAddrToStrAddr(&(tran_addr->self_addr_[0]), &(self_ip[0]), GTP_MAX_STR_IP_SZ, self_port);
    }

    GtpSockAddrToStrAddr(&(tran_addr->sock_addr_[0]), &(peer_ip[0]), GTP_MAX_STR_IP_SZ, peer_port);

    return;
}

u8* GtpDelFileNamePath(const u8 *pFileName) {
    goodtp_assertb((NULL == pFileName), NULL);

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

    return (u8 *)(pFileName + nLoop);
}

i64 GtpGetStackFreeSize(u32 *stack_size) {
    i64 stack_free_size = 0;
    u64 stack_base_addr = 0;
    u64 stack_cur_addr  = (u64)(&stack_free_size);

    #if (__linux__ || __APPLE__)
    pthread_attr_t attr;
    size_t stacksize;
    void *stackaddr;

    pthread_attr_init(&attr);

    pthread_getattr_np(pthread_self(), &attr);
    pthread_attr_getstack(&attr, &stackaddr, &stacksize);

    stack_base_addr = (u64)stackaddr;
    *stack_size     = (u32)stacksize;
    #endif

    #if (_WIN32 || _WIN64)
    ULONG_PTR stack_low, stack_high;
    GetCurrentThreadStackLimits(&stack_low, &stack_high);
    stack_base_addr = (u64)stack_low;
    *stack_size     = (u32)(stack_high - stack_low);
    #endif

    if (stack_cur_addr >= stack_base_addr) {
        stack_free_size = (i64)(stack_cur_addr - stack_base_addr);
    } else {
        stack_free_size = (i64)(stack_base_addr - stack_cur_addr);
        stack_free_size = 0 - stack_free_size;
    }

    return stack_free_size;
}

u32 GtpTranAddrIsValid(GtpAddr *tran_addr) {
    if (NULL == tran_addr) {
        RETURN_ERR(kGtpMgrMd, kGtpTranAddrNullErr);
    }

    if ((((u32)sizeof(struct sockaddr_in)) > tran_addr->sock_addr_len_)
     || (((u32)sizeof(struct sockaddr_in6)) < tran_addr->sock_addr_len_)) {
        RETURN_ERR(kGtpMgrMd, kGtpTranAddrDstLenErr);
    }

    if (((u32)sizeof(struct sockaddr_in6)) < tran_addr->self_addr_len_) {
        RETURN_ERR(kGtpMgrMd, kGtpTranAddrSrcLenErr);
    }

    if (((u64)(((u8*)(&(tran_addr->sock_addr_len_))) + sizeof(tran_addr->sock_addr_len_)))
     != ((u64)(tran_addr->sock_addr_))) {
        RETURN_ERR(kGtpMgrMd, kGtpTranAddrDstMemErr);
    }

    if (((u64)(((u8*)(&(tran_addr->sock_addr_len_))) + sizeof(tran_addr->sock_addr_len_) + GTP_SOCK_ADDR_SZ))
     != ((u64)(tran_addr->self_addr_))) {
        RETURN_ERR(kGtpMgrMd, kGtpTranAddrSrcMemErr);
    }

    return GTP_OK;
}

u32 GtpCheckPackMemOutBoundry(void *pack_mem, void *tran_addr) {
    if ((((u8*)tran_addr) + sizeof(GtpAddr)) >= ((u8*)pack_mem)) {
        RETURN_ERR(kGtpMgrMd, kGtpPackMemOutBoundryErr);
    }

    return GTP_OK;
}

GtpMemGuard::GtpMemGuard(volatile void *spin_locker) {
    #if (_WIN32 || _WIN64)
    while (InterlockedExchange((u32*)spin_locker, 1) == 1) {
        Sleep(0);
    }
    #endif

    #ifdef __linux__
    pthread_spin_lock((pthread_spinlock_t *)spin_locker);
    #endif

    #ifdef __APPLE__
    os_unfair_lock_lock((os_unfair_lock*)spin_locker);
    #endif

    spin_locker_ = spin_locker;
    return;
}

GtpMemGuard::~GtpMemGuard() {
    #if (_WIN32 || _WIN64)
    InterlockedExchange((u32*)spin_locker_, 0);
    #endif

    #ifdef __linux__
    pthread_spin_unlock((pthread_spinlock_t *)spin_locker_);
    #endif

    #ifdef __APPLE__
    os_unfair_lock_unlock((os_unfair_lock*)spin_locker_);
    #endif

    return;
}

GoodTp::GoodTp(const i32 &pos, const u32 &session_ttl_us, const GtpCallBackParam &reg_cb,
               const MemPoolConfig &mem_pool_cfg):
    handler_(pos + g_goodtp_handler_base),
    handler_base_(g_goodtp_handler_base),
    m_3rd_malloc_num_(0),
    m_3rd_free_num_(0),
    session_ttl_us_((u64)((MIN_SESSION_TTL_US > session_ttl_us) ? MIN_SESSION_TTL_US : session_ttl_us)),
    arq_node_mem_pool_(sizeof(ArqNode), MAX_ARQ_NODE_NUM),
    session_mem_pool_((sizeof(GtpSession) + sizeof(void*) + ((SlidwinInstanceSize() << 1) + SlidwinInstanceSize())),
                      MAX_SESSION_NUM),
    wheel_consume_("gtp_wheel"),
    packet_mem_pool_(mem_pool_cfg.m_256bytes_num_, mem_pool_cfg.m_512bytes_num_, mem_pool_cfg.m_1k_num_,
                     mem_pool_cfg.m_1_5k_num_, mem_pool_cfg.m_4k_num_, mem_pool_cfg.m_8k_num_,
                     mem_pool_cfg.m_64k_num_) {
    cb_.send_pack_cb_           = reg_cb.send_pack_cb_;
    cb_.receive_frame_cb_       = reg_cb.receive_frame_cb_;
    cb_.report_link_quality_cb_ = reg_cb.report_link_quality_cb_;
    cb_.write_log_cb_           = reg_cb.write_log_cb_;
    cb_.cur_log_level_cb_       = reg_cb.cur_log_level_cb_;
    // cb_.close_session_cb_    = reg_cb.close_session_cb_;
    cb_.close_session_cb_       = NULL;

    last_wheel_running_ts_us_ = GtpSysTimestampUs();
    second_ts_us_             = last_wheel_running_ts_us_;

    session_map_.reserve(MAX_SESSION_NUM);

    fec_mode_book_[0].mode_       = (u8)(Fec2Mode::kBlock);
    fec_mode_book_[0].h_flag_     = 1;
    fec_mode_book_[0].v_flag_     = 1;
    fec_mode_book_[0].uh_flag_    = 1;
    fec_mode_book_[0].dh_flag_    = 1;
    fec_mode_book_[0].h_size_     = 4;
    fec_mode_book_[0].v_size_     = 4;
    fec_mode_book_[0].block_size_ = 16;
    fec_mode_book_[0].rcv_        = 0;

    fec_mode_book_[1].mode_       = (u8)(Fec2Mode::kBlock);
    fec_mode_book_[1].h_flag_     = 1;
    fec_mode_book_[1].v_flag_     = 1;
    fec_mode_book_[1].uh_flag_    = 0;
    fec_mode_book_[1].dh_flag_    = 0;
    fec_mode_book_[1].h_size_     = 4;
    fec_mode_book_[1].v_size_     = 4;
    fec_mode_book_[1].block_size_ = 16;
    fec_mode_book_[1].rcv_        = 0;

    fec_mode_book_[2].mode_       = (u8)(Fec2Mode::kBlock);
    fec_mode_book_[2].h_flag_     = 1;
    fec_mode_book_[2].v_flag_     = 0;
    fec_mode_book_[2].uh_flag_    = 0;
    fec_mode_book_[2].dh_flag_    = 0;
    fec_mode_book_[2].h_size_     = 4;
    fec_mode_book_[2].v_size_     = 0;
    fec_mode_book_[2].block_size_ = 4;
    fec_mode_book_[2].rcv_        = 0;

    fec_mode_book_[3].mode_       = (u8)(Fec2Mode::kBlock);
    fec_mode_book_[3].h_flag_     = 1;
    fec_mode_book_[3].v_flag_     = 1;
    fec_mode_book_[3].uh_flag_    = 1;
    fec_mode_book_[3].dh_flag_    = 1;
    fec_mode_book_[3].h_size_     = 2;
    fec_mode_book_[3].v_size_     = 2;
    fec_mode_book_[3].block_size_ = 4;
    fec_mode_book_[3].rcv_        = 0;

    fec_mode_book_[4].mode_       = (u8)(Fec2Mode::kBlock);
    fec_mode_book_[4].h_flag_     = 1;
    fec_mode_book_[4].v_flag_     = 1;
    fec_mode_book_[4].uh_flag_    = 0;
    fec_mode_book_[4].dh_flag_    = 0;
    fec_mode_book_[4].h_size_     = 2;
    fec_mode_book_[4].v_size_     = 2;
    fec_mode_book_[4].block_size_ = 4;
    fec_mode_book_[4].rcv_        = 0;

    fec_mode_book_[5].mode_       = (u8)(Fec2Mode::kBlock);
    fec_mode_book_[5].h_flag_     = 1;
    fec_mode_book_[5].v_flag_     = 0;
    fec_mode_book_[5].uh_flag_    = 0;
    fec_mode_book_[5].dh_flag_    = 0;
    fec_mode_book_[5].h_size_     = 2;
    fec_mode_book_[5].v_size_     = 0;
    fec_mode_book_[5].block_size_ = 2;
    fec_mode_book_[5].rcv_        = 0;
}

GoodTp::~GoodTp() {
    unordered_map<GtpSessionKey, GtpSession*, GtpSessionKeyHash>::iterator itr = session_map_.begin();
    while (session_map_.end() != itr) {
        delete (itr->second);
        itr = session_map_.erase(itr);
    }
}

GtpHandler GoodTp::GetSelfHandler(void) {
    return handler_;
}

u32 GoodTp::Init(void) {
    u32 nret = arq_node_mem_pool_.Init(cb_.write_log_cb_);
    if (GTP_OK != nret) {
        RETURN_ERR(kGtpMgrMd, kCrtArqNodePoolFailed);
    }

    nret = session_mem_pool_.Init(cb_.write_log_cb_);
    if (GTP_OK != nret) {
        RETURN_ERR(kGtpMgrMd, kCrtSessionPoolFailed);
    }

    nret = (i32)packet_mem_pool_.Init();
    if (GTP_OK != nret) {
        RETURN_ERR(kGtpMgrMd, kCrtTranMemPoolFailed);
    }

    current_ts_us_ = GtpSysTimestampUs();
    init_tid_      = GtpGetThreadId();

    return GTP_OK;
}

GtpSession* GoodTp::GetSession(GtpAddr *tran_addr, GtpHandler_p app_gtp_hdl, const u32 &mode, const u32 &pack_sn,
                               const u32 &sort_sn) {
    goodtp_sock sfd = (goodtp_sock)(tran_addr->sfd_);

    u8  peer_bin_ip[IPV6_BIN_IP_SIZE] = {0};
    u8  self_bin_ip[IPV6_BIN_IP_SIZE] = {0};

    u16 peer_bin_ip_size = 0;
    u16 self_bin_ip_size = 0;

    u16 peer_bin_port    = 0;
    u16 self_bin_port    = 0;

    GtpSockAddrToBinAddr(&(tran_addr->sock_addr_[0]), &(peer_bin_ip[0]), &peer_bin_ip_size, &peer_bin_port);

    if (0 != tran_addr->self_addr_len_) {
        GtpSockAddrToBinAddr(&(tran_addr->self_addr_[0]), &(self_bin_ip[0]), &self_bin_ip_size, &self_bin_port);
    } else {
        memset(self_bin_ip, 0x00, sizeof(self_bin_ip));
    }

    unordered_map<GtpSessionKey, GtpSession*, GtpSessionKeyHash>::iterator itr;

    if (GTP_NO == tran_addr->enable_key_) {
        GtpSessionKey session_key(sfd, &(peer_bin_ip[0]), peer_bin_ip_size, peer_bin_port,
                                  &(self_bin_ip[0]), self_bin_ip_size, self_bin_port);

        itr = session_map_.find(session_key);
        if (session_map_.end() != itr) {
            return itr->second;
        }

        return BuildNewSession(session_key, tran_addr, app_gtp_hdl, mode, pack_sn, sort_sn, sfd);
    }

    GtpSessionKey out_key(tran_addr->stream_key_, sfd, &(peer_bin_ip[0]), peer_bin_ip_size, peer_bin_port,
                          &(self_bin_ip[0]), self_bin_ip_size, self_bin_port);

    itr = session_map_.find(out_key);
    if (session_map_.end() != itr) {
        return itr->second;
    }

    return BuildNewSession(out_key, tran_addr, app_gtp_hdl, mode, pack_sn, sort_sn, sfd);
}

u32 GoodTp::CheckSingleThreadCalling(void) {
    goodtp_tid cur_tid = GtpGetThreadId();
    if (cur_tid != init_tid_) {
        GtpLog(cb_.write_log_cb_, kGtpMgrMd, kGtpLogLevelWarning,
               "The current thread(tid=%u) isn't the init thread(tid=%u).\r\n", (u32)cur_tid, (u32)init_tid_);
        RETURN_ERR(kGtpInterfaceMd, kAppMixThreadErr);
    }

    return GTP_OK;
}

GtpSession* GoodTp::BuildNewSession(const GtpSessionKey &key, GtpAddr *tran_addr, GtpHandler_p app_gtp_hdl,
                                    const u32 &mode, const u32 &pack_sn, const u32 &sort_sn, const goodtp_sock &sfd) {
    GtpSession *session = NULL;

    try {
        u32 l_border_sn = pack_sn;
        u32 next_out_sn = sort_sn;

        #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
        next_out_sn &= 0xFFFFFF80;
        #endif

        #if (2 == APPLICATION_TYPE)
        next_out_sn &= 0xFFFFFFC0;
        #endif

        if ((0 <= pack_sn) && (10 > pack_sn)) {
            l_border_sn = 0;
            goto create_new_session_pos_;
        }
        if ((10 <= pack_sn) && (20 > pack_sn)) {
            l_border_sn = 10;
            goto create_new_session_pos_;
        }

create_new_session_pos_:
        #if 0
        GtpSession *new_session = new ((void*)(&session_mem_pool_))GtpSession(sfd, tran_addr->sock_addr_,
                            tran_addr->sock_addr_len_, tran_addr->self_addr_, tran_addr->self_addr_len_,
                            (u8)(tran_addr->stream_type_), 0, GetSelfHandler(),
                            &arq_node_mem_pool_, cb_, current_ts_us_, session_ttl_us_, packet_mem_pool_,
                            app_gtp_hdl, &(fec_mode_book_[0]), tran_addr->context_, mode);
        #else
        GtpSession *new_session = new ((void*)(&session_mem_pool_))GtpSession(tran_addr, GetSelfHandler(),
                            &arq_node_mem_pool_, cb_, current_ts_us_, session_ttl_us_, packet_mem_pool_,
                            app_gtp_hdl, &(fec_mode_book_[0]),  mode);
        #endif

        u32 nret = new_session->Init(current_ts_us_, &(win_cache_[0]), l_border_sn, next_out_sn);
        if (GTP_OK != nret) {
            delete new_session;
            new_session = NULL;

            GtpLog(cb_.write_log_cb_, kGtpMgrMd, kGtpLogLevelError,
                   "call Init() failed(0x%08x) for a new session.\r\n", nret);
            return NULL;
        }

        session = new_session;

        session_map_.emplace(key, new_session);
    } catch(const char *c) {
        const u8* arq_mem_status = arq_node_mem_pool_.GetMemPoolStatus();
        GtpLog(cb_.write_log_cb_, kGtpMgrMd, kGtpLogLevelError, "new a session failedly_%s(mem-pool:%s)\r\n",
               c, arq_mem_status);
    }

    return session;
}

void GoodTp::DelSpsSessionByKey(GtpHandler_p app_gtp_hdl, const GtpSessionKey &link_key) {
    current_ts_us_ = GtpSysTimestampUs();

    unordered_map<GtpSessionKey, GtpSession*, GtpSessionKeyHash>::iterator itr = session_map_.find(link_key);
    if (session_map_.end() != itr) {
        if (((u8)(SessionHealthState::kHealthNoraml)) != itr->second->session_health_) {
            GtpLog(cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "%s:%u<-->%s:%u(key=%llu) the current "\
                   "session is deleted at now by goodtp(health_status=%s pid=%d tid=%d)\r\n",
                   itr->second->pb_dt_.self_ip_, (u32)(itr->second->pb_dt_.self_port_),
                   itr->second->pb_dt_.peer_ip_, (u32)(itr->second->pb_dt_.peer_port_),
                   (unsigned long long)(itr->second->pb_dt_.tran_addr_.stream_key_),
                   SessionHealthToStr(itr->second->session_health_), (i32)GtpGetProcessId(), (i32)GtpGetThreadId());
            return;
        }

        itr->second->session_health_ = (u8)(SessionHealthState::kDeletingByApp);

        #if ((_WIN32 || _WIN64) && (1 == ENABLE_INPUT_PARAM_CHCK))
        if (app_gtp_hdl != itr->second->app_gtp_hdl_) {
            GtpLog(cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "%s:%u<-->%s:%u: the same session belong "\
                   "to diffrent gtp instance(first_gtp_hdl=%p current_gtp_hdl=%p pid=%d tid=%d)\r\n",
                   itr->second->self_ip_, (u32)(itr->second->self_port_), itr->second->peer_ip_,
                   (u32)(itr->second->peer_port_), itr->second->app_gtp_hdl_, app_gtp_hdl, (i32)GtpGetProcessId(),
                   (i32)GtpGetThreadId());
        }
        #endif

        if (NULL != cb_.close_session_cb_) {
            // TODO(Albert.feng):: need call cb_.close_session_cb_()?
            // cb_.close_session_cb_(app_gtp_hdl, itr->second->public_data.tran_addr_.context_);
        }

        #if (1 == ENABLE_ARQ)
        itr->second->arq_.PopAllPack(current_ts_us_);
        #endif

        #if (1 == ENABLE_FEC)
        itr->second->fec2_obj_.PopAllPack(current_ts_us_);
        #endif

        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "deleted session%s:%u<--->%s:%u(key=%llu).\r\n",
               itr->second->pb_dt_.self_ip_, (u32)(itr->second->pb_dt_.self_port_),
               itr->second->pb_dt_.peer_ip_, (u32)(itr->second->pb_dt_.peer_port_),
               (unsigned long long)(itr->second->pb_dt_.tran_addr_.stream_key_));

        delete (itr->second);
        session_map_.erase(itr);
        return;
    }

    return;
}

GtpSession* GoodTp::GetSession(const GtpSessionKey &session_key) {
    unordered_map<GtpSessionKey, GtpSession*, GtpSessionKeyHash>::iterator itr = session_map_.find(session_key);
    if (session_map_.end() == itr) {
        return NULL;
    }

    return itr->second;
}

void GoodTp::CheckResourceActiveStatus(GtpHandler_p app_gtp_hdl) {
    current_ts_us_ = GtpSysTimestampUs();

    #if (1 == ENABLE_MD_PERF_CHECK)
    wheel_consume_.ts_us_ = current_ts_us_;
    #endif

    u64 delta_us = current_ts_us_ - last_wheel_running_ts_us_;
    if (MIN_WHELL_PERIOD_US <= delta_us) {
        GtpLog(cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "gtp_hdl=%p wheel run too slow(delta=%lluus "\
               "last_run=%lluus now=%lluus).\r\n", app_gtp_hdl, (unsigned long long)delta_us,
               (unsigned long long)last_wheel_running_ts_us_, (unsigned long long)current_ts_us_);
    }

    last_wheel_running_ts_us_ = current_ts_us_;

    #if ((_WIN32 || _WIN64) && (1 == ENABLE_UDP_LOG))
    #if (1 == ENABLE_MD_PERF_CHECK)
    u64 gtp_send_us = 0;
    u64 gtp_recv_us = 0;
    u64 app_send_us = 0;
    u64 app_recv_us = 0;
    u64 session_num = 0;
    #endif
    #endif

    unordered_map<GtpSessionKey, GtpSession*, GtpSessionKeyHash>::iterator itr = session_map_.begin();
    while (session_map_.end() != itr) {
        #if ((_WIN32 || _WIN64) && (1 == ENABLE_INPUT_PARAM_CHCK))
        if (app_gtp_hdl != itr->second->app_gtp_hdl_) {
            GtpLog(cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "%s:%u<-->%s:%u: the same session belong "\
                   "to diffrent gtp instance(first_gtp_hdl=%p current_gtp_hdl=%p pid=%d tid=%d)\r\n",
                   itr->second->self_ip_, (u32)(itr->second->self_port_), itr->second->peer_ip_,
                   (u32)(itr->second->peer_port_), itr->second->app_gtp_hdl_, app_gtp_hdl, (i32)GtpGetProcessId(),
                   (i32)GtpGetThreadId());
        }
        #endif

        if ((itr->second->last_active_ts_us_ + session_ttl_us_) <= current_ts_us_) {
            if (((u8)(SessionHealthState::kHealthNoraml)) != itr->second->session_health_) {
                GtpLog(cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "%s:%u<-->%s:%u(key=%llu) the "\
                       "current session is deleted at now(health_status=%s pid=%d tid=%d)\r\n",
                       itr->second->pb_dt_.self_ip_, (u32)(itr->second->pb_dt_.self_port_),
                       itr->second->pb_dt_.peer_ip_, (u32)(itr->second->pb_dt_.peer_port_),
                       (unsigned long long)(itr->second->pb_dt_.tran_addr_.stream_key_),
                       SessionHealthToStr(itr->second->session_health_), (i32)GtpGetProcessId(), (i32)GtpGetThreadId());
                goto wheel_check_resource_next_pos_;
            }

            itr->second->session_health_ = (u8)(SessionHealthState::kTimeOutDeleting);

            itr->second->CalcSessionQualityBfDead(current_ts_us_);

            if (NULL != cb_.close_session_cb_) {
                cb_.close_session_cb_(app_gtp_hdl, itr->second->pb_dt_.tran_addr_.context_);
            }

            #if (1 == ENABLE_ARQ)
            itr->second->arq_.PopAllPack(current_ts_us_);
            #endif

            #if (1 == ENABLE_FEC)
            itr->second->fec2_obj_.PopAllPack(current_ts_us_);
            #endif

            GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "deleted session%s:%u<--->%s:%u.\r\n",
                   itr->second->pb_dt_.self_ip_, (u32)(itr->second->pb_dt_.self_port_),
                   itr->second->pb_dt_.peer_ip_, (u32)(itr->second->pb_dt_.peer_port_));

            delete (itr->second);
            itr = session_map_.erase(itr);
            continue;
        }

        itr->second->TimerHandler(current_ts_us_, &wheel_consume_);

        #if ((_WIN32 || _WIN64) && (1 == ENABLE_UDP_LOG))
        #if (1 == ENABLE_MD_PERF_CHECK)
        gtp_send_us += itr->second->pb_dt_.send_consume_.avg_consume_time_;
        gtp_recv_us += itr->second->pb_dt_.recv_consume_.avg_consume_time_;
        app_send_us += itr->second->pb_dt_.app_send_consume_.avg_consume_time_;
        app_recv_us += itr->second->pb_dt_.app_recv_consume_.avg_consume_time_;

        session_num += 1;
        #endif
        #endif

wheel_check_resource_next_pos_:
        ++itr;
    }

    if (0 == session_map_.size()) {
        unordered_map<GtpSessionKey, GtpSession*, GtpSessionKeyHash> tmp;
        session_map_.swap(tmp);
        session_map_.reserve(MAX_SESSION_NUM);
    }

    #if (1 == ENABLE_MD_PERF_CHECK)
    u64 tmp_us = GtpSysTimestampUs();
    tmp_us -= wheel_consume_.ts_us_;
    wheel_consume_.CacheConsumeTime(tmp_us);
    #endif

    #if ((_WIN32 || _WIN64) && (1 == ENABLE_UDP_LOG))
    #if (1 == ENABLE_MD_PERF_CHECK)
    if ((second_ts_us_ + 1000000) <= wheel_consume_.ts_us_) {
    second_ts_us_ = wheel_consume_.ts_us_;
    if (0 != session_num) {
        gtp_send_us /= session_num;
        gtp_recv_us /= session_num;
        app_send_us /= session_num;
        app_recv_us /= session_num;
    }

    GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning,
           "\r\ngtp_send=%lluus\r\ngtp_recv=%lluus\r\napp_send=%lluus\r\napp_recv=%lluus\r\ngtp_wheel=%lluus\r\n",
           (unsigned long long)gtp_send_us, (unsigned long long)gtp_recv_us, (unsigned long long)app_send_us,
           (unsigned long long)app_recv_us, (unsigned long long)(wheel_consume_.avg_consume_time_));
    }
    #endif
    #endif

    return;
}

void GoodTp::SendArqCachedPackWhenDead(void) {
    current_ts_us_ = GtpSysTimestampUs();
    
    unordered_map<GtpSessionKey, GtpSession*, GtpSessionKeyHash>::iterator itr = session_map_.begin();
    while (session_map_.end() != itr) {
        #if (1 == ENABLE_TRACE_CODE_FLAG)
        GtpLog(cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelWarning, "%s:%u<->%s:%u is deaded and clear its' "\
               "resource.\r\n", itr->second->pb_dt_.self_ip_, (u32)(itr->second->pb_dt_.self_port_),
               itr->second->pb_dt_.peer_ip_, (u32)(itr->second->pb_dt_.peer_port_);
        #endif

        #if (1 == ENABLE_ARQ)
        itr->second->arq_.PopAllPack(current_ts_us_);
        #endif

        #if (1 == ENABLE_FEC)
        itr->second->fec2_obj_.PopAllPack(current_ts_us_);
        #endif

        ++itr;
    }

    return;
}

void GoodTp::DelSpsSession(GtpHandler_p app_gtp_hdl, GtpAddr *tran_addr) {
    current_ts_us_ = GtpSysTimestampUs();

    if (GTP_NO == tran_addr->enable_key_) {
        goodtp_sock sfd = (goodtp_sock)(tran_addr->sfd_);

        u8  peer_bin_ip[IPV6_BIN_IP_SIZE] = {0};
        u8  self_bin_ip[IPV6_BIN_IP_SIZE] = {0};

        u16 peer_ip_size  = 0;
        u16 peer_bin_port = 0;
        u16 self_ip_size  = 0;
        u16 self_bin_port = 0;

        GtpSockAddrToBinAddr(&(tran_addr->sock_addr_[0]), &(peer_bin_ip[0]), &peer_ip_size, &peer_bin_port);

        if (0 != tran_addr->self_addr_len_) {
            GtpSockAddrToBinAddr(&(tran_addr->self_addr_[0]), &(self_bin_ip[0]), &self_ip_size, &self_bin_port);
        } else {
            memset(self_bin_ip, 0x00, sizeof(self_bin_ip));
        }

        GtpSessionKey link_key(sfd, &(peer_bin_ip[0]), peer_ip_size, peer_bin_port, &(self_bin_ip[0]),
                               self_ip_size, self_bin_port);

        DelSpsSessionByKey(app_gtp_hdl, link_key);

        return;
    }

    {
    GtpSessionKey link_key(tran_addr->stream_key_);
    DelSpsSessionByKey(app_gtp_hdl, link_key);
    }

    return;
}

u32 GoodTp::ShowBitMap(const u8 *src_ip, const u8 *dst_ip, u8 *out_str, const u32 &mem_size) {
    u8 *wrt_pos  = out_str;
    u32 free_sz  = mem_size;
    u32 str_size = 0;
    u32 run_rslt = GTP_OK;

    unordered_map<GtpSessionKey, GtpSession*, GtpSessionKeyHash>::iterator itr = session_map_.begin();

    while (session_map_.end() != itr) {
        if ((0 == strcmp((char*)src_ip, (char*)(itr->second->pb_dt_.self_ip_)))
         && (0 == strcmp((char*)dst_ip, (char*)(itr->second->pb_dt_.peer_ip_)))) {
            run_rslt = itr->second->WinBitMap(wrt_pos, free_sz, &str_size);
            if (GTP_OK != run_rslt) {
                break;
            }

            if (str_size >= free_sz) {
                break;
            }

            wrt_pos += str_size;
            free_sz -= str_size;
            str_size = 0;

            str_size = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n\n");
            if (str_size >= free_sz) {
                run_rslt = GEN_ERR(kGtpSessionMd, kMemNotEnoughErr);
                break;
            }

            wrt_pos += str_size;
            free_sz -= str_size;
            str_size = 0;
        }

        ++itr;
    }

    return run_rslt;
}

u32 GoodTp::ShowLinker(const u8 *matched_str, u8 *out_str, const u32 mem_size) {
    u8 *wrt_pos  = out_str;
    u32 free_sz  = mem_size;
    u32 str_size = 0;
    u32 total_sz = 0;

    unordered_map<GtpSessionKey, GtpSession*, GtpSessionKeyHash>::iterator itr = session_map_.begin();

    while (session_map_.end() != itr) {
        str_size = (u32)snprintf((char*)wrt_pos, free_sz, "%s:%u-->%s:%u\r\n", itr->second->pb_dt_.self_ip_,
                                 (u32)(itr->second->pb_dt_.self_port_), itr->second->pb_dt_.peer_ip_,
                                 (u32)(itr->second->pb_dt_.peer_port_));

        if (0 != *matched_str) {
            if (NULL == strstr((const char*)wrt_pos, (const char*)matched_str)) {
                *wrt_pos = 0;
                goto show_linker_loop_next_pos_;
            }
        }

        total_sz += str_size;

        if (str_size >= free_sz) {
            break;
        }

        wrt_pos += str_size;
        free_sz -= str_size;
        str_size = 0;

show_linker_loop_next_pos_:
        ++itr;
    }

    return total_sz;
}

u32 GoodTp::ShowAlgorithmParam(const u8 *src_ip, const u8 *dst_ip, u8 *out_str, const u32 &mem_size) {
    u8 *wrt_pos = out_str;
    u32 free_sz = mem_size;
    u32 wrt_num = 0;
    u32 str_len = 0;

    unordered_map<GtpSessionKey, GtpSession*, GtpSessionKeyHash>::iterator itr = session_map_.begin();

    while (session_map_.end() != itr) {
        if ((0 == strcmp((char*)src_ip, (char*)(itr->second->pb_dt_.self_ip_)))
         && (0 == strcmp((char*)dst_ip, (char*)(itr->second->pb_dt_.peer_ip_)))) {
            wrt_num = itr->second->PrintAlgorithmParam(wrt_pos, free_sz);
            PrintAfterHandlerBreak(str_len, wrt_num, free_sz, wrt_pos);

            wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n%s=%lluus\r\n\n",
                                    wheel_consume_.name_, (unsigned long long)wheel_consume_.avg_consume_time_);
            PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);
        }

        ++itr;
    }

    if (0 == str_len) {
        wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "No exist %s<-->%s session.\r\n", src_ip, dst_ip);
        PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);
    }

    return str_len;
}

u32 GoodTp::SessionNumber(void) {
    return (u32)(session_map_.size());
}

void GoodTp::GetTranMemPoolStatus(u8 mem[], const u32 &mem_size) {
    const string &info = packet_mem_pool_.TranMemPoolStatInfo();

    u32 writed_sum = 0;
    u32 writed_num = snprintf((char*)mem, mem_size, "pack memory pool(3rd_malloc=%u 3rd_free=%u):\r\n%s",
                              m_3rd_malloc_num_, m_3rd_free_num_, info.c_str());

    writed_sum += writed_num;
    writed_num  = snprintf((char*)(&mem[writed_sum]), mem_size - writed_sum,
                           "\r\narq memory pool: %s", arq_node_mem_pool_.GetMemPoolStatus());

    writed_sum += writed_num;
    writed_num  = snprintf((char*)(&mem[writed_sum]), mem_size - writed_sum,
                           "\r\nsession memory pool: %s", session_mem_pool_.GetMemPoolStatus());

    u32 harq_num  = 0;

    unordered_map<GtpSessionKey, GtpSession*, GtpSessionKeyHash>::iterator itr = session_map_.begin();
    while (session_map_.end() != itr) {
        harq_num  += itr->second->arq_.arq_list_.node_num_;

        ++itr;
    }

    writed_sum += writed_num;
    writed_num  = snprintf((char*)(&mem[writed_sum]), mem_size - writed_sum,
                           "\r\nharq_list_num:%u", harq_num);
    return;
}

u32 GtpSendPackCallBack(GtpHandler_p gtp_hdl, void *pack, u32 size, GtpAddr *tran_addr) {
    GoodTp *goodtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));

    GtpPacket *goodtp_pack = (GtpPacket*)pack;
    GtpSession *session   = NULL;

    session = goodtp_obj->GetSession(tran_addr, gtp_hdl);
    if (NULL == session) {
        RETURN_ERR(kGtpInterfaceMd, kGetSessionFailed);
    }

    tran_addr->timestamp_ = goodtp_obj->current_ts_us_;

    u32 nret = GTP_OK;

    GtpHeaderNewToOld(pack, session->pb_dt_.peer_version_);

    #if (1 == ENABLE_MD_PERF_CHECK)
    {
    u64 app_consume_us = GtpSysTimestampUs();
    nret = session->cb_.send_pack_cb_(gtp_hdl, pack, size, tran_addr);
    app_consume_us = GtpSysTimestampUs() - app_consume_us;
    session->pb_dt_.app_send_consume_.CacheConsumeTime(app_consume_us);
    session->pb_dt_.send_consume_.ts_us_ += app_consume_us;
    session->pb_dt_.recv_consume_.ts_us_ += app_consume_us;
    }
    #else
    nret = session->cb_.send_pack_cb_(gtp_hdl, pack, size, tran_addr);
    #endif

    session->AddNetStat(goodtp_pack, size);

    return nret;
}

void LogBinaryData(const u8 *bin_data, const u32 &size, u8 *log_buf, const u32 &buf_size) {
    i32 free_size = (i32)buf_size;
    i32 nret      = 0;
    u8* buf_pos   = log_buf;

    u32 i = 0;
    while (size > i) {
        nret = snprintf((char*)buf_pos, free_size, "%02x ", bin_data[i]);
        if (0 > nret) {
            break;
        }

        i         += 1;
        buf_pos   += nret;
        free_size -= nret;
    }

    return;
}

#ifdef _UTTEST
void UtLog(const u32 &log_level, const char *fmt, ...) {
    return;
}
#endif

#ifdef __cplusplus
}
#endif

