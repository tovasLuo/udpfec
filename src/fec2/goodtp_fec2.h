#ifndef _GOOD_TP_FEC2_H_
#define _GOOD_TP_FEC2_H_
/*********************************************************************************************************************

                           Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_fec2.h
  Version  : Initial
  Author   : Albert.Feng
  Function : The new fec header file for good transport platform.
  Modify record:
  1.Date   : June 26, 2024
    Author : Albert.Feng
    Content: New creates file.

*********************************************************************************************************************/
#include "goodtp_fec2_buffer.h"
#include "goodtp_comstruct.h"
#include "goodtp_macrodefine.h"
#include "tranmempool.h"
#include "goodtp.h"

#define MAX_RECV_FEC2_MATRIX_NUM ((MAX_FEC2_CACHE_CAPACITY >> 1))
#define FEC2_RECV_MATRIX_ID_MASK ((MAX_RECV_FEC2_MATRIX_NUM - 1))

#ifdef __cplusplus
extern "C" {
#endif

class SendFec2CodeMatrix {
 public:
    SendFec2CodeMatrix(const GtpFec2Mode *code_book, const u32 &code_book_id, TranMemPool &pack_mem_pool,
                       pLogCallBack write_log_cb = NULL);
    ~SendFec2CodeMatrix();

    void Init(void);
    void ChangeFecMode(const u32 &new_code_book_id);

    Fec2EnDeCodeMatrix encode_matrix_;

    Fec2Buffer fec_buf_;

PRIVATE:
    TranMemPool &pack_mem_pool_;
    pLogCallBack write_log_cb_;
    const GtpFec2Mode *code_book_;
};

class RecvFec2CodeMatrix {
 public:
    RecvFec2CodeMatrix(const GtpFec2Mode *code_book, const u32 &code_book_id, TranMemPool &pack_mem_pool,
                       pLogCallBack write_log_cb = NULL);
    ~RecvFec2CodeMatrix();

    void Init(void);

    void ClearSpsMatrixResource(Fec2EnDeCodeMatrix &decode_matrix);

    u32  PrintParameter(u8 *out_str, const u32 &mem_size);

    u8  code_book_id_;
    u8  rsv_[3];

    u32 fec_res_sucess_sum_;
    u32 fec_res_failed_sum_;
    u32 fec_res_repeat_sum_;

    Fec2EnDeCodeMatrix decode_matrix_[MAX_RECV_FEC2_MATRIX_NUM];

    Fec2Buffer fec_buf_;

    const GtpFec2Mode *code_book_;

PRIVATE:
    TranMemPool &pack_mem_pool_;
    pLogCallBack write_log_cb_;
};

class GtpFec2 {
 public:
    explicit GtpFec2(pSendPackCallBack send_pack_cb, pLogCallBack write_log_cb, TranMemPool &pack_mem_pool,
                     pFec2RestroreReceive receive_frame_cb, const GtpFec2Mode *fec_code_book);
    ~GtpFec2();

    u32  Init(SessionPublicData *pb_dt);
    void PopAllPack(const u64 &ts_us);
    u32  Encode(GtpPacket *pack, const u64 &ts_us);
    u32  Decode(Fec2CodePack *fec_code, const u64 &ts_us);
    u32  CacheDataPack(GtpPacket *pack, const u64 &ts_us);
    void ChangeFecMode(const u32 &new_code_book_id);
    u32  PrintFec2Param(u8 *out_str, const u32 &mem_size);
    void ClearResource(const f32 &loss, const u64 &ts_us);

PRIVATE:
    void SetEmptyFecEnCode(const u32 &send_fec_pack_flag);
    u32  BlockEncode(GtpPacket *pack, const u64 &ts_us);
    void HorizontalEncode(const encode_pos &h_id, const encode_pos &v_id, u8 *data, const u32 &data_size);
    void VerticalEncode(const encode_pos &h_id, const encode_pos &v_id, u8 *data, const u32 &data_size);
    void SendFecCodePacket(Fec2CodePackMgr &fec_code_mgr);
    void CachedFecEncodePack(Fec2CodePackMgr *fec_code_mgr, Fec2CodePack *fec_code_pack);

    u32  RestoreDataByHDir(const goodtp_pos &h_start_pos, const goodtp_pos &res_pos, const u8 &h_size,
                           Fec2CodePackMgr &fec_code_mgr, const Fec2EnDeCodeMatrix &decode_matrix);
    u32  RestoreDataByVDir(const goodtp_pos &v_start_pos, const goodtp_pos &res_pos, const u8 &h_size,
                      const u8 &v_size, Fec2CodePackMgr &fec_code_mgr, const Fec2EnDeCodeMatrix &decode_matrix);
    u32  RestoreDataByUHDir(const goodtp_pos &uh_start_pos, const goodtp_pos &res_pos, const u8 &h_size,
                       const encode_pos &uh_encode_pos, Fec2CodePackMgr &fec_code_mgr,
                       const Fec2EnDeCodeMatrix &decode_matrix);
    u32  RestoreDataByDHDir(const goodtp_pos &dh_start_pos, const goodtp_pos &res_pos, const u8 &h_size,
                       const encode_pos &dh_encode_pos, Fec2CodePackMgr &fec_code_mgr,
                       const Fec2EnDeCodeMatrix &decode_matrix);

    u32  ReAllocateEncodeMem(const u32 &new_size, Fec2CodePackMgr &fec_code_mgr);
    void TryRecoveryPackByFecPack(const encode_pos &fec_encode_pos, const Fec2CodeDir &fec_encode_dir,
                                  Fec2EnDeCodeMatrix &decode_matrix);
    void TryRecoveryPackByDataPack(const goodtp_pos &cache_pos, Fec2EnDeCodeMatrix &decode_matrix,
                                   const Fec2TryRestoreType &restored_type);
    void ClearReceiveUnUsedResource(const goodtp_pos &current_pos_in_cache);

    void UphillEncode(const encode_pos &h_pos, const encode_pos &v_pos, u8 *data, const u32 &data_size);
    void DownhillEncode(const encode_pos &h_pos, const encode_pos &v_pos, u8 *data, const u32 &data_size);

    goodtp_pos CalcRestorePosByHDir(const goodtp_pos &start_cache_pos, const encode_pos &h_pos,
                                    Fec2EnDeCodeMatrix &decode_matrix);
    goodtp_pos CalcRestorePosByVDir(const goodtp_pos &start_cache_pos, const encode_pos &v_pos,
                                    Fec2EnDeCodeMatrix &decode_matrix);
    goodtp_pos CalcRestorePosByUHDir(const goodtp_pos &start_cache_pos, const encode_pos &uh_pos,
                                     Fec2EnDeCodeMatrix &decode_matrix);
    goodtp_pos CalcRestorePosByDHDir(const goodtp_pos &start_cache_pos, const encode_pos &dh_pos,
                                     Fec2EnDeCodeMatrix &decode_matrix);

    u32  CalcRecvMatrixIdByPackSn(const u32 &pack_sn);
    void ClearNotBelongMatrixPack(const Fec2EnDeCodeMatrix &decode_matrix);

PRIVATE:
    TranMemPool &pack_mem_pool_;

    pSendPackCallBack send_pack_cb_;
    pLogCallBack write_log_cb_;
    pFec2RestroreReceive restorePackRecv_cb_;

    SessionPublicData *pb_dt_;

    u8  using_fec_book_id_;
    u8  byte_rsv_[3];
    u32 next_pack_sn_;

    SendFec2CodeMatrix encode_;
    RecvFec2CodeMatrix decode_;
};

#ifdef __cplusplus
}
#endif

#endif

