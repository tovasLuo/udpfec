#pragma once
#include <algorithm>
#include <cassert>
#include <cctype>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>
#include "ctype_helper.h"
#ifdef __GNUC__
#include <cxxabi.h>
#endif

#if __cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L) || (defined(_MSC_VER) && _MSC_VER >= 1600)
#include <regex>
#endif

#if _WIN32
#pragma warning(disable:4996)
#endif

//#ifdef _MSC_VER
//#define _CRT_SECURE_NO_WARNINGS
//#endif

//https://www.cnblogs.com/5iedu/articles/5541058.html

template<typename string_t, class = typename std::enable_if<std::is_same<std::string, string_t>::value || std::is_same<std::wstring, string_t>::value, string_t>::type>
struct string_helper
{
    static_assert(std::is_same<std::string, string_t>::value || std::is_same<std::wstring, string_t>::value, "string_t must be a std::string or std::wstring");

    static string_t spaces()
    {
        return ctype_helper<typename string_t::value_type>::spaces();
    }

    static string_t blanks()
    {
        return ctype_helper<typename string_t::value_type>::blanks();
    }

    template<typename I>
    static typename std::enable_if<std::is_same<I, int>::value, I>::type
    to_integral(const string_t& str, std::size_t* pos = nullptr, int base = 0)
    {
        return std::stoi(str, pos, base);
    }

    template<typename I>
    static typename std::enable_if<std::is_same<I, long>::value, I>::type
    to_integral(const string_t& str, std::size_t* pos = nullptr, int base = 0)
    {
        return std::stol(str, pos, base);
    }

    template<typename I>
    static typename std::enable_if<std::is_same<I, long long>::value, I>::type
    to_integral(const string_t& str, std::size_t* pos = nullptr, int base = 0)
    {
        return std::stoll(str, pos, base);
    }

    template<typename I>
    static typename std::enable_if<std::is_same<I, unsigned long>::value, I>::type
    to_integral(const string_t& str, std::size_t* pos = nullptr, int base = 0)
    {
        return std::stoul(str, pos, base);
    }

    template<typename I>
    static typename std::enable_if<std::is_same<I, unsigned long long>::value, I>::type
    to_integral(const string_t& str, std::size_t* pos = nullptr, int base = 0)
    {
        return std::stoull(str, pos, base);
    }

    template<typename I>
    static typename std::enable_if<std::is_same<I, unsigned int>::value, I>::type
    to_integral(const string_t& str, std::size_t* pos = nullptr, int base = 0)
    {
        if (sizeof(unsigned int) <= sizeof(unsigned long))
        {
            return static_cast<unsigned int>(to_integral<unsigned long>(str, pos, base));
        }

        assert(sizeof(unsigned int) == sizeof(unsigned long long));
        return static_cast<unsigned int>(to_integral<unsigned long long>(str, pos, base));
    }

    template<typename I>
    static typename std::enable_if<
        std::is_integral<I>::value &&
        !(std::is_same<I, bool>::value ||
          std::is_same<I, int>::value || std::is_same<I, unsigned int>::value ||
          std::is_same<I, long>::value || std::is_same<I, unsigned long>::value ||
          std::is_same<I, long long>::value || std::is_same<I, unsigned long long>::value), I>::type
    to_integral(const string_t& str, std::size_t* pos = nullptr, int base = 0)
    {
        long tmp = std::stoi(str, pos, base);
        if (std::numeric_limits<I>::lowest() > tmp || tmp > (std::numeric_limits<I>::max)())
        {
            std::unique_ptr<char, void(*)(void*)> own
            (
#ifndef __GNUC__
                nullptr,
#else
                abi::__cxa_demangle(typeid(I).name(), nullptr,
                    nullptr, nullptr),
#endif
                std::free
            );
            const std::string I_name = own != nullptr ? own.get() : typeid(I).name();
            throw std::out_of_range("to_integral<" + I_name + "> argument out of range");
        }
        return static_cast<I>(tmp);
    }

    template<typename I, class = typename std::enable_if <std::is_integral<I>::value && !std::is_same<I, bool>::value, I > ::type >
    static bool
    to_integral(const string_t& str, I& value, std::size_t* pos = nullptr, int base = 0)
    {
        try
        {
            value = to_integral<I>(str, pos, base);
        }
        catch (std::exception&/* e*/)
        {
            return false;
        }

        return true;
    }

    template<typename F>
    static typename std::enable_if<std::is_same<F, float>::value, F>::type
    to_floating_point(const string_t& str, std::size_t* pos = nullptr)
    {
        return std::stof(str, pos);
    }

    template<typename F>
    static typename std::enable_if<std::is_same<F, double>::value, F>::type
    to_floating_point(const string_t& str, std::size_t* pos = nullptr)
    {
        return std::stod(str, pos);
    }

    template<typename F>
    static typename std::enable_if<std::is_same<F, long double>::value, F>::type
    to_floating_point(const string_t& str, std::size_t* pos = nullptr)
    {
        return std::stold(str, pos);
    }

    template<typename F, class = typename std::enable_if<std::is_floating_point<F>::value, F>::type>
    static bool
    to_floating_point(const string_t& str, F& value, std::size_t* pos = nullptr)
    {
        try
        {
            value = to_floating_point<F>(str, pos);
        }
        catch (std::exception&/* e*/)
        {
            return false;
        }

        return true;
    }

    template<typename A>
    static typename std::enable_if<std::is_integral<A>::value && !std::is_same<A, bool>::value, A>::type
    to_arithmetic(const string_t& str, std::size_t* pos = nullptr)
    {
        return to_integral<A>(str, pos);
    }

    template<typename A>
    static typename std::enable_if<std::is_floating_point<A>::value, A>::type
    to_arithmetic(const string_t& str, std::size_t* pos = nullptr)
    {
        return to_floating_point<A>(str, pos);
    }

    template<typename A, class = typename std::enable_if<std::is_arithmetic<A>::value && !std::is_same<A, bool>::value, A>::type>
    static bool
    to_arithmetic(const string_t& str, A& value, std::size_t* pos = nullptr)
    {
        try
        {
            value = to_arithmetic<A>(str, pos);
        }
        catch (std::exception&/* e*/)
        {
            return false;
        }

        return true;
    }

    template<typename A>
    static typename std::enable_if<std::is_arithmetic<A>::value && !std::is_same<A, bool>::value, A>::type
    to_arithmetic_default(const string_t& str, A def = A(0), std::size_t* pos = nullptr)
    {
        A ret_val = 0;
        if (!to_arithmetic(str, ret_val, pos))
        {
            return def;
        }

        return ret_val;
    }

    template<typename A>
    static typename std::enable_if<std::is_arithmetic<A>::value && std::is_same<std::string, string_t>::value, string_t>::type
        to_string(A value)
    {
        return std::to_string(value);
    }

    template<typename A>
    static typename std::enable_if<std::is_arithmetic<A>::value && std::is_same<std::wstring, string_t>::value, string_t>::type
        to_string(A value)
    {
        return std::to_wstring(value);
    }

    static string_t to_lower(string_t const& str)
    {
        string_t dst{};
        std::transform(str.begin(), str.end(), std::insert_iterator<string_t>(dst, dst.end()),
            [](typename string_t::value_type c) { return ctype_helper<typename string_t::value_type>::tolower(c); });
        return dst;
    }

    static string_t to_upper(string_t const& str)
    {
        string_t dst{};
        std::transform(str.begin(), str.end(), std::insert_iterator<string_t>(dst, dst.end()),
            [](typename string_t::value_type c) { return ctype_helper<typename string_t::value_type>::toupper(c); });
        return dst;
    }

    static string_t& to_lower_output_self(string_t& str)
    {
        std::transform(str.begin(), str.end(), str.begin(),
            [](typename string_t::value_type c) { return ctype_helper<typename string_t::value_type>::tolower(c); });
        return str;
    }

    static string_t& to_upper_output_self(string_t& str)
    {
        std::transform(str.begin(), str.end(), str.begin(),
            [](typename string_t::value_type c) { return ctype_helper<typename string_t::value_type>::toupper(c); });
        return str;
    }

    static string_t reverse(string_t const& str)
    {
        string_t new_str(str);
        std::reverse(new_str.begin(), new_str.end());
        return new_str;
    }

    static void trim_left(string_t& str, string_t const& chars_to_trim = spaces())
    {
        assert(!chars_to_trim.empty());
        if (str.empty())
        {
            return;
        }

        str.erase(0, str.find_first_not_of(chars_to_trim));
    }

    static void trim_right(string_t& str, string_t const& chars_to_trim = spaces())
    {
        assert(!chars_to_trim.empty());
        if (str.empty())
        {
            return;
        }

        typename string_t::size_type pos = str.find_last_not_of(chars_to_trim);
        if (pos == string_t::npos)
        {
            str.clear();
            return;
        }

        if (++pos != string_t::npos)
        {
            str.erase(pos);
        }
    }

    static void trim(string_t& str, string_t const& chars_to_trim = spaces())
    {
        trim_right(str, chars_to_trim);
        trim_left(str, chars_to_trim);
    }

#if __cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L) || (defined(_MSC_VER) && _MSC_VER >= 1600)
    static std::vector<string_t> split_regex(const string_t& s, const string_t& r)
    {
        assert(!r.empty());
        const std::basic_regex<typename string_t::value_type> re(r);

        // passing -1 as the submatch index parameter performs splitting
        std::regex_token_iterator<typename string_t::const_iterator> it(s.begin(), s.end(), re, -1);
        return{ it,{} };

#if 0
        if (keep_empty)
        {
            return{ it,{} };
        }

        std::regex_token_iterator<typename string_t::const_iterator> reg_end;
        std::vector<string_t> result;

        for (; it != reg_end; ++it)
        {
            if (it->str().empty())
            {
                continue;
            }

            result.emplace_back(it->str());
        }

        return result;
#endif
    }
#endif

    static std::vector<string_t> split_compress(const string_t& s, const string_t& delims)
    {
        assert(!delims.empty());

        std::vector<string_t> result;
        for (typename string_t::size_type elem_start = s.begin(), elem_end = s.end();
            elem_end != string_t::npos && (elem_start = s.find_first_not_of(delims, elem_end)) != string_t::npos;
            )
        {
            elem_end = s.find_first_of(delims, elem_start);

#if __cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L) || (defined(_MSC_VER) && _MSC_VER >= 1600)
            result.emplace_back(
                s, elem_start, elem_end == string_t::npos ? string_t::npos : elem_end - elem_start);
#else
            result.push_back(
                s.substr(elem_start, elem_end == string_t::npos ? string_t::npos : elem_end - elem_start));
#endif
        }
        return result;
    }

    static std::vector<string_t> split(const string_t& s, const string_t& delims, const bool keep_empty = true)
    {
        assert(!delims.empty());

        std::vector<string_t> result;
        for (typename string_t::size_type elem_start = 0, elem_end = 0;
            elem_end != string_t::npos && elem_start < s.size();
            elem_start = elem_end + 1)
        {
            elem_end = s.find_first_of(delims, elem_start);
            if (elem_end == elem_start && !keep_empty)
            {
                continue;
            }

#if __cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L) || (defined(_MSC_VER) && _MSC_VER >= 1600)
            result.emplace_back(
                s, elem_start, elem_end == string_t::npos ? string_t::npos : elem_end - elem_start);
#else
            result.push_back(
                s.substr(elem_start, elem_end == string_t::npos ? string_t::npos : elem_end - elem_start));
#endif
        }
        return result;
    }

    template<typename UnaryPredicate>
    static std::vector<string_t> split_pre(const string_t& s, UnaryPredicate p, const bool keep_empty = true)
    {
        std::vector<string_t> result;
        for (typename string_t::const_iterator elem_start = s.begin(), elem_end = s.begin();
            elem_end != s.end() && elem_start != s.end();
            elem_start = elem_end == s.end() ? s.end() : ++elem_end)
        {
            elem_end = std::find_if(elem_start, s.end(), p);
            if (elem_end == elem_start && !keep_empty)
            {
                continue;
            }

#if __cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L) || (defined(_MSC_VER) && _MSC_VER >= 1600)
            result.emplace_back(
                s, elem_start - s.begin(), elem_end == s.end() ? string_t::npos : elem_end - elem_start);
#else
            result.push_back(
                s.substr(elem_start - s.begin(), elem_end == s.end() ? string_t::npos : elem_end - elem_start));
#endif
        }
        return result;
    }

    static std::vector<string_t> split(const string_t& s, const typename string_t::value_type delim, const bool keep_empty = true)
    {
        std::vector<string_t> result;
        for (typename string_t::size_type elem_start = 0, elem_end = 0;
            elem_end != string_t::npos && elem_start < s.size();
            elem_start = elem_end + 1)
        {
            elem_end = s.find(delim, elem_start);
            if (elem_end == elem_start && !keep_empty)
            {
                continue;
            }

#if __cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L) || (defined(_MSC_VER) && _MSC_VER >= 1600)
            result.emplace_back(
                s, elem_start, elem_end == string_t::npos ? string_t::npos : elem_end - elem_start);
#else
            result.push_back(
                s.substr(elem_start, elem_end == string_t::npos ? string_t::npos : elem_end - elem_start));
#endif
        }
        return result;
    }

    static std::vector<string_t> split_sstream(const string_t& s, const typename string_t::value_type delim, const bool keep_empty = true)
    {
        std::basic_stringstream<typename string_t::value_type> ss(s);
        string_t item{};
        std::vector<string_t> result;

        while (std::getline(ss, item, delim))
        {
            if (item.empty() && !keep_empty)
            {
                continue;
            }
#if __cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L) || (defined(_MSC_VER) && _MSC_VER >= 1600)
            result.push_back(std::move(item));
#else
            result.push_back(item);
#endif
        }
        return result;
    }

    static string_t& replace_all(string_t& str, string_t const& old_value, string_t const& new_value)
    {
        //assert(!str.empty());
        assert(!old_value.empty());

        if (old_value == new_value/* || new_value == ""*/)
        {
            return str;
        }

        for (typename string_t::size_type pos = 0;
            (pos = str.find(old_value, pos)) != string_t::npos;
            pos += new_value.length())
        {
            str.replace(pos, old_value.length(), new_value);
        }

        return str;
    }

    static int ignore_case_compare(const string_t& left, const string_t& right)
    {
        string_t l = to_upper(left);
        string_t r = to_upper(right);

        return l.compare(r);
    }

    static bool ignore_case_less(const string_t& left, const string_t& right)
    {
        return ignore_case_compare(left, right) < 0;
    }

    struct ignore_case_less_4_key
    {
        bool operator()(const string_t& left, const string_t& right) const
        {
            return std::lexicographical_compare(
                left.begin(), left.end(), right.begin(), right.end(),
                [](typename string_t::value_type const& lc, typename string_t::value_type const& rc)
                    { return ctype_helper<typename string_t::value_type>::toupper(lc) < ctype_helper<typename string_t::value_type>::toupper(rc); });
        }
    };

    struct ignore_case_equal_to_4_key
    {
        bool operator()(const string_t& left, const string_t& right) const
        {
            return std::equal(
                left.begin(), left.end(), right.begin(), right.end(),
                [](typename string_t::value_type const& lc, typename string_t::value_type const& rc)
                    { return ctype_helper<typename string_t::value_type>::toupper(lc) == ctype_helper<typename string_t::value_type>::toupper(rc); });
        }
    };

    struct ignore_case_hash_4_key
    {
        typedef string_t argument_type;
        typedef std::size_t result_type;

        result_type operator()(argument_type const& key) const
        {
            argument_type upper_key = to_upper(key);
            return std::hash<argument_type>()(upper_key);
        }
    };
};

#if 1
//http://www.voidcn.com/article/p-dsvvrbno-brp.html
#if __cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L) || (defined(_MSC_VER) && _MSC_VER >= 1600)
template<
    typename CharT,
    typename Traits = std::char_traits<CharT>,
    typename Allocator = std::allocator<CharT>,
    typename StringType = std::basic_string<CharT, Traits, Allocator>
>
std::vector<StringType> string_split_regex(const StringType& in, const StringType& delim)
{
    std::basic_regex<CharT> re{ delim };
    return std::vector<StringType>{ std::regex_token_iterator<typename StringType::const_iterator>(in.begin(), in.end(), re, -1), {} };
}

#endif
#endif

typedef string_helper<std::string> string_helperA;
typedef string_helper<std::wstring> string_helperW;

#if defined(UNICODE) || defined(_UNICODE)
#define tstring_helper   string_helperW
#else
#define tstring_helper   string_helperA
#endif

#if _WIN32
#pragma warning(default: 4996)
#endif

