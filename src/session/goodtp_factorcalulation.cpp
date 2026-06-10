/*********************************************************************************************************************

                           Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_factorcalculation.cpp
  Version  : 1.0
  Author   : zn.ye
  Function :
  Modify record:
  1.Date   : Dec 26th. 2023
    Author : zn.ye
    Content: Initialize the file.

*********************************************************************************************************************/
#include "goodtp_factorcalulation.h"
#include <cmath>
#include <cstdint>
#include "goodtp_mgr.h"

// --- EWMA start ---
EWMA::EWMA(float beta) : tail_(), head_(), size_(), beta_(beta) {
    for (int i = 0; i < SF_MAX_NUM; ++i) {
        arr[i].Reset();
    }

    // set size
    uint16_t i = 0;
    for (; i < SF_MAX_NUM; ++i) {
        if (pow(beta, i) < exp(-1)) {  // beta^i < e^-1
            break;
        }
    }
    size_ = i > SF_MAX_NUM ? SF_MAX_NUM : i;

    reserved_[0] = 0;
    reserved_[1] = 0;
    reserved_[2] = 0;
    reserved_[3] = 0;
    reserved_[4] = 0;
    reserved_[5] = 0;
}

void EWMA::NewSendingFreq(const SendingFrequence &sf) {
    arr[tail_] = sf;  // overflow mode.
    if (Full()) {
        head_ = Next(head_);
    }
    tail_ = Next(tail_);
}

void EWMA::ClearTimeoutSendingFreq() {
    if (Empty()) return;
    uint64_t now   = GtpSysTimestampUs();
    uint16_t index = head_;
    for (int i = 0; i < size_; ++i) {
        if (now - arr[index].timestamp_us > SD_TIMEOUT_US) {
            head_ = Next(head_);
        }
        index = Next(index);
    }
}

float EWMA::Formula(uint16_t temp_tail) {
    if (Prev(temp_tail) != head_) {
        return beta_ * Formula(Prev(temp_tail)) + (1 - beta_) * arr[Prev(temp_tail)].factorA;
    }
    return arr[Prev(temp_tail)].factorA;
}

float EWMA::NewFactor(const SendingFrequence &sf) {
    // 1. clear the timeout sf
    ClearTimeoutSendingFreq();
    // 2. write the new sf to the array.
    NewSendingFreq(sf);
    // 3. calculate the factor
    return Formula(tail_);
}
// --- EWMA end ---

FactorCalculation::FactorCalculation()
    : mi_f(DEFAULT_MI_F), ad_f(DEFAULT_AD_F), factorA_(1 / DEFALUT_FACTOR_1), feedback_link_loss_(), ewma_() {}

// --- MIAD ---
void FactorCalculation::AdditiveDecreaseFactorA() {
    if (factorA_ - ad_f > MIN_FACTOR_A) {
        factorA_ -= ad_f;
        return;
    }
    factorA_ = MIN_FACTOR_A;
}

void FactorCalculation::MultiplicativeIncreaseFactorA() {
    if (factorA_ * mi_f >= MAX_FACTOR_A) {
        factorA_ = MAX_FACTOR_A;
        return;
    }
    factorA_ *= mi_f;
}
// --- MIAD ---

bool FactorCalculation::FloatEqual(float a, float b, float precision) {
    return (a - b >= -precision && a - b <= precision);
}

float FactorCalculation::Factor1(const float &loss_rate, const u64 &ts_us) {
    if (loss_rate < 0 || loss_rate > 100.0) return DEFALUT_FACTOR_1;
    switch (AdjustType(loss_rate)) {
        case SendingFrequenceAdjustType::kAdditiveDecrease: {
            AdditiveDecreaseFactorA();
            break;
        }
        case SendingFrequenceAdjustType::kMultiplicativeIncrease: {
            MultiplicativeIncreaseFactorA();
            break;
        }
        case SendingFrequenceAdjustType::kKeep: {
            break;
        }
        case SendingFrequenceAdjustType::kFullSpeed: {
            feedback_link_loss_ = loss_rate;
            factorA_            = MAX_FACTOR_A;
            return MIN_FACTOR_1;
        }

        default:
            break;
    }

    // Smooth factorA_ via EWMA to avoid jarring jumps in Factor1
    // when individual loss samples fluctuate (beta=0.82, window≈6 samples).
    SendingFrequence new_sf = {};
    new_sf.timestamp_us = ts_us;
    new_sf.factorA      = factorA_;
    float new_factor_ewma = ewma_.NewFactor(new_sf);

    feedback_link_loss_ = loss_rate;
    return 1.0f / new_factor_ewma;
}

void FactorCalculation::SetCfg1(const FactorCfg1 &cfg) {
    mi_f = cfg.mi_f;
    ad_f = cfg.ad_f;
    ewma_.SetBeta(cfg.beta);
}

void FactorCalculation::SetCfg2(const FactorCfg2 &cfg) {}

SendingFrequenceAdjustType FactorCalculation::AdjustType(const float &loss_rate) {
    // MIMD: Multiplicative Increase when no loss, Additive Decrease when loss present/rising.
    // factorA_ ∈ [MIN=1, MAX=10]; Factor1 = 1/factorA_ ∈ [0.1, 1.0].
    // Larger factorA_ → smaller Factor1 → less intervention (good network).
    // Smaller factorA_ → larger Factor1 → more aggressive loss response.

    if (loss_rate == 0) {
        return SendingFrequenceAdjustType::kMultiplicativeIncrease;  // no loss: grow factorA_ fast
    }

    float difference = loss_rate - feedback_link_loss_;

    if (difference >= 0) {
        return SendingFrequenceAdjustType::kAdditiveDecrease;  // loss stable or rising: shrink factorA_
    }

    return SendingFrequenceAdjustType::kKeep;  // loss falling: hold current rate
}
