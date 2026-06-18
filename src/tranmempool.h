#ifndef _TRAN_MEM_POOL_H_
#define _TRAN_MEM_POOL_H_
/**********************************************************************************************************************

                  Copyright [2021~2031] Copyright (C), 2021-2031, Free Albort.Feng Studio

 **********************************************************************************************************************
  File name: mempool.h
  Version  : Init file
  Author   : Albert.Feng
  Gen date : 2021.08.04
  Last mdfy:
  Function : a managing the memory module which using about the data plane transporting in the transport platform.
  Func list:
  Mdf histroy:
  1.Date   : 2021.08.04
    Author : Albert.Feng
    Mdf ctx: creating file, the c3mempool isn't reentrant entry for the multi thread.

**********************************************************************************************************************/
#include "goodtp.h"
#include "goodtp_macrodefine.h"
#include <stdlib.h>
#include <memory.h>
#include <stdint.h>
#include <string>
#include <stdio.h>
#if (_WIN32 || _WIN64)
#pragma warning(disable:4061)
#pragma warning(disable:4100)
#pragma warning(disable:5045)

#include <time.h>
#include <shellapi.h>
#include <WS2tcpip.h>
#include <process.h>
#include <winSock2.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/time.h>
#endif

#define TIMEOUT_FREE_SWITCH         (0)

#define EXPAND_BUF_CAPACITY_STEP    (128)

#if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
#define MAX_256BYTES_DEFAULT_SIZE   (128)
#define MAX_512BYTES_DEFAULT_SIZE   (128)
#define MAX_1K_DEFAULT_SIZE         (128)
#define MAX_1DOT5K_DEFAULT_SIZE     (1024 << 10)
#define MAX_4K_DEFAULT_SIZE         (1024 << 6)
#define MAX_8K_DEFAULT_SIZE         (1024 << 4)
#define MAX_64K_DEFAULT_SIZE        (1024 << 1)
#endif

#if (2 == APPLICATION_TYPE)
#define MAX_256BYTES_DEFAULT_SIZE   (1024 << 10)
#define MAX_512BYTES_DEFAULT_SIZE   (1024 << 10)
#define MAX_1K_DEFAULT_SIZE         (1024 << 10)
#define MAX_1DOT5K_DEFAULT_SIZE     (1024 << 7)
#define MAX_4K_DEFAULT_SIZE         (1024 << 1)
#define MAX_8K_DEFAULT_SIZE         (1024 << 1)
#define MAX_64K_DEFAULT_SIZE        (1024 << 1)
#endif

#define HEADER_RSV_SIZE             (128)
#define TP_ADDR_RSV_SIZE            (128)
#define BUF_OFFSET_SIZE             (HEADER_RSV_SIZE + TP_ADDR_RSV_SIZE)
#define BUF_ALIGAN_SIZE             (64)
#define MIN_BUF_SIZE                (1)

#ifdef _UTTEST
#define MAX_USED_BUF_TIME_S         (1000000)
#else
#define MAX_USED_BUF_TIME_S         (3000000)
#endif

#define BUF_256_BYTES_SIZE          (256)
#define BUF_512_BYTES_SIZE          (512)
#define BUF_1K_SIZE                 (1024)
#define BUF_1DOT5_SIZE              (1536)
#define BUF_4K_SIZE                 (1024 << 2)
#define BUF_8K_SIZE                 (1024 << 3)
#define BUF_64K_SIZE                (1024 << 6)

#define InitBufElement(element, pool_type) {\
    (element)->mem_check_overlay_0 = 0x5a;\
    (element)->usedFlag            = 0;\
    (element)->bufType             = (uint8_t)(pool_type);\
    (element)->bitRsv              = 0x05;\
    (element)->referenceCounter    = 0;\
    (element)->current_pack_pos    = &((element)->tran_buf_mem[BUF_OFFSET_SIZE]);\
    (element)->current_malloc_pos  = &((element)->tran_buf_mem[BUF_OFFSET_SIZE]);\
    (element)->sub_last_malloc_pos = &((element)->tran_buf_mem[BUF_OFFSET_SIZE]);\
    (element)->used_time_us        = 0;\
    (element)->mem_check_overlay_1 = 0x5aa55aa5;\
}

enum class BufSizeType:unsigned char {
    kBuf256Bytes = 0,
    kBuf512Bytes = 1,
    kBuf1K       = 2,
    kBuf1dot5K   = 3,
    kBuf4K       = 4,
    kBuf8K       = 5,
    kBuf64K      = 6,

    kBufSizeTypeButt
};

#ifdef _UTTEST
#ifdef PRIVATE
#undef PRIVATE
#endif
#ifdef PROTECTED
#undef PROTECTED
#endif
#define PRIVATE        public
#define PROTECTED      public
#else
#ifdef PRIVATE
#undef PRIVATE
#endif
#ifdef PROTECTED
#undef PROTECTED
#endif
#define PRIVATE        private
#define PROTECTED      protected
#endif

class TranMemPool {
 public:
    typedef struct _TranMemPoolAddrInfo {
        uint8_t  type_;   // BufSizeType
        uint8_t  rsv_[3];
        uint32_t mem_pool_item_num_;
        void    *mem_pool_start_addr_;
        void    *mem_pool_end_addr_;
    }TranMemPoolAddrInfo;

PRIVATE:
    #pragma pack(1)
    typedef struct _TranbufElement {
        uint8_t mem_check_overlay_0;   // it's 0x5a forever, the others value means that the memory occured overflow.
        uint8_t usedFlag:1;            // 0: unused, 1: using
        uint8_t bufType:3;             // 0: 256bytes, 1: 512bytes, 2: 1K bytes, 3: 1.5K bytes, 4: 4K bytes,
                                       // 5: 8K bytes, 6: 64K bytes
        uint8_t bitRsv:4;              // it's 0x05 forever, the others value means that the memory occured overflow.

        int16_t referenceCounter;      // a counter for the memory is quoted, non zero means the memory is using and can't recovery.

        uint64_t used_time_us;         // the transport buffer used time length, the transport buffer will be recoveried automatically
                                       // when this time largs than 10 minutes.

        uint8_t *current_pack_pos;     // the pos which saved the current packet.
        uint8_t *current_malloc_pos;   // the pos which malloced the transport buffer.
        uint8_t *sub_last_malloc_pos;  // the pos which last malloced the transport buffer.

        uint32_t mem_check_overlay_1;  // it's 0x5aa55aa5 forever, the others value means that the memory occured overflow.

        #if (_WIN32 || _WIN64)
        #pragma warning(disable:4200)
        #endif
        uint8_t tran_buf_mem[0];
        #if (_WIN32 || _WIN64)
        #pragma warning(default:)
        #endif
    }TranbufElement;
    #pragma pack()

    typedef struct _TranBufMng {
        uint32_t  pool_size;
        uint8_t  *pool_header;         // a specifications transport memory pool's address header.
        uint8_t  *pool_tail;           // a specifications transport memory pool's address tailer.

        uint32_t  last_free_pos;
        uint32_t  bitmap_size;
        uint64_t *used_bitmap;

        uint32_t  used_num;                    // the transport buffer using sum number.
        uint32_t  free_num;                    // the transport buffer idle sum number.

        uint64_t  tran_buf_malloc_counter;     // the transport buffer malloced counter.
        uint64_t  tran_buf_free_counter;       // the transport buffer released counter.
        uint64_t  used_bit_abnormal_counter;   // the bitmap for marked the transport buffer used occured abnormal coutner.
        uint64_t  overlay_mem_counter;         // the memory overflows coutner.

        #if (_WIN32 || _WIN64)
        #pragma warning(disable:4200)
        #endif
        TranbufElement *tran_buf_pool[0];
        #if (_WIN32 || _WIN64)
        #pragma warning(default:)
        #endif
    }TranBufMng;

    typedef struct _RecoveryTranBuf {
        TranbufElement *tran_buf_element;
    }RecoveryTranBuf;

    typedef struct _RecoveryRingMem {
        uint32_t push_top_pos;
        uint32_t pop_bottom_pos;
        uint32_t ring_mem_size;

        uint64_t last_check_time;

        #if (_WIN32 || _WIN64)
        #pragma warning(disable:4200)
        #endif
        RecoveryTranBuf ring_mem_vec[0];
        #if (_WIN32 || _WIN64)
        #pragma warning(default:)
        #endif
    }RecoveryRingMem;

 public:
    TranMemPool(const uint32_t &pool_256bytes_sz = MAX_256BYTES_DEFAULT_SIZE,
                const uint32_t &pool_512bytes_sz = MAX_512BYTES_DEFAULT_SIZE,
                const uint32_t &pool_1k_sz = MAX_1K_DEFAULT_SIZE,
                const uint32_t &pool_1dot5k_sz = MAX_1DOT5K_DEFAULT_SIZE,
                const uint32_t &pool_4k_sz = MAX_4K_DEFAULT_SIZE,
                const uint32_t &pool_8k_sz = MAX_8K_DEFAULT_SIZE,
                const uint32_t &pool_64k_sz = MAX_64K_DEFAULT_SIZE):
              current_tran_buf_error_(""),
              tranmempool_stat_info_("") {
        buf_256bytes_pool_size_ = ((0 == pool_256bytes_sz) ? MIN_BUF_SIZE : pool_256bytes_sz);
        buf_512bytes_pool_size_ = ((0 == pool_512bytes_sz) ? MIN_BUF_SIZE : pool_512bytes_sz);
        buf_1k_pool_size_       = ((0 == pool_1k_sz) ? MIN_BUF_SIZE : pool_1k_sz);
        buf_1dot5k_pool_size_   = ((0 == pool_1dot5k_sz) ? MIN_BUF_SIZE : pool_1dot5k_sz);
        buf_4k_pool_size_       = ((0 == pool_4k_sz) ? MIN_BUF_SIZE : pool_4k_sz);
        buf_8k_pool_size_       = ((0 == pool_8k_sz) ? MIN_BUF_SIZE : pool_8k_sz);
        buf_64k_pool_size_      = ((0 == pool_64k_sz) ? MIN_BUF_SIZE : pool_64k_sz);

        if (buf_256bytes_pool_size_ > MAX_256BYTES_DEFAULT_SIZE) {
            buf_256bytes_pool_size_ = MAX_256BYTES_DEFAULT_SIZE;
        }

        if (buf_512bytes_pool_size_ > MAX_512BYTES_DEFAULT_SIZE) {
            buf_512bytes_pool_size_ = MAX_512BYTES_DEFAULT_SIZE;
        }

        if (buf_1k_pool_size_ > MAX_1K_DEFAULT_SIZE) {
            buf_1k_pool_size_ = MAX_1K_DEFAULT_SIZE;
        }

        if (buf_1dot5k_pool_size_ > MAX_1DOT5K_DEFAULT_SIZE) {
            buf_1dot5k_pool_size_ = MAX_1DOT5K_DEFAULT_SIZE;
        }

        if (buf_4k_pool_size_ > MAX_4K_DEFAULT_SIZE) {
            buf_4k_pool_size_ = MAX_4K_DEFAULT_SIZE;
        }

        if (buf_8k_pool_size_ > MAX_8K_DEFAULT_SIZE) {
            buf_8k_pool_size_ = MAX_8K_DEFAULT_SIZE;
        }

        if (buf_64k_pool_size_ > MAX_64K_DEFAULT_SIZE) {
            buf_64k_pool_size_ = MAX_64K_DEFAULT_SIZE;
        }

        buf_256bytes_pool_ = NULL;
        buf_512bytes_pool_ = NULL;
        buf_1k_pool_       = NULL;
        buf_1dot5k_pool_   = NULL;
        buf_4k_pool_       = NULL;
        buf_8k_pool_       = NULL;
        buf_64k_pool_      = NULL;
        recovery_          = NULL;

        buf_remalloc_sucess_counter_   = 0;
        buf_remalloc_failed_counter_   = 0;
        buf_malloc_failed_counter_     = 0;
        buf_timeout_free_counter_      = 0;
        average_used_time_per_item_us_ = 0.0;
    }

    ~TranMemPool() {
        if (NULL != buf_256bytes_pool_) {
            if (NULL != buf_256bytes_pool_->pool_header) {
                free(buf_256bytes_pool_->pool_header);
            }
            if (NULL != buf_256bytes_pool_->used_bitmap) {
                free(buf_256bytes_pool_->used_bitmap);
            }
            free(buf_256bytes_pool_);
            buf_256bytes_pool_ = NULL;
        }

        if (NULL != buf_512bytes_pool_) {
            if (NULL != buf_512bytes_pool_->pool_header) {
                free(buf_512bytes_pool_->pool_header);
            }
            if (NULL != buf_512bytes_pool_->used_bitmap) {
                free(buf_512bytes_pool_->used_bitmap);
            }
            free(buf_512bytes_pool_);
            buf_512bytes_pool_ = NULL;
        }

        if (NULL != buf_1k_pool_) {
            if (NULL != buf_1k_pool_->pool_header) {
                free(buf_1k_pool_->pool_header);
            }
            if (NULL != buf_1k_pool_->used_bitmap) {
                free(buf_1k_pool_->used_bitmap);
            }
            free(buf_1k_pool_);
            buf_1k_pool_ = NULL;
        }

        if (NULL != buf_1dot5k_pool_) {
            if (NULL != buf_1dot5k_pool_->pool_header) {
                free(buf_1dot5k_pool_->pool_header);
            }
            if (NULL != buf_1dot5k_pool_->used_bitmap) {
                free(buf_1dot5k_pool_->used_bitmap);
            }
            free(buf_1dot5k_pool_);
            buf_1dot5k_pool_ = NULL;
        }

        if (NULL != buf_4k_pool_) {
            if (NULL != buf_4k_pool_->pool_header) {
                free(buf_4k_pool_->pool_header);
            }
            if (NULL != buf_4k_pool_->used_bitmap) {
                free(buf_4k_pool_->used_bitmap);
            }
            free(buf_4k_pool_);
            buf_4k_pool_ = NULL;
        }

        if (NULL != buf_8k_pool_) {
            if (NULL != buf_8k_pool_->pool_header) {
                free(buf_8k_pool_->pool_header);
            }
            if (NULL != buf_8k_pool_->used_bitmap) {
                free(buf_8k_pool_->used_bitmap);
            }
            free(buf_8k_pool_);
            buf_8k_pool_ = NULL;
        }

        if (NULL != buf_64k_pool_) {
            if (NULL != buf_64k_pool_->pool_header) {
                free(buf_64k_pool_->pool_header);
            }
            if (NULL != buf_64k_pool_->used_bitmap) {
                free(buf_64k_pool_->used_bitmap);
            }
            free(buf_64k_pool_);
            buf_64k_pool_ = NULL;
        }

        if (NULL != recovery_) {
            free(recovery_);
            recovery_ = NULL;
        }
    }

    /*****************************************************************************************************************
    Name     : TranMemPoolStatInfo
    Function : get the transport memory pool's using information.
    In param : void
    Out param: void
    Return   : const std::string&

    Mdf history  :
    1.Date       : 2021.08.04
      Author     : Albert.Feng
      Mdf context: new function

    *****************************************************************************************************************/
    const std::string& TranMemPoolStatInfo(void) {
        // the summary information
        tranmempool_stat_info_ = std::string("           malloc sum : ")
                               + std::to_string(buf_256bytes_pool_->tran_buf_malloc_counter
                               + buf_512bytes_pool_->tran_buf_malloc_counter
                               + buf_1k_pool_->tran_buf_malloc_counter
                               + buf_1dot5k_pool_->tran_buf_malloc_counter
                               + buf_4k_pool_->tran_buf_malloc_counter + buf_8k_pool_->tran_buf_malloc_counter
                               + buf_64k_pool_->tran_buf_malloc_counter);

        tranmempool_stat_info_ += std::string("\r\n             free sum : ")
                               +  std::to_string(buf_256bytes_pool_->tran_buf_free_counter
                               +  buf_512bytes_pool_->tran_buf_free_counter
                               +  buf_1k_pool_->tran_buf_free_counter
                               +  buf_1dot5k_pool_->tran_buf_free_counter
                               +  buf_4k_pool_->tran_buf_free_counter + buf_8k_pool_->tran_buf_free_counter
                               +  buf_64k_pool_->tran_buf_free_counter);

        tranmempool_stat_info_ += std::string("\r\n    time out free sum : ")
                               +  std::to_string(buf_timeout_free_counter_);

        tranmempool_stat_info_ += std::string("\r\n    used average time : ")
                               +  std::to_string(average_used_time_per_item_us_)
                               +  std::string("us");

        tranmempool_stat_info_ += std::string("\r\n  remalloc sucess sum : ")
                               +  std::to_string(buf_remalloc_sucess_counter_);
        tranmempool_stat_info_ += std::string("\r\n  remalloc failed sum : ")
                               +  std::to_string(buf_remalloc_failed_counter_);
        tranmempool_stat_info_ += std::string("\r\n    malloc failed sum : ")
                               +  std::to_string(buf_malloc_failed_counter_);

        // every item memory information.
        tranmempool_stat_info_ += std::string("\r\n256 bytes memory pool : total size(")
                               +  std::to_string(buf_256bytes_pool_->pool_size)
                               +  std::string(") used size(") + std::to_string(buf_256bytes_pool_->used_num)
                               +  std::string(") free size(") + std::to_string(buf_256bytes_pool_->free_num)
                               +  std::string(") free pos(") + std::to_string(buf_256bytes_pool_->last_free_pos)
                               +  std::string(") markBitAbnormalNum(")
                               +  std::to_string(buf_256bytes_pool_->used_bit_abnormal_counter)
                               +  std::string(") outBoundMemNum(")
                               +  std::to_string(buf_256bytes_pool_->overlay_mem_counter) +  std::string(")");

        tranmempool_stat_info_ += std::string("\r\n512 bytes memory pool : total size(")
                               +  std::to_string(buf_512bytes_pool_->pool_size)
                               +  std::string(") used size(") + std::to_string(buf_512bytes_pool_->used_num)
                               +  std::string(") free size(") + std::to_string(buf_512bytes_pool_->free_num)
                               +  std::string(") free pos(") + std::to_string(buf_512bytes_pool_->last_free_pos)
                               +  std::string(") markBitAbnormalNum(")
                               +  std::to_string(buf_512bytes_pool_->used_bit_abnormal_counter)
                               +  std::string(") outBoundMemNum(")
                               +  std::to_string(buf_512bytes_pool_->overlay_mem_counter) +  std::string(")");

        tranmempool_stat_info_ += std::string("\r\n     1.0k memory pool : total size(")
                               +  std::to_string(buf_1k_pool_->pool_size)
                               +  std::string(") used size(") + std::to_string(buf_1k_pool_->used_num)
                               +  std::string(") free size(") + std::to_string(buf_1k_pool_->free_num)
                               +  std::string(") free pos(") + std::to_string(buf_1k_pool_->last_free_pos)
                               +  std::string(") markBitAbnormalNum(")
                               +  std::to_string(buf_1k_pool_->used_bit_abnormal_counter)
                               +  std::string(") outBoundMemNum(")
                               +  std::to_string(buf_1k_pool_->overlay_mem_counter) +  std::string(")");

        tranmempool_stat_info_ += std::string("\r\n     1.5k memory pool : total size(")
                               +  std::to_string(buf_1dot5k_pool_->pool_size)
                               +  std::string(") used size(") + std::to_string(buf_1dot5k_pool_->used_num)
                               +  std::string(") free size(") + std::to_string(buf_1dot5k_pool_->free_num)
                               +  std::string(") free pos(") + std::to_string(buf_1dot5k_pool_->last_free_pos)
                               +  std::string(") markBitAbnormalNum(")
                               +  std::to_string(buf_1dot5k_pool_->used_bit_abnormal_counter)
                               +  std::string(") outBoundMemNum(")
                               +  std::to_string(buf_1dot5k_pool_->overlay_mem_counter) +  std::string(")");

        tranmempool_stat_info_ += std::string("\r\n       4k memory pool : total size(")
                               +  std::to_string(buf_4k_pool_->pool_size)
                               +  std::string(") used size(") + std::to_string(buf_4k_pool_->used_num)
                               +  std::string(") free size(") + std::to_string(buf_4k_pool_->free_num)
                               +  std::string(") free pos(") + std::to_string(buf_4k_pool_->last_free_pos)
                               +  std::string(") markBitAbnormalNum(")
                               +  std::to_string(buf_4k_pool_->used_bit_abnormal_counter)
                               +  std::string(") outBoundMemNum(")
                               +  std::to_string(buf_4k_pool_->overlay_mem_counter) +  std::string(")");

        tranmempool_stat_info_ += std::string("\r\n       8k memory pool : total size(")
                               +  std::to_string(buf_8k_pool_->pool_size)
                               +  std::string(") used size(") + std::to_string(buf_8k_pool_->used_num)
                               +  std::string(") free size(") + std::to_string(buf_8k_pool_->free_num)
                               +  std::string(") free pos(") + std::to_string(buf_8k_pool_->last_free_pos)
                               +  std::string(") markBitAbnormalNum(")
                               +  std::to_string(buf_8k_pool_->used_bit_abnormal_counter)
                               +  std::string(") outBoundMemNum(")
                               +  std::to_string(buf_8k_pool_->overlay_mem_counter) +  std::string(")");

        tranmempool_stat_info_ += std::string("\r\n      64k memory pool : total size(")
                               +  std::to_string(buf_64k_pool_->pool_size)
                               +  std::string(") used size(") + std::to_string(buf_64k_pool_->used_num)
                               +  std::string(") free size(") + std::to_string(buf_64k_pool_->free_num)
                               +  std::string(") free pos(") + std::to_string(buf_8k_pool_->last_free_pos)
                               +  std::string(") markBitAbnormalNum(")
                               +  std::to_string(buf_64k_pool_->used_bit_abnormal_counter)
                               +  std::string(") outBoundMemNum(")
                               +  std::to_string(buf_64k_pool_->overlay_mem_counter) +  std::string(")");

        return tranmempool_stat_info_;
    }

    /*****************************************************************************************************************
    Name     : Init
    Function : initilizes the all type transport memory pools by structure parameters.
    In param : void
    Out param: void
    Return   : int32_t   // 0: sucessful, -1: failed, it need delete the transport memory pool's object to free the resource,
                         // and calls the Error() function to get the error info.
    Mdf history  :
    1.Date       : 2021.08.04
      Author     : Albert.Feng
      Mdf context: new function

    *****************************************************************************************************************/
    int32_t Init(void) {
        current_tran_buf_error_ = std::string("");

        int32_t nrunning_result = 0;
        uint32_t element_sum    = buf_1dot5k_pool_size_  + buf_4k_pool_size_ + buf_8k_pool_size_
                                + buf_64k_pool_size_;
        uint32_t mem_size = sizeof(RecoveryRingMem) + sizeof(RecoveryTranBuf) * element_sum;

        recovery_ = reinterpret_cast<RecoveryRingMem*>(malloc(mem_size));
        if (NULL == recovery_) {
            current_tran_buf_error_ = std::string("malloc memory failed for the recovery ring memory");
            return -1;
        }

        recovery_->push_top_pos    = 0;
        recovery_->pop_bottom_pos  = 0;
        recovery_->ring_mem_size   = element_sum;
        recovery_->last_check_time = 0;

        for (int32_t i = 0, j = (int32_t)(element_sum - 1); i <= j; ++i, --j) {  // NOLINT
            recovery_->ring_mem_vec[i].tran_buf_element     = NULL;
            recovery_->ring_mem_vec[j].tran_buf_element     = NULL;
        }

        nrunning_result = CreateTranBufPool(&buf_256bytes_pool_, BufSizeType::kBuf256Bytes, buf_256bytes_pool_size_);
        if (0 != nrunning_result) {
            return nrunning_result;
        }

        nrunning_result = CreateTranBufPool(&buf_512bytes_pool_, BufSizeType::kBuf512Bytes, buf_512bytes_pool_size_);
        if (0 != nrunning_result) {
            return nrunning_result;
        }

        nrunning_result = CreateTranBufPool(&buf_1k_pool_, BufSizeType::kBuf1K, buf_1k_pool_size_);
        if (0 != nrunning_result) {
            return nrunning_result;
        }

        nrunning_result = CreateTranBufPool(&buf_1dot5k_pool_, BufSizeType::kBuf1dot5K, buf_1dot5k_pool_size_);
        if (0 != nrunning_result) {
            return nrunning_result;
        }

        nrunning_result = CreateTranBufPool(&buf_4k_pool_, BufSizeType::kBuf4K, buf_4k_pool_size_);
        if (0 != nrunning_result) {
            return nrunning_result;
        }

        nrunning_result = CreateTranBufPool(&buf_8k_pool_, BufSizeType::kBuf8K, buf_8k_pool_size_);
        if (0 != nrunning_result) {
            return nrunning_result;
        }

        nrunning_result = CreateTranBufPool(&buf_64k_pool_, BufSizeType::kBuf64K, buf_64k_pool_size_);
        if (0 != nrunning_result) {
            return nrunning_result;
        }

        return 0;
    }

    /*****************************************************************************************************************
    Name     : Error
    Function : when calling any the function of the c3mempool class failed, calls this function to get detailed info.
    In param : void
    Out param: void
    Return   : std::string&
    Mdf history  :
    1.Date       : 2021.08.04
      Author     : Albert.Feng
      Mdf context: new function

    *****************************************************************************************************************/
    std::string& Error(void) {
        return current_tran_buf_error_;
    }

    /*****************************************************************************************************************
    Name     : TranBufUseRefAddOne
    Function : It only increases transport buffer's using refrence.
    In param : uint8_t* tran_buf  // NULL: a new mallocing transport buffer, the others: repeat mallocing transport
                                  // buffer on this.
               const uint32_t &using_counter
    Out param: void
    Return   : uint8_t*  // NULL: failed, the others: sucess.
    Mdf history  :
    1.Date       : 2023.11.20
      Author     : Albert.Feng
      Mdf context: new function

    *****************************************************************************************************************/
    uint8_t* TranBufUseRefAddOne(uint8_t* tran_buf, const uint32_t &using_counter = 1) {
        current_tran_buf_error_ = std::string("");

        uint32_t element_load_size  = 0;
        uint32_t pos_index          = 0;
        TranBufMng *mem_pool          = NULL;
        TranbufElement *element = (TranbufElement*)TranBufToElement(tran_buf, &element_load_size, NULL,
                                                                    &mem_pool, &pos_index);
        if (NULL == element) {
            current_tran_buf_error_ = std::string("Input the original memory isn't tran_buf memory in FreeTranBuf() function");
            return NULL;
        }

        element->referenceCounter += using_counter;

        return tran_buf;
    }

    /*****************************************************************************************************************
    Name     : MallocTranBuf
    Function : malloc the transport buffer including the new or repeat mallocing, if can't distribute the lesser 
               transport buffer, it will distribute the larger transport buffer, while it can't distribute the 64k 
               transport buffer, then return failed.
    In param : uint8_t* tran_buf  // NULL: a new mallocing transport buffer, the others: repeat mallocing transport 
                                  // buffer on this.
               uint32_t current_pack_size  // 0 for the new mallocing, used byte size for repeat mallocing.
               BufSizeType buf_size_type // the transport buffer type for the current mallocing.
    Out param: uint32_t *tran_buf_usable_size // the malloced transport buffer's byte size.
               void **tran_addr_mem  // this memory for saving the c3tp address info, it's the pair with transport buffer.
    Return   : uint8_t*  // NULL: failed, the others: sucess, it has been reserved 256 bytes before the return address.
    Mdf history  :
    1.Date       : 2021.08.04
      Author     : Albert.Feng
      Mdf context: new function

    *****************************************************************************************************************/
    uint8_t* MallocTranBuf(uint8_t* tran_buf, uint32_t current_pack_size, uint32_t *tran_buf_usable_size,
                           void **tran_addr_mem, BufSizeType buf_size_type = BufSizeType::kBuf1dot5K) {
        // current_tran_buf_error_ = std::string("");
        uint32_t remalloc_flag = 0;

        #if (1 == TIMEOUT_FREE_SWITCH)
        TimeoutRecoveryBuf();
        #endif

        if ((NULL == tran_buf_usable_size) || (NULL == tran_addr_mem)) {
            current_tran_buf_error_ = std::string("the inputparameter is invalid in MallocTranBuf() function");
            return NULL;
        }

        GtpAddr *tran_addr = NULL;

remalloc_pos_:
        if (NULL == tran_buf) {
            if (BufSizeType::kBufSizeTypeButt <= buf_size_type) {
                current_tran_buf_error_ = std::string("invalid tran_buf type(") + std::to_string((uint32_t)buf_size_type)
                                     + std::string(") when malloc new tran_buf in MallocTranBuf() function");
                return NULL;
            }

            uint8_t *new_buf = NULL;

            // priority distribute the special type transport buffer by caller, if distributs failedly then distributes the larger type.
            if (BufSizeType::kBuf256Bytes == buf_size_type) {
                new_buf = MallocTranBufElement(buf_256bytes_pool_);
                if (NULL != new_buf) {
                    *tran_buf_usable_size = BUF_256_BYTES_SIZE;
                    *tran_addr_mem        = new_buf - BUF_OFFSET_SIZE;
                    tran_addr             = (GtpAddr*)(*tran_addr_mem);

                    tran_addr->self_addr_len_ = 0;

                    return new_buf;
                }
                goto New512BytesBufPos_;
            }

            if (BufSizeType::kBuf512Bytes == buf_size_type) {
New512BytesBufPos_:
                new_buf = MallocTranBufElement(buf_512bytes_pool_);
                if (NULL != new_buf) {
                    *tran_buf_usable_size = BUF_512_BYTES_SIZE;
                    *tran_addr_mem        = new_buf - BUF_OFFSET_SIZE;
                    tran_addr             = (GtpAddr*)(*tran_addr_mem);

                    tran_addr->self_addr_len_ = 0;

                    return new_buf;
                }
                goto New1KBufPos_;
            }

            if (BufSizeType::kBuf1K == buf_size_type) {
New1KBufPos_:
                new_buf = MallocTranBufElement(buf_1k_pool_);
                if (NULL != new_buf) {
                    *tran_buf_usable_size = BUF_1K_SIZE;
                    *tran_addr_mem        = new_buf - BUF_OFFSET_SIZE;
                    tran_addr             = (GtpAddr*)(*tran_addr_mem);

                    tran_addr->self_addr_len_ = 0;

                    return new_buf;
                }
                goto New1dot5KBufPos_;
            }

            if (BufSizeType::kBuf1dot5K == buf_size_type) {
New1dot5KBufPos_:
                new_buf = MallocTranBufElement(buf_1dot5k_pool_);
                if (NULL != new_buf) {
                    *tran_buf_usable_size = BUF_1DOT5_SIZE;
                    *tran_addr_mem        = new_buf - BUF_OFFSET_SIZE;
                    tran_addr             = (GtpAddr*)(*tran_addr_mem);

                    tran_addr->self_addr_len_ = 0;

                    return new_buf;
                }
                goto New4KbufPos_;
            }

            if (BufSizeType::kBuf4K == buf_size_type) {
New4KbufPos_:
                new_buf = MallocTranBufElement(buf_4k_pool_);
                if (NULL != new_buf) {
                    *tran_buf_usable_size = BUF_4K_SIZE;
                    *tran_addr_mem        = new_buf - BUF_OFFSET_SIZE;
                    tran_addr             = (GtpAddr*)(*tran_addr_mem);

                    tran_addr->self_addr_len_ = 0;

                    return new_buf;
                }
                goto New8KbufPos_;
            }

            if (BufSizeType::kBuf8K == buf_size_type) {
New8KbufPos_:
                new_buf = MallocTranBufElement(buf_8k_pool_);
                if (NULL != new_buf) {
                    *tran_buf_usable_size = BUF_8K_SIZE;
                    *tran_addr_mem        = new_buf - BUF_OFFSET_SIZE;
                    tran_addr             = (GtpAddr*)(*tran_addr_mem);

                    tran_addr->self_addr_len_ = 0;

                    return new_buf;
                }
            }

            new_buf = MallocTranBufElement(buf_64k_pool_);
            if (NULL != new_buf) {
                *tran_buf_usable_size = BUF_64K_SIZE;
                *tran_addr_mem        = new_buf - BUF_OFFSET_SIZE;
                tran_addr             = (GtpAddr*)(*tran_addr_mem);

                tran_addr->self_addr_len_ = 0;

                return new_buf;
            }

            buf_malloc_failed_counter_ += 1;

            current_tran_buf_error_ = std::string("No any a usable tran_buf in the tran_buf's pool");
            return NULL;
        }

        uint32_t element_load_size  = 0;
        TranbufElement *tran_buf_element = (TranbufElement*)TranBufToElement(tran_buf, &element_load_size, NULL);
        if (NULL == tran_buf_element) {
            buf_remalloc_failed_counter_ += 1;
            current_tran_buf_error_ = std::string("Input the original memory isn't tran_buf memory "\
                                               "in MallocTranBuf() function");
            return NULL;
        }

        // cheches the original transport buffer is free memory space.
        if ((tran_buf_element->current_malloc_pos + BUF_OFFSET_SIZE + current_pack_size)
         > (&(tran_buf_element->tran_buf_mem[BUF_OFFSET_SIZE + element_load_size - 1]))) {
            if (0 == remalloc_flag) {
                #if (1 == TIMEOUT_FREE_SWITCH)
                TimeoutRecoveryBuf();
                #endif

                goto remalloc_pos_;
            }

            buf_remalloc_failed_counter_ += 1;
            current_tran_buf_error_ = std::string("No any usable memory in the original tran_buf in MallocTranBuf() function");
            return NULL;
        }

        tran_buf_element->referenceCounter += 1;

        *tran_addr_mem = tran_buf_element->current_malloc_pos + current_pack_size;
        tran_addr      = (GtpAddr*)(*tran_addr_mem);

        tran_addr->self_addr_len_ = 0;

        if (tran_buf_element->sub_last_malloc_pos != tran_buf_element->current_malloc_pos) {
            tran_buf_element->sub_last_malloc_pos = tran_buf_element->current_malloc_pos;
        }

        tran_buf_element->current_malloc_pos += (BUF_OFFSET_SIZE + current_pack_size);

        // reserve one byte for unusing.
        *tran_buf_usable_size = (uint32_t)((&(tran_buf_element->tran_buf_mem[BUF_OFFSET_SIZE + element_load_size - 1]))
                              - tran_buf_element->current_malloc_pos);

        buf_remalloc_sucess_counter_ += 1;

        return tran_buf_element->current_malloc_pos;
    }

    /*****************************************************************************************************************
    Name     : FreeTranBuf
    Function : free the transport buffer which has been malloced, the calling numbers of times for the MallocTranBuf() and
               the FreeTranBuf() are equale commonly, and no the transport buffer address isn't effected.
    In param : uint8_t* tran_buf_vec[]    // the any address by the returning address of the calling MallocTranBuf().
               const uint32_t &tran_buf_num
               pLogCallBack plog
    Out param: void
    Return   : void
    Mdf history  :
    1.Date       : 2023.11.16
      Author     : Albert.Feng
      Mdf context: new function

    *****************************************************************************************************************/
    void FreeTranBuf(uint8_t* tran_buf_vec[], const uint32_t &tran_buf_num, pLogCallBack plog = NULL) {
        if ((NULL == tran_buf_vec) || (0 == tran_buf_num)) {
            return;
        }

        current_tran_buf_error_ = std::string("");

        uint32_t element_load_size;
        uint32_t pos_index;
        uint32_t j;
        uint32_t used_time_us;
        uint32_t payload_size[] = {
            BUF_256_BYTES_SIZE, BUF_512_BYTES_SIZE, BUF_1K_SIZE, BUF_1DOT5_SIZE, BUF_4K_SIZE, BUF_8K_SIZE, BUF_64K_SIZE
        };
        uint32_t index;
        uint32_t tail_pos;
        uint64_t bit_mask;
        uint64_t used_time_length_us = GetCurrentSysTimeUs();
        TranBufMng *mem_pool;
        TranbufElement *element;

        j = 0;

        while (tran_buf_num > j) {
            element           = NULL;
            mem_pool          = NULL;
            pos_index         = 0;
            element_load_size = 0;

            element = (TranbufElement*)TranBufToElement(tran_buf_vec[j], &element_load_size, plog,
                                                        &mem_pool, &pos_index);
            if (NULL == element) {
                current_tran_buf_error_ = std::string("Input the original memory isn't tran_buf memory in FreeTranBuf() function");
                goto free_c3buf_vec_next_pos_;
            }

            if ((0 == element->usedFlag) || (0 >= element->referenceCounter)) {
                goto free_c3buf_vec_next_pos_;
            }

            element->referenceCounter -= 1;

            if (0 < element->referenceCounter) {
                goto free_c3buf_vec_next_pos_;
            }

            mem_pool->last_free_pos = pos_index;

            // the stat information.
            mem_pool->used_num           -= 1;
            mem_pool->free_num           += 1;
            mem_pool->tran_buf_free_counter += 1;

            bit_mask = 0x0000000000000001;
            bit_mask = ~(((uint64_t)bit_mask) << (pos_index & 0x0000003F));

            // the stat information.
            if (0 == ((*(mem_pool->used_bitmap + (pos_index >> 6))) & (~bit_mask))) {
                mem_pool->used_bit_abnormal_counter += 1;
            }

            index    = (uint32_t)((((uint32_t)(BufSizeType::kBuf64K)) >= element->bufType) ? element->bufType : 0);
            tail_pos = payload_size[index] + BUF_OFFSET_SIZE;

            // checking the overflowing of the memory.
            if ((0x5a != element->mem_check_overlay_0) || (0x5aa55aa5 != element->mem_check_overlay_1)
             || (0x05 != element->bitRsv)
             || (0xa5a5a5a55a5a5a5a != (*((uint64_t*)(&mem_pool->tran_buf_pool[pos_index]->tran_buf_mem[tail_pos]))))) {
                mem_pool->overlay_mem_counter += 1;

                element->mem_check_overlay_0 = 0x5a;
                element->bitRsv              = 0x05;
                element->mem_check_overlay_1 = 0x5aa55aa5;
                *((uint64_t*)(&mem_pool->tran_buf_pool[pos_index]->tran_buf_mem[tail_pos])) = 0xa5a5a5a55a5a5a5a;
            }

            *(mem_pool->used_bitmap + (pos_index >> 6)) &= bit_mask;

            used_time_us = (uint32_t)(used_time_length_us - element->used_time_us);
            average_used_time_per_item_us_ = (float)(average_used_time_per_item_us_ * 0.6 + used_time_us * 0.4);

            element->referenceCounter    = 0;
            element->usedFlag            = 0;
            element->current_pack_pos    = &(element->tran_buf_mem[BUF_OFFSET_SIZE]);
            element->current_malloc_pos  = &(element->tran_buf_mem[BUF_OFFSET_SIZE]);
            element->sub_last_malloc_pos = &(element->tran_buf_mem[BUF_OFFSET_SIZE]);
            element->used_time_us        = 0;

free_c3buf_vec_next_pos_:
            j += 1;
        }

        return;
    }

    /*****************************************************************************************************************
    Name     : FreeTranBuf
    Function : free the transport buffer which has been malloced, the calling numbers of times for the MallocTranBuf() and
               the FreeTranBuf() are equale commonly, and no the transport buffer address isn't effected.
    In param : uint8_t* tran_buf    // the any address by the returning address of the calling MallocTranBuf().
               pLogCallBack plog
    Out param: void
    Return   : void
    Mdf history  :
    1.Date       : 2021.08.04
      Author     : Albert.Feng
      Mdf context: new function

    *****************************************************************************************************************/
    void FreeTranBuf(uint8_t* tran_buf, pLogCallBack plog = NULL) {
        current_tran_buf_error_ = std::string("");

        uint32_t element_load_size  = 0;
        uint32_t pos_index          = 0;
        TranBufMng *mem_pool          = NULL;
        TranbufElement *element = (TranbufElement*)TranBufToElement(tran_buf, &element_load_size, plog, &mem_pool,
                                                              &pos_index);
        if (NULL == element) {
            current_tran_buf_error_ = std::string("Input the original memory isn't tran_buf memory in FreeTranBuf() function");
            return;
        }

        if ((0 == element->usedFlag) || (0 >= element->referenceCounter)) {
            return;
        }

        element->referenceCounter -= 1;

        if (0 < element->referenceCounter) {
            return;
        }

        mem_pool->last_free_pos = pos_index;

        // the stat information.
        mem_pool->used_num           -= 1;
        mem_pool->free_num           += 1;
        mem_pool->tran_buf_free_counter += 1;

        uint64_t bit_mask = 0x0000000000000001;
        bit_mask = ~(((uint64_t)bit_mask) << (pos_index & 0x0000003F));

        // the stat information.
        if (0 == ((*(mem_pool->used_bitmap + (pos_index >> 6))) & (~bit_mask))) {
            mem_pool->used_bit_abnormal_counter += 1;
        }

        uint32_t payload_size[] = {
            BUF_256_BYTES_SIZE, BUF_512_BYTES_SIZE, BUF_1K_SIZE, BUF_1DOT5_SIZE, BUF_4K_SIZE, BUF_8K_SIZE, BUF_64K_SIZE
        };
        uint32_t index          = (uint32_t)((((uint32_t)(BufSizeType::kBuf64K)) >= element->bufType) ? element->bufType : 0);
        uint32_t tail_pos       = payload_size[index] + BUF_OFFSET_SIZE;

        // checking the overflowing of the memory.
        if ((0x5a != element->mem_check_overlay_0) || (0x5aa55aa5 != element->mem_check_overlay_1)
         || (0x05 != element->bitRsv)
         || (0xa5a5a5a55a5a5a5a != (*((uint64_t*)(&mem_pool->tran_buf_pool[pos_index]->tran_buf_mem[tail_pos]))))) {
            mem_pool->overlay_mem_counter += 1;

            element->mem_check_overlay_0 = 0x5a;
            element->bitRsv              = 0x05;
            element->mem_check_overlay_1 = 0x5aa55aa5;
            *((uint64_t*)(&mem_pool->tran_buf_pool[pos_index]->tran_buf_mem[tail_pos])) = 0xa5a5a5a55a5a5a5a;
        }

        *(mem_pool->used_bitmap + (pos_index >> 6)) &= bit_mask;

        uint64_t used_time_length_us = GetCurrentSysTimeUs();

        used_time_length_us = used_time_length_us - element->used_time_us;
        average_used_time_per_item_us_ = (float)(average_used_time_per_item_us_ * 0.6 + used_time_length_us * 0.4);

        element->referenceCounter    = 0;
        element->usedFlag            = 0;
        element->current_pack_pos    = &(element->tran_buf_mem[BUF_OFFSET_SIZE]);
        element->current_malloc_pos  = &(element->tran_buf_mem[BUF_OFFSET_SIZE]);
        element->sub_last_malloc_pos = &(element->tran_buf_mem[BUF_OFFSET_SIZE]);
        element->used_time_us        = 0;

        return;
    }

    /*****************************************************************************************************************
    Name     : GetNextPack
    Function : Calling the GetNextPack() to get the next packet saving in a same transport buffer while there is
               many c3tp packets in this transport buffer.
    In param : uint8_t* current_pack_mem
               uint32_t current_pack_size
               void **tran_addr_mem
    Out param: void
    Return   : uint8_t*   // NULL: no any c3tp packet, the others: the header address of the c3tp packet.
    Mdf history  :
    1.Date       : 2021.08.04
      Author     : Albert.Feng
      Mdf context: new function

    *****************************************************************************************************************/
    uint8_t* GetNextPack(uint8_t* current_pack_mem, uint32_t current_pack_size, void **tran_addr_mem) {
        current_tran_buf_error_ = std::string("");

        if ((NULL == current_pack_mem) || (0 == current_pack_size) || (NULL == tran_addr_mem)) {
            current_tran_buf_error_ = std::string("The input parameter is invalid in GetNextPack() function");
            return NULL;
        }

        uint32_t element_load_size  = 0;
        TranbufElement *tran_buf_element = (TranbufElement*)TranBufToElement(current_pack_mem,
                                                                             &element_load_size, NULL);
        if (NULL == tran_buf_element) {
            current_tran_buf_error_ = std::string("Input the original memory isn't tran_buf memory "\
                                               "in GetNextPack() function");
            return NULL;
        }

        if (current_pack_mem != tran_buf_element->current_pack_pos) {
            current_tran_buf_error_ = std::string("The original packet's memory has been moved at outside"\
                                               " of the tran_buf in GetNextPack() function");
            return NULL;
        }

        uint8_t *next_pack = tran_buf_element->current_pack_pos + current_pack_size + BUF_OFFSET_SIZE;

        if (next_pack >= (&(tran_buf_element->tran_buf_mem[element_load_size + BUF_OFFSET_SIZE]))) {
            current_tran_buf_error_ = std::string("the pre packet's length greater than the tran_buf's length"\
                                               "in GetNextPack() function");
            return NULL;
        }

        *tran_addr_mem = tran_buf_element->current_pack_pos + current_pack_size;

        tran_buf_element->current_pack_pos = next_pack;

        return next_pack;
    }

    /*****************************************************************************************************************
    Name     : GetTranMemPoolAddrInfo
    Function : Getting the informations about the transport memory pools.
    In param : TranMemPoolAddrInfo *addr_256bytes
               TranMemPoolAddrInfo *addr_512bytes
               TranMemPoolAddrInfo *addr_1k
               TranMemPoolAddrInfo *addr_1dot5k
               TranMemPoolAddrInfo *addr_4k
               TranMemPoolAddrInfo *addr_8k
               TranMemPoolAddrInfo *addr_64k
    Out param: void
    Return   : int32_t   // 0: sucess, -1: failed, calling the Error() to get the detailed error information.
    Mdf history  :
    1.Date       : 2021.10.25
      Author     : Albert.Feng
      Mdf context: new function

    *****************************************************************************************************************/
    int32_t GetTranMemPoolAddrInfo(TranMemPoolAddrInfo *addr_256bytes, TranMemPoolAddrInfo *addr_512bytes,
                                   TranMemPoolAddrInfo *addr_1k, TranMemPoolAddrInfo *addr_1dot5k,
                                   TranMemPoolAddrInfo *addr_4k, TranMemPoolAddrInfo *addr_8k,
                                   TranMemPoolAddrInfo *addr_64k) {
        if ((NULL == addr_256bytes) || (NULL == addr_512bytes) || (NULL == addr_1k)
         || (NULL == addr_1dot5k) || (NULL == addr_4k) || (NULL == addr_8k) || (NULL == addr_64k)) {
            current_tran_buf_error_ = std::string("Parameter is invalid in the GetTranMemPoolAddrInfo() function");
            return -1;
        }

        addr_256bytes->type_                = (uint8_t)(BufSizeType::kBuf256Bytes);
        addr_256bytes->mem_pool_item_num_   = buf_256bytes_pool_size_;
        addr_256bytes->mem_pool_start_addr_ = buf_256bytes_pool_->pool_header;
        addr_256bytes->mem_pool_end_addr_   = buf_256bytes_pool_->pool_tail;

        addr_512bytes->type_                = (uint8_t)(BufSizeType::kBuf512Bytes);
        addr_512bytes->mem_pool_item_num_   = buf_512bytes_pool_size_;
        addr_512bytes->mem_pool_start_addr_ = buf_512bytes_pool_->pool_header;
        addr_512bytes->mem_pool_end_addr_   = buf_512bytes_pool_->pool_tail;

        addr_1k->type_                      = (uint8_t)(BufSizeType::kBuf1K);
        addr_1k->mem_pool_item_num_         = buf_1k_pool_size_;
        addr_1k->mem_pool_start_addr_       = buf_1k_pool_->pool_header;
        addr_1k->mem_pool_end_addr_         = buf_1k_pool_->pool_tail;

        addr_1dot5k->type_                  = (uint8_t)(BufSizeType::kBuf1dot5K);
        addr_1dot5k->mem_pool_item_num_     = buf_1dot5k_pool_size_;
        addr_1dot5k->mem_pool_start_addr_   = buf_1dot5k_pool_->pool_header;
        addr_1dot5k->mem_pool_end_addr_     = buf_1dot5k_pool_->pool_tail;

        addr_4k->type_                      = (uint8_t)(BufSizeType::kBuf4K);
        addr_4k->mem_pool_item_num_         = buf_4k_pool_size_;
        addr_4k->mem_pool_start_addr_       = buf_4k_pool_->pool_header;
        addr_4k->mem_pool_end_addr_         = buf_4k_pool_->pool_tail;

        addr_8k->type_                      = (uint8_t)(BufSizeType::kBuf8K);
        addr_8k->mem_pool_item_num_         = buf_8k_pool_size_;
        addr_8k->mem_pool_start_addr_       = buf_8k_pool_->pool_header;
        addr_8k->mem_pool_end_addr_         = buf_8k_pool_->pool_tail;

        addr_64k->type_                     = (uint8_t)(BufSizeType::kBuf64K);
        addr_64k->mem_pool_item_num_        = buf_64k_pool_size_;
        addr_64k->mem_pool_start_addr_      = buf_64k_pool_->pool_header;
        addr_64k->mem_pool_end_addr_        = buf_64k_pool_->pool_tail;

        return 0;
    }

    /*****************************************************************************************************************
    Name     : GetTranMemPoolUsedState
    Function : Getting the information about the using transport memory pools.
    In param : uint32_t *used_item_num  // outputting the zero means unused.
               uint32_t *free_item_num  // outputting the zero means no any useful transport buffer.
    Out param: void
    Return   : int32_t   // 0: sucess, -1: failed, calling the Error() to get the detailed error information.
    Mdf history  :
    1.Date       : 2021.10.26
      Author     : Albert.Feng
      Mdf context: new function

    *****************************************************************************************************************/
    int32_t GetTranMemPoolUsedState(uint32_t *used_item_num, uint32_t *free_item_num) {
        if ((NULL == used_item_num) || (NULL == free_item_num)) {
            current_tran_buf_error_ = std::string("Parameter is invalid in the GetTranMemPoolUsedState() function");
            return -1;
        }

        *used_item_num  = 0;
        *free_item_num  = 0;

        *used_item_num += buf_256bytes_pool_->used_num;
        *free_item_num += buf_256bytes_pool_->free_num;

        *used_item_num += buf_512bytes_pool_->used_num;
        *free_item_num += buf_512bytes_pool_->free_num;

        *used_item_num += buf_1k_pool_->used_num;
        *free_item_num += buf_1k_pool_->free_num;

        *used_item_num += buf_1dot5k_pool_->used_num;
        *free_item_num += buf_1dot5k_pool_->free_num;

        *used_item_num += buf_4k_pool_->used_num;
        *free_item_num += buf_4k_pool_->free_num;

        *used_item_num += buf_8k_pool_->used_num;
        *free_item_num += buf_8k_pool_->free_num;

        *used_item_num += buf_64k_pool_->used_num;
        *free_item_num += buf_64k_pool_->free_num;

        return 0;
    }

    // return: 0 is valid, others is invalid.
    uint32_t CheckAddrIsValid(uint8_t *mem_addr) {
        if (((mem_addr < buf_256bytes_pool_->pool_header) || (mem_addr > buf_256bytes_pool_->pool_tail))
         && ((mem_addr < buf_512bytes_pool_->pool_header) || (mem_addr > buf_512bytes_pool_->pool_tail))
         && ((mem_addr < buf_1k_pool_->pool_header) || (mem_addr > buf_1k_pool_->pool_tail))
         && ((mem_addr < buf_1dot5k_pool_->pool_header) || (mem_addr > buf_1dot5k_pool_->pool_tail))
         && ((mem_addr < buf_4k_pool_->pool_header) || (mem_addr > buf_4k_pool_->pool_tail))
         && ((mem_addr < buf_8k_pool_->pool_header) || (mem_addr > buf_8k_pool_->pool_tail))
         && ((mem_addr < buf_64k_pool_->pool_header) || (mem_addr > buf_64k_pool_->pool_tail))) {
            current_tran_buf_error_ = std::string("memory(") + std::to_string((uint64_t)mem_addr)
                                   +  std::string(") isn't belong to self momory pool[")
                                   +  std::to_string((uint64_t)(buf_1dot5k_pool_->pool_header)) +  std::string(", ")
                                   +  std::to_string((uint64_t)(buf_1dot5k_pool_->pool_tail)) + std::string("], [")
                                   +  std::to_string((uint64_t)(buf_4k_pool_->pool_header)) +  std::string(", ")
                                   +  std::to_string((uint64_t)(buf_4k_pool_->pool_tail)) + std::string("], [")
                                   +  std::to_string((uint64_t)(buf_8k_pool_->pool_header)) +  std::string(", ")
                                   +  std::to_string((uint64_t)(buf_8k_pool_->pool_tail)) + std::string("], [")
                                   +  std::to_string((uint64_t)(buf_64k_pool_->pool_header)) +  std::string(", ")
                                   +  std::to_string((uint64_t)(buf_64k_pool_->pool_tail)) + std::string("]");
            return 1;
        }

        return 0;
    }

PRIVATE:
    int32_t CreateTranBufPool(TranBufMng **tran_buf_mng_obj, BufSizeType pool_type, uint32_t pool_size) {
        if ((NULL == tran_buf_mng_obj) || (NULL != (*tran_buf_mng_obj)) || (0 == pool_size)
         || (BufSizeType::kBufSizeTypeButt <= pool_type)) {
            current_tran_buf_error_ = std::string("Parameter is invalid in the CreateTranBufPool() function");
            return -1;
        }

        uint32_t mem_size = 0;
        uint32_t i        = 0;
        uint32_t j        = 0;

        TranBufMng *temp_tran_buf_mng = NULL;

        // first: creating the management memory for the transport memory pool.
        mem_size       = sizeof(TranBufMng) + (sizeof(TranbufElement*) * pool_size);
        temp_tran_buf_mng = (TranBufMng*)malloc(mem_size);  // NOLINT
        if (NULL == temp_tran_buf_mng) {
            current_tran_buf_error_ = std::string("malloc memory failed for a tran_buf pool mng");
            return -1;
        }

        temp_tran_buf_mng->pool_size                 = pool_size;
        temp_tran_buf_mng->free_num                  = pool_size;
        temp_tran_buf_mng->used_num                  = 0;
        temp_tran_buf_mng->tran_buf_malloc_counter   = 0;
        temp_tran_buf_mng->used_bit_abnormal_counter = 0;
        temp_tran_buf_mng->tran_buf_free_counter     = 0;
        temp_tran_buf_mng->overlay_mem_counter       = 0;

        for (i = 0, j = pool_size - 1; j > i; --j, ++i) {
            temp_tran_buf_mng->tran_buf_pool[i] = NULL;
            temp_tran_buf_mng->tran_buf_pool[j] = NULL;
        }
        if (i == j) {
            temp_tran_buf_mng->tran_buf_pool[j] = NULL;
        }

        // second: creating the transport memory pool.
        uint32_t tran_buf_element_size = sizeof(TranbufElement) + BUF_OFFSET_SIZE;
        uint32_t tail_overlay_pos   = BUF_OFFSET_SIZE;
        switch (pool_type) {
        case BufSizeType::kBuf256Bytes:
        {
            tran_buf_element_size += BUF_256_BYTES_SIZE;
            tail_overlay_pos   += BUF_256_BYTES_SIZE;
        }
        break;

        case BufSizeType::kBuf512Bytes:
        {
            tran_buf_element_size += BUF_512_BYTES_SIZE;
            tail_overlay_pos   += BUF_512_BYTES_SIZE;
        }
        break;

        case BufSizeType::kBuf1K:
        {
            tran_buf_element_size += BUF_1K_SIZE;
            tail_overlay_pos   += BUF_1K_SIZE;
        }
        break;

        case BufSizeType::kBuf1dot5K:
        {
            tran_buf_element_size += BUF_1DOT5_SIZE;
            tail_overlay_pos   += BUF_1DOT5_SIZE;
        }
        break;

        case BufSizeType::kBuf4K:
        {
            tran_buf_element_size += BUF_4K_SIZE;
            tail_overlay_pos   += BUF_4K_SIZE;
        }
        break;

        case BufSizeType::kBuf8K:
        {
            tran_buf_element_size += BUF_8K_SIZE;
            tail_overlay_pos   += BUF_8K_SIZE;
        }
        break;

        default:
        {
            tran_buf_element_size += BUF_64K_SIZE;
            tail_overlay_pos   += BUF_64K_SIZE;
        }
        break;
        }

        tran_buf_element_size += 8;  // using for checking the overflow of the transport buffer.

        TranbufElement *element = NULL;
        uint32_t mem_sum_size = tran_buf_element_size * pool_size;

        element = (TranbufElement*)malloc(mem_sum_size);  // NOLINT
        if (NULL == element) {
            free(temp_tran_buf_mng);
            temp_tran_buf_mng = NULL;

            current_tran_buf_error_ = std::string("malloc memory failed for a tran_buf pool mng");
            return -1;
        }

        temp_tran_buf_mng->pool_header = (uint8_t*)element;
        temp_tran_buf_mng->pool_tail   = ((uint8_t*)element) + mem_sum_size;

        memset(element, 0x00, mem_sum_size);

        // third: cutting the larg memory to the transport buffer element.
        for (i = 0; pool_size > i; ++i) {
            temp_tran_buf_mng->tran_buf_pool[i] = (TranbufElement*)(((uint8_t*)element) + (tran_buf_element_size * i));

            *((uint64_t*)(&(temp_tran_buf_mng->tran_buf_pool[i]->tran_buf_mem[tail_overlay_pos]))) = 0xa5a5a5a55a5a5a5a;

            InitBufElement(temp_tran_buf_mng->tran_buf_pool[i], pool_type);
        }

        // fourth: distributing the memory for marking the status of the transport memory pool for using.
        temp_tran_buf_mng->bitmap_size = (temp_tran_buf_mng->pool_size >> 6);  // NOLINT 一个u64管理64个c3buf item
        temp_tran_buf_mng->used_bitmap = (uint64_t*)malloc(sizeof(uint64_t) * temp_tran_buf_mng->bitmap_size); // NOLINT
        if (NULL == temp_tran_buf_mng->used_bitmap) {
            free(temp_tran_buf_mng->pool_header);
            temp_tran_buf_mng->pool_header = NULL;

            free(temp_tran_buf_mng);
            temp_tran_buf_mng = NULL;

            current_tran_buf_error_ = std::string("malloc memory failed for a tran_buf pool mng");
            return -1;
        }

        memset(temp_tran_buf_mng->used_bitmap, 0x00, sizeof(uint64_t) * temp_tran_buf_mng->bitmap_size);

        temp_tran_buf_mng->last_free_pos = 0;

        *tran_buf_mng_obj = temp_tran_buf_mng;

        return 0;
    }

    uint8_t* MallocTranBufElement(TranBufMng *tran_buf_mng) {
        current_tran_buf_error_ = std::string("");

        if (NULL == tran_buf_mng) {
            return NULL;
        }

        uint8_t *tran_buf = NULL;
        TranbufElement *element = NULL;
        uint64_t bitmap_val   = 0;
        uint32_t pos_index    = (tran_buf_mng->last_free_pos >> 6);

        bitmap_val = *(tran_buf_mng->used_bitmap + pos_index);
        if (0xFFFFFFFFFFFFFFFF == bitmap_val) {
            for (pos_index = 0; tran_buf_mng->bitmap_size > pos_index; ++pos_index) {
                bitmap_val = *(tran_buf_mng->used_bitmap + pos_index);

                if (0xFFFFFFFFFFFFFFFF != bitmap_val) {
                    break;
                }
            }
        }

        if (0xFFFFFFFFFFFFFFFF == bitmap_val) {
            return NULL;
        }

        uint64_t tmp_bit_val = 0x0000000000000001;
        uint32_t j = 0;

        while (0 != (bitmap_val & tmp_bit_val)) {
            j += 1;
            tmp_bit_val = tmp_bit_val << 1;
        }

        *(tran_buf_mng->used_bitmap + pos_index) |= tmp_bit_val;

        pos_index = (pos_index << 6) + j;
        if (tran_buf_mng->pool_size <= pos_index) {
            return NULL;
        }
        tran_buf_mng->last_free_pos = pos_index;

        // the stat information.
        tran_buf_mng->used_num             += 1;
        tran_buf_mng->free_num             -= 1;
        tran_buf_mng->tran_buf_malloc_counter += 1;

        element = tran_buf_mng->tran_buf_pool[pos_index];

        // the stat information.
        if (0 != element->usedFlag) {
            tran_buf_mng->used_bit_abnormal_counter += 1;
        }

        uint32_t payload_size[] = {
            BUF_256_BYTES_SIZE, BUF_512_BYTES_SIZE, BUF_1K_SIZE, BUF_1DOT5_SIZE, BUF_4K_SIZE, BUF_8K_SIZE, BUF_64K_SIZE
        };
        uint32_t index          = (uint32_t)((((uint32_t)(BufSizeType::kBuf64K)) >= element->bufType) ? element->bufType : 0);
        uint32_t tail_pos       = payload_size[index] + BUF_OFFSET_SIZE;

        // checking the overflow of the transport buffer.
        if ((0x5a != element->mem_check_overlay_0) || (0x5aa55aa5 != element->mem_check_overlay_1)
         || (0x05 != element->bitRsv)
         || (0xa5a5a5a55a5a5a5a != (*((uint64_t*)(&tran_buf_mng->tran_buf_pool[pos_index]->tran_buf_mem[tail_pos]))))) {
            tran_buf_mng->overlay_mem_counter += 1;

            element->mem_check_overlay_0 = 0x5a;
            element->mem_check_overlay_1 = 0x5aa55aa5;
            element->bitRsv              = 0x05;
            *((uint64_t*)(&tran_buf_mng->tran_buf_pool[pos_index]->tran_buf_mem[tail_pos])) = 0xa5a5a5a55a5a5a5a;
        }

        element->usedFlag            = 1;
        element->referenceCounter    = 1;
        element->current_pack_pos    = &(tran_buf_mng->tran_buf_pool[pos_index]->tran_buf_mem[BUF_OFFSET_SIZE]);
        element->current_malloc_pos  = &(tran_buf_mng->tran_buf_pool[pos_index]->tran_buf_mem[BUF_OFFSET_SIZE]);
        element->sub_last_malloc_pos = &(tran_buf_mng->tran_buf_pool[pos_index]->tran_buf_mem[BUF_OFFSET_SIZE]);
        element->used_time_us         = GetCurrentSysTimeUs();

        tran_buf = tran_buf_mng->tran_buf_pool[pos_index]->current_malloc_pos;

        #if (1 == TIMEOUT_FREE_SWITCH)
        PushTranBufToRecovery(element);
        #endif

        return tran_buf;
    }

    void* TranBufToElement(uint8_t *tran_buf, uint32_t *element_load_size, pLogCallBack plog,
                           TranBufMng **mem_pool = NULL, uint32_t *pos_index = NULL) {
        current_tran_buf_error_ = std::string("");

        uint32_t tran_buf_element_size  = 0;
        uint32_t tran_buf_element_index = 0;

        if ((buf_256bytes_pool_->pool_header <= tran_buf) && (buf_256bytes_pool_->pool_tail > tran_buf)) {
            *element_load_size     = BUF_256_BYTES_SIZE;
            tran_buf_element_size  = sizeof(TranbufElement) + BUF_OFFSET_SIZE + BUF_256_BYTES_SIZE + 8;
            tran_buf_element_index = ((uint32_t)(tran_buf - buf_256bytes_pool_->pool_header)) / tran_buf_element_size;

            if (buf_256bytes_pool_->pool_size <= tran_buf_element_index) {
                return NULL;
            }

            if (NULL != mem_pool) {
                *mem_pool  = buf_256bytes_pool_;
            }

            if (NULL != pos_index) {
                *pos_index = tran_buf_element_index;
            }

            return buf_256bytes_pool_->tran_buf_pool[tran_buf_element_index];
        }

        if ((buf_512bytes_pool_->pool_header <= tran_buf) && (buf_512bytes_pool_->pool_tail > tran_buf)) {
            *element_load_size     = BUF_512_BYTES_SIZE;
            tran_buf_element_size  = sizeof(TranbufElement) + BUF_OFFSET_SIZE + BUF_512_BYTES_SIZE + 8;
            tran_buf_element_index = ((uint32_t)(tran_buf - buf_512bytes_pool_->pool_header)) / tran_buf_element_size;

            if (buf_512bytes_pool_->pool_size <= tran_buf_element_index) {
                return NULL;
            }

            if (NULL != mem_pool) {
                *mem_pool  = buf_512bytes_pool_;
            }

            if (NULL != pos_index) {
                *pos_index = tran_buf_element_index;
            }

            return buf_512bytes_pool_->tran_buf_pool[tran_buf_element_index];
        }

        if ((buf_1k_pool_->pool_header <= tran_buf) && (buf_1k_pool_->pool_tail > tran_buf)) {
            *element_load_size     = BUF_1K_SIZE;
            tran_buf_element_size  = sizeof(TranbufElement) + BUF_OFFSET_SIZE + BUF_1K_SIZE + 8;
            tran_buf_element_index = ((uint32_t)(tran_buf - buf_1k_pool_->pool_header)) / tran_buf_element_size;

            if (buf_1k_pool_->pool_size <= tran_buf_element_index) {
                return NULL;
            }

            if (NULL != mem_pool) {
                *mem_pool  = buf_1k_pool_;
            }

            if (NULL != pos_index) {
                *pos_index = tran_buf_element_index;
            }

            return buf_1k_pool_->tran_buf_pool[tran_buf_element_index];
        }

        if ((buf_1dot5k_pool_->pool_header <= tran_buf) && (buf_1dot5k_pool_->pool_tail > tran_buf)) {
            *element_load_size     = BUF_1DOT5_SIZE;
            tran_buf_element_size  = sizeof(TranbufElement) + BUF_OFFSET_SIZE + BUF_1DOT5_SIZE + 8;
            tran_buf_element_index = ((uint32_t)(tran_buf - buf_1dot5k_pool_->pool_header)) / tran_buf_element_size;

            if (buf_1dot5k_pool_->pool_size <= tran_buf_element_index) {
                return NULL;
            }

            if (NULL != mem_pool) {
                *mem_pool  = buf_1dot5k_pool_;
            }

            if (NULL != pos_index) {
                *pos_index = tran_buf_element_index;
            }

            return buf_1dot5k_pool_->tran_buf_pool[tran_buf_element_index];
        }

        if ((buf_4k_pool_->pool_header <= tran_buf) && (buf_4k_pool_->pool_tail > tran_buf)) {
            *element_load_size     = BUF_4K_SIZE;
            tran_buf_element_size  = sizeof(TranbufElement) + BUF_OFFSET_SIZE + BUF_4K_SIZE + 8;
            tran_buf_element_index = ((uint32_t)(tran_buf - buf_4k_pool_->pool_header)) / tran_buf_element_size;

            if (buf_4k_pool_->pool_size <= tran_buf_element_index) {
                return NULL;
            }

            if (NULL != mem_pool) {
                *mem_pool  = buf_4k_pool_;
            }

            if (NULL != pos_index) {
                *pos_index = tran_buf_element_index;
            }

            return buf_4k_pool_->tran_buf_pool[tran_buf_element_index];
        }

        if ((buf_8k_pool_->pool_header <= tran_buf) && (buf_8k_pool_->pool_tail > tran_buf)) {
            *element_load_size     = BUF_8K_SIZE;
            tran_buf_element_size  = sizeof(TranbufElement) + BUF_OFFSET_SIZE + BUF_8K_SIZE + 8;
            tran_buf_element_index = ((uint32_t)(tran_buf - buf_8k_pool_->pool_header)) / tran_buf_element_size;

            if (buf_8k_pool_->pool_size <= tran_buf_element_index) {
                return NULL;
            }

            if (NULL != mem_pool) {
                *mem_pool  = buf_8k_pool_;
            }

            if (NULL != pos_index) {
                *pos_index = tran_buf_element_index;
            }

            return buf_8k_pool_->tran_buf_pool[tran_buf_element_index];
        }

        if ((buf_64k_pool_->pool_header <= tran_buf) && (buf_64k_pool_->pool_tail > tran_buf)) {
            *element_load_size     = BUF_64K_SIZE;
            tran_buf_element_size  = sizeof(TranbufElement) + BUF_OFFSET_SIZE + BUF_64K_SIZE + 8;
            tran_buf_element_index = ((uint32_t)(tran_buf - buf_64k_pool_->pool_header)) / tran_buf_element_size;

            if (buf_64k_pool_->pool_size <= tran_buf_element_index) {
                return NULL;
            }

            if (NULL != mem_pool) {
                *mem_pool  = buf_64k_pool_;
            }

            if (NULL != pos_index) {
                *pos_index = tran_buf_element_index;
            }

            return buf_64k_pool_->tran_buf_pool[tran_buf_element_index];
        }

        if (NULL != plog) {
            plog(kGtpLogLevelError, "free's address(%p) isn't own([%p,%p), [%p,%p), [%p,%p), [%p,%p), "\
                 "[%p,%p), [%p,%p), [%p,%p)).\r\n", tran_buf,
                 buf_256bytes_pool_->pool_header, buf_256bytes_pool_->pool_tail,
                 buf_512bytes_pool_->pool_header, buf_512bytes_pool_->pool_tail,
                 buf_1k_pool_->pool_header, buf_1k_pool_->pool_tail,
                 buf_1dot5k_pool_->pool_header, buf_1dot5k_pool_->pool_tail,
                 buf_4k_pool_->pool_header, buf_4k_pool_->pool_tail,
                 buf_8k_pool_->pool_header, buf_8k_pool_->pool_tail,
                 buf_64k_pool_->pool_header, buf_64k_pool_->pool_tail);
        }

        return NULL;
    }

    void PushTranBufToRecovery(TranbufElement *tran_buf_element) {
        recovery_->ring_mem_vec[recovery_->push_top_pos].tran_buf_element = tran_buf_element;

        recovery_->push_top_pos += 1;
        if (recovery_->ring_mem_size <= recovery_->push_top_pos) {
            recovery_->push_top_pos = 0;
        }

        return;
    }

    void TimeoutRecoveryBuf(void) {
        uint64_t current_time_us = (uint64_t)GetCurrentSysTimeUs();
        #ifndef _UTTEST
        if (5000000 > (current_time_us - recovery_->last_check_time)) {
            return;
        }
        #endif

        uint32_t current_pos         = recovery_->pop_bottom_pos;
        uint32_t pre_pos             = 0;
        uint64_t used_time_length_us = 0;

        recovery_->last_check_time = current_time_us;

        while (recovery_->push_top_pos != current_pos) {
            if (NULL != recovery_->ring_mem_vec[current_pos].tran_buf_element) {
                if (MAX_USED_BUF_TIME_S > (current_time_us
                                                  - recovery_->ring_mem_vec[current_pos].tran_buf_element->used_time_us)) {
                    break;
                }

                if (0 != current_pos) {
                    pre_pos = current_pos - 1;
                } else {
                    pre_pos = recovery_->ring_mem_size - 1;
                }

                // no pop action when calling the FreeTranBuf(), at the same time removing the pushing
                // recording mark when multi pushing.
                if ((recovery_->push_top_pos != pre_pos)
                 && (NULL != recovery_->ring_mem_vec[pre_pos].tran_buf_element)
                 && ((recovery_->ring_mem_vec[pre_pos].tran_buf_element->used_time_us
                    > recovery_->ring_mem_vec[current_pos].tran_buf_element->used_time_us)
                  || (recovery_->ring_mem_vec[pre_pos].tran_buf_element
                   == recovery_->ring_mem_vec[current_pos].tran_buf_element))) {
                    recovery_->ring_mem_vec[pre_pos].tran_buf_element = NULL;
                }

                if (recovery_->ring_mem_vec[current_pos].tran_buf_element->current_pack_pos <
                    recovery_->ring_mem_vec[current_pos].tran_buf_element->sub_last_malloc_pos) {
                    used_time_length_us = current_time_us
                                        - recovery_->ring_mem_vec[current_pos].tran_buf_element->used_time_us;
                    average_used_time_per_item_us_ = (float)((float)(average_used_time_per_item_us_ * 0.6  // NOLINT
                                                   + (float)used_time_length_us * 0.4));  // NOLINT

                    recovery_->ring_mem_vec[current_pos].tran_buf_element->referenceCounter    = 0;
                    recovery_->ring_mem_vec[current_pos].tran_buf_element->usedFlag            = 0;
                    recovery_->ring_mem_vec[current_pos].tran_buf_element->used_time_us        = 0;
                    recovery_->ring_mem_vec[current_pos].tran_buf_element->current_pack_pos    =
                                    &(recovery_->ring_mem_vec[current_pos].tran_buf_element->tran_buf_mem[BUF_OFFSET_SIZE]);
                    recovery_->ring_mem_vec[current_pos].tran_buf_element->current_malloc_pos  =
                                    &(recovery_->ring_mem_vec[current_pos].tran_buf_element->tran_buf_mem[BUF_OFFSET_SIZE]);
                    recovery_->ring_mem_vec[current_pos].tran_buf_element->sub_last_malloc_pos =
                                    &(recovery_->ring_mem_vec[current_pos].tran_buf_element->tran_buf_mem[BUF_OFFSET_SIZE]);

                    recovery_->ring_mem_vec[current_pos].tran_buf_element = NULL;

                    buf_timeout_free_counter_ += 1;
                }
            }

            if (NULL == recovery_->ring_mem_vec[recovery_->pop_bottom_pos].tran_buf_element) {
                recovery_->pop_bottom_pos += 1;
                if (recovery_->ring_mem_size <= recovery_->pop_bottom_pos) {
                    recovery_->pop_bottom_pos = 0;
                }
            }

            current_pos += 1;
            if (recovery_->ring_mem_size <= current_pos) {
                current_pos = 0;
            }
        }

        return;
    }

    uint64_t GetCurrentSysTimeUs() {
        struct timeval current_date;

        gettimeofday(&current_date, NULL);

        return (uint64_t)(((uint64_t)(current_date.tv_sec)) * 1000000 + ((uint64_t)(current_date.tv_usec)));
    }

PRIVATE:
    #if (_WIN32 || _WIN64)
    int gettimeofday(struct timeval *tp, void *tzp) {
        struct tm tm;
        SYSTEMTIME wtm;
        GetLocalTime(&wtm);
        tm.tm_year   = wtm.wYear - 1900;
        tm.tm_mon   = wtm.wMonth - 1;
        tm.tm_mday   = wtm.wDay;
        tm.tm_hour   = wtm.wHour;
        tm.tm_min   = wtm.wMinute;
        tm.tm_sec   = wtm.wSecond;
        tm.tm_isdst  = -1;
        time_t clock = mktime(&tm);
        tp->tv_sec = (long)clock;  // NOLINT
        tp->tv_usec = wtm.wMilliseconds * 1000;
        return (0);
    }
    #endif

PRIVATE:
    uint32_t buf_256bytes_pool_size_;
    TranBufMng *buf_256bytes_pool_;

    uint32_t buf_512bytes_pool_size_;
    TranBufMng *buf_512bytes_pool_;

    uint32_t buf_1k_pool_size_;
    TranBufMng *buf_1k_pool_;

    uint32_t buf_1dot5k_pool_size_;
    TranBufMng *buf_1dot5k_pool_;

    uint32_t buf_4k_pool_size_;
    TranBufMng *buf_4k_pool_;

    uint32_t buf_8k_pool_size_;
    TranBufMng *buf_8k_pool_;

    uint32_t buf_64k_pool_size_;
    TranBufMng *buf_64k_pool_;

    std::string current_tran_buf_error_;
    std::string tranmempool_stat_info_;

    RecoveryRingMem *recovery_;

    uint64_t buf_remalloc_sucess_counter_;
    uint64_t buf_remalloc_failed_counter_;
    uint64_t buf_malloc_failed_counter_;
    uint64_t buf_timeout_free_counter_;     // the value will be zero under the normal env.
    float    average_used_time_per_item_us_;
};

#endif
