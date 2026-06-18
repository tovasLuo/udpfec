#include <csignal>
#include <cstdio>
#include "cli_business.h"
#include "comm_def.h"
#include "fec.h"
#include "pair_session_mgr.h"
#include "program_exists.h"
#include "root_authority.h"
#include "string_helper.h"
#include "sync_auth_info.h"
#include "sys_log.h"

int parse_options(console_param_t& param, int argc, char* argv[]);
void usage();

int main(int argc, char* argv[])
{
    root_authority root_auth;
#if 1
    if (system("ulimit -S -c unlimited") == -1)
    {
        printf("[main]ulimit -c unlimited failed[%d:%s]", errno, strerror(errno));
    }
#endif

    if (system("sysctl -w net.ipv4.ping_group_range=\"0 0\"") == -1)
    {
        printf("[main]sysctl -w net.ipv4.ping_group_range=\"0 0\" failed[%d:%s]", errno, strerror(errno));
    }

    std::string log_name = argv[0];

    std::string cur_work_dir = program_exists::get_program_dir();
    console_param_t console_param(cur_work_dir);
    if (parse_options(console_param, argc, argv) != 0)
    {
        return 0;
    }

    console_param.repair();

    change_log_level_4_string(console_param.log_level);
    g_enable_syslog = console_param.enable_syslog;

    if (console_param.daemon)
    {
        if (daemon(0, 0) == -1)
        {
            if (!g_enable_syslog)
            {
                g_log_level = LOG_DEBUG;
                g_enable_console_log = true;
            }
        }
    }
    else
    {
        g_enable_console_log = g_enable_syslog;
        g_enable_syslog = false;
    }

    program_exists self_exists;
    if (self_exists)
    {
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    //signal(SIGALRM, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    //if (console_param.enable_syslog)
    {
        auto pos = log_name.rfind('/');
        if (pos != std::string::npos)
        {
            log_name.erase(0, pos + 1);
        }
        init_sys_log(log_name);
    }

    sync_auth_info sync_auth(console_param.auth_fpath);
    sync_auth.start();

    std::shared_ptr<fec_env> fenv;
    if (console_param.fec_mode)
    {
        fenv = std::make_shared<fec_env>();
        if (!(*fenv))
        {
            return 0;
        }

        plog(LOG_INFO, "[main]fec ver[%s]\n", fenv->get_ver().c_str());
    }

    auto cpu_num = std::thread::hardware_concurrency();
    if (cpu_num == 0)
    {
        plog(LOG_WARNING, "[main]get_nprocs retval[%u]\n", cpu_num);
        cpu_num = 1;
    }
    plog(LOG_INFO, "[main]cpu num[%u]\n", cpu_num);

    uint32_t thread_num = console_param.thread_num;
    if (thread_num == 0)
    {
        thread_num = /*cpu_num == 1 ? 1 : */cpu_num * 2;
    }
    else if (thread_num > 64)
    {
        thread_num = 64;
    }

    size_t pair_session_num = thread_num;
    plog(LOG_INFO, "[main]create pair session num[%zu]\n", pair_session_num);

    std::vector<std::shared_ptr<pair_session_mgr>> pair_session_mgr_s;
    for (size_t i = 0; i < pair_session_num; ++i)
    {
        auto p_pair_session_mgr = std::make_shared<pair_session_mgr>(console_param, sync_auth);
        pair_session_mgr_s.push_back(p_pair_session_mgr);
        //if (i < pair_session_num -1)
        {
            p_pair_session_mgr->start();
        }
    }

    //pair_session_mgr_s[pair_session_num - 1]->run();

    cli_business cli_bus(log_name, console_param.cli_port);
    cli_bus.run();

    for (auto& p_pair_session_mgr : pair_session_mgr_s)
    {
        p_pair_session_mgr->stop();
        p_pair_session_mgr->wait_stop();
    }

    sync_auth.stop();
    sync_auth.wait_stop();

    //if (console_param.enable_syslog)
    {
        uninit_sys_log();
    }

    return 0;
}

int parse_options(console_param_t& param, int argc, char* argv[])
{
    int opt = -1;
    while ((opt = getopt(argc, argv, "?a:c:dfh:lL:o:p:t:v")) != -1)
    {
        switch (opt)
        {
        case '?':
            usage();
            return -1;
        case 'a':
            param.auth_fpath = optarg;
            break;
        case 'c':
            param.cli_port = string_helperA::to_arithmetic_default<uint16_t>(std::string(optarg), param.cli_port);
            break;
        case 'd':
            param.daemon = false;
            break;
        case 'f':
            param.fec_mode = true;
            break;
        case 'h':
            param.bind_host = optarg;
            break;
        case 'l':
            param.enable_syslog = false;
            break;
        case 'L':
            param.log_level = optarg;
            break;
        case 'o':
            param.sock_timeout_s = string_helperA::to_arithmetic_default<uint16_t>(std::string(optarg), param.sock_timeout_s);
            break;
        case 'p':
            param.bind_port = optarg;
            break;
        case 't':
            param.thread_num = string_helperA::to_arithmetic_default<uint16_t>(std::string(optarg), param.thread_num);
            break;
        case 'v':
            printf("********************************************\n");
            printf("*    versino socks5udps 1.0.3 build 016    *\n");
            printf("********************************************\n");
            return -1;
        default:
            usage();
            return -1;
        }
    }

    return 0;
}

void usage()
{
    printf("Usage:\n");
    printf(" -? <display this help>\n");
    printf(" -v <versino>\n");
    printf(" -l <disable syslog>\n");
    printf(" -L <log level>        debug info notice warning err crit alert emerg none\n");
    printf(" -d <disable daemon>\n");
    printf(" -f <enable fec mode>\n");
    printf(" -c <port number>      cli listen port\n");
    printf(" -t <thread number>    work thread number\n");
    printf(" -a <fpath>            authorize file path\n");
    printf(" -h <ip>               local listen host\n");
    printf(" -p <port number>      local listen port\n");
    printf(" -o <time number>      timeout for socket\n");
}
