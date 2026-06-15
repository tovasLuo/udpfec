#ifndef _GOOD_TP_MEM_POOL_H_
#define _GOOD_TP_MEM_POOL_H_
/*********************************************************************************************************************

                            Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_mem_pool.h
  Version  : Initial
  Author   : Albort.Feng
  Function : Realizes a highly efficient memory pool manager header file.
  Modify record:
  1.Date   : September 1, 2023
    Author : Albort.Feng
    Content: New creates file.

*********************************************************************************************************************/
#include "goodtp_comstruct.h"
#include "goodtp_macrodefine.h"
#include "bitlinker.h"

#include <cstddef>

#define MEM_GAP_SIZE          (8)
#define POOL_NODE_HEADER_INIT (0x00A5005A)

#ifdef __cplusplus
extern "C" {
#endif

class GtpMemPool {
PRIVATE:
    #pragma pack(1)
    typedef struct _PoolNode {
        u8 mem_check_overlay_0;
        u8 used_flag_;  // GTP_NO: unused, GTP_YES: using.
        u8 mem_check_overlay_1;
        u8 quote_counter_;

        u8  memory_[1];
    }PoolNode;

    typedef struct _PoolMgr {
        u32 pool_size_;
        u32 using_num_;

        PoolNode *pool_header_;
        PoolNode *pool_tail_;

        u8 *mem_pool_left_boundary_;
        u8 *mem_pool_right_boundary_;

        u32 malloc_counter_;
        u32 free_counter_;

        u32  last_free_pos_;
        u32  bitmap_length_;

        u64 *used_bitmap_;

        PoolNode *mem_pool_[0];
    }PoolMgr;
    #pragma pack()

 public:
    explicit GtpMemPool(const u32 &block_size, const u32 &block_num);
    ~GtpMemPool();

    u32 Init(pLogCallBack write_log_cb);

    u8*  MallocItem(void *node_payload = NULL);
    void FreeItem(void *mem_addr);

    const u8* GetMemPoolStatus(void);

PRIVATE:
    void* MemAddrToElementAddr(void *mem_addr, u32 *out_node_pos = NULL);

PRIVATE:
    pLogCallBack write_log_cb_;

    u32 block_size_;
    u32 block_num_;

    u8  buffer_[1024];

    PoolMgr *pool_mgr_;
};

#ifdef __cplusplus
}
#endif

#endif

