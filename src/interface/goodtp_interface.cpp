/*********************************************************************************************************************

                           Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_interface.cpp
  Version  : Initial
  Author   : Albort.Feng
  Function : Realizes the goodtp external interface code file.
  Modify record:
  1.Date   : August 25, 2023
    Author : Albort.Feng
    Content: New creates file.

*********************************************************************************************************************/

#include "goodtp_interface.h"
#include "goodtp_mgr.h"
#include "goodtp_comstruct.h"
#include "goodtp_macrodefine.h"
#include "bitlinker.h"

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************************************************
Name     : GtpMallocPackMem
Function : Malloc the receiving packet memory to copy zero address during the harq and the fec.
In param : GtpHandler_p gtp_hdl
           u8 *old_pack_mem    // single memory support receiving multi packets when it's length is enough,
                               // it's NULL when malloc a new memory.
           u32 used_size       // has been used memory length when receiving multi packet in single memory,
                               // it's zero when malloc a new memory.
           u32 pack_len        // the packet's byte number(max 64k).
Out param: u32 *new_mem_usable_size
           void **tran_addr_mem
Return   : u8*    // NULL: failed, the others: sucess(can be used memory's address)

Mdf history  :
1.Date       : 2024.06.04
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u8 *GtpMallocPackMem(GtpHandler_p gtp_hdl, u8* old_pack_mem, u32 used_size, u32 *new_mem_usable_size,
                     void **tran_addr_mem, u32 pack_len) {
    if ((NULL == new_mem_usable_size) || (0 == tran_addr_mem) || ((1024 << 6) <= pack_len)) {
        return NULL;
    }

    GoodTp *gtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));
    if (NULL == gtp_obj) {
        return NULL;
    }

    if (GTP_OK != gtp_obj->CheckSingleThreadCalling()) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "The same goodtp instance used by "\
               "the diffrent thread.\r\n");
    }

    u8 *new_mem  = NULL;
    u32 mem_spec = GtpPackSizeToMemSpec(pack_len);

    new_mem = gtp_obj->packet_mem_pool_.MallocTranBuf(old_pack_mem, used_size, new_mem_usable_size,
                                                   tran_addr_mem, (BufSizeType)mem_spec);

    if (NULL != new_mem) {
        gtp_obj->m_3rd_malloc_num_ += 1;
    }

    return new_mem;
}

/*****************************************************************************************************************
Name     : GtpFreePackMem
Function : Free the packet memory by calling GtpMallocPackMem() malloced memory.
In param : GtpHandler_p gtp_hdl
           u8 *pack_mem    // the memory must be malloced by GtpMallocPackMem(), while it support any address in
                           // the packet memory's valid range.
Out param: void
Return   : void

Mdf history  :
1.Date       : 2024.06.04
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
void GtpFreePackMem(GtpHandler_p gtp_hdl, u8 *pack_mem) {
    if (NULL == pack_mem) {
        return;
    }

    GoodTp *gtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));
    if (NULL == gtp_obj) {
        return;
    }

    if (GTP_OK != gtp_obj->CheckSingleThreadCalling()) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "The same goodtp instance used by "\
               "the diffrent thread.\r\n");
    }

    #if ((_WIN32 || _WIN64) && (1 == ENABLE_INPUT_PARAM_CHCK))
    if (GTP_OK != gtp_obj->packet_mem_pool_.CheckAddrIsValid(pack_mem)) {
        const std::string &err_info = gtp_obj->packet_mem_pool_.Error();
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "%s pid=%d tid=%d\r\n",
               err_info.c_str(), (i32)GtpGetProcessId(), (i32)GtpGetThreadId());
        return;
    }
    #endif

    gtp_obj->packet_mem_pool_.FreeTranBuf(pack_mem, gtp_obj->cb_.write_log_cb_);

    gtp_obj->m_3rd_free_num_ += 1;

    return;
}

/*****************************************************************************************************************
Name     : GtpFrameSend
Function : send frame interface provided by goodtp, goodtp will add itself packet header then call the registered
           send_pack_cb_ to send packet to internet.
In param : GtpHandler_p gtp_hdl
           void *frame    // there are at least 64 bytes memory to use for goodtp.
           u32 size       // it's only the frame data bytes number.
           GtpAddr *tran_addr
           u32 token
           u32 token_id
Out param: void
Return   : u32    // GTP_OK: sucess, the others: failed and call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2023.08.25
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 GtpFrameSend(GtpHandler_p gtp_hdl, void *frame, u32 size, GtpAddr *tran_addr, u32 token, u32 token_id) {
    if ((NULL == frame) || (0 == size) || (NULL == tran_addr)) {
        RETURN_ERR(kGtpInterfaceMd, kInvalidInPutParam);
    }

    GoodTp *gtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));
    if (NULL == gtp_obj) {
        RETURN_ERR(kGtpInterfaceMd, kInvalidGtpHandler);
    }

    CheckThreadStackFreeSize(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd);

    u32 nret = GTP_OK;

    #if (1 == ENABLE_TRAN_MEM_CHECK)
    nret = GtpTranAddrIsValid(tran_addr);
    if (GTP_OK != nret) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "Goodtp transport address "\
               "memory is invalid(0x%08x tran_addr=%p dst_addr_len=%u src_addr_len=%u dst_mem_addr=%p "\
               "src_mem_addr=%p).\r\n", nret, tran_addr,
               ((NULL != tran_addr) ? tran_addr->sock_addr_len_ : 0),
               ((NULL != tran_addr) ? tran_addr->self_addr_len_ : 0),
               ((NULL != tran_addr) ? tran_addr->sock_addr_ : NULL),
               ((NULL != tran_addr) ? tran_addr->self_addr_ : NULL));
         return nret;
    }
    #endif

    tran_addr->stream_type_   = 0;

    if (GTP_OK != gtp_obj->CheckSingleThreadCalling()) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "The same goodtp instance used by "\
               "the diffrent thread.\r\n");
    }

    #if ((_WIN32 || _WIN64) && (1 == ENABLE_INPUT_PARAM_CHCK))
    if ((GTP_OK != gtp_obj->packet_mem_pool_.CheckAddrIsValid((u8*)frame))
     || (GTP_OK != gtp_obj->packet_mem_pool_.CheckAddrIsValid((u8*)tran_addr))) {
        const std::string &err_info = gtp_obj->packet_mem_pool_.Error();
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "packet's memory isn't belong to the "\
               "memory pool%s pid=%d tid=%d\r\n", err_info.c_str(), (i32)GtpGetProcessId(), (i32)GtpGetThreadId());
        RETURN_ERR(kGtpInterfaceMd, kPackMemIsInvalid);
    }
    #endif

    gtp_obj->current_ts_us_ = GtpSysTimestampUs();

    GtpSession *session = gtp_obj->GetSession(tran_addr, gtp_hdl);
    if (NULL == session) {
        u8 self_ip[GTP_MAX_STR_IP_SZ] = {0};
        u8 peer_ip[GTP_MAX_STR_IP_SZ] = {0};

        u16 self_port = 0;
        u16 peer_port = 0;

        GtpAddrToStrIpAndPort(tran_addr, self_ip, peer_ip, &self_port, &peer_port);

        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelError, "%s:%u<-->%s:%u get session failed "\
               "for sending\r\n", self_ip, (u32)self_port, peer_ip, (u32)peer_port);

        RETURN_ERR(kGtpInterfaceMd, kGetSessionFailed);
    }

    #if (1 == ENABLE_MD_PERF_CHECK)
    session->pb_dt_.send_consume_.ts_us_ = gtp_obj->current_ts_us_;
    #endif

    #if ((_WIN32 || _WIN64) && (1 == ENABLE_INPUT_PARAM_CHCK))
    if (gtp_hdl != session->app_gtp_hdl_) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "%s:%u<-->%s:%u: the same "\
               "session belong to diffrent gtp instance(first_gtp_hdl=%p current_gtp_hdl=%p pid=%d tid=%d)\r\n",
               session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_), session->pb_dt_.peer_ip_,
               (u32)(session->pb_dt_.peer_port_), session->app_gtp_hdl_, gtp_hdl, (i32)GtpGetProcessId(),
               (i32)GtpGetThreadId());
    }
    #endif

    session->CalcMaxFramePeriod(gtp_obj->current_ts_us_);

    session->last_active_ts_us_           = gtp_obj->current_ts_us_;
    session->last_send_ts_us_             = gtp_obj->current_ts_us_;
    session->pb_dt_.tran_addr_.timestamp_ = gtp_obj->current_ts_us_;

    #if (_WIN32 || _WIN64)
    if (0 != tran_addr->self_addr_len_) {
        if (0 != memcmp(session->pb_dt_.tran_addr_.self_addr_, tran_addr->self_addr_, tran_addr->self_addr_len_)) {
            session->pb_dt_.tran_addr_.self_addr_len_ = tran_addr->self_addr_len_;
            memcpy(session->pb_dt_.tran_addr_.self_addr_, tran_addr->self_addr_, tran_addr->self_addr_len_);

            session->UpdateSelfIp(tran_addr->self_addr_, tran_addr->self_addr_len_);
        }
    }

    if (0 != memcmp(session->pb_dt_.tran_addr_.sock_addr_, tran_addr->sock_addr_, tran_addr->sock_addr_len_)) {
        session->pb_dt_.tran_addr_.sock_addr_len_ = tran_addr->sock_addr_len_;
        memcpy(session->pb_dt_.tran_addr_.sock_addr_, tran_addr->sock_addr_, tran_addr->sock_addr_len_);

        session->UpdatePeerIp(tran_addr->sock_addr_, tran_addr->sock_addr_len_);
    }

    if (session->pb_dt_.tran_addr_.sfd_ != tran_addr->sfd_) {
        session->pb_dt_.tran_addr_.sfd_ = tran_addr->sfd_;

        if (0 == tran_addr->self_addr_len_) {
            session->UpdateSelfIp(tran_addr->sfd_);
        }
    }
    #endif

    if (session->pb_dt_.tran_addr_.qos_ != tran_addr->qos_) {
        session->pb_dt_.tran_addr_.qos_ = tran_addr->qos_;
    }

    // step1: do packet header.
    GtpPacket *pack = NULL;
    u32 pack_size   = 0;
    u32 first_sn    = session->pack_sn_;

    nret = session->FramePrepHandler(frame, size, &pack, &pack_size, token, token_id, 0, 0);
    if (GTP_OK != nret) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelError, "%s:%u<-->%s:%u calling session's "\
               "FramePrepHandler() failed(0x%08x)\r\n", session->pb_dt_.self_ip_,
               (u32)(session->pb_dt_.self_port_), session->pb_dt_.peer_ip_,
               (u32)(session->pb_dt_.peer_port_), nret);
        return nret;
    }

    GtpHeaderNewToOld(pack, session->pb_dt_.peer_version_);

    // step2: calling call back function to send packet.
    #if (1 == ENABLE_MD_PERF_CHECK)
    {
    u64 app_consume_us = GtpSysTimestampUs();
    nret = gtp_obj->cb_.send_pack_cb_(gtp_hdl, pack, pack_size, tran_addr);
    app_consume_us = GtpSysTimestampUs() - app_consume_us;
    session->pb_dt_.app_send_consume_.CacheConsumeTime(app_consume_us);

    session->pb_dt_.send_consume_.ts_us_ += app_consume_us;
    session->pb_dt_.recv_consume_.ts_us_ += app_consume_us;
    }
    #else
    nret = gtp_obj->cb_.send_pack_cb_(gtp_hdl, pack, pack_size, tran_addr);
    #endif

    GtpHeaderOldToNew(pack);

    if (GTP_OK != nret) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelError, "%s:%u<-->%s:%u calling "\
               "send_pack_cb_() failed(0x%08x)\r\n", session->pb_dt_.self_ip_,
               (u32)(session->pb_dt_.self_port_), session->pb_dt_.peer_ip_,
               (u32)(session->pb_dt_.peer_port_), nret);
    }

    // step3: post handle after sending packet.
    nret = session->FramePostHandler(tran_addr, pack, pack_size, size, GTP_YES, first_sn);
    if (GTP_OK != nret) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelError, "%s:%u<-->%s:%u calling session's "\
               "FramePostHandler() failed(0x%08x)\r\n", session->pb_dt_.self_ip_,
               (u32)(session->pb_dt_.self_port_), session->pb_dt_.peer_ip_,
               (u32)(session->pb_dt_.peer_port_), nret);
        return nret;
    }

    session->pb_dt_.send_stat_.data_pack_sum_    += 1;
    session->pb_dt_.send_stat_.data_bitrate_sum_ += pack_size;

    #if (1 == ENABLE_MD_PERF_CHECK)
    u64 temp_ts_us = GtpSysTimestampUs();
    temp_ts_us -= session->pb_dt_.send_consume_.ts_us_;
    session->pb_dt_.send_consume_.CacheConsumeTime(temp_ts_us);
    #endif

    return GTP_OK;
}

inline u32 CalcHeaderOffset(const GtpPacket *pack) {
    u32 header_size = 0;

    if (GTP_YES == pack->has_loss_flag_) {
        header_size += sizeof(f32);
    }

    if (GTP_YES == pack->has_check_flag_) {
        header_size += sizeof(u64);
    }

    if (GTP_YES == pack->has_ts_flag_) {
        header_size += sizeof(u64);
    }

    if (GTP_YES == pack->has_rtt_flag_) {
        header_size += sizeof(u32);
    }

    if (0 != ((u8)(pack->repeat_counter_))) {
        header_size += sizeof(u32);
    }

    header_size += sizeof(GtpPacket);

    return header_size;
}

inline u32 InnerCheckPacketInvalid(const GtpPacket *pack, const u32 &size) {
    // basical check.
    if ((0x00 == pack->pack_type_) || (((u8)(GtpPackType::GtpPackTypeButt)) <= pack->pack_type_)) {
        RETURN_ERR(kGtpInterfaceMd, kUnknownPackTypeErr);
    }

    if ((sizeof(GtpPacket) > size) || (size != pack->pack_size_) || (sizeof(GtpPacket) > pack->header_offset_)
     || (size < pack->header_offset_)) {
        RETURN_ERR(kGtpInterfaceMd, kGtpPackLengthErr);
    }

    u32 nret          = GTP_OK;
    u32 header_offset = 0;

    // offset check: only version 0x02 is supported.
    if (0x02 != pack->goodtp_ver_) {
        return GEN_ERR(kGtpInterfaceMd, kUnknownGtpVerErr);
    }

    header_offset = CalcHeaderOffset(pack);
    if (header_offset > pack->header_offset_) {
        nret = GEN_ERR(kGtpInterfaceMd, kGtpPackHeaderErr);
    }

    return nret;
}

/*****************************************************************************************************************
Name     : GtpCheckPacketInvalid
Function : check goodtp packet's validity.
In param : void *pack
           u32 pack_size
           u64 *stream_key  // 0: stream_key unavailable, others: session's stream key.
                            // can be NULL if not needed.
Out param: void
Return   : u32    // GTP_OK: packet is valid, the others: packet is invalid.

Mdf history  :
1.Date       : 2024.07.23
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 GtpCheckPacketInvalid(void *pack, u32 pack_size, u64 *stream_key) {
    if ((NULL == pack) || (0 == pack_size)) {
        RETURN_ERR(kGtpInterfaceMd, kInvalidInPutParam);
    }

    if (NULL != stream_key) {
        *stream_key = 0;
    }

    GtpPacket *gtp_pack = (GtpPacket*)pack;

    u32 nret = InnerCheckPacketInvalid(gtp_pack, pack_size);
    if (GTP_OK != nret) {
        return nret;
    }

    // when the sender embedded stream_key_ under has_check_flag_, recover it here without needing a
    // session lookup.
    // DATA/ACK/NACK/RTT: layout [GtpPacket][optional first_pack_sn u32][hash u32][user_id u32]...
    // FEC: layout [Fec2CodePack header][hash u32][user_id u32][fec_code_...]
    if ((NULL != stream_key) && (GTP_YES == gtp_pack->has_check_flag_)) {
        u32 key_offset = (u32)sizeof(GtpPacket);
        if ((u8)(GtpPackType::kGtpFecPackType) == gtp_pack->pack_type_) {
            key_offset = (u32)sizeof(Fec2CodePack);
        } else if (0 != ((u8)(gtp_pack->repeat_counter_))) {
            key_offset += (u32)sizeof(u32);
        }

        if ((key_offset + sizeof(u64)) <= pack_size) {
            // wire layout: [HIGH32 of stream_key][LOW32 of stream_key] at key_offset.
            // local names 'hash'/'user_id' are misleading relics — the math is correct.
            u32 hash    = 0;
            u32 user_id = 0;
            memcpy(&hash,    ((u8*)gtp_pack) + key_offset,              sizeof(u32));
            memcpy(&user_id, ((u8*)gtp_pack) + key_offset + sizeof(u32), sizeof(u32));

            *stream_key = (((u64)hash) << 32) | ((u64)user_id);
        }
    }

    return GTP_OK;
}

/*****************************************************************************************************************
Name     : GtpPacketReceive
Function : receive packet interface provided by goodtp, goodtp will remove itself packet header then call the registered
           receive_frame_cb_ to hand out frame to application.
In param : GtpHandler_p gtp_hdl
           void *pack    // there are at least 64 bytes memory to use for goodtp.
           u32 pack_sz   // it's only the frame data bytes number.
           GtpAddr *tran_addr
Out param: void
Return   : u32    // GTP_OK: sucess, the others: failed and call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2023.08.25
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 GtpPacketReceive(GtpHandler_p gtp_hdl, void *pack, u32 pack_sz, GtpAddr *tran_addr) {
    if ((NULL == pack) || (NULL == tran_addr)) {
        RETURN_ERR(kGtpMgrMd, kInvalidInPutParam);
    }

    #if ((_WIN32 || _WIN64) && (1 == ENABLE_INPUT_PARAM_CHCK))
    const GtpHandler_p in_gtp_hdl = gtp_hdl;
    const void *in_pack = pack;
    const u32 in_pack_sz = pack_sz;
    const GtpAddr *in_tran_addr = tran_addr;
    #endif

    GtpPacket *gtp_pack = (GtpPacket*)pack;

    GoodTp *gtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));
    if (NULL == gtp_obj) {
        RETURN_ERR(kGtpMgrMd, kInvalidGtpHandler);
    }

    CheckThreadStackFreeSize(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd);

    u32 nret = GTP_OK;

    #if (1 == ENABLE_TRAN_MEM_CHECK)
    nret = GtpTranAddrIsValid(tran_addr);
    if (GTP_OK != nret) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpSessionMd, kGtpLogLevelError, "Goodtp transport address "\
               "memory is invalid(0x%08x tran_addr=%p dst_addr_len=%u src_addr_len=%u dst_mem_addr=%p "\
               "src_mem_addr=%p).\r\n", nret, tran_addr,
               ((NULL != tran_addr) ? tran_addr->sock_addr_len_ : 0),
               ((NULL != tran_addr) ? tran_addr->self_addr_len_ : 0),
               ((NULL != tran_addr) ? tran_addr->sock_addr_ : NULL),
               ((NULL != tran_addr) ? tran_addr->self_addr_ : NULL));
         return nret;
    }
    #endif

    if (0x00 == gtp_pack->goodtp_ver_) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelError, "The client version is too old.\r\n");
        RETURN_ERR(kGtpMgrMd, kPeerVerTooOldErr);
    }

    tran_addr->stream_type_   = 0;

    if (GTP_OK != gtp_obj->CheckSingleThreadCalling()) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "The same goodtp instance used by "\
               "the diffrent thread.\r\n");
    }

    #if ((_WIN32 || _WIN64) && (1 == ENABLE_INPUT_PARAM_CHCK))
    if ((in_gtp_hdl != gtp_hdl) || (in_pack != pack) || (in_pack_sz != pack_sz) || (in_tran_addr != in_tran_addr)) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "in parameter is modify abnormal("\
               "gtp_hdl(%p|%p) pack(%p|%p) size(%u|%u) tran_addr(%p|%p) pid=%d tid=%d\r\n", in_gtp_hdl, gtp_hdl,
               in_pack, pack, in_pack_sz, pack_sz, in_tran_addr, tran_addr, (i32)GtpGetProcessId(),
               (i32)GtpGetThreadId());
    }

    if ((GTP_OK != gtp_obj->packet_mem_pool_.CheckAddrIsValid((u8*)pack))
     || (GTP_OK != gtp_obj->packet_mem_pool_.CheckAddrIsValid((u8*)tran_addr))) {
        const std::string &err_info = gtp_obj->packet_mem_pool_.Error();
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "packet's memory isn't belong to the "\
               "memory pool%s pid=%d tid=%d\r\n", err_info.c_str(), (i32)GtpGetProcessId(), (i32)GtpGetThreadId());
        RETURN_ERR(kGtpInterfaceMd, kPackMemIsInvalid);
    }

    if ((in_gtp_hdl != gtp_hdl) || (in_pack != pack) || (in_pack_sz != pack_sz) || (in_tran_addr != in_tran_addr)) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "in parameter is modify abnormal("\
               "gtp_hdl(%p|%p) pack(%p|%p) size(%u|%u) tran_addr(%p|%p) pid=%d tid=%d\r\n", in_gtp_hdl, gtp_hdl,
               in_pack, pack, in_pack_sz, pack_sz, in_tran_addr, tran_addr, (i32)GtpGetProcessId(),
               (i32)GtpGetThreadId());
    }
    #endif

    #if (1 == ENABLE_INNER_VALID_CHCK)
    u32 org_value = *((u32*)pack);
    #endif

    GtpHeaderOldToNew(pack);

    #if (1 == ENABLE_INTERFACE_LOG)
    u8 self_ip[GTP_MAX_STR_IP_SZ] = {0};
    u8 peer_ip[GTP_MAX_STR_IP_SZ] = {0};

    u16 self_port = 0;
    u16 peer_port = 0;

    u8  version   = (u8)(gtp_pack->goodtp_ver_);
    u8  pack_type = (u8)(gtp_pack->pack_type_);
    u32 pack_sn   = gtp_pack->pack_sn_;
    u32 first_sn  = 0;

    if (((u8)(GtpPackType::kGtpDataPackType)) == ((u8)(gtp_pack->pack_type_))) {
        if (0 == gtp_pack->repeat_counter_) {
            first_sn = gtp_pack->pack_sn_;
        } else {
            first_sn = *((u32*)(((u8*)gtp_pack) + sizeof(GtpPacket)));
        }
    }

    GtpAddrToStrIpAndPort(tran_addr, self_ip, peer_ip, &self_port, &peer_port);
    #endif

    u32 frame_size      = 0;
    u8  saved_pack_type = 0;
    u32 saved_pack_sn   = 0;
    u32 saved_first_sn  = 0;

    #if (1 == ENABLE_INNER_VALID_CHCK)
    nret = InnerCheckPacketInvalid(gtp_pack, pack_sz);

    #if ((_WIN32 || _WIN64) && (1 == ENABLE_INPUT_PARAM_CHCK))
    if ((in_gtp_hdl != gtp_hdl) || (in_pack != pack) || (in_pack_sz != pack_sz) || (in_tran_addr != in_tran_addr)) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "in parameter is modify abnormal("\
               "gtp_hdl(%p|%p) pack(%p|%p) size(%u|%u) tran_addr(%p|%p) pid=%d tid=%d\r\n", in_gtp_hdl, gtp_hdl,
               in_pack, pack, in_pack_sz, pack_sz, in_tran_addr, tran_addr, (i32)GtpGetProcessId(),
               (i32)GtpGetThreadId());
    }
    #endif

    if (GTP_OK != nret) {
        #if (0 == ENABLE_INTERFACE_LOG)
        u8 self_ip[GTP_MAX_STR_IP_SZ] = {0};
        u8 peer_ip[GTP_MAX_STR_IP_SZ] = {0};

        u16 self_port = 0;
        u16 peer_port = 0;

        GtpAddrToStrIpAndPort(tran_addr, self_ip, peer_ip, &self_port, &peer_port);
        #endif

        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelError,
               "%s:%u<-->%s:%u(pack header): ver=0x%02x offset=%u loss_flag=%u check_flag=%u ts_flag=%u rtt_flag=%u "\
               "repeat_num=%u type=0x%02x pack_size=%u in_size=%u pack_sn=0x%08x nret=0x%08x pid=%d tid=%d\r\n",
               self_ip, (u32)self_port, peer_ip, (u32)peer_port, (u32)(gtp_pack->goodtp_ver_),
               (u32)(gtp_pack->header_offset_), (u32)(gtp_pack->has_loss_flag_), (u32)(gtp_pack->has_check_flag_),
               (u32)(gtp_pack->has_ts_flag_), (u32)(gtp_pack->has_rtt_flag_), (u32)(gtp_pack->repeat_counter_),
               (u32)(gtp_pack->pack_type_), (u32)(gtp_pack->pack_size_), pack_sz, gtp_pack->pack_sn_, nret,
               (i32)GtpGetProcessId(), (i32)GtpGetThreadId());

        if (0x00 == gtp_pack->goodtp_ver_) {
            *((u32*)pack) = org_value;  // restore original value.
        }

        return nret;
    }
    #endif

    if (0x02 > gtp_pack->goodtp_ver_) {
        gtp_pack->has_chg_zone_ = GTP_NO;
        gtp_pack->stream_type_  = kRealTimeStream;
    }

    // If caller did not pre-set enable_key_, extract stream_key from the packet so that
    // GetSession can find or create the session by key rather than by five-tuple.
    if ((GTP_NO == tran_addr->enable_key_) && (GTP_YES == gtp_pack->has_check_flag_)) {
        u32 key_offset = (u32)sizeof(GtpPacket);
        if ((u8)(GtpPackType::kGtpFecPackType) == gtp_pack->pack_type_) {
            key_offset = (u32)sizeof(Fec2CodePack);
        } else if (0 != ((u8)(gtp_pack->repeat_counter_))) {
            key_offset += (u32)sizeof(u32);
        }
        if ((key_offset + (u32)sizeof(u64)) <= pack_sz) {
            u32 hi = 0, lo = 0;
            memcpy(&hi, ((u8*)gtp_pack) + key_offset,              sizeof(u32));
            memcpy(&lo, ((u8*)gtp_pack) + key_offset + sizeof(u32), sizeof(u32));
            u64 embedded_key = ((u64)hi << 32) | (u64)lo;
            if (0 != embedded_key) {
                tran_addr->enable_key_ = GTP_YES;
                tran_addr->stream_key_ = embedded_key;
            }
        }
    }

    gtp_obj->current_ts_us_ = GtpSysTimestampUs();

    u32 sort_sn = 0;
    if ((0x01 < gtp_pack->goodtp_ver_) && (GTP_YES == gtp_pack->has_chg_zone_)) {
        sort_sn = ((ChangeZone*)(((u8*)gtp_pack) + gtp_pack->header_offset_))->sort_sn_;
    }

    GtpSession *session = gtp_obj->GetSession(tran_addr, gtp_hdl, (u32)(GtpSessionMode::kReceiver),
                                              gtp_pack->pack_sn_, sort_sn);
    if (NULL == session) {
        #if (0 == ENABLE_INTERFACE_LOG)
        u8 self_ip[GTP_MAX_STR_IP_SZ] = {0};
        u8 peer_ip[GTP_MAX_STR_IP_SZ] = {0};

        u16 self_port = 0;
        u16 peer_port = 0;

        GtpAddrToStrIpAndPort(tran_addr, self_ip, peer_ip, &self_port, &peer_port);
        #endif

        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelError,
               "call GetSession() failed(%s:%u<-->%s:%u).\r\n ", self_ip, (u32)self_port, peer_ip, (u32)peer_port);

        RETURN_ERR(kGtpInterfaceMd, kGetSessionFailed);
    }

    #if (1 == ENABLE_MD_PERF_CHECK)
    session->pb_dt_.recv_consume_.ts_us_ = gtp_obj->current_ts_us_;
    #endif

    #if ((_WIN32 || _WIN64) && (1 == ENABLE_INPUT_PARAM_CHCK))
    if (gtp_hdl != session->app_gtp_hdl_) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "%s:%u<-->%s:%u: the same "\
               "session belong to diffrent gtp instance(first_gtp_hdl=%p current_gtp_hdl=%p pid=%d tid=%d)\r\n",
               session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_),
               session->pb_dt_.peer_ip_, (u32)(session->pb_dt_.peer_port_), session->app_gtp_hdl_,
               gtp_hdl, (i32)GtpGetProcessId(), (i32)GtpGetThreadId());
    }
    #endif

    #if (1 == ENABLE_INTERFACE_LOG)
    GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelDebug, "[Entry GtpPacketReceive()]%s:%u<-->%s:%u "\
           "gtp_inst=%p gtp_hdl=%p session=%p ver=0x%02x type=0x%02x pack_sn=%u first_sn=%u \r\n ", self_ip,
           (u32)self_port, peer_ip, (u32)peer_port, gtp_obj, gtp_hdl, session, (u32)version, (u32)pack_type,
           pack_sn, first_sn);
    #endif

    if (session->pb_dt_.peer_version_ != gtp_pack->goodtp_ver_) {
        session->pb_dt_.peer_version_ = gtp_pack->goodtp_ver_;
    }

    if (((u8)(GtpPackType::kGtpDataPackType)) == gtp_pack->pack_type_) {
        session->last_active_ts_us_ = gtp_obj->current_ts_us_;
    }

    session->pb_dt_.tran_addr_.timestamp_ = gtp_obj->current_ts_us_;

    if (0 != tran_addr->self_addr_len_) {
        if (0 != memcmp(session->pb_dt_.tran_addr_.self_addr_, tran_addr->self_addr_, tran_addr->self_addr_len_)) {
            session->pb_dt_.tran_addr_.self_addr_len_ = tran_addr->self_addr_len_;
            memcpy(session->pb_dt_.tran_addr_.self_addr_, tran_addr->self_addr_, tran_addr->self_addr_len_);

            session->UpdateSelfIp(tran_addr->self_addr_, tran_addr->self_addr_len_);
        }
    }

    if (0 != memcmp(session->pb_dt_.tran_addr_.sock_addr_, tran_addr->sock_addr_, tran_addr->sock_addr_len_)) {
        session->pb_dt_.tran_addr_.sock_addr_len_ = tran_addr->sock_addr_len_;
        memcpy(session->pb_dt_.tran_addr_.sock_addr_, tran_addr->sock_addr_, tran_addr->sock_addr_len_);

        session->UpdatePeerIp(tran_addr->sock_addr_, tran_addr->sock_addr_len_);
    }

    if (session->pb_dt_.tran_addr_.sfd_ != tran_addr->sfd_) {
        session->pb_dt_.tran_addr_.sfd_ = tran_addr->sfd_;

        if (0 == tran_addr->self_addr_len_) {
            session->UpdateSelfIp(tran_addr->sfd_);
        }
    }

    u8 *frame     = NULL;
    u32 pack_size = (u32)(gtp_pack->pack_size_);

    #if (1 == ENABLE_FRAME_COPY_OUT)
    u8 *out_mem       = NULL;
    GtpAddr *out_addr = NULL;
    u32 out_size      = 0;
    u32 mem_spec      = 0;
    #endif

    nret = session->PackPrepHandler(gtp_pack, pack_size, &frame, &frame_size);
    if (GEN_ERR(kGtpSessionMd, kDuplicatePackErr) == nret) {
        u8  dup_pack_type = gtp_pack->pack_type_;
        u32 dup_pack_sn   = gtp_pack->pack_sn_;
        session->PackPostHandler(gtp_pack, pack_size, tran_addr);
        // ARQ retransmits use sequential new pack_sn_ values interleaved in the DATA stream.
        // Pass dup_pack_sn (not first_sn) so the reorder buffer advances past the retransmit
        // slot in the pack_sn_ sequence.
        if (((u8)(GtpPackType::kGtpDataPackType)) == dup_pack_type) {
            session->ReorderEnqueue(dup_pack_sn, NULL, 0, tran_addr, session->last_active_ts_us_);
        }
        nret = GTP_OK;
        goto pack_receive_exit_pos_;
    }

    if (GTP_OK != nret) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelDebug, "%s:%u<-->%s:%u gtp_inst=%p gtp_hdl=%p "\
               "ver=0x%02x type=0x%02x pack_sn=%u first_sn=%u don't repeat but prep process failed(0x%08x)\r\n ",
               session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_), session->pb_dt_.peer_ip_,
               (u32)(session->pb_dt_.peer_port_), gtp_obj, gtp_hdl, (u32)(gtp_pack->goodtp_ver_),
               (u32)(gtp_pack->pack_type_), gtp_pack->pack_sn_, *((u32*)(((u8*)gtp_pack) + gtp_pack->header_offset_)),
               nret);
        goto pack_receive_exit_pos_;
    }

    // Save pack_type and pack_sn before PackPostHandler, which frees FEC packet buffers.
    saved_pack_type = gtp_pack->pack_type_;
    saved_pack_sn   = gtp_pack->pack_sn_;
    // For retransmits (repeat_counter > 0), first_sn (the original logical SN) is stored
    // immediately after the fixed header.  We pass it as a hint to ReorderEnqueue so that when
    // the retransmit's new pack_sn_ would trigger a large-gap flush (delta >= REORDER_BUF_SIZE),
    // the frame lands at its correct logical position instead.
    saved_first_sn = (0 == gtp_pack->repeat_counter_)
                     ? gtp_pack->pack_sn_
                     : *((u32*)(((u8*)gtp_pack) + sizeof(GtpPacket)));

    nret = session->PackPostHandler(gtp_pack, pack_size, tran_addr);
    if (GTP_OK != nret) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelDebug, "%s:%u<-->%s:%u gtp_inst=%p gtp_hdl=%p "\
               "ver=0x%02x type=0x%02x pack_sn=%u first_sn=%u Call PackPostHandler() failed(0x%08x)\r\n ",
               session->pb_dt_.self_ip_, (u32)(session->pb_dt_.self_port_), session->pb_dt_.peer_ip_,
               (u32)(session->pb_dt_.peer_port_), gtp_obj, gtp_hdl, (u32)saved_pack_type,
               (u32)saved_pack_type, saved_pack_sn, *((u32*)(((u8*)gtp_pack) + gtp_pack->header_offset_)),
               nret);
        goto pack_receive_exit_pos_;
    }

    tran_addr->timestamp_ = session->pb_dt_.tran_addr_.timestamp_;

    // Only DATA packets go through the reorder buffer.
    // FEC-recovered packets arrive via Fec2RestoreFrameReceive which calls ReorderEnqueue directly.
    // FEC, ACK, NACK, and RTT packets use non-sequential or overlapping SNs and must be excluded.
    if (((u8)(GtpPackType::kGtpDataPackType)) == saved_pack_type) {
        // Primary key is pack_sn_ (retransmit's new SN) for normal nearby-slot behavior.
        // Pass saved_first_sn as a hint: if pack_sn_ would cause a large-gap flush, fall back
        // to the logical first_sn so the retransmit doesn't discard buffered good data.
        session->ReorderEnqueue(saved_pack_sn, frame, frame_size, tran_addr,
                                session->last_active_ts_us_, saved_first_sn);
    }

pack_receive_exit_pos_:
    #if (1 == ENABLE_INTERFACE_LOG)
    GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelDebug, "[Exit GtpPacketReceive()]%s:%u<-->%s:%u "\
           "gtp_inst=%p gtp_hdl=%p session=%p ver=0x%02x type=0x%02x pack_sn=%u first_sn=%u \r\n ",
           self_ip, (u32)self_port, peer_ip, (u32)peer_port, gtp_obj, gtp_hdl, session, (u32)version, (u32)pack_type,
           pack_sn, first_sn);
    #endif

    #if (1 == ENABLE_MD_PERF_CHECK)
    u64 temp_ts_us = GtpSysTimestampUs();
    temp_ts_us -= session->pb_dt_.recv_consume_.ts_us_;
    session->pb_dt_.recv_consume_.CacheConsumeTime(temp_ts_us);
    #endif

    return nret;
}

/*****************************************************************************************************************
Name     : PeriodGtpTimer
Function : Application system shall call this interface periodically.
In param : GtpHandler_p gtp_hdl
Out param: void
Return   : u32    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2023.09.12
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 PeriodGtpTimer(GtpHandler_p gtp_hdl) {
    GoodTp *gtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));
    if (NULL == gtp_obj) {
        RETURN_ERR(kGtpMgrMd, kInvalidGtpHandler);
    }

    CheckThreadStackFreeSize(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd);

    if (GTP_OK != gtp_obj->CheckSingleThreadCalling()) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "The same goodtp instance used by "\
               "the diffrent thread.\r\n");
    }

    gtp_obj->CheckResourceActiveStatus(gtp_hdl);

    return GTP_OK;
}

/*****************************************************************************************************************
Name     : GetLinkerQuality
Function : Get the special linker network quality by handler.
In param : GtpHandler_p gtp_hdl
           GtpLinkerKey *linker_key
Out param: GtpLinkQuality *out_quality
Return   : u32    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2023.10.20
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 GetLinkerQuality(GtpHandler_p gtp_hdl, GtpLinkerKey *linker_key, GtpLinkQuality *out_quality) {
    if ((NULL == linker_key) || (NULL == out_quality)) {
        RETURN_ERR(kGtpMgrMd, kInvalidInPutParam);
    }

    GoodTp *gtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));
    if (NULL == gtp_obj) {
        RETURN_ERR(kGtpMgrMd, kInvalidGtpHandler);
    }

    if (GTP_OK != gtp_obj->CheckSingleThreadCalling()) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "The same goodtp instance used by "\
               "the diffrent thread.\r\n");
    }

    goodtp_sock sfd = (goodtp_sock)(linker_key->sfd_);

    u8  peer_bin_ip[IPV6_BIN_IP_SIZE] = {0};
    u8  self_bin_ip[IPV6_BIN_IP_SIZE] = {0};

    u16 peer_ip_size  = 0;
    u16 peer_bin_port = 0;
    u16 self_ip_size  = 0;
    u16 self_bin_port = 0;

    GtpSockAddrToBinAddr(&(linker_key->peer_socket_addr_[0]), &(peer_bin_ip[0]), &peer_ip_size, &peer_bin_port);

    if (0 != linker_key->self_socket_addr_len_) {
        GtpSockAddrToBinAddr(&(linker_key->self_socket_addr_[0]), &(self_bin_ip[0]), &self_ip_size, &self_bin_port);
    } else {
        memset(self_bin_ip, 0x00, sizeof(self_bin_ip));
    }

    GtpSessionKey session_key(sfd, &(peer_bin_ip[0]), peer_ip_size, peer_bin_port, &(self_bin_ip[0]), self_ip_size,
                              peer_bin_port);

    GtpSession *session = gtp_obj->GetSession(session_key);
    if (NULL == session) {
        RETURN_ERR(kGtpMgrMd, kLinkerNoExistErr);
    }

    if (1 == linker_key->direction_) {
        return session->GetQuality((u32)kSenderQuality, out_quality);
    }

    return session->GetQuality((u32)kReceiverQuality, out_quality);
}

/*****************************************************************************************************************
Name     : DelGtpLinker
Function : Delete goodtp linker before not using.
In param : GtpHandler_p gtp_hdl
           GtpAddr *tran_addr
Out param: void
Return   : void

Mdf history  :
1.Date       : 2023.11.27
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
void DelGtpLinker(GtpHandler_p gtp_hdl, GtpAddr *tran_addr) {
    if (NULL == tran_addr) {
        return;
    }

    GoodTp *gtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));
    if (NULL == gtp_obj) {
        return;
    }

    if (GTP_OK != gtp_obj->CheckSingleThreadCalling()) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "The same goodtp instance used by "\
               "the diffrent thread.\r\n");
    }

    gtp_obj->DelSpsSession(gtp_hdl, tran_addr);

    return;
}

/*****************************************************************************************************************
Name     : PrintSessionWinBitMap
Function : Print session's slid window bitmap.
In param : GtpHandler_p gtp_hdl
           const u8 *src_ip
           const u8 *dst_ip
           u8 *out_str
           u32 mem_size
Out param: u8 *out_str
Return   : u32    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.01.10
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 PrintSessionWinBitMap(GtpHandler_p gtp_hdl, const u8 *src_ip, const u8 *dst_ip, u8 *out_str, u32 mem_size) {
    if ((NULL == src_ip) || (NULL == dst_ip) || (NULL == out_str) || (0 == mem_size)) {
        RETURN_ERR(kGtpMgrMd, kInvalidInPutParam);
    }

    GoodTp *gtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));
    if (NULL == gtp_obj) {
        RETURN_ERR(kGtpMgrMd, kInvalidGtpHandler);
    }

    if (GTP_OK != gtp_obj->CheckSingleThreadCalling()) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "The same goodtp instance used by "\
               "the diffrent thread.\r\n");
    }

    return gtp_obj->ShowBitMap(src_ip, dst_ip, out_str, mem_size);
}

/*****************************************************************************************************************
Name     : GetAlgorithmParam
Function : Get the goodtp algorithm parameters.
In param : GtpHandler_p gtp_hdl
           const u8 *src_ip
           const u8 *dst_ip
           u8 *out_str
           u32 mem_size
Out param: u8 *out_str
Return   : u32    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.09.12
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 GetAlgorithmParam(GtpHandler_p gtp_hdl, const u8 *src_ip, const u8 *dst_ip, u8 *out_str, u32 mem_size) {
    if ((NULL == src_ip) || (NULL == dst_ip) || (NULL == out_str) || (0 == mem_size)) {
        RETURN_ERR(kGtpMgrMd, kInvalidInPutParam);
    }

    GoodTp *gtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));
    if (NULL == gtp_obj) {
        RETURN_ERR(kGtpMgrMd, kInvalidGtpHandler);
    }

    if (GTP_OK != gtp_obj->CheckSingleThreadCalling()) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "The same goodtp instance used by "\
               "the diffrent thread.\r\n");
    }

    u32 str_len = gtp_obj->ShowAlgorithmParam(src_ip, dst_ip, out_str, mem_size);
    if (0 == str_len) {
        RETURN_ERR(kGtpMgrMd, kGtpMgrMd);
    }

    return GTP_OK;
}

/*****************************************************************************************************************
Name     : ShowTotalLinker
Function : show all the linker information.
In param : GtpHandler_p gtp_hdl
           const u8 *matched_str     // matched string ip, it will show all linker when it's empty.
           u8 *out_str
           u32 mem_size
Out param: u8 *out_str
Return   : u32    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.02.01
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 ShowTotalLinker(GtpHandler_p gtp_hdl, const u8 *matched_str, u8 *out_str, u32 mem_size) {
    if ((NULL == matched_str) || (NULL == out_str) || (0 == mem_size)) {
        RETURN_ERR(kGtpMgrMd, kInvalidInPutParam);
    }

    GoodTp *gtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));
    if (NULL == gtp_obj) {
        RETURN_ERR(kGtpMgrMd, kInvalidGtpHandler);
    }

    if (GTP_OK != gtp_obj->CheckSingleThreadCalling()) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "The same goodtp instance used by "\
               "the diffrent thread.\r\n");
    }

    u32 length = gtp_obj->ShowLinker(matched_str, out_str, mem_size);
    if (0 == length) {
        RETURN_ERR(kGtpMgrMd, kLinkerNoExistErr);
    }

    return GTP_OK;
}

/*****************************************************************************************************************
Name     : GetSlidWinBitMapInfo
Function : Get specific linker slid window's bitmap information.
In param : GtpHandler_p gtp_hdl
           GtpLinkerKey *linker_key
           u8 *out_str
           u32 mem_size
Out param: u8 *out_str
Return   : u32    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.06.16
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 GetSlidWinBitMapInfo(GtpHandler_p gtp_hdl, GtpLinkerKey *linker_key, u8 *out_str, u32 mem_size) {
    if ((NULL == linker_key) || (NULL == out_str)) {
        RETURN_ERR(kGtpMgrMd, kInvalidInPutParam);
    }

    GoodTp *gtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));
    if (NULL == gtp_obj) {
        RETURN_ERR(kGtpMgrMd, kInvalidGtpHandler);
    }

    if (GTP_OK != gtp_obj->CheckSingleThreadCalling()) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "The same goodtp instance used by "\
               "the diffrent thread.\r\n");
    }

    goodtp_sock sfd = (goodtp_sock)(linker_key->sfd_);

    u8  peer_bin_ip[IPV6_BIN_IP_SIZE] = {0};
    u8  self_bin_ip[IPV6_BIN_IP_SIZE] = {0};

    u16 peer_ip_size  = 0;
    u16 peer_bin_port = 0;
    u16 self_ip_size  = 0;
    u16 self_bin_port = 0;

    GtpSockAddrToBinAddr(&(linker_key->peer_socket_addr_[0]), &(peer_bin_ip[0]), &peer_ip_size, &peer_bin_port);

    if (0 != linker_key->self_socket_addr_len_) {
        GtpSockAddrToBinAddr(&(linker_key->self_socket_addr_[0]), &(self_bin_ip[0]), &self_ip_size, &self_bin_port);
    } else {
        memset(self_bin_ip, 0x00, sizeof(self_bin_ip));
    }

    GtpSessionKey session_key(sfd, &(peer_bin_ip[0]), peer_ip_size, peer_bin_port, &(self_bin_ip[0]), self_ip_size,
                              self_bin_port);

    GtpSession *session = gtp_obj->GetSession(session_key);
    if (NULL == session) {
        RETURN_ERR(kGtpMgrMd, kLinkerNoExistErr);
    }

    u32 str_sz = 0;
    u32 nret   = session->WinBitMap(out_str, mem_size, &str_sz);
    if (GTP_OK != nret) {
        return nret;
    }

    return GTP_OK;
}

/*****************************************************************************************************************
Name     : GetPackMemPoolStatus
Function : Get the packet memory pool current status.
In param : GtpHandler_p gtp_hdl
           u8 out_status[]   // at least 4096 bytes.
           u32 mem_size      // out_status's bytes size.
Out param: u8 out_status[]   // memory pool status string
Return   : u32    // GTP_OK: sucess, the others: failed, call LastGtpErrorInfo() to obtain the detailed information.

Mdf history  :
1.Date       : 2024.06.28
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 GetPackMemPoolStatus(GtpHandler_p gtp_hdl, u8 out_status[], u32 mem_size) {
    if ((NULL == out_status) || (1024 > mem_size)) {
        RETURN_ERR(kGtpMgrMd, kInvalidInPutParam);
    }

    GoodTp *gtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));
    if (NULL == gtp_obj) {
        RETURN_ERR(kGtpMgrMd, kInvalidGtpHandler);
    }

    if (GTP_OK != gtp_obj->CheckSingleThreadCalling()) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "The same goodtp instance used by "\
               "the diffrent thread.\r\n");
    }

    gtp_obj->GetTranMemPoolStatus(out_status, mem_size);

    return GTP_OK;
}

/*****************************************************************************************************************
Name     : GetSessionNumber
Function : Get the goodtp's existing session number.
In param : GtpHandler_p gtp_hdl
Out param: void
Return   : u32  // current existing session number.

Mdf history  :
1.Date       : 2024.07.15
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
u32 GetSessionNumber(GtpHandler_p gtp_hdl) {
    GoodTp *gtp_obj = GtpHandlerToObj(GtpHdlPointerToInt(gtp_hdl));
    if (NULL == gtp_obj) {
        return 0;
    }

    if (GTP_OK != gtp_obj->CheckSingleThreadCalling()) {
        GtpLog(gtp_obj->cb_.write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning, "The same goodtp instance used by "\
               "the diffrent thread.\r\n");
    }

    return gtp_obj->SessionNumber();
}

/*****************************************************************************************************************
Name     : CalcOnePacketLength
Function : get a bit linker packet's length from the four-byte header.
In param : const uint8_t header[4]  // the first four bytes of one bit linker packet.
Out param: void
Return   : int32_t  // -1: not a valid bit linker packet, others: packet's byte length.

Mdf history  :
1.Date       : 2025.05.13
    Author     : Albert.Feng
    Mdf context: new function

*****************************************************************************************************************/
i32 CalcOnePacketLength(const u8 header[4]) {
    NewGtpHeader4Byte h;
    memcpy(&h, header, sizeof(h));

    if (0x02 != h.goodtp_ver_) {
        return -1;
    }

    if ((0x00 == h.pack_type_) || (((u8)(GtpPackType::GtpPackTypeButt)) <= h.pack_type_)) {
        return -1;
    }

    return (i32)(h.pack_size_);
}

#ifdef __cplusplus
}
#endif

