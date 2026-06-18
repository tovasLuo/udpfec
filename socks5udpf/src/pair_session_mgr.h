#pragma once
#include <atomic>
#include <thread>
#include <unordered_map>
#ifdef UDP_RECV_SEND_MSG
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#endif
#include "comm_def.h"
#include "epoll_wrap.h"
#include "fec.h"
#include "pair_session_info.h"
#include "sockaddr_storage_index.h"
#include "socket_helper.h"
#include "sync_auth_info.h"

class pair_session_mgr : public epoll_wrap
{
public:
    pair_session_mgr(const console_param_t& console_param, sync_auth_info& auth_file);
    ~pair_session_mgr();

    void start();
    void run();
    void stop();
    void wait_stop();

private:
    bool init_listen_socket();
    std::vector<sockaddr_storage> get_listen_addr();
    std::shared_ptr<listen_session_info> create_listen_socket(const sockaddr_storage& listen_addr);
    static void extend_recv_send_buff(SOCKET fd);
    void handle_listen_socket_close(std::shared_ptr<listen_session_info>& listener);

    virtual void before_handle_epoll_wait();
    void clear_socket_4_timeout();
    void clear_game_client_socket_4_timeout();
    void clear_game_server_socket_4_timeout();

    virtual void handle_epoll_wait(uint32_t events, void* context);
    void handle_listen_socket_events(uint32_t events, std::shared_ptr<listen_session_info> listener);
    bool handle_listen_socket_recv(std::shared_ptr<listen_session_info>& listener);
    const char* get_print_prefix_4_client_recv(const std::shared_ptr<listen_session_info>& listener, bool update = false);

    bool handle_game_client_data(std::shared_ptr<listen_session_info>& listener);
    bool check_game_client_data_valid_4_header(std::shared_ptr<listen_session_info>& listener);
    void do_flag_echo(std::shared_ptr<listen_session_info>& listener);
    void get_local_recv_addr(sockaddr_storage& local_recv_addr);
#ifdef UDP_NONBLOCK
    void fill_msghdr(uint8_t* data, size_t data_len, const sockaddr_storage& dest_addr, const sockaddr_storage& local_send_addr);
#endif
    bool check_yb_client_cmd_valid(std::shared_ptr<listen_session_info>& listener);
    bool check_game_client_data_valid(std::shared_ptr<listen_session_info>& listener);

    std::shared_ptr<pair_session_info> get_game_client_pair_seesion(std::shared_ptr<listen_session_info>& listener);
    bool check_pair_session_info(std::shared_ptr<listen_session_info>& listener, std::shared_ptr<pair_session_info>& pair_session);
    void get_game_client_node_addr(std::shared_ptr<pair_session_info>& pair_session);
    bool create_pair_session_and_init(std::shared_ptr<listen_session_info>& listener, std::shared_ptr<pair_session_info>& pair_session);
    bool create_game_server_socket(std::shared_ptr<pair_session_info>& pair_session);

    bool process_yb_client_cmd(std::shared_ptr<pair_session_info>& pair_session);
    int do_cmd_port_check(std::shared_ptr<pair_session_info>& pair_session);
    int do_cmd_user_wan_addr(std::shared_ptr<pair_session_info>& pair_session);
    void fill_udp_base_header(std::shared_ptr<pair_session_info>& pair_session, bool is_yb_client_cmd);

    bool handle_game_client_send_immediate(std::shared_ptr<pair_session_info>& pair_session, size_t head_offset);
    void fill_udp_base_header_4_line_idx(std::shared_ptr<pair_session_info>& pair_session, size_t head_offset);
    bool handle_game_client_send_immediate_inner(std::shared_ptr<pair_session_info>& pair_session, size_t head_offset);
    bool handle_game_client_send_immediate_inner_second(std::shared_ptr<listen_session_info>& listener, sockaddr_storage& game_client_addr, sockaddr_storage& game_client_node_addr, sockaddr_storage& game_server_addr, uint8_t* data, size_t data_len, const char* print_prefix);
#ifdef UDP_NONBLOCK
    int handle_game_client_send(std::shared_ptr<listen_session_info>& listener);
#endif
    int handle_game_client_send_inner(std::shared_ptr<listen_session_info>& listener, sockaddr_storage& game_client_addr, sockaddr_storage& game_client_node_addr, sockaddr_storage& game_server_addr, uint8_t* data, size_t data_len, const char* print_prefix);
#ifdef UDP_NONBLOCK
    bool push_data_2_game_client(std::shared_ptr<listen_session_info>& listener, const sockaddr_storage& game_client_addr, sockaddr_storage& game_client_node_addr, sockaddr_storage& game_server_addr, const uint8_t* data, size_t data_len, const char* print_prefix);
#endif

    bool handle_game_server_send_immediate(std::shared_ptr<pair_session_info>& pair_session);
    void pre_process_game_client_data(std::shared_ptr<pair_session_info>& pair_session, sockaddr_storage& game_svr_addr);
#ifdef UDP_NONBLOCK
    int handle_game_server_send(std::shared_ptr<pair_session_info>& pair_session);
#endif
    int handle_game_server_send_inner(std::shared_ptr<pair_session_info>& pair_session, sockaddr_storage& game_svr_addr, uint8_t* data, size_t data_len);
#ifdef UDP_NONBLOCK
    bool push_data_2_game_server(std::shared_ptr<pair_session_info>& pair_session, const sockaddr_storage& game_svr_addr, const uint8_t* data, size_t data_len);
#endif

    void handle_game_server_events(uint32_t events, std::shared_ptr<pair_session_info> pair_session);
    bool handle_game_server_recv(std::shared_ptr<pair_session_info>& pair_session);
    void pre_process_game_server_data(std::shared_ptr<pair_session_info>& pair_session);

    void handle_game_server_close(std::shared_ptr<pair_session_info>& pair_session, bool del_from_relation = true);

private:
    const console_param_t&  console_param_;
    friend class fec;
    std::shared_ptr<fec>    fec_;
    bool                    fec_data_pkg_;
    uint64_t                fec_stream_key_;
    uint8_t                 fec_stream_type_;
    uint8_t                 fec_qos_;
    sync_auth_info&         auth_file_;
    std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>> auth_info_s_;

    std::unordered_map<SOCKET, std::shared_ptr<listen_session_info>>   listen_relation_s_;

    std::unordered_map<sockaddr_storage, std::shared_ptr<pair_session_info>> game_client_relation_s_; //加速器(游戏客户端数据)
    std::unordered_map<SOCKET, std::shared_ptr<pair_session_info>>           game_server_relation_s_; //游戏服务端

    std::string             print_prefix_4_client_recv_;
    size_t                  buf_size_;
    std::vector<uint8_t>    buf_;

    ssize_t                 data_len_;
    sockaddr_storage        data_addr_;
    socklen_t               data_addr_len_;

#ifdef UDP_RECV_SEND_MSG
    sockaddr_storage        data_node_addr_;
    msghdr msg_;
    iovec iov_;
    union
    {
#ifdef IP_RECVDSTADDR
        char in[CMSG_SPACE(sizeof(in_addr))];
#else
        char in[CMSG_SPACE(sizeof(in_pktinfo))];
#endif
        char in6[CMSG_SPACE(sizeof(in6_pktinfo))];
    } cdata_;
#endif

    timespec                old_sys_ts_;
    timespec                cur_sys_ts_;
    timespec                auth_file_update_time_;
    timespec                old_sys_ts_4_fec_;

    std::thread             thread_;
    std::atomic<bool>       stop_;
};
