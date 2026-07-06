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

RealtimeReorderWindow::RealtimeReorderWindow() :
    slots_(),
    occupied_bitmap_{0, 0},
    count_(0),
    expect_sn_(0),
    last_arrival_ts_us_(0),
    last_arrival_sn_(0),
    avg_interval_us_(0),
    inited_(false) {
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
    for (u32 i = 0; i < slots_.size(); ++i) {
        slots_[i].frame_.Release();
    }

    std::vector<Slot>().swap(slots_);
    occupied_bitmap_[0]   = 0;
    occupied_bitmap_[1]   = 0;
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

void RealtimeReorderWindow::AdvanceToFit(const u32 &sn) {
    while (kWindowCapacity <= SnDistance(expect_sn_, sn)) {
        if (!slots_.empty()) {
            const u32 slot_idx = expect_sn_ & (kWindowCapacity - 1);
            Slot *slot = &(slots_[slot_idx]);
            if (slot->used_ && (slot->sn_ == expect_sn_)) {
                ClearSlot(slot_idx);
            }
        }
        expect_sn_ += 1;
    }

    ClearLateSlots();
}

RealtimeReorderWindow::PushResult RealtimeReorderWindow::Push(const u32 &sn, const u8 *frame, const u32 &frame_size,
                                                              const GtpAddr &tran_addr, const u64 &ts_us,
                                                              TranMemPool *pack_mem_pool) {
    UpdateInterval(sn, ts_us);

    if (!inited_) {
        expect_sn_ = sn;
        inited_    = true;
    }

    if (SnBefore(sn, expect_sn_)) {
        return kPushStaleDeliver;
    }

    if ((0 == count_) && (sn == expect_sn_)) {
        expect_sn_ += 1;
        return kPushDirect;
    }

    AdvanceToFit(sn);
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

        expect_sn_ = nearest_slot->sn_;
    }

    return false;
}
