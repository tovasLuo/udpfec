#pragma once
#include <atomic>
#include <thread>
#include "libcli.h"
#include "socket_helper.h"

class cli_business
{
public:
    cli_business(const std::string& app_name, uint16_t cli_port);
    ~cli_business();

    void start();
    void run();
    void stop();
    void wait_stop();

private:
    bool init_socket();
    void uninit_socket();

    void cli_set_business();
    void run_accept();

    const char* listen_print_prefix(bool update = false);

private:
    std::string       app_name_;
    uint16_t          cli_port_;

    SOCKET            listen_fd_;
    sockaddr_storage  listen_addr_;
    std::string       listen_print_prefix_;

    std::thread       thread_;
    std::atomic<bool> stop_;


    cli_def*          cli_;
};

