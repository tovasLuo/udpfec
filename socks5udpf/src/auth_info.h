#pragma once
#include <ctime>
#include <cstdint>
#include <cstring>
#include <string>
#include <netinet/in.h>
#include "socket_helper.h"
#include "string_helper.h"

#define VERIFY_TYPE_NONE                 0
#define VERIFY_TYPE_PREP_VERIFY          1
#define VERIFY_TYPE_QUERY_USER_REQUEST   2

struct auth_info_t
{
    auth_info_t()
        : verify_type(VERIFY_TYPE_NONE)
        , user_pwd(0)
        , socks5_fd(INVALID_SOCKET)
        , user_type(0)
    {
        memset(&socks5_addr, 0, sizeof(socks5_addr));
        memset(&prep_verify_time, 0, sizeof(prep_verify_time));
        memset(&user_up_time, 0, sizeof(user_up_time));
        memset(&lask_check_time, 0, sizeof(lask_check_time));
        update_bind_local_addr();
        //IN6ADDR_ANY_INIT
    }

    auth_info_t(int in_verify_type, const std::string& in_user_name, const std::string& in_user_pwd,
        const std::string& in_user_ip, int in_user_type, const std::string& in_area_svr_id,
        const timespec& in_prep_verify_time, const timespec& in_user_up_time, const timespec& in_lask_check_time,
        const std::string& in_bind_local_ipv4_str = std::string(), const std::string& in_bind_local_ipv6_str = std::string(),
        const sockaddr_storage& in_socks5_addr = sockaddr_storage(), SOCKET in_socks5_fd = INVALID_SOCKET)
        : verify_type(in_verify_type)
        , user_pwd(0)
        , user_pwd_str(in_user_pwd)
        , user_name(in_user_name)
        , user_ip(in_user_ip)
        , socks5_addr(in_socks5_addr)
        , socks5_fd(in_socks5_fd)
        , user_type(in_user_type)
        , area_svr_id(in_area_svr_id)
        , prep_verify_time(in_prep_verify_time)
        , user_up_time(in_user_up_time)
        , lask_check_time(in_lask_check_time)
        , bind_local_ipv4_str(in_bind_local_ipv4_str)
        , bind_local_ipv6_str(in_bind_local_ipv6_str)
    {
        user_pwd = string_helperA::to_arithmetic_default<uint32_t>(user_pwd_str);
        update_bind_local_addr();
    }

    void update_bind_local_addr()
    {
        if (bind_local_ipv4_str.empty())
        {
            memset(&bind_local_ipv4_addr, 0, sizeof(bind_local_ipv4_addr));
            bind_local_ipv4_addr.ss_family = AF_INET;
        }
        else
        {
            socket_helper::ip_to_addr(bind_local_ipv4_str, bind_local_ipv4_addr, AF_INET);
        }

        if (bind_local_ipv6_str.empty())
        {
            memset(&bind_local_ipv6_addr, 0, sizeof(bind_local_ipv6_addr));
            bind_local_ipv6_addr.ss_family = AF_INET6;
        }
        else
        {
            socket_helper::ip_to_addr(bind_local_ipv6_str, bind_local_ipv6_addr, AF_INET6);
        }
    }

    int              verify_type;
    uint32_t         user_pwd;        //用户临时密码 DB分配 唯一
    std::string      user_pwd_str;
    std::string      user_name;       //用户
    std::string      user_ip;
    sockaddr_storage socks5_addr;
    SOCKET           socks5_fd;
    int              user_type;
    std::string      area_svr_id;     //gid,aid,sid,uip
    timespec         prep_verify_time;
    timespec         user_up_time;
    timespec         lask_check_time;
    std::string      bind_local_ipv4_str;
    std::string      bind_local_ipv6_str;
    sockaddr_storage bind_local_ipv4_addr;
    sockaddr_storage bind_local_ipv6_addr;
};
