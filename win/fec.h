#pragma once

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
    using StreamKey = u64;

    class callback
    {
    public:
		virtual void send_fec_pdu_cb(StreamKey streamKey, const sockaddr_full& epNode, const u8* data, u16 size, void* context) = 0;
        virtual void recv_pdu_cb(StreamKey streamKey, const sockaddr_full& epNode, const sockaddr_full& epTranserLocal, const u8* data, u16 size, void* context) = 0;
    };

    struct ref_buf
    {
        const u8* data;
        u16 len;

        ref_buf(const u8* _data, u16 _len) {
            data = _data;
            len = _len;
        }
    };

public:
    fec(callback* cb);
    ~fec();

    operator bool() const;

    bool fec_wheel();
    static bool is_fec_pkg(uint8_t* data, size_t size, StreamKey* streamKey = nullptr);

    bool recv_fec_pdu(StreamKey streamKey, const sockaddr_full& epLocal, const sockaddr_full& epRemote, const u8* data, u16 len);
    static uint32_t recv_pdu_cb(GtpHandler_p gtp_hdl, void* data, uint32_t size, GtpAddr* client_gtp_addr);

    bool send_pdu(StreamKey streamKey, const sockaddr_full& epLocal, const sockaddr_full& epRemote, const u8* data, u16 len);
	bool send_pdu(StreamKey streamKey, const sockaddr_full& epLocal, const sockaddr_full& epRemote, const std::vector<fec::ref_buf>& bufs);

    static uint32_t send_fec_pdu_cb(GtpHandler_p gtp_hdl, void* data, uint32_t size, GtpAddr* client_gtp_addr);

    static void fec_log_cb(uint32_t log_level, const char* fmt, ...);
    static uint32_t fec_log_level_cb();

    static uint32_t report_link_quality_cb(GtpHandler_p gtp_hdl, GtpLinkQuality vec[], uint32_t size);
    void del_fec_session(StreamKey streamKey, const sockaddr_full& epLocal, const sockaddr_full& epRemote);
    void del_fec_session(StreamKey streamKey);

private:
    GtpHandler_p hdl_;
    u32 thread_id_;

    static std::shared_mutex map_cb_mtx_;
    static std::map<u32, callback*> map_cb_;

};

_declspec(selectany) std::shared_mutex fec::map_cb_mtx_;
_declspec(selectany) std::map<u32, fec::callback*> fec::map_cb_;