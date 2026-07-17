#include "goodtp_reorder_window.h"

#include "tranmempool.h"

static inline u32 GtpReorderCtz64(const u64 &value) {
#if defined(__GNUC__) || defined(__clang__)
    return (u32)__builtin_ctzll(value);
#else
    u32 count = 0;
    u64 tmp = value;
    while (0 == (tmp & 1)) {
        tmp >>= 1;
        count += 1;
    }
    return count;
#endif
}

RealtimeReorderWindow::Frame::Frame() :
    frame_(),
    pool_frame_(NULL),
    frame_size_(0),
    pack_mem_pool_(NULL),
    tran_addr_(),
    sn_(0) {
}

bool RealtimeReorderWindow::Frame::Store(const u8 *frame, const u32 &frame_size, const GtpAddr &tran_addr,
                                         TranMemPool *pack_mem_pool) {
    Release();

    if ((NULL != pack_mem_pool) && (NULL != frame) && (0 != frame_size)) {
        u8 *pool_frame = pack_mem_pool->TranBufUseRefAddOne((u8*)frame);
        if (NULL != pool_frame) {
            pool_frame_ = pool_frame;
            frame_size_ = frame_size;
            pack_mem_pool_ = pack_mem_pool;
            tran_addr_ = tran_addr;
            return true;
        }
    }

    frame_.assign(frame, frame + frame_size);
    frame_size_ = frame_size;
    tran_addr_ = tran_addr;
    return true;
}

void RealtimeReorderWindow::Frame::MoveFrom(Frame *src) {
    if (NULL == src) {
        return;
    }

    Release();

    frame_.swap(src->frame_);
    pool_frame_ = src->pool_frame_;
    frame_size_ = src->frame_size_;
    pack_mem_pool_ = src->pack_mem_pool_;
    tran_addr_ = src->tran_addr_;
    sn_ = src->sn_;

    src->pool_frame_ = NULL;
    src->frame_size_ = 0;
    src->pack_mem_pool_ = NULL;
    src->sn_ = 0;
}

void RealtimeReorderWindow::Frame::Release(void) {
    if ((NULL != pack_mem_pool_) && (NULL != pool_frame_)) {
        pack_mem_pool_->FreeTranBuf(pool_frame_);
    }

    pool_frame_ = NULL;
    frame_size_ = 0;
    pack_mem_pool_ = NULL;
    frame_.clear();
}

const u8* RealtimeReorderWindow::Frame::Data(void) const {
    return (NULL != pool_frame_) ? pool_frame_ : frame_.data();
}

u32 RealtimeReorderWindow::Frame::Size(void) const {
    return (NULL != pool_frame_) ? frame_size_ : (u32)frame_.size();
}

RealtimeReorderWindow::Slot::Slot() :
    used_(false),
    sn_(0),
    arrival_ts_us_(0),
    frame_() {
}

RealtimeReorderWindow::GiveUpRecord::GiveUpRecord() :
    valid_(false),
    sn_(0),
    ts_us_(0) {
}

RealtimeReorderWindow::RealtimeReorderWindow() :
    slots_(),
    occupied_bitmap_{0, 0},
    give_up_records_(),
    count_(0),
    expect_sn_(0),
    last_arrival_ts_us_(0),
    last_arrival_sn_(0),
    avg_interval_us_(0),
    inited_(false),
    reset_watermark_sn_(0),
    has_reset_watermark_(false) {
}

u32 RealtimeReorderWindow::Count(void) const {
    return count_;
}

u32 RealtimeReorderWindow::ExpectSn(void) const {
    return expect_sn_;
}

u64 RealtimeReorderWindow::OldestAgeUs(const u64 &ts_us) const {
    u64 oldest_age_us = 0;

    for (u32 word_idx = 0; word_idx < kBitmapWordCount; ++word_idx) {
        u64 bitmap = occupied_bitmap_[word_idx];
        while (0 != bitmap) {
            const u32 bit_pos = GtpReorderCtz64(bitmap);
            const u32 slot_idx = (word_idx << 6) + bit_pos;
            bitmap &= bitmap - 1;

            const u64 arrival_ts_us = slots_[slot_idx].arrival_ts_us_;
            const u64 age_us = ts_us >= arrival_ts_us ? (ts_us - arrival_ts_us) : (arrival_ts_us - ts_us);
            if (oldest_age_us < age_us) {
                oldest_age_us = age_us;
            }
        }
    }

    return oldest_age_us;
}

bool RealtimeReorderWindow::HasExpectedFrame(void) const {
    if ((!inited_) || slots_.empty()) {
        return false;
    }

    const Slot &slot = slots_[expect_sn_ & (kWindowCapacity - 1)];
    return slot.used_ && (slot.sn_ == expect_sn_);
}

RealtimeReorderWindow::State RealtimeReorderWindow::Snapshot(const u64 &ts_us) const {
    State state;
    state.count_ = count_;
    state.expect_sn_ = expect_sn_;
    state.oldest_age_us_ = OldestAgeUs(ts_us);
    state.expected_ready_ = HasExpectedFrame();
    return state;
}

void RealtimeReorderWindow::Reset(void) {
    // Record where delivery had gotten to before wiping it: every sn this window has ever handed
    // to the app (in-order or via kPushStaleDeliver) was strictly before expect_sn_ at the time,
    // so this is a precise boundary Push() can still check after the reset re-seeds expect_sn_ to
    // the post-gap sn. Deliberately not reset alongside the fields below -- see the header comment.
    if (inited_) {
        reset_watermark_sn_ = expect_sn_;
        has_reset_watermark_ = true;
    }

    for (u32 i = 0; i < slots_.size(); ++i) {
        slots_[i].frame_.Release();
    }

    std::vector<Slot>().swap(slots_);
    occupied_bitmap_[0]   = 0;
    occupied_bitmap_[1]   = 0;
    for (u32 i = 0; i < kWindowCapacity; ++i) {
        give_up_records_[i].valid_ = false;
    }
    count_              = 0;
    expect_sn_          = 0;
    last_arrival_ts_us_ = 0;
    last_arrival_sn_    = 0;
    avg_interval_us_    = 0;
    inited_             = false;
}

bool RealtimeReorderWindow::SnBefore(const u32 &left, const u32 &right) const {
    return (left != right) && (((u32)(right - left)) < 0x80000000U);
}

u32 RealtimeReorderWindow::SnDistance(const u32 &from, const u32 &to) const {
    return to - from;
}

void RealtimeReorderWindow::UpdateInterval(const u32 &sn, const u64 &ts_us) {
    if (0 == last_arrival_ts_us_) {
        last_arrival_ts_us_ = ts_us;
        last_arrival_sn_    = sn;
        return;
    }

    u32 sn_delta = SnDistance(last_arrival_sn_, sn);
    if ((0 == sn_delta) || (kWindowCapacity < sn_delta) || SnBefore(sn, last_arrival_sn_)) {
        last_arrival_ts_us_ = ts_us;
        last_arrival_sn_    = sn;
        return;
    }

    u64 ts_delta = ts_us >= last_arrival_ts_us_ ? (ts_us - last_arrival_ts_us_) : (last_arrival_ts_us_ - ts_us);
    u32 interval_us = (u32)(ts_delta / sn_delta);
    if ((0 != interval_us) && (200000 > interval_us)) {
        if (0 == avg_interval_us_) {
            avg_interval_us_ = interval_us;
        } else {
            avg_interval_us_ = ((avg_interval_us_ * 7) + interval_us) >> 3;
        }
    }

    last_arrival_ts_us_ = ts_us;
    last_arrival_sn_    = sn;
}

u32 RealtimeReorderWindow::AvgIntervalUs(void) const {
    return avg_interval_us_;
}

RealtimeReorderWindow::Slot* RealtimeReorderWindow::SlotBySn(const u32 &sn) {
    return &(slots_[sn & (kWindowCapacity - 1)]);
}

void RealtimeReorderWindow::MarkGivenUp(const u32 &sn, const u64 &ts_us) {
    GiveUpRecord &rec = give_up_records_[sn & (kWindowCapacity - 1)];
    rec.valid_ = true;
    rec.sn_    = sn;
    rec.ts_us_ = ts_us;
}

bool RealtimeReorderWindow::LookupGiveUpTs(const u32 &sn, u64 *out_ts_us) const {
    const GiveUpRecord &rec = give_up_records_[sn & (kWindowCapacity - 1)];
    if ((!rec.valid_) || (rec.sn_ != sn)) {
        return false;
    }

    *out_ts_us = rec.ts_us_;
    return true;
}

void RealtimeReorderWindow::EnsureSlots(void) {
    if (slots_.empty()) {
        slots_.resize(kWindowCapacity);
    }
}

void RealtimeReorderWindow::MarkSlotUsed(const u32 &slot_idx) {
    occupied_bitmap_[slot_idx >> 6] |= ((u64)1) << (slot_idx & 0x3FU);
}

void RealtimeReorderWindow::ClearSlot(const u32 &slot_idx) {
    Slot &slot = slots_[slot_idx];
    if (!slot.used_) {
        occupied_bitmap_[slot_idx >> 6] &= ~(((u64)1) << (slot_idx & 0x3FU));
        return;
    }

    slot.used_ = false;
    slot.frame_.Release();
    occupied_bitmap_[slot_idx >> 6] &= ~(((u64)1) << (slot_idx & 0x3FU));
    count_ -= 1;
}

void RealtimeReorderWindow::ClearLateSlots(void) {
    for (u32 word_idx = 0; word_idx < kBitmapWordCount; ++word_idx) {
        u64 bitmap = occupied_bitmap_[word_idx];
        while (0 != bitmap) {
            const u32 bit_pos = GtpReorderCtz64(bitmap);
            const u32 slot_idx = (word_idx << 6) + bit_pos;
            bitmap &= bitmap - 1;

            if (SnBefore(slots_[slot_idx].sn_, expect_sn_)) {
                ClearSlot(slot_idx);
                bitmap = occupied_bitmap_[word_idx] & ~((((u64)1) << bit_pos) - 1ULL);
            }
        }
    }
}

void RealtimeReorderWindow::AdvanceToFit(const u32 &sn, const u64 &ts_us) {
    while (kWindowCapacity <= SnDistance(expect_sn_, sn)) {
        if (!slots_.empty()) {
            const u32 slot_idx = expect_sn_ & (kWindowCapacity - 1);
            Slot *slot = &(slots_[slot_idx]);
            if (slot->used_ && (slot->sn_ == expect_sn_)) {
                ClearSlot(slot_idx);
            } else {
                // expect_sn_ never arrived -- being force-abandoned here (a new arrival is more
                // than a full window ahead), not just skipped-ahead-and-maybe-still-coming. Same
                // give-up bookkeeping as PopReady()'s skip-ahead so a late straggler for this sn
                // still gets a correct elapsed-time reading in Push().
                MarkGivenUp(expect_sn_, ts_us);
            }
        }
        expect_sn_ += 1;
    }

    ClearLateSlots();
}

RealtimeReorderWindow::PushResult RealtimeReorderWindow::Push(const u32 &sn, const u8 *frame, const u32 &frame_size,
                                                              const GtpAddr &tran_addr, const u64 &ts_us,
                                                              TranMemPool *pack_mem_pool,
                                                              const u32 &late_grace_us) {
    UpdateInterval(sn, ts_us);

    if (!inited_) {
        expect_sn_ = sn;
        inited_    = true;
    }

    if (SnBefore(sn, expect_sn_)) {
        // sn is provably already delivered (in-order or via a previous stale-rescue) if it falls
        // at or before the boundary recorded by the last Reset() -- everything this window has
        // ever handed to the app came from strictly before expect_sn_ at the time it was handed
        // out, so reset_watermark_sn_ (== expect_sn_ just before the reset) is a precise cutoff,
        // not a guess. Without this check, a straggler/retransmission of a packet delivered
        // *before* a GtpSession::ResetSession() resync (which wipes filter_win_'s dedup memory
        // along with this window) looks identical to a genuine one-time late arrival and gets
        // redelivered. sn strictly after the watermark was never delivered -- it's either within
        // the outage gap itself or a recent genuine straggler -- and must still go through
        // kPushStaleDeliver (see commit history: dropping those unconditionally by distance alone
        // was tried and made things worse, since most far-behind stragglers after a big resync
        // are first deliveries, not duplicates).
        if (has_reset_watermark_ && ((sn == reset_watermark_sn_) || SnBefore(sn, reset_watermark_sn_))) {
            return kPushDrop;
        }

        // Give-up gate: real elapsed time since the window gave up waiting for this exact sn
        // (recorded by MarkGivenUp() at the moment PopReady()/AdvanceToFit() skipped past it),
        // not an sn-distance proxy. Whatever rescued it -- FEC completing late, a NACK-triggered
        // ARQ resend, a boost clone -- gets judged by the same clock: if it shows up within
        // late_grace_us of being abandoned, still deliver it out of order; past that, it's no more
        // useful than dropping it outright (see DeliverFrameInOrder()'s comment on why stale
        // rewinds hurt). Caller passes CalcRealtimeReorderWaitUs()'s own +10ms grace margin (see
        // that function's ceiling comment) so this give-up line and the skip-ahead ceiling share
        // one budget instead of two independently-tuned numbers.
        //
        // No recorded timestamp (LookupGiveUpTs() fails) means this specific sn was never actually
        // abandoned by this window -- filter_win_ upstream already screens out duplicates of
        // normally in-order-delivered packets before they ever reach here, so the only way to land
        // in this branch without a record is an sn old enough that its give_up_records_ slot has
        // since been overwritten by a more recent sn cycling through the same ring position (more
        // than kWindowCapacity sn's later). That's unambiguously stale -- drop it.
        u64 given_up_ts_us = 0;
        if (!LookupGiveUpTs(sn, &given_up_ts_us)) {
            return kPushGiveUpDrop;
        }

        const u64 staleness_us = (ts_us >= given_up_ts_us) ? (ts_us - given_up_ts_us) : 0;
        if (staleness_us > late_grace_us) {
            return kPushGiveUpDrop;
        }

        return kPushStaleDeliver;
    }

    if ((0 == count_) && (sn == expect_sn_)) {
        expect_sn_ += 1;
        return kPushDirect;
    }

    AdvanceToFit(sn, ts_us);
    EnsureSlots();

    Slot *slot = SlotBySn(sn);
    if (slot->used_) {
        if (slot->sn_ == sn) {
            return kPushDrop;
        }

        ClearSlot(sn & (kWindowCapacity - 1));
    }

    slot->used_ = true;
    slot->sn_   = sn;
    slot->arrival_ts_us_ = ts_us;
    slot->frame_.Store(frame, frame_size, tran_addr, pack_mem_pool);
    slot->frame_.sn_ = sn;
    MarkSlotUsed(sn & (kWindowCapacity - 1));
    count_ += 1;

    return kPushCached;
}

RealtimeReorderWindow::Slot* RealtimeReorderWindow::FindNearest(u32 *distance) {
    ClearLateSlots();

    if (0 == count_) {
        *distance = 0xFFFFFFFFU;
        return NULL;
    }

    Slot *best_slot = NULL;
    u32 best_distance = 0xFFFFFFFFU;

    for (u32 word_idx = 0; word_idx < kBitmapWordCount; ++word_idx) {
        u64 bitmap = occupied_bitmap_[word_idx];
        while (0 != bitmap) {
            const u32 bit_pos = GtpReorderCtz64(bitmap);
            const u32 slot_idx = (word_idx << 6) + bit_pos;
            Slot *slot = &(slots_[slot_idx]);
            const u32 cur_distance = SnDistance(expect_sn_, slot->sn_);

            if (cur_distance < best_distance) {
                best_distance = cur_distance;
                best_slot = slot;
            }

            bitmap &= bitmap - 1;
        }
    }

    *distance = best_distance;
    return best_slot;
}

bool RealtimeReorderWindow::PopReady(const u64 &ts_us, const u32 &target_cache_num, const u32 &wait_us,
                                     Frame *out_frame) {
    while (0 != count_) {
        Slot *slot = SlotBySn(expect_sn_);
        if (slot->used_ && (slot->sn_ == expect_sn_)) {
            out_frame->MoveFrom(&slot->frame_);
            out_frame->sn_ = slot->sn_;
            ClearSlot(expect_sn_ & (kWindowCapacity - 1));
            expect_sn_ += 1;
            return true;
        }

        u32 nearest_distance = 0;
        Slot *nearest_slot = FindNearest(&nearest_distance);
        if (NULL == nearest_slot) {
            return false;
        }

        const u64 arrival_ts_us = nearest_slot->arrival_ts_us_;
        const u64 age_us = ts_us >= arrival_ts_us ? (ts_us - arrival_ts_us) : (arrival_ts_us - ts_us);
        if ((count_ < target_cache_num) && (age_us < wait_us)) {
            return false;
        }

        // Giving up on every sn strictly between expect_sn_ and the slot we're skipping to --
        // record when, so a late arrival for any of them can look up its own real elapsed time in
        // Push() instead of a distance-based estimate. Bounded by nearest_slot's distance, which
        // FindNearest() only ever returns from within this same kWindowCapacity ring, so this loop
        // is small.
        for (u32 gone_sn = expect_sn_; gone_sn != nearest_slot->sn_; gone_sn += 1) {
            MarkGivenUp(gone_sn, ts_us);
        }

        expect_sn_ = nearest_slot->sn_;
    }

    return false;
}
