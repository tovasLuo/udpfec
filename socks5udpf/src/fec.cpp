#include "fec.h"
#include <sys_log.h>
#include <cassert>
#include "pair_session_mgr.h"

fec_env::fec_env()
    : env_ok_(true)
{
    if (InsLoadGtpModule() != GTP_OK)
    {
        env_ok_ = false;
        plog(LOG_ERR, "init fec module failed[%s]\n", LastGtpErrorInfo());
    }
}

fec_env::~fec_env()
{
    if (!env_ok_)
    {
        return;
    }

    uint32_t res = RmLoadGtpModule();
    if (res != GTP_OK)
    {
        plog(LOG_ERR, "uninit fec module failed[%u:%s]\n", res, LastGtpErrorInfo());
    }
}

fec_env::operator bool() const
{
    return env_ok_;
}

std::string fec_env::get_ver() const
{
    uint8_t buf[256 + 1] = { 0 };
    return std::string((char*)GetGtpPureVer(buf));
}

fec::fec(pair_session_mgr* session_mgr)
    : hdl_(INVALID_GTP_HANDLER)
    , session_mgr_(session_mgr)
{
    GtpCallBackParam reg_cb{};
    //memset(&reg_cb, 0, sizeof(reg_cb));

    reg_cb.send_pack_cb_ = send_fec_pdu_cb;
    reg_cb.receive_frame_cb_ = recv_pdu_cb;
    reg_cb.report_link_quality_cb_ = NULL;
    reg_cb.write_log_cb_ = fec_log_cb;
    reg_cb.cur_log_level_cb_ = fec_log_level_cb;
    reg_cb.close_session_cb_ = NULL;

    MemPoolConfig mem_pool_cfg{};
    //memset(&mem_pool_cfg, 0, sizeof(mem_pool_cfg));
    mem_pool_cfg.m_256bytes_num_ = (1024 << 6) + (1024 << 5);
    mem_pool_cfg.m_512bytes_num_ = (1024 << 5) + (1024 << 4);
    mem_pool_cfg.m_1k_num_ = (1024 << 5);
    mem_pool_cfg.m_1_5k_num_ = (1024 << 3);
    mem_pool_cfg.m_4k_num_ = 4096;
    mem_pool_cfg.m_8k_num_ = 32;
    mem_pool_cfg.m_64k_num_ = 32;

    hdl_ = CreateGtpInstance(1, 600000, &reg_cb, &mem_pool_cfg);
    if (hdl_ == INVALID_GTP_HANDLER)
    {
        plog(LOG_ERR, "ctor fec instance failed[%s]\n", LastGtpErrorInfo());
        return;
    }
}

fec::~fec()
{
    if (hdl_ == INVALID_GTP_HANDLER)
    {
        return;
    }

    uint32_t res = DeleteGtpInstance(hdl_);
    if (res != GTP_OK)
    {
        plog(LOG_ERR, "dtor fec instance failed[0x%x:%s]\n", res, LastGtpErrorInfo());
    }
    hdl_ = INVALID_GTP_HANDLER;
}

fec::operator bool() const
{
    return hdl_ != INVALID_GTP_HANDLER;
}

bool fec::fec_wheel()
{
    assert(hdl_ != INVALID_GTP_HANDLER);
    uint32_t res = PeriodGtpTimer(hdl_);
    if (res != GTP_OK)
    {
        plog(LOG_ERR, "recv fec pdu failed[0x%x:%s]\n", res, LastGtpErrorInfo());
        return false;
    }

    return true;
}

bool fec::is_fec_pkg(uint8_t* data, ssize_t size, uint64_t& stream_key)
{
    assert(data != NULL && size > 0);
    return GtpCheckPacketInvalid(data, (uint32_t)size, &stream_key) == GTP_OK;
}

bool fec::recv_fec_pdu(const std::shared_ptr<listen_session_info>& listener)
{
    assert(hdl_ != INVALID_GTP_HANDLER && session_mgr_ != NULL && listener != nullptr);

    uint32_t mem_size = 0;
    GtpAddr* client_gtp_addr = NULL;

    uint8_t* mem = GtpMallocPackMem(hdl_, NULL, 0, &mem_size, (void**)&client_gtp_addr, (uint32_t)session_mgr_->data_len_);
    if (mem == NULL)
    {
        plog(LOG_ERR, "[%s][][client]fec pdu mem failed[%d][%s]\n", session_mgr_->get_print_prefix_4_client_recv(listener), session_mgr_->data_len_, LastGtpErrorInfo());

        std::vector<uint8_t> status_buf(4096, 0);
        uint32_t res = GetPackMemPoolStatus(hdl_, status_buf.data(), (uint32_t)status_buf.size());
        if (res != GTP_OK)
        {
            plog(LOG_ERR, "[%s][][client]fec mem pool status failed[0x%x:%s]\n", session_mgr_->get_print_prefix_4_client_recv(listener), res, LastGtpErrorInfo());
        }
        else
        {
            plog(LOG_ERR, "[%s][][client]fec mem pool status\n[%s]\n", session_mgr_->get_print_prefix_4_client_recv(listener), (char*)status_buf.data());
        }

        return false;
    }

    assert(mem_size >= session_mgr_->data_len_);
    memcpy(mem, session_mgr_->buf_.data(), (size_t)session_mgr_->data_len_);

    memset(client_gtp_addr, 0, sizeof(*client_gtp_addr));
    client_gtp_addr->sfd_ = (uint32_t)listener->listen_fd;

    client_gtp_addr->sock_addr_len_ = session_mgr_->data_addr_.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    memcpy(client_gtp_addr->sock_addr_, &session_mgr_->data_addr_, client_gtp_addr->sock_addr_len_);

    client_gtp_addr->self_addr_len_ = session_mgr_->data_node_addr_.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    memcpy(client_gtp_addr->self_addr_, &session_mgr_->data_node_addr_, client_gtp_addr->self_addr_len_);

    client_gtp_addr->context_ = session_mgr_;
    //client_gtp_addr->alg_in_flow_ = 1;
    client_gtp_addr->enable_key_ = session_mgr_->fec_stream_key_ == 0 ? 0 : 1;
    client_gtp_addr->stream_key_ = session_mgr_->fec_stream_key_;
    //client_gtp_addr->stream_type_ = 0;
    //client_gtp_addr->qos_ = 0;

    if (client_gtp_addr->enable_key_ == 0) {
        plog(LOG_WARNING, "[%s][server]WARN: recv_fec_pdu fec_stream_key_=0 -> enable_key_=0 (will create 5-tuple session if auto-extraction fails) client=%s\n",
            session_mgr_->get_print_prefix_4_client_recv(listener),
            socket_helper::addr_to_ip_and_port(session_mgr_->data_addr_).c_str());
    } else {
        plog(LOG_DEBUG, "[%s][server]INFO: recv_fec_pdu fec_stream_key_=%llu enable_key_=1 client=%s\n",
            session_mgr_->get_print_prefix_4_client_recv(listener),
            (unsigned long long)client_gtp_addr->stream_key_,
            socket_helper::addr_to_ip_and_port(session_mgr_->data_addr_).c_str());
    }

    bool ret = true;
    uint32_t res = GtpPacketReceive(hdl_, mem, (uint32_t)session_mgr_->data_len_, client_gtp_addr);
    if (res != GTP_OK)
    {
        plog(LOG_ERR, "[%s][][client]fec pdu failed[%d][0x%x:%s]\n", session_mgr_->get_print_prefix_4_client_recv(listener), session_mgr_->data_len_, res, LastGtpErrorInfo());
        ret = false;
    }

    GtpFreePackMem(hdl_, mem);
    return ret;
}

uint32_t fec::recv_pdu_cb(GtpHandler_p gtp_hdl, void* data, uint32_t size, GtpAddr* client_gtp_addr)
{
    if (/*gtp_hdl == INVALID_GTP_HANDLER || */data == nullptr || size > UINT16_MAX || client_gtp_addr == nullptr || client_gtp_addr->context_ == nullptr)
    {
        plog(LOG_ERR, "[client]fec recv pdu param failed[%p,%u,%p,%p]\n", data, size, client_gtp_addr, client_gtp_addr != nullptr ? client_gtp_addr->context_ : client_gtp_addr);
        return (uint32_t)GTP_ERR;
    }

    pair_session_mgr* session_mgr = (pair_session_mgr*)client_gtp_addr->context_;
    session_mgr->data_len_ = size;
    memcpy(session_mgr->buf_.data(), data, size);

    session_mgr->fec_data_pkg_ = true;
    session_mgr->fec_stream_key_ = client_gtp_addr->enable_key_ == 1 ? client_gtp_addr->stream_key_ : 0;
    session_mgr->fec_stream_type_ = client_gtp_addr->stream_type_;
    session_mgr->fec_qos_ = client_gtp_addr->qos_;

    if (client_gtp_addr->enable_key_ == 0) {
        plog(LOG_WARNING, "[client]WARN: recv_pdu_cb library returned enable_key_=0 (5-tuple session was used) client=%s\n",
            socket_helper::addr_to_ip_and_port(*((sockaddr_storage*)client_gtp_addr->sock_addr_)).c_str());
    } else {
        plog(LOG_DEBUG, "[client]INFO: recv_pdu_cb library key=%llu enable_key_=1 client=%s\n",
            (unsigned long long)client_gtp_addr->stream_key_,
            socket_helper::addr_to_ip_and_port(*((sockaddr_storage*)client_gtp_addr->sock_addr_)).c_str());
    }

    auto it_listener = session_mgr->listen_relation_s_.find((SOCKET)client_gtp_addr->sfd_);
    if (it_listener != session_mgr->listen_relation_s_.end())
    {
        session_mgr->data_addr_len_ = (socklen_t)(((sockaddr_storage*)client_gtp_addr->sock_addr_)->ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in));
        memcpy(&session_mgr->data_addr_, client_gtp_addr->sock_addr_, session_mgr->data_addr_len_);
        memcpy(&session_mgr->data_node_addr_, client_gtp_addr->self_addr_, ((sockaddr_storage*)client_gtp_addr->self_addr_)->ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in));

        session_mgr->get_print_prefix_4_client_recv(it_listener->second, true);
        return session_mgr->handle_game_client_data(it_listener->second) ? GTP_OK : (uint32_t)GTP_ERR;
    }

    plog(LOG_ERR, "[client]fec recv pdu failed no listener found for sfd[%u]\n", client_gtp_addr->sfd_);
    return (uint32_t)GTP_OK;
}

bool fec::send_pdu(const std::shared_ptr<listen_session_info>& listener)
{
    assert(hdl_ != INVALID_GTP_HANDLER && session_mgr_ != NULL && listener != nullptr);

    uint32_t mem_size = 0;
    GtpAddr* client_gtp_addr = NULL;

    uint8_t* mem = GtpMallocPackMem(hdl_, NULL, 0, &mem_size, (void**)&client_gtp_addr, (uint32_t)session_mgr_->data_len_);
    if (mem == NULL)
    {
        plog(LOG_ERR, "[%s][server]pdu mem failed[%d][%s]\n", session_mgr_->get_print_prefix_4_client_recv(listener), session_mgr_->data_len_, LastGtpErrorInfo());

        std::vector<uint8_t> status_buf(4096, 0);
        uint32_t res = GetPackMemPoolStatus(hdl_, status_buf.data(), (uint32_t)status_buf.size());
        if (res != GTP_OK)
        {
            plog(LOG_ERR, "[%s][server]pdu mem pool status failed[0x%x:%s]\n", session_mgr_->get_print_prefix_4_client_recv(listener), res, LastGtpErrorInfo());
        }
        else
        {
            plog(LOG_ERR, "[%s][server]pdu mem pool status\n[%s]\n", session_mgr_->get_print_prefix_4_client_recv(listener), (char*)status_buf.data());
        }

        return false;
    }

    assert(mem_size >= (uint32_t)session_mgr_->data_len_);
    memcpy(mem, session_mgr_->buf_.data(), (size_t)session_mgr_->data_len_);

    memset(client_gtp_addr, 0, sizeof(*client_gtp_addr));
    client_gtp_addr->sfd_ = (uint32_t)listener->listen_fd;

    client_gtp_addr->sock_addr_len_ = session_mgr_->data_addr_.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    memcpy(client_gtp_addr->sock_addr_, &session_mgr_->data_addr_, client_gtp_addr->sock_addr_len_);

    client_gtp_addr->self_addr_len_ = session_mgr_->data_node_addr_.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    memcpy(client_gtp_addr->self_addr_, &session_mgr_->data_node_addr_, client_gtp_addr->self_addr_len_);

    client_gtp_addr->context_ = session_mgr_;
    //client_gtp_addr->alg_in_flow_ = 0;
    client_gtp_addr->enable_key_ = session_mgr_->fec_stream_key_ == 0 ? 0 : 1;
    client_gtp_addr->stream_key_ = session_mgr_->fec_stream_key_;
    client_gtp_addr->stream_type_ = session_mgr_->fec_stream_type_;
    client_gtp_addr->qos_ = session_mgr_->fec_qos_;

    if (session_mgr_->fec_stream_key_ == 0) {
        plog(LOG_WARNING, "[%s][server]WARN: send_pdu(listener/echo) fec_stream_key_=0 -> enable_key_=0 (5-tuple session) client=%s\n",
            session_mgr_->get_print_prefix_4_client_recv(listener),
            socket_helper::addr_to_ip_and_port(session_mgr_->data_addr_).c_str());
    }

    bool ret = true;
    uint32_t res = GtpFrameSend(hdl_, mem, (uint32_t)session_mgr_->data_len_, client_gtp_addr, 0, 0);
    if (res != GTP_OK)
    {
        plog(LOG_ERR, "[%s][server]pdu failed[%d][0x%x:%s]\n", session_mgr_->get_print_prefix_4_client_recv(listener), session_mgr_->data_len_, res, LastGtpErrorInfo());
        ret = false;
    }

    GtpFreePackMem(hdl_, mem);
    return ret;
}

bool fec::send_pdu(const std::shared_ptr<pair_session_info>& pair_session, size_t head_offset)
{
    assert(hdl_ != INVALID_GTP_HANDLER && session_mgr_ != NULL && pair_session != nullptr);

    uint32_t mem_size = 0;
    GtpAddr* client_gtp_addr = NULL;

    uint8_t* mem = GtpMallocPackMem(hdl_, NULL, 0, &mem_size, (void**)&client_gtp_addr, (uint32_t)session_mgr_->data_len_);
    if (mem == NULL)
    {
        plog(LOG_ERR, "[%s][%s][server]pdu mem failed[%d][%s]\n", pair_session->get_print_prefix(), socket_helper::addr_to_ip_and_port(session_mgr_->data_addr_).c_str(), session_mgr_->data_len_, LastGtpErrorInfo());

        std::vector<uint8_t> status_buf(4096, 0);
        uint32_t res = GetPackMemPoolStatus(hdl_, status_buf.data(), (uint32_t)status_buf.size());
        if (res != GTP_OK)
        {
            plog(LOG_ERR, "[%s][%s][server]pdu mem pool status failed[0x%x:%s]\n", pair_session->get_print_prefix(), socket_helper::addr_to_ip_and_port(session_mgr_->data_addr_).c_str(), res, LastGtpErrorInfo());
        }
        else
        {
            plog(LOG_ERR, "[%s][%s][server]pdu mem pool status\n[%s]\n", pair_session->get_print_prefix(), socket_helper::addr_to_ip_and_port(session_mgr_->data_addr_).c_str(), (char*)status_buf.data());
        }

        return false;
    }

    assert(mem_size >= (uint32_t)session_mgr_->data_len_);
    memcpy(mem, session_mgr_->buf_.data() + head_offset, (size_t)session_mgr_->data_len_);

    memset(client_gtp_addr, 0, sizeof(*client_gtp_addr));
    client_gtp_addr->sfd_ = (uint32_t)pair_session->game_client_node_listener->listen_fd;

    client_gtp_addr->sock_addr_len_ = pair_session->game_client_addr.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    memcpy(client_gtp_addr->sock_addr_, &pair_session->game_client_addr, client_gtp_addr->sock_addr_len_);

    client_gtp_addr->self_addr_len_ = pair_session->game_client_node_addr.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    memcpy(client_gtp_addr->self_addr_, &pair_session->game_client_node_addr, client_gtp_addr->self_addr_len_);

    client_gtp_addr->context_ = session_mgr_;
    //client_gtp_addr->alg_in_flow_ = 0;
    client_gtp_addr->enable_key_ = pair_session->game_client_fec_stream_key == 0 ? 0 : 1;
    client_gtp_addr->stream_key_ = pair_session->game_client_fec_stream_key;
    client_gtp_addr->stream_type_ = pair_session->game_client_fec_stream_type;
    client_gtp_addr->qos_ = pair_session->game_client_fec_qos;

    if (pair_session->game_client_fec_stream_key == 0) {
        plog(LOG_WARNING, "[%s][server]WARN: send_pdu(pair_session) game_client_fec_stream_key=0, enable_key_=0, client=%s\n",
            pair_session->get_print_prefix(), socket_helper::addr_to_ip_and_port(pair_session->game_client_addr).c_str());
    }

    bool ret = true;
    uint32_t res = GtpFrameSend(hdl_, mem, (uint32_t)session_mgr_->data_len_, client_gtp_addr, 0, 0);
    if (res != GTP_OK)
    {
        plog(LOG_ERR, "[%s][%s][server]pdu failed[%d][0x%x:%s]\n", pair_session->get_print_prefix(), socket_helper::addr_to_ip_and_port(session_mgr_->data_addr_).c_str(), session_mgr_->data_len_, res, LastGtpErrorInfo());
        ret = false;
    }

    GtpFreePackMem(hdl_, mem);
    return ret;
}

uint32_t fec::send_fec_pdu_cb(GtpHandler_p gtp_hdl, void* data, uint32_t size, GtpAddr* client_gtp_addr)
{
    if (/*gtp_hdl == INVALID_GTP_HANDLER || */data == nullptr || size > UINT16_MAX || client_gtp_addr == nullptr || client_gtp_addr->context_ == nullptr)
    {
        plog(LOG_ERR, "[server]fec send fec pdu param failed[%p,%u,%p,%p]\n", data, size, client_gtp_addr, client_gtp_addr != nullptr ? client_gtp_addr->context_ : client_gtp_addr);
        return (uint32_t)GTP_ERR;
    }

    if (client_gtp_addr->sock_addr_len_ != sizeof(sockaddr_in6) && client_gtp_addr->sock_addr_len_ != sizeof(sockaddr_in))
    {
        plog(LOG_ERR, "[server]fec send fec pdu client_gtp_addr->sock_addr_len_ failed[%u]\n", client_gtp_addr->sock_addr_len_);
        return (uint32_t)GTP_ERR;
    }

    pair_session_mgr* session_mgr = (pair_session_mgr*)client_gtp_addr->context_;
    session_mgr->data_len_ = size;
    memcpy(session_mgr->buf_.data(), data, size);

    session_mgr->fec_data_pkg_ = true;
    session_mgr->fec_stream_key_ = client_gtp_addr->enable_key_ == 1 ? client_gtp_addr->stream_key_ : 0;
    session_mgr->fec_stream_type_ = client_gtp_addr->stream_type_;
    session_mgr->fec_qos_ = client_gtp_addr->qos_;

    if (client_gtp_addr->enable_key_ == 0) {
        plog(LOG_WARNING, "[server]WARN: send_fec_pdu_cb enable_key_=0 (5-tuple GoodTP session) size=%u client=%s\n",
            size, socket_helper::addr_to_ip_and_port(*((sockaddr_storage*)client_gtp_addr->sock_addr_)).c_str());
    }

    sockaddr_storage game_client_addr{};
    //memset(&game_client_addr, 0, sizeof(game_client_addr));
    memcpy(&game_client_addr, client_gtp_addr->sock_addr_, ((sockaddr_storage*)client_gtp_addr->sock_addr_)->ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in));

    auto it_client = session_mgr->game_client_relation_s_.find(game_client_addr);
    if (it_client != session_mgr->game_client_relation_s_.end())
    {
        return session_mgr->handle_game_client_send_immediate(it_client->second, 0) ? GTP_OK : (uint32_t)GTP_ERR;
    }

    auto it_listener = session_mgr->listen_relation_s_.find((SOCKET)client_gtp_addr->sfd_);
    if (it_listener != session_mgr->listen_relation_s_.end())
    {
        session_mgr->data_addr_len_ = (socklen_t)(((sockaddr_storage*)client_gtp_addr->sock_addr_)->ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in));
        memcpy(&session_mgr->data_addr_, client_gtp_addr->sock_addr_, session_mgr->data_addr_len_);
        memcpy(&session_mgr->data_node_addr_, client_gtp_addr->self_addr_, ((sockaddr_storage*)client_gtp_addr->self_addr_)->ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in));

        static sockaddr_storage game_server_addr{};
        //memset(&game_server_addr, 0, sizeof(game_server_addr));
        return session_mgr->handle_game_client_send_immediate_inner_second(it_listener->second, session_mgr->data_addr_, session_mgr->data_node_addr_, game_server_addr, session_mgr->buf_.data(), (size_t)session_mgr->data_len_, session_mgr->get_print_prefix_4_client_recv(it_listener->second, true)) ? GTP_OK : (uint32_t)GTP_ERR;
    }

    plog(LOG_ERR, "[server]fec send pdu failed no listener found for sfd[%u]\n", client_gtp_addr->sfd_);
    return (uint32_t)GTP_OK;
}

void fec::fec_log_cb(uint32_t log_level, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vplog_inner((int)log_level, fmt, ap);
    va_end(ap);
}

uint32_t fec::fec_log_level_cb()
{
    if (!g_enable_syslog && !g_enable_console_log)
    {
        return LOG_EMERG;
    }

    return (uint32_t)g_log_level;
}

void fec::del_fec_session(const std::shared_ptr<pair_session_info>& pair_session)
{
    assert(hdl_ != INVALID_GTP_HANDLER && pair_session != nullptr);

    GtpAddr client_gtp_addr{};
    //memset(&client_gtp_addr, 0, sizeof(client_gtp_addr));

    client_gtp_addr.sfd_ = (uint32_t)pair_session->game_client_node_listener->listen_fd;

    client_gtp_addr.sock_addr_len_ = pair_session->game_client_addr.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    memcpy(client_gtp_addr.sock_addr_, &pair_session->game_client_addr, client_gtp_addr.sock_addr_len_);

    client_gtp_addr.self_addr_len_ = pair_session->game_client_node_addr.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    memcpy(client_gtp_addr.self_addr_, &pair_session->game_client_node_addr, client_gtp_addr.self_addr_len_);

    //client_gtp_addr.alg_in_flow_ = 0;
    client_gtp_addr.enable_key_ = pair_session->game_client_fec_stream_key == 0 ? 0 : 1;
    client_gtp_addr.stream_key_ = pair_session->game_client_fec_stream_key;
    client_gtp_addr.stream_type_ = pair_session->game_client_fec_stream_type;
    client_gtp_addr.qos_ = pair_session->game_client_fec_qos;

    DelGtpLinker(hdl_, &client_gtp_addr);
}
