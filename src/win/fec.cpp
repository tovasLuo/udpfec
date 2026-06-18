#include "stdafx.h"
#include "fec.h"
#ifdef __linux__
#include "sys_log.h"
#else
#define	LOG_EMERG	0	/* system is unusable */
#define	LOG_ALERT	1	/* action must be taken immediately */
#define	LOG_CRIT	2	/* critical conditions */
#define	LOG_ERR		3	/* error conditions */
#define	LOG_WARNING	4	/* warning conditions */
#define	LOG_NOTICE	5	/* normal but significant condition */
#define	LOG_INFO	6	/* informational */
#define	LOG_DEBUG	7	/* debug-level messages */

#define	LOG_PRIMASK	0x07	/* mask to extract priority part (internal) */
                /* extract priority */
#define	LOG_PRI(p)	((p) & LOG_PRIMASK)

inline void plog_inner(int pri, const char* fmt, ...);
inline void vplog_inner(int pri, const char* fmt, va_list ap);

#define plog(pri, fmt, ...) \
do { \
        plog_inner((pri), fmt, ##__VA_ARGS__); \
} while(0)

void plog_inner(int pri, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vplog_inner(pri, fmt, ap);
    va_end(ap);
}

void vplog_inner(int pri, const char* fmt, va_list ap)
{
    char buf[1024]={0};
    char dt[50] = { 0 };
    //time_t unixtime = time(NULL);
    //if (strftime(dt, sizeof(dt), "%Y/%m/%d %a %X", localtime(&unixtime)) != 0)
    {
        static const char* log_pre[]{ "EMERG  ","ALERT  ","CRIT   ","ERR    ","WARNING","NOTICE ","INFO   ","DEBUG  " };
        int len = snprintf(buf, sizeof(buf) - 1, "[FEC]%s[%s]", dt, log_pre[LOG_PRI(pri)]);
        if (len < 0) len = 0;
        int len2 = vsnprintf(buf + len, sizeof(buf) -1 - len, fmt, ap);
        if (len2 < 0) len2 = 0;
        buf[len + len2] = 0;
        OutputDebugStringA(buf);
    }
}
#endif

fec_env::fec_env()
    : env_ok_(true)
{
    if (InsLoadGtpModule() != GTP_OK)
    {
        env_ok_ = false;
        plog(LOG_ERR, "[FEC]init fec module failed[%s]\n", LastGtpErrorInfo());
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
        plog(LOG_ERR, "[FEC]uninit fec module failed[%u:%s]\n", res, LastGtpErrorInfo());
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

fec::fec(callback* cb)
    : hdl_(INVALID_GTP_HANDLER)
{
    GtpCallBackParam reg_cb{};

    reg_cb.send_pack_cb_ = fec::send_fec_pdu_cb;
    reg_cb.receive_frame_cb_ = fec::recv_pdu_cb;
    reg_cb.report_link_quality_cb_ = fec::report_link_quality_cb;
    reg_cb.write_log_cb_ = fec::fec_log_cb;
    reg_cb.cur_log_level_cb_ = fec::fec_log_level_cb;
    reg_cb.close_session_cb_ = NULL;

    MemPoolConfig mem_pool_cfg;
    memset(&mem_pool_cfg, 0, sizeof(mem_pool_cfg));
    mem_pool_cfg.m_256bytes_num_ = 4096;
    mem_pool_cfg.m_512bytes_num_ = 2048;
    mem_pool_cfg.m_1k_num_ = 512;
    mem_pool_cfg.m_1_5k_num_ = 128;
    mem_pool_cfg.m_4k_num_ = 128;
    mem_pool_cfg.m_8k_num_ = 32;
    mem_pool_cfg.m_64k_num_ = 32;

    hdl_ = CreateGtpInstance(1, 600000, &reg_cb, &mem_pool_cfg);
    if (hdl_ == INVALID_GTP_HANDLER)
    {
        plog(LOG_ERR, "[FEC]ctor fec instance failed[%s]\n", LastGtpErrorInfo());
        return;
    }

    {
        std::unique_lock lock_w(map_cb_mtx_);
        thread_id_ = GetCurrentThreadId();
        map_cb_[thread_id_] = cb;
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
        plog(LOG_ERR, "[FEC]dtor fec instance failed[0x%x:%s]\n", res, LastGtpErrorInfo());
    }
    hdl_ = INVALID_GTP_HANDLER;

    map_cb_mtx_.lock();
    map_cb_.extract(thread_id_);
    map_cb_mtx_.unlock();
}

fec::operator bool() const
{
    return hdl_ != NULL;
}

bool fec::fec_wheel()
{
    uint32_t res = PeriodGtpTimer(hdl_);
    if (res != GTP_OK)
    {
        plog(LOG_ERR, "[FEC]recv fec pdu failed[0x%x:%s]\n", res, LastGtpErrorInfo());
        return false;
    }

    return true;
}

bool fec::is_fec_pkg(uint8_t* data, size_t size, StreamKey* streamKey)
{
    uint64_t packet_stream_key = 0;
    bool valid = GtpCheckPacketInvalid(data, (uint32_t)size, &packet_stream_key) == GTP_OK;
    if (streamKey != nullptr) {
        *streamKey = valid ? packet_stream_key : 0;
    }
    return valid;
}

bool fec::recv_fec_pdu(StreamKey streamKey, const sockaddr_full& epLocal, const sockaddr_full& epRemote, const u8* data, u16 len)
{
    //plog(LOG_ERR, "[FEC][server]fec pdu[%d]\n", len);

    uint32_t mem_size = 0;
    GtpAddr* client_gtp_addr = NULL;

    uint8_t* mem = GtpMallocPackMem(hdl_, NULL, 0, &mem_size, (void**)&client_gtp_addr, (uint32_t)len);
    if (mem == NULL)
    {
        plog(LOG_ERR, "[FEC][server]fec pdu mem failed[%d]\n", len);
        return false;
    }

    assert(mem_size >= len);
    memcpy(mem, data, len);

    memset(client_gtp_addr, 0, sizeof(*client_gtp_addr));
    
    client_gtp_addr->sfd_ = 0;
    client_gtp_addr->enable_key_ = 1;
    client_gtp_addr->stream_key_ = streamKey;
    
    client_gtp_addr->self_addr_len_ = epLocal.family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    memcpy(client_gtp_addr->self_addr_, &epLocal, client_gtp_addr->self_addr_len_);

    client_gtp_addr->sock_addr_len_ = client_gtp_addr->self_addr_len_;
    memcpy(client_gtp_addr->sock_addr_, &epRemote, client_gtp_addr->sock_addr_len_);


    client_gtp_addr->context_ = (void*)streamKey;
    client_gtp_addr->qos_ = 63;

    bool ret = true;

    uint32_t res = GtpPacketReceive(hdl_, mem, (uint32_t)len, client_gtp_addr);
    if (res != GTP_OK)
    {
        plog(LOG_ERR, "[FEC][server]fec pdu failed[%d][0x%x:%s]\n", len, res, LastGtpErrorInfo());
        ret = false;
    }

    GtpFreePackMem(hdl_, mem);
    return ret;
}

uint32_t fec::recv_pdu_cb(GtpHandler_p gtp_hdl, void* data, uint32_t size, GtpAddr* client_gtp_addr)
{
    if (/*data == nullptr || gtp_hdl == INVALID_GTP_HANDLER || */size > UINT16_MAX || client_gtp_addr == nullptr/* || client_gtp_addr->context_ == nullptr*/)
    {
        plog(LOG_ERR, "[client]fec recv pdu param failed[%p,%u,%p]\n", data, size, client_gtp_addr);
        return GTP_ERR;
    }

    //plog(LOG_ERR, "[FEC][server]fec pdu recv_pdu_cb\n", );
    //UdpCoroSession* session_mgr = (UdpCoroSession*)client_gtp_addr->context_;
    //assert(session_mgr != NULL);

    {
        std::shared_lock lock(map_cb_mtx_);
        auto it = map_cb_.find(GetCurrentThreadId());
        if (it != map_cb_.end()) {
            auto cb = it->second;

            sockaddr_full epNode;/* memset(&epNode, 0, sizeof(epNode));*/
            epNode.family = client_gtp_addr->sock_addr_len_ == sizeof(sockaddr_in) ? AF_INET : AF_INET6;
            if (epNode.family == AF_INET) {
                epNode.si4 = *(sockaddr_in*)client_gtp_addr->sock_addr_;
                
            }
            else {
                epNode.si6 = *(sockaddr_in6*)client_gtp_addr->sock_addr_;
            }

            sockaddr_full epTranserLocal; /*memset(&epTranserLocal, 0, sizeof(epTranserLocal));*/
            epTranserLocal.family = client_gtp_addr->self_addr_len_ == sizeof(sockaddr_in) ? AF_INET : AF_INET6;
            if (epTranserLocal.family == AF_INET) {
                epTranserLocal.si4 = *(sockaddr_in*)client_gtp_addr->self_addr_;

            }
            else {
                epTranserLocal.si6 = *(sockaddr_in6*)client_gtp_addr->self_addr_;
            }

            cb->recv_pdu_cb(client_gtp_addr->stream_key_, epNode, epTranserLocal, (const u8*)data, size,client_gtp_addr->context_);
        }
    }

    return GTP_OK;
}

bool fec::send_pdu(StreamKey streamKey, const sockaddr_full& epLocal, const sockaddr_full& epRemote, const u8* data, u16 len)
{
    //plog(LOG_ERR, "[FEC][client]fec pdu[%d]\n", len);

    uint32_t mem_size = 0;
    GtpAddr* client_gtp_addr = NULL;

    uint8_t* mem = GtpMallocPackMem(hdl_, NULL, 0, &mem_size, (void**)&client_gtp_addr, (uint32_t)len);
    if (mem == NULL)
    {
        plog(LOG_ERR, "[FEC][client]pdu mem failed[%d]\n", len);
        return false;
    }

    assert(mem_size >= len);
    memcpy(mem, data, len);

    memset(client_gtp_addr, 0, sizeof(*client_gtp_addr));

    client_gtp_addr->sfd_ = 0;          //�̶���0
    client_gtp_addr->enable_key_ = 1;
    client_gtp_addr->stream_key_ = streamKey;

    client_gtp_addr->self_addr_len_ = epLocal.family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    memcpy(client_gtp_addr->self_addr_, &epLocal, client_gtp_addr->self_addr_len_); //sfd_��0��Ҫ�Լ�������

    client_gtp_addr->sock_addr_len_ = client_gtp_addr->self_addr_len_;
    memcpy(client_gtp_addr->sock_addr_, &epRemote, client_gtp_addr->sock_addr_len_);

    client_gtp_addr->context_ = (void*)streamKey;
    client_gtp_addr->qos_ = 63;

    bool ret = true;

    uint32_t res = GtpFrameSend(hdl_, mem, (uint32_t)len, client_gtp_addr, 0, 0);
    if (res != GTP_OK)
    {
        plog(LOG_ERR, "[FEC][client]pdu failed[%d][0x%x:%s]\n", len, res, LastGtpErrorInfo());
        ret = false;
    }

    GtpFreePackMem(hdl_, mem);
    return ret;
}
bool fec::send_pdu(StreamKey streamKey, const sockaddr_full& epLocal, const sockaddr_full& epRemote, const std::vector<fec::ref_buf>& bufs)
{
    //plog(LOG_ERR, "[FEC][client]fec pdu[%d]\n", len);

    uint32_t mem_size = 0;
    GtpAddr* client_gtp_addr = NULL;

    uint32_t len = 0;
    for (auto& element : bufs) {
        len += element.len;
    }

    uint8_t* mem = GtpMallocPackMem(hdl_, NULL, 0, &mem_size, (void**)&client_gtp_addr, (uint32_t)len);
    if (mem == NULL)
    {
        plog(LOG_ERR, "[FEC][client]pdu mem failed[%d]\n", len);
        return false;
    }


    assert(mem_size >= len);

    uint32_t pos = 0;
    for (auto& element : bufs) {
        memcpy(mem + pos, element.data, element.len);
        pos += element.len;
    }



    memset(client_gtp_addr, 0, sizeof(*client_gtp_addr));

    client_gtp_addr->sfd_ = 0;          //�̶���0
    client_gtp_addr->enable_key_ = 1;
    client_gtp_addr->stream_key_ = streamKey;

    client_gtp_addr->self_addr_len_ = epLocal.family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    memcpy(client_gtp_addr->self_addr_, &epLocal, client_gtp_addr->self_addr_len_); //sfd_��0��Ҫ�Լ�������

    client_gtp_addr->sock_addr_len_ = client_gtp_addr->self_addr_len_;
    memcpy(client_gtp_addr->sock_addr_, &epRemote, client_gtp_addr->sock_addr_len_);

    client_gtp_addr->context_ = (void*)streamKey;
    client_gtp_addr->qos_ = 63;

    bool ret = true;

    uint32_t res = GtpFrameSend(hdl_, mem, (uint32_t)len, client_gtp_addr, 0, 0);
    if (res != GTP_OK)
    {
        plog(LOG_ERR, "[FEC][client]pdu failed[%d][0x%x:%s]\n", len, res, LastGtpErrorInfo());
        ret = false;
    }

    GtpFreePackMem(hdl_, mem);
    return ret;
}
uint32_t fec::send_fec_pdu_cb(GtpHandler_p gtp_hdl, void* data, uint32_t size, GtpAddr* client_gtp_addr)
{
    if (/*data == nullptr || gtp_hdl == INVALID_GTP_HANDLER || */size > UINT16_MAX || client_gtp_addr == nullptr /*|| client_gtp_addr->context_ == nullptr*/)
    {
        plog(LOG_ERR, "[client]fec send fec pdu param failed[%p,%u,%p]\n", data, size, client_gtp_addr);
        return GTP_ERR;
    }

    {
        std::shared_lock lock(map_cb_mtx_);
        auto it = map_cb_.find(GetCurrentThreadId());
        if (it != map_cb_.end()) {
            auto cb = it->second;

            sockaddr_full ep; memset(&ep, 0, sizeof(ep));
            ep.family = client_gtp_addr->sock_addr_len_ == sizeof(sockaddr_in) ? AF_INET : AF_INET6;
            if (ep.family == AF_INET) {
                ep.si4 = *(sockaddr_in*)client_gtp_addr->sock_addr_;
            }
            else {
                ep.si6 = *(sockaddr_in6*)client_gtp_addr->sock_addr_;
            }

            cb->send_fec_pdu_cb(client_gtp_addr->stream_key_, ep, (const u8*)data, size,client_gtp_addr->context_);
        }
    }

    return GTP_OK;
}

void fec::fec_log_cb(uint32_t log_level, const char* fmt, ...)
{
    //plog(LOG_ERR, "[FEC]fec pdu fec_log_cb\n", );

    va_list ap;
    va_start(ap, fmt);
    vplog_inner((int)log_level, fmt, ap);
    va_end(ap);
}

uint32_t fec::fec_log_level_cb()
{
    return LOG_EMERG;
}

uint32_t fec::report_link_quality_cb(GtpHandler_p gtp_hdl, GtpLinkQuality vec[], uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i)
    {
        GtpLinkQuality& lq = vec[i];
        if (lq.loss_ < 30)
        {
            continue;
        }

      /*  std::shared_ptr<udp::endpoint> ep_tmp;
        if (lq.self_ip_family_ == kGtpIpv6)
        {
            address_v6::bytes_type v6bytes;
            memcpy(v6bytes.data(), ((sockaddr_in6*)lq.self_bin_ip_)->sin6_addr.s6_addr, 16);
            ep_tmp = std::make_shared<udp::endpoint>(address_v6(v6bytes, (asio::ip::scope_id_type)((sockaddr_in6*)lq.self_bin_ip_)->sin6_scope_id), ntohs(((sockaddr_in6*)lq.self_bin_ip_)->sin6_port));
        }
        else
        {
            address_v4::bytes_type v4bytes;
            memcpy(v4bytes.data(), &((sockaddr_in*)lq.self_bin_ip_)->sin_addr.s_addr, 4);
            ep_tmp = std::make_shared<udp::endpoint>(address_v4(v4bytes), ntohs(((sockaddr_in*)lq.self_bin_ip_)->sin_port));
        }*/

        fec::StreamKey streamKey = (fec::StreamKey)lq.context_;

        //udp::socket& sock_node = ((IUdpIcmpSession*)lq.context_)->IsICMP() ? ((IcmpSession*)lq.context_)->m_sock_node : ((UdpCoroSession*)lq.context_)->m_sock_node;
        //udp::endpoint& loc_ep = ((IUdpIcmpSession*)lq.context_)->IsICMP() ? ((IcmpSession*)lq.context_)->loc_ep_ : ((UdpCoroSession*)lq.context_)->loc_ep_;

        //sockaddr_storage game_server_addr;
        //game_server_addr.ss_family = AF_UNSPEC;
        //if (connect(sock_node.native_handle(), (sockaddr*)&game_server_addr, sizeof(sockaddr_in)) == SOCKET_ERROR) {}

        //sock_node.bind({ address_v4::any(),0 });
        //error_code ec;
        //loc_ep = sock_node.local_endpoint(ec);
    }

    return GTP_OK;
}

void fec::del_fec_session(StreamKey streamKey,const sockaddr_full& epLocal,const sockaddr_full& epRemote)
{
    TRACEX0("[fec] del_fec_session {:X}", streamKey);
    GtpAddr client_gtp_addr;
    memset(&client_gtp_addr, 0, sizeof(client_gtp_addr));

    client_gtp_addr.sfd_ = 0;
    client_gtp_addr.enable_key_ = 1;
    client_gtp_addr.stream_key_ = streamKey;

    client_gtp_addr.self_addr_len_ = epLocal.family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    memcpy(client_gtp_addr.self_addr_, &epLocal, client_gtp_addr.self_addr_len_);

    client_gtp_addr.sock_addr_len_ = client_gtp_addr.self_addr_len_;
    memcpy(client_gtp_addr.sock_addr_, &epRemote, client_gtp_addr.sock_addr_len_);

    DelGtpLinker(hdl_, &client_gtp_addr);
}
void fec::del_fec_session(StreamKey streamKey)
{
    TRACEX0("[fec] del_fec_session {:X}", streamKey);
    GtpAddr client_gtp_addr;
    memset(&client_gtp_addr, 0, sizeof(client_gtp_addr));

    client_gtp_addr.sfd_ = 0;
    client_gtp_addr.enable_key_ = 1;
    client_gtp_addr.stream_key_ = streamKey;

    DelGtpLinker(hdl_, &client_gtp_addr);
}