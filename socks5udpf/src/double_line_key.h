#pragma once
#include <cstdint>
#include <functional>

struct double_line_key_t
{
    uint32_t user_pass;
    uint16_t double_line_idx;
    uint16_t reserve;

    double_line_key_t()
#if 1
        : double_line_key_t(0, 0)
#else
        : user_pass(0)
        , double_line_idx(0)
        , reserve(0)
#endif
    {}

    double_line_key_t(uint32_t in_user_pass, uint16_t in_double_line_idx)
        : user_pass(in_user_pass)
        , double_line_idx(in_double_line_idx)
        , reserve(0)
    {}

    ~double_line_key_t(){}

    bool operator<(const double_line_key_t& r) const
    {
        if (user_pass != r.user_pass)
        {
            return user_pass < r.user_pass;
        }

        //if (double_line_idx != r.double_line_idx)
        //{
            return double_line_idx < r.double_line_idx;
        //}
    }

#if 1
    bool operator==(const double_line_key_t& r) const
    {
        return user_pass == r.user_pass && double_line_idx == r.double_line_idx;
        //return *(uint64_t*)this == *(uint64_t*)&r;
    }
#endif
};

namespace std
{
    template<>
    struct hash<double_line_key_t>
    {
        size_t operator()(const double_line_key_t& o) const
        {
            return hash<uint32_t>()(o.user_pass) ^ hash<uint16_t>()(o.double_line_idx);
            //return hash<uint64_t>()(*(uint64_t*)&o);
        }
    };

#if 0
    template<>
    struct equal_to<double_line_key_t>
    {
        bool operator()(const double_line_key_t& l, const double_line_key_t& r) const
        {
            //return l.user_pass == r.user_pass && l.double_line_idx == r.double_line_idx;
            //return *(uint64_t*)&l == *(uint64_t*)&r;
        }
    };
#endif
}
