#ifndef _GOOD_TP_FEC2_BUFFER_H_
#define _GOOD_TP_FEC2_BUFFER_H_
/*********************************************************************************************************************

                           Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_fec2_buffer.h
  Version  : Initial
  Author   : Albert.Feng
  Function : The new fec using buffer manager.
  Modify record:
  1.Date   : July 17, 2024
    Author : Albert.Feng
    Content: New creates file.

*********************************************************************************************************************/
#include "goodtp_comstruct.h"
#include "goodtp_macrodefine.h"
#include "tranmempool.h"
#include "goodtp.h"

#include <string>

#ifdef __cplusplus
extern "C" {
#endif

class Fec2Buffer {
 public:
    explicit Fec2Buffer(TranMemPool &pack_mem_pool, pLogCallBack write_log_cb = NULL);
    ~Fec2Buffer();

    void PopAllPack(void);
    void Init(void);
    void Clear(const goodtp_pos &start_pos, const goodtp_pos &end_pos);

    void PopPack(GtpPackCacheStru *pack);
    void PopPack(const goodtp_pos &cache_pos);

    GtpPackCacheStru* PushPack(GtpPacket *pack, const u32 &need_new_buf = GTP_YES);

    GtpPackCacheStru *pack_cache_[MAX_FEC2_CACHE_CAPACITY];

    u32 pack_num_in_cache_;
    u32 max_cached_sn_;

PRIVATE:
    pLogCallBack write_log_cb_;
    TranMemPool &pack_mem_pool_;
};

#ifdef __cplusplus
}
#endif

#endif

