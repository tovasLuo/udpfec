/*********************************************************************************************************************

                              Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_mem_pool.cpp
  Version  : Initial
  Author   : Albort.Feng
  Function : Realizes a highly efficient memory pool manager code file.
  Modify record:
  1.Date   : September 1, 2023
    Author : Albort.Feng
    Content: New creates file.

*********************************************************************************************************************/
#include "goodtp_mem_pool.h"
#include "goodtp_mgr.h"
#include "goodtp_comstruct.h"
#include "goodtp_macrodefine.h"
#include "goodtp.h"

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>

#ifdef __cplusplus
extern "C" {
#endif
GtpMemPool::GtpMemPool(const u32 &block_size, const u32 &block_num):
    block_size_(sizeof(PoolNode) - 1 + ((block_size & 0xFFFFFFFC) + 4)),
    block_num_((block_num & 0xFFFFFFC0) + 64),
    pool_mgr_(NULL) {
    memset(buffer_, 0x00, 256);
    if (0 == (block_size & 0xFFFFFFFC)) {
        block_size_ = sizeof(PoolNode) - 1 + block_size;
    }

    if (0 == (block_num & 0xFFFFFFC0)) {
        block_num_ = block_num;
    }
}

GtpMemPool::~GtpMemPool() {
    if (NULL != pool_mgr_) {
        free(pool_mgr_);
        pool_mgr_ = NULL;
    }
}

u32 GtpMemPool::Init(pLogCallBack write_log_cb) {
    write_log_cb_ = write_log_cb;

    u32 mgr_size      = sizeof(PoolMgr) + (sizeof(PoolNode*) * block_num_) + MEM_GAP_SIZE;
    u32 bitmap_size   = sizeof(u64) * (block_num_ >> 6) + MEM_GAP_SIZE;
    u32 mem_pool_size = block_size_ * block_num_;
    u32 total_mem_len = mgr_size + bitmap_size + mem_pool_size;

    u8  *mem = NULL;

    REGISTER u32 *ptr = NULL;
    REGISTER PoolNode *move_node = NULL;
    REGISTER u32 loop = 0;

    mem = (u8*)malloc(total_mem_len);
    if (NULL == mem) {
        RETURN_ERR(kGtpMemPoolMd, kDistributeMemFail);
    }

    pool_mgr_ = (PoolMgr*)mem;

    pool_mgr_->pool_size_      = block_num_;
    pool_mgr_->using_num_      = 0;
    pool_mgr_->pool_header_    = (PoolNode*)(mem + mgr_size + bitmap_size);
    pool_mgr_->pool_tail_      = (PoolNode*)(mem + (total_mem_len - block_size_));
    pool_mgr_->malloc_counter_ = 0;
    pool_mgr_->free_counter_   = 0;
    pool_mgr_->last_free_pos_  = 0;
    pool_mgr_->bitmap_length_  = (block_num_ >> 6);
    pool_mgr_->used_bitmap_    = (u64*)(mem + mgr_size);

    pool_mgr_->mem_pool_left_boundary_  = (u8*)(pool_mgr_->pool_header_);
    pool_mgr_->mem_pool_right_boundary_ = mem + total_mem_len;

    memset(pool_mgr_->used_bitmap_, 0x00, bitmap_size - MEM_GAP_SIZE);

    move_node = pool_mgr_->pool_header_;

    do {
        ptr  = (u32*)(&(move_node->mem_check_overlay_0));
        *ptr = POOL_NODE_HEADER_INIT;

        pool_mgr_->mem_pool_[loop] = move_node;

        move_node = (PoolNode*)(((u8*)move_node) + block_size_);

        ++loop;
    } while (block_num_ > loop);

    #if 0
    GtpLog(write_log_cb_, kGtpInterfaceMd, kGtpLogLevelWarning,
           "mem_size=%u block_size=%u block_num=%u\r\n", total_mem_len, block_size_, block_num_);
    #endif

    return GTP_OK;
}

u8* GtpMemPool::MallocItem(void *node_payload) {
    goodtp_assertb((NULL == pool_mgr_), NULL);

    if (NULL != node_payload) {
        PoolNode *node = (PoolNode*)MemAddrToElementAddr(node_payload);
        if (NULL != node) {
            node->quote_counter_       += 1;
            pool_mgr_->malloc_counter_ += 1;

            return &(node->memory_[0]);
        }
    }

    if (pool_mgr_->pool_size_ <= pool_mgr_->using_num_) {
        return NULL;
    }

    u8 *new_item = NULL;

    REGISTER u64 bit_mask;
    REGISTER u64 bitmap_value;
    REGISTER u32 free_pos;
    REGISTER u32 loop;
    REGISTER u32 bitmap_pos = (pool_mgr_->last_free_pos_ >> 6);

    if (GTP_NO == pool_mgr_->mem_pool_[pool_mgr_->last_free_pos_]->used_flag_) {
directly_distribute_pos_:
        bit_mask = 1;
        new_item = &(pool_mgr_->mem_pool_[pool_mgr_->last_free_pos_]->memory_[0]);

        pool_mgr_->mem_pool_[pool_mgr_->last_free_pos_]->used_flag_     = GTP_YES;
        pool_mgr_->mem_pool_[pool_mgr_->last_free_pos_]->quote_counter_ = 1;

        pool_mgr_->used_bitmap_[bitmap_pos] |= (bit_mask << (pool_mgr_->last_free_pos_ & 0x0000003F));

        pool_mgr_->malloc_counter_ += 1;
        pool_mgr_->using_num_      += 1;

        goto distribute_free_item_pos_;
    }

    bitmap_value = pool_mgr_->used_bitmap_[bitmap_pos];
    free_pos     = (pool_mgr_->last_free_pos_ & 0xFFFFFFC0);

    if (0xFFFFFFFFFFFFFFFF != bitmap_value) {
indirectly_distribute_pos_:
        bit_mask = 1;
        loop     = 0;

        do {
            if ((0 == (bitmap_value & bit_mask)) && (GTP_NO == pool_mgr_->mem_pool_[free_pos]->used_flag_)) {
                pool_mgr_->last_free_pos_ = free_pos;
                break;
            }

            loop      += 1;
            free_pos  += 1;
            bit_mask <<= 1;
        } while (64 > loop);

        if (64 > loop) {
            goto directly_distribute_pos_;
        }
    }

    bitmap_pos = 0;
    do {
        if (0xFFFFFFFFFFFFFFFF != pool_mgr_->used_bitmap_[bitmap_pos]) {
            break;
        }

        bitmap_pos += 1;
    } while (pool_mgr_->bitmap_length_ > bitmap_pos);

    if (pool_mgr_->bitmap_length_ > bitmap_pos) {
        bitmap_value = pool_mgr_->used_bitmap_[bitmap_pos];
        free_pos     = (bitmap_pos << 6);

        goto indirectly_distribute_pos_;
    }

    pool_mgr_->using_num_ = pool_mgr_->pool_size_;

    return NULL;

distribute_free_item_pos_:
    return new_item;
}

void GtpMemPool::FreeItem(void *mem_addr) {
    u32 node_pos   = 0;
    u32 bitmap_pos = 0;

    PoolNode *node = (PoolNode*)MemAddrToElementAddr(mem_addr, &node_pos);
    if (NULL == node) {
        return;
    }

    if (GTP_NO == node->used_flag_) {
        return;
    }

    pool_mgr_->free_counter_ += 1;

    if (0 < node->quote_counter_) {
        node->quote_counter_ -= 1;
    }

    if (0 < node->quote_counter_) {
        return;
    }

    if (0 < pool_mgr_->using_num_) {
        pool_mgr_->using_num_ -= 1;
    }

    node->used_flag_ = GTP_NO;

    bitmap_pos = (node_pos >> 6);

    u64 shift_bit = 1;
    shift_bit <<= (node_pos & 0x0000003F);

    pool_mgr_->used_bitmap_[bitmap_pos] &= (~shift_bit);
    pool_mgr_->last_free_pos_ = node_pos;

    return;
}

void* GtpMemPool::MemAddrToElementAddr(void *mem_addr, u32 *out_node_pos) {
     if ((((u8*)mem_addr) <  pool_mgr_->mem_pool_left_boundary_)
     || (((u8*)mem_addr) >= pool_mgr_->mem_pool_right_boundary_)) {
        return NULL;
    }

    #if (_WIN32 || _WIN64)
    #ifdef _WIN64
    u32 node_pos = (u32)((((u8*)mem_addr) - pool_mgr_->mem_pool_left_boundary_) / ((u64)block_size_));
    #else
    u32 node_pos = (((u8*)mem_addr) - pool_mgr_->mem_pool_left_boundary_) / block_size_;
    #endif
    #endif

    #ifdef __linux__
    #ifdef __i386__
    u32 node_pos = (((u8*)mem_addr) - pool_mgr_->mem_pool_left_boundary_) / block_size_;
    #else
    u32 node_pos = (u32)((((u8*)mem_addr) - pool_mgr_->mem_pool_left_boundary_) / ((u64)block_size_));
    #endif
    #endif

    #ifdef __APPLE__
    #if (arm64 == ARCHS_STANDARD)
    u32 node_pos = (u32)((((u8*)mem_addr) - pool_mgr_->mem_pool_left_boundary_) / ((u64)block_size_));
    #else
    u32 node_pos = (((u8*)mem_addr) - pool_mgr_->mem_pool_left_boundary_) / block_size_;
    #endif
    #endif

    if (NULL != out_node_pos) {
        *out_node_pos = node_pos;
    }

    return pool_mgr_->mem_pool_[node_pos];
}

const u8* GtpMemPool::GetMemPoolStatus(void) {
    snprintf((char*)buffer_, sizeof(buffer_), "block_size=%u cap=%u used=%u malloc_num=%u free_num=%u "\
             "free_pos=%u", block_size_, pool_mgr_->pool_size_, pool_mgr_->using_num_, pool_mgr_->malloc_counter_,
             pool_mgr_->free_counter_, pool_mgr_->last_free_pos_);

    return &(buffer_[0]);
}

#ifdef __cplusplus
}
#endif

