#pragma once
#include <cstdint>
#include <string>
#include <vector>

inline uint8_t hexchar_uint8(char ch)
{
    if ('0' <= ch &&  ch <= '9')
    {
        return (uint8_t)(ch - '0');
    }

    if ('A' <= ch &&  ch <= 'F')
    {
        return (uint8_t)(ch - 'A' + 10);
    }

    if ('a' <= ch &&  ch <= 'f')
    {
        return (uint8_t)(ch - 'a' + 10);
    }

    return 0xFF;
}

inline char uint8_hexchar(uint8_t num, bool is_lower = true)
{
    if (/*0 <= num && */num <= 9)
    {
        return (char)(num + '0');
    }

    if (0xA <= num &&  num <= 0xF)
    {
        return is_lower ? (char)(num - 10 + 'a') : (char)(num - 10 + 'A');
    }

    return ' ';
}

inline std::vector<uint8_t> hexstr_mem(const std::string& str_in)
{
    std::vector<uint8_t> ret_val;
    if (str_in.empty())
    {
        return ret_val;
    }

    std::string str = str_in;
    if (str_in.size() % 2 != 0)
    {
        str += '0';
    }
    ret_val.reserve(str.size() / 2);

    for (size_t i = 0; i < str.size(); i += 2)
    {
        uint8_t val = (uint8_t)(hexchar_uint8(str[i]) << 4) | hexchar_uint8(str[i + 1]);
        ret_val.push_back(val);
    }

    return ret_val;
}

inline std::string mem_hexstr(const uint8_t* data, size_t len, bool is_lower = true)
{
    std::string ret_val;
    if (data == nullptr || len == 0)
    {
        return ret_val;
    }

    ret_val.reserve(len * 2);

    for (size_t i = 0; i < len; ++i)
    {
        ret_val.push_back(uint8_hexchar((data[i] & 0xF0) >> 4, is_lower));
        ret_val.push_back(uint8_hexchar(data[i] & 0x0F, is_lower));
    }

    return ret_val;
}

inline std::string mem_hexstr(const std::vector<uint8_t>& data, bool is_lower = true)
{
    if (data.empty())
    {
        return std::string();
    }
    return mem_hexstr(data.data(), data.size(), is_lower);
}
