#pragma once
#include <cctype>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <cwchar>
#include <type_traits>

#if _WIN32
#pragma warning(disable:4996)
#endif
//#ifdef _MSC_VER
//#define _CRT_SECURE_NO_WARNINGS
//#endif


template<typename char_t, class = typename std::enable_if<std::is_same<char, char_t>::value || std::is_same<wchar_t, char_t>::value, char_t>::type>
struct ctype_helper;

template<>
struct ctype_helper<char>
{
    typedef unsigned char sargs_t; //arguments safely type
    const static sargs_t eof = (sargs_t)EOF;
    const static sargs_t value_min = 0x00;
    const static sargs_t value_max = UCHAR_MAX;

    static char nul()
    {
        return '\0';
    }

    static constexpr char pound()
    {
        return '#';
    }

    static constexpr char semicolon()
    {
        return ';';
    }

    static char open_square_bracket()
    {
        return '[';
    }

    static char close_square_bracket()
    {
        return ']';
    }

    static char equals_sign()
    {
        return '=';
    }

    static char space()
    {
        return ' ';
    }

    static char form_feed()
    {
        return '\f';
    }

    static char line_feed()
    {
        return '\n';
    }

    static char carriage_return()
    {
        return '\r';
    }

    static char horizontal_tab()
    {
        return '\t';
    }

    static char vertical_tab()
    {
        return '\v';
    }

    static char underline()
    {
        return '_';
    }

    static const char* spaces()
    {
        return " \f\n\r\t\v\0";
    }

    static const char* blanks()
    {
        return " \t";
    }

    static int isalnum(char ch)
    {
        return std::isalnum(static_cast<unsigned char>(ch));
    }

    static int isalpha(char ch)
    {
        return std::isalpha(static_cast<unsigned char>(ch));
    }

    static int islower(char ch)
    {
        return std::islower(static_cast<unsigned char>(ch));
    }

    static int isupper(char ch)
    {
        return std::isupper(static_cast<unsigned char>(ch));
    }

    static int isdigit(char ch)
    {
        return std::isdigit(static_cast<unsigned char>(ch));
    }

    static int isxdigit(char ch)
    {
        return std::isxdigit(static_cast<unsigned char>(ch));
    }

    static int iscntrl(char ch)
    {
        return std::iscntrl(static_cast<unsigned char>(ch));
    }

    static int isgraph(char ch)
    {
        return std::isgraph(static_cast<unsigned char>(ch));
    }

    static int isspace(char ch)
    {
        return std::isspace(static_cast<unsigned char>(ch));
    }

    static int isblank(char ch)
    {
        return std::isblank(static_cast<unsigned char>(ch));
    }

    static int isprint(char ch)
    {
        return std::isprint(static_cast<unsigned char>(ch));
    }

    static int ispunct(char ch)
    {
        return std::ispunct(static_cast<unsigned char>(ch));
    }

    static int tolower(char ch)
    {
        return std::tolower(static_cast<unsigned char>(ch));
    }

    static int toupper(char ch)
    {
        return std::toupper(static_cast<unsigned char>(ch));
    }

    static long strtol(const char* str, char** str_end, int base = 0)
    {
        return std::strtol(str, str_end, base);
    }

    static long long strtoll(const char* str, char** str_end, int base = 0)
    {
        return std::strtoll(str, str_end, base);
    }

    static unsigned long strtoul(const char* str, char** str_end, int base = 0)
    {
        return std::strtoul(str, str_end, base);
    }

    static unsigned long long strtoull(const char* str, char** str_end, int base = 0)
    {
        return std::strtoull(str, str_end, base);
    }

    static float strtof(const char* str, char** str_end)
    {
        return std::strtof(str, str_end);
    }

    static double strtod(const char* str, char** str_end)
    {
        return std::strtod(str, str_end);
    }

    static long double strtold(const char* str, char** str_end)
    {
        return std::strtold(str, str_end);
    }

    static std::intmax_t strtoimax(const char* nptr, char** endptr, int base = 0)
    {
        return std::strtoimax(nptr, endptr, base);
    }

    static std::uintmax_t strtoumax(const char* nptr, char** endptr, int base = 0)
    {
        return std::strtoumax(nptr, endptr, base);
    }

    static char* strcpy(char* dest, const char* src)
    {
        return std::strcpy(dest, src);
    }

    static char* strncpy(char* dest, const char* src, std::size_t count)
    {
        return std::strncpy(dest, src, count);
    }

    static char* strcat(char* dest, const char* src)
    {
        return std::strcat(dest, src);
    }

    static char* strncat(char* dest, const char* src, std::size_t count)
    {
        return std::strncat(dest, src, count);
    }

#ifdef _WIN32
    static errno_t strcpy_s(char* dest, size_t dest_size, const char* src)
    {
        return ::strcpy_s(dest, dest_size, src);
    }

    static errno_t strncpy_s(char* dest, size_t dest_size, const char* src, std::size_t count)
    {
        return ::strncpy_s(dest, dest_size, src, count);
    }

    static errno_t strcat_s(char* dest, size_t dest_size, const char* src)
    {
        return ::strcat_s(dest, dest_size, src);
    }

    static errno_t strncat_s(char* dest, size_t dest_size, const char* src, std::size_t count)
    {
        return ::strncat_s(dest, dest_size, src, count);
    }
#endif

    static std::size_t strxfrm(char* dest, const char* src, std::size_t count)
    {
        return std::strxfrm(dest, src, count);
    }

    static std::size_t strlen(const char* str)
    {
        return std::strlen(str);
    }

    static int strcmp(const char* lhs, const char* rhs)
    {
        return std::strcmp(lhs, rhs);
    }

    static int strncmp(const char* lhs, const char* rhs, size_t count)
    {
        return std::strncmp(lhs, rhs, count);
    }

    static int strcoll(const char* lhs, const char* rhs)
    {
        return std::strcoll(lhs, rhs);
    }

    static const char* strchr(const char* str, char ch)
    {
        return std::strchr(str, static_cast<unsigned char>(ch));
    }

    static char* strchr(char* str, char ch)
    {
        return std::strchr(str, static_cast<unsigned char>(ch));
    }

    static const char* strrchr(const char* str, char ch)
    {
        return std::strchr(str, static_cast<unsigned char>(ch));
    }

    static char* strrchr(char* str, char ch)
    {
        return std::strchr(str, static_cast<unsigned char>(ch));
    }

    static size_t strspn(const char* dest, const char* src)
    {
        return std::strspn(dest, src);
    }

    static size_t strcspn(const char* dest, const char* src)
    {
        return std::strcspn(dest, src);
    }

    static const char* strpbrk(const char* dest, const char* breakset)
    {
        return std::strpbrk(dest, breakset);
    }

    static char* strpbrk(char* dest, const char* breakset)
    {
        return std::strpbrk(dest, breakset);
    }

    static const char* strstr(const char* str, const char* target)
    {
        return std::strstr(str, target);
    }

    static char* strstr(char* str, const char* target)
    {
        return std::strstr(str, target);
    }

    static char* strtok(char* str, const char* delim, char** ptr)
    {
#if defined(_WIN32)
        return strtok_s(str, delim, ptr);
#elif defined(__linux__) || defined(__unix__ ) || defined(_POSIX_VERSION)
        return strtok_r(str, delim, ptr);
#else
        return std::strtok(str, delim);
#endif
    }

    static const char* memchr(const char* ptr, char ch, std::size_t count)
    {
        return static_cast<const char*>(std::memchr(ptr, static_cast<unsigned char>(ch), count));
    }

    static char* memchr(char* ptr, char ch, std::size_t count)
    {
        return static_cast<char*>(std::memchr(ptr, static_cast<unsigned char>(ch), count));
    }

    static int memcmp(const char* lhs, const char* rhs, std::size_t count)
    {
        return std::memcmp(lhs, rhs, count);
    }

    static char* memcpy(char* dest, const char* src, std::size_t count)
    {
        return static_cast<char*>(std::memcpy(dest, src, count));
    }

    static char* memmove(char* dest, const char* src, std::size_t count)
    {
        return static_cast<char*>(std::memmove(dest, src, count));
    }

    static char* memset(char* dest, char ch, std::size_t count)
    {
        return static_cast<char*>(std::memset(dest, static_cast<unsigned char>(ch), count));
    }
};

template<>
struct ctype_helper<wchar_t>
{
    typedef std::wint_t sargs_t; //arguments safely type
    const static sargs_t eof = WEOF;
    const static sargs_t value_min = (sargs_t)WCHAR_MIN;
    const static sargs_t value_max = (sargs_t)WCHAR_MAX;

    static wchar_t nul()
    {
        return L'\0';
    }

    static constexpr wchar_t pound()
    {
        return L'#';
    }

    static constexpr wchar_t semicolon()
    {
        return L';';
    }

    static wchar_t open_square_bracket()
    {
        return L'[';
    }

    static wchar_t close_square_bracket()
    {
        return L']';
    }

    static wchar_t equals_sign()
    {
        return L'=';
    }

    static wchar_t space()
    {
        return L' ';
    }

    static wchar_t form_feed()
    {
        return L'\f';
    }

    static wchar_t line_feed()
    {
        return L'\n';
    }

    static wchar_t carriage_return()
    {
        return L'\r';
    }

    static wchar_t horizontal_tab()
    {
        return L'\t';
    }

    static wchar_t vertical_tab()
    {
        return L'\v';
    }

    static wchar_t underline()
    {
        return L'_';
    }

    static const wchar_t* spaces()
    {
        return L" \f\n\r\t\v\0";
    }

    static const wchar_t* blanks()
    {
        return L" \t";
    }

    static int isalnum(wchar_t ch)
    {
        return std::iswalnum(static_cast<std::wint_t>(ch));
    }

    static int isalpha(wchar_t ch)
    {
        return std::iswalpha(static_cast<std::wint_t>(ch));
    }

    static int islower(wchar_t ch)
    {
        return std::iswlower(static_cast<std::wint_t>(ch));
    }

    static int isupper(wchar_t ch)
    {
        return std::iswupper(static_cast<std::wint_t>(ch));
    }

    static int isdigit(wchar_t ch)
    {
        return std::iswdigit(static_cast<std::wint_t>(ch));
    }

    static int isxdigit(wchar_t ch)
    {
        return std::iswxdigit(static_cast<std::wint_t>(ch));
    }

    static int iscntrl(wchar_t ch)
    {
        return std::iswcntrl(static_cast<std::wint_t>(ch));
    }

    static int isgraph(wchar_t ch)
    {
        return std::iswgraph(static_cast<std::wint_t>(ch));
    }

    static int isspace(wchar_t ch)
    {
        return std::iswspace(static_cast<std::wint_t>(ch));
    }

    static int isblank(wchar_t ch)
    {
        return std::iswblank(static_cast<std::wint_t>(ch));
    }

    static int isprint(wchar_t ch)
    {
        return std::iswprint(static_cast<std::wint_t>(ch));
    }

    static int ispunct(wchar_t ch)
    {
        return std::iswpunct(static_cast<std::wint_t>(ch));
    }

    static std::wint_t tolower(wchar_t ch)
    {
        return std::towlower(static_cast<std::wint_t>(ch));
    }

    static std::wint_t toupper(wchar_t ch)
    {
        return std::towupper(static_cast<std::wint_t>(ch));
    }

    static long strtol(const wchar_t* str, wchar_t** str_end, int base = 0)
    {
        return std::wcstol(str, str_end, base);
    }

    static long long strtoll(const wchar_t* str, wchar_t** str_end, int base = 0)
    {
        return std::wcstoll(str, str_end, base);
    }

    static unsigned long strtoul(const wchar_t* str, wchar_t** str_end, int base = 0)
    {
        return std::wcstoul(str, str_end, base);
    }

    static unsigned long long strtoull(const wchar_t* str, wchar_t** str_end, int base = 0)
    {
        return std::wcstoull(str, str_end, base);
    }

    static float strtof(const wchar_t* str, wchar_t** str_end)
    {
        return std::wcstof(str, str_end);
    }

    static double strtod(const wchar_t* str, wchar_t** str_end)
    {
        return std::wcstod(str, str_end);
    }

    static long double strtold(const wchar_t* str, wchar_t** str_end)
    {
        return std::wcstold(str, str_end);
    }

    static std::intmax_t strtoimax(const wchar_t* nptr, wchar_t** endptr, int base = 0)
    {
        return std::wcstoimax(nptr, endptr, base);
    }

    static std::uintmax_t strtoumax(const wchar_t* nptr, wchar_t** endptr, int base = 0)
    {
        return std::wcstoumax(nptr, endptr, base);
    }

    static wchar_t* strcpy(wchar_t* dest, const wchar_t* src)
    {
        return std::wcscpy(dest, src);
    }

    static wchar_t* strncpy(wchar_t* dest, const wchar_t* src, std::size_t count)
    {
        return std::wcsncpy(dest, src, count);
    }

    static wchar_t* strcat(wchar_t* dest, const wchar_t* src)
    {
        return std::wcscat(dest, src);
    }

    static wchar_t* strncat(wchar_t* dest, const wchar_t* src, std::size_t count)
    {
        return std::wcsncat(dest, src, count);
    }

#ifdef _WIN32
    static errno_t strcpy_s(wchar_t* dest, size_t dest_size, const wchar_t* src)
    {
        return wcscpy_s(dest, dest_size, src);
    }

    static errno_t strncpy_s(wchar_t* dest, size_t dest_size, const wchar_t* src, std::size_t count)
    {
        return wcsncpy_s(dest, dest_size, src, count);
    }

    static errno_t strcat_s(wchar_t* dest, size_t dest_size, const wchar_t* src)
    {
        return wcscat_s(dest, dest_size, src);
    }

    static errno_t strncat_s(wchar_t* dest, size_t dest_size, const wchar_t* src, std::size_t count)
    {
        return wcsncat_s(dest, dest_size, src, count);
    }
#endif

    static std::size_t strxfrm(wchar_t* dest, const wchar_t* src, std::size_t count)
    {
        return std::wcsxfrm(dest, src, count);
    }

    static std::size_t strlen(const wchar_t* str)
    {
        return std::wcslen(str);
    }

    static int strcmp(const wchar_t* lhs, const wchar_t* rhs)
    {
        return std::wcscmp(lhs, rhs);
    }

    static int strncmp(const wchar_t* lhs, const wchar_t* rhs, std::size_t count)
    {
        return std::wcsncmp(lhs, rhs, count);
    }

    static int strcoll(const wchar_t* lhs, const wchar_t* rhs)
    {
        return std::wcscoll(lhs, rhs);
    }

    static const wchar_t* strchr(const wchar_t* str, wchar_t ch)
    {
        return std::wcschr(str, ch);
    }

    static wchar_t* strchr(wchar_t* str, wchar_t ch)
    {
        return std::wcschr(str, ch);
    }

    static const wchar_t* strrchr(const wchar_t* str, wchar_t ch)
    {
        return std::wcsrchr(str, ch);
    }

    static wchar_t* strrchr(wchar_t* str, wchar_t ch)
    {
        return std::wcsrchr(str, ch);
    }

    static size_t strspn(const wchar_t* dest, const wchar_t* src)
    {
        return std::wcsspn(dest, src);
    }

    static size_t strcspn(const wchar_t* dest, const wchar_t* src)
    {
        return std::wcscspn(dest, src);
    }

    static const wchar_t* strpbrk(const wchar_t* dest, const wchar_t* breakset)
    {
        return std::wcspbrk(dest, breakset);
    }

    static wchar_t* strpbrk(wchar_t* dest, const wchar_t* breakset)
    {
        return std::wcspbrk(dest, breakset);
    }

    static const wchar_t* strstr(const wchar_t* str, const wchar_t* target)
    {
        return std::wcsstr(str, target);
    }

    static wchar_t* strstr(wchar_t* str, const wchar_t* target)
    {
        return std::wcsstr(str, target);
    }

    static wchar_t* strtok(wchar_t* str, const wchar_t* delim, wchar_t** ptr)
    {
#if defined(_WIN32)
        return wcstok_s(str, delim, ptr);
#else
        return std::wcstok(str, delim, ptr);
#endif
    }

    static const wchar_t* memchr(const wchar_t* ptr, wchar_t ch, std::size_t count)
    {
        return static_cast<const wchar_t*>(std::wmemchr(ptr, ch, count));
    }

    static wchar_t* memchr(wchar_t* ptr, wchar_t ch, std::size_t count)
    {
        return static_cast<wchar_t*>(std::wmemchr(ptr, ch, count));
    }

    static int memcmp(const wchar_t* lhs, const wchar_t* rhs, std::size_t count)
    {
        return std::wmemcmp(lhs, rhs, count);
    }

    static wchar_t* memcpy(wchar_t* dest, const wchar_t* src, std::size_t count)
    {
        return std::wmemcpy(dest, src, count);
    }

    static wchar_t* memmove(wchar_t* dest, const wchar_t* src, std::size_t count)
    {
        return std::wmemmove(dest, src, count);
    }

    static wchar_t* memset(wchar_t* dest, wchar_t ch, std::size_t count)
    {
        return std::wmemset(dest, ch, count);
    }
};

#if _WIN32
#pragma warning(default: 4996)
#endif

typedef ctype_helper<char> char_helper;
typedef ctype_helper<wchar_t> wchar_helper;

#if defined(UNICODE) || defined(_UNICODE)
#define tctype_helper   wchar_helper
#else
#define tctype_helper   char_helper
#endif
