#pragma once
#ifdef _WIN32
//#ifndef WIN32_LEAN_AND_MEAN
//#define WIN32_LEAN_AND_MEAN
//#endif
//#include <WinSock2.h>
//#include <Windows.h>
#include <errhandlingapi.h>
#include <profileapi.h>
#include <sysinfoapi.h>
#include <timezoneapi.h>
#endif
#include <cstring>
#include <ctime>
#include <string>

#ifdef _WIN32
#define localtime_r(a, b) localtime_s(b, a)
#define gmtime_r(a, b)    gmtime_s(b, a)

#define asctime_r(a, b)   asctime_s(b, 26, a)
#define ctime_r(a, b)     ctime_s(b, 26, a)

#if 0
// 见time.h
struct timespec
{
    time_t tv_sec; // seconds
    long tv_nsec;  // nanoseconds
};

// 见WinSock2.h
struct timeval
{
    long tv_sec;   // seconds
    long tv_usec;  // microseconds
};
#endif

struct timezone
{
    int tz_minuteswest; // Minutes west of GMT
    int tz_dsttime;     // Nonzero if DST is ever in effect
};

/* Identifier for system-wide realtime clock.  */
# define CLOCK_REALTIME         0
/* Monotonic system-wide clock.  */
# define CLOCK_MONOTONIC        1

// 实现clock_gettime函数
inline int clock_gettime(int clock_id, struct timespec* ts)
{
    if (ts == NULL)
    {
        SetLastError(EINVAL);
        return -1;
    }

    if (clock_id == CLOCK_REALTIME)
    {
        FILETIME ft;
        //memset(&ft, 0, sizeof(ft));
#if (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
        GetSystemTimePreciseAsFileTime(&ft);
#else
        GetSystemTimeAsFileTime(&ft);
#endif

        LARGE_INTEGER t;
        memset(&t, 0, sizeof(t));
        t.LowPart = ft.dwLowDateTime;
        t.HighPart = ft.dwHighDateTime;

        // 将FILETIME转换为UNIX时间（1970年1月1日以来的秒数）
        t.QuadPart -= 116444736000000000LL;  // FILETIME是从1601年1月1日开始计算的100ns数，而UNIX时间是从1970年1月1日开始的

        ts->tv_sec = (time_t)(t.QuadPart / 10000000LL);
        ts->tv_nsec = (long)((t.QuadPart % 10000000LL)*100LL);

        return 0;
    }
    else if (clock_id == CLOCK_MONOTONIC)
    {
        LARGE_INTEGER freq = {};
        LARGE_INTEGER count = {};
        //memset(&freq, 0, sizeof(freq));
        //memset(&count, 0, sizeof(count));

        BOOL ret = TRUE;
        ret &= QueryPerformanceFrequency(&freq);
        ret &= QueryPerformanceCounter(&count);

        ts->tv_sec = (time_t)(count.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)((count.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
        return ret ? 0 : -1;
    }

    SetLastError(EINVAL);
    return -1;  // 未支持的clock_id
}

// 实现gettimeofday函数
inline int gettimeofday(struct timeval* tv, struct timezone* tz)
{
    bool ok = true;
    if (tv != NULL)
    {
        struct timespec ts = {};
        //memset(&ts, 0, sizeof(ts));
        ok &= (clock_gettime(CLOCK_REALTIME, &ts) != -1);
        tv->tv_sec = (long)ts.tv_sec;
        tv->tv_usec = ts.tv_nsec / (long)1000;
    }

    if (tz != NULL)
    {
        TIME_ZONE_INFORMATION tzi = {};
        //memset(&tzi, 0, sizeof(tzi));
        ok &= (GetTimeZoneInformation(&tzi) != TIME_ZONE_ID_INVALID);
        tz->tz_minuteswest = tzi.Bias;
        tz->tz_dsttime = tzi.DaylightBias != 0 ? 1 : 0;
    }

    return ok ? 0 : -1;
}
#endif

namespace timespec_helper
{
    inline std::string timespec_string(const timespec& ts, bool is_local = true);
    inline timespec& timespec_fix(timespec& ts);

    inline timespec timespec_addition(const timespec& ts1, const timespec& ts2);
    inline timespec timespec_subtraction(const timespec& ts1, const timespec& ts2);
    inline int timespec_compare(const timespec& ts1, const timespec& ts2);
    inline bool timespec_equal(const timespec& ts1, const timespec& ts2);

    inline timespec timespec_addition_safe(timespec ts1, timespec ts2);
    inline timespec timespec_subtraction_safe(timespec ts1, timespec ts2);
    inline int timespec_compare_safe(timespec ts1, timespec ts2);
    inline bool timespec_equal_safe(timespec ts1, timespec ts2);

    inline bool timespec_diff(const timespec& ts_larger, const timespec& ts_smaller, const timespec& ts_diff);
    inline bool timespec_diff(const timespec& ts_larger, const timespec& ts_smaller, uint64_t nsec);

    std::string timespec_string(const timespec& ts, bool is_local /*= true*/)
    {
        struct tm tm_tmp;
        memset(&tm_tmp, 0, sizeof(tm_tmp));
        if (is_local)
        {
            localtime_r(&ts.tv_sec, &tm_tmp);
        }
        else
        {
            gmtime_r(&ts.tv_sec, &tm_tmp);
        }

        char buf[128] = { 0 };
        size_t len0 = strftime(buf, sizeof(buf), "%FT%T", &tm_tmp);
        /*size_t len1 = (size_t)*/snprintf(buf + len0, sizeof(buf) - len0, ".%06ld", ts.tv_nsec / 1000);
        //strftime(buf + len0 + len1, sizeof(buf) - len0 - len1, "%z", &tm_tmp);

        return buf;
    }

    timespec& timespec_fix(timespec& ts)
    {
        if (ts.tv_nsec <= -1000000000 || ts.tv_nsec >= 1000000000)
        {
            ts.tv_sec += ts.tv_nsec / 1000000000;
            ts.tv_nsec %= (long)1000000000;
        }

        if ((ts.tv_nsec < 0 && ts.tv_sec > 0) || (ts.tv_nsec > 0 && ts.tv_sec < 0))
        {
            ts.tv_sec += (ts.tv_nsec < 0) ? -1 : 1;
            ts.tv_nsec += (ts.tv_nsec < 0) ? 1000000000 : -1000000000;
        }

        return ts;
    }

    timespec timespec_addition_safe(timespec ts1, timespec ts2)
    {
        timespec_fix(ts1);
        timespec_fix(ts2);
        return timespec_addition(ts1, ts2);
    }

    timespec timespec_addition(const timespec& ts1, const timespec& ts2)
    {
        timespec ts;
        memset(&ts, 0, sizeof(ts));

        ts.tv_sec = ts1.tv_sec + ts2.tv_sec;
        ts.tv_nsec = ts1.tv_nsec + ts2.tv_nsec;

        return timespec_fix(ts);
    }

    timespec timespec_subtraction_safe(timespec ts1, timespec ts2)
    {
        timespec_fix(ts1);
        timespec_fix(ts2);
        return timespec_subtraction(ts1, ts2);
    }

    timespec timespec_subtraction(const timespec& ts1, const timespec& ts2)
    {
        timespec ts;
        memset(&ts, 0, sizeof(ts));

        ts.tv_sec = ts1.tv_sec - ts2.tv_sec;
        ts.tv_nsec = ts1.tv_nsec - ts2.tv_nsec;

        return timespec_fix(ts);
    }

    int timespec_compare_safe(timespec ts1, timespec ts2)
    {
        timespec_fix(ts1);
        timespec_fix(ts2);
        return timespec_compare(ts1, ts2);
    }

    int timespec_compare(const timespec& ts1, const timespec& ts2)
    {
        if (ts1.tv_sec != ts2.tv_sec)
        {
            return ts1.tv_sec < ts2.tv_sec ? -1 : 1;
        }

        if (ts1.tv_nsec != ts2.tv_nsec)
        {
            return ts1.tv_nsec < ts2.tv_nsec ? -1 : 1;
        }

        return 0;
    }

    bool timespec_equal_safe(timespec ts1, timespec ts2)
    {
        timespec_fix(ts1);
        timespec_fix(ts2);
        return timespec_equal(ts1, ts2);
    }

    bool timespec_equal(const timespec& ts1, const timespec& ts2)
    {
        return ts1.tv_sec == ts2.tv_sec && ts1.tv_nsec == ts2.tv_nsec;
    }

    bool timespec_diff(const timespec& ts_larger, const timespec& ts_smaller, const timespec& ts_diff)
    {
        timespec ts = timespec_subtraction(ts_larger, ts_smaller);
        return timespec_compare(ts, ts_diff) >= 0;
    }

    bool timespec_diff(const timespec& ts_larger, const timespec& ts_smaller, uint64_t nsec)
    {
        timespec ts_diff;
        memset(&ts_diff, 0, sizeof(ts_diff));
        ts_diff.tv_sec = (time_t)(nsec / 1000000000);
        ts_diff.tv_nsec = (long)(nsec % 1000000000);
        timespec_fix(ts_diff);

        return timespec_diff(ts_larger, ts_smaller, ts_diff);
    }
}

