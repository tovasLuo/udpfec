#include "dly_cache.h"

void dly_cache::update_start_time(const timespec& cur_sys_ts, uint16_t pkg_start_time_ms)
{
    used_ = 1;
    pkg_start_time_ms_ = pkg_start_time_ms & 0x7fff;

    pkg_enter_sys_time_ms_ = (uint32_t)(((uint64_t)cur_sys_ts.tv_sec * 1000 + (uint64_t)cur_sys_ts.tv_nsec / 1000000) & UINT32_MAX);
}

uint16_t dly_cache::get_start_time(const timespec& ts)
{
    uint32_t delta = 0;
    const uint32_t cur_sys_time_ms = (uint32_t)(((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000) & UINT32_MAX);
    if (pkg_enter_sys_time_ms_ > cur_sys_time_ms)
    {
        //时间翻转
        delta = UINT32_MAX - pkg_enter_sys_time_ms_ + cur_sys_time_ms;
    }
    else
    {
        delta = cur_sys_time_ms - pkg_enter_sys_time_ms_;
    }

    delta += pkg_start_time_ms_;
    delta &= 0x7fff;

    return (uint16_t)delta;
}
