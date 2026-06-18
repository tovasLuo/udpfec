#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <ws2ipdef.h>
//#include <in6addr.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
//#include <sys/types.h>
//#include <sys/uio.h>
#include <sys/un.h>
#endif
#include "hash_combiner.h"

#ifdef _WIN32
#if 0
#ifndef ___swap_16___
#define ___swap_16___(x)                    \
  ((uint16_t) ((((x) & 0xff00u) >> 8)       \
   |           (((x) & 0x00ffu) << 8)))
#endif

#ifndef ___swap_32___
#define ___swap_32___(x)                    \
    ((((x) & 0xff000000u) >> 24)            \
   | (((x) & 0x00ff0000u) >> 8)             \
   | (((x) & 0x0000ff00u) << 8)             \
   | (((x) & 0x000000ffu) << 24))
#endif
#endif

#ifndef ___swap_64___
#define ___swap_64___(x)                    \
    ((((x) & 0xff00000000000000ull) >> 56)  \
   | (((x) & 0x00ff000000000000ull) >> 40)  \
   | (((x) & 0x0000ff0000000000ull) >> 24)  \
   | (((x) & 0x000000ff00000000ull) >> 8)   \
   | (((x) & 0x00000000ff000000ull) << 8)   \
   | (((x) & 0x0000000000ff0000ull) << 24)  \
   | (((x) & 0x000000000000ff00ull) << 40)  \
   | (((x) & 0x00000000000000ffull) << 56))
#endif

#ifndef htonll
#define htonll ___swap_64___
#endif

#ifndef ntohll
#define ntohll htonll
#endif

#else
#ifndef htonll
#define htonll htobe64
#endif
#ifndef ntohll
#define ntohll be64toh
#endif
#endif


//使用其它操作符时(!=、>、<=、>=)，先使用using namespace std::rel_ops;

inline bool operator<(const in_addr& l, const in_addr& r)
{
    return ntohl(l.s_addr) < ntohl(r.s_addr);
}

inline bool operator==(const in_addr& l, const in_addr& r)
{
    return l.s_addr == r.s_addr;
}

template<> struct std::hash<in_addr>
{
    size_t operator()(const in_addr& ss) const
    {
        return make_hash(ntohl(ss.s_addr));
    }
};

inline bool operator<(const in6_addr& l, const in6_addr& r)
{
    return memcmp(l.s6_addr, r.s6_addr, sizeof(r.s6_addr)) < 0;
}

inline bool operator==(const in6_addr& l, const in6_addr& r)
{
    return memcmp(l.s6_addr, r.s6_addr, sizeof(r.s6_addr)) == 0;
}

template<> struct std::hash<in6_addr>
{
    size_t operator()(const in6_addr& ss) const
    {
        return make_hash((const uint8_t(&)[16])ss.s6_addr);
    }
};

struct sockaddr_in_only_addr_less
{
    bool operator()(const sockaddr_in& l, const sockaddr_in& r) const
    {
        assert(l.sin_family == r.sin_family && r.sin_family == AF_INET);

#if 0
        if (l.sin_family != r.sin_family)
        {
            return l.sin_family < r.sin_family;
        }
#endif

        return l.sin_addr < r.sin_addr;
    }
};

struct sockaddr_in_only_addr_equal_to
{
    bool operator()(const sockaddr_in& l, const sockaddr_in& r) const
    {
        assert(l.sin_family == r.sin_family && r.sin_family == AF_INET);
        return /*l.sin_family == r.sin_family &&*/
                l.sin_addr == r.sin_addr;
    }
};

struct sockaddr_in_only_addr_hash
{
    size_t operator()(const sockaddr_in& ss) const
    {
        assert(ss.sin_family == AF_INET);
        return make_hash(ss.sin_family, ntohl(ss.sin_addr.s_addr));
    }
};

inline bool operator<(const sockaddr_in& l, const sockaddr_in& r)
{
    if (!sockaddr_in_only_addr_equal_to()(l, r))
    {
        return sockaddr_in_only_addr_less()(l, r);
    }

    return ntohs(l.sin_port) < ntohs(r.sin_port);
}

inline bool operator==(const sockaddr_in& l, const sockaddr_in& r)
{
    return sockaddr_in_only_addr_equal_to()(l, r) &&
           l.sin_port == r.sin_port;
}

template<> struct std::hash<sockaddr_in>
{
    size_t operator()(const sockaddr_in& ss) const
    {
        return make_hash(ss.sin_family, ntohs(ss.sin_port), ntohl(ss.sin_addr.s_addr));
    }
};

struct sockaddr_in6_only_addr_less
{
    bool operator()(const sockaddr_in6& l, const sockaddr_in6& r) const
    {
        assert(l.sin6_family == r.sin6_family && r.sin6_family == AF_INET6);

#if 0
        if (l.sin6_family != r.sin6_family)
        {
            return l.sin6_family < r.sin6_family;
        }
#endif

        return l.sin6_addr < r.sin6_addr;
    }
};

struct sockaddr_in6_only_addr_equal_to
{
    bool operator()(const sockaddr_in6& l, const sockaddr_in6& r) const
    {
        assert(l.sin6_family == r.sin6_family && r.sin6_family == AF_INET6);
        return /*l.sin6_family == r.sin6_family &&*/
                l.sin6_addr == r.sin6_addr;
    }
};

struct sockaddr_in6_only_addr_hash
{
    size_t operator()(const sockaddr_in6& ss) const
    {
        assert(ss.sin6_family == AF_INET6);
        return make_hash(ss.sin6_family, (const uint8_t(&)[16])ss.sin6_addr.s6_addr);
    }
};

inline bool operator<(const sockaddr_in6& l, const sockaddr_in6& r)
{
    if (!sockaddr_in6_only_addr_equal_to()(l, r))
    {
        return sockaddr_in6_only_addr_less()(l, r);
    }

    return ntohs(l.sin6_port) < ntohs(r.sin6_port);
}

inline bool operator==(const sockaddr_in6& l, const sockaddr_in6& r)
{
    return sockaddr_in6_only_addr_equal_to()(l, r) &&
           l.sin6_port == r.sin6_port;
}

template<> struct std::hash<sockaddr_in6>
{
    size_t operator()(const sockaddr_in6& ss) const
    {
        return make_hash(ss.sin6_family, ntohs(ss.sin6_port), (const uint8_t(&)[16])ss.sin6_addr.s6_addr);
    }
};

struct sockaddr_in6_complete_less
{
    bool operator()(const sockaddr_in6& l, const sockaddr_in6& r) const
    {
        if (!(l == r))
        {
            return l < r;
        }

        if (l.sin6_flowinfo != r.sin6_flowinfo)
        {
            return ntohl(l.sin6_flowinfo) < ntohl(r.sin6_flowinfo);
        }

        return l.sin6_scope_id < r.sin6_scope_id;
    }
};

struct sockaddr_in6_complete_equal_to
{
    bool operator()(const sockaddr_in6& l, const sockaddr_in6& r) const
    {
        return l == r &&
               l.sin6_flowinfo == r.sin6_flowinfo &&
               l.sin6_scope_id == r.sin6_scope_id;
    }
};

struct sockaddr_in6_complete_hash
{
    size_t operator()(const sockaddr_in6& ss) const
    {
        return make_hash(ss.sin6_family, ntohs(ss.sin6_port), ntohl(ss.sin6_flowinfo), (const uint8_t(&)[16])ss.sin6_addr.s6_addr, ss.sin6_scope_id);
    }
};

#ifndef _WIN32
inline bool operator<(const sockaddr_un& l, const sockaddr_un& r)
{
    assert(l.sun_family == r.sun_family && r.sun_family == AF_UNIX);

#if 0
    if (l.sun_family != r.sun_family)
    {
        return l.sun_family < r.sun_family;
    }
#endif

    return strcmp(l.sun_path, r.sun_path) < 0;
}

inline bool operator==(const sockaddr_un& l, const sockaddr_un& r)
{
    assert(l.sun_family == r.sun_family && r.sun_family == AF_UNIX);
    return /*l.sun_family == r.sun_family &&*/
            strcmp(l.sun_path, r.sun_path) == 0;
}

template<> struct std::hash<sockaddr_un>
{
    size_t operator()(const sockaddr_un& ss) const
    {
        assert(ss.sun_family == AF_UNIX);
        size_t seed = make_hash(ss.sun_family);
        size_t path_len = strlen(ss.sun_path);
        if (path_len > 0)
        {
            combine_hash_bytes(seed, (const unsigned char*)ss.sun_path, path_len);
        }

        return seed;
    }
};
#endif

struct sockaddr_storage_only_addr_less
{
    bool operator()(const sockaddr_storage& l, const sockaddr_storage& r) const
    {
        if (l.ss_family != r.ss_family)
        {
            return l.ss_family < r.ss_family;
        }

        if (l.ss_family == AF_INET)
        {
            return sockaddr_in_only_addr_less()(*(sockaddr_in*)&l, *(sockaddr_in*)&r);
        }

        if (l.ss_family == AF_INET6)
        {
            return sockaddr_in6_only_addr_less()(*(sockaddr_in6*)&l, *(sockaddr_in6*)&r);
        }

#ifndef _WIN32
        if (l.ss_family == AF_UNIX)
        {
            return *(sockaddr_un*)&l < *(sockaddr_un*)&r;
        }
#endif
        return memcmp((char*)&l + sizeof(l.ss_family), (char*)&r + sizeof(r.ss_family), sizeof(r) - sizeof(r.ss_family)) < 0;
        //return memcmp(&l, &r, sizeof(r)) < 0;
    }
};

struct sockaddr_storage_only_addr_equal_to
{
    bool operator()(const sockaddr_storage& l, const sockaddr_storage& r) const
    {
        if (l.ss_family != r.ss_family)
        {
            return false;
        }

        if (l.ss_family == AF_INET)
        {
            return sockaddr_in_only_addr_equal_to()(*(sockaddr_in*)&l, *(sockaddr_in*)&r);
        }

        if (l.ss_family == AF_INET6)
        {
            return sockaddr_in6_only_addr_equal_to()(*(sockaddr_in6*)&l, *(sockaddr_in6*)&r);
        }

#ifndef _WIN32
        if (l.ss_family == AF_UNIX)
        {
            return *(sockaddr_un*)&l == *(sockaddr_un*)&r;
        }
#endif

        //return memcmp((char*)&l + sizeof(l.ss_family), (char*)&r + sizeof(r.ss_family), sizeof(r) - sizeof(r.ss_family)) == 0;
        return memcmp(&l, &r, sizeof(r)) == 0;
    }
};

struct sockaddr_storage_only_addr_hash
{
    size_t operator()(const sockaddr_storage& ss) const
    {
        if (ss.ss_family == AF_INET)
        {
            return sockaddr_in_only_addr_hash()(*(sockaddr_in*)&ss);
        }

        if (ss.ss_family == AF_INET6)
        {
            return sockaddr_in6_only_addr_hash()(*(sockaddr_in6*)&ss);
        }

#ifndef _WIN32
        if (ss.ss_family == AF_UNIX)
        {
            return std::hash<sockaddr_un>()(*(sockaddr_un*)&ss);
        }
#endif

        size_t seed = make_hash(ss.ss_family);
        combine_hash_bytes(seed, (const unsigned char*)&ss + sizeof(ss.ss_family), sizeof(ss) - sizeof(ss.ss_family));
        return seed;
    }
};

inline bool operator<(const sockaddr_storage& l, const sockaddr_storage& r)
{
    if (l.ss_family != r.ss_family)
    {
        return l.ss_family < r.ss_family;
    }

    if (l.ss_family == AF_INET)
    {
        return *(sockaddr_in*)&l < *(sockaddr_in*)&r;
    }

    if (l.ss_family == AF_INET6)
    {
        return *(sockaddr_in6*)&l < *(sockaddr_in6*)&r;
    }

#ifndef _WIN32
    if (l.ss_family == AF_UNIX)
    {
        return *(sockaddr_un*)&l < *(sockaddr_un*)&r;
    }
#endif

    return memcmp((char*)&l + sizeof(l.ss_family), (char*)&r + sizeof(r.ss_family), sizeof(r) - sizeof(r.ss_family)) < 0;
    //return memcmp(&l, &r, sizeof(r)) < 0;
}

inline bool operator==(const sockaddr_storage& l, const sockaddr_storage& r)
{
    if (l.ss_family != r.ss_family)
    {
        return false;
    }

    if (l.ss_family == AF_INET)
    {
        return *(sockaddr_in*)&l == *(sockaddr_in*)&r;
    }

    if (l.ss_family == AF_INET6)
    {
        return *(sockaddr_in6*)&l == *(sockaddr_in6*)&r;
    }

#ifndef _WIN32
    if (l.ss_family == AF_UNIX)
    {
        return *(sockaddr_un*)&l == *(sockaddr_un*)&r;
    }
#endif

    //return memcmp((char*)&l + sizeof(l.ss_family), (char*)&r + sizeof(r.ss_family), sizeof(r) - sizeof(r.ss_family)) == 0;
    return memcmp(&l, &r, sizeof(r)) == 0;
}

template<> struct std::hash<sockaddr_storage>
{
    size_t operator()(const sockaddr_storage& ss) const
    {
        if (ss.ss_family == AF_INET)
        {
            return std::hash<sockaddr_in>()(*(sockaddr_in*)&ss);
        }

        if (ss.ss_family == AF_INET6)
        {
            return std::hash<sockaddr_in6>()(*(sockaddr_in6*)&ss);
        }

#ifndef _WIN32
        if (ss.ss_family == AF_UNIX)
        {
            return std::hash<sockaddr_un>()(*(sockaddr_un*)&ss);
        }
#endif

        size_t seed = make_hash(ss.ss_family);
        combine_hash_bytes(seed, (const unsigned char*)&ss + sizeof(ss.ss_family), sizeof(ss) - sizeof(ss.ss_family));
        return seed;
    }
};

struct sockaddr_storage_complete_less
{
    bool operator()(const sockaddr_storage& l, const sockaddr_storage& r) const
    {
        if (l.ss_family != r.ss_family)
        {
            return l.ss_family < r.ss_family;
        }

        if (l.ss_family == AF_INET6)
        {
            return sockaddr_in6_complete_less()(*(sockaddr_in6*)&l, *(sockaddr_in6*)&r);
        }

        return l < r;
    }
};

struct sockaddr_storage_complete_equal_to
{
    bool operator()(const sockaddr_storage& l, const sockaddr_storage& r) const
    {
        if (l.ss_family != r.ss_family)
        {
            return false;
        }

        if (l.ss_family == AF_INET6)
        {
            return sockaddr_in6_complete_equal_to()(*(sockaddr_in6*)&l, *(sockaddr_in6*)&r);
        }

        return l == r;
    }
};

struct sockaddr_storage_complete_hash
{
    size_t operator()(const sockaddr_storage& ss) const
    {
        if (ss.ss_family == AF_INET)
        {
            return std::hash<sockaddr_in>()(*(sockaddr_in*)&ss);
        }

        if (ss.ss_family == AF_INET6)
        {
            return sockaddr_in6_complete_hash()(*(sockaddr_in6*)&ss);
        }

#ifndef _WIN32
        if (ss.ss_family == AF_UNIX)
        {
            return std::hash<sockaddr_un>()(*(sockaddr_un*)&ss);
        }
#endif

        size_t seed = make_hash(ss.ss_family);
        combine_hash_bytes(seed, (const unsigned char*)&ss + sizeof(ss.ss_family), sizeof(ss) - sizeof(ss.ss_family));
        return seed;
    }
};

#ifdef _WIN32
#define sockaddr_inet SOCKADDR_INET
#else
union sockaddr_inet
{
    sockaddr_in Ipv4;
    sockaddr_in6 Ipv6;
    sa_family_t si_family;
};
#endif

struct sockaddr_inet_only_addr_less
{
    bool operator()(const sockaddr_inet& l, const sockaddr_inet& r) const
    {
        return sockaddr_storage_only_addr_less()(*(sockaddr_storage*)&l, *(sockaddr_storage*)&r);
    }
};

struct sockaddr_inet_only_addr_equal_to
{
    bool operator()(const sockaddr_inet& l, const sockaddr_inet& r) const
    {
        return sockaddr_storage_only_addr_equal_to()(*(sockaddr_storage*)&l, *(sockaddr_storage*)&r);
    }
};

struct sockaddr_inet_only_addr_hash
{
    size_t operator()(const sockaddr_inet& ss) const
    {
        return sockaddr_storage_only_addr_hash()(*(sockaddr_storage*)&ss);
    }
};

inline bool operator<(const sockaddr_inet& l, const sockaddr_inet& r)
{
    return *(sockaddr_storage*)&l < *(sockaddr_storage*)&r;
}

inline bool operator==(const sockaddr_inet& l, const sockaddr_inet& r)
{
    return *(sockaddr_storage*)&l == *(sockaddr_storage*)&r;
}

template<> struct std::hash<sockaddr_inet>
{
    size_t operator()(const sockaddr_inet& ss) const
    {
        return std::hash<sockaddr_storage>()(*(sockaddr_storage*)&ss);
    }
};

struct sockaddr_inet_complete_less
{
    bool operator()(const sockaddr_inet& l, const sockaddr_inet& r) const
    {
        return sockaddr_storage_complete_less()(*(sockaddr_storage*)&l, *(sockaddr_storage*)&r);
    }
};

struct sockaddr_inet_complete_equal_to
{
    bool operator()(const sockaddr_inet& l, const sockaddr_inet& r) const
    {
        return sockaddr_storage_complete_equal_to()(*(sockaddr_storage*)&l, *(sockaddr_storage*)&r);
    }
};

struct sockaddr_inet_complete_hash
{
    size_t operator()(const sockaddr_inet& ss) const
    {
        return sockaddr_storage_complete_hash()(*(sockaddr_storage*)&ss);
    }
};

//network communication quintuple
//network communication five-tuple
struct five_tuple_ss
{
    sockaddr_storage src_addr;
    sockaddr_storage dst_addr;
    int              protocol; // IPPROTO_TCP, IPPROTO_UDP
};

struct five_tuple_ss_only_addr_less
{
    bool operator()(const five_tuple_ss& l, const five_tuple_ss& r) const
    {
        if (!sockaddr_storage_only_addr_equal_to()(l.src_addr, r.src_addr))
        {
            return sockaddr_storage_only_addr_less()(l.src_addr, r.src_addr);
        }

        if (!sockaddr_storage_only_addr_equal_to()(l.dst_addr, r.dst_addr))
        {
            return sockaddr_storage_only_addr_less()(l.dst_addr, r.dst_addr);
        }

        return l.protocol < r.protocol;
    }
};

struct five_tuple_ss_only_addr_equal_to
{
    bool operator()(const five_tuple_ss& l, const five_tuple_ss& r) const
    {
        return sockaddr_storage_only_addr_equal_to()(l.src_addr, r.src_addr) &&
               sockaddr_storage_only_addr_equal_to()(l.dst_addr, r.dst_addr) &&
               l.protocol == r.protocol;
    }
};

struct five_tuple_ss_only_addr_hash
{
    size_t operator()(const five_tuple_ss& ss) const
    {
        size_t seed(sockaddr_storage_only_addr_hash()(ss.src_addr));

        if (ss.dst_addr.ss_family == AF_INET)
        {
            sockaddr_in* si4 = (sockaddr_in*)&ss.dst_addr;
            combine_hash(seed, si4->sin_family, ntohl(si4->sin_addr.s_addr));
        }

        else if (ss.dst_addr.ss_family == AF_INET6)
        {
            sockaddr_in6* si6 = (sockaddr_in6*)&ss.dst_addr;
            combine_hash(seed, si6->sin6_family, (const uint8_t(&)[16])si6->sin6_addr.s6_addr);
        }

#ifndef _WIN32
        else if (ss.dst_addr.ss_family == AF_UNIX)
        {
            sockaddr_un* su = (sockaddr_un*)&ss.dst_addr;
            combine_hash(seed, su->sun_family);
            size_t path_len = strlen(su->sun_path);
            if (path_len > 0)
            {
                combine_hash_bytes(seed, (const unsigned char*)su->sun_path, path_len);
            }
        }
#endif
        else
        {
            combine_hash(seed, ss.dst_addr.ss_family);
            combine_hash_bytes(seed, (const unsigned char*)&ss.dst_addr + sizeof(ss.dst_addr.ss_family), sizeof(ss.dst_addr) - sizeof(ss.dst_addr.ss_family));
        }

        combine_hash(seed, ss.protocol);
        return seed;
    }
};

inline bool operator<(const five_tuple_ss& l, const five_tuple_ss& r)
{
    if (!(l.src_addr == r.src_addr))
    {
        return l.src_addr < r.src_addr;
    }

    if (!(l.dst_addr == r.dst_addr))
    {
        return l.dst_addr < r.dst_addr;
    }

    return l.protocol < r.protocol;
}

inline bool operator==(const five_tuple_ss& l, const five_tuple_ss& r)
{
    return l.src_addr == r.src_addr &&
           l.dst_addr == r.dst_addr &&
           l.protocol == r.protocol;
}

template<> struct std::hash<five_tuple_ss>
{
    size_t operator()(const five_tuple_ss& ss) const
    {
        size_t seed(std::hash<sockaddr_storage>()(ss.src_addr));

        if (ss.dst_addr.ss_family == AF_INET)
        {
            sockaddr_in* si4 = (sockaddr_in*)&ss.dst_addr;
            combine_hash(seed, si4->sin_family, ntohs(si4->sin_port), ntohl(si4->sin_addr.s_addr));
        }

        else if (ss.dst_addr.ss_family == AF_INET6)
        {
            sockaddr_in6* si6 = (sockaddr_in6*)&ss.dst_addr;
            combine_hash(seed, si6->sin6_family, ntohs(si6->sin6_port), (const uint8_t(&)[16])si6->sin6_addr.s6_addr);
        }

#ifndef _WIN32
        else if (ss.dst_addr.ss_family == AF_UNIX)
        {
            sockaddr_un* su = (sockaddr_un*)&ss.dst_addr;
            combine_hash(seed, su->sun_family);
            size_t path_len = strlen(su->sun_path);
            if (path_len > 0)
            {
                combine_hash_bytes(seed, (const unsigned char*)su->sun_path, path_len);
            }
        }
#endif
        else
        {
            combine_hash(seed, ss.dst_addr.ss_family);
            combine_hash_bytes(seed, (const unsigned char*)&ss.dst_addr + sizeof(ss.dst_addr.ss_family), sizeof(ss.dst_addr) - sizeof(ss.dst_addr.ss_family));
        }

        combine_hash(seed, ss.protocol);
        return seed;
    }
};

struct five_tuple_ss_complete_less
{
    bool operator()(const five_tuple_ss& l, const five_tuple_ss& r) const
    {
        if (!sockaddr_storage_complete_equal_to()(l.src_addr, r.src_addr))
        {
            return sockaddr_storage_complete_less()(l.src_addr, r.src_addr);
        }

        if (!sockaddr_storage_complete_equal_to()(l.dst_addr, r.dst_addr))
        {
            return sockaddr_storage_complete_less()(l.dst_addr, r.dst_addr);
        }

        return l.protocol < r.protocol;
    }
};

struct five_tuple_ss_complete_equal_to
{
    bool operator()(const five_tuple_ss& l, const five_tuple_ss& r) const
    {
        return sockaddr_storage_complete_equal_to()(l.src_addr, r.src_addr) &&
               sockaddr_storage_complete_equal_to()(l.dst_addr, r.dst_addr) &&
               l.protocol == r.protocol;
    }
};

struct five_tuple_ss_complete_hash
{
    size_t operator()(const five_tuple_ss& ss) const
    {
        size_t seed(sockaddr_storage_complete_hash()(ss.src_addr));

        if (ss.dst_addr.ss_family == AF_INET)
        {
            sockaddr_in* si4 = (sockaddr_in*)&ss.dst_addr;
            combine_hash(seed, si4->sin_family, ntohs(si4->sin_port), ntohl(si4->sin_addr.s_addr));
        }

        else if (ss.dst_addr.ss_family == AF_INET6)
        {
            sockaddr_in6* si6= (sockaddr_in6*)&ss.dst_addr;
            combine_hash(seed, si6->sin6_family, ntohs(si6->sin6_port), ntohl(si6->sin6_flowinfo), (const uint8_t(&)[16])si6->sin6_addr.s6_addr, si6->sin6_scope_id);
        }

#ifndef _WIN32
        else if (ss.dst_addr.ss_family == AF_UNIX)
        {
            sockaddr_un* su = (sockaddr_un*)&ss.dst_addr;
            combine_hash(seed, ntohs(su->sun_family));
            size_t path_len = strlen(su->sun_path);
            if (path_len > 0)
            {
                combine_hash_bytes(seed, (const unsigned char*)su->sun_path, path_len);
            }
        }
#endif
        else
        {
            combine_hash(seed, ss.dst_addr.ss_family);
            combine_hash_bytes(seed, (const unsigned char*)&ss.dst_addr + sizeof(ss.dst_addr.ss_family), sizeof(ss.dst_addr) - sizeof(ss.dst_addr.ss_family));
        }

        combine_hash(seed, ss.protocol);
        return seed;
    }
};

struct five_tuple_inet
{
    sockaddr_inet src_addr;
    sockaddr_inet dst_addr;
    int           protocol;   // IPPROTO_TCP, IPPROTO_UDP
};

struct five_tuple_inet_only_addr_less
{
    bool operator()(const five_tuple_inet& l, const five_tuple_inet& r) const
    {
        if (!sockaddr_inet_only_addr_equal_to()(l.src_addr, r.src_addr))
        {
            return sockaddr_inet_only_addr_less()(l.src_addr, r.src_addr);
        }

        if (!sockaddr_inet_only_addr_equal_to()(l.dst_addr, r.dst_addr))
        {
            return sockaddr_inet_only_addr_less()(l.dst_addr, r.dst_addr);
        }

        return l.protocol < r.protocol;
    }
};

struct five_tuple_inet_only_addr_equal_to
{
    bool operator()(const five_tuple_inet& l, const five_tuple_inet& r) const
    {
        return sockaddr_inet_only_addr_equal_to()(l.src_addr, r.src_addr) &&
               sockaddr_inet_only_addr_equal_to()(l.dst_addr, r.dst_addr) &&
               l.protocol == r.protocol;
    }
};

struct five_tuple_inet_only_addr_hash
{
    size_t operator()(const five_tuple_inet& ss) const
    {
        size_t seed(sockaddr_inet_only_addr_hash()(ss.src_addr));

        if (ss.dst_addr.si_family == AF_INET)
        {
            sockaddr_in* si4 = (sockaddr_in*)&ss.dst_addr;
            combine_hash(seed, si4->sin_family, ntohl(si4->sin_addr.s_addr));
        }

        else if (ss.dst_addr.si_family == AF_INET6)
        {
            sockaddr_in6* si6 = (sockaddr_in6*)&ss.dst_addr;
            combine_hash(seed, si6->sin6_family, (const uint8_t(&)[16])si6->sin6_addr.s6_addr);
        }

#ifndef _WIN32
        else if (ss.dst_addr.si_family == AF_UNIX)
        {
            sockaddr_un* su = (sockaddr_un*)&ss.dst_addr;
            combine_hash(seed, su->sun_family);
            size_t path_len = strlen(su->sun_path);
            if (path_len > 0)
            {
                combine_hash_bytes(seed, (const unsigned char*)su->sun_path, path_len);
            }
        }
#endif
        else
        {
            combine_hash(seed, ss.dst_addr.si_family);
            combine_hash_bytes(seed, (const unsigned char*)&ss.dst_addr + sizeof(ss.dst_addr.si_family), sizeof(ss.dst_addr) - sizeof(ss.dst_addr.si_family));
        }

        combine_hash(seed, ss.protocol);
        return seed;
    }
};

inline bool operator<(const five_tuple_inet& l, const five_tuple_inet& r)
{
    if (!(l.src_addr == r.src_addr))
    {
        return l.src_addr < r.src_addr;
    }

    if (!(l.dst_addr == r.dst_addr))
    {
        return l.dst_addr < r.dst_addr;
    }

    return l.protocol < r.protocol;
}

inline bool operator==(const five_tuple_inet& l, const five_tuple_inet& r)
{
    return l.src_addr == r.src_addr &&
           l.dst_addr == r.dst_addr &&
           l.protocol == r.protocol;
}

template<> struct std::hash<five_tuple_inet>
{
    size_t operator()(const five_tuple_inet& ss) const
    {
        size_t seed(std::hash<sockaddr_inet>()(ss.src_addr));

        if (ss.dst_addr.si_family == AF_INET)
        {
            sockaddr_in* si4 = (sockaddr_in*)&ss.dst_addr;
            combine_hash(seed, si4->sin_family, ntohs(si4->sin_port), ntohl(si4->sin_addr.s_addr));
        }

        else if (ss.dst_addr.si_family == AF_INET6)
        {
            sockaddr_in6* si6 = (sockaddr_in6*)&ss.dst_addr;
            combine_hash(seed, si6->sin6_family, ntohs(si6->sin6_port), (const uint8_t(&)[16])si6->sin6_addr.s6_addr);
        }

#ifndef _WIN32
        else if (ss.dst_addr.si_family == AF_UNIX)
        {
            sockaddr_un* su = (sockaddr_un*)&ss.dst_addr;
            combine_hash(seed, su->sun_family);
            size_t path_len = strlen(su->sun_path);
            if (path_len > 0)
            {
                combine_hash_bytes(seed, (const unsigned char*)su->sun_path, path_len);
            }
        }
#endif
        else
        {
            combine_hash(seed, ss.dst_addr.si_family);
            combine_hash_bytes(seed, (const unsigned char*)&ss.dst_addr + sizeof(ss.dst_addr.si_family), sizeof(ss.dst_addr) - sizeof(ss.dst_addr.si_family));
        }

        combine_hash(seed, ss.protocol);
        return seed;
    }
};

struct five_tuple_inet_complete_less
{
    bool operator()(const five_tuple_inet& l, const five_tuple_inet& r) const
    {
        if (!sockaddr_inet_complete_equal_to()(l.src_addr, r.src_addr))
        {
            return sockaddr_inet_complete_less()(l.src_addr, r.src_addr);
        }

        if (!sockaddr_inet_complete_equal_to()(l.dst_addr, r.dst_addr))
        {
            return sockaddr_inet_complete_less()(l.dst_addr, r.dst_addr);
        }

        return l.protocol < r.protocol;
    }
};

struct five_tuple_inet_complete_equal_to
{
    bool operator()(const five_tuple_inet& l, const five_tuple_inet& r) const
    {
        return sockaddr_inet_complete_equal_to()(l.src_addr, r.src_addr) &&
               sockaddr_inet_complete_equal_to()(l.dst_addr, r.dst_addr) &&
               l.protocol == r.protocol;
    }
};

struct five_tuple_inet_complete_hash
{
    size_t operator()(const five_tuple_inet& ss) const
    {
        size_t seed(sockaddr_inet_complete_hash()(ss.src_addr));

        if (ss.dst_addr.si_family == AF_INET)
        {
            sockaddr_in* si4 = (sockaddr_in*)&ss.dst_addr;
            combine_hash(seed, si4->sin_family, ntohs(si4->sin_port), ntohl(si4->sin_addr.s_addr));
        }

        else if (ss.dst_addr.si_family == AF_INET6)
        {
            sockaddr_in6* si6 = (sockaddr_in6*)&ss.dst_addr;
            combine_hash(seed, si6->sin6_family, ntohs(si6->sin6_port), ntohl(si6->sin6_flowinfo), (const uint8_t(&)[16])si6->sin6_addr.s6_addr, si6->sin6_scope_id);
        }

#ifndef _WIN32
        else if (ss.dst_addr.si_family == AF_UNIX)
        {
            sockaddr_un* su = (sockaddr_un*)&ss.dst_addr;
            combine_hash(seed, su->sun_family);
            size_t path_len = strlen(su->sun_path);
            if (path_len > 0)
            {
                combine_hash_bytes(seed, (const unsigned char*)su->sun_path, path_len);
            }
        }
#endif
        else
        {
            combine_hash(seed, ss.dst_addr.si_family);
            combine_hash_bytes(seed, (const unsigned char*)&ss.dst_addr + sizeof(ss.dst_addr.si_family), sizeof(ss.dst_addr) - sizeof(ss.dst_addr.si_family));
        }

        combine_hash(seed, ss.protocol);
        return seed;
    }
};

//////////////////////////////////////////////////////////////////////////////////////////////////
template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_in, key_t>::value, key_t>::type>
using sockaddr_in_only_addr_map
= std::map<key_t, value_t,
    sockaddr_in_only_addr_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_in, key_t>::value, key_t>::type>
using sockaddr_in_only_addr_multimap
= std::multimap<key_t, value_t,
    sockaddr_in_only_addr_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_in, key_t>::value, key_t>::type>
using sockaddr_in_only_addr_set
= std::set<key_t,
    sockaddr_in_only_addr_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_in, key_t>::value, key_t>::type>
using sockaddr_in_only_addr_multiset
= std::multiset<key_t,
    sockaddr_in_only_addr_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_in, key_t>::value, key_t>::type>
using sockaddr_in_only_addr_unordered_map
= std::unordered_map<key_t, value_t,
    sockaddr_in_only_addr_hash,
    sockaddr_in_only_addr_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_in, key_t>::value, key_t>::type>
using sockaddr_in_only_addr_unordered_multimap
= std::unordered_multimap<key_t, value_t,
    sockaddr_in_only_addr_hash,
    sockaddr_in_only_addr_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_in, key_t>::value, key_t>::type>
using sockaddr_in_only_addr_unordered_set
= std::unordered_set<key_t,
    sockaddr_in_only_addr_hash,
    sockaddr_in_only_addr_equal_to,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_in, key_t>::value, key_t>::type>
using sockaddr_in_only_addr_unordered_multiset
= std::unordered_multiset<key_t,
    sockaddr_in_only_addr_hash,
    sockaddr_in_only_addr_equal_to,
    std::allocator<key_t>>;

//////////////////////////////////////////////////////////////////////////////////////////////////
template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_only_addr_map
= std::map<key_t, value_t,
    sockaddr_in6_only_addr_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_only_addr_multimap
= std::multimap<key_t, value_t,
    sockaddr_in6_only_addr_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_only_addr_set
= std::set<key_t,
    sockaddr_in6_only_addr_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_only_addr_multiset
= std::multiset<key_t,
    sockaddr_in6_only_addr_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_only_addr_unordered_map
= std::unordered_map<key_t, value_t,
    sockaddr_in6_only_addr_hash,
    sockaddr_in6_only_addr_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_only_addr_unordered_multimap
= std::unordered_multimap<key_t, value_t,
    sockaddr_in6_only_addr_hash,
    sockaddr_in6_only_addr_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_only_addr_unordered_set
= std::unordered_set<key_t,
    sockaddr_in6_only_addr_hash,
    sockaddr_in6_only_addr_equal_to,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_only_addr_unordered_multiset
= std::unordered_multiset<key_t,
    sockaddr_in6_only_addr_hash,
    sockaddr_in6_only_addr_equal_to,
    std::allocator<key_t>>;

//////////////////////////////////////////////////////////////////////////////////////////////////
template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_complete_map
= std::map<key_t, value_t,
    sockaddr_in6_complete_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_complete_multimap
= std::multimap<key_t, value_t,
    sockaddr_in6_complete_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_complete_set
= std::set<key_t,
    sockaddr_in6_complete_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_complete_multiset
= std::multiset<key_t,
    sockaddr_in6_complete_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_complete_unordered_map
= std::unordered_map<key_t, value_t,
    sockaddr_in6_complete_hash,
    sockaddr_in6_complete_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_complete_unordered_multimap
= std::unordered_multimap<key_t, value_t,
    sockaddr_in6_complete_hash,
    sockaddr_in6_complete_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_complete_unordered_set
= std::unordered_set<key_t,
    sockaddr_in6_complete_hash,
    sockaddr_in6_complete_equal_to,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_in6, key_t>::value, key_t>::type>
using sockaddr_in6_complete_unordered_multiset
= std::unordered_multiset<key_t,
    sockaddr_in6_complete_hash,
    sockaddr_in6_complete_equal_to,
    std::allocator<key_t>>;

//////////////////////////////////////////////////////////////////////////////////////////////////
template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using only_addr_map
= std::map<key_t, value_t,
    sockaddr_storage_only_addr_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using only_addr_multimap
= std::multimap<key_t, value_t,
    sockaddr_storage_only_addr_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using only_addr_set
= std::set<key_t,
    sockaddr_storage_only_addr_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using only_addr_multiset
= std::multiset<key_t,
    sockaddr_storage_only_addr_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using only_addr_unordered_map
= std::unordered_map<key_t, value_t,
    sockaddr_storage_only_addr_hash,
    sockaddr_storage_only_addr_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using only_addr_unordered_multimap
= std::unordered_multimap<key_t, value_t,
    sockaddr_storage_only_addr_hash,
    sockaddr_storage_only_addr_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using only_addr_unordered_set
= std::unordered_set<key_t,
    sockaddr_storage_only_addr_hash,
    sockaddr_storage_only_addr_equal_to,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using only_addr_unordered_multiset
= std::unordered_multiset<key_t,
    sockaddr_storage_only_addr_hash,
    sockaddr_storage_only_addr_equal_to,
    std::allocator<key_t>>;

//////////////////////////////////////////////////////////////////////////////////////////////////
template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using complete_sstorage_map
= std::map<key_t, value_t,
    sockaddr_storage_complete_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using complete_sstorage_multimap
= std::multimap<key_t, value_t,
    sockaddr_storage_complete_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using complete_sstorage_set
= std::set<key_t,
    sockaddr_storage_complete_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using complete_sstorage_multiset
= std::multiset<key_t,
    sockaddr_storage_complete_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using complete_sstorage_unordered_map
= std::unordered_map<key_t, value_t,
    sockaddr_storage_complete_hash,
    sockaddr_storage_complete_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using complete_sstorage_unordered_multimap
= std::unordered_multimap<key_t, value_t,
    sockaddr_storage_complete_hash,
    sockaddr_storage_complete_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using complete_sstorage_unordered_set
= std::unordered_set<key_t,
    sockaddr_storage_complete_hash,
    sockaddr_storage_complete_equal_to,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_storage, key_t>::value, key_t>::type>
using complete_sstorage_unordered_multiset
= std::unordered_multiset<key_t,
    sockaddr_storage_complete_hash,
    sockaddr_storage_complete_equal_to,
    std::allocator<key_t>>;

//////////////////////////////////////////////////////////////////////////////////////////////////
template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_only_addr_map
= std::map<key_t, value_t,
    sockaddr_inet_only_addr_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_only_addr_multimap
= std::multimap<key_t, value_t,
    sockaddr_inet_only_addr_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_only_addr_set
= std::set<sockaddr_inet,
    sockaddr_inet_only_addr_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_only_addr_multiset
= std::multiset<key_t,
    sockaddr_inet_only_addr_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_only_addr_unordered_map
= std::unordered_map<key_t, value_t,
    sockaddr_inet_only_addr_hash,
    sockaddr_inet_only_addr_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_only_addr_unordered_multimap
= std::unordered_multimap<key_t, value_t,
    sockaddr_inet_only_addr_hash,
    sockaddr_inet_only_addr_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_only_addr_unordered_set
= std::unordered_set<key_t,
    sockaddr_inet_only_addr_hash,
    sockaddr_inet_only_addr_equal_to,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_only_addr_unordered_multiset
= std::unordered_multiset<key_t,
    sockaddr_inet_only_addr_hash,
    sockaddr_inet_only_addr_equal_to,
    std::allocator<key_t>>;

//////////////////////////////////////////////////////////////////////////////////////////////////
template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_complete_map
= std::map<key_t, value_t,
    sockaddr_inet_complete_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_complete_multimap
= std::multimap<sockaddr_inet, value_t,
    sockaddr_inet_complete_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_complete_set
= std::set<key_t,
    sockaddr_inet_complete_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_complete_multiset
= std::multiset<key_t,
    sockaddr_inet_complete_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_complete_unordered_map
= std::unordered_map<key_t, value_t,
    sockaddr_inet_complete_hash,
    sockaddr_inet_complete_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_complete_unordered_multimap
= std::unordered_multimap<key_t, value_t,
    sockaddr_inet_complete_hash,
    sockaddr_inet_complete_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_complete_unordered_set
= std::unordered_set<key_t,
    sockaddr_inet_complete_hash,
    sockaddr_inet_complete_equal_to,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr_inet, key_t>::value, key_t>::type>
using sockaddr_inet_complete_unordered_multiset
= std::unordered_multiset<key_t,
    sockaddr_inet_complete_hash,
    sockaddr_inet_complete_equal_to,
    std::allocator<key_t>>;

//////////////////////////////////////////////////////////////////////////////////////////////////
template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_only_addr_map
= std::map<key_t, value_t,
    five_tuple_ss_only_addr_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_only_addr_multimap
= std::multimap<key_t, value_t,
    five_tuple_ss_only_addr_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_only_addr_set
= std::set<key_t,
    five_tuple_ss_only_addr_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_only_addr_multiset
= std::multiset<key_t,
    five_tuple_ss_only_addr_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_only_addr_unordered_map
= std::unordered_map<key_t, value_t,
    five_tuple_ss_only_addr_hash,
    five_tuple_ss_only_addr_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_only_addr_unordered_multimap
= std::unordered_multimap<key_t, value_t,
    five_tuple_ss_only_addr_hash,
    five_tuple_ss_only_addr_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_only_addr_unordered_set
= std::unordered_set<key_t,
    five_tuple_ss_only_addr_hash,
    five_tuple_ss_only_addr_equal_to,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_only_addr_unordered_multiset
= std::unordered_multiset<key_t,
    five_tuple_ss_only_addr_hash,
    five_tuple_ss_only_addr_equal_to,
    std::allocator<key_t>>;

//////////////////////////////////////////////////////////////////////////////////////////////////
template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_complete_map
= std::map<key_t, value_t,
    five_tuple_ss_complete_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_complete_multimap
= std::multimap<key_t, value_t,
    five_tuple_ss_complete_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_complete_set
= std::set<key_t,
    five_tuple_ss_complete_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_complete_multiset
= std::multiset<key_t,
    five_tuple_ss_complete_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_complete_unordered_map
= std::unordered_map<key_t, value_t,
    five_tuple_ss_complete_hash,
    five_tuple_ss_complete_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_complete_unordered_multimap
= std::unordered_multimap<key_t, value_t,
    five_tuple_ss_complete_hash,
    five_tuple_ss_complete_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_complete_unordered_set
= std::unordered_set<key_t,
    five_tuple_ss_complete_hash,
    five_tuple_ss_complete_equal_to,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_ss, key_t>::value, key_t>::type>
using five_tuple_ss_complete_unordered_multiset
= std::unordered_multiset<key_t,
    five_tuple_ss_complete_hash,
    five_tuple_ss_complete_equal_to,
    std::allocator<key_t>>;

//////////////////////////////////////////////////////////////////////////////////////////////////
template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_only_addr_map
= std::map<key_t, value_t,
    five_tuple_inet_only_addr_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_only_addr_multimap
= std::multimap<key_t, value_t,
    five_tuple_inet_only_addr_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_only_addr_set
= std::set<key_t,
    five_tuple_inet_only_addr_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_only_addr_multiset
= std::multiset<key_t,
    five_tuple_inet_only_addr_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_only_addr_unordered_map
= std::unordered_map<key_t, value_t,
    five_tuple_inet_only_addr_hash,
    five_tuple_inet_only_addr_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_only_addr_unordered_multimap
= std::unordered_multimap<key_t, value_t,
    five_tuple_inet_only_addr_hash,
    five_tuple_inet_only_addr_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_only_addr_unordered_set
= std::unordered_set<key_t,
    five_tuple_inet_only_addr_hash,
    five_tuple_inet_only_addr_equal_to,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_only_addr_unordered_multiset
= std::unordered_multiset<key_t,
    five_tuple_inet_only_addr_hash,
    five_tuple_inet_only_addr_equal_to,
    std::allocator<key_t>>;

//////////////////////////////////////////////////////////////////////////////////////////////////
template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_complete_map
= std::map<key_t, value_t,
    five_tuple_inet_complete_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_complete_multimap
= std::multimap<key_t, value_t,
    five_tuple_inet_complete_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_complete_set
= std::set<key_t,
    five_tuple_inet_complete_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_complete_multiset
= std::multiset<key_t,
    five_tuple_inet_complete_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_complete_unordered_map
= std::unordered_map<key_t, value_t,
    five_tuple_inet_complete_hash,
    five_tuple_inet_complete_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_complete_unordered_multimap
= std::unordered_multimap<key_t, value_t,
    five_tuple_inet_complete_hash,
    five_tuple_inet_complete_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_complete_unordered_set
= std::unordered_set<key_t,
    five_tuple_inet_complete_hash,
    five_tuple_inet_complete_equal_to,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<five_tuple_inet, key_t>::value, key_t>::type>
using five_tuple_inet_complete_unordered_multiset
= std::unordered_multiset<key_t,
    five_tuple_inet_complete_hash,
    five_tuple_inet_complete_equal_to,
    std::allocator<key_t>>;

//////////////////////////////////////////////////////////////////////////////////////////////////
#if 0
struct sockaddr_only_addr_less
{
    bool operator()(const sockaddr& l, const sockaddr& r) const
    {
        return sockaddr_storage_only_addr_less()(*(sockaddr_storage*)&l, *(sockaddr_storage*)&r);
    }
};

struct sockaddr_only_addr_equal_to
{
    bool operator()(const sockaddr& l, const sockaddr& r) const
    {
        return sockaddr_storage_only_addr_equal_to()(*(sockaddr_storage*)&l, *(sockaddr_storage*)&r);
    }
};

struct sockaddr_only_addr_hash
{
    size_t operator()(const sockaddr& ss) const
    {
        return sockaddr_storage_only_addr_hash()(*(sockaddr_storage*)&ss);
    }
};

inline bool operator<(const sockaddr& l, const sockaddr& r)
{
    return *(sockaddr_storage*)&l < *(sockaddr_storage*)&r;
}

inline bool operator==(const sockaddr& l, const sockaddr& r)
{
    return *(sockaddr_storage*)&l == *(sockaddr_storage*)&r;
}

template<> struct std::hash<sockaddr>
{
    size_t operator()(const sockaddr& ss) const
    {
        return std::hash<sockaddr_storage>()(*(sockaddr_storage*)&ss);
    }
};

struct sockaddr_complete_less
{
    bool operator()(const sockaddr& l, const sockaddr& r) const
    {
        return sockaddr_storage_complete_less()(*(sockaddr_storage*)&l, *(sockaddr_storage*)&r);
    }
};

struct sockaddr_complete_equal_to
{
    bool operator()(const sockaddr& l, const sockaddr& r) const
    {
        return sockaddr_storage_complete_equal_to()(*(sockaddr_storage*)&l, *(sockaddr_storage*)&r);
    }
};

struct sockaddr_complete_hash
{
    size_t operator()(const sockaddr& ss) const
    {
        return sockaddr_storage_complete_hash()(*(sockaddr_storage*)&ss);
    }
};

#if 1
template<> struct std::allocator<sockaddr>
{
public:
    using value_type = sockaddr;

    sockaddr* allocate(std::size_t n)
    {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(sockaddr_in6))
        {
            throw std::bad_alloc();
        }
        void* p = std::malloc(n * sizeof(sockaddr_in6));
        if (!p)
        {
            throw std::bad_alloc();
        }
        return static_cast<sockaddr*>(p);
    }

    void deallocate(sockaddr* p, std::size_t) noexcept
    {
        std::free(p);
    }

    template <typename U, typename... Args>
    void construct(U* p, Args&&... args)
    {
        ::new((void*)p) U(std::forward<Args>(args)...);
    }

    template <typename U>
    void destroy(U* p)
    {
        p->~U();
    }
};
#endif

//////////////////////////////////////////////////////////////////////////////////////////////////
template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_only_addr_map
= std::map<key_t, value_t,
    sockaddr_only_addr_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_only_addr_multimap
= std::multimap<key_t, value_t,
    sockaddr_only_addr_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_only_addr_set
= std::set<key_t,
    sockaddr_only_addr_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_only_addr_multiset
= std::multiset<key_t,
    sockaddr_only_addr_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_only_addr_unordered_map
= std::unordered_map<key_t, value_t,
    sockaddr_only_addr_hash,
    sockaddr_only_addr_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_only_addr_unordered_multimap
= std::unordered_multimap<key_t, value_t,
    sockaddr_only_addr_hash,
    sockaddr_only_addr_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_only_addr_unordered_set
= std::unordered_set<key_t,
    sockaddr_only_addr_hash,
    sockaddr_only_addr_equal_to,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_only_addr_unordered_multiset
= std::unordered_multiset<key_t,
    sockaddr_only_addr_hash,
    sockaddr_only_addr_equal_to,
    std::allocator<key_t>>;

//////////////////////////////////////////////////////////////////////////////////////////////////
template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_complete_map
= std::map<key_t, value_t,
    sockaddr_complete_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_complete_multimap
= std::multimap<key_t, value_t,
    sockaddr_complete_less,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_complete_set
= std::set<key_t,
    sockaddr_complete_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_complete_multiset
= std::multiset<key_t,
    sockaddr_complete_less,
    std::allocator<key_t>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_complete_unordered_map
= std::unordered_map<key_t, value_t,
    sockaddr_complete_hash,
    sockaddr_complete_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class value_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_complete_unordered_multimap
= std::unordered_multimap<key_t, value_t,
    sockaddr_complete_hash,
    sockaddr_complete_equal_to,
    std::allocator<std::pair<const key_t, value_t>>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_complete_unordered_set
= std::unordered_set<key_t,
    sockaddr_complete_hash,
    sockaddr_complete_equal_to,
    std::allocator<key_t>>;

template<
    class key_t,
    class = typename std::enable_if<std::is_same<sockaddr, key_t>::value, key_t>::type>
using sockaddr_complete_unordered_multiset
= std::unordered_multiset<key_t,
    sockaddr_complete_hash,
    sockaddr_complete_equal_to,
    std::allocator<key_t>>;

#endif
