#ifndef _GOOD_TP_REORDER_WINDOW_H_
#define _GOOD_TP_REORDER_WINDOW_H_

#include "goodtp_comstruct.h"

#include <vector>

class TranMemPool;

class RealtimeReorderWindow {
 public:
    enum PushResult {
        kPushDrop = 0,      // Duplicate packet already in cache; keep silent-drop semantics.
        kPushDirect,
        kPushCached,
        kPushStaleDeliver   // Late FEC/ARQ restored packet; bypass cache and deliver now.
    };

    struct Frame {
        Frame();

        bool Store(const u8 *frame, const u32 &frame_size, const GtpAddr &tran_addr, TranMemPool *pack_mem_pool);
        void MoveFrom(Frame *src);
        void Release(void);
        const u8* Data(void) const;
        u32 Size(void) const;

        std::vector<u8> frame_;
        u8 *pool_frame_;
        u32 frame_size_;
        TranMemPool *pack_mem_pool_;
        GtpAddr tran_addr_;
        u32 sn_;
    };

    struct State {
        u32 count_;
        u32 expect_sn_;
        u64 oldest_age_us_;
        bool expected_ready_;
    };

    RealtimeReorderWindow();

    void Reset(void);
    u32  AvgIntervalUs(void) const;
    u32  Count(void) const;
    u32  ExpectSn(void) const;
    u64  OldestAgeUs(const u64 &ts_us) const;
    bool HasExpectedFrame(void) const;
    State Snapshot(const u64 &ts_us) const;

    PushResult Push(const u32 &sn, const u8 *frame, const u32 &frame_size, const GtpAddr &tran_addr,
                    const u64 &ts_us, TranMemPool *pack_mem_pool = NULL);
    bool PopReady(const u64 &ts_us, const u32 &target_cache_num, const u32 &wait_us, Frame *out_frame);

 private:
    struct Slot {
        Slot();

        bool used_;
        u32 sn_;
        u64 arrival_ts_us_;
        Frame frame_;
    };

    bool SnBefore(const u32 &left, const u32 &right) const;
    u32  SnDistance(const u32 &from, const u32 &to) const;
    void UpdateInterval(const u32 &sn, const u64 &ts_us);
    void EnsureSlots(void);
    void MarkSlotUsed(const u32 &slot_idx);
    void ClearSlot(const u32 &slot_idx);
    Slot* FindNearest(u32 *distance);
    void ClearLateSlots(void);
    void AdvanceToFit(const u32 &sn);
    Slot* SlotBySn(const u32 &sn);

    static const u32 kWindowCapacity = 128;
    static const u32 kBitmapWordBits = 64;
    static const u32 kBitmapWordCount = kWindowCapacity / kBitmapWordBits;

    std::vector<Slot> slots_;
    u64 occupied_bitmap_[kBitmapWordCount];
    u32 count_;
    u32 expect_sn_;
    u64 last_arrival_ts_us_;
    u32 last_arrival_sn_;
    u32 avg_interval_us_;
    bool inited_;

    // expect_sn_ at the moment of the most recent Reset() (GtpSession::ResetSession(), called
    // after a gap too large to bridge incrementally forces a resync). Every delivery path here
    // -- in-order via PopReady/kPushDirect, or out-of-order via kPushStaleDeliver -- only ever
    // hands out an sn strictly before expect_sn_ at the time, so this is a precise "everything
    // before this point was already delivered" watermark that Push() can still consult after
    // Reset() wipes expect_sn_/the cache. Not cleared by Reset() itself -- it needs to survive
    // the very reset it's recorded from. See Push() for how it's used.
    u32  reset_watermark_sn_;
    bool has_reset_watermark_;
};

#endif
