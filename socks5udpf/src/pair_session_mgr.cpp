#include "pair_session_mgr.h"
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include "m_udp_proto.h"
#include "packet_checksum.h"
#include "string_helper.h"
#include "sys_log.h"

pair_session_mgr::pair_session_mgr(const console_param_t& console_param, sync_auth_info& auth_file)
    : epoll_wrap(100, 500)
    , console_param_(console_param)
    , fec_data_pkg_(false)
    , fec_stream_key_(0)
    , fec_stream_type_(0)
    , fec_qos_(0)
    , auth_file_(auth_file)
    , auth_info_s_(std::make_shared<std::unordered_map<uint32_t, auth_info_t>>())
    , buf_size_(UINT16_MAX + sizeof(udp_down6_header) + sizeof(user_wan_addr_info))
    , buf_(buf_size_, 0)
    , data_len_(0)
    , data_addr_len_(sizeof(data_addr_))
    , stop_(false)
{
    memset(&data_addr_, 0, sizeof(data_addr_));

#ifdef UDP_RECV_SEND_MSG
    memset(&data_node_addr_, 0, sizeof(data_node_addr_));
    memset(&msg_, 0, sizeof(msg_));
    memset(&iov_, 0, sizeof(iov_));
    memset(&cdata_, 0, sizeof(cdata_));
#endif

    memset(&old_sys_ts_, 0, sizeof(old_sys_ts_));
    if (clock_gettime(CLOCK_MONOTONIC, &old_sys_ts_) == -1) {}
    memset(&cur_sys_ts_, 0, sizeof(cur_sys_ts_));

    memset(&auth_file_update_time_, 0, sizeof(auth_file_update_time_));

    memset(&old_sys_ts_4_fec_, 0, sizeof(old_sys_ts_4_fec_));
    old_sys_ts_4_fec_ = old_sys_ts_;
}

pair_session_mgr::~pair_session_mgr()
{
    stop();
    wait_stop();

    while (!listen_relation_s_.empty())
    {
        handle_listen_socket_close(listen_relation_s_.begin()->second);
    }

    while (!game_server_relation_s_.empty())
    {
        handle_game_server_close(game_server_relation_s_.begin()->second);
    }
}

void pair_session_mgr::start()
{
    thread_ = std::thread(&pair_session_mgr::run, this);
}

void pair_session_mgr::stop()
{
    stop_ = true;
}

void pair_session_mgr::wait_stop()
{
    if (thread_.joinable())
    {
        thread_.join();
    }
    stop_ = true;
}

void pair_session_mgr::run()
{
#if 0
    pthread_t tid =/* pthread_self()*/thread_.native_handle();

    sched_param param{};
    //memset(&param, 0, sizeof(param));
    param.sched_priority = 99;

    int err_no = pthread_setschedparam(tid, SCHED_RR, &param);
    if (err_no != 0)
    {
        plog(LOG_WARNING, "[%d]pthread_setschedparam failed[SCHED_RR:%d][%d:%s]\n", syscall(SYS_gettid), param.sched_priority, err_no, strerror(err_no));
    }
#endif

    do
    {
        if (console_param_.fec_mode)
        {
            set_epoll_wait_timeout(1);
            fec_ = std::make_shared<fec>(this);
            if (fec_ == nullptr)
            {
                break;
            }
        }

        if (!valid())
        {
            break;
        }

        if (!init_listen_socket())
        {
            break;
        }

        if (clock_gettime(CLOCK_MONOTONIC, &old_sys_ts_) == -1) {}
        old_sys_ts_4_fec_ = old_sys_ts_;
        while (!stop_)
        {
            if (!do_epoll_wait())
            {
                //break;
            }
            //std::this_thread::yield();
        }

    } while (0);

    stop_ = true;
    fec_ = nullptr;
}

bool pair_session_mgr::init_listen_socket()
{
    auto addrs = get_listen_addr();
    if (addrs.empty())
    {
        plog(LOG_ERR, "[][][][-1][][][%s:%s][][][listen]get_listen_addr empty\n", console_param_.bind_host.c_str(), console_param_.bind_port.c_str());
        return false;
    }

    for (const auto& addr : addrs)
    {
        auto listener = create_listen_socket(addr);
        if (listener == nullptr)
        {
            continue;
        }

        listen_relation_s_[listener->listen_fd] = listener;
    }

    if (listen_relation_s_.empty())
    {
        plog(LOG_ERR, "[][][][-1][][][%s:%s][][][listen]init_listen_socket failed\n", console_param_.bind_host.c_str(), console_param_.bind_port.c_str());
        return false;
    }

    return true;
}

std::vector<sockaddr_storage> pair_session_mgr::get_listen_addr()
{
    std::vector<sockaddr_storage> addrs;

    std::vector<std::string> host_s = string_helperA::split(console_param_.bind_host, ',', false);
    if (host_s.empty())
    {
        host_s.push_back(std::string());
    }

    std::vector<std::string> port_s = string_helperA::split(console_param_.bind_port, ',', false);
    assert(!port_s.empty());

    for (auto it_host = host_s.begin(); it_host != host_s.end(); ++it_host)
    {
        for (auto it_port = port_s.begin(); it_port != port_s.end(); ++it_port)
        {
            auto item_addrs = socket_helper::get_addr_info(*it_host, *it_port, AF_UNSPEC, AI_PASSIVE, SOCK_STREAM);
            if (item_addrs.empty())
            {
                plog(LOG_ERR, "[][][][-1][][][%s:%s][][][listen]get_addr_info failed\n", it_host->c_str(), it_port->c_str());
                continue;
            }

            addrs.insert(addrs.end(), item_addrs.begin(), item_addrs.end());
        }
    }

    return addrs;
}

std::shared_ptr<listen_session_info> pair_session_mgr::create_listen_socket(const sockaddr_storage& listen_addr)
{
    auto listener = std::make_shared<listen_session_info>();
    listener->epoll_ptr = this;
    listener->listen_addr = listen_addr;
    listener->get_print_prefix(true);

    SOCKET& sock = listener->listen_fd;

    sock = socket_helper::create_socket(listen_addr.ss_family, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        plog(LOG_ERR, "[%s][listen]socket create failed[%d:%s]\n", listener->get_print_prefix(), errno, strerror(errno));
        return nullptr;
    }
    listener->get_print_prefix(true);

    do
    {
//#ifdef UDP_NONBLOCK
//        socket_helper::set_non_block(sock);
//#endif
        socket_helper::set_reuse_addr(sock);
        socket_helper::set_reuse_port(sock);
        //socket_helper::set_rcv_timeout(sock, 10);
        //socket_helper::set_snd_timeout(sock, 10);
#ifdef UDP_RECV_SEND_MSG
        socket_helper::set_ip_pktinfo(sock, listen_addr.ss_family);
#endif

        if (bind(sock, (sockaddr*)&listen_addr, sizeof(listen_addr)) == SOCKET_ERROR)
        {
            plog(LOG_ERR, "[%s][listen]bind failed[%d:%s]\n", listener->get_print_prefix(), errno, strerror(errno));
            break;
        }

        sockaddr_storage listen_local_addr{};
        //memset(&listen_local_addr, 0, sizeof(listen_local_addr));
        if (!socket_helper::get_local_addr_by_fd(sock, listen_local_addr))
        {
            plog(LOG_ERR, "[%s][listen]get_local_addr_by_fd failed[%d:%s]\n", listener->get_print_prefix(), errno, strerror(errno));
            break;
        }
        listener->listen_addr = listen_local_addr;
        listener->get_print_prefix(true);

        //extend_recv_send_buff(sock);

        if (!reg_event(sock, EPOLLIN))
        {
            plog(LOG_WARNING, "[%s][listen]reg_event failed[%d:%s]\n", listener->get_print_prefix(), errno, strerror(errno));
            break;
        }

        plog(LOG_INFO, "[%s][listen]socket create succeed\n", listener->get_print_prefix());

        return listener;
    } while (0);

    plog(LOG_INFO, "[%s][listen]socket will be close\n", listener->get_print_prefix());

    socket_helper::close_socket(sock);
    return nullptr;
}

void pair_session_mgr::extend_recv_send_buff(SOCKET fd)
{
    int send_buf_size = socket_helper::get_snd_buf_size(fd);
    if (send_buf_size <= 0)
    {
        send_buf_size = 1024 * 1000;
    }
    send_buf_size *= 4;
    socket_helper::set_snd_buf_size(fd, send_buf_size);

    int recv_buf_size = socket_helper::get_rcv_buf_size(fd);
    if (recv_buf_size <= 0)
    {
        recv_buf_size = 1024 * 1000;
    }
    recv_buf_size *= 4;
    socket_helper::set_rcv_buf_size(fd, recv_buf_size);
}

void pair_session_mgr::before_handle_epoll_wait()
{
    if (clock_gettime(CLOCK_MONOTONIC, &cur_sys_ts_) == -1) {}

    if (console_param_.fec_mode)
    {
        time_t delta_4_fec_ms = (1000000000 + cur_sys_ts_.tv_nsec - old_sys_ts_4_fec_.tv_nsec) / 1000000;
        delta_4_fec_ms += (cur_sys_ts_.tv_sec - 1 - old_sys_ts_4_fec_.tv_sec) * 1000;
        if (delta_4_fec_ms >= 1)
        {
            fec_->fec_wheel();
            old_sys_ts_4_fec_ = cur_sys_ts_;
        }
    }

    do
    {
        if (auth_file_.update_auth_info_s_custom(auth_info_s_, auth_file_update_time_))
        {
            break;
        }

        time_t delta_s = (1000000000 + cur_sys_ts_.tv_nsec - old_sys_ts_.tv_nsec) / 1000000000;
        delta_s += cur_sys_ts_.tv_sec - 1 - old_sys_ts_.tv_sec;
        if (delta_s >= (console_param_.sock_timeout_s + 1) / 2)
        {
            break;
        }

        return;
    } while (0);

    clear_socket_4_timeout();
    old_sys_ts_ = cur_sys_ts_;
}

void pair_session_mgr::clear_socket_4_timeout()
{
    clear_game_server_socket_4_timeout();
    clear_game_client_socket_4_timeout();
}

void pair_session_mgr::clear_game_server_socket_4_timeout()
{
    for (auto it = game_server_relation_s_.begin(); it != game_server_relation_s_.end();)
    {
        std::shared_ptr<pair_session_info>& pair_session = it->second;

        bool is_ok = false;
        do
        {
            if (cur_sys_ts_.tv_sec - pair_session->last_transpond_time >= console_param_.sock_timeout_s)
            {
                plog(LOG_DEBUG, "[%s][][server]last transpond timeout [%ld]s\n", pair_session->get_print_prefix(), console_param_.sock_timeout_s);
                break;
            }

            assert(!pair_session->user_name.empty());
            if (!auth_file_.auth_pwd_and_user_name_custom(auth_info_s_, pair_session->user_pwd, pair_session->user_name))
            {
                plog(LOG_ERR, "[%s][][client]pass word not find for clear[%u]\n", pair_session->get_print_prefix(), pair_session->user_pwd);
                break;
            }

            is_ok = true;
        } while (0);

        if (is_ok)
        {
            ++it;
            continue;
        }

        if (console_param_.fec_mode)
        {
            fec_->del_fec_session(pair_session);
        }
        game_client_relation_s_.erase(pair_session->game_client_addr);

        handle_game_server_close(pair_session, false);
        it = game_server_relation_s_.erase(it);
    }
}

void pair_session_mgr::clear_game_client_socket_4_timeout()
{
    for (auto it = game_client_relation_s_.begin(); it != game_client_relation_s_.end();)
    {
        std::shared_ptr<pair_session_info>& pair_session = it->second;
        if (pair_session->game_server_node_fd == INVALID_SOCKET)
        {
            ++it;
            continue;
        }

        bool is_ok = false;
        do
        {
            if (cur_sys_ts_.tv_sec - pair_session->last_transpond_time >= console_param_.sock_timeout_s)
            {
                plog(LOG_DEBUG, "[%s][][client]last transpond timeout [%ld]s\n", pair_session->get_print_prefix(), console_param_.sock_timeout_s);
                break;
            }

            assert(!pair_session->user_name.empty());
            if (!auth_file_.auth_pwd_and_user_name_custom(auth_info_s_, pair_session->user_pwd, pair_session->user_name))
            {
                plog(LOG_ERR, "[%s][][client]pass word not find for clear[%u]\n", pair_session->get_print_prefix(), pair_session->user_pwd);
                break;
            }

            is_ok = true;
        } while (0);

        if (is_ok)
        {
            ++it;
            continue;
        }

        if (console_param_.fec_mode)
        {
            fec_->del_fec_session(pair_session);
        }

        it = game_client_relation_s_.erase(it);
    }
}

void pair_session_mgr::handle_epoll_wait(uint32_t events, void* context)
{
    SOCKET sock = (SOCKET)(long)context;

    auto it_listen = listen_relation_s_.find(sock);
    if (it_listen != listen_relation_s_.end())
    {
        handle_listen_socket_events(events, it_listen->second);
        return;
    }

    auto it_server = game_server_relation_s_.find(sock);
    if (it_server != game_server_relation_s_.end())
    {
        handle_game_server_events(events, it_server->second);
        return;
    }
}

void pair_session_mgr::handle_listen_socket_events(uint32_t events, std::shared_ptr<listen_session_info> listener)
{
    if (events & (EPOLLIN | EPOLLHUP | EPOLLERR))
    {
        handle_listen_socket_recv(listener);
    }

#ifdef UDP_NONBLOCK
    if (events & EPOLLOUT)
    {
        handle_game_client_send(listener);
    }
#endif
}

bool pair_session_mgr::handle_listen_socket_recv(std::shared_ptr<listen_session_info>& listener)
{
    while (!stop_)
    {
        memset(&data_addr_, 0, sizeof(data_addr_));
        //memset(buf_.data(), 0, buf_size_);

#ifdef UDP_RECV_SEND_MSG
        iov_.iov_base = buf_.data();
        iov_.iov_len = buf_size_;

        memset(&msg_, 0, sizeof(msg_));
        msg_.msg_name = &data_addr_;
        msg_.msg_namelen = (socklen_t)sizeof(data_addr_);
        msg_.msg_iov = &iov_;
        msg_.msg_iovlen = 1;
        msg_.msg_control = &cdata_;
        msg_.msg_controllen = sizeof(cdata_);

        data_len_ = recvmsg(listener->listen_fd, &msg_, 0);
#else
        data_len_ = recvfrom(listener->listen_fd, buf_.data(), buf_size_, 0, (sockaddr*)&data_addr_, &data_addr_len_);
#endif
        if (data_len_ == SOCKET_ERROR)
        {
            int err_no = socket_helper::get_errno();
            if (err_no == EINTR)
            {
                continue;
            }

            if (err_no == EAGAIN/*EWOULDBLOCK*/)
            {
                return true;
            }

            plog(LOG_ERR, "[][][][%d][][%s][%s][][][client]recvfrom failed[%d:%s]\n", listener->listen_fd, socket_helper::addr_to_ip_and_port(data_addr_).c_str(), socket_helper::addr_to_ip_and_port(listener->listen_addr).c_str(), err_no, strerror(err_no));
            return true;
            //return false;
        }

#ifdef UDP_RECV_SEND_MSG
        data_addr_len_ = msg_.msg_namelen;
        get_local_recv_addr(data_node_addr_);
        socket_helper::port_to_addr(socket_helper::port_from_addr(listener->listen_addr), data_node_addr_);
#endif
        fec_stream_key_ = 0;
        fec_stream_type_ = 0;
        fec_qos_ = 0;
        fec_data_pkg_ = console_param_.fec_mode && data_len_ > 0 && fec_->is_fec_pkg(buf_.data(), data_len_, fec_stream_key_);

        get_print_prefix_4_client_recv(listener, true);
        plog(LOG_DEBUG, "[%s][][client]recvfrom [%d]\n", get_print_prefix_4_client_recv(listener), data_len_);

        if (fec_data_pkg_)
        {
            (void)fec_->recv_fec_pdu(listener);
        }
        else
        {
            /*return*/ (void)handle_game_client_data(listener);
        }
        //break;
    }

    return true;
}

const char* pair_session_mgr::get_print_prefix_4_client_recv(const std::shared_ptr<listen_session_info>& listener, bool update/* = false*/)
{
    assert(listener != nullptr);

    auto it = game_client_relation_s_.find(data_addr_);
    if (it != game_client_relation_s_.end())
    {
        return it->second->get_print_prefix(/*update*/);
    }

    if (update || print_prefix_4_client_recv_.empty())
    {
        print_prefix_4_client_recv_  = /*"[" + */std::string("]");
        print_prefix_4_client_recv_ += "[]";
        print_prefix_4_client_recv_ += "[]";
        print_prefix_4_client_recv_ += "[" + std::to_string(listener->listen_fd) + "]";
        print_prefix_4_client_recv_ += "[]";
        print_prefix_4_client_recv_ += "[" + socket_helper::addr_to_ip_and_port(data_addr_) + "]";
        print_prefix_4_client_recv_ += "[" + socket_helper::addr_to_ip_and_port(data_node_addr_) + "]";
        //print_prefix_4_client_recv_ += "[]";
        print_prefix_4_client_recv_ += std::string("[")/* + "]"*/;
    }

    return print_prefix_4_client_recv_.c_str();
}

bool pair_session_mgr::handle_game_client_data(std::shared_ptr<listen_session_info>& listener)
{
    if (!check_game_client_data_valid_4_header(listener))
    {
        return false;
    }

    auto pair_session = get_game_client_pair_seesion(listener);
    if (pair_session == nullptr)
    {
        return false;
    }

    pair_session->return_user_wan_addr = ((m_udp_base_header*)buf_.data())->rt_user_wan_addr == M_UDP_FLAG_RETURN_USER_WAN_ADDR_ENABLE;
    pair_session->last_transpond_time = cur_sys_ts_.tv_sec;
    if (fec_stream_key_ != 0) {
        pair_session->game_client_fec_stream_key = fec_stream_key_;
        pair_session->game_client_fec_stream_type = fec_stream_type_;
        pair_session->game_client_fec_qos = fec_qos_;
    }

    m_udp_up_base_header* header = (m_udp_up_base_header*)buf_.data();
    if (header->pkg_type == M_UDP_FLAG_PKG_TYPE_CONTROL)
    {
        return process_yb_client_cmd(pair_session);
    }

    if (!handle_game_server_send_immediate(pair_session))
    {
        //game_client_relation_s_.erase(pair_session->game_client_addr);
        handle_game_server_close(pair_session);
        return false;
    }

    return true;
}

bool pair_session_mgr::check_game_client_data_valid_4_header(std::shared_ptr<listen_session_info>& listener)
{
    bool len_ok = false;
    do
    {
        if (data_len_ < 2)
        {
            break;
        }

        //丢弃客户端发送的长度为2个字节的假包 内容为"0"
        if (data_len_ == 2 && buf_.data()[0] == '0' && buf_.data()[1] == '\0')
        {
            return false;
        }

        if (ntohs(*(uint16_t*)buf_.data()) == M_UDP_FLAG_ECHO_HEADER)
        {
            do_flag_echo(listener);
            return false;
        }

        if (data_len_ < (uint16_t)sizeof(m_udp_up_base_header))
        {
            break;
        }

        if (ntohs(*(uint16_t*)buf_.data()) != M_UDP_FLAG_HEADER)
        {
            plog(LOG_ERR, "[%s][][client]data header flag abnormal[0x%04x!=0x%04x]\n", get_print_prefix_4_client_recv(listener), ntohs(*(uint16_t*)buf_.data()), M_UDP_FLAG_HEADER);
            return false;
        }

        m_udp_up_base_header* header = (m_udp_up_base_header*)buf_.data();
        if (header->pkg_type == M_UDP_FLAG_PKG_TYPE_CONTROL && !check_yb_client_cmd_valid(listener))
        {
            return false;
        }

        if (header->pkg_type == M_UDP_FLAG_PKG_TYPE_BUSINESS && !check_game_client_data_valid(listener))
        {
            return false;
        }

        len_ok = true;
    } while (0);

    if (!len_ok)
    {
        plog(LOG_ERR, "[%s][][client]recvfrom len abnormal\n", get_print_prefix_4_client_recv(listener));
        return false;
    }

    m_udp_up_base_header* header = (m_udp_up_base_header*)buf_.data();
#if 0
    if (ntohs(header->pkg_len) != data_len_)
    {
        plog(LOG_ERR, "[%s][][client]data pkg_len abnormal\n", get_print_prefix_4_client_recv(listener));
        return false;
    }

    uint16_t check_sum = packet_checksum::ip_header_check_sum(buf_.data(), (uint16_t)data_len_);
    if (check_sum != UINT16_MAX)
    {
        plog(LOG_ERR, "[%s][][client]data checksum abnormal[0x%04x][0xffff]\n", get_print_prefix_4_client_recv(listener), check_sum);
        return false;
    }
#endif

#if 1
    uint32_t user_pwd = ntohl(header->user_password);
    if (!auth_file_.auth_pwd_custom(auth_info_s_, user_pwd))
    {
        plog(LOG_ERR, "[%s][][client]pass word not find[%u]\n", get_print_prefix_4_client_recv(listener), user_pwd);
        return false;
    }
#endif

    return true;
}

void pair_session_mgr::do_flag_echo(std::shared_ptr<listen_session_info>& listener)
{
    if (console_param_.fec_mode && fec_data_pkg_)
    {
        (void)fec_->send_pdu(listener);
        return;
    }

    static sockaddr_storage game_server_addr{};
    //memset(&game_server_addr, 0, sizeof(game_server_addr));
    (void)handle_game_client_send_immediate_inner_second(listener, data_addr_, data_node_addr_, game_server_addr, buf_.data(), (size_t)data_len_, get_print_prefix_4_client_recv(listener));
}

void pair_session_mgr::get_local_recv_addr(sockaddr_storage& local_recv_addr)
{
#ifdef UDP_RECV_SEND_MSG
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg_); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg_, cmsg))
    {
#ifdef IP_RECVDSTADDR
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_RECVDSTADDR)
        {
            in_addr* addrinfo = (in_addr*)CMSG_DATA(cmsg);
            ((sockaddr_in*)&local_recv_addr)->sin_addr = *addrinfo;
            ((sockaddr_in*)&local_recv_addr)->sin_family = AF_INET;
        }
#else
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO)
        {
            in_pktinfo* pktinfo = (in_pktinfo*)CMSG_DATA(cmsg);
            ((sockaddr_in*)&local_recv_addr)->sin_addr = pktinfo->ipi_addr;
            ((sockaddr_in*)&local_recv_addr)->sin_family = AF_INET;
        }
#endif
        else if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO)
        {
            in6_pktinfo* pktinfo = (in6_pktinfo*)CMSG_DATA(cmsg);
            ((sockaddr_in6*)&local_recv_addr)->sin6_addr = pktinfo->ipi6_addr;
            ((sockaddr_in6*)&local_recv_addr)->sin6_family = AF_INET6;
        }
    }
#endif
}

#ifdef UDP_NONBLOCK
void pair_session_mgr::fill_msghdr(uint8_t* data, size_t data_len, const sockaddr_storage& dest_addr, const sockaddr_storage& local_send_addr)
{
    iov_.iov_base = data;
    iov_.iov_len = data_len;

    socklen_t addr_len = dest_addr.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    memset(&msg_, 0, sizeof(msg_));
    msg_.msg_name = (void*)&dest_addr;
    msg_.msg_namelen = addr_len;
    msg_.msg_iov = &iov_;
    msg_.msg_iovlen = 1;
    msg_.msg_control = &cdata_;
    msg_.msg_controllen = sizeof(cdata_);

    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg_);

    size_t controllen = 0;
    switch (local_send_addr.ss_family)
    {
    case AF_INET:
    {
#ifdef IP_SENDSRCADDR
        memset(cmsg, 0, CMSG_SPACE(sizeof(in_addr)));
        cmsg->cmsg_level = IPPROTO_IP;
        cmsg->cmsg_type = IP_SENDSRCADDR;
        cmsg->cmsg_len = CMSG_LEN(sizeof(in_addr));
        in_addr* addrinfo = (in_addr*)CMSG_DATA(cmsg);
        addrinfo->s_addr = ((sockaddr_in*)&local_send_addr)->sin_addr->s_addr;
        controllen += CMSG_SPACE(sizeof(in_addr));
#else
        memset(cmsg, 0, CMSG_SPACE(sizeof(in_pktinfo)));
        cmsg->cmsg_level = IPPROTO_IP;
        cmsg->cmsg_type = IP_PKTINFO;
        cmsg->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
        in_pktinfo* pktinfo = (in_pktinfo*)CMSG_DATA(cmsg);
        pktinfo->ipi_spec_dst = ((sockaddr_in*)&local_send_addr)->sin_addr;
        controllen += CMSG_SPACE(sizeof(in_pktinfo));
#endif
    } break;

    case AF_INET6:
    {
        memset(cmsg, 0, CMSG_SPACE(sizeof(in6_pktinfo)));
        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_PKTINFO;
        cmsg->cmsg_len = CMSG_LEN(sizeof(in6_pktinfo));
        in6_pktinfo* pktinfo = (in6_pktinfo*)CMSG_DATA(cmsg);
        memcpy(&pktinfo->ipi6_addr, &((sockaddr_in6*)&local_send_addr)->sin6_addr, sizeof(pktinfo->ipi6_addr));
        controllen += CMSG_SPACE(sizeof(in6_pktinfo));
    } break;
    }

    msg_.msg_controllen = controllen;
    if (msg_.msg_controllen == 0)
    {
        msg_.msg_control = NULL;
    }
}
#endif

bool pair_session_mgr::check_yb_client_cmd_valid(std::shared_ptr<listen_session_info>& listener)
{
    //udp_cmd_up_header* header =(udp_cmd_up_header*)buf_.data();
    if (data_len_ < (uint16_t)sizeof(udp_cmd_up_header))
    {
        plog(LOG_ERR, "[%s][][client]recvfrom len abnormal for cmd\n", get_print_prefix_4_client_recv(listener));
        return false;
    }

    return true;
}

bool pair_session_mgr::check_game_client_data_valid(std::shared_ptr<listen_session_info>& listener)
{
    bool len_ok = false;
    do
    {
        m_udp_up_base_header* header = (m_udp_up_base_header*)buf_.data();
        assert(header->pkg_type == M_UDP_FLAG_PKG_TYPE_BUSINESS);

        //return check_yb_client_data_valid(header, listener);
        bool is_ipv6 = header->ip_type == M_UDP_FLAG_IP_TYPE_IPV6;
        size_t game_data_offset = is_ipv6 ? sizeof(udp_up6_header) : sizeof(udp_up4_header);

        if (data_len_ < (uint16_t)game_data_offset)
        {
            break;
        }

        //由于字节对齐，ipv4，ipv6的double_line_idx, user_passwords属性位置偏移相同，
        //因此统一用udp_up4_header进行转换
        udp_up4_header* up4_header = (udp_up4_header*)buf_.data();
        if (ntohs(up4_header->game_svr_port) == 0)
        {
            //icmphdr和icmp6_hdr长度相同
            if (data_len_ < (uint16_t)(game_data_offset + sizeof(icmphdr)))
            {
                break;
            }
        }

        len_ok = true;
    } while (0);

    if (!len_ok)
    {
        plog(LOG_ERR, "[%s][][client]recvfrom len abnormal\n", get_print_prefix_4_client_recv(listener));
        return false;
    }

    return true;
}

std::shared_ptr<pair_session_info> pair_session_mgr::get_game_client_pair_seesion(std::shared_ptr<listen_session_info>& listener)
{
    //m_udp_up_base_header* header = (m_udp_up_base_header*)buf_.data();
    std::shared_ptr<pair_session_info> pair_session = nullptr;

    do
    {
        auto pos = game_client_relation_s_.find(data_addr_);
        if (pos == game_client_relation_s_.end())
        {
            break;
        }

        pair_session = pos->second;
        if (check_pair_session_info(listener, pair_session))
        {
            break;
        }

        if (pair_session->game_server_node_fd != INVALID_SOCKET)
        {
            handle_game_server_close(pair_session);
        }

        game_client_relation_s_.erase(pos);
        pair_session = nullptr;
    } while (0);

    if (pair_session != nullptr)
    {
        pair_session->game_client_fec_enable = fec_data_pkg_;
        pair_session->game_client_node_addr = data_node_addr_;
        //get_game_client_node_addr(pair_session);
        pair_session->get_print_prefix(true);
#if 0
        //pair_session->game_client_slide_win.pkg_sn_valid(ntohs(header->pkg_sn));
        pair_session->game_client_pkg_loss_stat.cache_pkg_sn(ntohs(header->pkg_sn), cur_sys_ts_);
        pair_session->game_client_dly_cache.update_start_time(cur_sys_ts_, ntohs(header->pkg_start_time));
#endif
        return pair_session;
    }

    pair_session = std::make_shared<pair_session_info>();
    bool init_ok = create_pair_session_and_init(listener, pair_session);
    if (!init_ok)
    {
        pair_session = nullptr;
    }

    if (pair_session != nullptr)
    {
        //game_client_relation_s_.insert(std::make_pair(data_addr_, pair_session));
        game_client_relation_s_[data_addr_] = pair_session;
    }

    return pair_session;
}

bool pair_session_mgr::check_pair_session_info(std::shared_ptr<listen_session_info>& listener, std::shared_ptr<pair_session_info>& pair_session)
{
    udp_up4_header* header = (udp_up4_header*)buf_.data();
    bool is_ipv6 = header->ip_type == M_UDP_FLAG_IP_TYPE_IPV6;
    bool is_icmp = pair_session->game_client_is_icmp;
    if (header->pkg_type == M_UDP_FLAG_PKG_TYPE_BUSINESS)
    {
        is_icmp = ntohs(header->game_svr_port) == 0;
    }
    bool is_double_line = header->double_line == M_UDP_FLAG_IP_DOUBLE_ENABLE;

    uint16_t org_icmp_id = pair_session->game_client_icmp_id;
    if (header->pkg_type == M_UDP_FLAG_PKG_TYPE_BUSINESS && is_icmp)
    {
        size_t game_data_offset = is_ipv6 ? sizeof(udp_up6_header) : sizeof(udp_up4_header);
        icmphdr* hdr = (icmphdr*)(buf_.data() + game_data_offset);
        org_icmp_id = ntohs(hdr->un.echo.id);
    }

    bool print_prefix_update = false;
    do
    {
        if (pair_session->game_server_node_fd == INVALID_SOCKET)
        {
            plog(LOG_WARNING, "[%s][][client]old game server invalid\n", pair_session->get_print_prefix());
            break;
        }

        if (pair_session->game_client_is_icmp != is_icmp)
        {
            plog(LOG_WARNING, "[%s][][client]data type changed[%s:%s]\n", pair_session->get_print_prefix(), pair_session->game_client_is_icmp ? "icmp" : "udp", is_icmp ? "icmp" : "udp");
            break;
        }

        if (pair_session->game_client_is_ipv6 != is_ipv6)
        {
            plog(LOG_WARNING, "[%s][][client]ip type changed[%s:%s]\n", pair_session->get_print_prefix(), pair_session->game_client_is_ipv6 ? "ipv6" : "ipv4", is_ipv6 ? "ipv6" : "ipv4");
            break;
        }

        if (pair_session->user_pwd != ntohl(header->user_password))
        {
            plog(LOG_WARNING, "[%s][][client]user password changed[%u:%u]\n", pair_session->get_print_prefix(), pair_session->user_pwd, ntohl(header->user_password));
            break;
        }

        if (pair_session->game_client_node_listener->listen_fd != listener->listen_fd)
        {
            plog(LOG_WARNING, "[%s][][client]listen addr changed[%d:%d][%s:%s]\n", pair_session->get_print_prefix(),
                pair_session->game_client_node_listener->listen_fd, listener->listen_fd,
                socket_helper::addr_to_ip_and_port(pair_session->game_client_node_listener->listen_addr).c_str(), socket_helper::addr_to_ip_and_port(listener->listen_addr).c_str());
            //pair_session->game_client_node_listener = listener;
            //print_prefix_update = true;
            break;
        }

        if (pair_session->double_line_idx != ntohs(header->double_line_idx))
        {
            plog(LOG_WARNING, "[%s][][client]double line idx changed[%hu:%hu]\n", pair_session->get_print_prefix(), pair_session->double_line_idx, ntohs(header->double_line_idx));
            //pair_session->double_line_idx = ntohs(header->double_line_idx);
            //print_prefix_update = true;
            break;
        }

        if (pair_session->game_client_icmp_id != org_icmp_id)
        {
            plog(LOG_WARNING, "[%s][][client]icmp id changed[%hu:%hu]\n", pair_session->get_print_prefix(), pair_session->game_client_icmp_id, org_icmp_id);
            break;
        }

        if (pair_session->game_client_partner_enable != is_double_line)
        {
            plog(LOG_WARNING, "[%s][][client]double line type changed[%s:%s]\n", pair_session->get_print_prefix(), pair_session->game_client_partner_enable ? "enable" : "disable", is_double_line ? "enable" : "disable");
            //pair_session->game_client_partner_enable = is_double_line;
            //print_prefix_update = true;
            break;
        }

        if (print_prefix_update)
        {
            pair_session->data_s_2_game_server.clear();
#if 0
            pair_session->game_client_pkg_loss_stat.reset();
#endif
            pair_session->get_print_prefix(true);
        }

        return true;
    } while (0);

    return false;
}

void pair_session_mgr::get_game_client_node_addr(std::shared_ptr<pair_session_info>& pair_session)
{
    pair_session->game_client_node_addr = data_node_addr_;
}

bool pair_session_mgr::create_pair_session_and_init(std::shared_ptr<listen_session_info>& listener, std::shared_ptr<pair_session_info>& pair_session)
{
    udp_up4_header* header = (udp_up4_header*)buf_.data();
    bool is_ipv6 = header->ip_type == M_UDP_FLAG_IP_TYPE_IPV6;
    bool is_icmp = false;
    if (header->pkg_type == M_UDP_FLAG_PKG_TYPE_BUSINESS)
    {
        is_icmp = ntohs(header->game_svr_port) == 0;
    }

    pair_session->game_client_partner_enable = header->double_line == M_UDP_FLAG_IP_DOUBLE_ENABLE;
    pair_session->game_client_is_ipv6 = is_ipv6;
    pair_session->game_client_is_icmp = is_icmp;
    pair_session->double_line_idx = ntohs(header->double_line_idx);
    pair_session->user_pwd = ntohl(header->user_password);

    pair_session->game_client_addr = data_addr_;
    //pair_session->game_client_node_addr = listener->listen_addr;
    pair_session->game_client_node_listener = listener;
    //get_game_client_node_addr(pair_session);
    pair_session->game_client_node_addr = data_node_addr_;
    pair_session->game_client_fec_enable = fec_data_pkg_;

    if (!create_game_server_socket(pair_session))
    {
        return false;
    }

#if 0
    //pair_session->game_client_slide_win.pkg_sn_valid(ntohs(header->pkg_sn));
    pair_session->game_client_pkg_loss_stat.cache_pkg_sn(ntohs(header->pkg_sn), cur_sys_ts_);
    pair_session->game_client_dly_cache.update_start_time(cur_sys_ts_, ntohs(header->pkg_start_time));
#endif

    if (is_icmp)
    {
        size_t game_data_offset = is_ipv6 ? sizeof(udp_up6_header) : sizeof(udp_up4_header);
        icmphdr* hdr = (icmphdr*)(buf_.data() + game_data_offset);

        pair_session->game_client_icmp_id = ntohs(hdr->un.echo.id);
        pair_session->game_server_icmp_id = socket_helper::port_from_addr(pair_session->game_server_node_addr);
        //pair_session->game_server_icmp_id = ntohs(*(uint16_t*)&pair_session->game_server_node_addr + 1);
    }

    //game_server_relation_s_.insert(std::make_pair(pair_session->game_server_node_fd, pair_session));
    game_server_relation_s_[pair_session->game_server_node_fd] = pair_session;
    return true;
}

bool pair_session_mgr::create_game_server_socket(std::shared_ptr<pair_session_info>& pair_session)
{
    bool is_ok = auth_file_.get_bind_addr_and_user_name_custom(auth_info_s_, pair_session->user_pwd, pair_session->game_client_is_ipv6,
        pair_session->game_server_node_addr, pair_session->user_name);
    pair_session->get_print_prefix(true);
    if (!is_ok)
    {
        plog(LOG_ERR, "[%s][][server]user was not found while get the addr\n", pair_session->get_print_prefix());
        return false;
    }

    plog(LOG_INFO, "[%s][][server]try create socket\n", pair_session->get_print_prefix());

    SOCKET& sock = pair_session->game_server_node_fd;

    int protocol = IPPROTO_UDP;
    if (pair_session->game_client_is_icmp)
    {
        protocol = IPPROTO_ICMP;
        if (pair_session->game_client_is_ipv6)
        {
            protocol = IPPROTO_ICMPV6;
        }
    }

    sock = socket_helper::create_socket(pair_session->game_client_is_ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, protocol
        /*pair_session->game_client_is_icmp ? (pair_session->game_client_is_ipv6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP) : IPPROTO_UDP*/);
    if (sock == INVALID_SOCKET)
    {
        plog(LOG_ERR, "[%s][][server]socket create failed[%d:%s]\n", pair_session->get_print_prefix(), errno, strerror(errno));
        return false;
    }
    pair_session->get_print_prefix(true);

    do
    {
        //socket_helper::set_non_block(sock);
        socket_helper::set_reuse_addr(sock);
        //socket_helper::set_reuse_port(sock);
        //socket_helper::set_rcv_timeout(sock, 10);
        //socket_helper::set_snd_timeout(sock, 10);
#ifdef UDP_RECV_SEND_MSG
        socket_helper::set_ip_pktinfo(sock, pair_session->game_client_is_ipv6 ? AF_INET6 : AF_INET);
#endif

        if (bind(sock, (sockaddr*)&pair_session->game_server_node_addr, sizeof(pair_session->game_server_node_addr)) == SOCKET_ERROR)
        {
            plog(LOG_ERR, "[%s][][server]bind failed[%d:%s]\n", pair_session->get_print_prefix(), errno, strerror(errno));
            break;
        }

        sockaddr_storage local_addr{};
        //memset(&local_addr, 0, sizeof(local_addr));
        if (!socket_helper::get_local_addr_by_fd(sock, local_addr))
        {
            plog(LOG_ERR, "[%s][][server]get_local_addr_by_fd failed[%d:%s]\n", pair_session->get_print_prefix(), errno, strerror(errno));
            break;
        }
        pair_session->game_server_node_addr = local_addr;
        pair_session->get_print_prefix(true);

        //extend_recv_send_buff(sock);

        if (!reg_event(sock, EPOLLIN))
        {
            plog(LOG_WARNING, "[%s][][server]reg_event failed[%d:%s]\n", pair_session->get_print_prefix(), errno, strerror(errno));
            break;
        }

        plog(LOG_INFO, "[%s][][server]socket create succeed\n", pair_session->get_print_prefix());

        return true;
    } while (0);

    plog(LOG_INFO, "[%s][][server]socket will be close\n", pair_session->get_print_prefix());

    socket_helper::close_socket(sock);
    return false;
}

bool pair_session_mgr::process_yb_client_cmd(std::shared_ptr<pair_session_info>& pair_session)
{
    int do_ret_val = -1;
    udp_cmd_up_header* header = (udp_cmd_up_header*)buf_.data();
    switch (ntohs(header->command))
    {
    case k_udp_cmd_port_check:
        do_ret_val = do_cmd_port_check(pair_session);
        break;
    case k_udp_cmd_user_wan_addr:
        do_ret_val = do_cmd_user_wan_addr(pair_session);
        break;
    }

    if (do_ret_val == -1)
    {
        return false;
    }

    if (do_ret_val == 0)
    {
        return true;
    }

    assert(do_ret_val == 1);

    udp_cmd_down_header* down_header = (udp_cmd_down_header*)buf_.data();
    fill_udp_base_header(pair_session, true);
    size_t user_wan_addr_size = (pair_session->return_user_wan_addr || down_header->command == htons(k_udp_cmd_user_wan_addr)) ? (sizeof(user_wan_addr_info) - (pair_session->game_client_addr.ss_family == AF_INET6 ? 0 : sizeof(in6_addr) - sizeof(in_addr))) : 0;
    data_len_ += (ssize_t)(sizeof(udp_cmd_down_header) + user_wan_addr_size);

    down_header->checksum = 0;
#if 0
    down_header->checksum = packet_checksum::ip_header_check_sum(buf_.data(), (uint16_t)data_len_);
#endif

    fill_udp_base_header_4_line_idx(pair_session, 0);
    if (/*console_param_.fec_mode && */pair_session->game_client_fec_enable)
    {
        fec_->send_pdu(pair_session, 0);
    }
    else
    {
        handle_game_client_send_immediate(pair_session, 0);
    }

    return true;
}

int pair_session_mgr::do_cmd_port_check(std::shared_ptr<pair_session_info>& pair_session)
{
#if 0
    if (data_len_ < (uint16_t)sizeof(udp_cmd_port_check))
    {
        plog(LOG_ERR, "[%s][][client]recvfrom len abnormal for cmd: udp_cmd_port_check\n", pair_session->get_print_prefix());
        return -1;
    }
#endif

#if 1
    if (data_len_ - (ssize_t)sizeof(udp_cmd_up_header) > 0)
    {
        size_t user_wan_addr_size = pair_session->return_user_wan_addr ? (sizeof(user_wan_addr_info) - (pair_session->game_client_addr.ss_family == AF_INET6 ? 0 : sizeof(in6_addr) - sizeof(in_addr))) : 0;
        memmove(buf_.data() + sizeof(udp_cmd_down_header) + user_wan_addr_size, buf_.data() + sizeof(udp_cmd_up_header), (size_t)data_len_ - sizeof(udp_cmd_up_header));
    }
    data_len_ -= (ssize_t)sizeof(udp_cmd_up_header);
#else
    data_len_ = 0;
#endif

    udp_cmd_down_header* down_header = (udp_cmd_down_header*)buf_.data();
    down_header->command = htons(k_udp_cmd_port_check);
    //data_len_ += (ssize_t)sizeof(down_header->command);

    return 1;
}

int pair_session_mgr::do_cmd_user_wan_addr(std::shared_ptr<pair_session_info>& pair_session)
{
#if 0
    if (data_len_ < (uint16_t)sizeof(udp_cmd_up_user_wan_addr))
    {
        plog(LOG_ERR, "[%s][][client]recvfrom len abnormal for cmd: udp_cmd_up_user_wan_addr\n", pair_session->get_print_prefix());
        return -1;
    }
#endif

    data_len_ = 0;

    udp_cmd_down_header* header = (udp_cmd_down_header*)buf_.data();
    header->command = htons(k_udp_cmd_user_wan_addr);
    //data_len_ += (ssize_t)sizeof(header->command);

    return 1;
}

void pair_session_mgr::fill_udp_base_header(std::shared_ptr<pair_session_info>& pair_session, bool is_yb_client_cmd)
{
    uint16_t yb_client_cmd = is_yb_client_cmd ? ((udp_cmd_down_header*)(buf_.data()))->command : 0;
    size_t user_wan_addr_size = (pair_session->return_user_wan_addr || yb_client_cmd == htons(k_udp_cmd_user_wan_addr)) ? (sizeof(user_wan_addr_info) - (pair_session->game_client_addr.ss_family == AF_INET6 ? 0 : sizeof(in6_addr) - sizeof(in_addr))) : 0;
    size_t game_data_offset = 0;
    size_t head_offset = 0;

    if (is_yb_client_cmd)
    {
        game_data_offset = user_wan_addr_size + sizeof(udp_cmd_down_header);
    }
    else
    {
        game_data_offset = user_wan_addr_size + (/*pair_session->game_client_is_ipv6*/data_addr_.ss_family == AF_INET6 ? sizeof(udp_down6_header) : sizeof(udp_down4_header));
        head_offset = sizeof(udp_down6_header) + sizeof(user_wan_addr_info) - game_data_offset;
    }

    m_udp_base_header* header = (m_udp_base_header*)(buf_.data() + head_offset);

    memset(header, 0, game_data_offset);
    header->header = htons(M_UDP_FLAG_HEADER);
    header->pkg_len = htons((uint16_t)((size_t)data_len_ + game_data_offset));
#if 0
    header->pkg_start_time = htons(pair_session->game_client_dly_cache.get_start_time(cur_sys_ts_));
#else
    const uint32_t cur_sys_time_ms = (uint32_t)(((uint64_t)cur_sys_ts_.tv_sec * 1000 + (uint64_t)cur_sys_ts_.tv_nsec / 1000000) & UINT32_MAX);
    header->pkg_start_time = htons((uint16_t)(cur_sys_time_ms & 0x7fff));
#endif
    header->pkg_type = is_yb_client_cmd ? M_UDP_FLAG_PKG_TYPE_CONTROL : M_UDP_FLAG_PKG_TYPE_BUSINESS;
    header->ip_type = /*pair_session->game_client_is_ipv6*/data_addr_.ss_family == AF_INET6 ? M_UDP_FLAG_IP_TYPE_IPV6 : M_UDP_FLAG_IP_TYPE_IPV4;
    header->double_line = pair_session->game_client_partner_enable ? M_UDP_FLAG_IP_DOUBLE_ENABLE : M_UDP_FLAG_IP_DOUBLE_DISABLE;
    header->rt_user_wan_addr = user_wan_addr_size != 0 ? M_UDP_FLAG_RETURN_USER_WAN_ADDR_ENABLE : M_UDP_FLAG_RETURN_USER_WAN_ADDR_DISABLE;
    //header->rt_user_wan_addr = pair_session->return_user_wan_addr ? M_UDP_FLAG_RETURN_USER_WAN_ADDR_ENABLE : M_UDP_FLAG_RETURN_USER_WAN_ADDR_DISABLE;

    //plog(LOG_DEBUG, "[%s][][%u]\n", pair_session->get_print_prefix(), pair_session->snd_pkg_sn_2_game_client);
    header->double_line_idx = htons(pair_session->double_line_idx);

    if (is_yb_client_cmd)
    {
        ((udp_cmd_down_header*)header)->command = yb_client_cmd;
        return;
    }

    header->pkg_sn = htons(pair_session->snd_pkg_sn_2_game_client);
    if (pair_session->snd_pkg_sn_2_game_client != UINT16_MAX)
    {
        ++pair_session->snd_pkg_sn_2_game_client;
    }
    else
    {
        pair_session->snd_pkg_sn_2_game_client = 0;
    }
}

bool pair_session_mgr::handle_game_client_send_immediate(std::shared_ptr<pair_session_info>& pair_session, size_t head_offset)
{
    //fill_udp_base_header_4_line_idx(pair_session, head_offset);
    return handle_game_client_send_immediate_inner(pair_session, head_offset);
}

void pair_session_mgr::fill_udp_base_header_4_line_idx(std::shared_ptr<pair_session_info>& pair_session, size_t head_offset)
{
    sockaddr_storage& client_addr = pair_session->game_client_addr;
    m_udp_base_header* header = (m_udp_base_header*)(buf_.data() + head_offset);

    if (header->rt_user_wan_addr == M_UDP_FLAG_RETURN_USER_WAN_ADDR_ENABLE)
    {
        size_t user_wan_addr_offset = header->pkg_type == M_UDP_FLAG_PKG_TYPE_CONTROL ? sizeof(udp_cmd_down_header) : (/*pair_session->game_client_is_ipv6*/data_addr_.ss_family == AF_INET6 ? sizeof(udp_down6_header) : sizeof(udp_down4_header));
        user_wan_addr_info* user_wan_addr = (user_wan_addr_info*)(((uint8_t*)header) + user_wan_addr_offset);

        if (client_addr.ss_family == AF_INET6)
        {
            memcpy(&user_wan_addr->addr6, &((sockaddr_in6*)&client_addr)->sin6_addr, sizeof(user_wan_addr->addr6));
        }
        else
        {
            memcpy(&user_wan_addr->addr4, &((sockaddr_in*)&client_addr)->sin_addr, sizeof(user_wan_addr->addr4));
        }

        user_wan_addr->port = client_addr.ss_family == AF_INET6 ? ((sockaddr_in6*)&client_addr)->sin6_port : ((sockaddr_in*)&client_addr)->sin_port;
        user_wan_addr->family = client_addr.ss_family;
    }

    header->checksum = 0;
#if 0
    header->checksum = packet_checksum::ip_header_check_sum(buf_.data() + head_offset, (uint16_t)(data_len_));
#endif
}

bool pair_session_mgr::handle_game_client_send_immediate_inner(std::shared_ptr<pair_session_info>& pair_session, size_t head_offset)
{
    std::shared_ptr<listen_session_info>& listener = pair_session->game_client_node_listener;
    sockaddr_storage& client_node_addr = pair_session->game_client_node_addr;
    //size_t game_data_offset = /*pair_session->game_client_is_ipv6*/data_addr_.ss_family == AF_INET6 ? sizeof(udp_down6_header) : sizeof(udp_down4_header);
    sockaddr_storage& client_addr = pair_session->game_client_addr;

    static sockaddr_storage game_server_addr{};
    //memset(&game_server_addr, 0, sizeof(game_server_addr));

    return handle_game_client_send_immediate_inner_second(listener, client_addr, client_node_addr, (data_addr_ == client_addr) ? game_server_addr : data_addr_, buf_.data() + head_offset, (size_t)data_len_, pair_session->get_print_prefix());
}

bool pair_session_mgr::handle_game_client_send_immediate_inner_second(std::shared_ptr<listen_session_info>& listener, sockaddr_storage& game_client_addr, sockaddr_storage& game_client_node_addr, sockaddr_storage& game_server_addr, uint8_t* data, size_t data_len, const char* print_prefix)
{
    int
#ifdef UDP_NONBLOCK
        send_status = handle_game_client_send(listener);
    if (send_status == -1)
    {
        //return false;
    }
#endif

    send_status = handle_game_client_send_inner(listener, game_client_addr, game_client_node_addr, game_server_addr, data, data_len, print_prefix);
    if (send_status == -1)
    {
        return false;
    }

#ifdef UDP_NONBLOCK
    if (send_status == 0)
    {
        return push_data_2_game_client(listener, game_client_addr, game_client_node_addr, game_server_addr, data, data_len, print_prefix);
    }
#endif

    return true;
}

#ifdef UDP_NONBLOCK
int pair_session_mgr::handle_game_client_send(std::shared_ptr<listen_session_info>& listener)
{
    if (listener->data_s_2_game_client.empty())
    {
        return 1;
    }

    int ret_val = 1;

    size_t old_size = listener->data_s_2_game_client.size();
    while (!listener->data_s_2_game_client.empty())
    {
        auto& data_2_game_client = listener->data_s_2_game_client.front();
        if (game_client_relation_s_.find(data_2_game_client.client_addr) != game_client_relation_s_.end())
        {
            int send_status = handle_game_client_send_inner(listener, data_2_game_client.client_addr, data_2_game_client.client_node_addr, data_2_game_client.server_addr, data_2_game_client.data.data(), data_2_game_client.data.size(), data_2_game_client.print_prefix.c_str());
            if (send_status == 0)
            {
                ret_val = 0;
                break;
            }

            if (send_status == -1)
            {
                ret_val = -1;
                break;
            }
        }
        listener->data_s_2_game_client.pop_front();
    }

    if (old_size != 0 && listener->data_s_2_game_client.empty())
    {
        if (!listener->epoll_ptr->change_event(listener->listen_fd, EPOLLIN))
        {
            plog(LOG_WARNING, "[%s][listen]change_event failed[%d:%s][EPOLLIN]\n", listener->get_print_prefix(), errno, strerror(errno));
            ret_val = -1;
        }
    }

    return ret_val;
}
#endif

int pair_session_mgr::handle_game_client_send_inner(std::shared_ptr<listen_session_info>& listener, sockaddr_storage& game_client_addr, sockaddr_storage& game_client_node_addr, sockaddr_storage& game_server_addr, uint8_t* data, size_t data_len, const char* print_prefix)
{
    while (!stop_)
    {
#ifdef UDP_RECV_SEND_MSG
        fill_msghdr(data, data_len, game_client_addr, game_client_node_addr);
        ssize_t send_len = sendmsg(listener->listen_fd, &msg_, 0);
#else
        socklen_t addr_len = game_client_addr.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
        ssize_t send_len = sendto(listener->listen_fd, data, data_len, 0, (sockaddr*)&game_client_addr, addr_len);
#endif
        if (send_len == SOCKET_ERROR)
        {
            int err_no = socket_helper::get_errno();
            if (err_no == EINTR)
            {
                continue;
            }

            if (err_no == EAGAIN/*EWOULDBLOCK*/)
            {
#ifdef UDP_NONBLOCK
                return 0;
#else
                assert(0);
                continue;
#endif
            }

            plog(LOG_ERR, "[%s][%s][client]sendto failed[%d:%s][%zu]\n", print_prefix, socket_helper::addr_to_ip_and_port(game_server_addr).c_str(), err_no, strerror(err_no), data_len);
            return 1;
            //return -1;
        }

        plog(LOG_DEBUG, "[%s][%s][client]sendto   [%d]\n", print_prefix, socket_helper::addr_to_ip_and_port(game_server_addr).c_str(), send_len);
        break;
    }

    return 1;
}

#ifdef UDP_NONBLOCK
bool pair_session_mgr::push_data_2_game_client(std::shared_ptr<listen_session_info>& listener, const sockaddr_storage& game_client_addr, sockaddr_storage& game_client_node_addr, sockaddr_storage& game_server_addr, const uint8_t* data, size_t data_len, const char* print_prefix)
{
    data_2_game_client_t data_2_game_client;
    data_2_game_client.client_addr = game_client_addr;
    data_2_game_client.client_node_addr = game_client_node_addr;
    data_2_game_client.server_addr = game_server_addr;
    data_2_game_client.data.assign(data, data + data_len);
    data_2_game_client.print_prefix = print_prefix == NULL ? "" : print_prefix;
    listener->data_s_2_game_client.push_back(data_2_game_client);

    if (!listener->epoll_ptr->change_event(listener->listen_fd, EPOLLIN | EPOLLOUT))
    {
        plog(LOG_WARNING, "[%s][listen]change_event failed[%d:%s][EPOLLIN|EPOLLOUT]\n", listener->get_print_prefix(), errno, strerror(errno));
        return false;
    }

    return true;
}
#endif

bool pair_session_mgr::handle_game_server_send_immediate(std::shared_ptr<pair_session_info>& pair_session)
{
    sockaddr_storage game_svr_addr{};
    //memset(&game_svr_addr, 0, sizeof(game_svr_addr));
    pre_process_game_client_data(pair_session, game_svr_addr);

    size_t game_data_offset = pair_session->game_client_is_ipv6 ? sizeof(udp_up6_header) : sizeof(udp_up4_header);

    int
#ifdef UDP_NONBLOCK
    send_status = handle_game_server_send(pair_session);
    if (send_status == -1)
    {
        return false;
    }
#endif

    send_status = handle_game_server_send_inner(pair_session, game_svr_addr, buf_.data() + game_data_offset, (size_t)data_len_ - game_data_offset);
    if (send_status == -1)
    {
        return false;
    }

#ifdef UDP_NONBLOCK
    if (send_status == 0)
    {
        return push_data_2_game_server(pair_session, game_svr_addr, buf_.data() + game_data_offset, (size_t)data_len_ - game_data_offset);
    }
#endif

    return true;
}

void pair_session_mgr::pre_process_game_client_data(std::shared_ptr<pair_session_info>& pair_session, sockaddr_storage& game_svr_addr)
{
    assert(pair_session->game_server_node_fd != INVALID_SOCKET);
    //pair_session->last_transpond_time = cur_sys_ts_.tv_sec;
#if 0
    //pair_session->game_client_slide_win.pkg_sn_valid(ntohs(header->pkg_sn));
    pair_session->game_client_pkg_loss_stat.cache_pkg_sn(ntohs(header->pkg_sn), cur_sys_ts_);
    pair_session->game_client_dly_cache.update_start_time(cur_sys_ts_, ntohs(header->pkg_start_time));
#endif

    udp_up4_header* header = (udp_up4_header*)buf_.data();
    bool is_ipv6 = header->ip_type == M_UDP_FLAG_IP_TYPE_IPV6;

    //sockaddr_storage game_svr_addr{};
    memset(&game_svr_addr, 0, sizeof(game_svr_addr));
    game_svr_addr.ss_family = is_ipv6 ? AF_INET6 : AF_INET;
    socket_helper::port_to_addr(ntohs(header->game_svr_port), game_svr_addr);

    if (is_ipv6)
    {
        udp_up6_header* header6 = (udp_up6_header*)buf_.data();
        memcpy(&((sockaddr_in6*)&game_svr_addr)->sin6_addr, &header6->game_svr_ip, sizeof(header6->game_svr_ip));
    }
    else
    {
        memcpy(&((sockaddr_in*)&game_svr_addr)->sin_addr, &header->game_svr_ip, sizeof(header->game_svr_ip));
    }

    size_t game_data_offset = is_ipv6 ? sizeof(udp_up6_header) : sizeof(udp_up4_header);
    bool is_icmp = ntohs(header->game_svr_port) == 0;
    if (is_icmp)
    {
        icmphdr* hdr = (icmphdr*)(buf_.data() + game_data_offset);
        hdr->un.echo.id = htons(pair_session->game_server_icmp_id);
        hdr->checksum = 0;
#if 0
        if (is_ipv6)
        {
            hdr->checksum = packet_checksum::proto_payload_check_sum(
                (uint8_t*)hdr, (uint16_t)sizeof(icmp6_hdr),
                (uint8_t*)hdr + sizeof(icmp6_hdr), (uint16_t)(data_len_ - game_data_offset - sizeof(icmp6_hdr)),
                (uint8_t*)&((sockaddr_in6*)&pair_session->game_server_node_addr)->sin6_addr, (uint8_t*)&((sockaddr_in6*)&game_svr_addr)->sin6_addr,
                (uint8_t)sizeof(((sockaddr_in6*)&game_svr_addr)->sin6_addr), IPPROTO_ICMPV6);
        }
        else
        {
            hdr->checksum = packet_checksum::ip_header_check_sum((uint8_t*)hdr, (uint16_t)(data_len_- game_data_offset));
        }
#endif
    }
}

#ifdef UDP_NONBLOCK
int pair_session_mgr::handle_game_server_send(std::shared_ptr<pair_session_info>& pair_session)
{
    if (pair_session->data_s_2_game_server.empty())
    {
        return 1;
    }

    int ret_val = 1;

    size_t old_size = pair_session->data_s_2_game_server.size();
    while (!pair_session->data_s_2_game_server.empty())
    {
        auto& data_2_game_server = pair_session->data_s_2_game_server.front();
        int send_status = handle_game_server_send_inner(pair_session, data_2_game_server.addr, data_2_game_server.data.data(), data_2_game_server.data.size());
        if (send_status == -1)
        {
            ret_val = -1;
            break;
        }

        if (send_status == 0)
        {
            ret_val = 0;
            break;
        }

        pair_session->data_s_2_game_server.pop_front();
    }

    if (old_size != 0 && pair_session->data_s_2_game_server.empty())
    {
        if (!pair_session->game_client_node_listener->epoll_ptr->change_event(pair_session->game_server_node_fd, EPOLLIN))
        {
            plog(LOG_WARNING, "[%s][]change_event failed[%d:%s][EPOLLIN]\n", pair_session->get_print_prefix(), errno, strerror(errno));
            ret_val = -1;
        }
    }

    return ret_val;
}
#endif

int pair_session_mgr::handle_game_server_send_inner(std::shared_ptr<pair_session_info>& pair_session, sockaddr_storage& game_svr_addr, uint8_t* data, size_t data_len)
{
    while (!stop_)
    {
#ifdef UDP_RECV_SEND_MSG
        fill_msghdr(data, data_len, game_svr_addr, pair_session->game_server_node_addr);
        ssize_t send_len = sendmsg(pair_session->game_server_node_fd, &msg_, 0);
#else
        socklen_t addr_len = game_svr_addr.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
        ssize_t send_len = sendto(pair_session->game_server_node_fd, data, data_len, 0, (sockaddr*)&game_svr_addr, addr_len);
#endif
        if (send_len == SOCKET_ERROR)
        {
            int err_no = socket_helper::get_errno();
            if (err_no == EINTR)
            {
                continue;
            }

            if (err_no == EAGAIN/*EWOULDBLOCK*/)
            {
#ifdef UDP_NONBLOCK
                return 0;
#else
                assert(0);
                continue;
#endif
            }

            plog(LOG_ERR, "[%s][%s][server]sendto failed[%d:%s][%zu]\n", pair_session->get_print_prefix(), socket_helper::addr_to_ip_and_port(game_svr_addr).c_str(), err_no, strerror(err_no), data_len);
            return 1;
            //return -1;
        }

        plog(LOG_DEBUG, "[%s][%s][server]sendto   [%d]\n", pair_session->get_print_prefix(), socket_helper::addr_to_ip_and_port(game_svr_addr).c_str(), send_len);
        break;
    }

    return 1;
}

#ifdef UDP_NONBLOCK
bool pair_session_mgr::push_data_2_game_server(std::shared_ptr<pair_session_info>& pair_session, const sockaddr_storage& game_svr_addr, const uint8_t* data, size_t data_len)
{
    data_2_game_server_t data_2_game_server;
    data_2_game_server.addr = game_svr_addr;
    data_2_game_server.data.assign(data, data + data_len);
    pair_session->data_s_2_game_server.push_back(data_2_game_server);

    if (!pair_session->game_client_node_listener->epoll_ptr->change_event(pair_session->game_server_node_fd, EPOLLIN | EPOLLOUT))
    {
        plog(LOG_WARNING, "[%s][listen]change_event failed[%d:%s][EPOLLIN|EPOLLOUT]\n", pair_session->game_client_node_listener->get_print_prefix(), errno, strerror(errno));
        return false;
    }

    return true;
}
#endif

void pair_session_mgr::handle_game_server_events(uint32_t events, std::shared_ptr<pair_session_info> pair_session)
{
    do
    {
        if (events & (EPOLLIN | EPOLLHUP | EPOLLERR))
        {
            if (!handle_game_server_recv(pair_session))
            {
                break;
            }
        }

#ifdef UDP_NONBLOCK
        if (events & EPOLLOUT)
        {
            if (handle_game_server_send(pair_session) == -1)
            {
                break;
            }
        }
#endif

        return;
    } while (0);

    //game_client_relation_s_.erase(pair_session->game_client_addr);
    handle_game_server_close(pair_session);
}

bool pair_session_mgr::handle_game_server_recv(std::shared_ptr<pair_session_info>& pair_session)
{
    while (!stop_)
    {
        memset(&data_addr_, 0, sizeof(data_addr_));
        //memset(buf_.data(), 0, buf_size_);
        size_t game_data_offset = sizeof(udp_down6_header) + sizeof(user_wan_addr_info);

#ifdef UDP_RECV_SEND_MSG
        iov_.iov_base = buf_.data() + game_data_offset;
        iov_.iov_len = buf_size_ - game_data_offset;

        memset(&msg_, 0, sizeof(msg_));
        msg_.msg_name = &data_addr_;
        msg_.msg_namelen = (socklen_t)sizeof(data_addr_);
        msg_.msg_iov = &iov_;
        msg_.msg_iovlen = 1;
        msg_.msg_control = &cdata_;
        msg_.msg_controllen = sizeof(cdata_);

        data_len_ = recvmsg(pair_session->game_server_node_fd, &msg_, 0);
#else
        data_len_ = recvfrom(pair_session->game_server_node_fd, buf_.data() + game_data_offset, buf_size_ - game_data_offset, 0, (sockaddr*)&data_addr_, &data_addr_len_);
#endif
        if (data_len_ == SOCKET_ERROR)
        {
            int err_no = socket_helper::get_errno();
            if (err_no == EINTR)
            {
                continue;
            }

            if (err_no == EAGAIN/*EWOULDBLOCK*/)
            {
                return true;
            }

            plog(LOG_ERR, "[%s][][server]recvfrom failed[%d:%s]\n", pair_session->get_print_prefix(), err_no, strerror(err_no));
            return true;
            //return false;
        }

#ifdef UDP_RECV_SEND_MSG
        data_addr_len_ = msg_.msg_namelen;
        get_local_recv_addr(pair_session->game_server_node_addr);
#endif
        pair_session->get_print_prefix(true);
        plog(LOG_DEBUG, "[%s][%s][server]recvfrom [%d]\n", pair_session->get_print_prefix(), socket_helper::addr_to_ip_and_port(data_addr_).c_str(), data_len_);
        pair_session->last_transpond_time = cur_sys_ts_.tv_sec;
        pre_process_game_server_data(pair_session);

        size_t user_wan_addr_size = pair_session->return_user_wan_addr ? (sizeof(user_wan_addr_info) - (pair_session->game_client_addr.ss_family == AF_INET6 ? 0 : sizeof(in6_addr) - sizeof(in_addr))) : 0;
        size_t head_offset = sizeof(udp_down6_header) + sizeof(user_wan_addr_info) - user_wan_addr_size - (pair_session->game_client_is_ipv6 ? sizeof(udp_down6_header) : sizeof(udp_down4_header));
        fill_udp_base_header_4_line_idx(pair_session, head_offset);
        if (/*console_param_.fec_mode*/pair_session->game_client_fec_enable)
        {
            (void)fec_->send_pdu(pair_session, head_offset);
        }
        else
        {
            (void)handle_game_client_send_immediate(pair_session, head_offset);
        }
        //break;
    }

    return true;
}

void pair_session_mgr::pre_process_game_server_data(std::shared_ptr<pair_session_info>& pair_session)
{
    fill_udp_base_header(pair_session, false);

    size_t user_wan_addr_size = pair_session->return_user_wan_addr ? (sizeof(user_wan_addr_info) - (pair_session->game_client_addr.ss_family == AF_INET6 ? 0 : sizeof(in6_addr) - sizeof(in_addr))) : 0;
    size_t game_data_offset = user_wan_addr_size + (/*pair_session->game_client_is_ipv6*/data_addr_.ss_family == AF_INET6 ? sizeof(udp_down6_header) : sizeof(udp_down4_header));
    m_udp_base_header* header = (m_udp_base_header*)(buf_.data() + sizeof(udp_down6_header) + sizeof(user_wan_addr_info) - game_data_offset);

    if (/*pair_session->game_client_is_ipv6*/data_addr_.ss_family == AF_INET6)
    {
        udp_down6_header* header6 = (udp_down6_header*)header;
        header6->game_svr_port = ((sockaddr_in6*)&data_addr_)->sin6_port;
        memcpy(&header6->game_svr_ip, &((sockaddr_in6*)&data_addr_)->sin6_addr, sizeof(header6->game_svr_ip));
    }
    else
    {
        udp_down4_header* header4 = (udp_down4_header*)header;
        header4->game_svr_port = ((sockaddr_in*)&data_addr_)->sin_port;
        memcpy(&header4->game_svr_ip, &((sockaddr_in*)&data_addr_)->sin_addr, sizeof(header4->game_svr_ip));
    }

    bool is_icmp = socket_helper::port_from_addr(data_addr_) == 0;
    if (is_icmp)
    {
        if (data_len_ >= (ssize_t)sizeof(icmphdr))
        {
            icmphdr* hdr = (icmphdr*)(buf_.data() + sizeof(udp_down6_header) + sizeof(user_wan_addr_info));
            hdr->un.echo.id = htons(pair_session->game_client_icmp_id);
            hdr->checksum = 0;
#if 0
            if (/*pair_session->game_client_is_ipv6*/data_addr_.ss_family == AF_INET6)
            {
#if 0
                hdr->checksum = packet_checksum::proto_payload_check_sum(
                    (uint8_t*)hdr, (uint16_t)sizeof(icmp6_hdr),
                    (uint8_t*)hdr + sizeof(icmp6_hdr), (uint16_t)(data_len_ - game_data_offset - sizeof(icmp6_hdr)),
                    (uint8_t*)&((sockaddr_in6*)&data_addr_)->sin6_addr, (uint8_t*)&((sockaddr_in6*)&pair_session->game_client_addr)->sin6_addr,
                    (uint8_t)sizeof(((sockaddr_in6*)&data_addr_)->sin6_addr), IPPROTO_ICMPV6);
#endif
            }
            else
            {
                hdr->checksum = packet_checksum::ip_header_check_sum((uint8_t*)hdr, (uint16_t)(data_len_ - game_data_offset));
            }
#endif
       }
    }

    header->checksum = 0;
#if 0
    header->checksum = packet_checksum::ip_header_check_sum(buf_.data() + sizeof(udp_down6_header) + sizeof(user_wan_addr_info) - game_data_offset, (uint16_t)(data_len_ + game_data_offset));
#endif

    data_len_ += game_data_offset;
}

void pair_session_mgr::handle_game_server_close(std::shared_ptr<pair_session_info>& pair_session, bool del_from_relation /*= true*/)
{
    if (pair_session->game_server_node_fd == INVALID_SOCKET)
    {
        return;
    }

    unreg_event(pair_session->game_server_node_fd);
    plog(LOG_INFO, "[%s][][server]socket will be close\n", pair_session->get_print_prefix());

    SOCKET sock = pair_session->game_server_node_fd;
    socket_helper::close_socket(pair_session->game_server_node_fd);
    if (del_from_relation)
    {
        game_server_relation_s_.erase(sock);
    }
}

void pair_session_mgr::handle_listen_socket_close(std::shared_ptr<listen_session_info>& listener)
{
    assert(listener != nullptr);
    if (listener->listen_fd == INVALID_SOCKET)
    {
        return;
    }

    unreg_event(listener->listen_fd);
    plog(LOG_INFO, "[%s][listen]socket will be close\n", listener->get_print_prefix());

    SOCKET sock = listener->listen_fd;
    socket_helper::close_socket(listener->listen_fd);
    listen_relation_s_.erase(sock);
}
