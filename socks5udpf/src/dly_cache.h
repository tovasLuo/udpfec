#pragma once
#include <cstdint>
#include <ctime>

class dly_cache
{
public:
    dly_cache()
        : pkg_start_time_ms_(0)
        , used_(0)
        , pkg_enter_sys_time_ms_(0)
    {}

    ~dly_cache(){}

    void update_start_time(const timespec& cur_sys_ts, uint16_t pkg_start_time_ms);
    uint16_t get_start_time(const timespec& ts);

private:
    uint16_t pkg_start_time_ms_ : 15;
    uint16_t used_               : 1;

    uint32_t pkg_enter_sys_time_ms_;    //包进入时的系统时间，用于计算delta
};
