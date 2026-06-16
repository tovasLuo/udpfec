#ifndef _GOOD_TP_H_
#define _GOOD_TP_H_
/*********************************************************************************************************************

                             Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp.h
  Version  : Initial
  Author   : Albort.Feng
  Function : Define the goodtp platform interface.
  Modify record:
  1.Date   : June 04, 2024
    Author : Albort.Feng
    Content: New creates file.

*********************************************************************************************************************/

#if (__linux__ || __APPLE__)
#include <netdb.h>
#include <arpa/inet.h>
#ifdef __linux__
#include <linux/tcp.h>
#endif
#ifdef __APPLE__
#include <sys/socket.h>
#endif
#endif

#if (_WIN32 || _WIN64)
#include <WS2tcpip.h>
#include <WinSock2.h>
#include <Windows.h>
#endif

#include <stdint.h>

#if (_WIN32 || _WIN64)
#if defined(GOODTP_STATIC)
#define GOODTP_API
#elif defined(GOODTP_BUILDING_DLL)
#define GOODTP_API __declspec(dllexport)
#else
#define GOODTP_API __declspec(dllimport)
#endif
#else
#define GOODTP_API
#endif

typedef int32_t GtpHandler;
typedef void*   GtpHandler_p;

#define GTP_OK              (0)
#define GTP_ERR             (-1)

#define GTP_INVALID         (-1)

#define GTP_YES             (1)
#define GTP_NO              (0)

#define GTP_ON              (1)
#define GTP_OFF             (0)

#define GTP_MAX_STR_IP_SZ   (64)
#define GTP_MAX_BIN_IP_SZ   (16)

#define INVALID_GTP_HANDLER    (((GtpHandler_p)GTP_ERR))

typedef enum _GtpModuleIdEn {
    kGtpBaseMd      = 0x50,
    kGtpMgrMd       = 0x51,
    kGtpMemPoolMd   = 0x52,
    kGtpSessionMd   = 0x53,
    kGtpInterfaceMd = 0x54,
    kGtpArqMd       = 0x55,
    kGtpFecMd       = 0x56,
    kGtpSortMd      = 0x57
}GtpModuleIdEn;
typedef uint32_t GtpModuleIdEnU32;

typedef enum _GtpErrorCodeEn {
    kGtpErrorCodeBase           = 0x00006000,
    kDistributeMemFail          = 0x00006001,
    kCrtedMaxSpecNum            = 0x00006002,
    kInvalidGtpHandler          = 0x00006003,
    kInvalidInPutParam          = 0x00006004,
    kGetSockAddrFailed          = 0x00006005,
    kSockAddrToStrFailed        = 0x00006006,
    kCrtArqNodePoolFailed       = 0x00006007,
    kCrtArqSnPoolFailed         = 0x00006008,
    kCrtSessionPoolFailed       = 0x00006009,
    kCrtTranMemPoolFailed       = 0x0000600a,
    kCrtSlidWinFailed           = 0x0000600b,
    kGetSessionFailed           = 0x0000600c,
    kGtpPackLengthErr           = 0x0000600d,
    kGtpPackHeaderErr           = 0x0000600e,
    kGtpPackHeaderLenErr        = 0x0000600f,
    kGtpPackHeaderPaddingErr    = 0x00006010,
    kGtpPackHeaderPaddingLenErr = 0x00006011,
    kGtpArqInitFailed           = 0x00006012,
    kCrtArqNodeFailed           = 0x00006013,
    kUnknownPackTypeErr         = 0x00006014,
    kUnknownGtpVerErr           = 0x00006015,
    kDuplicatePackErr           = 0x00006016,
    kLinkerNoExistErr           = 0x00006017,
    kGtpVerErr                  = 0x00006018,
    kMemNotEnoughErr            = 0x00006019,
    kGtpFecInitFailed           = 0x0000601a,
    kFecInvalidCfg              = 0x0000601b,
    kPackMemIsInvalid           = 0x0000601c,
    kMallocMemPoolItemFailedErr = 0x0000601d,
    kInvalidFecCodeBookId       = 0x0000601e,
    kFecRestoreFailedErr        = 0x0000601f,
    kMallocPackMemFailed        = 0x00006020,
    kGtpFec2InitFailed          = 0x00006021,
    kGetFec2ParameterFailed     = 0x00006022,
    kCachePackFailedErr         = 0x00006023,
    kReliableStreamNoSortSnErr  = 0x00006024,
    kReliablePackTooLateErr     = 0x00006025,
    kAppMixThreadErr            = 0x00006026,
    kPeerVerTooOldErr           = 0x00006027,
    kGtpTranAddrNullErr         = 0x00006028,
    kGtpTranAddrDstLenErr       = 0x00006029,
    kGtpTranAddrSrcLenErr       = 0x0000602a,
    kGtpTranAddrDstMemErr       = 0x0000602b,
    kGtpTranAddrSrcMemErr       = 0x0000602c,
    kGtpPackMemOutBoundryErr    = 0x0000602d
}GtpErrorCodeEn;
typedef uint32_t GtpErrorCodeEnU32;

typedef enum _GtpLossDirectEn {
    kUplinkLoss   = 0,
    kDownLinkLoss = 1,

    kLossButt
}GtpLossDirectEn;
typedef uint32_t GtpLossDirectEnU32;

typedef enum _GtpNetQualityPosEn {
    kSenderQuality   = 0,
    kReceiverQuality = 1,

    kQualityPosButt
}GtpNetQualityPosEn;
typedef uint32_t GtpNetQualityPosEnU32;

typedef enum _GtpLogLevelEn {
    kGtpLogLevelEmerg   = 0x00,
    kGtpLogLevelAlert   = 0x01,
    kGtpLogLevelCrit    = 0x02,
    kGtpLogLevelError   = 0x03,
    kGtpLogLevelWarning = 0x04,
    kGtpLogLevelNotice  = 0x05,
    kGtpLogLevelInfo    = 0x06,
    kGtpLogLevelDebug   = 0x07,

    kGtpLogLevelButt
}GtpLogLevelEn;
typedef uint32_t GtpLogLevelEnU32;

typedef enum _GtpPreCongestRankEn {
    kGtpNoGenCongest      = 0,
    kGtpMaybeGenCongest   = 1,
    kGtpWillBeGenCongest  = 2,
    kGtpMustBeGenCongest  = 3,
    kGtpUnknownGenCongest = 4,

    kGtpPreCongestRankButt
}GtpPreCongestRankEn;
typedef uint32_t GtpPreCongestRankEnU32;

typedef enum _GtpIpFamilyEn {
    kGtpIpv4 = 0,
    kGtpIpv6 = 1,

    kGtpIpFamilyButt
}GtpIpFamilyEn;
typedef uint32_t GtpIpFamilyEnU32;

typedef enum _PackMemSpec {
    kMemSpec256Bytes = 0,
    kMemSpec512Bytes = 1,
    kMemSpec1k       = 2,
    kMemSpec1Dot5k   = 3,
    kMemSpec4k       = 4,
    kMemSpec8k       = 5,
    kMemSpec64k      = 6,

    kPackMemSpecButt
}PackMemSpec;
typedef uint32_t PackMemSpecU32;

typedef enum _GtpStreamType {
    kRealTimeStream = 0,
    kReliableStream = 1,

    kGtpStreamTypeButt
}GtpStreamType;
typedef uint8_t GtpStreamTypeU8;

#define GtpPackSizeToMemSpec(size) ((256  > (size)) ? kMemSpec256Bytes :\
                                   ((512  > (size)) ? kMemSpec512Bytes :\
                                   ((1024 > (size)) ? kMemSpec1k :\
                                   ((1536 > (size)) ? kMemSpec1Dot5k :\
                                   ((4096 > (size)) ? kMemSpec4k :\
                                   ((8192 > (size)) ? kMemSpec8k : kMemSpec64k))))))

#ifdef __linux__
#ifdef __i386__
#define GTP_SOCK_ADDR_SZ (((sizeof(struct sockaddr_in6) & 0xFFFFFFFC) + 4))
#else
#define GTP_SOCK_ADDR_SZ (((sizeof(struct sockaddr_in6) & 0xFFFFFFF8) + 8))
#endif
#endif

#ifdef __APPLE__
#define GTP_SOCK_ADDR_SZ (((sizeof(struct sockaddr_in6) & 0xFFFFFFF8) + 8))
#endif

#if (_WIN32 || _WIN64)
#ifdef _WIN64
#define GTP_SOCK_ADDR_SZ (((sizeof(struct sockaddr_in6) & 0xFFFFFFF8) + 8))
#else
#define GTP_SOCK_ADDR_SZ (((sizeof(struct sockaddr_in6) & 0xFFFFFFFC) + 4))
#endif
#endif

#pragma pack(1)
typedef struct _GtpLinkerKey {
    uint32_t sfd_;                                 // [must] send socket handler.
    uint32_t direction_;                           // [must] 1: sender, 2: receiver.
    uint32_t peer_socket_addr_len_;                // [must] peer_socket_addr_'s byte length.
    uint32_t self_socket_addr_len_;                // [must] self_socket_addr_'s byte length, when don't use
                                                   // sendmsg() and recvmsg() for sfd_, it must be setted to 0.
    uint8_t  peer_socket_addr_[GTP_SOCK_ADDR_SZ];  // [must] sfd_'s peer socket address.
    uint8_t  self_socket_addr_[GTP_SOCK_ADDR_SZ];  // [option] sfd_'s self socket address for using sendmsg() and
                                                   // recvmsg().
}GtpLinkerKey;

typedef struct _GtpLinkQuality {
    uint8_t  self_bin_ip_[GTP_MAX_BIN_IP_SZ];
    uint8_t  peer_bin_ip_[GTP_MAX_BIN_IP_SZ];

    uint16_t self_bin_port_;     // network sequence
    uint16_t peer_bin_port_;     // network sequence

    uint8_t  self_ip_family_;    // 0:ipv4, 1:ipv6(GtpIpFamilyEn)
    uint8_t  peer_ip_family_;    // 0:ipv4, 1:ipv6(GtpIpFamilyEn)
    uint8_t  report_pos_;        // GtpNetQualityPosEn
    uint8_t  rsv_;

    const uint8_t *self_ip_;
    const uint8_t *peer_ip_;

    uint16_t self_port_;         // host sequence
    uint16_t peer_port_;         // host sequence

    uint16_t rtt_ms_;
    uint16_t rtt_jitter_ms_;

    uint32_t data_bitrate_bps_;  // It doesn't include fec packets, only includes data pakcets.
    uint32_t net_bitrate_bps_;   // It includes fec restoring packets.
    uint32_t data_pack_pps_;     // It doesn't include fec packets, only includes data packets.
    uint32_t net_pack_pps_;      // It includes fec restoring packets.

    uint32_t total_frame_num_;
    uint32_t total_pack_num_;
    uint32_t total_ack_num_;
    uint32_t total_retran_num_;
    uint32_t total_ack_loss_num_;
    uint32_t total_rto_loss_num_;
    uint32_t total_ack_err_num_;
    uint32_t pre_congest_rank_;  // GtpPreCongestRankU32

    void *context_;

    GtpLinkerKey linker_key_;    // when linker exist loss, it can obtain slid window's bitmap easily.

    uint32_t total_ai_repair_num_;
    uint32_t loss_direct_;       // GtpLossDirectEnU32
    float    loss_;
    float    bitrage_chg_k_;
}GtpLinkQuality;

typedef struct _GtpAddr {
    void *context_;                         // [option]the running application env, it can be used to transmit
                                            // application' parameters.

    uint64_t timestamp_;                    // [option]  don't care if no using.
    uint64_t stream_key_;                   // [option]  it isn't same for different stream, don't care if no using.
    uint8_t  loss_;                         // [option]  don't care if no using.
    uint8_t  qos_;                          // [option]  don't care if no using.
    uint16_t stream_type_:1;                // [must] 0: real time transport(udp).
                                            //        1: reliable transport(tcp).

    uint16_t smooth_jitter_:1;              // [must] 0: don't remove jitter for reliable stream.
                                            //        1: remove jitter for reliable stream.

    uint16_t enable_key_:1;                 // [must] 0: don't use the stream_key_.
                                            //        1: using the stream_key_ to identify a stream session.

    uint16_t bit_rsv_:13;
    uint32_t sfd_;                          // [must] send or receive socket handler.

    uint32_t self_addr_len_;                // [must] the self_sock_addr_'s byte length, when don't use sendmsg()
                                            // and recvmsg() the self_sock_addr_ must be setted to 0.
    uint32_t sock_addr_len_;                // [must] the sock_addr_'s byte length.
    uint8_t  sock_addr_[GTP_SOCK_ADDR_SZ];  // [must] peer socket address.
    uint8_t  self_addr_[GTP_SOCK_ADDR_SZ];  // [option] sfd_'s self socket address for using sendmsg() and recvmsg().
}GtpAddr;

/* the goodtp supports 256bytes 512bytes 1k 1.5k, 4k, 8k and 64k packet memory cluster */
typedef struct _MemPoolConfig {
    uint32_t m_256bytes_num_;  // 256 bytes packet memory cluster item number.
    uint32_t m_512bytes_num_;  // 512 bytes packet memory cluster item number.
    uint32_t m_1k_num_;        // 1k packet memory cluster item number.
    uint32_t m_1_5k_num_;      // 1.5k packet memory cluster item number.
    uint32_t m_4k_num_;        // 4k packet memory cluster item number.
    uint32_t m_8k_num_;        // 8k packet memory cluster item number.
    uint32_t m_64k_num_;       // 64k packet memory cluster item number.
    uint32_t arq_node_num_;    // 0: use library default ARQ node number.
    uint32_t session_num_;     // 0: use library default session number.
}MemPoolConfig;

#pragma pack()

typedef uint32_t (*pSendPackCallBack)(GtpHandler_p gtp_hdl, void *pack, uint32_t size, GtpAddr *tran_addr);
typedef uint32_t (*pReceivFrameCallBack)(GtpHandler_p gtp_hdl, void *frame, uint32_t size, GtpAddr *tran_addr);
typedef uint32_t (*pReportLinkQualityCallBack)(GtpHandler_p gtp_hdl, GtpLinkQuality vec[], uint32_t size);
typedef void (*pLogCallBack)(uint32_t log_level, const char *fmt, ...);
typedef uint32_t (*pLogLevelCallBack)(void);
typedef void (*pCloseSessionCallBack)(GtpHandler_p gtp_hdl, void *context);

#pragma pack(1)

typedef struct _GtpCallBackParam {
    pSendPackCallBack send_pack_cb_;         // [must] mustn't be NULL, the goodtp calls this interface to complete
                                             // sending packet.

    pReceivFrameCallBack receive_frame_cb_;  // [must] mustn't be NULL, the goodtp calls this interface to hand in the
                                             // application data coming from network.

    pReportLinkQualityCallBack report_link_quality_cb_;  // [option] if it isn't NULL, the goodtp calls this interface
                                                         // to report current linker's network quality, while it is
                                                         // usually quick and right.

    pLogCallBack write_log_cb_;             // [option] when it's NULL, the goodtp works quietly, while don't recommend.
    pLogLevelCallBack cur_log_level_cb_;    // [option] when log level >= the current process's log level, the goodtp
                                            // calls the write_log_cb_ to output itself logs.

    pCloseSessionCallBack close_session_cb_;
}GtpCallBackParam;

#pragma pack()

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************************************************
Name     : InsLoadGtpModule
Function : When boot the current system, it must be insload the goodtp module only one before calling any the others
           goodtp interface, calling this function but it don't create any goodtp transporing instance.
In param : void
Out param: void
Return   : u32    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t InsLoadGtpModule(void);

/*****************************************************************************************************************
Name     : RmLoadGtpModule
Function : When poweroff the current system, it must remove load goodtp module only one, calling this function it
           delete all the existing goodtp instances automatically.
In param : void
Out param: void
Return   : u32    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t RmLoadGtpModule(void);

/*****************************************************************************************************************
Name     : CreateGtpInstance
Function : creates a goodtp instance, while a single goodtp can't support multi thread using.
In param : uint32_t system_id
           uint32_t session_ttl_us
           GtpCallBackParam *reg_cb
           MemPoolConfig *mem_pool_cfg
Out param: void
Return   : GtpHandler_p // INVALID_GTP_HANDLER: failed and call LastGtpErrorInfo() to obtain the detailed information,
                        // the others: sucess,it's the current goodtp instance handler.

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API GtpHandler_p CreateGtpInstance(uint32_t system_id, uint32_t session_ttl_us, GtpCallBackParam *reg_cb,
                                          MemPoolConfig *mem_pool_cfg);

/*****************************************************************************************************************
Name     : DeleteGtpInstance
Function : Deletes a goodtp instance, it's a secure function.
In param : GtpHandler_p gtp_hdl
Out param: void
Return   : u32    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t DeleteGtpInstance(GtpHandler_p gtp_hdl);

/*****************************************************************************************************************
Name     : GtpMallocPackMem
Function : Malloc the receiving packet memory to copy zero address during the harq and the fec.
In param : GtpHandler_p gtp_hdl
           uint8_t *old_pack_mem   // single memory support receiving multi packets when it's length is enough,
                                   // it's NULL when malloc a new memory.
           uint32_t used_size      // has been used memory length when receiving multi packet in single memory,
                                   // it's zero when malloc a new memory.
           uint32_t pack_len       // the packet's byte number(max 64k).
Out param: uint32_t *new_mem_usable_size
           void **tran_addr_mem
Return   : uint8_t*    // NULL: failed, the others: sucess(can be used memory's address)

Mdf history  :
1.Date       : 2024.06.04
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint8_t *GtpMallocPackMem(GtpHandler_p gtp_hdl, uint8_t* old_pack_mem, uint32_t used_size,
                                     uint32_t *new_mem_usable_size, void **tran_addr_mem, uint32_t pack_len);

/*****************************************************************************************************************
Name     : GtpFreePackMem
Function : Free the packet memory by calling GtpMallocPackMem() malloced memory, it must be called pairly with
           GtpMallocPackMem().
In param : GtpHandler_p gtp_hdl
           uint8_t *pack_mem  // the memory must be malloced by GtpMallocPackMem(), while it support any address in
                              // the packet memory's valid range.
Out param: void
Return   : void

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API void GtpFreePackMem(GtpHandler_p gtp_hdl, uint8_t *pack_mem);

/*****************************************************************************************************************
Name     : GtpFrameSend
Function : send frame interface provided by goodtp, goodtp will add itself packet header then call the registered
           send_pack_cb_ to send packet to internet.
In param : GtpHandler_p gtp_hdl
           void *frame    // there are at least 64 bytes memory to use for goodtp.
           uint32_t size  // it's only the frame data bytes number.
           GtpAddr *tran_addr
           uint32_t token
           uint32_t token_id
Out param: void
Return   : uint32_t  // GTP_OK: sucess, the others: failed and call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t GtpFrameSend(GtpHandler_p gtp_hdl, void *frame, uint32_t size, GtpAddr *tran_addr,
                                 uint32_t token, uint32_t token_id);

/*****************************************************************************************************************
Name     : GtpCheckPacketInvalid
Function : check goodtp packet's validity.
In param : GtpHandler_p gtp_hdl
           void *pack
           uint32_t pack_size
Out param: void
Return   : uint32_t    // GTP_OK: packet is valid, the others: packet is invalid.

Mdf history  :
1.Date       : 2024.07.23
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t GtpCheckPacketInvalid(void *pack, uint32_t pack_size, uint64_t *stream_key
#ifdef __cplusplus
                               = 0
#endif
);

/*****************************************************************************************************************
Name     : GtpPacketReceive
Function : receive packet interface provided by goodtp, goodtp will remove itself packet header then call the registered
           receive_frame_cb_ to hand out frame to application.
In param : GtpHandler_p gtp_hdl
           void *pack      // there are at least 64 bytes memory to use for goodtp.
           uint32_t size   // it's only the frame data bytes number.
           GtpAddr *tran_addr
Out param: void
Return   : u32    // GTP_OK: sucess, the others: failed and call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t GtpPacketReceive(GtpHandler_p gtp_hdl, void *pack, uint32_t size, GtpAddr *tran_addr);

/*****************************************************************************************************************
Name     : PeriodGtpTimer
Function : Application system shall call this interface periodically.
In param : GtpHandler_p gtp_hdl
Out param: void
Return   : uint32_t    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t PeriodGtpTimer(GtpHandler_p gtp_hdl);

/*****************************************************************************************************************
Name     : GetLinkerQuality
Function : Get the special linker network quality by handler.
In param : GtpHandler_p gtp_hdl
           GtpLinkerKey *linker_key
Out param: GtpLinkQuality *out_quality
Return   : u32    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t GetLinkerQuality(GtpHandler_p gtp_hdl, GtpLinkerKey *linker_key, GtpLinkQuality *out_quality);

/*****************************************************************************************************************
Name     : LastGtpErrorInfo
Function : Get goodtp module last commont error information when creating goodtp instance faile, when call c3tpt
           interface failedly, it shall be call this function to get last error information, otherwise this error
           information maybe loss, and a same error information can get only one.
In param : void
Out param: void
Return   : const char*

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API const char* LastGtpErrorInfo(void);

/*****************************************************************************************************************
Name     : GtpAddrToHostAddr
Function : Converts goodtp address to host address and port.
In param : GtpAddr *goodtp_addr
Out param: uint8_t out_self_ip[]    // memory's size is 64 bytes at least.
           uint8_t out_peer_ip[]    // memory's size is 64 bytes at least.
           uint16_t *out_self_port
           uint16_t *out_peer_port
Return   : uint32_t  // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t GtpAddrToHostAddr(GtpAddr *goodtp_addr, uint8_t out_self_ip[], uint8_t out_peer_ip[],
                                      uint16_t *out_self_port, uint16_t *out_peer_port);

/*****************************************************************************************************************
Name     : GetGtpVersion
Function : Get GoodTp libary's version.
In param : uint8_t *pout_ver   // this memory must be 256 bytes at least.
Out param: uint8_t *pout_ver
Return   : uint8_t*

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint8_t* GetGtpVersion(uint8_t *pout_ver);

/*****************************************************************************************************************
Name     : GetGtpPureVer
Function : Get GoodTp libary's pure version.
In param : uint8_t *pout_ver   // this memory must be 256 bytes at least.
Out param: uint8_t *pout_ver
Return   : uint8_t*

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint8_t* GetGtpPureVer(uint8_t *pout_ver);

/*****************************************************************************************************************
Name     : DelGtpLinker
Function : Delete goodtp linker before not using.
In param : GtpHandler_p gtp_hdl
           GtpAddr *tran_addr
Out param: void
Return   : void

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API void DelGtpLinker(GtpHandler_p gtp_hdl, GtpAddr *tran_addr);

/*****************************************************************************************************************
Name     : PrintSessionWinBitMap
Function : Print session's slid window bitmap.
In param : GtpHandler_p gtp_hdl
           const uint8_t *src_ip
           const uint8_t *dst_ip
           uint8_t *out_str
           uint32_t mem_size
Out param: uint8_t *out_str
Return   : uint32_t    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t PrintSessionWinBitMap(GtpHandler_p gtp_hdl, const uint8_t *src_ip, const uint8_t *dst_ip,
                                          uint8_t *out_str, uint32_t mem_size);

/*****************************************************************************************************************
Name     : GetAlgorithmParam
Function : Get the goodtp algorithm parameters.
In param : GtpHandler_p gtp_hdl
           const uint8_t *src_ip
           const uint8_t *dst_ip
           uint8_t *out_str
           uint32_t mem_size
Out param: uint8_t *out_str
Return   : uint32_t    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.09.12
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t GetAlgorithmParam(GtpHandler_p gtp_hdl, const uint8_t *src_ip, const uint8_t *dst_ip,
                                      uint8_t *out_str, uint32_t mem_size);

/*****************************************************************************************************************
Name     : ShowTotalLinker
Function : show all the linker information.
In param : GtpHandler_p gtp_hdl
           const uint8_t *matched_str     // matched string ip, it will show all linker when it's empty.
           uint8_t *out_str
           uint32_t mem_size
Out param: uint8_t *out_str
Return   : uint32_t    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.06.05
  Author     : Albert.Feng
  Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t ShowTotalLinker(GtpHandler_p gtp_hdl, const uint8_t *matched_str, uint8_t *out_str,
                                    uint32_t mem_size);

/*****************************************************************************************************************
Name     : GetSlidWinBitMapInfo
Function : Get specific linker slid window's bitmap information.
In param : GtpHandler_p gtp_hdl
           GtpLinkerKey *linker_key
           uint8_t *out_str
           uint32_t mem_size
Out param: uint8_t *out_str
Return   : uint32_t    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.06.16
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t GetSlidWinBitMapInfo(GtpHandler_p gtp_hdl, GtpLinkerKey *linker_key, uint8_t *out_str,
                                         uint32_t mem_size);

/*****************************************************************************************************************
Name     : GetPackMemPoolStatus
Function : Get the packet memory pool current status.
In param : GtpHandler_p gtp_hdl
           uint8_t out_status[]   // at least 4096 bytes.
           uint32_t mem_size      // out_status's bytes size.
Out param: uint8_t out_status[]   // memory pool status string
Return   : uint32_t    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.06.28
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t GetPackMemPoolStatus(GtpHandler_p gtp_hdl, uint8_t out_status[], uint32_t mem_size);

/*****************************************************************************************************************
Name     : GetSessionNumber
Function : Get the goodtp's existing session number.
In param : GtpHandler_p gtp_hdl
Out param: void
Return   : uint32_t  // current existing session number.

Mdf history  :
1.Date       : 2024.07.15
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
GOODTP_API uint32_t GetSessionNumber(GtpHandler_p gtp_hdl);

/*****************************************************************************************************************
Name     : GtpGetStackFreeSize
Function : Get current thread stack free size.
In param : uint32_t *stack_size  // save stack size memory.
Out param: uint32_t *stack_size  // stack size(unit: bytes)
Return   : int64_t  // current thread stack free size.

Mdf history  :
1.Date       : 2024.11.29
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
GOODTP_API int64_t GtpGetStackFreeSize(uint32_t *stack_size);

#ifdef __cplusplus
}
#endif

#endif
