#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#ifdef __linux__
#include <sys/epoll.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
#include "bepoll.h"
#elif defined(_WIN32)
#include "wepoll.h"
#endif

// Windows下HANDLE和SOCKET的定义
// typedef void*     HANDLE;
// typedef uintptr_t SOCKET;
// #define INVALID_HANDLE_VALUE ((void*)(intptr_t)-1)
// #define INVALID_SOCKET       (uintptr_t)(~0)
// #define SOCKET_ERROR         (-1)

#if !defined(_WIN32)
typedef int HANDLE;
typedef int SOCKET;
#define INVALID_HANDLE_VALUE -1
#endif

#ifdef __linux__
#define epoll_close close
#endif

class epoll_wrap
{
public:
    epoll_wrap(int max_rcv_fd_size = 64, int timeout_ms = 10);
    virtual ~epoll_wrap();

    epoll_wrap(const epoll_wrap&) = delete;
    epoll_wrap& operator=(const epoll_wrap&) = delete;

    bool valid() const;
    void set_epoll_wait_timeout(int timeout_ms);

    bool reg_event(SOCKET fd, uint32_t events, void* context = NULL);
    bool unreg_event(SOCKET fd);
    bool change_event(SOCKET fd, uint32_t events, void* context = NULL);
    bool do_epoll_wait();

    virtual void before_handle_epoll_wait();
    virtual void handle_epoll_wait(uint32_t events, void* context);

private:
    bool init_epoll();
    void uninit_epoll();
    bool set_epoll_ctrl(SOCKET fd, int op, uint32_t events, void* context);

private:
    bool valid_;
    HANDLE epoll_fd_;

    int timeout_ms_;
    std::vector<epoll_event> rcv_event_s_;
};
