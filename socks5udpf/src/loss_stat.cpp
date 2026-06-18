#include "loss_stat.h"
#include <cassert>
#include <cstring>
#include "buf_operation.h"

loss_stat::loss_stat()
{
    reset();
}

loss_stat::~loss_stat()
{
}

void loss_stat::reset()
{
    pkg_loss_rate_ = 0;
    memset(k_pkg_loss_stat_buf_, 0, sizeof(k_pkg_loss_stat_buf_));

    pos_idx_min_ = 0;
    pos_idx_mid_ = 0;
    pos_idx_max_ = 0;

    period_pkg_sn_min_ = 0;
    period_pkg_sn_mid_ = 0;
    period_pkg_sn_max_ = 0;

    first_pkg_ = true;
    period_no_pkg_ = true;

    timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1){}

    uint64_t cur_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    pos_idx_mid_time_ms_    = cur_ms + (k_pkg_loss_stat_period >> 2) + (k_pkg_loss_stat_period >> 1);
    calc_loss_rate_time_ms_ = cur_ms + k_pkg_loss_stat_period;
}

void loss_stat::cache_pkg_sn(uint16_t pkg_sn, const timespec& cur_sys_ts)
{
    period_no_pkg_ = false;

    uint64_t cur_ms = (uint64_t)cur_sys_ts.tv_sec * 1000 + (uint64_t)cur_sys_ts.tv_nsec / 1000000;

    if (first_pkg_)
    {
        first_pkg_ = false;

        period_pkg_sn_min_ = pkg_sn;
        period_pkg_sn_mid_ = pkg_sn;
        period_pkg_sn_max_ = pkg_sn;

        set_buf_bit(k_pkg_loss_stat_buf_, k_pkg_loss_stat_buf_size, pos_idx_min_);
    }
    else
    {
        uint16_t delta = 0;
        if (pkg_sn < period_pkg_sn_min_)
        {
            delta = (uint16_t)(UINT16_MAX + 1 - period_pkg_sn_min_ + pkg_sn);
        }
        else
        {
            delta = (uint16_t)(pkg_sn - period_pkg_sn_min_);
        }

        //当前pkg_sn波动太大,缓存无法统计需复位
        if (delta >= k_pkg_num_pps_max)
        {
            reset();
            return;
        }

        delta = (uint16_t)(delta + pos_idx_min_);
        delta %= k_pkg_num_pps_max;

        set_buf_bit(k_pkg_loss_stat_buf_, k_pkg_loss_stat_buf_size, delta);

        if (cur_ms < pos_idx_mid_time_ms_) //1500ms
        {
            if (pkg_sn > period_pkg_sn_mid_)
            {
                period_pkg_sn_mid_ = pkg_sn;
                pos_idx_mid_ = delta;
            }
        }

        if (pkg_sn > period_pkg_sn_max_) //500ms
        {
            period_pkg_sn_max_ = pkg_sn;
            pos_idx_max_ = delta;
        }
    }

    if (cur_ms < calc_loss_rate_time_ms_)
    {
        //计算丢包率时间未到,不做计算
        return;
    }

    calc_pkg_loss_rate(cur_sys_ts);
}

void loss_stat::calc_pkg_loss_rate(const timespec& cur_sys_ts)
{
    assert(pos_idx_min_ < k_pkg_num_pps_max);
    assert(pos_idx_mid_ < k_pkg_num_pps_max);
    assert(pos_idx_max_ < k_pkg_num_pps_max);

    //包来的太慢,时间已超过了
    if (pos_idx_mid_ == pos_idx_min_)
    {
        pos_idx_mid_ = pos_idx_max_;
        period_pkg_sn_mid_ = period_pkg_sn_max_;
    }

    uint16_t pkg_loss_count = 0;
    uint16_t pkg_total = 0;
    if (pos_idx_min_ <=  pos_idx_mid_)
    {
        for (uint16_t i = pos_idx_min_; i < pos_idx_mid_; ++i)
        {
            if (get_buf_bit(k_pkg_loss_stat_buf_, k_pkg_loss_stat_buf_size, i) == 0)
            {
                ++pkg_loss_count;
            }
            else
            {
                clr_buf_bit(k_pkg_loss_stat_buf_, k_pkg_loss_stat_buf_size, i);
            }

            ++pkg_total;
        }
    }
    else //if (pos_idx_min_ > pos_idx_mid_)
    {
        for (uint16_t i = pos_idx_min_; i < k_pkg_num_pps_max; ++i)
        {
            if (get_buf_bit(k_pkg_loss_stat_buf_, k_pkg_loss_stat_buf_size, i) == 0)
            {
                ++pkg_loss_count;
            }
            else
            {
                clr_buf_bit(k_pkg_loss_stat_buf_, k_pkg_loss_stat_buf_size, i);
            }

            ++pkg_total;
        }

        for (uint16_t i = 0; i < pos_idx_mid_; ++i)
        {
            if (get_buf_bit(k_pkg_loss_stat_buf_, k_pkg_loss_stat_buf_size, i) == 0)
            {
                ++pkg_loss_count;
            }
            else
            {
                clr_buf_bit(k_pkg_loss_stat_buf_, k_pkg_loss_stat_buf_size, i);
            }

            ++pkg_total;
        }
    }

    pkg_loss_rate_ = 0;
    if (pkg_total != 0)
    {
        pkg_loss_rate_ = pkg_loss_count * 10000 / pkg_total;
    }

    //500ms
    period_pkg_sn_min_ = period_pkg_sn_mid_;
    pos_idx_min_        = pos_idx_mid_;

    uint64_t cur_ms = (uint64_t)cur_sys_ts.tv_sec * 1000 + (uint64_t)cur_sys_ts.tv_nsec / 1000000;

    pos_idx_mid_time_ms_    = cur_ms + (k_pkg_loss_stat_period >> 2) + (k_pkg_loss_stat_period >> 1); //1500ms
    calc_loss_rate_time_ms_ = cur_ms + k_pkg_loss_stat_period;

    period_no_pkg_ = true;
}

float loss_stat::get_pkg_loss_rate()
{
    timespec cur_sys_ts;
    memset(&cur_sys_ts, 0, sizeof(cur_sys_ts));
    if (clock_gettime(CLOCK_MONOTONIC, &cur_sys_ts) == -1) {}

    return get_pkg_loss_rate(cur_sys_ts);
}

float loss_stat::get_pkg_loss_rate(const timespec& cur_sys_ts)
{
    uint16_t period_no_pkg_count = 0;

    if (period_no_pkg_)
    {
        uint64_t cur_ms = (uint64_t)cur_sys_ts.tv_sec * 1000 + (uint64_t)cur_sys_ts.tv_nsec / 1000000;
        if (cur_ms >= calc_loss_rate_time_ms_)
        {
            period_no_pkg_count = (uint16_t)((cur_ms - calc_loss_rate_time_ms_) % k_pkg_loss_stat_period);
            period_no_pkg_count = (uint16_t)(period_no_pkg_count + 1);
        }
    }

#if 0
    //连续多次都未计算过丢包率,说明该UDP端口后续未有包交互
    if (period_no_pkg_count >= k_period_no_pkg_count_max)
    {
        pkg_loss_rate_ = 0;
        return 0.0f;
    }
#endif

    if (period_no_pkg_count > 1)
    {
        return 0.0f;
    }

    float pkg_loss_rate = (float)pkg_loss_rate_ / 100.0f;
    if (pkg_loss_rate > 100.0001f) //精度问题
    {
        pkg_loss_rate = 0.0f;
    }

    return pkg_loss_rate;
}

