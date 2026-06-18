#pragma once
#include <cstring>
#include <ctime>
#ifdef UDP_NONBLOCK
#include <list>
#endif
#include <string>
#include <netinet/in.h>
#include <sys/syscall.h>
#include <unistd.h>
//#include "dly_cache.h"
#include "epoll_wrap.h"
//#include "loss_stat.h"
#include "sockaddr_storage_index.h"
#include "socket_helper.h"

#ifdef UDP_NONBLOCK
struct data_2_game_client_t
{
    data_2_game_client_t()
        //: print_prefix(NULL)
    {
        bzero(&client_addr, sizeof(client_addr));
        bzero(&client_node_addr, sizeof(client_node_addr));
        bzero(&server_addr, sizeof(server_addr));
    }

    ~data_2_game_client_t()
    {}

    sockaddr_storage     client_addr;
    sockaddr_storage     client_node_addr;
    sockaddr_storage     server_addr;
    std::vector<uint8_t> data;
    std::string          print_prefix;
};
#endif

struct listen_session_info
{
    listen_session_info()
        : listen_fd(INVALID_SOCKET)
#ifdef UDP_NONBLOCK
        , epoll_ptr( NULL)
#endif
        //, last_transpond_time(0)
    {
        bzero(&listen_addr, sizeof(listen_addr));
    }

    ~listen_session_info() {}

    const char* get_print_prefix(bool update = false)
    {
        if (!print_prefix.empty() && !update)
        {
            return print_prefix.c_str();
        }

        print_prefix  = /*"[" + */std::string("]");
        print_prefix += "[]";
        print_prefix += "[]";
        print_prefix += "[" + std::to_string(listen_fd) + "]";
        print_prefix += "[]";
        print_prefix += "[]";
        print_prefix += "[" + socket_helper::addr_to_ip_and_port(listen_addr) + "]";
        print_prefix += "[]";
        print_prefix += std::string("[") /* + "]"*/;

        return print_prefix.c_str();
    }

    sockaddr_storage        listen_addr;
    SOCKET                  listen_fd;
#ifdef UDP_NONBLOCK
    epoll_wrap*             epoll_ptr;

    std::list<data_2_game_client_t> data_s_2_game_client;
#endif

    //time_t                last_transpond_time;
    std::string             print_prefix;
};

#ifdef UDP_NONBLOCK
struct data_2_game_server_t
{
    data_2_game_server_t()
    {
        bzero(&addr, sizeof(addr));
    }

    ~data_2_game_server_t()
    {}

    sockaddr_storage     addr;
    std::vector<uint8_t> data;
};
#endif

struct pair_session_info
{
    pair_session_info()
        : game_server_node_fd(INVALID_SOCKET)
        , game_client_icmp_id(0)
        , game_server_icmp_id(0)
        , game_client_is_ipv6(false)
        , game_client_is_icmp(false)
        , game_client_partner_enable(false)
        , game_client_fec_enable(false)
        , game_client_fec_stream_key(0)
        , game_client_fec_stream_type(0)
        , game_client_fec_qos(0)
        , return_user_wan_addr(false)
        , user_pwd(0)
        , double_line_idx(0)
        , snd_pkg_sn_2_game_client(0)
        , last_transpond_time(0)
    {
        memset(&game_client_addr, 0, sizeof(game_client_addr));
        memset(&game_client_node_addr, 0, sizeof(game_client_node_addr));

        memset(&game_server_node_addr, 0, sizeof(game_server_node_addr));
        //memset(&game_server_addr, 0, sizeof(game_server_addr));
    }

    ~pair_session_info(){}

    const char* get_print_prefix(bool update = false)
    {
        if (!print_prefix.empty() && !update)
        {
            return print_prefix.c_str();
        }

        print_prefix  = /*"[" + */user_name + "]";
        print_prefix += "[" + std::to_string(user_pwd) + "]";
        print_prefix += "[" + std::to_string(double_line_idx) + "]";

        print_prefix += "[" + std::to_string(game_client_node_listener->listen_fd) + "]";
        print_prefix += "[" + std::to_string(game_server_node_fd) + "]";

        print_prefix += "[" + socket_helper::addr_to_ip_and_port(game_client_addr) + "]";
        print_prefix += "[" + socket_helper::addr_to_ip_and_port(game_client_node_addr/*game_client_node_listener->listen_addr*/) + "]";
        print_prefix += "[" + socket_helper::addr_to_ip_and_port(game_server_node_addr)/* + "]"*/;

        return print_prefix.c_str();
    }

    sockaddr_storage                     game_client_addr;
    sockaddr_storage                     game_client_node_addr;
    std::shared_ptr<listen_session_info> game_client_node_listener;

    sockaddr_storage                     game_server_node_addr;
    //sockaddr_storage                     game_server_addr;         // 不固定
    SOCKET                               game_server_node_fd;
#ifdef UDP_NONBLOCK
    std::list<data_2_game_server_t>      data_s_2_game_server;
#endif

    uint16_t                             game_client_icmp_id;      //客户端icmp id
    uint16_t                             game_server_icmp_id;      //icmp id(由节点生成)

    ////slide_win                            game_client_slide_win;
    //dly_cache                            game_client_dly_cache;
    //loss_stat                            game_client_pkg_loss_stat;

    bool                                 game_client_is_ipv6;
    bool                                 game_client_is_icmp;
    bool                                 game_client_partner_enable;
    bool                                 game_client_fec_enable;
    uint64_t                             game_client_fec_stream_key;
    uint8_t                              game_client_fec_stream_type;
    uint8_t                              game_client_fec_qos;

    bool                                 return_user_wan_addr;
    std::string                          user_name;
    uint32_t                             user_pwd;
    uint16_t                             double_line_idx;
    uint16_t                             snd_pkg_sn_2_game_client;
    time_t                               last_transpond_time;      //最近一次交互时间

    std::string                          print_prefix;
};
