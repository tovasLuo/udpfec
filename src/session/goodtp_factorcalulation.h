#pragma once
/*********************************************************************************************************************

                           Copyright (C), 2021-2031, Free Albort.Feng Studio

 *********************************************************************************************************************
  File name: goodtp_factorcalculation.h
  Version  : 1.0
  Author   : zn.ye
  Function :
  Modify record:
  1.Date   : Dec 26th. 2023
    Author : zn.ye
    Content: Initialize the file.

*********************************************************************************************************************/

#include <cstdint>
#include "goodtp_comstruct.h"

#define SF_MAX_NUM (16)
#define DEFAULT_BETA (0.82f)
#define DEFALUT_FACTOR_1 (0.2f)   // 0.1 ~ 1
#define MIN_FACTOR_1 (0.1f)
#define DEFALUT_FACTOR_2 (8)     // 1 ~ 30
#define SD_TIMEOUT_US (5000000)  // expired time: 5 seconds.
#define DEFAULT_MI_F (4.0f)
#define DEFAULT_AD_F (1.0f)
#define DEFAULT_FACTOR_A (5.0f)
#define MIN_FACTOR_A (1.0f)
#define MAX_FACTOR_A (10.0f)
#define DEFAULT_THRESHOLD_VALUE (40.0f)

enum class LossType {
    kLossBeZero,
    kLossLineFailure,
    kLossCongestion,
};

struct FactorCfg1{
    float mi_f;
    float ad_f;
    float beta;
};

struct FactorCfg2 {};

// union Cfg {
//     Cfg(float mi, float ad, float beta) : cfg1({mi, ad, beta}) {}
//     Cfg() : cfg2({}) {}
//     FactorCfg1 cfg1;
//     FactorCfg2 cfg2;
// };

struct SendingFrequence {
    SendingFrequence() : timestamp_us(0), factorA(0.0f), reserved_bytes(0) {}

    uint64_t timestamp_us;
    float    factorA;  // [0.526, 10];
    uint32_t reserved_bytes;
    void     Reset() {
        timestamp_us   = 0;
        factorA        = 0.0f;
        reserved_bytes = 0;
    }
};

class EWMA {
 public:
    explicit EWMA(float beta = DEFAULT_BETA);
    ~EWMA() = default;

    /**
     * @brief Calculate a new factor using EWMA based on the newly received factor and return the value of that factor
     *
     * @param sf
     * @return float
     */
    float NewFactor(const SendingFrequence& sf);
    void  SetBeta(float beta) { beta_ = beta; }

  PRIVATE:
    /**
     * @brief Store the newly received factor.
     *
     * @param sf
     */
    void NewSendingFreq(const SendingFrequence& sf);

    /**
     * @brief Clear the expired factors.
     *
     */
    void ClearTimeoutSendingFreq();

    /**
     * @brief The formula for calculating the EWMA.
     *
     * @param temp_tail
     * @return float
     */
    float Formula(uint16_t temp_tail);

    uint16_t Next(uint16_t index) { return (index + 1) % (size_ + 1); }
    uint16_t Prev(uint16_t index) { return (index + size_) % (size_ + 1); }
    uint16_t Size() { return (tail_ + size_ + 1 - head_) % (size_ + 1); }

    bool     Full() { return Next(tail_) == head_; }
    bool     Empty() { return tail_ == head_; }

    SendingFrequence arr[SF_MAX_NUM];
    float beta_;  // beta <= 1, the bigger beta is, the more weight the new factor has. The coefficient beta represents
                  // the rate of weighted decline, with smaller values indicating a faster decline.
    uint16_t tail_;
    uint16_t head_;
    uint16_t size_;
    uint8_t  reserved_[6];
};

enum class SendingFrequenceAdjustType {
    kMultiplicativeIncrease,
    kAdditiveDecrease,
    kKeep,
    kFullSpeed,
};

/**
 * @brief The first factor determines the packet sending speed of the feedback
 * link, ranges from 0.1 to 1.9. The value type is a floating-point number; The second factor determines the rto sn of
 * slide-window, ranges from 1 to 30.
 *
 */
class FactorCalculation {
 public:
    /**
     * @brief strategies for the first factor:
     * 1. Multi-level Thresholds: Set multiple packet loss rate thresholds, each corresponding to a different packet
     * sending rate adjustment strategy. For example. a slight increase in packet loss rate may cause a small increase
     * in the packet sending rate, while a higher packet loss rate triggers a decrease in the rate, thus finely
     * controlling the packet sending rate.
     *
     * 2. Smoothing Factor(EWMA, Exponetially Weighted Moving-Average): When calculating the packet sending rate, a
     * smoothing factor can be used to avoid drastic rate adjustments due to temporary changes. This helps to smooth out
     * uint16_t-term fluctuations and prevent overreactions.
     *
     * 3. MIAD Mechanism: This is a classic congestion control method that employs Multiplicative Increase when
     * increasing the packet send rate and Additive Decrease when congestion is detected.
     *
     * 4. Feedback Mechanism: Real-time monitoring of network feedback (such as ACKS or NACKs) is used to adjust the
     * packet sending rate, ensuring timely and accurate responses.
     *
     * 5. User-configurable Parameters: Allow administrators or users to adjust relevant parameters according to the
     * specific conditions of the network, such as thresholds, caps, and smoothing factors.
     */

    FactorCalculation();
    ~FactorCalculation() = default;

    /* @param loss_rate
     * @param rtt
     * @return float
     */
    // float                      Factor1(float loss_rate, LossType loss_type);
    float                      Factor1(const float &loss_rate, const u64 &ts_us);
    uint32_t                   Factor2();
    void                       SetCfg1(const FactorCfg1& cfg);
    void                       SetCfg2(const FactorCfg2& cfg);
    SendingFrequenceAdjustType AdjustType(const float &loss_rate);
  PRIVATE :
    void            AdditiveDecreaseFactorA();
    void            MultiplicativeIncreaseFactorA();
    bool            FloatEqual(float a, float b, float precision);

    float                  mi_f;  // multiplicative_increase_factor_;
    float                  ad_f;  // additive_decrease_factor_;
    float                  factorA_;
    float                  feedback_link_loss_;
    EWMA                   ewma_;
};
