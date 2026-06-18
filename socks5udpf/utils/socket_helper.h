#pragma once
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <handleapi.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#ifdef _WIN32

#ifndef socklen_t
typedef int                socklen_t;
#endif

#ifndef in_port_t
typedef unsigned short int in_port_t;
#endif

#ifndef sa_family_t
typedef unsigned short int sa_family_t;
#endif

#ifndef ssize_t
typedef int                ssize_t;
#endif

#ifndef SHUT_RD
#define SHUT_RD            SD_RECEIVE
#endif

#ifndef SHUT_WR
#define SHUT_WR            SD_SEND
#endif

#ifndef SHUT_RDWR
#define SHUT_RDWR          SD_BOTH
#endif

#else

#ifndef INVALID_SOCKET
#define INVALID_SOCKET  (-1)
#endif

#ifndef SOCKET_ERROR
#define SOCKET_ERROR    (-1)
#endif

#ifndef SOCKET
typedef int             SOCKET;
#endif

#ifndef ioctlsocket
#define ioctlsocket     ioctl
#endif

#ifndef closesocket
#define closesocket     close
#endif

#endif

#if defined(IPV6_2292PKTINFO) && !defined(IPV6_PKTINFO)
#define IPV6_PKTINFO IPV6_2292PKTINFO
#endif

namespace socket_helper
{
    int get_errno();
    char* get_errinfo();
    bool get_socket_error(SOCKET fd);
    int get_socket_error_no(SOCKET fd);

    bool set_cloexec(SOCKET fd);
    bool set_non_block(SOCKET fd);
    bool clr_non_block(SOCKET fd);
    bool set_exclusive_addr_use(SOCKET fd);
    bool set_reuse_addr(SOCKET fd);
    bool set_reuse_port(SOCKET fd);
    bool set_no_check(SOCKET fd);
    bool set_conditional_accept(SOCKET fd);

    uint32_t get_snd_timeout(SOCKET fd);
    bool set_snd_timeout(SOCKET fd, uint32_t ms);
    uint32_t get_rcv_timeout(SOCKET fd);
    bool set_rcv_timeout(SOCKET fd, uint32_t ms);

    int get_snd_buf_size(SOCKET fd);
    bool set_no_snd_buf(SOCKET fd);
    bool set_snd_buf_size(SOCKET fd, int buf_size);
    int get_rcv_buf_size(SOCKET fd);
    bool set_no_rcv_buf(SOCKET fd);
    bool set_rcv_buf_size(SOCKET fd, int buf_size);

    int get_snd_buf_force_size(SOCKET fd);
    bool set_no_snd_buf_force(SOCKET fd);
    bool set_snd_buf_force_size(SOCKET fd, int buf_size);
    int get_rcv_buf_force_size(SOCKET fd);
    bool set_no_rcv_buf_force(SOCKET fd);
    bool set_rcv_buf_force_size(SOCKET fd, int buf_size);

    bool set_snd_lowat(SOCKET fd, int lowat_size = 1);
    bool set_rcv_lowat(SOCKET fd, int lowat_size = 1);

    bool set_msg_no_signal(SOCKET fd);
    bool set_no_sigpipe(SOCKET fd);
    bool set_tcp_quick_ack(SOCKET fd);
    bool set_tcp_no_delay(SOCKET fd);
    bool set_tcp_no_push(SOCKET fd);
    bool set_tcp_cork(SOCKET fd);
    bool set_udp_cork(SOCKET fd);
    int get_mss(SOCKET fd);
    int get_mtu(SOCKET fd);
    int get_mtu(const std::string& if_name);

    bool set_socket_linger(SOCKET fd, int linger_s = 0); //4.4BSD假设其单位是时钟滴答(百分之一秒),但Posix.1g规定单位为秒
    bool set_socket_dont_linger(SOCKET fd);
    bool set_socket_oob_inline(SOCKET fd);

    bool set_socket_keep_alive(SOCKET fd);
    bool set_tcp_keep_alive(SOCKET fd, int idle_s, int interval_s, int count);
    bool set_tcp_user_timeout(SOCKET fd, uint32_t ms = 0);

    bool set_socket_dont_route(SOCKET fd);
    bool set_ip_free_bind(SOCKET fd, sa_family_t fa = AF_INET);
    bool set_ip_free_bind_v4(SOCKET fd);
    bool set_ip_free_bind_v6(SOCKET fd);
    bool set_socket_bind_any(SOCKET fd); //openbsd
    bool set_ip_bind_any(SOCKET fd, sa_family_t fa = AF_INET); //freebsd
    bool set_ip_bind_any_v4(SOCKET fd);
    bool set_ip_bind_any_v6(SOCKET fd);
    bool set_ip_transparent(SOCKET fd, sa_family_t fa = AF_INET);
    bool set_ip_transparent_v4(SOCKET fd);
    bool set_ip_transparent_v6(SOCKET fd);

    bool set_ipv6_v6only(SOCKET fd);    //linux下开启仅IPv6模式
    bool clr_ipv6_v6only(SOCKET fd);    //windows下开启仅IPv6兼IPv4模式
    bool set_mark(SOCKET fd, int mark);
    bool set_icmp_ttl(SOCKET fd, int ttl);
    bool set_sio_udp_connreset(SOCKET fd); //控制是否报告 UDP PORT_UNREACHABLE消息。 设置为 TRUE 以启用报告。 设置为 FALSE 可禁用报告。
    ssize_t recv_data_size(SOCKET fd);
    bool econnrefused_enable(SOCKET fd);

    bool set_bind_to_device(SOCKET fd, const std::string& if_name);//Linux
    bool set_packet_fanout(SOCKET fd);
    bool set_ip_hdr_incl(SOCKET fd);
    uint32_t get_if_nametoindex(const std::string& if_name);
    bool set_ip_bind_to_if(SOCKET fd, uint32_t if_index, sa_family_t fa = AF_INET);//Mac
    bool set_ip_bind_to_if_v4(SOCKET fd, uint32_t if_index);//Mac
    bool set_ip_bind_to_if_v6(SOCKET fd, uint32_t if_index);//Mac

    bool set_ip_pktinfo(SOCKET fd, sa_family_t fa = AF_INET);
    bool set_ip_pktinfo_v4(SOCKET fd);
    bool set_ip_pktinfo_v6(SOCKET fd);
    bool set_ip_recv_dst_addr_v4(SOCKET fd);
    bool set_ip_recv_pktinfo_v6(SOCKET fd);

    bool set_ip_orig_dst_addr(SOCKET fd, sa_family_t fa = AF_INET);
    bool set_ip_orig_dst_addr_v4(SOCKET fd);
    bool set_ip_orig_dst_addr_v6(SOCKET fd);
    bool set_ip_recv_orig_dst_addr(SOCKET fd, sa_family_t fa = AF_INET);
    bool set_ip_recv_orig_dst_addr_v4(SOCKET fd);
    bool set_ip_recv_orig_dst_addr_v6(SOCKET fd);

    bool set_ip_multicast_loop(SOCKET fd, sa_family_t fa = AF_INET);
    bool set_ip_multicast_loop_v4(SOCKET fd);
    bool set_ip_multicast_loop_v6(SOCKET fd);

    SOCKET create_socket(sa_family_t af = AF_INET, int type = SOCK_STREAM, int protocol = IPPROTO_TCP);
    bool shutdown_socket(SOCKET fd, int how = SHUT_WR);
    void close_socket(SOCKET& fd);

    bool get_local_addr_by_fd(SOCKET fd, sockaddr_storage& addr);
    bool get_remote_addr_by_fd(SOCKET fd, sockaddr_storage& addr);
    bool get_original_dest_addr_by_fd(SOCKET fd, sockaddr_storage& addr);

    sockaddr_storage ip_to_addr(const std::string& ip);
    bool ip_to_addr(const std::string& ip, sockaddr_storage& addr);
    bool ip_to_addr(const std::string& ip, sockaddr_storage& addr, sa_family_t fa);
    bool addr_to_ip(std::string& ip, const sockaddr_storage& addr);
    std::string addr_to_ip(const sockaddr_storage& addr);

    void port_to_addr(uint16_t port, sockaddr_storage& addr);
    uint16_t port_from_addr(const sockaddr_storage& addr);

    sockaddr_storage ip_and_port_to_addr(const std::string& ip, uint16_t port);
    bool ip_and_port_to_addr(const std::string& ip, uint16_t port, sockaddr_storage& addr);
    bool ip_and_port_to_addr(const std::string& ip, uint16_t port, sockaddr_storage& addr, sa_family_t fa);
    std::string addr_to_ip_and_port(const sockaddr_storage& addr);

    int netmask_ip_to_prefixlen(const std::string& ip);
    int netmask_addr_to_prefixlen(const sockaddr_storage& addr);
    bool netmask_prefixlen_to_addr(int prefix_length, sockaddr_storage& addr, sa_family_t fa = AF_INET);
    std::string netmask_prefixlen_to_ip(int prefix_length, sa_family_t fa = AF_INET);

    bool is_same_network(const sockaddr_storage& addr, const sockaddr_storage& net_addr, uint8_t mask_bits);

    std::vector<sockaddr_storage> get_if_addrs(int ai_family = AF_UNSPEC, bool disable_loopback = true);
    size_t get_ip_by_if(std::vector<std::string>& ipv4_addr_s, std::vector<std::string>& ipv6_addr_s, const std::vector<std::string>& if_name_s, bool disable_loopback = true);
    std::vector<sockaddr_storage> get_addr_info(const std::string& node_name, const std::string& serv_name = std::string(),
        int ai_family = AF_UNSPEC, int ai_flags = AI_PASSIVE, int ai_socktype = 0, int ai_protocol = 0);
    bool get_name_info(const sockaddr_storage& addr, std::string& host, std::string& serv, int flags = 0);
    bool if_is_up(const std::string& if_name, const sockaddr_storage* addr = nullptr);
};
