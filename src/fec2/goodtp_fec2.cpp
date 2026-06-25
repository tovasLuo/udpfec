/*********************************************************************************************************************

                           Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_fec2.cpp
  Version  : Initial
  Author   : Albert.Feng
  Function : The new fec realizing file for good transport platform.
  Modify record:
  1.Date   : June 26, 2024
    Author : Albert.Feng
    Content: New creates file.

*********************************************************************************************************************/
#include "goodtp_fec2.h"
#include "goodtp_mgr.h"
#include "goodtp_comstruct.h"
#include "goodtp_macrodefine.h"
#include "tranmempool.h"
#include "bitlinker.h"

#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <string.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

using namespace std;

#ifdef __cplusplus
extern "C" {
#endif

// @describe: block belong to [start_sn, end_sn) zoon.
// @return  : GTP_NO(outside of the block), GTP_YES(inside of the block),
inline u32 CheckPackSnInBlock(const u32 &pack_sn, const u32 &start_sn, const u32 &end_sn) {
    if (start_sn <= end_sn) {
        if ((start_sn > pack_sn) || (end_sn <= pack_sn)) {
            return GTP_NO;
        }

        return GTP_YES;
    }

    if ((start_sn > pack_sn) && (end_sn <= pack_sn)) {
        return GTP_NO;
    }

    return GTP_YES;
}

inline goodtp_pos CalcPackPosInMatrix(const goodtp_pos &cache_pos, const u32 &matrix_size,
                                      pLogCallBack write_log_cb) {
    if (0 == matrix_size) {
        GtpLog(write_log_cb, kGtpFecMd, kGtpLogLevelError, "Invalid matrix's size(%u)\r\n", matrix_size);
        return 0;
    }

    goodtp_pos matrix_pos = 0;

    switch (matrix_size) {
    case 2:
    case 4:
    case 8:
    case 16: {
        matrix_pos = (cache_pos & ((goodtp_pos)(matrix_size - 1)));
        break;
    }

    default: {
         GtpLog(write_log_cb, kGtpFecMd, kGtpLogLevelWarning, "Invalid matrix's size(%u)\r\n", matrix_size);
         matrix_pos = cache_pos % ((goodtp_pos)matrix_size);
    }
    }

    return matrix_pos;
}

// @return: -1(invalid), the others(valid)
inline u32 PackSnToBlockStartPos(const u32 &pack_sn, const u32 &block_size) {
    volatile u32 start_pos = CalcPosInPackCache(pack_sn);
    volatile u32 tmp_cache = 0;

    if (0 == block_size) {
        return start_pos;
    }

    switch (block_size) {
    case 2:
    case 4:
    case 8:
    case 16: {
        start_pos &= (~(block_size - 1));
        break;
    }

    default: {
        tmp_cache = start_pos / block_size;
        start_pos = tmp_cache * block_size;
    }
    }

    return start_pos;
}

inline encode_pos CalcHorizontalEncodePos(const Fec2EnDeCodeMatrix &matrix, const goodtp_pos &pos_in_matrix,
                                   pLogCallBack write_log_cb) {
    if (0 == matrix.h_size_) {
        GtpLog(write_log_cb, kGtpFecMd, kGtpLogLevelError, "Invalid matrix's h_size(%u)\r\n", (u32)(matrix.h_size_));
        return INVALID_ENCODE_POS;
    }

    if (((u32)(matrix.matrix_size_)) <= pos_in_matrix) {
        GtpLog(write_log_cb, kGtpFecMd, kGtpLogLevelError, "pos in matrix large matrix's size(pos=%u size=%u)\r\n",
               pos_in_matrix, (u32)(matrix.matrix_size_));
        return INVALID_ENCODE_POS;
    }

    encode_pos h_pos = 0;

    switch (matrix.h_size_) {
    case 2: {
        h_pos = (encode_pos)(pos_in_matrix >> 1);
        break;
    }

    case 4: {
        h_pos = (encode_pos)(pos_in_matrix >> 2);
        break;
    }

    case 8: {
        h_pos = (encode_pos)(pos_in_matrix >> 3);
        break;
    }

    default: {
        h_pos = (encode_pos)(pos_in_matrix / ((goodtp_pos)(matrix.h_size_)));
    }
    }

    return h_pos;
}

inline encode_pos CalcVerticalEncodePos(const Fec2EnDeCodeMatrix &matrix, const goodtp_pos &pos_in_matrix,
                                 pLogCallBack write_log_cb) {
    if (0 == matrix.h_size_) {
        GtpLog(write_log_cb, kGtpFecMd, kGtpLogLevelError, "Invalid matrix's v_size(%u)\r\n", (u32)(matrix.v_size_));
        return INVALID_ENCODE_POS;
    }

    if (((u32)(matrix.matrix_size_)) <= pos_in_matrix) {
        GtpLog(write_log_cb, kGtpFecMd, kGtpLogLevelError, "pos in matrix large matrix's size(pos=%u size=%u)\r\n",
               pos_in_matrix, (u32)(matrix.matrix_size_));
        return INVALID_ENCODE_POS;
    }

    encode_pos v_pos    = 0;
    goodtp_pos bit_mask = (goodtp_pos)(matrix.h_size_);

    switch (bit_mask) {
    case 2:
    case 4:
    case 8:
    case 16: {
        v_pos = (encode_pos)(pos_in_matrix & (bit_mask - 1));
        break;
    }

    default: {
        v_pos = (encode_pos)(pos_in_matrix % bit_mask);
    }
    }

    return v_pos;
}

inline u32 PackSnToStartSn(const u32 &pack_sn, const u32 &block_size) {
    if (0 == block_size) {
        return 0;
    }

    volatile u32 tmp_cache = 0;
    volatile u32 start_sn  = pack_sn;

    switch (block_size) {
    case 2:
    case 4:
    case 8:
    case 16: {
        start_sn &= (~(block_size - 1));
        break;
    }

    default: {
        tmp_cache = pack_sn / block_size;
        start_sn  = tmp_cache * block_size;
    }
    }

    return start_sn;
}

inline u32 PackSnToMatrixId(const u32 &pack_sn, const u32 &block_size) {
    if (0 == block_size) {
        return 0;
    }

    u32 matrix_id = (u32)CalcPosInPackCache(pack_sn);

    switch (block_size) {
    case 2: {
        matrix_id >>= 1;
        break;
    }

    case 4: {
        matrix_id >>= 2;
        break;
    }

    case 8: {
        matrix_id >>= 3;
        break;
    }

    case 16: {
        matrix_id >>= 4;
        break;
    }

    default: {
        matrix_id /= block_size;
    }
    }

    return matrix_id;
}

void XorEncode(u8 *data, const u32 &data_size, u8 *code, const u32 &code_size, u32 *out_coded_size) {
    u32 xor_num      = 0;
    u32 nloop        = 0;
    u32 result_size  = 0;
    u32 encoded_num  = 0;

    if (data_size > code_size) {
        result_size = data_size;

        // the encode memory must include the packet max size.
        memset(code + code_size, 0x00, data_size - code_size);
        goto xor_code_start_pos_;
    }

    if (data_size < code_size) {
        result_size = code_size;

        // the data memory maybe small.
        goto xor_code_start_pos_;
    }

    result_size = data_size;

xor_code_start_pos_:

#if defined(__SSE2__)
    // SSE2: 16 bytes per iteration (2× the 64-bit scalar path).
    // _mm_loadu / _mm_storeu handle unaligned pointers correctly.
    {
        u32 sse_loops = data_size >> 4;
        encoded_num   = sse_loops << 4;

        const __m128i *dp = (const __m128i*)data;
        __m128i       *cp = (__m128i*)code;

        for (nloop = 0; nloop < sse_loops; ++nloop) {
            _mm_storeu_si128(cp + nloop,
                _mm_xor_si128(_mm_loadu_si128(cp + nloop),
                              _mm_loadu_si128(dp + nloop)));
        }
    }
#elif defined(__ARM_NEON__)
    // NEON: 16 bytes per iteration.
    {
        u32 neon_loops = data_size >> 4;
        encoded_num    = neon_loops << 4;

        const uint8_t *dp = data;
        uint8_t       *cp = code;

        for (nloop = 0; nloop < neon_loops; ++nloop) {
            uint8x16_t dv = vld1q_u8(dp + (nloop << 4));
            uint8x16_t cv = vld1q_u8(cp + (nloop << 4));
            vst1q_u8(cp + (nloop << 4), veorq_u8(cv, dv));
        }
    }
#elif defined(__x86_64__) || defined(_M_X64)
    // 64-bit scalar fallback.
    {
        u64 *data_ptr = (u64*)data;
        u64 *code_ptr = (u64*)code;

        xor_num     = (data_size >> 3);
        encoded_num = (xor_num << 3);

        for (nloop = 0; nloop < xor_num; ++nloop) {
            code_ptr[nloop] ^= data_ptr[nloop];
        }
    }
#else
    // 32-bit scalar fallback.
    {
        u32 *data_ptr = (u32*)data;
        u32 *code_ptr = (u32*)code;

        xor_num     = (data_size >> 2);
        encoded_num = (xor_num << 2);

        for (nloop = 0; nloop < xor_num; ++nloop) {
            code_ptr[nloop] ^= data_ptr[nloop];
        }
    }
#endif

    // Tail bytes (0-15 remaining).
    while (data_size > encoded_num) {
        code[encoded_num] ^= data[encoded_num];
        encoded_num += 1;
    }

    *out_coded_size = result_size;

    return;
}

// @return: GTP_YES(in matrix), GTP_NO(out matrix)
inline u32 CheckCurSnIsInMatrix(const u32 &cur_sn, const Fec2EnDeCodeMatrix &matrix) {
    u32 start_sn = matrix.start_pack_sn_;
    u32 end_sn   = start_sn + matrix.matrix_size_;

    if (start_sn <= end_sn) {
        if ((start_sn <= cur_sn) && (end_sn > cur_sn)) {
            return GTP_YES;
        }
        return GTP_NO;
    }

    // sn has been turn over(end_sn < start_sn).
    if ((start_sn <= cur_sn) || (end_sn > cur_sn)) {
        return GTP_YES;
    }
    return GTP_NO;
}

inline goodtp_pos CalcFirstHorizontalPosInCache(const goodtp_pos &cache_pos, const encode_pos &v_pos) {
    if (((goodtp_pos)v_pos) <= cache_pos) {
        return (cache_pos - ((goodtp_pos)v_pos));
    }

    return ((cache_pos + MAX_FEC2_CACHE_CAPACITY) - ((goodtp_pos)v_pos));
}

inline goodtp_pos CalcFirstVerticalPosInCache(const goodtp_pos &cache_pos, const encode_pos &h_pos,
                                              const u8 &h_size) {
    goodtp_pos offset = ((goodtp_pos)h_size) * ((goodtp_pos)h_pos);

    if (offset <=  cache_pos) {
        return (cache_pos - offset);
    }

    return ((cache_pos + MAX_FEC2_CACHE_CAPACITY) - offset);
}

inline i32 DoCorrection(const volatile u32 &org_value, const u8 &k, const CorrectionAct &act) {
    volatile i32 new_value = (i32)org_value;

    switch (act) {
    case CorrectionAct::kZeroAct: {
        new_value = 0;
        break;
    }

    case CorrectionAct::kAddAct: {
        new_value = (i32)(org_value << k);
        break;
    }

    case CorrectionAct::kDecAct: {
        new_value = (i32)(org_value >> k);
        break;
    }

    default: {
    }
    }

    return new_value;
}

SendFec2CodeMatrix::SendFec2CodeMatrix(const GtpFec2Mode *code_book, const u32 &code_book_id,
                                       TranMemPool &pack_mem_pool, pLogCallBack write_log_cb) :
    encode_matrix_(code_book, code_book_id),
    pack_mem_pool_(pack_mem_pool),
    write_log_cb_(write_log_cb),
    code_book_(code_book),
    fec_buf_(pack_mem_pool, write_log_cb) {
}

SendFec2CodeMatrix::~SendFec2CodeMatrix() {
}

void SendFec2CodeMatrix::Init(void) {
    fec_buf_.Init();
    encode_matrix_.Init(code_book_, (u32)(encode_matrix_.code_book_id_), pack_mem_pool_);

    return;
}

void SendFec2CodeMatrix::ChangeFecMode(const u32 &new_code_book_id) {
    if (new_code_book_id != ((u32)(encode_matrix_.code_book_id_))) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelNotice, "change fec2 encode book id from %u to %u.\r\n",
               (u32)(encode_matrix_.code_book_id_), new_code_book_id);
    }

    encode_matrix_.code_book_id_ = (u8)new_code_book_id;
    Init();

    return;
}

RecvFec2CodeMatrix::RecvFec2CodeMatrix(const GtpFec2Mode *code_book, const u32 &code_book_id,
                                       TranMemPool &pack_mem_pool, pLogCallBack write_log_cb) :
    code_book_id_((u8)code_book_id),
    fec_res_sucess_sum_(0),
    fec_res_failed_sum_(0),
    fec_res_repeat_sum_(0),
    fec_buf_(pack_mem_pool, write_log_cb),
    code_book_(code_book),
    pack_mem_pool_(pack_mem_pool),
    write_log_cb_(write_log_cb) {
    u32 loop = 0;

    while (MAX_RECV_FEC2_MATRIX_NUM > loop) {
        decode_matrix_[loop].Init(code_book, code_book_id);
        loop += 1;
    }

    rsv_[0] = 0;
    rsv_[1] = 0;
    rsv_[2] = 0;
}

RecvFec2CodeMatrix::~RecvFec2CodeMatrix() {
}

void RecvFec2CodeMatrix::Init(void) {
    fec_buf_.Init();

    u32 i;

    for (i = 0; MAX_RECV_FEC2_MATRIX_NUM > i; ++i) {
        decode_matrix_[i].Init(code_book_, (u32)code_book_id_, pack_mem_pool_);
    }

    return;
}

// clear the decode and the packet buffer resource.
void RecvFec2CodeMatrix::ClearSpsMatrixResource(Fec2EnDeCodeMatrix &decode_matrix) {
    if (GTP_NO == decode_matrix.using_flag_) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelNotice, "clear a unusing decode matrix.\r\n");
        return;
    }

    goodtp_pos start_pos = decode_matrix.start_pos_;
    goodtp_pos end_pos   = (decode_matrix.start_pos_ + ((goodtp_pos)(decode_matrix.matrix_size_))) - 1;

    end_pos &= ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);

    fec_buf_.Clear(start_pos, end_pos);

    decode_matrix.Clear(pack_mem_pool_);

    return;
}

u32 RecvFec2CodeMatrix::PrintParameter(u8 *out_str, const u32 &mem_size) {
    u8 *wrt_pos = out_str;
    u32 free_sz = mem_size;
    u32 str_len = 0;
    u32 wrt_num = 0;
    u32 loop    = 0;

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n fec_res_sucess_sum= %u", fec_res_sucess_sum_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n fec_res_repeat_sum= %u", fec_res_repeat_sum_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n fec_res_to_app_sum= %u",
                            (fec_res_sucess_sum_ - fec_res_repeat_sum_));
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n fec_res_failed_sum= %u", fec_res_failed_sum_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    if ((NULL != g_goodtp_inst_mgr.cur_log_level_cb_)
     && (kGtpLogLevelNotice <= g_goodtp_inst_mgr.cur_log_level_cb_())) {
        loop = 0;

        do {
            if (GTP_NO == decode_matrix_[loop].using_flag_) {
                goto recv_fec2_alg_print_next_pos_;
            }

            wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\nNo.%02u block:", loop);
            PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

            wrt_num = decode_matrix_[loop].PrintMatrix(wrt_pos, free_sz);
            PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

recv_fec2_alg_print_next_pos_:
            loop += 1;
        } while (MAX_RECV_FEC2_MATRIX_NUM > loop);
    }

    return str_len;
}

GtpFec2::GtpFec2(pSendPackCallBack send_pack_cb, pLogCallBack write_log_cb, TranMemPool &pack_mem_pool,
                 pFec2RestroreReceive receive_frame_cb, const GtpFec2Mode *fec_code_book):
    pack_mem_pool_(pack_mem_pool),
    send_pack_cb_(send_pack_cb),
    write_log_cb_(write_log_cb),
    restorePackRecv_cb_(receive_frame_cb),
    pb_dt_(NULL),
    using_fec_book_id_(DEFAULT_FEC2_BOOK_ID),
    next_pack_sn_(0),
    encode_(fec_code_book, DEFAULT_FEC2_BOOK_ID, pack_mem_pool, write_log_cb),
    decode_(fec_code_book, DEFAULT_FEC2_BOOK_ID, pack_mem_pool, write_log_cb) {
    memset(byte_rsv_, 0x00, sizeof(byte_rsv_));
}

GtpFec2::~GtpFec2() {
}

u32 GtpFec2::Init( SessionPublicData *pb_dt) {
    pb_dt_ = pb_dt;
    return GTP_OK;
}

void GtpFec2::PopAllPack(const u64 &ts_us) {
    encode_.Init();
    decode_.Init();

    return;
}

u32 GtpFec2::Encode(GtpPacket *pack, const u64 &ts_us) {
    u32 nret = GTP_OK;

    if (next_pack_sn_ != pack->pack_sn_) {
        {
        u32 delta_sn = 0;

        if (next_pack_sn_ <= pack->pack_sn_) {
            delta_sn = pack->pack_sn_ - next_pack_sn_;
        } else {
            delta_sn = 0xFFFFFFFF - next_pack_sn_ + pack->pack_sn_;
        }

        if (MAX_FEC2_CACHE_CAPACITY >= delta_sn) {
            SetEmptyFecEnCode(GTP_YES);
        } else {
            SetEmptyFecEnCode(GTP_NO);
        }
        }

        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelNotice,
               "The sn isn't continue(next_pack_sn=%u current_sn=%u).\r\n", next_pack_sn_, pack->pack_sn_);
    }

    next_pack_sn_ = pack->pack_sn_ + 1;

try_again_encode_pos_:
    switch ((Fec2Mode)(encode_.encode_matrix_.mode_)) {
    case Fec2Mode::kBlock: {
        nret = BlockEncode(pack, ts_us);
        break;
    }

    case Fec2Mode::kConvolution: {
        // nret = ConvolutionEncode(pack, ts_us);
        break;
    }

    case Fec2Mode::kFountain: {
        // nret = FountainEncode(pack, ts_us);
        break;
    }

    default: {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelWarning, "Unkown fec encode mode(%u), now change to "\
               "code_book_id=0\r\n", (u32)(encode_.encode_matrix_.mode_));

        encode_.ChangeFecMode(DEFAULT_FEC2_BOOK_ID);
        using_fec_book_id_ = DEFAULT_FEC2_BOOK_ID;

        goto try_again_encode_pos_;
    }
    }

    return nret;
}

u32 GtpFec2::Decode(Fec2CodePack *fec_code_pack, const u64 &ts_us) {
    if (MAX_VALID_FEC2_BOOK_ID < fec_code_pack->code_book_id_) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError,
               "Invalid fec code book id(%u pack_sn=%u pack_size=%u).\r\n",
               (u32)(fec_code_pack->code_book_id_), fec_code_pack->pack_sn_,
               (u32)(fec_code_pack->pack_size_));
        return GTP_OK;
    }

    u16 init_flag   = GTP_NO;
    u16 net_book_id = fec_code_pack->code_book_id_;
    u32 matrix_id   = 0;

    matrix_id = PackSnToMatrixId(fec_code_pack->pack_sn_, (u32)(decode_.code_book_[net_book_id].block_size_));

    // maybe a new block or old block's fec encode packet.
    if (GTP_NO != decode_.decode_matrix_[matrix_id].using_flag_) {
        if (decode_.decode_matrix_[matrix_id].start_pack_sn_ == fec_code_pack->pack_sn_) {
            // a new encode dir packet for the same encode matrix.
            goto fec2_decode_judge_book_chg_pos_;
        }

        // maybe a new bloc or old block's fec encode packet.
        if (decode_.decode_matrix_[matrix_id].start_pack_sn_ < fec_code_pack->pack_sn_) {
            // a new encode matrix for the packet's sn dosen't turn over.
            decode_.decode_matrix_[matrix_id].Clear(pack_mem_pool_);
            init_flag = GTP_YES;
            goto fec2_decode_cache_encode_pos_;
        }

        {
        u32 delta_sn = 0xFFFFFFFF - decode_.decode_matrix_[matrix_id].start_pack_sn_;
        delta_sn    += fec_code_pack->pack_sn_;

        if (MAX_FEC2_CACHE_CAPACITY >= delta_sn) {
            // a new encode matrix for the packet's sn has been turn over.
            decode_.decode_matrix_[matrix_id].Clear(pack_mem_pool_);
            init_flag = GTP_YES;
            goto fec2_decode_cache_encode_pos_;
        }
        }

        #ifdef _SELFDEBUG
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelWarning, "The fec2 encode packet is too late(start_sn=%u|%u "\
               "start_pos=%u|%u fec_type=%u fec_id=%u)\r\n", decode_.decode_matrix_[matrix_id].start_pack_sn_,
               fec_code_pack->pack_sn_, (u32)(decode_.decode_matrix_[matrix_id].start_pos_),
               CalcPosInPackCache(fec_code_pack->pack_sn_), (u32)(fec_code_pack->fec_encode_dir_),
               (u32)(fec_code_pack->fec_encode_pos_));
        #endif

        return GTP_OK;
    }

fec2_decode_cache_encode_pos_:
    if (GTP_NO == decode_.decode_matrix_[matrix_id].using_flag_) {
        decode_.decode_matrix_[matrix_id].start_pack_sn_ = fec_code_pack->pack_sn_;
        decode_.decode_matrix_[matrix_id].start_pos_     = CalcPosInPackCache(fec_code_pack->pack_sn_);
        decode_.decode_matrix_[matrix_id].using_flag_    = GTP_YES;
    }

fec2_decode_judge_book_chg_pos_:
    if (((u16)(decode_.decode_matrix_[matrix_id].code_book_id_)) != net_book_id) {
        #ifdef _SELFDEBUG
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelInfo, "The fec encode book has been happened during "\
               "encode the same block(old|new_book_id=%u|%u old|new_start_sn=%u|%u old|new_start_pos=%u|%u "\
               "encode_idr=%s).\r\n", (u32)(decode_.decode_matrix_[matrix_id].code_book_id_), (u32)net_book_id,
               decode_.decode_matrix_[matrix_id].start_pack_sn_, fec_code_pack->pack_sn_,
               (u32)(decode_.decode_matrix_[matrix_id].start_pos_), (u32)CalcPosInPackCache(fec_code_pack->pack_sn_),
               Fec2CodeDirToStr(fec_code_pack->fec_encode_dir_));
        #endif

        decode_.code_book_id_ = (u8)net_book_id;

        decode_.decode_matrix_[matrix_id].code_book_id_  = (u8)net_book_id;
        decode_.decode_matrix_[matrix_id].matrix_size_   = decode_.code_book_[net_book_id].block_size_;
        decode_.decode_matrix_[matrix_id].mode_          = decode_.code_book_[net_book_id].mode_;
        decode_.decode_matrix_[matrix_id].h_flag_        = decode_.code_book_[net_book_id].h_flag_;
        decode_.decode_matrix_[matrix_id].v_flag_        = decode_.code_book_[net_book_id].v_flag_;
        decode_.decode_matrix_[matrix_id].uh_flag_       = decode_.code_book_[net_book_id].uh_flag_;
        decode_.decode_matrix_[matrix_id].dh_flag_       = decode_.code_book_[net_book_id].dh_flag_;
        decode_.decode_matrix_[matrix_id].h_size_        = decode_.code_book_[net_book_id].h_size_;
        decode_.decode_matrix_[matrix_id].v_size_        = decode_.code_book_[net_book_id].v_size_;
        decode_.decode_matrix_[matrix_id].start_pack_sn_ = fec_code_pack->pack_sn_;
        decode_.decode_matrix_[matrix_id].start_pos_     = CalcPosInPackCache(fec_code_pack->pack_sn_);
    }

    if (GTP_YES == init_flag) {
        ClearNotBelongMatrixPack(decode_.decode_matrix_[matrix_id]);
    }

    Fec2CodePackMgr *fec_cache = NULL;

    switch ((Fec2CodeDir)(fec_code_pack->fec_encode_dir_)) {
    case Fec2CodeDir::kHorizontal: {
        if (MAX_FEC2_MATRIX_H_SIZE <= fec_code_pack->fec_encode_pos_) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Invalid horizontal fec_code_id(%u).\r\n",
                   (u32)(fec_code_pack->fec_encode_pos_));
            break;
        }

        fec_cache = &(decode_.decode_matrix_[matrix_id].h_fec_code_[fec_code_pack->fec_encode_pos_]);
        break;
    }

    case Fec2CodeDir::kVertical: {
        if (MAX_FEC2_MATRIX_V_SIZE <= fec_code_pack->fec_encode_pos_) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Invalid vertical fec_code_id(%u).\r\n",
                   (u32)(fec_code_pack->fec_encode_pos_));
            break;
        }

        fec_cache = &(decode_.decode_matrix_[matrix_id].v_fec_code_[fec_code_pack->fec_encode_pos_]);
        break;
    }

    case Fec2CodeDir::kUpHill: {
        if (MAX_FEC2_HILL_SIZE <= fec_code_pack->fec_encode_pos_) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Invalid uphill fec_code_id(%u).\r\n",
                   (u32)(fec_code_pack->fec_encode_pos_));
            break;
        }

        fec_cache = &(decode_.decode_matrix_[matrix_id].uh_fec_code_[fec_code_pack->fec_encode_pos_]);
        break;
    }

    case Fec2CodeDir::kDownHill: {
        if (MAX_FEC2_HILL_SIZE <= fec_code_pack->fec_encode_pos_) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Invalid downhill fec_code_id(%u).\r\n",
                   (u32)(fec_code_pack->fec_encode_pos_));
            break;
        }

        fec_cache = &(decode_.decode_matrix_[matrix_id].dh_fec_code_[fec_code_pack->fec_encode_pos_]);
        break;
    }

    default: {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError,
               "Unknown fec encode type(%u pack_sn=%u code_book_id=%u).\r\n",
               (u32)(fec_code_pack->fec_encode_dir_), fec_code_pack->pack_sn_,
               (u32)(fec_code_pack->code_book_id_));
    }
    }

    if (NULL != fec_cache) {
        CachedFecEncodePack(fec_cache, fec_code_pack);

        TryRecoveryPackByFecPack((encode_pos)(fec_code_pack->fec_encode_pos_),
                                 (Fec2CodeDir)(fec_code_pack->fec_encode_dir_), decode_.decode_matrix_[matrix_id]);
    }

    return GTP_OK;
}

u32 GtpFec2::CacheDataPack(GtpPacket *pack, const u64 &ts_us) {
    #if (1 == ENABLE_FRAME_COPY_OUT)
    GtpPackCacheStru *cache = decode_.fec_buf_.PushPack(pack, GTP_YES);
    #else
    GtpPackCacheStru *cache = decode_.fec_buf_.PushPack(pack, GTP_NO);
    #endif

    if (NULL == cache) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Cache packet into fec2 buffer failedly.\r\n");
        RETURN_ERR(kGtpFecMd, kCachePackFailedErr);
    }

    u32 matrix_id = CalcRecvMatrixIdByPackSn(pack->pack_sn_);
    if (MAX_RECV_FEC2_MATRIX_NUM <= matrix_id) {
        goto cache_data_pack_exit_pos_;
    }

    TryRecoveryPackByDataPack((u32)(cache->cache_id_), decode_.decode_matrix_[matrix_id],
                              Fec2TryRestoreType::kFec2RestoreBoot);

cache_data_pack_exit_pos_:
    ClearReceiveUnUsedResource(CalcPosInPackCache(pack->pack_sn_));

    return GTP_OK;
}

void GtpFec2::ChangeFecMode(const u32 &new_code_book_id) {
    if (MAX_VALID_FEC2_BOOK_ID < new_code_book_id) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelWarning,
               "Invalid code_book_id(max_id=%u now_id=%u).\r\n", MAX_VALID_FEC2_BOOK_ID, new_code_book_id);
        return;
    }

    using_fec_book_id_ = new_code_book_id;
    return;
}

void GtpFec2::SetEmptyFecEnCode(const u32 &send_fec_pack_flag) {
    u32 i;

    if (GTP_NO == send_fec_pack_flag) {
        goto init_encode_pos_;
    }

    if (GTP_YES == encode_.encode_matrix_.h_flag_) {
        for (i = 0; ((u32)(encode_.encode_matrix_.h_size_)) > i; ++i) {
            if ((NULL != encode_.encode_matrix_.h_fec_code_[i].fec_pack_)
             && (0 != encode_.encode_matrix_.h_fec_code_[i].fec_pack_->encode_bit_map_)) {
                SendFecCodePacket(encode_.encode_matrix_.h_fec_code_[i]);
            }
        }
    }

    if (GTP_YES == encode_.encode_matrix_.v_flag_) {
        for (i = 0; ((u32)(encode_.encode_matrix_.v_size_)) > i; ++i) {
            if ((NULL != encode_.encode_matrix_.v_fec_code_[i].fec_pack_)
             && (0 != encode_.encode_matrix_.v_fec_code_[i].fec_pack_->encode_bit_map_)) {
                SendFecCodePacket(encode_.encode_matrix_.v_fec_code_[i]);
            }
        }
    }

    if (GTP_YES == encode_.encode_matrix_.uh_flag_) {
        for (i = 0; MAX_FEC2_HILL_SIZE > i; ++i) {
            if ((NULL != encode_.encode_matrix_.uh_fec_code_[i].fec_pack_)
             && (0 != encode_.encode_matrix_.uh_fec_code_[i].fec_pack_->encode_bit_map_)) {
                SendFecCodePacket(encode_.encode_matrix_.uh_fec_code_[i]);
            }
        }
    }

    if (GTP_YES == encode_.encode_matrix_.dh_flag_) {
        for (i = 0; MAX_FEC2_HILL_SIZE > i; ++i) {
            if ((NULL != encode_.encode_matrix_.dh_fec_code_[i].fec_pack_)
             && (0 != encode_.encode_matrix_.dh_fec_code_[i].fec_pack_->encode_bit_map_)) {
                SendFecCodePacket(encode_.encode_matrix_.dh_fec_code_[i]);
            }
        }
    }

init_encode_pos_:
    encode_.ChangeFecMode((u32)using_fec_book_id_);

    return;
}

u32 GtpFec2::BlockEncode(GtpPacket *pack, const u64 &ts_us) {
    encode_pos h_pos = 0;
    encode_pos v_pos = 0;
    goodtp_pos pos   = CalcPackPosInMatrix(CalcPosInPackCache(pack->pack_sn_),
                                           (u32)(encode_.encode_matrix_.matrix_size_), write_log_cb_);

    if ((0 == pos) || (GTP_NO == encode_.encode_matrix_.using_flag_)) {
new_block_proc_pos_:
        h_pos = (encode_pos)(encode_.encode_matrix_.matrix_size_);

        {
        u32 delta_sn = 0;

        if (encode_.encode_matrix_.start_pack_sn_ <= pack->pack_sn_) {
            delta_sn = pack->pack_sn_ - encode_.encode_matrix_.start_pack_sn_;
        } else {
            delta_sn = 0xFFFFFFFF - encode_.encode_matrix_.start_pack_sn_ + pack->pack_sn_;
        }

        if (MAX_FEC2_CACHE_CAPACITY >= delta_sn) {
            SetEmptyFecEnCode(GTP_YES);
        } else {
            SetEmptyFecEnCode(GTP_NO);
        }
        }

        if (h_pos != ((encode_pos)(encode_.encode_matrix_.matrix_size_))) {
            // has been changed fec encode mode.
            pos = CalcPackPosInMatrix(CalcPosInPackCache(pack->pack_sn_),
                                       (u32)(encode_.encode_matrix_.matrix_size_), write_log_cb_);
        }

        encode_.encode_matrix_.using_flag_    = GTP_YES;
        encode_.encode_matrix_.start_pack_sn_ = PackSnToStartSn(pack->pack_sn_,
                                                                (u32)(encode_.encode_matrix_.matrix_size_));
        encode_.encode_matrix_.start_pos_     = CalcPosInPackCache(encode_.encode_matrix_.start_pack_sn_);

        goto block_encode_start_pos_;
    }

    if ((encode_.encode_matrix_.start_pack_sn_ + encode_.encode_matrix_.matrix_size_) <= pack->pack_sn_) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelWarning, "The sn has been jumped maxtirx(start_sn=%u "\
               "current_sn=%u block_size=%u).\r\n", encode_.encode_matrix_.start_pack_sn_, pack->pack_sn_,
               (u32)(encode_.encode_matrix_.matrix_size_));
        goto new_block_proc_pos_;
    }

block_encode_start_pos_:
    h_pos = CalcHorizontalEncodePos(encode_.encode_matrix_, pos, write_log_cb_);
    if (INVALID_ENCODE_POS == h_pos) {
        // TODO(Albert.feng) :: clear encode env?
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Call CalcHorizontalEncodePos() failed(%u).\r\n", (u32)h_pos);
        return GTP_OK;
    }

    v_pos = CalcVerticalEncodePos(encode_.encode_matrix_, pos, write_log_cb_);
    if (INVALID_ENCODE_POS == v_pos) {
        // TODO(Albert.feng) :: clear encode env?
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Call CalcVerticalEncodePos() failed(%u).\r\n", (u32)v_pos);
        return GTP_OK;
    }

    if (GTP_YES == encode_.encode_matrix_.h_flag_) {
        HorizontalEncode(h_pos, v_pos, (u8*)pack, (u32)(pack->pack_size_));
    }

    if (GTP_YES == encode_.encode_matrix_.v_flag_) {
        VerticalEncode(h_pos, v_pos, (u8*)pack, (u32)(pack->pack_size_));
    }

    if (GTP_YES == encode_.encode_matrix_.uh_flag_) {
        UphillEncode(h_pos, v_pos, (u8*)pack, (u32)(pack->pack_size_));
    }

    if (GTP_YES == encode_.encode_matrix_.dh_flag_) {
        DownhillEncode(h_pos, v_pos, (u8*)pack, (u32)(pack->pack_size_));
    }

    return GTP_OK;
}

void GtpFec2::HorizontalEncode(const encode_pos &h_pos, const encode_pos &v_pos, u8 *data, const u32 &data_size) {
    u32 com_var  = 0;
    u32 mem_spec = 0;
    u8 *new_mem  = 0;
    GtpAddr* tran_addr = NULL;

    if (NULL == encode_.encode_matrix_.h_fec_code_[h_pos].fec_pack_) {
h_first_fec_encode_pos_:
        mem_spec = GtpPackSizeToMemSpec(data_size);
        new_mem  = pack_mem_pool_.MallocTranBuf(NULL, 0, &com_var, (void**)(&tran_addr), (BufSizeType)mem_spec);
        if (NULL == new_mem) {
            const string err = pack_mem_pool_.Error();
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Call MallocTranBuf() failed(%s)\r\n", err.c_str());

            const string &stat_info = pack_mem_pool_.TranMemPoolStatInfo();
            GtpLog(write_log_cb_, kGtpArqMd, kGtpLogLevelWarning, "%s\r\n", stat_info.c_str());

            return;
        }

        encode_.encode_matrix_.h_fec_code_[h_pos].fec_pack_  = (Fec2CodePack*)(new_mem - sizeof(Fec2CodePack));
        encode_.encode_matrix_.h_fec_code_[h_pos].tran_addr_ = tran_addr;

        memcpy(encode_.encode_matrix_.h_fec_code_[h_pos].fec_pack_->fec_code_, data, data_size);

        encode_.encode_matrix_.h_fec_code_[h_pos].fec_pack_->code_len_       = (u16)data_size;
        encode_.encode_matrix_.h_fec_code_[h_pos].fec_pack_->code_book_id_   = encode_.encode_matrix_.code_book_id_;
        encode_.encode_matrix_.h_fec_code_[h_pos].fec_pack_->encode_bit_map_ = (0x01 << v_pos);
        encode_.encode_matrix_.h_fec_code_[h_pos].fec_pack_->pack_sn_        = encode_.encode_matrix_.start_pack_sn_;
        encode_.encode_matrix_.h_fec_code_[h_pos].fec_pack_->fec_encode_dir_ = (u8)(Fec2CodeDir::kHorizontal);
        encode_.encode_matrix_.h_fec_code_[h_pos].fec_pack_->fec_encode_pos_ = (u8)h_pos;

        encode_.encode_matrix_.h_fec_code_[h_pos].alloc_cap_ = com_var;

        goto h_fec_encode_exit_pos_;
    }

    if (0 == v_pos) {
        Fec2CodePackMgr &hmgr = encode_.encode_matrix_.h_fec_code_[h_pos];
        if (data_size <= hmgr.alloc_cap_) {
            memcpy(hmgr.fec_pack_->fec_code_, data, data_size);
            hmgr.fec_pack_->code_len_       = (u16)data_size;
            hmgr.fec_pack_->code_book_id_   = encode_.encode_matrix_.code_book_id_;
            hmgr.fec_pack_->encode_bit_map_ = (u8)(0x01 << v_pos);
            hmgr.fec_pack_->pack_sn_        = encode_.encode_matrix_.start_pack_sn_;
            hmgr.fec_pack_->fec_encode_dir_ = (u8)(Fec2CodeDir::kHorizontal);
            hmgr.fec_pack_->fec_encode_pos_ = (u8)h_pos;
            goto h_fec_encode_exit_pos_;
        }
        pack_mem_pool_.FreeTranBuf((u8*)(hmgr.fec_pack_));
        hmgr.fec_pack_  = NULL;
        hmgr.tran_addr_ = NULL;
        goto h_first_fec_encode_pos_;
    }

    if (data_size > encode_.encode_matrix_.h_fec_code_[h_pos].alloc_cap_) {
        com_var = ReAllocateEncodeMem(data_size, encode_.encode_matrix_.h_fec_code_[h_pos]);
        if (GTP_OK != com_var) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError,
                   "Call ReAllocateEncodeMem() failed(0x%08x) when horizontal encode.\r\n", com_var);
            return;
        }
    }

    encode_.encode_matrix_.h_fec_code_[h_pos].fec_pack_->encode_bit_map_ |= ((u8)(0x01 << v_pos));

    XorEncode(data, data_size, &(encode_.encode_matrix_.h_fec_code_[h_pos].fec_pack_->fec_code_[0]),
              (u32)(encode_.encode_matrix_.h_fec_code_[h_pos].fec_pack_->code_len_), &com_var);

    encode_.encode_matrix_.h_fec_code_[h_pos].fec_pack_->code_len_ = (u16)com_var;

h_fec_encode_exit_pos_:
    // current sn is the last of horizontal.
    if (((encode_pos)(encode_.encode_matrix_.h_size_)) <= (v_pos + 1)) {
        SendFecCodePacket(encode_.encode_matrix_.h_fec_code_[h_pos]);
    }

    return;
}

void GtpFec2::VerticalEncode(const encode_pos &h_pos, const encode_pos &v_pos, u8 *data, const u32 &data_size) {
    u32 com_var  = 0;
    u32 mem_spec = 0;
    u8 *new_mem  = 0;
    GtpAddr* tran_addr = NULL;

    if (NULL == encode_.encode_matrix_.v_fec_code_[v_pos].fec_pack_) {
v_first_fec_encode_pos_:
        mem_spec = GtpPackSizeToMemSpec(data_size);
        new_mem  = pack_mem_pool_.MallocTranBuf(NULL, 0, &com_var, (void**)(&tran_addr), (BufSizeType)mem_spec);
        if (NULL == new_mem) {
            const string err = pack_mem_pool_.Error();
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Call MallocTranBuf() failed(%s)\r\n", err.c_str());

            const string &stat_info = pack_mem_pool_.TranMemPoolStatInfo();
            GtpLog(write_log_cb_, kGtpArqMd, kGtpLogLevelWarning, "%s\r\n", stat_info.c_str());

            return;
        }

        encode_.encode_matrix_.v_fec_code_[v_pos].fec_pack_  = (Fec2CodePack*)(new_mem - sizeof(Fec2CodePack));
        encode_.encode_matrix_.v_fec_code_[v_pos].tran_addr_ = tran_addr;

        memcpy(encode_.encode_matrix_.v_fec_code_[v_pos].fec_pack_->fec_code_, data, data_size);

        encode_.encode_matrix_.v_fec_code_[v_pos].fec_pack_->code_len_       = (u16)data_size;
        encode_.encode_matrix_.v_fec_code_[v_pos].fec_pack_->code_book_id_   = encode_.encode_matrix_.code_book_id_;
        encode_.encode_matrix_.v_fec_code_[v_pos].fec_pack_->encode_bit_map_ = (0x01 << h_pos);
        encode_.encode_matrix_.v_fec_code_[v_pos].fec_pack_->pack_sn_        = encode_.encode_matrix_.start_pack_sn_;
        encode_.encode_matrix_.v_fec_code_[v_pos].fec_pack_->fec_encode_dir_ = (u8)(Fec2CodeDir::kVertical);
        encode_.encode_matrix_.v_fec_code_[v_pos].fec_pack_->fec_encode_pos_ = (u8)v_pos;

        encode_.encode_matrix_.v_fec_code_[v_pos].alloc_cap_ = com_var;

        goto v_fec_encode_exit_pos_;
    }

    if (0 == h_pos) {
        Fec2CodePackMgr &vmgr = encode_.encode_matrix_.v_fec_code_[v_pos];
        if (data_size <= vmgr.alloc_cap_) {
            memcpy(vmgr.fec_pack_->fec_code_, data, data_size);
            vmgr.fec_pack_->code_len_       = (u16)data_size;
            vmgr.fec_pack_->code_book_id_   = encode_.encode_matrix_.code_book_id_;
            vmgr.fec_pack_->encode_bit_map_ = (u8)(0x01 << h_pos);
            vmgr.fec_pack_->pack_sn_        = encode_.encode_matrix_.start_pack_sn_;
            vmgr.fec_pack_->fec_encode_dir_ = (u8)(Fec2CodeDir::kVertical);
            vmgr.fec_pack_->fec_encode_pos_ = (u8)v_pos;
            goto v_fec_encode_exit_pos_;
        }
        pack_mem_pool_.FreeTranBuf((u8*)(vmgr.fec_pack_));
        vmgr.fec_pack_  = NULL;
        vmgr.tran_addr_ = NULL;
        goto v_first_fec_encode_pos_;
    }

    if (data_size > encode_.encode_matrix_.v_fec_code_[v_pos].alloc_cap_) {
        com_var = ReAllocateEncodeMem(data_size, encode_.encode_matrix_.v_fec_code_[v_pos]);
        if (GTP_OK != com_var) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError,
                   "Call ReAllocateEncodeMem failed(0x%08x) when vertical encode.\r\n", com_var);
            return;
        }
    }

    encode_.encode_matrix_.v_fec_code_[v_pos].fec_pack_->encode_bit_map_ |= ((u8)(0x01 << h_pos));

    XorEncode(data, data_size, &(encode_.encode_matrix_.v_fec_code_[v_pos].fec_pack_->fec_code_[0]),
              (u32)(encode_.encode_matrix_.v_fec_code_[v_pos].fec_pack_->code_len_), &com_var);

    encode_.encode_matrix_.v_fec_code_[v_pos].fec_pack_->code_len_ = (u16)com_var;

v_fec_encode_exit_pos_:
    // current sn is the last of vertical.
    if (((encode_pos)(encode_.encode_matrix_.v_size_)) <= (h_pos + 1)) {
        SendFecCodePacket(encode_.encode_matrix_.v_fec_code_[v_pos]);
    }

    return;
}

void GtpFec2::UphillEncode(const encode_pos &h_pos, const encode_pos &v_pos, u8 *data, const u32 &data_size) {
    // UH diagonal: h_pos + v_pos = sum = uh_idx + 1.
    // Skip single-element diagonals (sum == 0 or sum == 2*h_size-2).
    int uh_sum   = (int)h_pos + (int)v_pos;
    int uh_h_sz  = (int)(encode_.encode_matrix_.h_size_);

    if (uh_sum < 1 || uh_sum > (2 * uh_h_sz - 3)) {
        return;
    }

    encode_pos uh_idx     = (encode_pos)(uh_sum - 1);
    encode_pos h_pos_start = (encode_pos)((uh_sum > uh_h_sz - 1) ? (uh_sum - uh_h_sz + 1) : 0);

    u32 com_var  = 0;
    u32 mem_spec = 0;
    u8 *new_mem  = NULL;
    GtpAddr *tran_addr = NULL;

    Fec2CodePackMgr &mgr = encode_.encode_matrix_.uh_fec_code_[uh_idx];

    if (NULL == mgr.fec_pack_) {
uh_first_fec_encode_pos_:
        mem_spec = GtpPackSizeToMemSpec(data_size);
        new_mem  = pack_mem_pool_.MallocTranBuf(NULL, 0, &com_var, (void**)(&tran_addr), (BufSizeType)mem_spec);
        if (NULL == new_mem) {
            const string err = pack_mem_pool_.Error();
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Call MallocTranBuf() failed(%s)\r\n", err.c_str());
            return;
        }

        mgr.fec_pack_  = (Fec2CodePack*)(new_mem - sizeof(Fec2CodePack));
        mgr.tran_addr_ = tran_addr;

        memcpy(mgr.fec_pack_->fec_code_, data, data_size);

        mgr.fec_pack_->code_len_       = (u16)data_size;
        mgr.fec_pack_->code_book_id_   = encode_.encode_matrix_.code_book_id_;
        mgr.fec_pack_->encode_bit_map_ = (u8)(0x01 << h_pos);
        mgr.fec_pack_->pack_sn_        = encode_.encode_matrix_.start_pack_sn_;
        mgr.fec_pack_->fec_encode_dir_ = (u8)(Fec2CodeDir::kUpHill);
        mgr.fec_pack_->fec_encode_pos_ = (u8)uh_idx;
        mgr.alloc_cap_ = com_var;

        goto uh_fec_encode_exit_pos_;
    }

    if (h_pos == h_pos_start) {
        if (data_size <= mgr.alloc_cap_) {
            memcpy(mgr.fec_pack_->fec_code_, data, data_size);
            mgr.fec_pack_->code_len_       = (u16)data_size;
            mgr.fec_pack_->code_book_id_   = encode_.encode_matrix_.code_book_id_;
            mgr.fec_pack_->encode_bit_map_ = (u8)(0x01 << h_pos);
            mgr.fec_pack_->pack_sn_        = encode_.encode_matrix_.start_pack_sn_;
            mgr.fec_pack_->fec_encode_dir_ = (u8)(Fec2CodeDir::kUpHill);
            mgr.fec_pack_->fec_encode_pos_ = (u8)uh_idx;
            goto uh_fec_encode_exit_pos_;
        }
        pack_mem_pool_.FreeTranBuf((u8*)(mgr.fec_pack_));
        mgr.fec_pack_  = NULL;
        mgr.tran_addr_ = NULL;
        goto uh_first_fec_encode_pos_;
    }

    if (data_size > mgr.alloc_cap_) {
        com_var = ReAllocateEncodeMem(data_size, mgr);
        if (GTP_OK != com_var) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError,
                   "Call ReAllocateEncodeMem failed(0x%08x) when uphill encode.\r\n", com_var);
            return;
        }
    }

    mgr.fec_pack_->encode_bit_map_ |= (u8)(0x01 << h_pos);

    XorEncode(data, data_size, &(mgr.fec_pack_->fec_code_[0]),
              (u32)(mgr.fec_pack_->code_len_), &com_var);

    mgr.fec_pack_->code_len_ = (u16)com_var;

uh_fec_encode_exit_pos_:
    // End of diagonal: next step (+1 row, -1 col) would leave the matrix.
    if (((encode_pos)(encode_.encode_matrix_.h_size_ - 1) == h_pos) || (0 == v_pos)) {
        SendFecCodePacket(mgr);
    }
}

void GtpFec2::DownhillEncode(const encode_pos &h_pos, const encode_pos &v_pos, u8 *data, const u32 &data_size) {
    // DH diagonal: h_pos - v_pos = diff = dh_idx - (h_size-2).
    // Skip single-element diagonals (|diff| == h_size-1).
    int dh_diff  = (int)h_pos - (int)v_pos;
    int dh_h_sz  = (int)(encode_.encode_matrix_.h_size_);

    if (dh_diff <= -(dh_h_sz - 1) || dh_diff >= (dh_h_sz - 1)) {
        return;
    }

    encode_pos dh_idx     = (encode_pos)(dh_diff + dh_h_sz - 2);
    encode_pos h_pos_start = (encode_pos)((dh_diff > 0) ? dh_diff : 0);

    u32 com_var  = 0;
    u32 mem_spec = 0;
    u8 *new_mem  = NULL;
    GtpAddr *tran_addr = NULL;

    Fec2CodePackMgr &mgr = encode_.encode_matrix_.dh_fec_code_[dh_idx];

    if (NULL == mgr.fec_pack_) {
dh_first_fec_encode_pos_:
        mem_spec = GtpPackSizeToMemSpec(data_size);
        new_mem  = pack_mem_pool_.MallocTranBuf(NULL, 0, &com_var, (void**)(&tran_addr), (BufSizeType)mem_spec);
        if (NULL == new_mem) {
            const string err = pack_mem_pool_.Error();
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Call MallocTranBuf() failed(%s)\r\n", err.c_str());
            return;
        }

        mgr.fec_pack_  = (Fec2CodePack*)(new_mem - sizeof(Fec2CodePack));
        mgr.tran_addr_ = tran_addr;

        memcpy(mgr.fec_pack_->fec_code_, data, data_size);

        mgr.fec_pack_->code_len_       = (u16)data_size;
        mgr.fec_pack_->code_book_id_   = encode_.encode_matrix_.code_book_id_;
        mgr.fec_pack_->encode_bit_map_ = (u8)(0x01 << h_pos);
        mgr.fec_pack_->pack_sn_        = encode_.encode_matrix_.start_pack_sn_;
        mgr.fec_pack_->fec_encode_dir_ = (u8)(Fec2CodeDir::kDownHill);
        mgr.fec_pack_->fec_encode_pos_ = (u8)dh_idx;
        mgr.alloc_cap_ = com_var;

        goto dh_fec_encode_exit_pos_;
    }

    if (h_pos == h_pos_start) {
        if (data_size <= mgr.alloc_cap_) {
            memcpy(mgr.fec_pack_->fec_code_, data, data_size);
            mgr.fec_pack_->code_len_       = (u16)data_size;
            mgr.fec_pack_->code_book_id_   = encode_.encode_matrix_.code_book_id_;
            mgr.fec_pack_->encode_bit_map_ = (u8)(0x01 << h_pos);
            mgr.fec_pack_->pack_sn_        = encode_.encode_matrix_.start_pack_sn_;
            mgr.fec_pack_->fec_encode_dir_ = (u8)(Fec2CodeDir::kDownHill);
            mgr.fec_pack_->fec_encode_pos_ = (u8)dh_idx;
            goto dh_fec_encode_exit_pos_;
        }
        pack_mem_pool_.FreeTranBuf((u8*)(mgr.fec_pack_));
        mgr.fec_pack_  = NULL;
        mgr.tran_addr_ = NULL;
        goto dh_first_fec_encode_pos_;
    }

    if (data_size > mgr.alloc_cap_) {
        com_var = ReAllocateEncodeMem(data_size, mgr);
        if (GTP_OK != com_var) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError,
                   "Call ReAllocateEncodeMem failed(0x%08x) when downhill encode.\r\n", com_var);
            return;
        }
    }

    mgr.fec_pack_->encode_bit_map_ |= (u8)(0x01 << h_pos);

    XorEncode(data, data_size, &(mgr.fec_pack_->fec_code_[0]),
              (u32)(mgr.fec_pack_->code_len_), &com_var);

    mgr.fec_pack_->code_len_ = (u16)com_var;

dh_fec_encode_exit_pos_:
    // End of diagonal: next step (+1 row, +1 col) would leave the matrix.
    if (((encode_pos)(encode_.encode_matrix_.h_size_ - 1) == h_pos) ||
        ((encode_pos)(encode_.encode_matrix_.v_size_ - 1) == v_pos)) {
        SendFecCodePacket(mgr);
    }
}

void GtpFec2::SendFecCodePacket(Fec2CodePackMgr &fec_code_mgr) {
    u32 pack_size = (u32)sizeof(Fec2CodePack) + (u32)(fec_code_mgr.fec_pack_->code_len_);

    fec_code_mgr.fec_pack_->stream_key_     = pb_dt_->tran_addr_.stream_key_;

    fec_code_mgr.fec_pack_->goodtp_ver_     = CalcRightGtpVer(GTP_VERSION, pb_dt_->peer_version_);
    fec_code_mgr.fec_pack_->header_offset_  = (u8)sizeof(GtpPacket);
    fec_code_mgr.fec_pack_->cache_us_flag_  = GTP_NO;
    fec_code_mgr.fec_pack_->has_loss_flag_  = GTP_NO;
    fec_code_mgr.fec_pack_->pack_type_      = (u8)(GtpPackType::kGtpFecPackType);
    fec_code_mgr.fec_pack_->pack_size_      = (u16)pack_size;
    fec_code_mgr.fec_pack_->has_check_flag_ = GTP_NO;
    fec_code_mgr.fec_pack_->has_ts_flag_    = GTP_NO;
    fec_code_mgr.fec_pack_->has_rtt_flag_   = GTP_NO;
    fec_code_mgr.fec_pack_->init_flag_      = GTP_NO;
    fec_code_mgr.fec_pack_->repeat_counter_ = 0;
    fec_code_mgr.fec_pack_->is_qos_flg_     = GTP_NO;
    fec_code_mgr.fec_pack_->share_zone_     = 0;

    memcpy(fec_code_mgr.tran_addr_, &(pb_dt_->tran_addr_), sizeof(GtpAddr));
    
    u32 nret = send_pack_cb_(GtpHdlIntToPointer(pb_dt_->gtp_hdl_), fec_code_mgr.fec_pack_,
                             (u32)(fec_code_mgr.fec_pack_->pack_size_), fec_code_mgr.tran_addr_);
    if (GTP_OK != nret) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Call send_pack_cb() failed(0x%08x) fec_type=%s "\
               "start_sn=%u code_book_id=%u fec_code_id=%u encode_bitmap=0x%02x.\r\n", nret,
               Fec2CodeDirToStr(fec_code_mgr.fec_pack_->fec_encode_dir_), fec_code_mgr.fec_pack_->pack_sn_,
               (u32)(fec_code_mgr.fec_pack_->code_book_id_), (u32)(fec_code_mgr.fec_pack_->fec_encode_pos_),
               (u32)(fec_code_mgr.fec_pack_->encode_bit_map_));
    }

    pack_mem_pool_.FreeTranBuf((u8*)(fec_code_mgr.fec_pack_));

    fec_code_mgr.fec_pack_  = NULL;
    fec_code_mgr.tran_addr_ = NULL;

    return;
}

void GtpFec2::CachedFecEncodePack(Fec2CodePackMgr *fec_code_mgr, Fec2CodePack *fec_code_pack) {
    if (NULL != fec_code_mgr->fec_pack_) {
        pack_mem_pool_.FreeTranBuf((u8*)(fec_code_mgr->fec_pack_));
    }

    // Zero-copy: bump the refcount on the incoming TranBuf so PackPostHandler's
    // FreeTranBuf only decrements to 1, not 0. The actual free happens when the
    // decode matrix is cleared (via Fec2EnDeCodeMatrix::Clear → FreeTranBuf).
    // GtpAddr is at packet_ptr - BUF_OFFSET_SIZE (= tran_buf_mem[0]), NOT at
    // packet_ptr - TP_ADDR_RSV_SIZE (= tran_buf_mem[HEADER_RSV_SIZE]) — that would
    // land 128 bytes inside the GtpAddr region and corrupt it on the memcpy in
    // RestoreDataBy{H,V}Dir.
    pack_mem_pool_.TranBufUseRefAddOne((u8*)fec_code_pack);

    fec_code_mgr->fec_pack_  = fec_code_pack;
    fec_code_mgr->tran_addr_ = (GtpAddr*)((u8*)fec_code_pack - BUF_OFFSET_SIZE);

    return;
}

u32 GtpFec2::RestoreDataByHDir(const goodtp_pos &h_start_pos, const goodtp_pos &res_pos, const u8 &h_size,
                               Fec2CodePackMgr &fec_code_mgr, const Fec2EnDeCodeMatrix &decode_matrix) {
    goodtp_pos move_pos = h_start_pos;
    u8  bitmap   = 0x01;
    u8  loop     = 0;

    u32 coded_sz = 0;

    while (h_size > loop) {
        if (0x00 == (fec_code_mgr.fec_pack_->encode_bit_map_ & bitmap)) {
            goto h_restore_next_pos_;
        }

        if (NULL == decode_.fec_buf_.pack_cache_[move_pos]) {
            goto h_restore_next_pos_;
        }

        // the fec encode packet's length is the most large.
        XorEncode(&(decode_.fec_buf_.pack_cache_[move_pos]->payload_[0]),
                  (u32)(decode_.fec_buf_.pack_cache_[move_pos]->payload_len_),
                  &(fec_code_mgr.fec_pack_->fec_code_[0]), (u32)(fec_code_mgr.fec_pack_->code_len_), &coded_sz);

        if (coded_sz != ((u32)(fec_code_mgr.fec_pack_->code_len_))) {
            GtpPacket *tmp_pack = (GtpPacket*)(&(decode_.fec_buf_.pack_cache_[move_pos]->payload_[0]));

            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelNotice, "fec code's size changed when horizontal decoding"\
                   "(%u != %u start_sn_code=%u start_sn_matrix=%u block_start_pos=%u h_start_pos=%u h_size=%u "\
                   "res_pos=%u cur_pack_sn=%u book_id=%u block_size=%u).\r\n",
                   (u32)(fec_code_mgr.fec_pack_->code_len_), coded_sz, fec_code_mgr.fec_pack_->pack_sn_,
                   decode_matrix.start_pack_sn_, (u32)(decode_matrix.start_pos_), (u32)h_start_pos, (u32)h_size,
                   (u32)res_pos, tmp_pack->pack_sn_, (u32)(decode_matrix.code_book_id_),
                   (u32)(decode_matrix.matrix_size_));
        }

        fec_code_mgr.fec_pack_->code_len_ = (u16)coded_sz;

h_restore_next_pos_:
        loop     += 1;
        move_pos += 1;
        move_pos &= ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
        bitmap  <<= 1;
    }

    GtpPacket *pack = (GtpPacket*)(&(fec_code_mgr.fec_pack_->fec_code_[0]));

    if (res_pos != CalcPosInPackCache(pack->pack_sn_)) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "fec restore abnormal by horizontal(pack_sn=%u "\
               "res_pos=%u calc_res_pos=%u).\r\n", pack->pack_sn_, (u32)res_pos,
               (u32)CalcPosInPackCache(pack->pack_sn_));

        pack_mem_pool_.FreeTranBuf((u8*)(fec_code_mgr.fec_pack_));

        fec_code_mgr.fec_pack_  = NULL;
        fec_code_mgr.tran_addr_ = NULL;

        RETURN_ERR(kGtpFecMd, kFecRestoreFailedErr);
    }

    memcpy(fec_code_mgr.tran_addr_, &(pb_dt_->tran_addr_), sizeof(GtpAddr));

    coded_sz = restorePackRecv_cb_(pb_dt_->session_, pb_dt_->gtp_hdl_, pack, fec_code_mgr.tran_addr_,
                  &(decode_.fec_res_sucess_sum_), &(decode_.fec_res_failed_sum_), &(decode_.fec_res_repeat_sum_));
    if (GTP_OK != coded_sz) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "call restorePackRecv_cb_() failed(0x%08x).\r\n",
               coded_sz);
    }

    decode_.fec_buf_.PushPack(pack, GTP_NO);

    pack_mem_pool_.FreeTranBuf((u8*)pack);

    fec_code_mgr.fec_pack_  = NULL;
    fec_code_mgr.tran_addr_ = NULL;

    return GTP_OK;
}

u32 GtpFec2::RestoreDataByVDir(const goodtp_pos &v_start_pos, const goodtp_pos &res_pos, const u8 &h_size,
              const u8 &v_size, Fec2CodePackMgr &fec_code_mgr, const Fec2EnDeCodeMatrix &decode_matrix) {
    goodtp_pos move_pos = v_start_pos;
    u8  bitmap   = 0x01;
    u8  loop     = 0;

    u32 coded_sz = 0;

    while (v_size > loop) {
        if (0x00 == (fec_code_mgr.fec_pack_->encode_bit_map_ & bitmap)) {
            goto v_restore_next_pos_;
        }

        if (NULL == decode_.fec_buf_.pack_cache_[move_pos]) {
            goto v_restore_next_pos_;
        }

        // the fec encode packet's length is the most large.
        XorEncode(&(decode_.fec_buf_.pack_cache_[move_pos]->payload_[0]),
                  (u32)(decode_.fec_buf_.pack_cache_[move_pos]->payload_len_),
                  &(fec_code_mgr.fec_pack_->fec_code_[0]), (u32)(fec_code_mgr.fec_pack_->code_len_), &coded_sz);

        if (coded_sz != ((u32)(fec_code_mgr.fec_pack_->code_len_))) {
            GtpPacket *tmp_pack = (GtpPacket*)(&(decode_.fec_buf_.pack_cache_[move_pos]->payload_[0]));

            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelNotice, "fec code's size changed when horizontal decoding"\
                   "(%u != %u start_sn_code=%u start_sn_matrix=%u block_start_pos=%u v_start_pos=%u h_size=%u"\
                   "v_size=%u res_pos=%u cur_pack_sn=%u book_id=%u block_size=%u).\r\n",
                   (u32)(fec_code_mgr.fec_pack_->code_len_), coded_sz, fec_code_mgr.fec_pack_->pack_sn_,
                   decode_matrix.start_pack_sn_, (u32)(decode_matrix.start_pos_), (u32)v_start_pos, (u32)h_size,
                   (u32)v_size, (u32)res_pos, tmp_pack->pack_sn_, (u32)(decode_matrix.code_book_id_),
                   (u32)(decode_matrix.matrix_size_));
        }

        fec_code_mgr.fec_pack_->code_len_ = (u16)coded_sz;

v_restore_next_pos_:
        loop     += 1;
        move_pos += h_size;
        move_pos &= FEC2_CACHE_CAPACITY_MASK;
        bitmap  <<= 1;
    }

    GtpPacket *pack = (GtpPacket*)(&(fec_code_mgr.fec_pack_->fec_code_[0]));

    if (res_pos != CalcPosInPackCache(pack->pack_sn_)) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "fec restore abnormal by vertical(pack_sn=%u "\
               "res_pos=%u calc_res_pos=%u).\r\n", pack->pack_sn_, (u32)res_pos,
               (u32)CalcPosInPackCache(pack->pack_sn_));

        pack_mem_pool_.FreeTranBuf((u8*)(fec_code_mgr.fec_pack_));

        fec_code_mgr.fec_pack_  = NULL;
        fec_code_mgr.tran_addr_ = NULL;

        RETURN_ERR(kGtpFecMd, kFecRestoreFailedErr);
    }

    memcpy(fec_code_mgr.tran_addr_, &(pb_dt_->tran_addr_), sizeof(GtpAddr));

    coded_sz = restorePackRecv_cb_(pb_dt_->session_, pb_dt_->gtp_hdl_, pack, fec_code_mgr.tran_addr_,
                      &(decode_.fec_res_sucess_sum_), &(decode_.fec_res_failed_sum_), &(decode_.fec_res_repeat_sum_));
    if (GTP_OK != coded_sz) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "call restorePackRecv_cb_() failed(0x%08x).\r\n", coded_sz);
    }

    decode_.fec_buf_.PushPack(pack, GTP_NO);

    pack_mem_pool_.FreeTranBuf((u8*)pack);

    fec_code_mgr.fec_pack_  = NULL;
    fec_code_mgr.tran_addr_ = NULL;

    return GTP_OK;
}

u32 GtpFec2::RestoreDataByUHDir(const goodtp_pos &uh_start_pos, const goodtp_pos &res_pos, const u8 &h_size,
                                 const encode_pos &uh_encode_pos, Fec2CodePackMgr &fec_code_mgr,
                                 const Fec2EnDeCodeMatrix &decode_matrix) {
    u8  sum        = uh_encode_pos + 1;
    u8  h_pos_start = (sum > h_size - 1) ? (u8)(sum - h_size + 1) : (u8)0;
    u8  h_pos_end   = (sum < h_size) ? sum : (u8)(h_size - 1);
    u8  diag_count  = h_pos_end - h_pos_start + 1;

    goodtp_pos move_pos = uh_start_pos;
    u8  loop     = 0;
    u8  bitmap   = (u8)(0x01 << h_pos_start);
    u32 coded_sz = 0;

    while (diag_count > loop) {
        if (0x00 == (fec_code_mgr.fec_pack_->encode_bit_map_ & bitmap)) {
            goto uh_restore_next_pos_;
        }

        if (NULL == decode_.fec_buf_.pack_cache_[move_pos]) {
            goto uh_restore_next_pos_;
        }

        XorEncode(&(decode_.fec_buf_.pack_cache_[move_pos]->payload_[0]),
                  (u32)(decode_.fec_buf_.pack_cache_[move_pos]->payload_len_),
                  &(fec_code_mgr.fec_pack_->fec_code_[0]),
                  (u32)(fec_code_mgr.fec_pack_->code_len_), &coded_sz);

        if (coded_sz != (u32)(fec_code_mgr.fec_pack_->code_len_)) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelNotice, "fec code size changed when uphill decoding"
                   "(%u != %u uh_id=%u block_size=%u).\r\n",
                   (u32)(fec_code_mgr.fec_pack_->code_len_), coded_sz,
                   (u32)uh_encode_pos, (u32)(decode_matrix.matrix_size_));
        }

        fec_code_mgr.fec_pack_->code_len_ = (u16)coded_sz;

uh_restore_next_pos_:
        loop     += 1;
        move_pos  = (move_pos + h_size - 1) & ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
        bitmap  <<= 1;
    }

    GtpPacket *pack = (GtpPacket*)(&(fec_code_mgr.fec_pack_->fec_code_[0]));

    if (res_pos != CalcPosInPackCache(pack->pack_sn_)) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError,
               "fec restore abnormal by uphill(pack_sn=%u res_pos=%u calc_res_pos=%u).\r\n",
               pack->pack_sn_, (u32)res_pos, (u32)CalcPosInPackCache(pack->pack_sn_));

        pack_mem_pool_.FreeTranBuf((u8*)(fec_code_mgr.fec_pack_));
        fec_code_mgr.fec_pack_  = NULL;
        fec_code_mgr.tran_addr_ = NULL;

        RETURN_ERR(kGtpFecMd, kFecRestoreFailedErr);
    }

    memcpy(fec_code_mgr.tran_addr_, &(pb_dt_->tran_addr_), sizeof(GtpAddr));

    coded_sz = restorePackRecv_cb_(pb_dt_->session_, pb_dt_->gtp_hdl_, pack, fec_code_mgr.tran_addr_,
                  &(decode_.fec_res_sucess_sum_), &(decode_.fec_res_failed_sum_), &(decode_.fec_res_repeat_sum_));
    if (GTP_OK != coded_sz) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError,
               "call restorePackRecv_cb_() failed(0x%08x) in uphill.\r\n", coded_sz);
    }

    decode_.fec_buf_.PushPack(pack, GTP_NO);
    pack_mem_pool_.FreeTranBuf((u8*)pack);

    fec_code_mgr.fec_pack_  = NULL;
    fec_code_mgr.tran_addr_ = NULL;

    return GTP_OK;
}

u32 GtpFec2::RestoreDataByDHDir(const goodtp_pos &dh_start_pos, const goodtp_pos &res_pos, const u8 &h_size,
                                 const encode_pos &dh_encode_pos, Fec2CodePackMgr &fec_code_mgr,
                                 const Fec2EnDeCodeMatrix &decode_matrix) {
    int dh_diff    = (int)dh_encode_pos - (int)(h_size - 2);
    u8  h_pos_start = (dh_diff > 0) ? (u8)dh_diff : (u8)0;
    u8  h_pos_end   = ((int)(h_size - 1) + dh_diff < (int)(h_size - 1))
                      ? (u8)((int)(h_size - 1) + dh_diff) : (u8)(h_size - 1);
    u8  diag_count  = h_pos_end - h_pos_start + 1;

    goodtp_pos move_pos = dh_start_pos;
    u8  loop     = 0;
    u8  bitmap   = (u8)(0x01 << h_pos_start);
    u32 coded_sz = 0;

    while (diag_count > loop) {
        if (0x00 == (fec_code_mgr.fec_pack_->encode_bit_map_ & bitmap)) {
            goto dh_restore_next_pos_;
        }

        if (NULL == decode_.fec_buf_.pack_cache_[move_pos]) {
            goto dh_restore_next_pos_;
        }

        XorEncode(&(decode_.fec_buf_.pack_cache_[move_pos]->payload_[0]),
                  (u32)(decode_.fec_buf_.pack_cache_[move_pos]->payload_len_),
                  &(fec_code_mgr.fec_pack_->fec_code_[0]),
                  (u32)(fec_code_mgr.fec_pack_->code_len_), &coded_sz);

        if (coded_sz != (u32)(fec_code_mgr.fec_pack_->code_len_)) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelNotice, "fec code size changed when downhill decoding"
                   "(%u != %u dh_id=%u block_size=%u).\r\n",
                   (u32)(fec_code_mgr.fec_pack_->code_len_), coded_sz,
                   (u32)dh_encode_pos, (u32)(decode_matrix.matrix_size_));
        }

        fec_code_mgr.fec_pack_->code_len_ = (u16)coded_sz;

dh_restore_next_pos_:
        loop     += 1;
        move_pos  = (move_pos + h_size + 1) & ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
        bitmap  <<= 1;
    }

    GtpPacket *pack = (GtpPacket*)(&(fec_code_mgr.fec_pack_->fec_code_[0]));

    if (res_pos != CalcPosInPackCache(pack->pack_sn_)) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError,
               "fec restore abnormal by downhill(pack_sn=%u res_pos=%u calc_res_pos=%u).\r\n",
               pack->pack_sn_, (u32)res_pos, (u32)CalcPosInPackCache(pack->pack_sn_));

        pack_mem_pool_.FreeTranBuf((u8*)(fec_code_mgr.fec_pack_));
        fec_code_mgr.fec_pack_  = NULL;
        fec_code_mgr.tran_addr_ = NULL;

        RETURN_ERR(kGtpFecMd, kFecRestoreFailedErr);
    }

    memcpy(fec_code_mgr.tran_addr_, &(pb_dt_->tran_addr_), sizeof(GtpAddr));

    coded_sz = restorePackRecv_cb_(pb_dt_->session_, pb_dt_->gtp_hdl_, pack, fec_code_mgr.tran_addr_,
                  &(decode_.fec_res_sucess_sum_), &(decode_.fec_res_failed_sum_), &(decode_.fec_res_repeat_sum_));
    if (GTP_OK != coded_sz) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError,
               "call restorePackRecv_cb_() failed(0x%08x) in downhill.\r\n", coded_sz);
    }

    decode_.fec_buf_.PushPack(pack, GTP_NO);
    pack_mem_pool_.FreeTranBuf((u8*)pack);

    fec_code_mgr.fec_pack_  = NULL;
    fec_code_mgr.tran_addr_ = NULL;

    return GTP_OK;
}

u32 GtpFec2::ReAllocateEncodeMem(const u32 &new_size, Fec2CodePackMgr &fec_code_mgr) {
    u32 mem_spec = 0;
    u32 mem_size = 0;
    u8 *new_mem  = NULL;
    GtpAddr *tran_addr = NULL;

    mem_spec = GtpPackSizeToMemSpec(new_size);
    new_mem  = pack_mem_pool_.MallocTranBuf(NULL, 0, &mem_size, (void**)(&tran_addr), (BufSizeType)mem_spec);
    if (NULL == new_mem) {
        const string err = pack_mem_pool_.Error();
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Call MallocTranBuf() failed(%s)\r\n", err.c_str());

        const string &stat_info = pack_mem_pool_.TranMemPoolStatInfo();
        GtpLog(write_log_cb_, kGtpArqMd, kGtpLogLevelWarning, "%s\r\n", stat_info.c_str());

        RETURN_ERR(kGtpFecMd, kMallocMemPoolItemFailedErr);
    }

    new_mem -= sizeof(Fec2CodePack);

    memcpy(new_mem, fec_code_mgr.fec_pack_, fec_code_mgr.fec_pack_->code_len_ + sizeof(Fec2CodePack));

    pack_mem_pool_.FreeTranBuf((u8*)(fec_code_mgr.fec_pack_));

    fec_code_mgr.fec_pack_  = (Fec2CodePack*)new_mem;
    fec_code_mgr.tran_addr_ = tran_addr;

    fec_code_mgr.alloc_cap_ = mem_size;

    return GTP_OK;
}

void GtpFec2::TryRecoveryPackByFecPack(const encode_pos &fec_encode_pos, const Fec2CodeDir &fec_encode_dir,
                                  Fec2EnDeCodeMatrix &decode_matrix) {
    goodtp_pos res_pos   = 0;
    goodtp_pos start_pos = CalcPosInPackCache(decode_matrix.start_pack_sn_);  // the block's start pos.
    u32 nret = GTP_OK;
    u32 loop = 0;

    if (start_pos != decode_matrix.start_pos_) {
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelWarning, "Calcing block's start pos isn't equal to "\
               " the start pos in decode matrix(%u != %u start_sn=%u).\r\n", start_pos,
               (u32)(decode_matrix.start_pos_), decode_matrix.start_pack_sn_);
    }

    switch (fec_encode_dir) {
    case Fec2CodeDir::kHorizontal: {
        if (NULL == decode_matrix.h_fec_code_[fec_encode_pos].fec_pack_) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelWarning, "horizontal fec code is null(fec_code_id=%u "\
                   "block_start_pos=%u block_start_sn=%u).\r\n", (u32)fec_encode_pos, start_pos,
                   decode_matrix.start_pack_sn_);
            break;
        }

        // the start pos of the horizontal line of the block.
        start_pos += (((goodtp_pos)fec_encode_pos) * ((goodtp_pos)(decode_matrix.h_size_)));
        start_pos &= ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);

        res_pos = CalcRestorePosByHDir(start_pos, fec_encode_pos, decode_matrix);
        if (MAX_FEC2_CACHE_CAPACITY <= res_pos) {
            break;
        }

        // TOCTOU guard: if the missing slot was filled between CalcRestorePosByHDir and here
        // (concurrent thread violation — two threads on same GoodTP handle), the XOR would
        // include the newly-arrived packet and produce the wrong recovered SN. Skip silently.
        if (NULL != decode_.fec_buf_.pack_cache_[res_pos]) {
            break;
        }

        nret = RestoreDataByHDir(start_pos, res_pos, (u8)(decode_matrix.h_size_),
                                 decode_matrix.h_fec_code_[fec_encode_pos], decode_matrix);
        if (GTP_OK != nret) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Call RestoreDataByHDir() failed(0x%08x) by "\
                   "horizontal(encode_pos=%u start_sn=%u).\r\n", nret,
                   (u32)fec_encode_pos, decode_matrix.start_pack_sn_);
        }

        if ((GTP_NO == decode_matrix.v_flag_) && (GTP_NO == decode_matrix.uh_flag_)
         && (GTP_NO == decode_matrix.dh_flag_)) {
            /* it's only horizontal fec encode, don't need any more, can't free decode matrix because there is
               the others line. */
            loop = 0;
            while (((u32)(decode_matrix.h_size_)) > loop) {
                if (NULL != decode_.fec_buf_.pack_cache_[start_pos]) {
                    decode_.fec_buf_.PopPack(start_pos);
                }

                loop      += 1;
                start_pos += 1;
                start_pos &= ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
            }
            break;
        }

        TryRecoveryPackByDataPack(res_pos, decode_matrix, Fec2TryRestoreType::kFec2RestoreHorizontal);

        break;
    }

    case Fec2CodeDir::kVertical: {
        if (NULL == decode_matrix.v_fec_code_[fec_encode_pos].fec_pack_) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelWarning, "vertical fec code is null(fec_code_id=%u "\
              "block_start_pos=%u block_start_sn=%u).\r\n", (u32)fec_encode_pos, start_pos, decode_matrix.start_pack_sn_);
            break;
        }

        // the start pos of the vertical cow of the block.
        start_pos += fec_encode_pos;
        start_pos &= FEC2_CACHE_CAPACITY_MASK;

        res_pos = CalcRestorePosByVDir(start_pos, fec_encode_pos, decode_matrix);
        if (MAX_FEC2_CACHE_CAPACITY <= res_pos) {
            break;
        }

        // TOCTOU guard: same race as horizontal — skip if slot filled between Calc and Restore.
        if (NULL != decode_.fec_buf_.pack_cache_[res_pos]) {
            break;
        }

        nret = RestoreDataByVDir(start_pos, res_pos, (u8)(decode_matrix.h_size_), (u8)(decode_matrix.v_size_),
                                 decode_matrix.v_fec_code_[fec_encode_pos], decode_matrix);
        if (GTP_OK != nret) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Call RestoreDataByVDir() failed(0x%08x) by "\
                   "vertical(encode_pos=%u start_sn=%u).\r\n", nret,
                   (u32)fec_encode_pos, decode_matrix.start_pack_sn_);
            break;
        }

        if ((GTP_NO == decode_matrix.h_flag_) && (GTP_NO == decode_matrix.uh_flag_)
         && (GTP_NO == decode_matrix.dh_flag_)) {
            /* it's only vertical fec encode, don't need any more, can't free decode matrix because there is
               the others cow. */
            loop = 0;
            while (((u32)(decode_matrix.v_size_)) > loop) {
                if (NULL != decode_.fec_buf_.pack_cache_[start_pos]) {
                    decode_.fec_buf_.PopPack(start_pos);
                }

                loop      += 1;
                start_pos += ((goodtp_pos)(decode_matrix.h_size_));
                start_pos &= ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
            }
            break;
        }

        TryRecoveryPackByDataPack(res_pos, decode_matrix, Fec2TryRestoreType::kFec2RestoreVertical);

        break;
    }

    case Fec2CodeDir::kUpHill: {
        if (NULL == decode_matrix.uh_fec_code_[fec_encode_pos].fec_pack_) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelWarning, "uphill fec code is null(fec_code_id=%u "\
                   "block_start_pos=%u block_start_sn=%u).\r\n", (u32)fec_encode_pos, start_pos,
                   decode_matrix.start_pack_sn_);
            break;
        }

        {
        u8 h_size      = (u8)(decode_matrix.h_size_);
        u8 uh_sum      = fec_encode_pos + 1;
        u8 h_pos_start = (uh_sum > h_size - 1) ? (u8)(uh_sum - h_size + 1) : (u8)0;
        u8 v_pos_start = uh_sum - h_pos_start;

        goodtp_pos uh_start_pos = (start_pos + (goodtp_pos)h_pos_start * h_size
                                   + (goodtp_pos)v_pos_start)
                                  & ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);

        res_pos = CalcRestorePosByUHDir(uh_start_pos, fec_encode_pos, decode_matrix);
        if (MAX_FEC2_CACHE_CAPACITY <= res_pos) {
            break;
        }

        nret = RestoreDataByUHDir(uh_start_pos, res_pos, h_size, fec_encode_pos,
                                   decode_matrix.uh_fec_code_[fec_encode_pos], decode_matrix);
        if (GTP_OK != nret) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError,
                   "Call RestoreDataByUHDir() failed(0x%08x).\r\n", nret);
        }

        TryRecoveryPackByDataPack(res_pos, decode_matrix, Fec2TryRestoreType::kFec2RestoreUpHill);
        }
        break;
    }

    case Fec2CodeDir::kDownHill: {
        if (NULL == decode_matrix.dh_fec_code_[fec_encode_pos].fec_pack_) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelWarning, "downhill fec code is null(fec_code_id=%u "\
                   "block_start_pos=%u block_start_sn=%u).\r\n", (u32)fec_encode_pos, start_pos,
                   decode_matrix.start_pack_sn_);
            break;
        }

        {
        u8 h_size      = (u8)(decode_matrix.h_size_);
        int dh_diff    = (int)fec_encode_pos - (int)(h_size - 2);
        u8 h_pos_start = (dh_diff > 0) ? (u8)dh_diff  : (u8)0;
        u8 v_pos_start = (dh_diff < 0) ? (u8)(-dh_diff) : (u8)0;

        goodtp_pos dh_start_pos = (start_pos + (goodtp_pos)h_pos_start * h_size
                                   + (goodtp_pos)v_pos_start)
                                  & ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);

        res_pos = CalcRestorePosByDHDir(dh_start_pos, fec_encode_pos, decode_matrix);
        if (MAX_FEC2_CACHE_CAPACITY <= res_pos) {
            break;
        }

        nret = RestoreDataByDHDir(dh_start_pos, res_pos, h_size, fec_encode_pos,
                                   decode_matrix.dh_fec_code_[fec_encode_pos], decode_matrix);
        if (GTP_OK != nret) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError,
                   "Call RestoreDataByDHDir() failed(0x%08x).\r\n", nret);
        }

        TryRecoveryPackByDataPack(res_pos, decode_matrix, Fec2TryRestoreType::kFec2RestoreDownHill);
        }
        break;
    }

    default: {
        break;
    }
    }

    return;
}

void GtpFec2::TryRecoveryPackByDataPack(const goodtp_pos &cache_pos, Fec2EnDeCodeMatrix &decode_matrix,
                                        const Fec2TryRestoreType &restored_type) {
    goodtp_pos move_pos = 0;
    goodtp_pos res_pos  = 0;
    encode_pos h_pos    = 0;
    encode_pos v_pos    = 0;
    goodtp_pos pos_in_matrix = CalcPackPosInMatrix(cache_pos, (u32)(decode_matrix.matrix_size_), write_log_cb_);
    u32 loop = 0;
    u32 nret = GTP_OK;

    h_pos = CalcHorizontalEncodePos(decode_matrix, pos_in_matrix, write_log_cb_);
    if (INVALID_ENCODE_POS == h_pos) {
        // TODO(Albert.feng) :: clear decode env?
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Call CalcHorizontalEncodePos() failed(%u).\r\n", (u32)h_pos);
        return;
    }

    v_pos = CalcVerticalEncodePos(decode_matrix, pos_in_matrix, write_log_cb_);
    if (INVALID_ENCODE_POS == v_pos) {
        // TODO(Albert.feng) :: clear decode env?
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Call CalcVerticalEncodePos() failed(%u).\r\n", (u32)v_pos);
        return;
    }

    if (Fec2TryRestoreType::kFec2RestoreHorizontal == restored_type) {
        goto try_restore_v_pack_pos_;
    }

    if (GTP_YES == decode_matrix.h_flag_) {
        if (NULL == decode_matrix.h_fec_code_[h_pos].fec_pack_) {
            goto try_restore_v_pack_pos_;
        }

        move_pos = CalcFirstHorizontalPosInCache(cache_pos, v_pos);

        res_pos = CalcRestorePosByHDir(move_pos, (u8)h_pos, decode_matrix);
        if (MAX_FEC2_CACHE_CAPACITY <= res_pos) {
            goto try_restore_v_pack_pos_;
        }

        // TOCTOU guard: concurrent thread may have filled res_pos between Calc and Restore.
        if (NULL != decode_.fec_buf_.pack_cache_[res_pos]) {
            goto try_restore_v_pack_pos_;
        }

        // unreceived only one packet, it can be restore.
        nret = RestoreDataByHDir(move_pos, res_pos, (u8)(decode_matrix.h_size_), decode_matrix.h_fec_code_[h_pos],
                                 decode_matrix);
        if (GTP_OK != nret) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Call RestoreDataByHDir() failed(0x%08x) by "\
                   "horizontal(h_pos=%u start_sn=%u).\r\n", nret,
                   (u32)h_pos, decode_matrix.start_pack_sn_);
        }

        if ((GTP_NO == decode_matrix.v_flag_) && (GTP_NO == decode_matrix.uh_flag_)
         && (GTP_NO == decode_matrix.dh_flag_)) {
            /* it's only horizontal fec encode, don't need any more, can't free decode matrix because there is
               the others line. */
            loop = 0;
            while (((u32)(decode_matrix.h_size_)) > loop) {
                if (NULL != decode_.fec_buf_.pack_cache_[move_pos]) {
                    decode_.fec_buf_.PopPack(move_pos);
                }

                loop     += 1;
                move_pos += 1;
                move_pos &= ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
            }
            return;
        }

        TryRecoveryPackByDataPack(res_pos, decode_matrix, Fec2TryRestoreType::kFec2RestoreHorizontal);
    }

try_restore_v_pack_pos_:
    if (Fec2TryRestoreType::kFec2RestoreVertical == restored_type) {
        goto try_restore_uh_pack_pos_;
    }

    if (GTP_YES == decode_matrix.v_flag_) {
        if (NULL == decode_matrix.v_fec_code_[v_pos].fec_pack_) {
            goto try_restore_uh_pack_pos_;
        }

        move_pos = CalcFirstVerticalPosInCache(cache_pos, h_pos, ((u8)(decode_matrix.h_size_)));

        res_pos = CalcRestorePosByVDir(move_pos, v_pos, decode_matrix);
        if (MAX_FEC2_CACHE_CAPACITY <= res_pos) {
            goto try_restore_uh_pack_pos_;
        }

        // TOCTOU guard: concurrent thread may have filled res_pos between Calc and Restore.
        if (NULL != decode_.fec_buf_.pack_cache_[res_pos]) {
            goto try_restore_uh_pack_pos_;
        }

        // unreceived only one packet, it can be restore.
        nret = RestoreDataByVDir(move_pos, res_pos, (u8)(decode_matrix.h_size_), (u8)(decode_matrix.v_size_),
                                 decode_matrix.v_fec_code_[v_pos], decode_matrix);
        if (GTP_OK != nret) {
            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "Call RestoreDataByHDir() failed(0x%08x) by "\
                   "horizontal.\r\n", nret);
        }

        if ((GTP_NO == decode_matrix.h_flag_) && (GTP_NO == decode_matrix.uh_flag_)
         && (GTP_NO == decode_matrix.dh_flag_)) {
            /* it's only vertical fec encode, don't need any more, can't free decode matrix because there is
               the others cow. */
            loop = 0;
            while (((u32)(decode_matrix.v_size_)) > loop) {
                if (NULL != decode_.fec_buf_.pack_cache_[move_pos]) {
                    decode_.fec_buf_.PopPack(move_pos);
                }

                loop     += 1;
                move_pos += decode_matrix.h_size_;
                move_pos &= ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
            }
            return;
        }

        TryRecoveryPackByDataPack(res_pos, decode_matrix, Fec2TryRestoreType::kFec2RestoreVertical);
    }

try_restore_uh_pack_pos_:
    if (Fec2TryRestoreType::kFec2RestoreUpHill == restored_type) {
        goto try_restore_dh_pack_pos_;
    }

    if (GTP_YES == decode_matrix.uh_flag_) {
        {
        int uh_sum_int = (int)h_pos + (int)v_pos;
        int uh_max     = 2 * (int)(decode_matrix.h_size_) - 3;

        if (uh_sum_int >= 1 && uh_sum_int <= uh_max) {
            encode_pos uh_idx = (encode_pos)(uh_sum_int - 1);

            if (NULL != decode_matrix.uh_fec_code_[uh_idx].fec_pack_) {
                u8 h_size      = (u8)(decode_matrix.h_size_);
                u8 uh_sum      = (u8)uh_sum_int;
                u8 h_pos_start = (uh_sum > h_size - 1) ? (u8)(uh_sum - h_size + 1) : (u8)0;
                u8 v_pos_start = uh_sum - h_pos_start;

                goodtp_pos uh_start = (decode_matrix.start_pos_
                                       + (goodtp_pos)h_pos_start * h_size
                                       + (goodtp_pos)v_pos_start)
                                      & ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);

                res_pos = CalcRestorePosByUHDir(uh_start, uh_idx, decode_matrix);
                if (MAX_FEC2_CACHE_CAPACITY > res_pos) {
                    nret = RestoreDataByUHDir(uh_start, res_pos, h_size, uh_idx,
                                              decode_matrix.uh_fec_code_[uh_idx], decode_matrix);
                    if (GTP_OK != nret) {
                        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError,
                               "Call RestoreDataByUHDir() failed(0x%08x) by data.\r\n", nret);
                    }
                    TryRecoveryPackByDataPack(res_pos, decode_matrix,
                                              Fec2TryRestoreType::kFec2RestoreUpHill);
                }
            }
        }
        }
    }

try_restore_dh_pack_pos_:
    if (Fec2TryRestoreType::kFec2RestoreDownHill == restored_type) {
        goto try_restore_data_exit_pos_;
    }

    if (GTP_YES == decode_matrix.dh_flag_) {
        {
        int dh_diff_int = (int)h_pos - (int)v_pos;
        int dh_h_sz     = (int)(decode_matrix.h_size_);

        if (dh_diff_int > -(dh_h_sz - 1) && dh_diff_int < (dh_h_sz - 1)) {
            encode_pos dh_idx = (encode_pos)(dh_diff_int + dh_h_sz - 2);

            if (NULL != decode_matrix.dh_fec_code_[dh_idx].fec_pack_) {
                u8 h_size      = (u8)(decode_matrix.h_size_);
                u8 h_pos_start = (dh_diff_int > 0) ? (u8)dh_diff_int  : (u8)0;
                u8 v_pos_start = (dh_diff_int < 0) ? (u8)(-dh_diff_int) : (u8)0;

                goodtp_pos dh_start = (decode_matrix.start_pos_
                                       + (goodtp_pos)h_pos_start * h_size
                                       + (goodtp_pos)v_pos_start)
                                      & ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);

                res_pos = CalcRestorePosByDHDir(dh_start, dh_idx, decode_matrix);
                if (MAX_FEC2_CACHE_CAPACITY > res_pos) {
                    nret = RestoreDataByDHDir(dh_start, res_pos, h_size, dh_idx,
                                              decode_matrix.dh_fec_code_[dh_idx], decode_matrix);
                    if (GTP_OK != nret) {
                        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError,
                               "Call RestoreDataByDHDir() failed(0x%08x) by data.\r\n", nret);
                    }
                    TryRecoveryPackByDataPack(res_pos, decode_matrix,
                                              Fec2TryRestoreType::kFec2RestoreDownHill);
                }
            }
        }
        }
    }

try_restore_data_exit_pos_:
    return;
}

goodtp_pos GtpFec2::CalcRestorePosByHDir(const goodtp_pos &start_cache_pos, const encode_pos &h_pos,
                                         Fec2EnDeCodeMatrix &decode_matrix) {
    goodtp_pos res_pos  = 0xFFFF;
    goodtp_pos move_pos = start_cache_pos;
    u8  loop      = 0;
    u8  bitmap    = 0x01;
    u8  unrcv_num = 0;
    u8  rcved_num = 0;
    u32 start_sn  = decode_matrix.start_pack_sn_;
    u32 end_sn    = decode_matrix.start_pack_sn_ + decode_matrix.matrix_size_;

    GtpPacket *pack = NULL;

    if (start_sn <= end_sn) {
        while (((u8)(decode_matrix.h_size_)) > loop) {
            if (NULL == decode_.fec_buf_.pack_cache_[move_pos]) {
h_calc_restore_null_pos_:
                if (0x00 != (decode_matrix.h_fec_code_[h_pos].fec_pack_->encode_bit_map_ & bitmap)) {
                    res_pos    = move_pos;
                    unrcv_num += 1;
                }

                goto h_calc_restore_loop_next_pos_;
            }

            pack = (GtpPacket*)(decode_.fec_buf_.pack_cache_[move_pos]->payload_);

            if ((start_sn > pack->pack_sn_) || (end_sn <= pack->pack_sn_)) {
                // current packet isn't belong to this decode matrix block.
                decode_.fec_buf_.PopPack(move_pos);
                goto h_calc_restore_null_pos_;
            }

            if (0x00 != (decode_matrix.h_fec_code_[h_pos].fec_pack_->encode_bit_map_ & bitmap)) {
                rcved_num += 1;
                goto h_calc_restore_loop_next_pos_;
            }

            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelNotice, "it's a diffrence between encode and decode for "\
                   "horizontal(code_bit_map=0x%02x start_sn=%u cur_sn=%u fec_type=h fec_id=%u block_size=%u).\r\n",
                   (u32)(decode_matrix.h_fec_code_[h_pos].fec_pack_->encode_bit_map_), start_sn, pack->pack_sn_,
                   (u32)h_pos, (u32)(decode_matrix.matrix_size_));

h_calc_restore_loop_next_pos_:
            loop     += 1;
            move_pos += 1;
            move_pos &= ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
            bitmap  <<= 1;
        }

        goto h_calc_restore_pos_check_pos_;
    }

    // the packet's sn has been turn over.
    while (((u8)(decode_matrix.h_size_)) > loop) {
        if (NULL == decode_.fec_buf_.pack_cache_[move_pos]) {
h_calc_restore_null_pos1_:
            if (0x00 != (decode_matrix.h_fec_code_[h_pos].fec_pack_->encode_bit_map_ & bitmap)) {
                res_pos    = move_pos;
                unrcv_num += 1;
            }

            goto h_calc_restore_loop_next_pos1_;
        }

        pack = (GtpPacket*)(decode_.fec_buf_.pack_cache_[move_pos]->payload_);

        if ((start_sn > pack->pack_sn_) && (end_sn <= pack->pack_sn_)) {
            decode_.fec_buf_.PopPack(move_pos);
            goto h_calc_restore_null_pos1_;
        }

        if (0x00 != (decode_matrix.h_fec_code_[h_pos].fec_pack_->encode_bit_map_ & bitmap)) {
            rcved_num += 1;
            goto h_calc_restore_loop_next_pos1_;
        }

        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "it's a diffrence between encode and decode for "\
               "horizontal(code_bit_map=0x%02x start_sn=%u cur_sn=%u fec_type=h fec_id=%u block_size=%u).\r\n",
               (u32)(decode_matrix.h_fec_code_[h_pos].fec_pack_->encode_bit_map_), start_sn, pack->pack_sn_,
               (u32)h_pos, (u32)(decode_matrix.matrix_size_));

h_calc_restore_loop_next_pos1_:
        loop     += 1;
        move_pos += 1;
        move_pos &= ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
        bitmap  <<= 1;
    }

h_calc_restore_pos_check_pos_:
    // has received all packets in the horizontal, don't need restore.
    if ((0 == unrcv_num) || (rcved_num == ((u8)(decode_matrix.h_size_)))) {
        pack_mem_pool_.FreeTranBuf((u8*)(decode_matrix.h_fec_code_[h_pos].fec_pack_));

        decode_matrix.h_fec_code_[h_pos].fec_pack_  = NULL;
        decode_matrix.h_fec_code_[h_pos].tran_addr_ = NULL;

        return 0xFFFF;
    }

    // more than one unreceived packets, it can't be restore.
    if (1 != unrcv_num) {
        #ifdef _SELFDEBUG
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelInfo, "Discarded more than one packet in horizontal dir"\
               "(unrcv_num=%u h_size=%u encode_bit_map=0x%02x start_sn=%u start_pos=%u).\r\n", (u32)unrcv_num,
               (u32)(decode_matrix.h_size_), (u32)(decode_matrix.h_fec_code_[h_pos].fec_pack_->encode_bit_map_),
               decode_matrix.start_pack_sn_, (u32)(decode_matrix.start_pos_));
        #endif

        return 0xFFFF;
    }

    return res_pos;
}

goodtp_pos GtpFec2::CalcRestorePosByVDir(const goodtp_pos &start_cache_pos, const encode_pos &v_pos,
                                         Fec2EnDeCodeMatrix &decode_matrix) {
    goodtp_pos res_pos  = 0xFFFF;
    goodtp_pos move_pos = start_cache_pos;
    u8  loop      = 0;
    u8  bitmap    = 0x01;
    u8  unrcv_num = 0;
    u8  rcved_num = 0;
    u32 start_sn  = decode_matrix.start_pack_sn_;
    u32 end_sn    = decode_matrix.start_pack_sn_ + decode_matrix.matrix_size_;

    GtpPacket *pack = NULL;

    if (start_sn <= end_sn) {
        while (((u8)(decode_matrix.v_size_)) > loop) {
            if (NULL == decode_.fec_buf_.pack_cache_[move_pos]) {
v_calc_restore_null_pos_:
                if (0x00 != (decode_matrix.v_fec_code_[v_pos].fec_pack_->encode_bit_map_ & bitmap)) {
                    res_pos    = move_pos;
                    unrcv_num += 1;
                }
                goto v_calc_restore_loop_next_pos_;
            }

            pack = (GtpPacket*)(decode_.fec_buf_.pack_cache_[move_pos]->payload_);

            if ((start_sn > pack->pack_sn_) || (end_sn <= pack->pack_sn_)) {
                decode_.fec_buf_.PopPack(move_pos);
                goto v_calc_restore_null_pos_;
            }

            if (0x00 != (decode_matrix.v_fec_code_[v_pos].fec_pack_->encode_bit_map_ & bitmap)) {
                rcved_num += 1;
                goto v_calc_restore_loop_next_pos_;
            }

            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelNotice, "it's a diffrence between encode and decode for "\
                   "vertical(code_bit_map=0x%02x start_sn=%u cur_sn=%u fec_type=v fec_id=%u block_size=%u).\r\n",
                   (u32)(decode_matrix.v_fec_code_[v_pos].fec_pack_->encode_bit_map_), start_sn, pack->pack_sn_,
                   (u32)v_pos, (u32)(decode_matrix.matrix_size_));

v_calc_restore_loop_next_pos_:
            loop     += 1;
            move_pos += decode_matrix.h_size_;
            move_pos &= ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
            bitmap  <<= 1;
        }

        goto v_calc_restore_pos_check_pos_;
    }

    // the packet's sn has been turn over.
    while (((u8)(decode_matrix.v_size_)) > loop) {
        if (NULL == decode_.fec_buf_.pack_cache_[move_pos]) {
v_calc_restore_null_pos1_:
            if (0x00 != (decode_matrix.v_fec_code_[v_pos].fec_pack_->encode_bit_map_ & bitmap)) {
                res_pos    = move_pos;
                unrcv_num += 1;
            }
            goto v_calc_restore_loop_next_pos1_;
        }

        pack = (GtpPacket*)(decode_.fec_buf_.pack_cache_[move_pos]->payload_);

        if ((start_sn > pack->pack_sn_) && (end_sn <= pack->pack_sn_)) {
            decode_.fec_buf_.PopPack(move_pos);
            goto v_calc_restore_null_pos1_;
        }

        if (0x00 != (decode_matrix.v_fec_code_[v_pos].fec_pack_->encode_bit_map_ & bitmap)) {
            rcved_num += 1;
            goto v_calc_restore_loop_next_pos1_;
        }

        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "it's a diffrence between encode and decode for "\
               "vertical(code_bit_map=0x%02x start_sn=%u cur_sn=%u fec_type=v fec_id=%u block_size=%u).\r\n",
               (u32)(decode_matrix.v_fec_code_[v_pos].fec_pack_->encode_bit_map_), start_sn, pack->pack_sn_,
               (u32)v_pos, (u32)(decode_matrix.matrix_size_));

v_calc_restore_loop_next_pos1_:
        loop     += 1;
        move_pos += decode_matrix.h_size_;
        move_pos &= ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
        bitmap  <<= 1;
    }

v_calc_restore_pos_check_pos_:
    // has received all packets in the vertical.
    if ((0 == unrcv_num) || (rcved_num == ((u8)(decode_matrix.v_size_)))) {
        pack_mem_pool_.FreeTranBuf((u8*)(decode_matrix.v_fec_code_[v_pos].fec_pack_));

        decode_matrix.v_fec_code_[v_pos].fec_pack_  = NULL;
        decode_matrix.v_fec_code_[v_pos].tran_addr_ = NULL;

        return 0xFFFF;
    }

    // large than one unreceived packets, it can't be restore.
    if (1 != unrcv_num) {
        #ifdef _SELFDEBUG
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelInfo, "Discarded more than one packet in vertical dir"\
               "(unrcv_num=%u v_size=%u encode_bit_map=0x%02x start_sn=%u start_pos=%u).\r\n", (u32)unrcv_num,
               (u32)(decode_matrix.v_size_), (u32)(decode_matrix.v_fec_code_[v_pos].fec_pack_->encode_bit_map_),
               decode_matrix.start_pack_sn_, (u32)(decode_matrix.start_pos_));
        #endif

        return 0xFFFF;
    }

    return res_pos;
}

goodtp_pos GtpFec2::CalcRestorePosByUHDir(const goodtp_pos &start_cache_pos, const encode_pos &uh_pos,
                                           Fec2EnDeCodeMatrix &decode_matrix) {
    u8  sum        = uh_pos + 1;
    u8  h_size     = (u8)(decode_matrix.h_size_);
    u8  h_pos_start = (sum > h_size - 1) ? (u8)(sum - h_size + 1) : (u8)0;
    u8  h_pos_end   = (sum < h_size) ? sum : (u8)(h_size - 1);
    u8  diag_count  = h_pos_end - h_pos_start + 1;

    goodtp_pos res_pos  = 0xFFFF;
    goodtp_pos move_pos = start_cache_pos;
    u8  loop      = 0;
    u8  bitmap    = (u8)(0x01 << h_pos_start);
    u8  unrcv_num = 0;
    u8  rcved_num = 0;
    u32 start_sn  = decode_matrix.start_pack_sn_;
    u32 end_sn    = decode_matrix.start_pack_sn_ + decode_matrix.matrix_size_;

    GtpPacket *pack = NULL;

    if (start_sn <= end_sn) {
        while (diag_count > loop) {
            if (NULL == decode_.fec_buf_.pack_cache_[move_pos]) {
uh_calc_restore_null_pos_:
                if (0x00 != (decode_matrix.uh_fec_code_[uh_pos].fec_pack_->encode_bit_map_ & bitmap)) {
                    res_pos    = move_pos;
                    unrcv_num += 1;
                }
                goto uh_calc_restore_loop_next_pos_;
            }

            pack = (GtpPacket*)(decode_.fec_buf_.pack_cache_[move_pos]->payload_);

            if ((start_sn > pack->pack_sn_) || (end_sn <= pack->pack_sn_)) {
                decode_.fec_buf_.PopPack(move_pos);
                goto uh_calc_restore_null_pos_;
            }

            if (0x00 != (decode_matrix.uh_fec_code_[uh_pos].fec_pack_->encode_bit_map_ & bitmap)) {
                rcved_num += 1;
                goto uh_calc_restore_loop_next_pos_;
            }

            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelNotice, "encode/decode mismatch for uphill "
                   "(bit_map=0x%02x start_sn=%u cur_sn=%u uh_id=%u block_size=%u).\r\n",
                   (u32)(decode_matrix.uh_fec_code_[uh_pos].fec_pack_->encode_bit_map_),
                   start_sn, pack->pack_sn_, (u32)uh_pos, (u32)(decode_matrix.matrix_size_));

uh_calc_restore_loop_next_pos_:
            loop     += 1;
            move_pos  = (move_pos + h_size - 1) & ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
            bitmap  <<= 1;
        }
        goto uh_calc_restore_check_pos_;
    }

    while (diag_count > loop) {
        if (NULL == decode_.fec_buf_.pack_cache_[move_pos]) {
uh_calc_restore_null_pos1_:
            if (0x00 != (decode_matrix.uh_fec_code_[uh_pos].fec_pack_->encode_bit_map_ & bitmap)) {
                res_pos    = move_pos;
                unrcv_num += 1;
            }
            goto uh_calc_restore_loop_next_pos1_;
        }

        pack = (GtpPacket*)(decode_.fec_buf_.pack_cache_[move_pos]->payload_);

        if ((start_sn > pack->pack_sn_) && (end_sn <= pack->pack_sn_)) {
            decode_.fec_buf_.PopPack(move_pos);
            goto uh_calc_restore_null_pos1_;
        }

        if (0x00 != (decode_matrix.uh_fec_code_[uh_pos].fec_pack_->encode_bit_map_ & bitmap)) {
            rcved_num += 1;
            goto uh_calc_restore_loop_next_pos1_;
        }

        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "encode/decode mismatch for uphill "
               "(bit_map=0x%02x start_sn=%u cur_sn=%u uh_id=%u block_size=%u).\r\n",
               (u32)(decode_matrix.uh_fec_code_[uh_pos].fec_pack_->encode_bit_map_),
               start_sn, pack->pack_sn_, (u32)uh_pos, (u32)(decode_matrix.matrix_size_));

uh_calc_restore_loop_next_pos1_:
        loop     += 1;
        move_pos  = (move_pos + h_size - 1) & ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
        bitmap  <<= 1;
    }

uh_calc_restore_check_pos_:
    if ((0 == unrcv_num) || (rcved_num == (u8)(diag_count))) {
        pack_mem_pool_.FreeTranBuf((u8*)(decode_matrix.uh_fec_code_[uh_pos].fec_pack_));
        decode_matrix.uh_fec_code_[uh_pos].fec_pack_  = NULL;
        decode_matrix.uh_fec_code_[uh_pos].tran_addr_ = NULL;
        return 0xFFFF;
    }

    if (1 != unrcv_num) {
        return 0xFFFF;
    }

    return res_pos;
}

goodtp_pos GtpFec2::CalcRestorePosByDHDir(const goodtp_pos &start_cache_pos, const encode_pos &dh_pos,
                                           Fec2EnDeCodeMatrix &decode_matrix) {
    int dh_diff    = (int)dh_pos - (int)(decode_matrix.h_size_ - 2);
    u8  h_size     = (u8)(decode_matrix.h_size_);
    u8  h_pos_start = (dh_diff > 0) ? (u8)dh_diff : (u8)0;
    u8  h_pos_end   = ((int)(h_size - 1) + dh_diff < (int)(h_size - 1))
                      ? (u8)((int)(h_size - 1) + dh_diff) : (u8)(h_size - 1);
    u8  diag_count  = h_pos_end - h_pos_start + 1;

    goodtp_pos res_pos  = 0xFFFF;
    goodtp_pos move_pos = start_cache_pos;
    u8  loop      = 0;
    u8  bitmap    = (u8)(0x01 << h_pos_start);
    u8  unrcv_num = 0;
    u8  rcved_num = 0;
    u32 start_sn  = decode_matrix.start_pack_sn_;
    u32 end_sn    = decode_matrix.start_pack_sn_ + decode_matrix.matrix_size_;

    GtpPacket *pack = NULL;

    if (start_sn <= end_sn) {
        while (diag_count > loop) {
            if (NULL == decode_.fec_buf_.pack_cache_[move_pos]) {
dh_calc_restore_null_pos_:
                if (0x00 != (decode_matrix.dh_fec_code_[dh_pos].fec_pack_->encode_bit_map_ & bitmap)) {
                    res_pos    = move_pos;
                    unrcv_num += 1;
                }
                goto dh_calc_restore_loop_next_pos_;
            }

            pack = (GtpPacket*)(decode_.fec_buf_.pack_cache_[move_pos]->payload_);

            if ((start_sn > pack->pack_sn_) || (end_sn <= pack->pack_sn_)) {
                decode_.fec_buf_.PopPack(move_pos);
                goto dh_calc_restore_null_pos_;
            }

            if (0x00 != (decode_matrix.dh_fec_code_[dh_pos].fec_pack_->encode_bit_map_ & bitmap)) {
                rcved_num += 1;
                goto dh_calc_restore_loop_next_pos_;
            }

            GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelNotice, "encode/decode mismatch for downhill "
                   "(bit_map=0x%02x start_sn=%u cur_sn=%u dh_id=%u block_size=%u).\r\n",
                   (u32)(decode_matrix.dh_fec_code_[dh_pos].fec_pack_->encode_bit_map_),
                   start_sn, pack->pack_sn_, (u32)dh_pos, (u32)(decode_matrix.matrix_size_));

dh_calc_restore_loop_next_pos_:
            loop     += 1;
            move_pos  = (move_pos + h_size + 1) & ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
            bitmap  <<= 1;
        }
        goto dh_calc_restore_check_pos_;
    }

    while (diag_count > loop) {
        if (NULL == decode_.fec_buf_.pack_cache_[move_pos]) {
dh_calc_restore_null_pos1_:
            if (0x00 != (decode_matrix.dh_fec_code_[dh_pos].fec_pack_->encode_bit_map_ & bitmap)) {
                res_pos    = move_pos;
                unrcv_num += 1;
            }
            goto dh_calc_restore_loop_next_pos1_;
        }

        pack = (GtpPacket*)(decode_.fec_buf_.pack_cache_[move_pos]->payload_);

        if ((start_sn > pack->pack_sn_) && (end_sn <= pack->pack_sn_)) {
            decode_.fec_buf_.PopPack(move_pos);
            goto dh_calc_restore_null_pos1_;
        }

        if (0x00 != (decode_matrix.dh_fec_code_[dh_pos].fec_pack_->encode_bit_map_ & bitmap)) {
            rcved_num += 1;
            goto dh_calc_restore_loop_next_pos1_;
        }

        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelError, "encode/decode mismatch for downhill "
               "(bit_map=0x%02x start_sn=%u cur_sn=%u dh_id=%u block_size=%u).\r\n",
               (u32)(decode_matrix.dh_fec_code_[dh_pos].fec_pack_->encode_bit_map_),
               start_sn, pack->pack_sn_, (u32)dh_pos, (u32)(decode_matrix.matrix_size_));

dh_calc_restore_loop_next_pos1_:
        loop     += 1;
        move_pos  = (move_pos + h_size + 1) & ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
        bitmap  <<= 1;
    }

dh_calc_restore_check_pos_:
    if ((0 == unrcv_num) || (rcved_num == (u8)(diag_count))) {
        pack_mem_pool_.FreeTranBuf((u8*)(decode_matrix.dh_fec_code_[dh_pos].fec_pack_));
        decode_matrix.dh_fec_code_[dh_pos].fec_pack_  = NULL;
        decode_matrix.dh_fec_code_[dh_pos].tran_addr_ = NULL;
        return 0xFFFF;
    }

    if (1 != unrcv_num) {
        return 0xFFFF;
    }

    return res_pos;
}

void GtpFec2::ClearReceiveUnUsedResource(const goodtp_pos &current_pos_in_cache) {
    // PushPack has already placed the new packet at current_pos_in_cache (possibly overwriting
    // an old entry). Scan all active decode matrices: if a matrix claims this cache slot but
    // the new packet's SN falls outside that matrix's SN range, the slot was lapped — the
    // matrix can never be fully recovered and must be cleared before its stale FEC data
    // causes a false XOR reconstruction.

    if (NULL == decode_.fec_buf_.pack_cache_[current_pos_in_cache]) {
        return;
    }

    GtpPacket *cur_pack = (GtpPacket*)(decode_.fec_buf_.pack_cache_[current_pos_in_cache]->payload_);
    u32 cur_sn = cur_pack->pack_sn_;

    u16 m_id = 0;
    while (MAX_RECV_FEC2_MATRIX_NUM > m_id) {
        Fec2EnDeCodeMatrix &dm = decode_.decode_matrix_[m_id];

        if (GTP_NO == dm.using_flag_) {
            m_id += 1;
            continue;
        }

        // Compute offset of current_pos_in_cache relative to this matrix's start_pos_.
        // Using modular subtraction avoids branch-heavy range checks.
        goodtp_pos block_offset = (current_pos_in_cache + MAX_FEC2_CACHE_CAPACITY - dm.start_pos_)
                                   & ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);

        if (block_offset >= (goodtp_pos)(dm.matrix_size_)) {
            // current_pos_in_cache is outside this matrix's cache range.
            m_id += 1;
            continue;
        }

        // current_pos_in_cache is inside this matrix's range.
        // Check whether the new packet's SN actually belongs to this matrix.
        u32 matrix_end_sn = dm.start_pack_sn_ + dm.matrix_size_;
        if (GTP_YES == CheckPackSnInBlock(cur_sn, dm.start_pack_sn_, matrix_end_sn)) {
            // Same block: the new packet is a legitimate member; nothing is stale.
            m_id += 1;
            continue;
        }

        // The slot was lapped by a newer SN block.  Free the other cache entries that
        // belong to this stale matrix (skip current_pos_in_cache — that slot now holds
        // valid new data and must not be freed), then reset the matrix.
        goodtp_pos clear_pos = dm.start_pos_;
        u8 steps = 0;
        while (steps < dm.matrix_size_) {
            if ((clear_pos != current_pos_in_cache) && (NULL != decode_.fec_buf_.pack_cache_[clear_pos])) {
                decode_.fec_buf_.PopPack(clear_pos);
            }
            steps    += 1;
            clear_pos = (clear_pos + 1) & ((goodtp_pos)FEC2_CACHE_CAPACITY_MASK);
        }

        dm.Clear(pack_mem_pool_);   // frees H/V/UH/DH FEC packs, sets using_flag_ = GTP_NO

        #ifdef _SELFDEBUG
        GtpLog(write_log_cb_, kGtpFecMd, kGtpLogLevelDebug,
               "ClearReceiveUnUsedResource: cleared stale matrix(m_id=%u start_sn=%u end_sn=%u "
               "new_sn=%u start_pos=%u cur_pos=%u).\r\n",
               (u32)m_id, dm.start_pack_sn_, matrix_end_sn, cur_sn,
               (u32)(dm.start_pos_), (u32)current_pos_in_cache);
        #endif

        m_id += 1;
    }
}

void GtpFec2::ClearNotBelongMatrixPack(const Fec2EnDeCodeMatrix &decode_matrix) {
    if (GTP_NO == decode_matrix.using_flag_) {
        return;
    }

    GtpPacket *pack      = NULL;
    u32 end_pack_sn      = decode_matrix.start_pack_sn_ + decode_matrix.matrix_size_;
    goodtp_pos clear_pos = decode_matrix.start_pos_;
    goodtp_pos end_pos   = clear_pos + ((goodtp_pos)(decode_matrix.matrix_size_));

    end_pos &= FEC2_CACHE_CAPACITY_MASK;

    if (decode_matrix.start_pack_sn_ <= end_pack_sn) {
        while (clear_pos != end_pos) {
            if (NULL != decode_.fec_buf_.pack_cache_[clear_pos]) {
                pack = (GtpPacket*)(decode_.fec_buf_.pack_cache_[clear_pos]->payload_);

                if ((decode_matrix.start_pack_sn_ > pack->pack_sn_) || (end_pack_sn <= pack->pack_sn_)) {
                    decode_.fec_buf_.PopPack(clear_pos);
                }
            }

            clear_pos += 1;
            clear_pos &= FEC2_CACHE_CAPACITY_MASK;
        }

        goto clear_not_belong_matrix_pack_exit_pos_;
    }

    // packet's sn has been turn over.
    while (clear_pos != end_pos) {
        if (NULL != decode_.fec_buf_.pack_cache_[clear_pos]) {
            pack = (GtpPacket*)(decode_.fec_buf_.pack_cache_[clear_pos]->payload_);

            if ((decode_matrix.start_pack_sn_ > pack->pack_sn_) && (end_pack_sn <= pack->pack_sn_)) {
                decode_.fec_buf_.PopPack(clear_pos);
            }
        }

        clear_pos += 1;
        clear_pos &= FEC2_CACHE_CAPACITY_MASK;
    }

clear_not_belong_matrix_pack_exit_pos_:
    return;
}

// @return: < MAX_RECV_FEC2_MATRIX_NUM(sucess), >= MAX_RECV_FEC2_MATRIX_NUM(failed, it means invalid).
u32 GtpFec2::CalcRecvMatrixIdByPackSn(const u32 &pack_sn) {
    // check by block size = 2.
    u32 matrix_id = PackSnToMatrixId(pack_sn, 2);

    if ((MAX_RECV_FEC2_MATRIX_NUM > matrix_id)
     && (GTP_YES == decode_.decode_matrix_[matrix_id].using_flag_)
     && (2 == decode_.decode_matrix_[matrix_id].matrix_size_)
     && (decode_.decode_matrix_[matrix_id].start_pack_sn_ == PackSnToStartSn(pack_sn, 2))) {
        return matrix_id;
    }

    // check by block size = 4.
    matrix_id = PackSnToMatrixId(pack_sn, 4);

    if ((MAX_RECV_FEC2_MATRIX_NUM > matrix_id)
     && (GTP_YES == decode_.decode_matrix_[matrix_id].using_flag_)
     && (4 == decode_.decode_matrix_[matrix_id].matrix_size_)
     && (decode_.decode_matrix_[matrix_id].start_pack_sn_ == PackSnToStartSn(pack_sn, 4))) {
        return matrix_id;
    }

    // check by block size = 16.
    matrix_id = PackSnToMatrixId(pack_sn, 16);

    if ((MAX_RECV_FEC2_MATRIX_NUM > matrix_id)
     && (GTP_YES == decode_.decode_matrix_[matrix_id].using_flag_)
     && (16 == decode_.decode_matrix_[matrix_id].matrix_size_)
     && (decode_.decode_matrix_[matrix_id].start_pack_sn_ == PackSnToStartSn(pack_sn, 16))) {
        return matrix_id;
    }

    // check by block size = 8.
    matrix_id = PackSnToMatrixId(pack_sn, 8);

    if ((MAX_RECV_FEC2_MATRIX_NUM > matrix_id)
     && (GTP_YES == decode_.decode_matrix_[matrix_id].using_flag_)
     && (8 == decode_.decode_matrix_[matrix_id].matrix_size_)
     && (decode_.decode_matrix_[matrix_id].start_pack_sn_ == PackSnToStartSn(pack_sn, 8))) {
        return matrix_id;
    }

    return MAX_RECV_FEC2_MATRIX_NUM;
}

// @return: string's length.
u32 GtpFec2::PrintFec2Param(u8 *out_str, const u32 &mem_size) {
    u8 *wrt_pos = out_str;
    u32 free_sz = mem_size;
    u32 str_len = 0;
    u32 wrt_num = 0;

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\nfec encode(%u pack in buffer):",
                            encode_.fec_buf_.pack_num_in_cache_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = encode_.encode_matrix_.PrintMatrix(wrt_pos, free_sz);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = (u32)snprintf((char*)wrt_pos, free_sz, "\r\n\nfec decode(%u pack in buffer):",
                            decode_.fec_buf_.pack_num_in_cache_);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    wrt_num = decode_.PrintParameter(wrt_pos, free_sz);
    PrintAfterHandlerReturn(str_len, wrt_num, free_sz, wrt_pos);

    return str_len;
}

void GtpFec2::ClearResource(const f32 &loss, const u64 &ts_us) {
    u16 m_id     = 0;
    u16 pack_num = 0;
    goodtp_pos mv_pos  = 0;
    goodtp_pos end_pos = 0;
    u32 start_sn = 0;
    u32 end_sn   = 0;

    GtpPacket *pack = NULL;

    // No.1 step: clear the don't discard block.
    do {
        if (GTP_NO == decode_.decode_matrix_[m_id].using_flag_) {
            goto fec2_clear_resource_next_pos_;
        }

        start_sn = decode_.decode_matrix_[m_id].start_pack_sn_;
        end_sn   = start_sn + decode_.decode_matrix_[m_id].matrix_size_;
        mv_pos   = (u16)(decode_.decode_matrix_[m_id].start_pos_);
        end_pos  = mv_pos + decode_.decode_matrix_[m_id].matrix_size_;
        end_pos &= FEC2_CACHE_CAPACITY_MASK;
        pack_num = 0;

        while (end_pos != mv_pos) {
            if (NULL == decode_.fec_buf_.pack_cache_[mv_pos]) {
                break;
            }

            pack = (GtpPacket*)(decode_.fec_buf_.pack_cache_[mv_pos]->payload_);

            if (GTP_NO == CheckPackSnInBlock(pack->pack_sn_, start_sn, end_sn)) {
                break;
            }

            pack_num += 1;
            mv_pos   += 1;
            mv_pos   &= FEC2_CACHE_CAPACITY_MASK;
        }

        if (((u16)(decode_.decode_matrix_[m_id].matrix_size_)) <= pack_num) {
            // there isn't discarding packet in current block.
            #ifdef _SELFDEBUG
            GtpLog(write_log_cb_, kGtpSessionMd, kGtpLogLevelDebug, "advanced clear fec2 receiving "\
                   "resource(start_pos=%u end_pos=%u dematrix_id=%u book_id=%u matrix_size=%u).\r\n",
                   (u32)(decode_.decode_matrix_[m_id].start_pos_), (u32)end_pos, (u32)m_id,
                   (u32)(decode_.decode_matrix_[m_id].code_book_id_),
                   (u32)(decode_.decode_matrix_[m_id].matrix_size_));
            #endif

            decode_.ClearSpsMatrixResource(decode_.decode_matrix_[m_id]);
        }

fec2_clear_resource_next_pos_:
        m_id += 1;
    } while (MAX_RECV_FEC2_MATRIX_NUM > m_id);

    // No.2 step: clear the oldest resource by pps.
    mv_pos = CalcPosInPackCache(decode_.fec_buf_.max_cached_sn_);

    #if ((0 == APPLICATION_TYPE) || (1 == APPLICATION_TYPE))
    u32 rsv_step[] = {
        12,   // pps < 500
        32,   // pps < 2000
        48,   // pps < 5000
        64,   // pps < 10000
        80,   // pps < 20000
        96,   // pps < 32000
        112   // pps >= 32000
    };

    u32 std_pps[] = {500, 2000, 5000, 10000, 20000, 32000};
    #endif

    #if (2 == APPLICATION_TYPE)
    u32 rsv_step[] = {
        4,   // pps < 10
        4,   // pps < 50
        8,   // pps < 100
        8,   // pps < 150
        8,   // pps < 200
        12,  // pps < 300
        12   // pps >= 300
    };

    u32 std_pps[] = {10, 50, 100, 150, 200, 300};
    #endif

    if (std_pps[0] > pb_dt_->recv_stat_.data_pack_pps_) {
        m_id = 0;
        goto fec2_clear_oldest_resrouce_pos_;
    }

    if (std_pps[1] > pb_dt_->recv_stat_.data_pack_pps_) {
        m_id = 1;
        goto fec2_clear_oldest_resrouce_pos_;
    }

    if (std_pps[2] > pb_dt_->recv_stat_.data_pack_pps_) {
        m_id = 2;
        goto fec2_clear_oldest_resrouce_pos_;
    }

    if (std_pps[3] > pb_dt_->recv_stat_.data_pack_pps_) {
        m_id = 3;
        goto fec2_clear_oldest_resrouce_pos_;
    }

    if (std_pps[4] > pb_dt_->recv_stat_.data_pack_pps_) {
        m_id = 4;
        goto fec2_clear_oldest_resrouce_pos_;
    }

    if (std_pps[5] > pb_dt_->recv_stat_.data_pack_pps_) {
        m_id = 5;
        goto fec2_clear_oldest_resrouce_pos_;
    }
    m_id = 6;

fec2_clear_oldest_resrouce_pos_:
    volatile u32 correct_step = rsv_step[m_id];

    Correction correct_k[] = {
        {CorrectionAct::kZeroAct, CorrectionAct::kDecAct, 0, 3},   // loss < 0.00001(0.125, decrease)
        {CorrectionAct::kAddAct,  CorrectionAct::kDecAct, 0, 0},   // loss < 1.0(1.0, keep)
        {CorrectionAct::kAddAct,  CorrectionAct::kDecAct, 0, 1},   // loss < 10.0(1.5, expand)
        {CorrectionAct::kAddAct,  CorrectionAct::kDecAct, 0, 1},   // loss < 20.0(1.5, expand)
        {CorrectionAct::kAddAct,  CorrectionAct::kDecAct, 1, 0},   // loss < 30.0(2.0, expand)
        {CorrectionAct::kAddAct,  CorrectionAct::kDecAct, 1, 1}    // loss >= 30.0(2.5, expand)
    };

    if (FLOAT_ZERO >= loss) {
        m_id = 0;
        goto fec2_clear_correct_step_pos;
    }

    if (1.00001 > loss) {
        m_id = 1;
        goto fec2_clear_correct_step_pos;
    }

    if (10.00001 > loss) {
        m_id = 2;
        goto fec2_clear_correct_step_pos;
    }

    if (20.00001 > loss) {
        m_id = 3;
        goto fec2_clear_correct_step_pos;
    }

    if (30.00001 > loss) {
        m_id = 4;
        goto fec2_clear_correct_step_pos;
    }
    m_id = 5;

fec2_clear_correct_step_pos:
    volatile i32 calc_step = DoCorrection(correct_step, correct_k[m_id].exp_k_, correct_k[m_id].exp_act_);
    calc_step += DoCorrection(correct_step, correct_k[m_id].dec_k_, correct_k[m_id].dec_act_);

    correct_step = (u32)calc_step;
    correct_step = GtpLimit(MIN_CLEAR_FEC_RECV_STEP, MAX_CLEAR_FEC_RECV_STEP, correct_step);

    if (((goodtp_pos)correct_step) <= mv_pos) {
        end_pos = mv_pos - ((goodtp_pos)correct_step);
    } else {
        end_pos = (mv_pos + MAX_FEC2_CACHE_CAPACITY) - ((goodtp_pos)correct_step);
    }
    end_pos &= FEC2_CACHE_CAPACITY_MASK;

    mv_pos += 1;
    mv_pos &= FEC2_CACHE_CAPACITY_MASK;

    while (end_pos != mv_pos) {
        if (NULL != decode_.fec_buf_.pack_cache_[mv_pos]) {
            decode_.fec_buf_.PopPack(mv_pos);
        }

        mv_pos += 1;
        mv_pos &= FEC2_CACHE_CAPACITY_MASK;
    }

    return;
}

#ifdef __cplusplus
}
#endif

