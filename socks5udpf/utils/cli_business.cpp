#include "cli_business.h"
#include <cstring>
#if defined(__linux__)
#if defined(LIBCLI_USE_POLL)
#include <poll.h>
#endif
#include <strings.h>
#include <unistd.h>
#else
#include <string.h>
#endif
#include "socket_helper.h"
#include "sys_log.h"

#undef UNUSED
#if defined(__GNUC__) || defined(__clang__)
#define UNUSED(x) x __attribute__((unused))
#else
#define UNUSED(x) x
#endif

#if defined(_WIN32)
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#endif

int msleep(unsigned int millisecond)
{
#if defined(_WIN32)
    Sleep(millisecond);
    return 0;
#else
    return usleep(millisecond * 1000);
#endif
}

int syslog_enable(cli_def* cli, UNUSED(const char* command), UNUSED(char* argv[]), UNUSED(int argc));
int syslog_disable(cli_def* cli, UNUSED(const char* command), UNUSED(char* argv[]), UNUSED(int argc));

int log_level_perimeter(cli_def* cli, const char* command, char* argv[], int argc);
int log_level_completor(cli_def* cli, const char* name, const char* word, cli_comphelp* comphelp);
int log_level_validator(cli_def* cli, const char* name, const char* value);

cli_business::cli_business(const std::string& app_name, uint16_t cli_port)
    : app_name_(app_name)
    , cli_port_(cli_port)
    , listen_fd_(INVALID_SOCKET)
    , stop_(false)
    , cli_(NULL)
{
    memset(&listen_addr_, 0, sizeof(listen_addr_));
}

cli_business::~cli_business()
{
    stop();
    wait_stop();
}

void cli_business::start()
{
    thread_ = std::thread(&cli_business::run, this);
}

void cli_business::run()
{
    cli_ = cli_init();
    cli_set_business();

    while (!stop_)
    {
        if (init_socket())
        {
            run_accept();
        }
        msleep(1000);
    }

    stop_ = true;
    cli_done(cli_);
}

void cli_business::cli_set_business()
{
    cli_set_banner(cli_, (app_name_ + " cli_console").c_str());
    cli_set_hostname(cli_, app_name_.c_str());
    //cli_telnet_protocol(cli_, 1);

    //cli_regular_interval(cli_, 5);   // Defaults to 1 second
    cli_set_idle_timeout(cli_, 600); // 60 second idle timeout

    cli_command* syslog = cli_register_command(cli_, NULL, "syslog", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "syslog enable or disable");
    cli_register_command(cli_, syslog, "enable", syslog_enable, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "syslog enable");
    cli_register_command(cli_, syslog, "disable", syslog_disable, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "syslog disable");

    cli_command* log = cli_register_command(cli_, NULL, "log", log_level_perimeter, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "log switch");
    cli_optarg* level = cli_register_optarg(log, "level", CLI_CMD_OPTIONAL_ARGUMENT, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "set the log level",
        log_level_completor, log_level_validator, NULL);

    cli_optarg_addhelp(level, "emerg",   "system is unusable.");
    cli_optarg_addhelp(level, "alert",   "action must be taken immediately.");
    cli_optarg_addhelp(level, "crit",    "critical conditions.");
    cli_optarg_addhelp(level, "err",     "error conditions.");
    cli_optarg_addhelp(level, "warning", "warning conditions.");
    cli_optarg_addhelp(level, "notice",  "normal but significant condition.");
    cli_optarg_addhelp(level, "info",    "informational messages.");
    cli_optarg_addhelp(level, "debug",   "debug-level messages.");
    cli_optarg_addhelp(level, "*",       "all level messages.");
    cli_optarg_addhelp(level, "none",    "the \"no priority\" priority.");

    //cli_allow_user(cli_, "fred", "nerk");
    cli_allow_user(cli_, "root", "root");

    //cli_set_auth_callback(cli, check_auth);
    //cli_set_enable_callback(cli, check_enable);
}

int syslog_enable(cli_def* cli, UNUSED(const char* command), UNUSED(char* argv[]), UNUSED(int argc))
{
    (void)command;
    (void)argv;
    (void)argc;

    g_enable_syslog = true;
    cli_print(cli, "syslog enable");

    return CLI_OK;
}

int syslog_disable(cli_def* cli, UNUSED(const char* command), UNUSED(char* argv[]), UNUSED(int argc))
{
    (void)command;
    (void)argv;
    (void)argc;

    g_enable_syslog = false;
    cli_print(cli, "syslog disable");

    return CLI_OK;
}

static const char* s_log_level[]{ "emerg", "alert", "crit", "err", "warning", "notice", "info", "debug", "*", "none", NULL };
int log_level_perimeter(cli_def* cli, const char* command, char* argv[], int argc)
{
    (void)command;
    (void)argv;
    (void)argc;

    char* level_str = cli_get_optarg_value(cli, "level", NULL);
    if (level_str == NULL)
    {
        cli_error(cli, "no log level name given");
        return CLI_ERROR;
    }

    int level = 0;
    for (const char** log_level = s_log_level; *log_level != NULL; ++log_level, ++level)
    {
        if (strncasecmp(*log_level, level_str, strlen(level_str)) == 0)
        {
            if (strncasecmp("*", level_str, strlen(level_str)) == 0)
            {
                --level;
            }
            else if (strncasecmp("none", level_str, strlen(level_str)) == 0)
            {
#ifndef INTERNAL_NOPRI
#define INTERNAL_NOPRI  0x10                /* the "no priority" priority */
#endif
                level = INTERNAL_NOPRI;
            }
            change_log_level(level);
            cli_print(cli, "set the log level: %s", *log_level);
            return CLI_OK;
        }
    }

    cli_error(cli, "unrecognized log level given");
    return CLI_ERROR;
}

int log_level_completor(cli_def* cli, const char* name, const char* word, struct cli_comphelp* comphelp)
{
    (void)cli;
    (void)name;

    int rc = CLI_OK;
    for (const char** log_level = s_log_level; *log_level != NULL && rc == CLI_OK; ++log_level)
    {
        if (word == NULL || strncasecmp(*log_level, word, strlen(word)) == 0)
        {
            rc = cli_add_comphelp_entry(comphelp, *log_level);
        }
    }
    return rc;
}

int log_level_validator(cli_def* cli, const char* name, const char* value)
{
    (void)cli;
    (void)name;

    int level = 0;
    for (const char** log_level = s_log_level; *log_level != NULL; ++log_level, ++level)
    {
        if (strcasecmp(value, *log_level) == 0)
        {
            return CLI_OK;
        }
    }

    return CLI_ERROR;
}

bool cli_business::init_socket()
{
    memset(&listen_addr_, 0, sizeof(listen_addr_));
    sockaddr_in* sai = (sockaddr_in*)&listen_addr_;
    sai->sin_family = AF_INET;
    sai->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sai->sin_port = htons(cli_port_);
    listen_print_prefix(true);

    if ((listen_fd_ = socket_helper::create_socket(AF_INET, SOCK_STREAM/* | SOCK_CLOEXEC | SOCK_NONBLOCK*/, IPPROTO_TCP)) == INVALID_SOCKET)
    {
        plog(LOG_WARNING, "[%s][listen]create socket failed[%d:%s]\n", listen_print_prefix(), errno, strerror(errno));
        return false;
    }
    listen_print_prefix(true);

    do
    {
        socket_helper::set_cloexec(listen_fd_);
        socket_helper::set_non_block(listen_fd_);
        socket_helper::set_tcp_no_delay(listen_fd_);
        socket_helper::set_reuse_addr(listen_fd_);
        socket_helper::set_reuse_port(listen_fd_);
        //socket_helper::set_rcv_timeout(listen_fd_, 500);

        if (bind(listen_fd_, (const sockaddr*)&listen_addr_, (socklen_t)sizeof(listen_addr_)) == SOCKET_ERROR)
        {
            plog(LOG_WARNING, "[%s][listen]bind failed[%d:%s]\n", listen_print_prefix(), errno, strerror(errno));
            break;
        }

        if (listen(listen_fd_, 1) == SOCKET_ERROR)
        {
            plog(LOG_WARNING, "[%s][listen]listen failed[%d:%s]\n", listen_print_prefix(), errno, strerror(errno));
            break;
        }

        sockaddr_storage listen_local_addr;
        memset(&listen_local_addr, 0, sizeof(listen_local_addr));
        if (!socket_helper::get_local_addr_by_fd(listen_fd_, listen_local_addr))
        {
            plog(LOG_WARNING, "[%s][listen]get_local_addr_by_fd failed[%d:%s]\n", listen_print_prefix(), errno, strerror(errno));
            break;
        }

        memcpy(&listen_addr_, &listen_local_addr, sizeof(listen_addr_));
        listen_print_prefix(true);
        plog(LOG_INFO, "[%s][listen]socket create succeed\n", listen_print_prefix());

        return true;
    } while (0);

    uninit_socket();
    return false;
}

const char* cli_business::listen_print_prefix(bool update/* = false*/)
{
    if (!listen_print_prefix_.empty() && !update)
    {
        return listen_print_prefix_.c_str();
    }

    listen_print_prefix_  = /*"[" + */std::to_string(listen_fd_) + "]";
    listen_print_prefix_ += "[" + std::string("]");
    listen_print_prefix_ += "[" + socket_helper::addr_to_ip_and_port(listen_addr_)/* + "]"*/;
    //listen_print_prefix_ += "[" + std::string("cli_listen")/* + "]"*/;

    return listen_print_prefix_.c_str();
}

void cli_business::run_accept()
{
    SOCKET           session_fd = INVALID_SOCKET;
    sockaddr_storage session_addr = { 0 };
    socklen_t        session_addr_len = 0;

    timeval tv = { 0, 1000 * 500 };

#ifdef LIBCLI_USE_POLL
    struct pollfd pfds = { listen_fd_, POLLIN, 0 };
    while (!stop_)
    {
        const int ret_val = poll(&pfds, 1, (int)((tv.tv_sec * 1000) + (tv.tv_usec / 1000)));
        if (ret_val == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }

            plog(LOG_ERR, "[%s][listen]poll failed[%d:%s]\n", listen_print_prefix(), errno, strerror(errno));
            break;
        }

        if (ret_val == 0)
        {
            continue;
        }
        
        if (pfds.revents == 0)
        {
            continue;
        }

        if (pfds.revents & POLLHUP)
        {
            plog(LOG_ERR, "[%s][listen]the socket was terminated by the peer\n", listen_print_prefix());
            break;
        }

        if (pfds.revents & POLLERR)
        {
            int err_no = 0;
            socklen_t err_no_size = sizeof(err_no);
            if (getsockopt(listen_fd_, SOL_SOCKET, SO_ERROR, &err_no, &err_no_size) == -1)
            {
                err_no = errno;
            }
            plog(LOG_ERR, "[%s][listen]exceptfds error occured[%d:%s]\n", listen_print_prefix(), err_no, strerror(err_no));
            break;
        }

        if (pfds.revents & POLLIN)
        {
            memset(&session_addr, 0, sizeof(session_addr));
            session_addr_len = sizeof(session_addr);

            session_fd = accept4(listen_fd_, (sockaddr*)&session_addr, &session_addr_len, SOCK_CLOEXEC/* | SOCK_NONBLOCK*/);
            if (session_fd == INVALID_SOCKET)
            {
                int err_no = socket_helper::get_errno();
                if (err_no == EINTR/*WSAEINTR*/ ||
                    err_no == EAGAIN/*WSAEWOULDBLOCK*/ ||
                    err_no == EWOULDBLOCK ||
                    err_no == ECONNRESET/*WSAECONNRESET*/ ||
                    err_no == ECONNABORTED/*WSAECONNABORTED*/ ||
                    err_no == EPROTO/*WSAEPROTONOSUPPORT*//* ||
                    err_no == ETIMEDOUT*/)
                {
                    continue;
                }

                plog(LOG_ERR, "[%d][%s][%s][listen]accept failed[%d:%s]\n", listen_fd_, socket_helper::addr_to_ip_and_port(session_addr).c_str(), socket_helper::addr_to_ip_and_port(listen_addr_).c_str(), err_no, strerror(err_no));
                break;
            }

            socket_helper::set_tcp_no_delay(session_fd);
            socket_helper::set_reuse_addr(session_fd);
            socket_helper::set_reuse_port(session_fd);

            plog(LOG_INFO, "[%d][%s][%s][listen]accept by[%d]\n", session_fd, socket_helper::addr_to_ip_and_port(session_addr).c_str(), socket_helper::addr_to_ip_and_port(listen_addr_).c_str(), listen_fd_);

            cli_loop(cli_, session_fd);

            plog(LOG_INFO, "[%d][%s][%s][client]socket be closed\n", session_fd, socket_helper::addr_to_ip_and_port(session_addr).c_str(), socket_helper::addr_to_ip_and_port(listen_addr_).c_str());
            //socket_helper::close_socket(session_fd);

            pfds.revents = 0;
        }
    }
#else
    fd_set fdr;
    fd_set fde;
    FD_ZERO(&fdr);
    FD_ZERO(&fde);

    while (!stop_)
    {
        FD_ZERO(&fdr);
        FD_ZERO(&fde);
        FD_SET(listen_fd_, &fdr);
        FD_SET(listen_fd_, &fde);

        tv.tv_sec = 0;
        tv.tv_usec = 1000 * 500;

        const int ret_val = select((int)listen_fd_ + 1, &fdr, NULL, &fde, &tv);
        if (ret_val == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }

            plog(LOG_ERR, "[%s][listen]select failed[%d:%s]\n", listen_print_prefix(), errno, strerror(errno));
            break;
        }

        if (ret_val == 0)
        {
            continue;
        }

        if (/*listen_fd_ != INVALID_SOCKET && */FD_ISSET(listen_fd_, &fde))
        {
            int err_no = 0;
            socklen_t err_no_size = sizeof(err_no);
            if (getsockopt(listen_fd_, SOL_SOCKET, SO_ERROR, (char*)&err_no, &err_no_size) == -1)
            {
                err_no = errno;
            }
            plog(LOG_ERR, "[%s][listen]exceptfds error occured[%d:%s]\n", listen_print_prefix(), err_no, strerror(err_no));

            break;
        }

        if (/*listen_fd_ != INVALID_SOCKET && */FD_ISSET(listen_fd_, &fdr))
        {
            memset(&session_addr, 0, sizeof(session_addr));
            session_addr_len = sizeof(session_addr);

            session_fd = accept/*4*/(listen_fd_, (sockaddr*)&session_addr, &session_addr_len/*, SOCK_CLOEXEC | SOCK_NONBLOCK*/);
            if (session_fd == INVALID_SOCKET)
            {
                int err_no = socket_helper::get_errno();
                if (
#if defined(_WIN32)
                    err_no == WSAEINTR ||
                    err_no == WSAEWOULDBLOCK ||
                    err_no == WSAECONNRESET ||
                    err_no == WSAECONNABORTED ||
                    err_no == WSAEPROTONOSUPPORT
#else
                    err_no == EINTR ||
                    err_no == EAGAIN || err_no == EWOULDBLOCK ||
                    err_no == ECONNRESET ||
                    err_no == ECONNABORTED ||
                    err_no == EPROTO
#endif
                    )
                {
                    continue;
                }

                plog(LOG_ERR, "[%d][%s][%s][listen]accept failed[%d:%s]\n", listen_fd_, socket_helper::addr_to_ip_and_port(session_addr).c_str(), socket_helper::addr_to_ip_and_port(listen_addr_).c_str(), err_no, strerror(err_no));
                break;
            }

            socket_helper::set_cloexec(session_fd);
            //socket_helper::set_non_block(session_fd);
            socket_helper::set_tcp_no_delay(session_fd);
            socket_helper::set_reuse_addr(session_fd);
            socket_helper::set_reuse_port(session_fd);

            plog(LOG_INFO, "[%d][%s][%s][listen]accept by[%d]\n", session_fd, socket_helper::addr_to_ip_and_port(session_addr).c_str(), socket_helper::addr_to_ip_and_port(listen_addr_).c_str(), listen_fd_);

            cli_loop(cli_, session_fd);

            plog(LOG_INFO, "[%d][%s][%s][client]socket be closed\n", session_fd, socket_helper::addr_to_ip_and_port(session_addr).c_str(), socket_helper::addr_to_ip_and_port(listen_addr_).c_str());
            //socket_helper::close_socket(session_fd);
        }
    }
#endif

    uninit_socket();
}

void cli_business::uninit_socket()
{
    if (listen_fd_ == -1)
    {
        plog(LOG_INFO, "[%d][][%s][listen]socket will be close\n", listen_fd_, socket_helper::addr_to_ip_and_port(listen_addr_).c_str());
        socket_helper::close_socket(listen_fd_);
        listen_fd_ = (SOCKET)-1;
    }
}

void cli_business::stop()
{
    stop_ = true;
}

void cli_business::wait_stop()
{
    if (thread_.joinable())
    {
        thread_.join();
    }
    stop_ = true;
}
