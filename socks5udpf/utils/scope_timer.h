#pragma once
#include <chrono>
#include <cstdint>

class scope_timer
{
public:
    scope_timer();
    virtual ~scope_timer();
    void reset();

    int64_t elapsed() const;
    double elapsed_second() const;

    int64_t elapsed_nano() const;
    int64_t elapsed_micro() const;
    int64_t elapsed_milli() const;
    int64_t elapsed_seconds() const;
    int64_t elapsed_minutes() const;
    int64_t elapsed_hours() const;

private:
    std::chrono::time_point<std::chrono::steady_clock> begin_;
};


inline scope_timer::scope_timer()
    : begin_(std::chrono::steady_clock::now())
{
}

inline scope_timer::~scope_timer()
{
}

inline void scope_timer::reset()
{
    begin_ = std::chrono::steady_clock::now();
}

inline int64_t scope_timer::elapsed() const
{
    return elapsed_milli();
}

inline double scope_timer::elapsed_second() const
{
    return std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - begin_).count();
}

inline int64_t scope_timer::elapsed_milli() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin_).count();
}

inline int64_t scope_timer::elapsed_micro() const
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin_).count();
}

inline int64_t scope_timer::elapsed_nano() const
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin_).count();
}

inline int64_t scope_timer::elapsed_seconds() const
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - begin_).count();
}

inline int64_t scope_timer::elapsed_minutes() const
{
    return std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now() - begin_).count();
}

inline int64_t scope_timer::elapsed_hours() const
{
    return std::chrono::duration_cast<std::chrono::hours>(std::chrono::steady_clock::now() - begin_).count();
}

