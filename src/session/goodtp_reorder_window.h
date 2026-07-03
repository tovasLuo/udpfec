#ifndef _GOOD_TP_REORDER_WINDOW_H_
#define _GOOD_TP_REORDER_WINDOW_H_

#include "goodtp_comstruct.h"

#include <vector>

class RealtimeReorderWindow {
 public:
    enum PushResult {
        kPushDrop = 0,      // 真正的重复包（同一sn已在缓存中），静默丢弃语义不变
        kPushDirect,
        kPushCached,
        kPushStaleDeliver   // expect_sn_已越过此sn（迟到的FEC/ARQ恢复包），不缓存，旁路立即交付
    };

    struct Frame {
        std::vector<u8> frame_;
        GtpAddr tran_addr_;
        u32 sn_;
    };

    RealtimeReorderWindow();

    void Reset(void);
    u32  AvgIntervalUs(void) const;
    u32  Count(void) const;
    u32  ExpectSn(void) const;
    u64  OldestAgeUs(const u64 &ts_us) const;
    bool HasExpectedFrame(void) const;

    PushResult Push(const u32 &sn, const u8 *frame, const u32 &frame_size, const GtpAddr &tran_addr, const u64 &ts_us);
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
    Slot* FindNearest(u32 *distance);
    void ClearLateSlots(void);
    void AdvanceToFit(const u32 &sn);
    Slot* SlotBySn(const u32 &sn);

    static const u32 kWindowCapacity = 128;

    std::vector<Slot> slots_;
    u32 count_;
    u32 expect_sn_;
    u64 last_arrival_ts_us_;
    u32 last_arrival_sn_;
    u32 avg_interval_us_;
    bool inited_;
};

#endif
