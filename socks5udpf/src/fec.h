#pragma once
#include <cstdint>
#include <memory>
#include "bitlinker.h"
#include "pair_session_info.h"
class pair_session_mgr;

class fec_env
{
public:
    fec_env();
    ~fec_env();

    operator bool() const;
    std::string get_ver() const;

private:
    bool env_ok_;
};

class fec
{
public:
    fec(pair_session_mgr* session_mgr);
    ~fec();

    operator bool() const;

    bool fec_wheel();
    static bool is_fec_pkg(uint8_t* data, ssize_t size, uint64_t& stream_key);

    bool recv_fec_pdu(const std::shared_ptr<listen_session_info>& listener);
    static uint32_t recv_pdu_cb(GtpHandler_p gtp_hdl, void* data, uint32_t size, GtpAddr* client_gtp_addr);

    bool send_pdu(const std::shared_ptr<listen_session_info>& listener);
    bool send_pdu(const std::shared_ptr<pair_session_info>& pair_session, size_t head_offset);
    static uint32_t send_fec_pdu_cb(GtpHandler_p gtp_hdl, void* data, uint32_t size, GtpAddr* client_gtp_addr);

    static void fec_log_cb(uint32_t log_level, const char* fmt, ...);
    static uint32_t fec_log_level_cb();

    void del_fec_session(const std::shared_ptr<pair_session_info>& pair_session);
    //void close_session_cb(GtpHandler_p gtp_hdl, void* context);

    //uint32_t report_link_quality_cb(GtpHandler_p gtp_hdl, GtpLinkQuality* vec, uint32_t size);

private:
    GtpHandler_p hdl_;
    pair_session_mgr* session_mgr_;
};
