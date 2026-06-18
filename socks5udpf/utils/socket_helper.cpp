#include "socket_helper.h"
#include <algorithm>
#include <bitset>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <ws2def.h>
#include <ws2ipdef.h>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <iphlpapi.h>
#include <netioapi.h> //iphlpapi.lib
#else
#include <fcntl.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#ifdef __linux__
#include <linux/if_packet.h>                     // SOL_PACKET
#include <linux/netfilter_ipv4.h>                // SO_ORIGINAL_DST
#if 0
#include <linux/netfilter_ipv6/ip6_tables.h>     // IP6T_SO_ORIGINAL_DST
#else
#ifndef IP6T_SO_ORIGINAL_DST
/* obtain original address if REDIRECT'd connection */
#define IP6T_SO_ORIGINAL_DST            80
#endif
#endif
#endif
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#endif

#if _WIN32
//#pragma comment(lib, "iphlpapi.lib")
#endif

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4100)
#pragma warning(disable:4189)
#pragma warning(disable:4996)
#endif

#ifdef __linux__
#include "sys_log.h"
#else
#define plog(pri, fmt, ...)
#endif

#ifndef SOL_TCP
#define SOL_TCP     IPPROTO_TCP
#endif

namespace socket_helper
{
int get_errno()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

char* get_errinfo()
{
#ifdef _WIN32
    __declspec(thread) static char err_buf[256];
    (void)FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK, NULL, get_errno(), 0, err_buf, sizeof(err_buf), NULL);
    return err_buf;
#else
    thread_local static char err_buf[256];
    strerror_r(get_errno(), err_buf, sizeof(err_buf));
    return err_buf;
    //return strerror(get_errno());
#endif
}

bool get_socket_error(SOCKET fd)
{
    int err_no = 0;
#ifdef SO_ERROR
    socklen_t err_no_size = sizeof(err_no);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err_no, &err_no_size) == SOCKET_ERROR)
    {
        err_no = get_errno();
        plog(LOG_ERR, "[%d]getsockopt SOL_SOCKET SO_ERROR failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return true;
    }

#else
    err_no = get_errno();
#endif

    if (err_no != 0)
    {
        plog(LOG_ERR, "[%d]failed[%d:%s]\n", fd, err_no, strerror(err_no));
        return true;
    }

    return false;
}

int get_socket_error_no(SOCKET fd)
{
    int err_no = 0;
#ifdef SO_ERROR
    socklen_t err_no_size = sizeof(err_no);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err_no, &err_no_size) == SOCKET_ERROR)
    {
        err_no = get_errno();
        plog(LOG_ERR, "[%d]getsockopt SOL_SOCKET SO_ERROR failed[%d:%s]\n", fd, get_errno(), get_errinfo());
    }

#else
    err_no = get_errno();
#endif

    return err_no;
}

bool set_cloexec(SOCKET fd)
{
#ifdef _WIN32
    //   SetHandleInformation(hHandle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT); // 允许句柄继承（取消FD_CLOEXEC）
    if (!SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0))                // 禁止句柄继承（等效FD_CLOEXEC）
    {
        plog(LOG_ERR, "[%d]SetHandleInformation HANDLE_FLAG_INHERIT 0 failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#else
    int flags = fcntl(fd, F_GETFD);
    if (flags == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]fcntl F_GETFD failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }

    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]fcntl F_SETFD FD_CLOEXEC failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_non_block(SOCKET fd)
{
#if defined(_WIN32)// && defined(FIONBIO)
    unsigned long enable = 1;
    if (ioctlsocket(fd, FIONBIO, &enable) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]ioctlsocket FIONBIO set failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }

#else
    int flags = fcntl(fd, F_GETFL);
    if (flags == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]fcntl F_GETFL failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]fcntl F_SETFL O_NONBLOCK failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool clr_non_block(SOCKET fd)
{
#if defined(_WIN32)// && defined(FIONBIO)
    unsigned long enable = 0;
    if (ioctlsocket(fd, FIONBIO, &enable) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]ioctlsocket FIONBIO clr failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }

#else
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1)
    {
        plog(LOG_ERR, "[%d]fcntl F_GETFL failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }

    if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1)
    {
        plog(LOG_ERR, "[%d]fcntl F_SETFL ~O_NONBLOCK failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_exclusive_addr_use(SOCKET fd)
{
#if defined(_WIN32)// && defined(SO_EXCLUSIVEADDRUSE)
    BOOL enable = TRUE;
    if (setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_EXCLUSIVEADDRUSE failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_reuse_addr(SOCKET fd)
{
#ifdef SO_REUSEADDR
    int enable = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_REUSEADDR failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_reuse_port(SOCKET fd)
{
    int enable = 1;
#if defined(SO_REUSEPORT)
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_REUSEPORT failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#elif defined(SO_REUSEPORT_LB)
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT_LB, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_REUSEPORT_LB failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_no_check(SOCKET fd)
{
#if defined(SO_NO_CHECK)
#ifdef _WIN32
    BOOL enable = TRUE;
#else
    int enable = 1;
#endif
    if (setsockopt(fd, SOL_SOCKET, SO_NO_CHECK, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_NO_CHECK failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_conditional_accept(SOCKET fd)
{
#if defined(_WIN32)// && defined(SO_CONDITIONAL_ACCEPT)
    BOOL enable = TRUE;
    if (setsockopt(fd, SOL_SOCKET, SO_CONDITIONAL_ACCEPT, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_CONDITIONAL_ACCEPT failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

uint32_t get_snd_timeout(SOCKET fd)
{
#if defined _WIN32
    uint32_t timeout = 0;
#else
    timeval timeout{};
#endif
    socklen_t timeout_len = sizeof(timeout);

    if (getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, &timeout_len) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]getsockopt SOL_SOCKET SO_SNDTIMEO failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return (uint32_t)-1;
    }

#if defined _WIN32
    return timeout;
#else
    return (uint32_t)(timeout.tv_sec * 1000 + timeout.tv_usec / 1000);
#endif
}

bool set_snd_timeout(SOCKET fd, uint32_t ms)
{
#if defined _WIN32
    uint32_t timeout = ms;
#else
    timeval timeout{};
    timeout.tv_sec = ms / 1000;
    timeout.tv_usec = (ms % 1000) * 1000;
#endif

    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_SNDTIMEO failed[%d:%s][%u]\n", fd, get_errno(), get_errinfo(), ms);
        return false;
    }

    return true;
}

uint32_t get_rcv_timeout(SOCKET fd)
{
#if defined _WIN32
    uint32_t timeout = 0;
#else
    timeval timeout{};
#endif
    socklen_t timeout_len = sizeof(timeout);

    if (getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, &timeout_len) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]getsockopt SOL_SOCKET SO_RCVTIMEO failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return (uint32_t)-1;
    }

#if defined _WIN32
    return timeout;
#else
    return (uint32_t)(timeout.tv_sec * 1000 + timeout.tv_usec / 1000);
#endif
}

bool set_rcv_timeout(SOCKET fd, uint32_t ms)
{
#if defined _WIN32
    uint32_t timeout = ms;
#else
    timeval timeout{};
    timeout.tv_sec = ms / 1000;
    timeout.tv_usec = (ms % 1000) * 1000;
#endif

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_RCVTIMEO failed[%d:%s][%u]\n", fd, get_errno(), get_errinfo(), ms);
        return false;
    }

    return true;
}

int get_snd_buf_size(SOCKET fd)
{
    int buf_size = 0;
    socklen_t buf_size_len = sizeof(buf_size);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char*)&buf_size, &buf_size_len) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]getsockopt SOL_SOCKET SO_SNDBUF failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return -1;
    }

    return buf_size;
}

bool set_no_snd_buf(SOCKET fd)
{
    return set_snd_buf_size(fd, 0);
}

bool set_snd_buf_size(SOCKET fd, int buf_size)
{
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char*)&buf_size, sizeof(buf_size)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_SNDBUF failed[%d:%s][%d]\n", fd, get_errno(), get_errinfo(), buf_size);
        return false;
    }

    return true;
}

int get_rcv_buf_size(SOCKET fd)
{
    int buf_size = 0;
    socklen_t buf_size_len = sizeof(buf_size);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&buf_size, &buf_size_len) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]getsockopt SOL_SOCKET SO_RCVBUF failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return -1;
    }

    return buf_size;
}

bool set_no_rcv_buf(SOCKET fd)
{
    return set_rcv_buf_size(fd, 0);
}

bool set_rcv_buf_size(SOCKET fd, int buf_size)
{
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&buf_size, sizeof(buf_size)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_RCVBUF failed[%d:%s][%d]\n", fd, get_errno(), get_errinfo(), buf_size);
        return false;
    }

    return true;
}

int get_snd_buf_force_size(SOCKET fd)
{
    int buf_size = 0;
#ifdef SO_SNDBUFFORCE
    socklen_t buf_size_len = sizeof(buf_size);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, (char*)&buf_size, &buf_size_len) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]getsockopt SOL_SOCKET SO_SNDBUFFORCE failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return -1;
    }
#endif

    return buf_size;
}

int get_rcv_buf_force_size(SOCKET fd)
{
    int buf_size = 0;
#ifdef SO_RCVBUFFORCE
    socklen_t buf_size_len = sizeof(buf_size);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, (char*)&buf_size, &buf_size_len) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]getsockopt SOL_SOCKET SO_RCVBUFFORCE failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return -1;
    }
#endif

    return buf_size;
}

bool set_no_rcv_buf_force(SOCKET fd)
{
    return set_rcv_buf_force_size(fd, 0);
}

bool set_rcv_buf_force_size(SOCKET fd, int buf_size)
{
#ifdef SO_RCVBUFFORCE
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, (char*)&buf_size, sizeof(buf_size)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_RCVBUFFORCE failed[%d:%s][%d]\n", fd, get_errno(), get_errinfo(), buf_size);
        return false;
    }
#endif

    return true;
}

bool set_no_snd_buf_force(SOCKET fd)
{
    return set_snd_buf_force_size(fd, 0);
}

bool set_snd_buf_force_size(SOCKET fd, int buf_size)
{
#ifdef SO_SNDBUFFORCE
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, (char*)&buf_size, sizeof(buf_size)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_SNDBUFFORCE failed[%d:%s][%d]\n", fd, get_errno(), get_errinfo(), buf_size);
        return false;
    }
#endif

    return true;
}

bool set_snd_lowat(SOCKET fd, int lowat_size/* = 1*/)
{
#ifdef SO_SNDLOWAT
    if (setsockopt(fd, SOL_SOCKET, SO_SNDLOWAT, (char*)&lowat_size, sizeof(lowat_size)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_SNDLOWAT failed[%d:%s][%d]\n", fd, get_errno(), get_errinfo(), lowat_size);
        return false;
    }
#endif

    return true;
}

bool set_rcv_lowat(SOCKET fd, int lowat_size/* = 1*/)
{
#ifdef SO_RCVLOWAT
    if (setsockopt(fd, SOL_SOCKET, SO_RCVLOWAT, (char*)&lowat_size, sizeof(lowat_size)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_RCVLOWAT failed[%d:%s][%d]\n", fd, get_errno(), get_errinfo(), lowat_size);
        return false;
    }
#endif

    return true;
}

bool set_msg_no_signal(SOCKET fd)
{
    return set_no_sigpipe(fd);
}

bool set_no_sigpipe(SOCKET fd)
{
#ifdef SO_NOSIGPIPE
    int enable = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_NOSIGPIPE failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
/*
#elif defined(MSG_NOSIGNAL)
    int enable = 1;
    if (setsockopt(fd, SOL_SOCKET, MSG_NOSIGNAL, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET MSG_NOSIGNAL failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
*/
#endif

    return true;
}

bool set_tcp_quick_ack(SOCKET fd)
{
#ifdef TCP_QUICKACK
    int enable = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_TCP TCP_QUICKACK failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_tcp_no_delay(SOCKET fd)
{
#ifdef TCP_NODELAY
    int enable = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_TCP TCP_NODELAY failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_tcp_no_push(SOCKET fd)
{
    return set_tcp_cork(fd);
}

bool set_tcp_cork(SOCKET fd)
{
    int enable = 1;
#ifdef TCP_CORK
    if (setsockopt(fd, IPPROTO_TCP, TCP_CORK, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_TCP TCP_CORK failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }

#elif defined(TCP_NOPUSH)
    if (setsockopt(fd, IPPROTO_TCP, TCP_NOPUSH, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_TCP TCP_NOPUSH failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_udp_cork(SOCKET fd)
{
#ifdef UDP_CORK
    int enable = 1;
    if (setsockopt(fd, IPPROTO_UDP, UDP_CORK, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_UDP UDP_CORK failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

int get_mss(SOCKET fd)
{
#ifdef TCP_MAXSEG
    int mss = -1;
    socklen_t len = sizeof(mss);
    if (getsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, (char*)&mss, &len) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]getsockopt IPPROTO_TCP TCP_MAXSEG failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return -1;
    }
    return mss;
#else
    return -1;
#endif
}

int get_mtu(SOCKET fd)
{
#ifdef _WIN32
    DWORD mtu = 0;
    DWORD bytes_returned = 0;

    if (WSAIoctl(fd, SIO_ROUTING_INTERFACE_QUERY, NULL, 0, &mtu, sizeof(mtu), &bytes_returned, NULL, NULL) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]get mtu WSAIoctl SIO_ROUTING_INTERFACE_QUERY failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return -1;
    }

    return (int)mtu;
#else
#ifdef SIOCGIFMTU
    ifreq ifr{};
    socklen_t ifr_len = (socklen_t)sizeof(ifr);
    if (getsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifr.ifr_name, &ifr_len) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]get mtu getsockopt SOL_SOCKET SO_BINDTODEVICE failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return -1;
    }

    int err = ioctl(fd, SIOCGIFMTU, &ifr);
    if (err == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]get mtu failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return -1;
    }

    return ifr.ifr_mtu;

#elif defined(SO_DOMAIN)
    sa_family_t family = 0xffff;
    socklen_t len = sizeof(family);

    // 确定套接字地址族
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &family, &len) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]get mtu getsockopt SOL_SOCKET SO_DOMAIN failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return -1;
    }

    if (family == AF_INET6)
    {
#ifdef IPV6_PATHMTU
        ip6_mtuinfo mtuinfo = {};
        len = sizeof(mtuinfo);

        //IPV6_MTU
        if (getsockopt(fd, IPPROTO_IPV6, IPV6_PATHMTU, &mtuinfo, &len) == SOCKET_ERROR)
        {
            int mtu_disc = IPV6_PMTUDISC_DO;
            if (setsockopt(fd, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &mtu_disc, sizeof(mtu_disc)) == SOCKET_ERROR)
            {
                plog(LOG_ERR, "[%d]get mtu setsockopt IPPROTO_IPV6 IPV6_MTU_DISCOVER failed[%d:%s][IPV6_PMTUDISC_DO(%d)]\n", fd, get_errno(), get_errinfo(), mtu_disc);
                return -1;
            }

            if (getsockopt(fd, IPPROTO_IPV6, IPV6_PATHMTU, &mtuinfo, &len) == SOCKET_ERROR)
            {
                plog(LOG_ERR, "[%d]get mtu retry getsockopt IPPROTO_IPV6 IPV6_PATHMTU failed[%d:%s]\n", fd, get_errno(), get_errinfo());
                return -1;
            }
        }

        return (int)mtuinfo.ip6m_mtu;
#else
        return 1500;
#endif
    }

    assert(family == AF_INET);
#ifdef IP_MTU
    // Linux支持直接获取路径MTU
    unsigned long mtu = 0;
    len = sizeof(mtu);

    if (getsockopt(fd, IPPROTO_IP, IP_MTU, &mtu, &len) == SOCKET_ERROR)
    {
        if (errno != ENOPROTOOPT)
        {
            plog(LOG_ERR, "[%d]get mtu getsockopt IPPROTO_IP IP_MTU failed [%d:%s]\n", fd, get_errno(), get_errinfo());
            return -1;
        }

        int mtu_disc = IP_PMTUDISC_DO;
        if (setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &mtu_disc, sizeof(mtu_disc)) == SOCKET_ERROR)
        {
            plog(LOG_ERR, "[%d]get mtu setsockopt IPPROTO_IP IP_MTU_DISCOVER failed[%d:%s][IP_PMTUDISC_DO(%d)]\n", fd, get_errno(), get_errinfo(), mtu_disc);
            return -1;
        }

        if (getsockopt(fd, IPPROTO_IP, IP_MTU, &mtu, &len) == -1)
        {
            plog(LOG_ERR, "[%d]get mtu retry getsockopt IPPROTO_IP IP_MTU failed[%d:%s]\n", fd, get_errno(), get_errinfo());
            return -1;
        }
    }

    return (int)mtu;
#else
    // 其他系统（如BSD/macOS）可能不直接支持获取路径MTU
    // 这里提供一个估算方法，使用默认MTU减去头部开销
    plog(LOG_WARN, "IP_MTU not supported, using default MTU estimation\n");
    //MAX_IPOPTLEN
    return 1500;
#endif

#else
    return 1500;
#endif
#endif
}

int get_mtu(const std::string& if_name)
{
#ifdef _WIN32
    MIB_IF_ROW2 ifRow = {};
    ifRow.InterfaceLuid = NET_LUID{};
    if (ConvertInterfaceNameToLuidA(if_name.c_str(), &ifRow.InterfaceLuid) != NO_ERROR)
    {
#if 0
        plog(LOG_WARNING, "get MTU 4 ConvertInterfaceNameToLuidA failed[%d:%s][%s]\n", get_errno(), get_errinfo(), if_name.c_str());
        PMIB_IF_TABLE2 pIfTable = {};
        if (GetIfTable2(&pIfTable) != NO_ERROR)
        {
            plog(LOG_ERR, "get MTU 4 GetIfTable2 failed[%d:%s][%s]\n", get_errno(), get_errinfo(), if_name.c_str());
            return -1;
        }

        const std::wstring if_name_w = character_convert::string_wstring(if_name);
        bool found = false;
        for (ULONG i = 0; i < pIfTable->NumEntries; ++i)
        {
            if (std::wstring(pIfTable->Table[i].Alias) == if_name_w)
            {
                ifRow = pIfTable->Table[i];
                found = true;
                break;
            }
        }

        FreeMibTable(pIfTable);
        if (!found)
        {
            plog(LOG_ERR, "get MTU failed: interface not found [%s]\n", if_name.c_str());
            return -1;
        }
#else
        plog(LOG_ERR, "get MTU 4 ConvertInterfaceNameToLuidA failed[%d:%s][%s]\n", get_errno(), get_errinfo(), if_name.c_str());
        return -1;
#endif
    }
    else
    {
        //ifRow.InterfaceIndex = if_nametoindex(if_name.c_str());
        if (GetIfEntry2(&ifRow) != NO_ERROR)
        {
            plog(LOG_ERR, "get MTU failed[%d:%s][%s]\n", get_errno(), get_errinfo(), if_name.c_str());
            return -1;
        }
    }

    return ifRow.Mtu;
#else
    SOCKET fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET)
    {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
    }
    if (fd == INVALID_SOCKET)
    {
        plog(LOG_ERR, "get MTU failed 4 socket failed[%d:%s][%s]\n", get_errno(), get_errinfo(), if_name.c_str());
        return -1;
    }

    int fd_mtu = -1;
    if (set_bind_to_device(fd, if_name))
    {
        fd_mtu = get_mtu(fd);
    }
    close_socket(fd);
    return fd_mtu;
#endif
}

bool set_socket_linger(SOCKET fd, int linger_s/* = 0*/)
{
#ifdef SO_LINGER
    linger linger_data;
    memset(&linger_data, 0, sizeof(linger_data));
    linger_data.l_onoff = 1;
    linger_data.l_linger = (u_short)linger_s;

    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, (char*)&linger_data, sizeof(linger_data)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_LINGER failed[%d:%s][1,%d]\n", fd, get_errno(), get_errinfo(), linger_s);
        return false;
    }
#endif

    return true;
}

bool set_socket_dont_linger(SOCKET fd)
{
#if defined(SO_DONTLINGER)
    int enable = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_DONTLINGER, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_DONTLINGER failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }

#elif defined(SO_LINGER)
    linger linger_data;
    memset(&linger_data, 0, sizeof(linger_data));
    linger_data.l_onoff = 0;
    linger_data.l_linger = 0;

    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, (char*)&linger_data, sizeof(linger_data)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_LINGER failed[%d:%s][0,0]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_socket_oob_inline(SOCKET fd)
{
#ifdef SO_OOBINLINE
    int enable = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_OOBINLINE, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_OOBINLINE failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_socket_keep_alive(SOCKET fd)
{
#ifdef SO_KEEPALIVE
    int enable = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_KEEPALIVE failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_tcp_keep_alive(SOCKET fd, int idle_s, int interval_s, int count)
{
    if (!set_socket_keep_alive(fd))
    {
        return false;
    }

#ifdef TCP_KEEPIDLE
#ifdef _WIN32
    DWORD idle = (DWORD)idle_s * 1000;
#else
    int idle = idle_s;
#endif
    if (idle != 0 && setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, (char*)&idle, sizeof(idle)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_TCP TCP_KEEPIDLE failed[%d:%s][%ds]\n", fd, get_errno(), get_errinfo(), idle);
        return false;
    }
#endif

#ifdef TCP_KEEPINTVL
#ifdef _WIN32
    DWORD interval = (DWORD)interval_s * 1000;
#else
    int interval = interval_s;
#endif
    if (interval_s != 0 && setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, (char*)&interval, sizeof(interval)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_TCP TCP_KEEPINTVL failed[%d:%s][%ds]\n", fd, get_errno(), get_errinfo(), interval);
        return false;
    }
#endif

#ifdef TCP_KEEPCNT
    if (count != 0 && setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, (char*)&count, sizeof(count)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_TCP TCP_KEEPCNT failed[%d:%s][%d]\n", fd, get_errno(), get_errinfo(), count);
        return false;
    }
#endif

    return true;
}

bool set_tcp_user_timeout(SOCKET fd, uint32_t ms/* = 0*/)
{
#ifdef TCP_USER_TIMEOUT
    if (setsockopt(fd, SOL_TCP, TCP_USER_TIMEOUT, (char*)&ms, sizeof(ms)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_TCP TCP_USER_TIMEOUT failed[%d:%s][%u]\n", fd, get_errno(), get_errinfo(), ms);
        return false;
    }
#endif

    return true;
}

bool set_socket_dont_route(SOCKET fd)
{
#ifdef SO_DONTROUTE
    int enable = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_DONTROUTE, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_DONTROUTE failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_ip_free_bind(SOCKET fd, sa_family_t fa /*= AF_INET*/)
{
    if (fa == AF_INET)
    {
        return set_ip_free_bind_v4(fd);
    }

    assert(fa == AF_INET6);
    return set_ip_free_bind_v6(fd);
}

bool set_ip_free_bind_v4(SOCKET fd)
{
#if defined(IP_FREEBIND)
    int enable = 1;
    if (setsockopt(fd, IPPROTO_IP, IP_FREEBIND, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IP IP_FREEBIND failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_ip_free_bind_v6(SOCKET fd)
{
#if defined(IPV6_FREEBIND)
    int enable = 1;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_FREEBIND, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IPV6 IPV6_FREEBIND failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_ip_bind_any(SOCKET fd, sa_family_t fa /*= AF_INET*/)
{
    if (!set_socket_bind_any(fd))
    {
        return false;
    }

    if (fa == AF_INET)
    {
        return set_ip_bind_any_v4(fd);
    }

    assert(fa == AF_INET6);
    return set_ip_bind_any_v6(fd);
}

bool set_socket_bind_any(SOCKET fd)
{
#ifdef SO_BINDANY
    int enable = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BINDANY, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_BINDANY failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_ip_bind_any_v4(SOCKET fd)
{
    int enable = 1;
#if defined(IP_TRANSPARENT)
    if (setsockopt(fd, IPPROTO_IP, IP_TRANSPARENT, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IP IP_TRANSPARENT failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }

#elif defined(IP_BINDANY)
    if (setsockopt(fd, IPPROTO_IP, IP_BINDANY, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IP IP_BINDANY failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_ip_bind_any_v6(SOCKET fd)
{
    int enable = 1;
#if defined(IPV6_TRANSPARENT)
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_TRANSPARENT, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IPV6 IPV6_TRANSPARENT failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#elif defined(IPV6_BINDANY)
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_BINDANY, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IPV6 IPV6_BINDANY failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_ip_transparent(SOCKET fd, sa_family_t fa/* = AF_INET*/)
{
    return set_ip_bind_any(fd, fa);
}

bool set_ip_transparent_v4(SOCKET fd)
{
    return set_ip_bind_any_v4(fd);
}

bool set_ip_transparent_v6(SOCKET fd)
{
    return set_ip_bind_any_v6(fd);
}

bool set_ipv6_v6only(SOCKET fd)
{
#ifdef IPV6_V6ONLY
    int enable = 1;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IPV6 IPV6_V6ONLY 1 failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool clr_ipv6_v6only(SOCKET fd)
{
#ifdef IPV6_V6ONLY
    int enable = 0;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IPV6 IPV6_V6ONLY 0 failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_mark(SOCKET fd, int mark)
{
#ifdef SO_MARK
    if (setsockopt(fd, SOL_SOCKET, SO_MARK, (char*)&mark, sizeof(mark)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_MARK failed[%d:%s][%d]\n", fd, get_errno(), get_errinfo(), mark);
        return false;
    }
#endif

    return true;
}

bool set_icmp_ttl(SOCKET fd, int ttl)
{
    if (setsockopt(fd, IPPROTO_ICMP, IP_TTL, (char*)&ttl, sizeof(ttl)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_ICMP IP_TTL failed[%d:%s][%d]\n", fd, get_errno(), get_errinfo(), ttl);
        return false;
    }

    return true;
}

bool set_sio_udp_connreset(SOCKET fd)
{
#if defined(_WIN32) && defined(SIO_UDP_CONNRESET)
#if 1
    DWORD bytes_returned = 0;
    BOOL enable = FALSE;
    if (WSAIoctl(fd, SIO_UDP_CONNRESET, &enable, sizeof(enable), NULL, 0, &bytes_returned, NULL, NULL) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]WSAIoctl SIO_UDP_CONNRESET failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#else
    u_long enable = FALSE;
    if (ioctlsocket(fd, SIO_UDP_CONNRESET, &enable) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]ioctlsocket SIO_UDP_CONNRESET failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif
#endif

    return true;
}

ssize_t recv_data_size(SOCKET fd)
{
    ssize_t data_size = -1;
    do
    {
        //Linux/Windows/BSD/macOS/Solaris
#ifdef FIONREAD
        if (ioctlsocket(fd, FIONREAD, (unsigned long*)&data_size) != SOCKET_ERROR)
        {
            break;
        }

        plog(LOG_WARNING, "[%d]ioctlsocket FIONREAD get failed[%d:%s]\n", fd, get_errno(), get_errinfo());
#endif

/*
        //BSD
#ifdef SO_NREAD
        socklen_t data_size_len = sizeof(data_size);
        if (getsockopt(fd, SOL_SOCKET, SO_NREAD, &data_size, &data_size_len) != SOCKET_ERROR)
        {
            break;
        }

        plog(LOG_WARNING, "[%d]getsockopt SOL_SOCKET SO_NREAD get failed[%d:%s]\n", fd, get_errno(), get_errinfo());
#endif

        //Linux
#ifdef SIOCINQ
        if (ioctlsocket(fd, SIOCINQ, (unsigned long*)&data_size) != SOCKET_ERROR)
        {
            break;
        }

        plog(LOG_WARNING, "[%d]ioctlsocket SIOCINQ get failed[%d:%s]\n", fd, get_errno(), get_errinfo());
#endif
*/
        int flags = MSG_PEEK | MSG_TRUNC;
#ifdef MSG_DONTWAIT
        flags |= MSG_DONTWAIT;
#endif
        if ((data_size = recvfrom(fd, NULL, 0, flags, NULL, NULL)) != SOCKET_ERROR)
        {
            break;
        }

        int err_no = get_errno();
#ifdef _WIN32
        if (err_no != WSAEFAULT)
#else
        if (err_no == EFAULT)
#endif
        {
            static char dummy[1] = {0};
            if ((data_size = recvfrom(fd, dummy, sizeof(dummy), flags, NULL, NULL)) != SOCKET_ERROR)
            {
                break;
            }
        }

#ifdef _WIN32
        if (err_no != WSAEWOULDBLOCK)
#else
        if (err_no != EAGAIN && err_no != EWOULDBLOCK)
#endif
        {
            plog(LOG_ERR, "[%d]recvfrom MSG_PEEK | MSG_TRUNC get failed[%d:%s]\n", fd, err_no, get_errinfo());
        }

    } while (0);

    return data_size;
}

bool econnrefused_enable(SOCKET fd)
{
    int enable = 1;
#if defined(SO_RCVALL)
    if (setsockopt(fd, SOL_SOCKET, SO_RCVALL, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_RCVALL 1 failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#elif defined(IP_RECVERR)
    if (setsockopt(fd, IPPROTO_IP, IP_RECVERR, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IP IP_RECVERR 1 failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_bind_to_device(SOCKET fd, const std::string& if_name)
{
#if defined(SO_BINDTODEVICE)/* && !defined(MULTIPLE_EXTERNAL_IP)*/
#if defined(__linux__)
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, (char*)if_name.c_str(), (socklen_t)if_name.size()) == SOCKET_ERROR)
#else
    ifreq interface{};
    strncpy(interface.ifr_name, if_name.c_str(), IFNAMSIZ - 1);
    interface.ifr_name[IFNAMSIZ - 1] = '\0';
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, (char*)&interface, (socklen_t)sizeof(interface)) == SOCKET_ERROR)
#endif
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_SOCKET SO_BINDTODEVICE failed[%d:%s][%s]\n", fd, get_errno(), get_errinfo(), if_name.c_str());
        return false;
    }
#elif defined(SO_DOMAIN)
    uint32_t if_index = get_if_nametoindex(if_name);
    if (if_index == 0)
    {
        return false;
    }

    sa_family_t family = 0xffff;
    socklen_t len = sizeof(family);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &family, &len) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]set_bind_to_device getsockopt SOL_SOCKET SO_DOMAIN failed[%d:%s][%s]\n", fd, get_errno(), get_errinfo(), if_name.c_str());
        return false;
    }

    if (!set_ip_bind_to_if(fd, if_index, family))
    {
        return false;
    }
#endif
    return true;
}

bool set_packet_fanout(SOCKET fd)
{
#ifdef PACKET_FANOUT
    uint fanout_id = ((uint)getpid()) & 0xffff;//gettid
    uint32_t option = (/*PACKET_FANOUT_FLAG_DEFRAG | */(PACKET_FANOUT_HASH << 16) | fanout_id); //(fanout_flags(31 - 24) |  fanout_type(23 - 16) | fanout_grp_id(15 - 0))
    if (setsockopt(fd, SOL_PACKET, PACKET_FANOUT, (char*)&option, sizeof(option)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt SOL_PACKET PACKET_FANOUT failed[%d:%s][0x%08x]\n", fd, get_errno(), get_errinfo(), option);
        return false;
    }
#endif

    return true;
}

bool set_ip_hdr_incl(SOCKET fd)
{
#ifdef IP_HDRINCL
    int enable = 1;
    if (setsockopt(fd, IPPROTO_IP, IP_HDRINCL, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IP IP_HDRINCL failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

uint32_t get_if_nametoindex(const std::string& if_name)
{
#if defined(_WIN32)
    NET_LUID luid{};
    DWORD val = ConvertInterfaceNameToLuidA(if_name.c_str(), &luid);
    if (val != 0/*STATUS_SUCCESS*/)
    {
        plog(LOG_ERR, "ConvertInterfaceNameToLuidA failed[%d:%s][%s]\n", val, strerror(val), if_name.c_str());
        return 0;
    }

    NET_IFINDEX idx{};
    val = ConvertInterfaceLuidToIndex(&luid, &idx);
    if (val != 0/*STATUS_SUCCESS*/)
    {
        plog(LOG_ERR, "ConvertInterfaceLuidToIndex failed[%d:%s][%s]\n", get_errno(), get_errinfo(), if_name.c_str());
        return 0;
    }

    return (uint32_t)idx;

#else
    uint32_t idx = if_nametoindex(if_name.c_str());
    if (idx == 0)
    {
        plog(LOG_ERR, "if_nametoindex failed[%d:%s][%s]\n", get_errno(), get_errinfo(), if_name.c_str());
    }

    return idx;

#endif
}

bool set_ip_bind_to_if(SOCKET fd, uint32_t if_index, sa_family_t fa/* = AF_INET*/)
{
    if (fa == AF_INET)
    {
        return set_ip_bind_to_if_v4(fd, if_index);
    }

    assert(fa == AF_INET6);
    return set_ip_bind_to_if_v6(fd, if_index);
}

bool set_ip_bind_to_if_v4(SOCKET fd, uint32_t if_index)
{
#ifdef IP_BOUND_IF
    if (setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, (char*)&if_index, sizeof(if_index)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IP IP_BOUND_IF failed[%d:%s][%d]\n", fd, get_errno(), get_errinfo(), if_index);
        return false;
    }
#endif

    return true;
}

bool set_ip_bind_to_if_v6(SOCKET fd, uint32_t if_index)
{
#ifdef IPV6_BOUND_IF
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, (char*)&if_index, sizeof(if_index)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IPV6 IPV6_BOUND_IF failed[%d:%s][%d]\n", fd, get_errno(), get_errinfo(), if_index);
        return false;
    }
#endif

    return true;
}

bool set_ip_pktinfo(SOCKET fd, sa_family_t fa/* = AF_INET*/)
{
    if (fa == AF_INET)
    {
        return set_ip_pktinfo_v4(fd);
    }

    assert(fa == AF_INET6);
    return set_ip_pktinfo_v6(fd);
}

bool set_ip_pktinfo_v4(SOCKET fd)
{
    int enable = 1;
#if defined(IP_PKTINFO)
    if (setsockopt(fd, IPPROTO_IP, IP_PKTINFO, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IP IP_PKTINFO failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#elif defined(IP_RECVIF) && !defined(__APPLE__)
    if (setsockopt(fd, IPPROTO_IP, IP_RECVIF, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IP IP_RECVIF failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }

#elif defined(IP_RECVDSTADDR) && !defined(__APPLE__)
    if (setsockopt(fd, IPPROTO_IP, IP_RECVDSTADDR, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IP IP_RECVDSTADDR failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_ip_pktinfo_v6(SOCKET fd)
{
    int enable = 1;

#if defined(IPV6_RECVPKTINFO)
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
#ifdef IPV6_2292PKTINFO
        if (get_errno() == ENOPROTOOPT)
        {
            if (setsockopt(fd, IPPROTO_IPV6, IPV6_2292PKTINFO, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
            {
                plog(LOG_ERR, "[%d]setsockopt IPPROTO_IPV6 IPV6_2292PKTINFO failed[%d:%s]\n", fd, get_errno(), get_errinfo());
                return false;
            }

            return true;
        }

#endif
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IPV6 IPV6_RECVPKTINFO failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }

#elif defined(IPV6_PKTINFO) && !defined(__APPLE__)
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_PKTINFO, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IPV6 IPV6_PKTINFO failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_ip_recv_dst_addr_v4(SOCKET fd)
{
    return set_ip_pktinfo_v4(fd);
}

bool set_ip_recv_pktinfo_v6(SOCKET fd)
{
    return set_ip_pktinfo_v6(fd);
}

bool set_ip_orig_dst_addr(SOCKET fd, sa_family_t fa/* = AF_INET*/)
{
    return set_ip_recv_orig_dst_addr(fd, fa);
}

bool set_ip_orig_dst_addr_v4(SOCKET fd)
{
    return set_ip_recv_orig_dst_addr_v4(fd);
}

bool set_ip_orig_dst_addr_v6(SOCKET fd)
{
    return set_ip_recv_orig_dst_addr_v6(fd);
}

bool set_ip_recv_orig_dst_addr(SOCKET fd, sa_family_t fa/* = AF_INET*/)
{
    if (fa == AF_INET)
    {
        return set_ip_recv_orig_dst_addr_v4(fd);
    }

    assert(fa == AF_INET6);
    return set_ip_recv_orig_dst_addr_v6(fd);
}

bool set_ip_recv_orig_dst_addr_v4(SOCKET fd)
{
    int enable = 1;
#if defined(IP_RECVORIGDSTADDR)
    if (setsockopt(fd, IPPROTO_IP, IP_RECVORIGDSTADDR, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IP IP_RECVORIGDSTADDR failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }

#elif defined(IP_ORIGDSTADDR)
    if (setsockopt(fd, IPPROTO_IP, IP_ORIGDSTADDR, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IP IP_ORIGDSTADDR failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_ip_recv_orig_dst_addr_v6(SOCKET fd)
{
    int enable = 1;
#if defined(IPV6_RECVORIGDSTADDR)
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_RECVORIGDSTADDR, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IPV6 IPV6_RECVORIGDSTADDR failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }

#elif defined(IPV6_ORIGDSTADDR)
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_ORIGDSTADDR, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IPV6 IPV6_ORIGDSTADDR failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_ip_multicast_loop(SOCKET fd, sa_family_t fa /*= AF_INET*/)
{
    if (fa == AF_INET)
    {
        return set_ip_multicast_loop_v4(fd);
    }

    assert(fa == AF_INET6);
    return set_ip_multicast_loop_v6(fd);
}

bool set_ip_multicast_loop_v4(SOCKET fd)
{
    int enable = 1;
#if defined(IP_MULTICAST_LOOP)
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IP IP_MULTICAST_LOOP failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

bool set_ip_multicast_loop_v6(SOCKET fd)
{
    int enable = 1;
#if defined(IPV6_MULTICAST_LOOP)
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]setsockopt IPPROTO_IPV6 IPV6_MULTICAST_LOOP failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }
#endif

    return true;
}

SOCKET create_socket(sa_family_t af /*= AF_INET*/, int type /*= SOCK_STREAM*/, int protocol /*= IPPROTO_TCP*/)
{
    SOCKET fd = socket(af, type, protocol);
    if (fd == INVALID_SOCKET)
    {
        //plog(LOG_ERR, "create socket failed[%d:%s][%d,%d,%d]\n", get_errno(), get_errinfo(), af, type, protocol);
        return INVALID_SOCKET;
    }

    //plog(LOG_INFO, "[%d]create socket successed\n", fd);
    return fd;
}

bool shutdown_socket(SOCKET fd, int how/* = SHUT_WR*/)
{
    if (shutdown(fd, how) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]shutdown failed[%d:%s][%d]\n", fd, get_errno(), get_errinfo(), how);
        return false;
    }

    return true;
}

void close_socket(SOCKET& fd)
{
    if (fd == INVALID_SOCKET)
    {
        return;
    }

#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
    //plog(LOG_INFO, "[%d]close socket successed\n", fd);
    fd = INVALID_SOCKET;
}

bool get_local_addr_by_fd(SOCKET fd, sockaddr_storage& addr)
{
    //memset(&addr, 0, sizeof(addr));
    socklen_t addr_len = sizeof(addr);
    if (getsockname(fd, (sockaddr*)&addr, &addr_len) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]getsockname failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }

    return true;
}

bool get_remote_addr_by_fd(SOCKET fd, sockaddr_storage& addr)
{
    //memset(&addr, 0, sizeof(addr));
    socklen_t addr_len = sizeof(addr);
    if (getpeername(fd, (sockaddr*)&addr, &addr_len) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "[%d]getpeername failed[%d:%s]\n", fd, get_errno(), get_errinfo());
        return false;
    }

    return true;
}

bool get_original_dest_addr_by_fd(SOCKET fd, sockaddr_storage& addr)
{
#ifdef __linux__
    //memset(&addr, 0, sizeof(addr));
    socklen_t addr_len = sizeof(addr);
    if (getsockopt(fd, IPPROTO_IP, SO_ORIGINAL_DST, (char*)&addr, &addr_len) != SOCKET_ERROR)
    {
        return true;
    }

    if (getsockopt(fd, IPPROTO_IPV6, IP6T_SO_ORIGINAL_DST, (char*)&addr, &addr_len) != SOCKET_ERROR)
    {
        return true;
    }
#else

#endif

    plog(LOG_ERR, "[%d]getsockopt IPPROTO_IPV6 IP6T_SO_ORIGINAL_DST failed[%d:%s]\n", fd, get_errno(), get_errinfo());
    return false;
}

sockaddr_storage ip_and_port_to_addr(const std::string& ip, uint16_t port)
{
    sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));

    (void)ip_and_port_to_addr(ip, port, addr);
    return addr;
}

bool ip_and_port_to_addr(const std::string& ip, uint16_t port, sockaddr_storage& addr)
{
    if (!ip_to_addr(ip, addr))
    {
        return false;
    }

    port_to_addr(port, addr);
    return true;
}

bool ip_and_port_to_addr(const std::string& ip, uint16_t port, sockaddr_storage& addr, sa_family_t fa)
{
    if (!ip_to_addr(ip, addr, fa))
    {
        return false;
    }

    port_to_addr(port, addr);
    return true;
}

sockaddr_storage ip_to_addr(const std::string& ip)
{
    sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));

    (void)ip_to_addr(ip, addr);
    return addr;
}

bool ip_to_addr(const std::string& ip, sockaddr_storage& addr)
{
    if (ip_to_addr(ip, addr, AF_INET) || ip_to_addr(ip, addr, AF_INET6))
    {
        return true;
    }

    return false;
}

bool ip_to_addr(const std::string& ip, sockaddr_storage& addr, sa_family_t fa)
{
#ifndef _WIN32
    if (fa == AF_UNIX)
    {
        static const size_t k_sun_path_max_size = sizeof(((sockaddr_un*)0)->sun_path);
        if (ip.length() >= k_sun_path_max_size)
        {
            plog(LOG_ERR, "ip length >= sun_path_max_size[%zu:%zu][%s]\n", ip.length(), k_sun_path_max_size, ip.c_str());
            return false;
        }

        sockaddr_un& su_addr = *(sockaddr_un*)&addr;
        memset(&su_addr, 0, sizeof(su_addr));
        su_addr.sun_family = AF_UNIX;
        strcpy(su_addr.sun_path, ip.c_str());
#if defined(__APPLE__)
        su_addr.sun_len = (uint8_t)sizeof(sockaddr_un);
#endif
        return true;
    }
#endif

#if defined(_WIN32) && defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0600

    int addr_len = fa == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    if (WSAStringToAddressA((char*)ip.c_str(), fa, NULL, (sockaddr*)&addr, &addr_len) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "WSAStringToAddressA failed[%d:%s][%s]\n", get_errno(), get_errinfo(), ip.c_str());
        return false;
    }

#else

    std::string tmp_ip = ip;
    std::string::size_type pos = fa == AF_INET ? ip.rfind(':') : ip.rfind(']');
    if (pos != std::string::npos)
    {
        tmp_ip = fa == AF_INET ? ip.substr(0, pos) : ip.substr(1, pos - 1);
    }
    if (inet_pton(fa,
                  tmp_ip.c_str(),
                  fa == AF_INET ? (char*)&((sockaddr_in*)&addr)->sin_addr
                                : (char*)&((sockaddr_in6*)&addr)->sin6_addr) <= 0)
    {
        plog(LOG_ERR, "inet_pton failed[%d:%s][%s]\n", get_errno(), get_errinfo(), tmp_ip.c_str());
        return false;
    }

    if (pos != std::string::npos)
    {
        std::string port;
        if (fa == AF_INET)
        {
            port = ip.substr(pos + 1);
        }
        else if (fa == AF_INET6 && (pos = ip.rfind("]:")) != std::string::npos)
        {
            port = ip.substr(pos + 2);
        }

        if (!port.empty())
        {
            addr.ss_family = fa;
            port_to_addr((uint16_t)atol(port.c_str()), addr);
        }
    }

#if defined(__APPLE__)
    addr.ss_len = (uint8_t)(fa == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
#endif

#endif

    addr.ss_family = fa;
    return true;
}

std::string addr_to_ip_and_port(const sockaddr_storage& addr)
{
    std::string ip = addr_to_ip(addr);
#ifndef _WIN32
    if (addr.ss_family == AF_UNIX)
    {
        return ip.empty() ? "" : ip;
    }
#endif

    uint16_t port = port_from_addr(addr);

#if 0
    if (port == 0)
    {
        return ip.empty() ? "" : ip;
    }
#else
    if (ip.empty() && port == 0)
    {
        return "";
    }
#endif

    return addr.ss_family == AF_INET ? ip + ":" + std::to_string(port) : "[" + ip + "]:" + std::to_string(port);
}

std::string addr_to_ip(const sockaddr_storage& addr)
{
    std::string ip;
    addr_to_ip(ip, addr);
    return ip;
}

bool addr_to_ip(std::string& ip, const sockaddr_storage& addr)
{
    if (addr.ss_family == AF_UNSPEC)
    {
        return true;
    }

#ifndef _WIN32
    if (addr.ss_family == AF_UNIX)
    {
        sockaddr_un* sau = (sockaddr_un*)&addr;

        ip = sau->sun_path;
        return true;
    }
#endif

    char ip_buf[INET6_ADDRSTRLEN] = { 0 };

#if defined(_WIN32) && defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0600

    DWORD ip_buf_len = sizeof(ip_buf);
    DWORD addr_len = addr.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    if (WSAAddressToStringA((sockaddr*)&addr, addr_len, NULL, ip_buf, &ip_buf_len) == SOCKET_ERROR)
    {
        plog(LOG_ERR, "WSAAddressToStringA failed[%d:%s]\n", get_errno(), get_errinfo());
        return false;
    }

#else
    if (inet_ntop(addr.ss_family,
                  addr.ss_family == AF_INET ? (char*)&((const sockaddr_in*)&addr)->sin_addr
                                            : (char*)&((const sockaddr_in6*)&addr)->sin6_addr,
                  ip_buf, (socklen_t)sizeof(ip_buf)) == NULL)
    {
        plog(LOG_ERR, "inet_ntop failed[%d:%s]\n", get_errno(), get_errinfo());
        return false;
    }
#endif

    ip = ip_buf;
#if defined(_WIN32) && defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0600
    std::string::size_type pos = addr.ss_family == AF_INET ? ip.rfind(':') : ip.rfind("]:");
    if (pos != std::string::npos)
    {
        ip = addr.ss_family == AF_INET ? ip.substr(0, pos) : ip.substr(1, pos -1);
    }
#endif

    return true;
}

uint16_t port_from_addr(const sockaddr_storage& addr)
{
    if (addr.ss_family == AF_UNSPEC)
    {
        return 0;
    }
    if (!(addr.ss_family == AF_INET || addr.ss_family == AF_INET6)) {
        plog(LOG_ERR, "port_from_addr udp.ss_family is not ipv4 or ipv6,[%d]\n", addr.ss_family);
        return 0;
    }

    return ntohs(addr.ss_family == AF_INET ? ((const sockaddr_in*)&addr)->sin_port
                                           : ((const sockaddr_in6*)&addr)->sin6_port);
}

void port_to_addr(uint16_t port, sockaddr_storage& addr)
{
    if (addr.ss_family == AF_UNSPEC)
    {
        return;
    }
    if (!(addr.ss_family == AF_INET || addr.ss_family == AF_INET6)) {
        plog(LOG_ERR, "port_to_addr udp.ss_family is not ipv4 or ipv6,[%d][%hu]\n", addr.ss_family, port);
        return ;
    }

    (addr.ss_family == AF_INET ? ((sockaddr_in*)&addr)->sin_port
                               : ((sockaddr_in6*)&addr)->sin6_port) = htons(port);

#if defined(__APPLE__)
    addr.ss_len = (uint8_t)(addr.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
#endif
}

int netmask_ip_to_prefixlen(const std::string& ip)
{
    sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    if (!socket_helper::ip_to_addr(ip, addr))
    {
        return -1;
    }

    return netmask_addr_to_prefixlen(addr);
}

int netmask_addr_to_prefixlen(const sockaddr_storage& addr)
{
    if (addr.ss_family != AF_INET && addr.ss_family != AF_INET6)
    {
        return 0;
    }

    int prefix_length = 0;
    size_t s_addr_size = addr.ss_family == AF_INET ? 4 : 16;
    uint8_t* s_addr_p = addr.ss_family == AF_INET ? (uint8_t*)(&((sockaddr_in*)&addr)->sin_addr.s_addr) : (uint8_t*)(&((sockaddr_in6*)&addr)->sin6_addr.s6_addr);
#if 0
    size_t i = 0;
    for (; i < s_addr_size; ++i)
    {
        if (s_addr_p[i] != 0xFF)
        {
            break;
        }
        prefix_length += 8;
    }

    if (i != s_addr_size)
    {
        uint8_t last_ip_byte = s_addr_p[i];
        for (int j = 0; j < 7; ++j)
        {
            if (last_ip_byte == 0)
            {
                break;
            }

            last_ip_byte <<= 1;
            ++prefix_length;
        }
    }
#else
    std::bitset<128> bits;
    for (size_t i = 16 - s_addr_size; i < s_addr_size; ++i)
    {
        bits |= std::bitset<128>(s_addr_p[i]) << (8 * (15 - i));
    }
    prefix_length = (int)bits.count();
#endif

    return prefix_length;
}

std::string netmask_prefixlen_to_ip(int prefix_length, sa_family_t fa /*= AF_INET*/)
{
    sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));

    if (!netmask_prefixlen_to_addr(prefix_length, addr, fa))
    {
        return std::string();
    }

    return addr_to_ip(addr);
}

bool netmask_prefixlen_to_addr(int prefix_length, sockaddr_storage& addr, sa_family_t fa /*= AF_INET*/)
{
    if (prefix_length < 1)
    {
        return false;
    }

    if (fa != AF_INET && fa != AF_INET6)
    {
        return false;
    }

    //memset(&addr, 0, sizeof(addr));
    if (fa == AF_INET)
    {
        prefix_length = prefix_length > 32 ? 32 : prefix_length;
        uint32_t mask = ~0u;
        mask <<= (32 - prefix_length);

        sockaddr_in* net_mask = (sockaddr_in*)&addr;
        net_mask->sin_family = AF_INET;
        net_mask->sin_addr.s_addr = htonl(mask);
#if defined(__APPLE__)
        net_mask->sin_len = (uint8_t)sizeof(sockaddr_in);
#endif
    }

    if (fa == AF_INET6)
    {
        prefix_length = prefix_length > 128 ? 128 : prefix_length;

        sockaddr_in6* net_mask6 = (sockaddr_in6*)&addr;
        net_mask6->sin6_family = AF_INET6;
        for (int i = 0; i < prefix_length / 8; ++i)
        {
            net_mask6->sin6_addr.s6_addr[i] = 0xff;
        }
        if (prefix_length % 8)
        {
            net_mask6->sin6_addr.s6_addr[prefix_length / 8] = (uint8_t)(0xff << (8 - (prefix_length % 8)));
        }
#if defined(__APPLE__)
        net_mask6->sin6_len = (uint8_t)sizeof(sockaddr_in6);
#endif
    }

    return true;
}

bool is_same_network(const sockaddr_storage& addr, const sockaddr_storage& net_addr, uint8_t mask_bits)
{
    assert(mask_bits != 0);
    if (addr.ss_family != net_addr.ss_family)
    {
        return false;
    }

    if (addr.ss_family == AF_INET)
    {
        uint8_t mask_tmp = mask_bits > 32 ? 32 : mask_bits;
        in_addr& addr_sa = ((sockaddr_in*)&addr)->sin_addr;
        in_addr& net_addr_sa = ((sockaddr_in*)&net_addr)->sin_addr;

        if (((addr_sa.s_addr ^ net_addr_sa.s_addr) & htonl(0xFFFFFFFF << (32 - mask_tmp))) != 0)
        {
            return false;
        }

        return true;
    }

    if (addr.ss_family == AF_INET6)
    {
        uint8_t mask_tmp = mask_bits > 128 ? 128 : mask_bits;
        uint8_t* addr_sa_s6 = &((sockaddr_in6*)&addr)->sin6_addr.s6_addr[0];
        uint8_t* net_addr_sa_s6 = &((sockaddr_in6*)&net_addr)->sin6_addr.s6_addr[0];

        for (int i=0; i<mask_tmp/8; ++i)
        {
            if (addr_sa_s6[i] != net_addr_sa_s6[i])
            {
                return false;
            }
        }

        if ((mask_tmp % 8) != 0)
        {
            if (((addr_sa_s6[mask_tmp / 8] ^ net_addr_sa_s6[mask_tmp / 8]) & (0xFF << (8 - (mask_tmp % 8)))) != 0)
            {
                return false;
            }
        }

        return true;
    }

    return false;
}

std::vector<sockaddr_storage> get_if_addrs(int ai_family/* = AF_UNSPEC*/, bool disable_loopback /*= true*/)
{
    std::vector<sockaddr_storage> addrs;

#if !defined(_WIN32) && !defined(__ANDROID__)
    do
    {
        ifaddrs* ifaddr = NULL;
        if (getifaddrs(&ifaddr) == -1)
        {
            plog(LOG_ERR, "get_if_addrs: getifaddrs failed[%d:%s]\n", get_errno(), get_errinfo());
            break;
        }

        for (ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
        {
            if (disable_loopback && (ifa->ifa_flags & IFF_LOOPBACK))
            {
                continue;
            }

            if (!(ifa->ifa_flags & IFF_UP))
            {
                continue;
            }

#if 0
            if (!(ifa->ifa_flags & IFF_RUNNING))
            {
                continue;
            }

            if (ifa->ifa_flags & IFF_BROADCAST)
            {
                continue;
            }
#endif

            if (ifa->ifa_addr == NULL)
            {
                continue;
            }

            if (ai_family != AF_UNSPEC && ifa->ifa_addr->sa_family != ai_family)
            {
                continue;
            }

            if (ifa->ifa_addr->sa_family != AF_INET && ifa->ifa_addr->sa_family != AF_INET6)
            {
                continue;
            }

            sockaddr_storage ss;
            memset(&ss, 0, sizeof(ss));
            memcpy(&ss, ifa->ifa_addr, ifa->ifa_addr->sa_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
            addrs.push_back(ss);
        }

        freeifaddrs(ifaddr);
    } while (0);
#endif

    return addrs;
}

size_t get_ip_by_if(std::vector<std::string>& ipv4_addr_s, std::vector<std::string>& ipv6_addr_s, const std::vector<std::string>& if_name_s, bool disable_loopback /*= true*/)
{
    ipv4_addr_s.clear();
    ipv6_addr_s.clear();

    size_t addr_num = 0;

#if !defined(_WIN32) && !defined(__ANDROID__)
    do
    {
        ifaddrs* addr = NULL;
        if (getifaddrs(&addr) == -1)
        {
            plog(LOG_ERR, "get_ip_by_if: getifaddrs failed[%d:%s]\n", get_errno(), get_errinfo());
            break;
        }

        for (ifaddrs* ifa = addr; ifa != NULL; ifa = ifa->ifa_next)
        {
            if (disable_loopback && (ifa->ifa_flags & IFF_LOOPBACK))
            {
                continue;
            }

            if (!(ifa->ifa_flags & IFF_UP))
            {
                continue;
            }

#if 0
            if (!(ifa->ifa_flags & IFF_RUNNING))
            {
                continue;
            }

            if (ifa->ifa_flags & IFF_BROADCAST)
            {
                continue;
            }
#endif

            if (ifa->ifa_addr == NULL)
            {
                continue;
            }

            if (ifa->ifa_addr->sa_family != AF_INET && ifa->ifa_addr->sa_family != AF_INET6)
            {
                continue;
            }

            if (ifa->ifa_name == NULL)
            {
                continue;
            }

            if (std::find(if_name_s.begin(), if_name_s.end(), ifa->ifa_name) == if_name_s.end())
            {
                continue;
            }

#if 0
            char host[NI_MAXHOST] = { 0 };
            int errcode = getnameinfo(ifa->ifa_addr, ifa->ifa_addr->sa_family == AF_INET ? (socklen_t)sizeof(struct sockaddr_in) : (socklen_t)sizeof(struct sockaddr_in6),
                host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            if (errcode != 0)
            {
                plog(LOG_ERR, "getnameinfo failed[%d:%s]\n", errcode, gai_strerror(errcode));
                continue;
            }

            if (ifa->ifa_addr->sa_family == AF_INET6)
            {
                char* ifname = strrchr(host, '%');
                if (ifname != NULL)
                {
                    *ifname = '\0';
                }
            }
#else
            char host[INET6_ADDRSTRLEN] = { 0 };
            char* presentation = (char*)inet_ntop(ifa->ifa_addr->sa_family,
                ifa->ifa_addr->sa_family == AF_INET ? (char*)&((sockaddr_in*)ifa->ifa_addr)->sin_addr : (char*)&((sockaddr_in6*)ifa->ifa_addr)->sin6_addr, host, (socklen_t)sizeof(host));
            if (presentation == NULL)
            {
                plog(LOG_ERR, "inet_ntop failed[%d:%s]\n", get_errno(), get_errinfo());
                continue;
            }
#endif

            plog(LOG_WARNING, "[%s]%s\n", ifa->ifa_name, host);
            ifa->ifa_addr->sa_family == AF_INET ? ipv4_addr_s.emplace_back(host) : ipv6_addr_s.emplace_back(host);
            ++addr_num;
        }

        freeifaddrs(addr);
    } while (0);

#endif

    return addr_num;
}

std::vector<sockaddr_storage> get_addr_info(const std::string& node_name, const std::string& serv_name/* = std::string()*/,
    int ai_family/* = AF_UNSPEC*/, int ai_flags/* = AI_PASSIVE*/, int ai_socktype/* = 0*/, int ai_protocol/* = 0*/)
{
    std::vector<sockaddr_storage> addrs;

    addrinfo addr_info;
    memset(&addr_info, 0, sizeof(addr_info));
    addr_info.ai_flags = ai_flags;
    addr_info.ai_family = ai_family;
    addr_info.ai_socktype = ai_socktype;
    addr_info.ai_protocol = ai_protocol;

    do
    {
        addrinfo* addr_info_list = NULL;
        int err_no = getaddrinfo(node_name.c_str(), serv_name.c_str(), &addr_info, &addr_info_list);
        if (err_no != 0)
        {
            plog(LOG_ERR, "getaddrinfo failed[%d:%s][%s:%s]\n", err_no,  gai_strerror(err_no), node_name.c_str(), serv_name.c_str());
            break;
        }

        for (addrinfo* curr_addr_info = addr_info_list; curr_addr_info != NULL; curr_addr_info = curr_addr_info->ai_next)
        {
            if (ai_family != AF_UNSPEC && curr_addr_info->ai_family != ai_family)
            {
                continue;
            }

            sockaddr_storage ss;
            memset(&ss, 0, sizeof(ss));
            memcpy(&ss, curr_addr_info->ai_addr, curr_addr_info->ai_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6)/*curr_addr_info->ai_addrlen*/);
            addrs.push_back(ss);
        }

        freeaddrinfo(addr_info_list);
    } while (0);

    return addrs;
}

bool get_name_info(const sockaddr_storage& addr, std::string& host, std::string& serv, int flags/* = 0*/)
{
    host.resize(NI_MAXHOST, 0);
    serv.resize(NI_MAXSERV, 0);

    socklen_t addr_len = addr.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    int err_no = getnameinfo((const sockaddr*)&addr, addr_len, (char*)host.c_str(), NI_MAXHOST, (char*)serv.c_str(), NI_MAXSERV, flags);
    if (err_no != 0)
    {
        plog(LOG_ERR, "getnameinfo failed[%d:%s][%s:%s]\n", err_no,  gai_strerror(err_no), host.c_str(), serv.c_str());
        host.clear();
        serv.clear();
        return false;
    }

    host.resize(strlen(host.c_str()));
    serv.resize(strlen(serv.c_str()));
    return true;
}

bool if_is_up(const std::string& if_name, const sockaddr_storage* addr/* = nullptr*/)
{
    if (if_name.empty())
    {
        return false;
    }

    if (addr != nullptr && addr->ss_family != AF_INET && addr->ss_family != AF_INET6)
    {
        addr = nullptr;
    }

    bool is_up = false;
#if !defined(_WIN32) && !defined(__ANDROID__)
    do
    {
        ifaddrs* ifaddr = NULL;
        if (getifaddrs(&ifaddr) == -1)
        {
            plog(LOG_ERR, "if_is_up: getifaddrs failed[%d:%s]\n", get_errno(), get_errinfo());
            break;
        }

        for (ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
        {
            if (!(ifa->ifa_flags & IFF_UP))
            {
                continue;
            }

#if 0
            if (!(ifa->ifa_flags & IFF_RUNNING))
            {
                continue;
            }
#endif

            if (ifa->ifa_addr == NULL)
            {
                continue;
            }

            if (ifa->ifa_addr->sa_family != AF_INET && ifa->ifa_addr->sa_family != AF_INET6)
            {
                continue;
            }

            if (ifa->ifa_name == NULL)
            {
                continue;
            }

            if (strstr(ifa->ifa_name, if_name.c_str()) == NULL)
            {
                continue;
            }

            if (addr == nullptr)
            {
                is_up = true;
                break;
            }

            size_t addr_offset = addr->ss_family == AF_INET ? offsetof(sockaddr_in, sin_addr) : offsetof(sockaddr_in6, sin6_addr);
            size_t addr_size = addr->ss_family == AF_INET ? sizeof(in_addr) : sizeof(in6_addr);
            if (memcmp(((uint8_t*)ifa->ifa_addr) + addr_offset, ((uint8_t*)addr) + addr_offset, addr_size) == 0)
            {
                is_up = true;
                break;
            }
        }

        freeifaddrs(ifaddr);
    } while (0);
#endif

    return is_up;
}

}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
