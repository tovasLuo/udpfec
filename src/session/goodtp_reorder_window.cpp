#include "goodtp_reorder_window.h"

RealtimeReorderWindow::Slot::Slot() :
    used_(false),
    sn_(0),
    arrival_ts_us_(0),
    frame_() {
}

RealtimeReorderWindow::RealtimeReorderWindow() :
    slots_(),
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

    for (u32 i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].used_) {
            continue;
        }

        const u64 arrival_ts_us = slots_[i].arrival_ts_us_;
        const u64 age_us = ts_us >= arrival_ts_us ? (ts_us - arrival_ts_us) : (arrival_ts_us - ts_us);
        if (oldest_age_us < age_us) {
            oldest_age_us = age_us;
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

void RealtimeReorderWindow::Reset(void) {
    std::vector<Slot>().swap(slots_);
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

void RealtimeReorderWindow::ClearLateSlots(void) {
    for (u32 i = 0; i < slots_.size(); ++i) {
        if (slots_[i].used_ && SnBefore(slots_[i].sn_, expect_sn_)) {
            slots_[i].used_ = false;
            slots_[i].frame_.frame_.clear();
            count_ -= 1;
        }
    }
}

void RealtimeReorderWindow::AdvanceToFit(const u32 &sn) {
    while (kWindowCapacity <= SnDistance(expect_sn_, sn)) {
        if (!slots_.empty()) {
            Slot *slot = SlotBySn(expect_sn_);
            if (slot->used_ && (slot->sn_ == expect_sn_)) {
                slot->used_ = false;
                slot->frame_.frame_.clear();
                count_ -= 1;
            }
        }
        expect_sn_ += 1;
    }

    ClearLateSlots();
}

RealtimeReorderWindow::PushResult RealtimeReorderWindow::Push(const u32 &sn, const u8 *frame, const u32 &frame_size,
                                                              const GtpAddr &tran_addr, const u64 &ts_us) {
    UpdateInterval(sn, ts_us);

    if (!inited_) {
        expect_sn_ = sn;
        inited_    = true;
    }

    if (SnBefore(sn, expect_sn_)) {
        return kPushDrop;
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

        slot->frame_.frame_.clear();
        count_ -= 1;
    }

    slot->used_ = true;
    slot->sn_   = sn;
    slot->arrival_ts_us_ = ts_us;
    slot->frame_.frame_.assign(frame, frame + frame_size);
    slot->frame_.tran_addr_ = tran_addr;
    count_ += 1;

    return kPushCached;
}

RealtimeReorderWindow::Slot* RealtimeReorderWindow::FindNearest(u32 *distance) {
    Slot *best_slot    = NULL;
    u32 best_distance = 0xFFFFFFFFU;

    ClearLateSlots();

    for (u32 i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].used_) {
            continue;
        }

        const u32 cur_distance = SnDistance(expect_sn_, slots_[i].sn_);
        if (cur_distance < best_distance) {
            best_distance = cur_distance;
            best_slot     = &(slots_[i]);
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
            out_frame->frame_.swap(slot->frame_.frame_);
            out_frame->tran_addr_ = slot->frame_.tran_addr_;
            out_frame->sn_ = slot->sn_;
            slot->used_ = false;
            slot->frame_.frame_.clear();
            count_ -= 1;
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
