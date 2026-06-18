#pragma once
#include <cstdint>
#include <ctime>

class loss_stat
{
public:
    loss_stat();
    ~loss_stat();

    void reset();

    void cache_pkg_sn(uint16_t pkg_sn, const timespec& cur_sys_ts);
    float get_pkg_loss_rate(const timespec& cur_sys_ts);
    float get_pkg_loss_rate();

private:
    void calc_pkg_loss_rate(const timespec& cur_sys_ts);

private:
    static const uint16_t k_pkg_num_pps_max = 2048;
    static const uint16_t k_pkg_loss_stat_buf_size = k_pkg_num_pps_max >> 3;
    static const uint16_t k_pkg_loss_stat_period = 2000;       //计数丢包率周期(单位:毫秒)
#if 0
    static const uint16_t k_period_no_pkg_count_max = 10;      //UDP端口最大连续统计丢包率周期内未存在报文交互
#endif

    uint32_t pkg_loss_rate_;                                   //单位: 10000%即精确到小数点两位
    uint8_t  k_pkg_loss_stat_buf_[k_pkg_loss_stat_buf_size];   //每个包序列号占有一个bit

    uint16_t pos_idx_min_;
    uint16_t pos_idx_mid_;
    uint16_t pos_idx_max_;

    uint16_t period_pkg_sn_min_;
    uint16_t period_pkg_sn_mid_;
    uint16_t period_pkg_sn_max_;

    bool     first_pkg_;
    bool     period_no_pkg_;

    uint64_t pos_idx_mid_time_ms_;                              //周期内统计丢包率结束点的系统时间
    uint64_t calc_loss_rate_time_ms_;                           //开始计算丢包率的系统时间
};
