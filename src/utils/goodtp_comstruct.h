#ifndef _GOOD_TP_COMSTRUCT_H_
#define _GOOD_TP_COMSTRUCT_H_
/*********************************************************************************************************************

                           Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_comstruce.h
  Version  : Initial
  Author   : Albort.Feng
  Function : Define the common data struct header file.
  Modify record:
  1.Date   : August 24, 2023
    Author : Albort.Feng
    Content: New creates file.

*********************************************************************************************************************/
#include "goodtp_macrodefine.h"
#include "goodtp.h"
#include "tranmempool.h"

#if (__linux__ || __APPLE__)
#include <netdb.h>
#include <arpa/inet.h>
#ifdef __linux__
#include <linux/tcp.h>
#endif
#ifdef __APPLE__
#include <sys/socket.h>
#endif
#include <pthread.h>
#endif

#if (_WIN32 || _WIN64)
#include <WS2tcpip.h>
#include <WinSock2.h>
#include <Windows.h>
#endif

#include <memory.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef float    f32;
typedef double   f64;

typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

inline u16 GtpReadU16Unaligned(const void *ptr) {
    u16 value = 0;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

inline u32 GtpReadU32Unaligned(const void *ptr) {
    u32 value = 0;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

inline u64 GtpReadU64Unaligned(const void *ptr) {
    u64 value = 0;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

inline f32 GtpReadF32Unaligned(const void *ptr) {
    f32 value = 0.0f;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

inline void GtpWriteU16Unaligned(void *ptr, const u16 &value) {
    memcpy(ptr, &value, sizeof(value));
}

inline void GtpWriteU32Unaligned(void *ptr, const u32 &value) {
    memcpy(ptr, &value, sizeof(value));
}

inline void GtpWriteU64Unaligned(void *ptr, const u64 &value) {
    memcpy(ptr, &value, sizeof(value));
}

inline void GtpWriteF32Unaligned(void *ptr, const f32 &value) {
    memcpy(ptr, &value, sizeof(value));
}

typedef u16 goodtp_pos;
typedef u8  encode_pos;

#if (_WIN32 || _WIN64)
#ifdef _WIN64
typedef i64 goodtp_sock;
#else
typedef i32 goodtp_sock;
#endif
typedef i32 goodtp_tid;
typedef i32 goodtp_pid;
#endif

#ifdef __linux__
#ifdef __i386__
typedef i32 goodtp_sock;
#else
typedef i64 goodtp_sock;
#endif
typedef pid_t goodtp_tid;
typedef i32 goodtp_pid;
#endif

#ifdef __APPLE__
#if (arm64 == ARCHS_STANDARD)
typedef i64 goodtp_sock;
#else
typedef i32 goodtp_sock;
#endif
typedef pid_t goodtp_tid;
typedef i32 goodtp_pid;
#endif

// TODO(Albert.Feng) :: now only support max length 4096 bytes udp packet because pack_size_ is only
// 12 bits.
#if (__linux__ || __APPLE__)
#define GTP_PACK_HEADER     \
    u32 goodtp_ver_:8;      \
    u32 header_offset_:6;   \
    u32 cache_us_flag_:1;   \
    u32 has_loss_flag_:1;   \
    u32 pack_type_:3;       \
    u32 pack_size_:13;      \
    u32 pack_sn_;           \
    u16 has_check_flag_:1;  \
    u16 has_ts_flag_:1;     \
    u16 has_rtt_flag_:1;    \
    u16 init_flag_:1;       \
    u16 repeat_counter_:3;  \
    u16 is_qos_flg_:1;      \
    u16 share_zone_:6;      \
    u16 has_chg_zone_:1;    \
    u16 stream_type_:1;     \
    u32 first_pack_sn_[0];  \
    u32 hash_[0];           \
    u32 user_id_[0];        \
    u32 rtt_us_[0];         \
    u64 ts_us_[0];          \
    f32 loss_[0];           \
    u8  padding[0];
#endif

#if (_WIN32 || _WIN64)
#define GTP_PACK_HEADER     \
    u32 goodtp_ver_:8;      \
    u32 header_offset_:6;   \
    u32 cache_us_flag_:1;   \
    u32 has_loss_flag_:1;   \
    u32 pack_type_:3;       \
    u32 pack_size_:13;      \
    u32 pack_sn_;           \
    u16 has_check_flag_:1;  \
    u16 has_ts_flag_:1;     \
    u16 has_rtt_flag_:1;    \
    u16 init_flag_:1;       \
    u16 repeat_counter_:3;  \
    u16 is_qos_flg_:1;      \
    u16 share_zone_:6;      \
    u16 has_chg_zone_:1;    \
    u16 stream_type_:1;
#endif

typedef u32 (*pDecodePacketCallBack)(void *pack, const u32 &size, u8 **out_frame, u32 *out_frame_size);

#pragma pack(1)

typedef struct _OldGtpHeader4Byte {
    u32 goodtp_ver_:8;
    u32 header_offset_:6;
    u32 cache_us_flag_:1;
    u32 has_loss_flag_:1;
    u32 pack_type_:4;
    u32 pack_size_:12;
}OldGtpHeader4Byte;

typedef struct _NewGtpHeader4Byte {
    u32 goodtp_ver_:8;
    u32 header_offset_:6;
    u32 cache_us_flag_:1;
    u32 has_loss_flag_:1;
    u32 pack_type_:3;
    u32 pack_size_:13;
}NewGtpHeader4Byte;

typedef struct _ChangeZone {
    u32 sort_sn_;
    u32 senter_ts_us_;
}ChangeZone;

#if (_WIN32 || _WIN64)
#pragma warning(disable:4200)
#endif
typedef struct _GtpPacket {
    GTP_PACK_HEADER   // pack_size_ is the whole packet's byte length.

    #if (__linux__ || __APPLE__)
    ChangeZone chg_zone_[0];
    #endif

    u8 payload_[0];
}GtpPacket;
#if (_WIN32 || _WIN64)
#pragma warning(default:)
#endif

typedef struct _GtpFeedbackFecModePack {
    GTP_PACK_HEADER
    u8  mode_ : 4;
    u8  horizonal_fec_flag_ : 1;
    u8  vertical_fec_flag_ : 1;
    u8  uphill_fec_flag_ : 1;
    u8  downhill_fec_flag_ : 1;
    u8  horizonal_power_ : 4;  // the power of 2
    u8  vertical_power_ : 4;   // the power of 2
}GtpFeedbackFecModePack;

typedef struct _GtpAckPacketLoad {
    u16 sn_size_;
    u16 tail_sn_offset_;
    u16 rto_sn_offset_;
    u32 head_sn_;
    u32 recv_loss_;

    // there is this memory field for windows, it's only for passing compiling.
    #if (__linux__ || __APPLE__)
    u64 cache_us_[0];
    u8  sn_bitmap_[0];
    #endif
}GtpAckPacketLoad;

typedef struct _GtpAckPacket {
    GTP_PACK_HEADER
    u16 sn_size_;
    u16 tail_sn_offset_;
    u16 rto_sn_offset_;
    u32 head_sn_;
    u32 recv_loss_;

    // there is this memory field for windows, it's only for passing compiling.
    #if (__linux__ || __APPLE__)
    u64 cache_us_[0];
    u8  sn_bitmap_[0];
    #endif
}GtpAckPacket;

typedef struct _GtpNackPacketLoad {
    u16 nack_num_;
    u16 tail_sn_offset_;
    u16 rto_sn_offset_;
    u32 head_sn_;
    u32 recv_loss_;

    // there is this memory field for windows, it's only for passing compiling.
    #if (__linux__ || __APPLE__)
    u64 cache_us_[0];
    u16 nack_[0];
    #endif
}GtpNackPacketLoad;

typedef struct _GtpNackPacket {
    GTP_PACK_HEADER
    u16 nack_num_;
    u16 tail_sn_offset_;
    u16 rto_sn_offset_;
    u32 head_sn_;
    u32 recv_loss_;

    // there is this memory field for windows, it's only for passing compiling.
    #if (__linux__ || __APPLE__)
    u64 cache_us_[0];
    u16 nack_[0];
    #endif
}GtpNackPacket;

typedef struct _ArqNode {
    struct _ArqNode *pre_node_;
    struct _ArqNode *nxt_node_;

    u64 last_send_ts_us_;

    u8 *pack_;
    u8 *tran_addr_;

    u16 pack_len_;

    u8  payload_offset_;
    u8  retran_counter_;
    u8  boost_threshold_;
    u8  try_agin_flg_:1;
    u8  boost_node_flg_:1;
    u8  failed_ack_:1;
    u8  has_boost_node_:1;
    u8  bit_rsv_:4;
    u8  byte_rsv_[2];

    u32 pack_sn_;
    u32 first_pack_sn_;
}ArqNode;

typedef struct _ArqList {
    ArqNode *head_;
    ArqNode *tail_;

    u32 node_num_;
    u32 ai_repair_sum_;
}ArqList;

typedef struct _GtpDateTime {
    u32 ulYear;    // begin frome 2000 years.
    u32 ulMonth;   // range[1, 12].
    u32 ulMday;    // range[1, 31].
    u32 ulHour;    // range[0, 23].
    u32 ulMin;     // range[0, 59].
    u32 ulSec;     // range[0, 59].
    u32 ulYDay;    // No. day number in one year.
    u32 ulMSec;
    u64 timestamp_us;
}GtpDateTime;

typedef struct _GtpSessionKey {
    _GtpSessionKey(const goodtp_sock &sfd, const u8 peer_bin_ip[], const u16 &peer_ip_size, const u16 &peer_bin_port,
                   const u8 self_bin_ip[], const u16 &self_ip_size, const u16 &self_bin_port):
        key_(0), sfd_(sfd), peer_ip_size_((u8)peer_ip_size), peer_bin_port_(peer_bin_port),
        self_ip_size_((u8)self_ip_size), self_bin_port_(self_bin_port) {
        memcpy(peer_bin_ip_, peer_bin_ip, (u32)peer_ip_size);

        if (0 != self_ip_size) {
            memcpy(self_bin_ip_, self_bin_ip, (u32)self_ip_size);
        } else {
            memset(self_bin_ip_, 0x00, sizeof(self_bin_ip_));
        }

        u16 swap_tmp = (u16)(sfd & 0x0000FFFF);
        u16 swap_sfd = 0;
        u16 loop     = 0;
        u16 bit_val  = 1;

        do {
            swap_sfd |= ((swap_tmp & bit_val) << (((7 - loop) << 1) + 1));

            bit_val <<= 1;
            loop     += 1;
        } while (8 > loop);

        loop = 0;

        do {
            swap_sfd |= ((swap_tmp & bit_val) >> ((loop << 1) + 1));

            bit_val <<= 1;
            loop     += 1;
        } while (8 > loop);

        key_   = (u64)swap_sfd;
        key_ <<= 48;

        u64 ip_value = 0;

        if (IPV4_BIN_IP_SIZE == peer_ip_size) {
            ip_value  = (u64)(GtpReadU32Unaligned(peer_bin_ip_));
            ip_value += (u64)(GtpReadU32Unaligned(self_bin_ip_));
            ip_value |= ((((u64)peer_bin_port_) + ((u64)self_bin_port_)) << 33);
        } else {
            for (u32 i = 0; 8 > i; ++i) {
                ip_value += ((u64)GtpReadU16Unaligned(peer_bin_ip_ + (i * sizeof(u16))));
            }

            for (u32 i = 0; 8 > i; ++i) {
                ip_value += ((u64)GtpReadU16Unaligned(self_bin_ip_ + (i * sizeof(u16))));
            }

            ip_value |= ((((u64)peer_bin_port_) + ((u64)self_bin_port_)) << 32);
        }

        key_         |= ip_value;
        key_3rd_flag_ = GTP_NO;
        bit_rsv_       = 0;

        return;
    }

    _GtpSessionKey(const u64 &stream_key, const goodtp_sock &sfd, const u8 peer_bin_ip[], const u16 &peer_ip_size,
               const u16 &peer_bin_port, const u8 self_bin_ip[], const u16 &self_ip_size, const u16 &self_bin_port):
        key_(stream_key), sfd_(sfd), peer_ip_size_((u8)peer_ip_size), peer_bin_port_(peer_bin_port),
        self_ip_size_((u8)self_ip_size), self_bin_port_(self_bin_port) {
        memcpy(peer_bin_ip_, peer_bin_ip, (u32)peer_ip_size);

        if (0 != self_ip_size) {
            memcpy(self_bin_ip_, self_bin_ip, (u32)self_ip_size);
        } else {
            memset(self_bin_ip_, 0x00, sizeof(self_bin_ip_));
        }

        key_3rd_flag_ = GTP_YES;
        bit_rsv_       = 0;

        return;
    }

    _GtpSessionKey(const u64 &stream_key):
        key_(stream_key), sfd_(-1), peer_ip_size_(0), peer_bin_port_(0), self_ip_size_(0), self_bin_port_(0) {
        memset(peer_bin_ip_, 0x00, IPV6_BIN_IP_SIZE);
        memset(self_bin_ip_, 0x00, IPV6_BIN_IP_SIZE);

        key_3rd_flag_ = GTP_YES;
        bit_rsv_       = 0;

        return;
    }

    _GtpSessionKey(const _GtpSessionKey &old_obj): key_(old_obj.key_), sfd_(old_obj.sfd_),
        key_3rd_flag_(old_obj.key_3rd_flag_),
        peer_ip_size_(old_obj.peer_ip_size_), peer_bin_port_(old_obj.peer_bin_port_),
        self_ip_size_(old_obj.self_ip_size_), self_bin_port_(old_obj.self_bin_port_) {
        memcpy(peer_bin_ip_, old_obj.peer_bin_ip_, IPV6_BIN_IP_SIZE);
        memcpy(self_bin_ip_, old_obj.self_bin_ip_, IPV6_BIN_IP_SIZE);
        bit_rsv_ = old_obj.bit_rsv_;
    }

    const _GtpSessionKey& operator =(const _GtpSessionKey &r_obj) {
        key_           = r_obj.key_;
        sfd_           = r_obj.sfd_;
        peer_bin_port_ = r_obj.peer_bin_port_;
        peer_ip_size_  = r_obj.peer_ip_size_;
        self_bin_port_ = r_obj.self_bin_port_;
        self_ip_size_  = r_obj.self_ip_size_;
        key_3rd_flag_  = r_obj.key_3rd_flag_;
        bit_rsv_       = r_obj.bit_rsv_;

        memcpy(peer_bin_ip_, r_obj.peer_bin_ip_, IPV6_BIN_IP_SIZE);
        memcpy(self_bin_ip_, r_obj.self_bin_ip_, IPV6_BIN_IP_SIZE);

        return *this;
    }

    bool operator ==(const _GtpSessionKey &r_obj) const {
        if (key_ != r_obj.key_) {
            return false;
        }

        if ((GTP_YES == key_3rd_flag_) && (GTP_YES == r_obj.key_3rd_flag_)) {
            return true;
        }

        if ((sfd_ == r_obj.sfd_) && (peer_bin_port_ == r_obj.peer_bin_port_) && (self_bin_port_ == r_obj.self_bin_port_)
         && (0 == memcmp((char*)peer_bin_ip_, (char*)(r_obj.peer_bin_ip_), peer_ip_size_))
         && ((0 == self_ip_size_) || (0 == memcmp((char*)self_bin_ip_, (char*)(r_obj.self_bin_ip_), self_ip_size_)))) {
            return true;
        }

        return false;
    }

    void KeyToString(u8 buf[], const u32 &buf_size) {
        u32 wrted_sz = 0;
        u32 unuse_sz = buf_size;
        u8 *wrt_pos  = buf;

        wrted_sz= (u32)snprintf((char*)wrt_pos, unuse_sz, "key=%llu sfd=%u peer_port=%u self_port=%u peer_addr_len=%u "\
                                "self_addr_len=%u peer_addr=", (unsigned long long)key_, (u32)sfd_, (u32)peer_bin_port_,
                                (u32)self_bin_port_, (u32)peer_ip_size_, (u32)self_ip_size_);
        if (wrted_sz >= unuse_sz) {
            return;
        }

        unuse_sz -= wrted_sz;
        wrt_pos  += wrted_sz;
        wrted_sz  = 0;

        u32 nloop = 0;
        while (((u32)peer_ip_size_) > nloop) {
            wrted_sz = (u32)snprintf((char*)wrt_pos, unuse_sz, "%02x ", peer_bin_ip_[nloop]);
            if (wrted_sz >= unuse_sz) {
                unuse_sz = 0;
                break;
            }

            unuse_sz -= wrted_sz;
            wrt_pos  += wrted_sz;
            wrted_sz  = 0;
            nloop    += 1;
        }

        if (0 == unuse_sz) {
            return;
        }

        wrted_sz= (u32)snprintf((char*)wrt_pos, unuse_sz, "self_addr=");
        if (wrted_sz >= unuse_sz) {
            return;
        }

        unuse_sz -= wrted_sz;
        wrt_pos  += wrted_sz;
        wrted_sz  = 0;

        nloop = 0;
        while (((u32)self_ip_size_) > nloop) {
            wrted_sz = (u32)snprintf((char*)wrt_pos, unuse_sz, "%02x ", self_bin_ip_[nloop]);
            if (wrted_sz >= unuse_sz) {
                unuse_sz = 0;
                break;
            }

            unuse_sz -= wrted_sz;
            wrt_pos  += wrted_sz;
            wrted_sz  = 0;
            nloop    += 1;
        }

        return;
    }

    u64 key_;

    u8  peer_bin_ip_[IPV6_BIN_IP_SIZE];
    u8  self_bin_ip_[IPV6_BIN_IP_SIZE];

    u8  peer_ip_size_;
    u8  self_ip_size_;
    u16 peer_bin_port_;
    u16 self_bin_port_;
    u16 key_3rd_flag_:1;  // GTP_NO: calced session key by udp quituple, GTP_YES: application setted sesion key.
    u16 bit_rsv_:15;

    goodtp_sock sfd_;
}GtpSessionKey;

typedef struct _GtpSessionKeyHash {
    u64 operator ()(const GtpSessionKey &session_key) const {
        return session_key.key_;
    }
}GtpSessionKeyHash;

// new fec
typedef struct _GtpFec2Mode {
    u8 mode_:4;
    u8 h_flag_:1;
    u8 v_flag_:1;
    u8 uh_flag_:1;
    u8 dh_flag_:1;

    u8 h_size_:4;
    u8 v_size_:4;

    u8 block_size_;
    u8 rcv_;
}GtpFec2Mode;

typedef struct _GtpPackCacheStru {
    goodtp_pos cache_id_;
    u16 payload_len_;
    u8  payload_[0];
}GtpPackCacheStru;

#if (_WIN32 || _WIN64)
#pragma warning(disable:4200)
#endif

typedef struct _Fec2CodePack {
    GTP_PACK_HEADER
    u8 code_book_id_;
    u8 encode_bit_map_;

    u8 fec_encode_dir_;  // 0: horizontal, 1: vertical, 2: uphill, 3: downhill(Fec2CodeDir)
    u8 fec_encode_pos_;

    u16 code_len_;

    u8 fec_code_[0];    // this position is align by 8 bytes.
}Fec2CodePack;

#if (_WIN32 || _WIN64)
#pragma warning(default:)
#endif

typedef struct _Fec2CodePackMgr {
    Fec2CodePack *fec_pack_;
    GtpAddr *tran_addr_;
}Fec2CodePackMgr;

typedef struct _Fec2EnDeCodeMatrix {
    _Fec2EnDeCodeMatrix() {
        using_flag_    = GTP_NO;
        code_book_id_  = 0;
        mode_          = 0;
        h_flag_        = 0;
        v_flag_        = 0;
        uh_flag_       = 0;
        dh_flag_       = 0;
        h_size_        = 0;
        v_size_        = 0;
        matrix_size_   = 0;
        start_pos_     = 0;
        start_pack_sn_ = 0;
        memset(rsv_byte_, 0x00, sizeof(rsv_byte_));

        u32 i;

        for (i = 0; MAX_FEC2_MATRIX_H_SIZE > i; ++i) {
            h_fec_code_[i].fec_pack_  = NULL;
            h_fec_code_[i].tran_addr_ = NULL;

            v_fec_code_[i].fec_pack_  = NULL;
            v_fec_code_[i].tran_addr_ = NULL;
        }

        for (i = 0; MAX_FEC2_HILL_SIZE > i; ++i) {
            uh_fec_code_[i].fec_pack_  = NULL;
            uh_fec_code_[i].tran_addr_ = NULL;

            dh_fec_code_[i].fec_pack_  = NULL;
            dh_fec_code_[i].tran_addr_ = NULL;
        }
    }

    _Fec2EnDeCodeMatrix(const GtpFec2Mode *code_book, const u32 &code_book_id) {
        using_flag_    = GTP_NO;
        code_book_id_  = (u8)code_book_id;
        mode_          = code_book[code_book_id].mode_;
        h_flag_        = code_book[code_book_id].h_flag_;
        v_flag_        = code_book[code_book_id].v_flag_;
        uh_flag_       = code_book[code_book_id].uh_flag_;
        dh_flag_       = code_book[code_book_id].dh_flag_;
        h_size_        = code_book[code_book_id].h_size_;
        v_size_        = code_book[code_book_id].v_size_;
        matrix_size_   = code_book[code_book_id].block_size_;
        start_pos_     = 0;
        start_pack_sn_ = 0;
        memset(rsv_byte_, 0x00, sizeof(rsv_byte_));

        u32 i;

        for (i = 0; MAX_FEC2_MATRIX_H_SIZE > i; ++i) {
            h_fec_code_[i].fec_pack_  = NULL;
            h_fec_code_[i].tran_addr_ = NULL;

            v_fec_code_[i].fec_pack_  = NULL;
            v_fec_code_[i].tran_addr_ = NULL;
        }

        for (i = 0; MAX_FEC2_HILL_SIZE > i; ++i) {
            uh_fec_code_[i].fec_pack_  = NULL;
            uh_fec_code_[i].tran_addr_ = NULL;

            dh_fec_code_[i].fec_pack_  = NULL;
            dh_fec_code_[i].tran_addr_ = NULL;
        }
    }

    void Init(const GtpFec2Mode *code_book, const u32 &code_book_id) {
        using_flag_    = GTP_NO;
        code_book_id_  = (u8)code_book_id;
        mode_          = code_book[code_book_id].mode_;
        h_flag_        = code_book[code_book_id].h_flag_;
        v_flag_        = code_book[code_book_id].v_flag_;
        uh_flag_       = code_book[code_book_id].uh_flag_;
        dh_flag_       = code_book[code_book_id].dh_flag_;
        h_size_        = code_book[code_book_id].h_size_;
        v_size_        = code_book[code_book_id].v_size_;
        matrix_size_   = code_book[code_book_id].block_size_;
        start_pos_     = 0;
        start_pack_sn_ = 0;

        u32 i;

        for (i = 0; MAX_FEC2_MATRIX_H_SIZE > i; ++i) {
            h_fec_code_[i].fec_pack_  = NULL;
            h_fec_code_[i].tran_addr_ = NULL;

            v_fec_code_[i].fec_pack_  = NULL;
            v_fec_code_[i].tran_addr_ = NULL;
        }

        for (i = 0; MAX_FEC2_HILL_SIZE > i; ++i) {
            uh_fec_code_[i].fec_pack_  = NULL;
            uh_fec_code_[i].tran_addr_ = NULL;

            dh_fec_code_[i].fec_pack_  = NULL;
            dh_fec_code_[i].tran_addr_ = NULL;
        }
    }

    void Init(const GtpFec2Mode *code_book, const u32 &code_book_id, TranMemPool &pack_mem_pool) {
        u32 i;

        for (i = 0; MAX_FEC2_MATRIX_H_SIZE > i; ++i) {
            if (NULL != h_fec_code_[i].fec_pack_) {
                pack_mem_pool.FreeTranBuf((u8*)(h_fec_code_[i].fec_pack_));
                h_fec_code_[i].fec_pack_  = NULL;
                h_fec_code_[i].tran_addr_ = NULL;
            }

            if (NULL != v_fec_code_[i].fec_pack_) {
                pack_mem_pool.FreeTranBuf((u8*)(v_fec_code_[i].fec_pack_));
                v_fec_code_[i].fec_pack_  = NULL;
                v_fec_code_[i].tran_addr_ = NULL;
            }
        }

        if ((GTP_NO == uh_flag_) && (GTP_NO == dh_flag_)) {
            goto recv_matrix_init_exit_pos_;
        }

        for (i = 0; MAX_FEC2_HILL_SIZE > i; ++i) {
            if (NULL != uh_fec_code_[i].fec_pack_) {
                pack_mem_pool.FreeTranBuf((u8*)(uh_fec_code_[i].fec_pack_));
                uh_fec_code_[i].fec_pack_  = NULL;
                uh_fec_code_[i].tran_addr_ = NULL;
            }

            if (NULL != dh_fec_code_[i].fec_pack_) {
                pack_mem_pool.FreeTranBuf((u8*)(dh_fec_code_[i].fec_pack_));
                dh_fec_code_[i].fec_pack_  = NULL;
                dh_fec_code_[i].tran_addr_ = NULL;
            }
        }

recv_matrix_init_exit_pos_:
        using_flag_    = GTP_NO;
        code_book_id_  = (u8)code_book_id;
        mode_          = code_book[code_book_id].mode_;
        h_flag_        = code_book[code_book_id].h_flag_;
        v_flag_        = code_book[code_book_id].v_flag_;
        uh_flag_       = code_book[code_book_id].uh_flag_;
        dh_flag_       = code_book[code_book_id].dh_flag_;
        h_size_        = code_book[code_book_id].h_size_;
        v_size_        = code_book[code_book_id].v_size_;
        matrix_size_   = code_book[code_book_id].block_size_;
        start_pos_     = 0;
        start_pack_sn_ = 0;

        return;
    }

    // describe: it can't clear fec buffer's packets that maybe belong to the current decode matrix.
    void Clear(TranMemPool &pack_mem_pool) {
        u32 i;

        for (i = 0; MAX_FEC2_MATRIX_H_SIZE > i; ++i) {
            if (NULL != h_fec_code_[i].fec_pack_) {
                pack_mem_pool.FreeTranBuf((u8*)(h_fec_code_[i].fec_pack_));
                h_fec_code_[i].fec_pack_  = NULL;
                h_fec_code_[i].tran_addr_ = NULL;
            }

            if (NULL != v_fec_code_[i].fec_pack_) {
                pack_mem_pool.FreeTranBuf((u8*)(v_fec_code_[i].fec_pack_));
                v_fec_code_[i].fec_pack_  = NULL;
                v_fec_code_[i].tran_addr_ = NULL;
            }
        }

        if ((GTP_NO == uh_flag_) && (GTP_NO == dh_flag_)) {
            goto recv_matrix_clear_exit_pos_;
        }

        for (i = 0; MAX_FEC2_HILL_SIZE > i; ++i) {
            if (NULL != uh_fec_code_[i].fec_pack_) {
                pack_mem_pool.FreeTranBuf((u8*)(uh_fec_code_[i].fec_pack_));
                uh_fec_code_[i].fec_pack_  = NULL;
                uh_fec_code_[i].tran_addr_ = NULL;
            }

            if (NULL != dh_fec_code_[i].fec_pack_) {
                pack_mem_pool.FreeTranBuf((u8*)(dh_fec_code_[i].fec_pack_));
                dh_fec_code_[i].fec_pack_  = NULL;
                dh_fec_code_[i].tran_addr_ = NULL;
            }
        }

recv_matrix_clear_exit_pos_:
        start_pos_     = 0;
        start_pack_sn_ = 0;
        using_flag_    = GTP_NO;

        return;
    }

    u32 PrintMatrix(u8 *out_str, const u32 &mem_size) {
        u8 *wrt_pos = out_str;
        u32 free_sz = mem_size;
        u32 str_len = 0;
        u32 wrt_num = 0;

        wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\nusing_flag= %u", (u32)using_flag_);
        PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

        wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n   book_id= %u", (u32)code_book_id_);
        PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

        wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n fec_mode_= %s", Fec2ModeToStr(mode_));
        PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

        wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n  code_dir= %u %u %u %u(0, 90, 45, -45)",
                                (u32)h_flag_, (u32)v_flag_, (u32)uh_flag_, (u32)dh_flag_);
        PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

        wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n  h_v_size= %u %u", (u32)h_size_, (u32)v_size_);
        PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

        wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n matrix_sz= %u", (u32)matrix_size_);
        PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

        wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n start_pos= %u", (u32)start_pos_);
        PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

        wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n  start_sn= %u", start_pack_sn_);
        PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

        return str_len;
    }

    u32 using_flag_:1;      // GTP_NO: no using, GTP_YES: using.
    u32 code_book_id_:7;
    u32 matrix_size_:8;
    u32 mode_:4;
    u32 h_flag_:1;
    u32 v_flag_:1;
    u32 uh_flag_:1;
    u32 dh_flag_:1;
    u32 h_size_:4;
    u32 v_size_:4;

    u32 start_pack_sn_;

    goodtp_pos start_pos_;  // the matrixt's first packet position in packet cache.
    u8  rsv_byte_[6];

    Fec2CodePackMgr h_fec_code_[MAX_FEC2_MATRIX_H_SIZE];
    Fec2CodePackMgr v_fec_code_[MAX_FEC2_MATRIX_V_SIZE];
    Fec2CodePackMgr uh_fec_code_[MAX_FEC2_HILL_SIZE];
    Fec2CodePackMgr dh_fec_code_[MAX_FEC2_HILL_SIZE];
}Fec2EnDeCodeMatrix;

typedef struct _HarqSnPair {
    u32 pack_sn_;
    u32 first_sn_;
}HarqSnPair;

typedef struct _Correction {
    CorrectionAct exp_act_;  // CorrectionAct
    CorrectionAct dec_act_;  // CorrectionAct
    u8 exp_k_;    // expand coefficient.
    u8 dec_k_;    // decrease coefficient.
}Correction;

typedef struct _LinkerStat {
    _LinkerStat(const u64 &create_ts_us):
        data_bitrate_sum_(0), data_bitrate_bkup_(0), net_bitrate_sum_(0), net_bitrate_bkup_(0),
        data_pack_sum_(0), data_pack_bkup_(0), net_pack_sum_(0), net_pack_bkup_(0),
        data_bitrate_bps_(0), net_bitrate_bps_(0), data_pack_pps_(0), net_pack_pps_(0),
        first_frame_sum_(0), ack_sum_(0), nack_sum_(0), retran_frame_sum_(0),
        first_frame_bkup_(0), retran_frame_bkup_(0), first_frame_pps_(0), retran_frame_pps_(0),
        last_calc_ts_us_((u32)create_ts_us) {
    }

    _LinkerStat(const _LinkerStat &a) :
        data_bitrate_sum_(a.data_bitrate_sum_),
        data_bitrate_bkup_(a.data_bitrate_bkup_),
        net_bitrate_sum_(a.net_bitrate_sum_),
        net_bitrate_bkup_(a.net_bitrate_bkup_),
        data_pack_sum_(a.data_pack_sum_),
        data_pack_bkup_(a.data_pack_bkup_),
        net_pack_sum_(a.net_pack_sum_),
        net_pack_bkup_(a.net_pack_bkup_),
        data_bitrate_bps_(a.data_bitrate_bps_),
        net_bitrate_bps_(a.net_bitrate_bps_),
        data_pack_pps_(a.data_pack_pps_),
        net_pack_pps_(a.net_pack_pps_),
        first_frame_sum_(a.first_frame_sum_),
        retran_frame_sum_(a.retran_frame_sum_),
        first_frame_bkup_(a.first_frame_bkup_),
        retran_frame_bkup_(a.retran_frame_bkup_),
        first_frame_pps_(a.first_frame_pps_),
        retran_frame_pps_(a.retran_frame_pps_),
        ack_sum_(a.ack_sum_),
        nack_sum_(a.nack_sum_),
        last_calc_ts_us_(a.last_calc_ts_us_) {
    }

    const _LinkerStat& operator =(const _LinkerStat &a) {
        data_bitrate_sum_  = a.data_bitrate_sum_;
        data_bitrate_bkup_ = a.data_bitrate_bkup_;
        net_bitrate_sum_   = a.net_bitrate_sum_;
        net_bitrate_bkup_  = a.net_bitrate_bkup_;
        data_pack_sum_     = a.data_pack_sum_;
        data_pack_bkup_    = a.data_pack_bkup_;
        net_pack_sum_      = a.net_pack_sum_;
        net_pack_bkup_     = a.net_pack_bkup_;
        data_bitrate_bps_  = a.data_bitrate_bps_;
        net_bitrate_bps_   = a.net_bitrate_bps_;
        data_pack_pps_     = a.data_pack_pps_;
        net_pack_pps_      = a.net_pack_pps_;
        first_frame_sum_   = a.first_frame_sum_;
        retran_frame_sum_  = a.retran_frame_sum_;
        first_frame_bkup_  = a.first_frame_bkup_;
        retran_frame_bkup_ = a.retran_frame_bkup_;
        first_frame_pps_   = a.first_frame_pps_;
        retran_frame_pps_  = a.retran_frame_pps_;
        ack_sum_           = a.ack_sum_;
        nack_sum_          = a.nack_sum_;
        last_calc_ts_us_   = a.last_calc_ts_us_;

        return *this;
    }

    void Calc(const u64 &cur_ts_us) {
        if (data_bitrate_bkup_ <= data_bitrate_sum_) {
            data_bitrate_bps_ = data_bitrate_sum_ - data_bitrate_bkup_;
        } else {
            data_bitrate_bps_ = data_bitrate_sum_ + (0xFFFFFFFF - data_bitrate_bkup_);
        }
        data_bitrate_bkup_  = data_bitrate_sum_;
        data_bitrate_bps_ <<= 3;  // sum is byte.

        if (net_bitrate_bkup_ <= net_bitrate_sum_) {
            net_bitrate_bps_ = net_bitrate_sum_ - net_bitrate_bkup_;
        } else {
            net_bitrate_bps_ = net_bitrate_sum_ + (0xFFFFFFFF - net_bitrate_bkup_);
        }
        net_bitrate_bkup_  = net_bitrate_sum_;
        net_bitrate_bps_ <<= 3;  // sum is byte.

        if (net_pack_bkup_ <= net_pack_sum_) {
            net_pack_pps_ = net_pack_sum_ - net_pack_bkup_;
        } else {
            net_pack_pps_ = net_pack_sum_ + (0xFFFFFFFF - net_pack_bkup_);
        }
        net_pack_bkup_ = net_pack_sum_;

        if (data_pack_bkup_ <= data_pack_sum_) {
            data_pack_pps_ = data_pack_sum_ - data_pack_bkup_;
        } else {
            data_pack_pps_ = data_pack_sum_ + (0xFFFFFFFF - data_pack_bkup_);
        }
        data_pack_bkup_ = data_pack_sum_;

        if (first_frame_bkup_ <= first_frame_sum_) {
            first_frame_pps_ = first_frame_sum_ - first_frame_bkup_;
        } else {
            first_frame_pps_ = first_frame_sum_ + (0xFFFFFFFF - first_frame_bkup_);
        }
        first_frame_bkup_ = first_frame_sum_;

        if (retran_frame_bkup_ <= retran_frame_sum_) {
            retran_frame_pps_ = retran_frame_sum_ - retran_frame_bkup_;
        } else {
            retran_frame_pps_ = retran_frame_sum_ + (0xFFFFFFFF - retran_frame_bkup_);
        }
        retran_frame_bkup_ = retran_frame_sum_;

        //corrects the pps value.
        if (((u32)(cur_ts_us & 0x00000000FFFFFFFF)) <= last_calc_ts_us_) {
            last_calc_ts_us_ = ((u32)(cur_ts_us & 0x00000000FFFFFFFF));
            return;
        }

        f32 delta_us = (f32)(((u32)(cur_ts_us & 0x00000000FFFFFFFF)) - last_calc_ts_us_);
        f32 k_factor = (f32)(1000000.001 / delta_us);

        data_pack_pps_    = (u32)(((f32)data_pack_pps_) * k_factor);
        net_pack_pps_     = (u32)(((f32)net_pack_pps_) * k_factor);
        first_frame_pps_  = (u32)(((f32)first_frame_pps_) * k_factor);
        retran_frame_pps_ = (u32)(((f32)retran_frame_pps_) * k_factor);
        data_bitrate_bps_ = (u32)(((f32)data_bitrate_bps_) * k_factor);
        net_bitrate_bps_  = (u32)(((f32)net_bitrate_bps_) * k_factor);

        last_calc_ts_us_ = ((u32)(cur_ts_us & 0x00000000FFFFFFFF));

        return;
    }

    u32 data_bitrate_sum_;
    u32 data_bitrate_bkup_;

    u32 net_bitrate_sum_;
    u32 net_bitrate_bkup_;

    u32 data_pack_sum_;
    u32 data_pack_bkup_;

    u32 net_pack_sum_;
    u32 net_pack_bkup_;

    u32 data_bitrate_bps_;
    u32 net_bitrate_bps_;

    u32 data_pack_pps_;
    u32 net_pack_pps_;

    u32 first_frame_sum_;
    u32 retran_frame_sum_;

    u32 first_frame_bkup_;
    u32 retran_frame_bkup_;
    u32 first_frame_pps_;
    u32 retran_frame_pps_;

    u32 ack_sum_;
    u32 nack_sum_;
    u32 last_calc_ts_us_;
}LinkerStat;

typedef struct _ConsumeTime {
    _ConsumeTime(const char *name) {
        sum_     = 0;
        counter_ = 0;
        ts_us_   = 0;
        avg_consume_time_ = 0;
        memset(name_, 0x00, 32);
        memcpy(name_, name, strlen((const char*)name));
    }

    _ConsumeTime(const _ConsumeTime &a) {
        sum_     = a.sum_;
        counter_ = a.counter_;
        ts_us_   = a.ts_us_;
        avg_consume_time_ = a.avg_consume_time_;
        memcpy(name_, a.name_, 32);
    }

    const _ConsumeTime& operator =(const _ConsumeTime &a) {
        sum_     = a.sum_;
        counter_ = a.counter_;
        ts_us_   = a.ts_us_;
        avg_consume_time_ = a.avg_consume_time_;
        memcpy(name_, a.name_, 32);
        return *this;
    }

    void CacheConsumeTime(const u64 &consume_time) {
        sum_     += consume_time;
        counter_ += 1;

        return;
    }

    u64 CalcAvgConsume(void) {
        if (0 != counter_) {
            avg_consume_time_ = sum_ / counter_;
        }

        counter_ = 0;
        sum_     = 0;

        return avg_consume_time_;
    }

    u8 name_[32];
    u64 ts_us_;
    u64 sum_;
    u64 counter_;
    u64 avg_consume_time_;
}ConsumeTime;

typedef struct _SessionPublicData {
    _SessionPublicData(const GtpHandler &gtp_hdl, const u64 &ts_us):
        min_delay_us_(0xFFFFFFFF),
        max_delay_us_(0),
        rtt_us_(0),
        clock_sync_delta_us_(0),
        self_port_(0),
        peer_port_(0),
        max_receive_pps_(MAX_SUPPORT_PPS),
        send_stat_(ts_us),
        recv_stat_(ts_us),
        gtp_hdl_(gtp_hdl),
        alg_top_switch_(GTP_ON),
        peer_version_(0xFF),
        max_send_loss_per_s_(FLOAT_ZERO),
        bakeup_send_loss_(FLOAT_ZERO),
        game_fec_policy_loss_(FLOAT_ZERO),
        session_(NULL),
        send_consume_("gtp_send"),
        recv_consume_("gtp_recv"),
        app_send_consume_("app_send"),
        app_recv_consume_("app_recv") {
        memset(self_ip_, 0x00, GTP_MAX_STR_IP_SZ);
        memset(peer_ip_, 0x00, GTP_MAX_STR_IP_SZ);
        memset(&tran_addr_, 0x00, sizeof(tran_addr_));
        bit_rsv_ = 0;
        memset(byte_rsv_, 0x00, sizeof(byte_rsv_));
        return;
    }

    _SessionPublicData(const _SessionPublicData &a):
        min_delay_us_(a.min_delay_us_),
        max_delay_us_(a.max_delay_us_),
        rtt_us_(a.rtt_us_),
        clock_sync_delta_us_(a.clock_sync_delta_us_),
        self_port_(a.self_port_),
        peer_port_(a.peer_port_),
        max_receive_pps_(a.max_receive_pps_),
        send_stat_(a.send_stat_),
        recv_stat_(a.recv_stat_),
        gtp_hdl_(a.gtp_hdl_),
        alg_top_switch_(a.alg_top_switch_),
        peer_version_(a.peer_version_),
        max_send_loss_per_s_(a.max_send_loss_per_s_),
        bakeup_send_loss_(a.bakeup_send_loss_),
        game_fec_policy_loss_(a.game_fec_policy_loss_),
        session_(a.session_),
        send_consume_(a.send_consume_),
        recv_consume_(a.recv_consume_),
        app_send_consume_(a.app_send_consume_),
        app_recv_consume_(a.app_recv_consume_) {
        memcpy(self_ip_, a.self_ip_, GTP_MAX_STR_IP_SZ);
        memcpy(peer_ip_, a.peer_ip_, GTP_MAX_STR_IP_SZ);

        memcpy(&tran_addr_, &(a.tran_addr_), sizeof(tran_addr_));
        bit_rsv_ = a.bit_rsv_;
        memcpy(byte_rsv_, a.byte_rsv_, sizeof(byte_rsv_));

        return;
    }

    const _SessionPublicData& operator =(const _SessionPublicData &a) {
        this->min_delay_us_        = a.min_delay_us_;
        this->max_delay_us_        = a.max_delay_us_;
        this->rtt_us_              = a.rtt_us_;
        this->clock_sync_delta_us_ = a.clock_sync_delta_us_;
        this->self_port_           = a.self_port_;
        this->peer_port_           = a.peer_port_;
        this->max_receive_pps_     = a.max_receive_pps_;
        this->send_stat_           = a.send_stat_;
        this->recv_stat_           = a.recv_stat_;
        this->gtp_hdl_             = a.gtp_hdl_;
        this->alg_top_switch_      = a.alg_top_switch_;
        this->peer_version_        = a.peer_version_;
        this->session_             = a.session_;
        this->max_send_loss_per_s_ = a.max_send_loss_per_s_;
        this->bakeup_send_loss_    = a.bakeup_send_loss_;
        this->game_fec_policy_loss_ = a.game_fec_policy_loss_;
        this->bit_rsv_             = a.bit_rsv_;
        this->send_consume_        = a.send_consume_;
        this->recv_consume_        = a.recv_consume_;
        this->app_send_consume_    = a.app_send_consume_;
        this->app_recv_consume_    = a.app_recv_consume_;

        memcpy(self_ip_, a.self_ip_, GTP_MAX_STR_IP_SZ);
        memcpy(peer_ip_, a.peer_ip_, GTP_MAX_STR_IP_SZ);
        memcpy(byte_rsv_, a.byte_rsv_, sizeof(byte_rsv_));

        memcpy(&tran_addr_, &(a.tran_addr_), sizeof(tran_addr_));

        return *this;
    }

    void UpdateMaxDelay(const u32 &cur_delay_us) {
        if (0x7FFFFFFF <= cur_delay_us) {
            return;  // calc abnormal.
        }

        if (max_delay_us_ < cur_delay_us) {
            max_delay_us_ = cur_delay_us;
        }
        return;
    }

    void UpdateMinDelay(const u32 &cur_delay_us) {
        if (min_delay_us_ > cur_delay_us) {
            min_delay_us_ = cur_delay_us;
        }
        return;
    }

    void CalcSysClockSync(const u64 &peer_ts_us, const u64 &self_ts_us) {
        u64 correct_self_ts = self_ts_us - ((u64)(rtt_us_ >> 1));
        if (peer_ts_us <= correct_self_ts) {
            clock_sync_delta_us_ = (i32)(correct_self_ts - peer_ts_us);
            if (10000 >= clock_sync_delta_us_) {
                clock_sync_delta_us_ = 0;
            }

            clock_sync_delta_us_ -= 10000;
            return;
        }

        clock_sync_delta_us_ = (i32)(peer_ts_us - correct_self_ts);
        if (10000 >= clock_sync_delta_us_) {
            clock_sync_delta_us_ = 0;
            return;
        }

        clock_sync_delta_us_ -= 10000;
        clock_sync_delta_us_  = ((i32)0) - clock_sync_delta_us_;
    }

    void UpdatedMaxLoss(const f32 &loss) {
        if (max_send_loss_per_s_ < loss) {
            max_send_loss_per_s_ = loss;
        }

        if (bakeup_send_loss_ < loss) {
            bakeup_send_loss_ = loss;
        }
    }

    void SecondTimerHandler(const u64 &ts_us) {
        send_stat_.Calc(ts_us);
        recv_stat_.Calc(ts_us);

        if (MAX_SUPPORT_PPS == max_receive_pps_) {
            max_receive_pps_ = recv_stat_.data_pack_pps_;
            return;
        }

        if (max_receive_pps_ < recv_stat_.data_pack_pps_) {
            max_receive_pps_ = recv_stat_.data_pack_pps_;
        }

        max_send_loss_per_s_ = bakeup_send_loss_;
        bakeup_send_loss_    = FLOAT_ZERO;

        #if (2 == APPLICATION_TYPE)
        if (max_send_loss_per_s_ > game_fec_policy_loss_) {
            game_fec_policy_loss_ = (max_send_loss_per_s_ * 0.80f) + (game_fec_policy_loss_ * 0.20f);
        } else {
            game_fec_policy_loss_ = (max_send_loss_per_s_ * 0.10f) + (game_fec_policy_loss_ * 0.90f);
        }
        #endif

        #if (1 == ENABLE_MD_PERF_CHECK)
        send_consume_.CalcAvgConsume();
        recv_consume_.CalcAvgConsume();
        app_send_consume_.CalcAvgConsume();
        app_recv_consume_.CalcAvgConsume();
        #endif

        return;
    }

    u32 min_delay_us_;
    u32 max_delay_us_;

    u32 rtt_us_;
    // it's system clock diffrent between the sender's system clock and the receiver's system clock.
    i32 clock_sync_delta_us_;

    GtpAddr tran_addr_;

    u16 self_port_;       // host sequence.
    u16 peer_port_;       // host sequence.

    u32 max_receive_pps_;

    LinkerStat send_stat_;
    LinkerStat recv_stat_;

    GtpHandler gtp_hdl_;
    u8 alg_top_switch_:1;
    u8 bit_rsv_:7;
    u8 peer_version_;
    u8 byte_rsv_[2];

    f32 max_send_loss_per_s_;
    f32 bakeup_send_loss_;
    f32 game_fec_policy_loss_;

    void *session_;

    ConsumeTime send_consume_;
    ConsumeTime recv_consume_;
    ConsumeTime app_send_consume_;
    ConsumeTime app_recv_consume_;

    u8  self_ip_[GTP_MAX_STR_IP_SZ];
    u8  peer_ip_[GTP_MAX_STR_IP_SZ];
}SessionPublicData;

#pragma pack()

inline u64 GtpMakeStreamKey(const u32 &hash, const u32 &user_id) {
    return ((((u64)user_id) << 32) | ((u64)hash));
}

inline void GtpSplitStreamKey(const u64 &stream_key, u32 *hash, u32 *user_id) {
    if (NULL != hash) {
        *hash = (u32)(stream_key & 0x00000000FFFFFFFFULL);
    }

    if (NULL != user_id) {
        *user_id = (u32)((stream_key >> 32) & 0x00000000FFFFFFFFULL);
    }
}

inline ChangeZone* GtpGetPacketChangeZone(GtpPacket *pack) {
    if ((NULL == pack) || (GTP_YES != pack->has_chg_zone_) || (pack->pack_size_ < pack->header_offset_)) {
        return NULL;
    }

    u32 min_header_offset = sizeof(GtpPacket) + sizeof(ChangeZone);
    if (GTP_YES == pack->has_loss_flag_) {
        min_header_offset += sizeof(f32);
    }

    if (GTP_YES == pack->has_check_flag_) {
        min_header_offset += sizeof(u64);
    }

    if (GTP_YES == pack->has_ts_flag_) {
        min_header_offset += sizeof(u64);
    }

    if (GTP_YES == pack->has_rtt_flag_) {
        min_header_offset += sizeof(u32);
    }

    if (0 != ((u8)(pack->repeat_counter_))) {
        min_header_offset += sizeof(u32);
    }

    if (min_header_offset > pack->header_offset_) {
        return NULL;
    }

    return (ChangeZone*)(((u8*)pack) + pack->header_offset_ - sizeof(ChangeZone));
}

inline const ChangeZone* GtpGetPacketChangeZone(const GtpPacket *pack) {
    return GtpGetPacketChangeZone((GtpPacket*)pack);
}

inline u64 GtpReadPacketStreamKey(const GtpPacket *pack) {
    if ((NULL == pack) || (GTP_YES != pack->has_check_flag_)) {
        return 0;
    }

    u8 *move = ((u8*)pack) + sizeof(GtpPacket);
    if (0 != ((u8)(pack->repeat_counter_))) {
        move += sizeof(u32);
    }

    const u32 hash = GtpReadU32Unaligned(move);
    move += sizeof(u32);

    const u32 user_id = GtpReadU32Unaligned(move);
    return GtpMakeStreamKey(hash, user_id);
}

inline u32 GtpInsertStreamKeyHeader(GtpPacket *pack, const u64 &stream_key) {
    if ((NULL == pack) || (0 == stream_key)) {
        return 0;
    }

    const u32 key_size = sizeof(u64);
    const u32 old_header_offset = pack->header_offset_;
    const u32 old_pack_size = pack->pack_size_;

    memmove(((u8*)pack) + old_header_offset + key_size, ((u8*)pack) + old_header_offset,
            old_pack_size - old_header_offset);

    u32 hash = 0;
    u32 user_id = 0;
    GtpSplitStreamKey(stream_key, &hash, &user_id);

    u8 *move = ((u8*)pack) + sizeof(GtpPacket);
    GtpWriteU32Unaligned(move, hash);
    move += sizeof(u32);
    GtpWriteU32Unaligned(move, user_id);

    pack->has_check_flag_ = GTP_YES;
    pack->header_offset_  = (u8)(old_header_offset + key_size);
    pack->pack_size_      = (u16)(old_pack_size + key_size);

    return key_size;
}

inline u32 GtpWriteStreamKeyHeader(GtpPacket *pack, const u64 &stream_key) {
    if ((NULL == pack) || (0 == stream_key)) {
        return 0;
    }

    u32 hash = 0;
    u32 user_id = 0;
    GtpSplitStreamKey(stream_key, &hash, &user_id);

    u8 *move = ((u8*)pack) + sizeof(GtpPacket);
    GtpWriteU32Unaligned(move, hash);
    move += sizeof(u32);
    GtpWriteU32Unaligned(move, user_id);

    pack->has_check_flag_ = GTP_YES;
    pack->header_offset_  = (u8)(sizeof(GtpPacket) + sizeof(u64));

    return sizeof(u64);
}

inline u32 GtpRemoveStreamKeyHeader(GtpPacket *pack) {
    if ((NULL == pack) || (GTP_YES != pack->has_check_flag_) || (sizeof(GtpPacket) > pack->header_offset_)) {
        return 0;
    }

    const u32 key_size = sizeof(u64);
    const u32 old_header_offset = pack->header_offset_;
    const u32 old_pack_size = pack->pack_size_;

    if ((sizeof(GtpPacket) + key_size) > old_header_offset) {
        return 0;
    }

    memmove(((u8*)pack) + old_header_offset - key_size, ((u8*)pack) + old_header_offset,
            old_pack_size - old_header_offset);

    pack->has_check_flag_ = GTP_NO;
    pack->header_offset_  = (u8)(old_header_offset - key_size);
    pack->pack_size_      = (u16)(old_pack_size - key_size);

    return key_size;
}

#define GtpHeaderOldToNew(pack) {\
    if (0x00 == (*((u8*)(pack)))) {\
        u32 tmp_org_value = GtpReadU32Unaligned(pack);\
        OldGtpHeader4Byte *old_header = (OldGtpHeader4Byte*)(&tmp_org_value);\
        NewGtpHeader4Byte *new_header = (NewGtpHeader4Byte*)(pack);\
        new_header->goodtp_ver_    = old_header->goodtp_ver_;\
        new_header->header_offset_ = old_header->header_offset_;\
        new_header->cache_us_flag_ = old_header->cache_us_flag_;\
        new_header->has_loss_flag_ = old_header->has_loss_flag_;\
        new_header->pack_type_     = old_header->pack_type_;\
        new_header->pack_size_     = old_header->pack_size_;\
    }\
}

#define GtpHeaderNewToOld(pack, peer_version) {\
    if (0x00 == (peer_version)) {\
        u32 tmp_org_value = GtpReadU32Unaligned(pack);\
        OldGtpHeader4Byte *old_header = (OldGtpHeader4Byte*)(pack);\
        NewGtpHeader4Byte *new_header = (NewGtpHeader4Byte*)(&tmp_org_value);\
        old_header->goodtp_ver_    = new_header->goodtp_ver_;\
        old_header->header_offset_ = new_header->header_offset_;\
        old_header->cache_us_flag_ = new_header->cache_us_flag_;\
        old_header->has_loss_flag_ = new_header->has_loss_flag_;\
        old_header->pack_type_     = new_header->pack_type_;\
        old_header->pack_size_     = new_header->pack_size_;\
    }\
}

#define CalcPosInPackCache(pack_sn) ((goodtp_pos)((pack_sn) & FEC2_CACHE_CAPACITY_MASK))

typedef u32 (*pFec2RestroreReceive) (void*, GtpHandler, GtpPacket*, GtpAddr*, u32*, u32*, u32*);

#if (1 == ENABLE_STACK_CHECK)
#define CheckThreadStackFreeSize(log_cb, md_id) {\
    u32 stack_size      = 0;\
    i64 stack_free_size = GtpGetStackFreeSize(&stack_size);\
    if (MIN_STACK_SIZE >= stack_free_size) {\
        GtpLog(log_cb, md_id, kGtpLogLevelCrit, "stack free size is too small(%lld bytes "\
               "stack_size=%u bytes).\r\n", (long long)stack_free_size, stack_size);\
    }\
}
#else
#define CheckThreadStackFreeSize(log_cb, md_id) {\
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif
