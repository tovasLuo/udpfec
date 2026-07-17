#ifndef _GOOD_TP_REORDER_WINDOW_H_
#define _GOOD_TP_REORDER_WINDOW_H_

#include "goodtp_comstruct.h"

#include <vector>

class TranMemPool;

class RealtimeReorderWindow {
 public:
    enum PushResult {
        kPushDrop = 0,      // Already-delivered duplicate (pre-dates reset_watermark_sn_); silent, uncounted.
        kPushDirect,
        kPushCached,
        kPushStaleDeliver,  // Late FEC/ARQ/NACK/boost rescue arriving after its slot was skipped; delivered
                            // anyway, out of order, because it beat late_grace_us.
        kPushGiveUpDrop     // Arrived more than late_grace_us after the window gave up waiting for it; dropped.
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
                    const u64 &ts_us, TranMemPool *pack_mem_pool = NULL,
                    const u32 &late_grace_us = kDefaultLateGraceUs);
    bool PopReady(const u64 &ts_us, const u32 &target_cache_num, const u32 &wait_us, Frame *out_frame);

 private:
    struct Slot {
        Slot();

        bool used_;
        u32 sn_;
        u64 arrival_ts_us_;
        Frame frame_;
    };

    // Records when the window gave up waiting for a specific sn (skip-ahead or a forced
    // AdvanceToFit() jump), keyed by the same ring-buffer index as Slot. A late arrival for that
    // sn looks this up to get its *actual* elapsed time since being abandoned, instead of
    // estimating it from sn distance x average interval. valid_/sn_ pair guards against index
    // collisions once more than kWindowCapacity sn's have cycled through since this was recorded.
    struct GiveUpRecord {
        GiveUpRecord();

        bool valid_;
        u32 sn_;
        u64 ts_us_;
    };

    bool SnBefore(const u32 &left, const u32 &right) const;
    u32  SnDistance(const u32 &from, const u32 &to) const;
    void UpdateInterval(const u32 &sn, const u64 &ts_us);
    void EnsureSlots(void);
    void MarkSlotUsed(const u32 &slot_idx);
    void ClearSlot(const u32 &slot_idx);
    Slot* FindNearest(u32 *distance);
    void ClearLateSlots(void);
    void AdvanceToFit(const u32 &sn, const u64 &ts_us);
    Slot* SlotBySn(const u32 &sn);
    void MarkGivenUp(const u32 &sn, const u64 &ts_us);
    bool LookupGiveUpTs(const u32 &sn, u64 *out_ts_us) const;

    // Fallback for Push()'s late_grace_us when the caller doesn't pass its own. GtpSession passes
    // an explicit value (10ms, see CalcRealtimeReorderWaitUs()'s ceiling comment for why that
    // number) instead of relying on this.
    static const u32 kDefaultLateGraceUs = 10000;

    static const u32 kWindowCapacity = 128;
    static const u32 kBitmapWordBits = 64;
    static const u32 kBitmapWordCount = kWindowCapacity / kBitmapWordBits;

    std::vector<Slot> slots_;
    u64 occupied_bitmap_[kBitmapWordCount];
    GiveUpRecord give_up_records_[kWindowCapacity];
    u32 count_;
    u32 expect_sn_;
    u64 last_arrival_ts_us_;
    u32 last_arrival_sn_;
    u32 avg_interval_us_;
    bool inited_;

    // expect_sn_ at the moment of the most recent Reset() (GtpSession::ResetSession(), called
    // after a gap too large to bridge incrementally forces a resync). Every delivery path here --
    // in-order via PopReady/kPushDirect, or out-of-order via kPushStaleDeliver -- only ever hands
    // out an sn strictly before expect_sn_ at the time, so this is a precise "everything before
    // this point was already delivered" watermark that Push() can still consult after Reset()
    // wipes expect_sn_/the cache. Not cleared by Reset() itself -- it needs to survive the very
    // reset it's recorded from. See Push() for how it's used.
    u32  reset_watermark_sn_;
    bool has_reset_watermark_;
};

#endif
