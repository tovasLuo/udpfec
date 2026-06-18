#pragma once
#include <string>

struct console_param_t
{
    console_param_t(const std::string& cur_work_dir = std::string())
        : daemon(true)
        , enable_syslog(true)
        , fec_mode(false)
        , log_level("warning")
        , auth_fpath("/opt/ysnet/socks5/log/vppp_socks5.user_online")
        , thread_num(0)
        , cli_port(0)
        , bind_host("0.0.0.0")
        //, bind_port("2086,2087,2088,9998,9999")
        , sock_timeout_s(60*2)
    {}

    void repair()
    {
        if (cli_port == 0)
        {
            cli_port = fec_mode ? 21004 : 21003;
        }

        if (bind_port.empty())
        {
            bind_port = fec_mode ? "2084,2085" : "2086,2087,2088,9998,9999";
        }
    }

    bool        daemon;
    bool        enable_syslog;
    bool        fec_mode;
    std::string log_level;
    std::string auth_fpath;
    uint16_t    thread_num;
    uint16_t    cli_port;
    std::string bind_host;
    std::string bind_port;
    uint16_t    sock_timeout_s;
};
