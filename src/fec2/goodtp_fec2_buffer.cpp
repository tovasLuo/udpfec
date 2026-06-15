/*********************************************************************************************************************

                           Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_fec2_buffer.cpp
  Version  : Initial
  Author   : Albert.Feng
  Function : The new fec using buffer manager.
  Modify record:
  1.Date   : July 17, 2024
    Author : Albert.Feng
    Content: New creates file.

*********************************************************************************************************************/
#include "goodtp_fec2_buffer.h"
#include "goodtp_mgr.h"
#include "goodtp_comstruct.h"
#include "goodtp_macrodefine.h"
#include "tranmempool.h"
#include "bitlinker.h"

#ifdef __cplusplus
extern "C" {
#endif

Fec2Buffer::Fec2Buffer(TranMemPool &pack_mem_pool, pLogCallBack write_log_cb) :
    write_log_cb_(write_log_cb),
    pack_num_in_cache_(0),
    max_cached_sn_(0),
    pack_mem_pool_(pack_mem_pool) {
    u32 i = 0;
    do {
        pack_cache_[i] = NULL;
        i += 1;
    } while (MAX_FEC2_CACHE_CAPACITY > i);
}

Fec2Buffer::~Fec2Buffer() {
}

void Fec2Buffer::PopAllPack(void) {
    if (0 == pack_num_in_cache_) {
        return;
    }

    u32 i = 0;
    do {
        if (NULL != pack_cache_[i]) {
            pack_mem_pool_.FreeTranBuf((u8*)pack_cache_[i]);
            pack_cache_[i] = NULL;
        }

        i += 1;
    } while (MAX_FEC2_CACHE_CAPACITY > i);

    pack_num_in_cache_ = 0;

    return;
}

void Fec2Buffer::Init(void) {
    PopAllPack();
}

// clear zone is [start_pos, end_pos]
void Fec2Buffer::Clear(const goodtp_pos &start_pos, const goodtp_pos &end_pos) {
    goodtp_pos i = start_pos;
    u32 counter  = 0;

    do {
        if (NULL != pack_cache_[i]) {
            pack_mem_pool_.FreeTranBuf((u8*)pack_cache_[i]);
            pack_cache_[i] = NULL;

            counter += 1;
        }

        i += 1;
        i &= FEC2_CACHE_CAPACITY_MASK;
    } while (end_pos != i);

    if (NULL != pack_cache_[i]) {
        pack_mem_pool_.FreeTranBuf((u8*)pack_cache_[i]);
        pack_cache_[i] = NULL;

        counter += 1;
    }

    if (counter <= pack_num_in_cache_) {
        pack_num_in_cache_ -= counter;
    } else {
        pack_num_in_cache_  = 0;
    }

    return;
}

void Fec2Buffer::PopPack(GtpPackCacheStru *pack) {
    if (MAX_FEC2_CACHE_CAPACITY <= pack->cache_id_) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelWarning, "Current packet memory isn't come from "\
               "fec2 buffer(buffer_capacity=%u cache_id=%u).\r\n", MAX_FEC2_CACHE_CAPACITY,
               (u32)(pack->cache_id_));
        return;
    }

    if (NULL == pack_cache_[pack->cache_id_]) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelWarning, "cache pos(%u) is null, only free input paramter.\r\n",
               (u32)(pack->cache_id_));
        pack_mem_pool_.FreeTranBuf((u8*)pack);
        return;
    }

    if (0 < pack_num_in_cache_) {
        pack_num_in_cache_ -= 1;
    }

    if (pack != pack_cache_[pack->cache_id_]) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelWarning, "pop packet isn't equal(%p != %p), free input "\
               "parameter and cahced memory.\r\n", pack, pack_cache_[pack->cache_id_]);

        pack_mem_pool_.FreeTranBuf((u8*)(pack_cache_[pack->cache_id_]));
        pack_cache_[pack->cache_id_] = NULL;

        pack_mem_pool_.FreeTranBuf((u8*)pack);

        return;
    }

    pack_cache_[pack->cache_id_] = NULL;
    pack_mem_pool_.FreeTranBuf((u8*)pack);

    return;
}

void Fec2Buffer::PopPack(const goodtp_pos &cache_pos) {
    if (MAX_FEC2_CACHE_CAPACITY <= cache_pos) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelWarning, "current cache pos(%u) is over.\r\n", (u32)cache_pos);
        return;
    }

    if (NULL == pack_cache_[cache_pos]) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelWarning, "current cache pos(%u) is null.\r\n", (u32)cache_pos);
        return;
    }

    if (0 < pack_num_in_cache_) {
        pack_num_in_cache_ -= 1;
    }

    pack_mem_pool_.FreeTranBuf((u8*)(pack_cache_[cache_pos]));

    pack_cache_[cache_pos] = NULL;

    return;
}

GtpPackCacheStru* Fec2Buffer::PushPack(GtpPacket *pack, const u32 &need_new_buf) {
    GtpPackCacheStru *new_cache = NULL;
    GtpAddr *tran_addr          = NULL;
    u32 com_var                 = 0;
    u32 mem_spec = GtpPackSizeToMemSpec(pack->pack_size_);

    if (GTP_YES == need_new_buf) {
        new_cache = (GtpPackCacheStru*)pack_mem_pool_.MallocTranBuf(NULL, 0, &com_var, (void**)(&tran_addr),
                                                                    (BufSizeType)mem_spec);
        if (NULL == new_cache) {
            const std::string &err_info = pack_mem_pool_.Error();
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Malloc packet memory failed(%s).\r\n",
                   err_info.c_str());

            const string &stat_info = pack_mem_pool_.TranMemPoolStatInfo();
            GtpLog(write_log_cb_, kGtpArqMd, kGtpLogLevelWarning, "%s\r\n", stat_info.c_str());

            return NULL;
        }

        new_cache = (GtpPackCacheStru*)(((u8*)new_cache) - sizeof(GtpPackCacheStru));

        new_cache->cache_id_    = CalcPosInPackCache(pack->pack_sn_);
        new_cache->payload_len_ = pack->pack_size_;

        memcpy(new_cache->payload_, pack, pack->pack_size_);
    } else {
        new_cache = (GtpPackCacheStru*)(((u8*)pack) - sizeof(GtpPackCacheStru));

        new_cache->cache_id_    = CalcPosInPackCache(pack->pack_sn_);
        new_cache->payload_len_ = pack->pack_size_;

        pack_mem_pool_.TranBufUseRefAddOne((u8*)pack);
    }

    if (NULL != pack_cache_[new_cache->cache_id_]) {
        pack_mem_pool_.FreeTranBuf((u8*)pack_cache_[new_cache->cache_id_]);
    } else {
        pack_num_in_cache_ += 1;
    }

    pack_cache_[new_cache->cache_id_] = new_cache;

    if (max_cached_sn_ <= pack->pack_sn_) {
        max_cached_sn_ = pack->pack_sn_;
        goto fec2_cache_pack_exit_pos_;
    }

    com_var = (0xFFFFFFFF - max_cached_sn_) + pack->pack_sn_;
    if (MAX_FEC2_CACHE_CAPACITY >= com_var) {
        // packet's sn has been turn over.
        max_cached_sn_ = pack->pack_sn_;
    }

fec2_cache_pack_exit_pos_:
    return new_cache;
}

#ifdef __cplusplus
}
#endif

