#include "epoll_wrap.h"
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#if !defined(_WIN32)
#include <unistd.h>
#include <sys/socket.h>
#endif
#include "sys_log.h"

//https://man.freebsd.org/cgi/man.cgi?kqueue
//https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/kqueue.2.html
//https://man7.org/linux/man-pages/man7/epoll.7.html
//https://github.com/piscisaureus/wepoll

epoll_wrap::epoll_wrap(int max_rcv_fd_size/* = 64*/, int timeout_ms/* = 10*/)
    : valid_(false)
    , epoll_fd_(INVALID_HANDLE_VALUE)
    , timeout_ms_(timeout_ms)
{
    if (max_rcv_fd_size <= 0 || timeout_ms < -1)
    {
        plog(LOG_ERR, "epoll_wrap arguments: max_rcv_fd_size, timeout_ms[%d,%d]\n", max_rcv_fd_size, timeout_ms);
        valid_ = false;
        return;
    }

    set_epoll_wait_timeout(timeout_ms);
    rcv_event_s_.resize((size_t)max_rcv_fd_size);
    valid_ = init_epoll();
}

epoll_wrap::~epoll_wrap()
{
    uninit_epoll();
}

bool epoll_wrap::valid() const
{
    return valid_;
}

void epoll_wrap::set_epoll_wait_timeout(int timeout_ms)
{
    timeout_ms_ = timeout_ms;
}

bool epoll_wrap::init_epoll()
{
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ == INVALID_HANDLE_VALUE)
    {
        plog(LOG_ERR, "epoll create failed[%d:%s]\n", errno, strerror(errno));
        return false;
    }

    return true;
}

void epoll_wrap::uninit_epoll()
{
    if (epoll_fd_ == INVALID_HANDLE_VALUE)
    {
        return;
    }

    (void)epoll_close(epoll_fd_);
    epoll_fd_ = INVALID_HANDLE_VALUE;
}

bool epoll_wrap::reg_event(SOCKET fd, uint32_t events, void* context/* = NULL*/)
{
    return set_epoll_ctrl(fd, EPOLL_CTL_ADD, events, context);
}

bool epoll_wrap::unreg_event(SOCKET fd)
{
    uint32_t events = 0;
    return set_epoll_ctrl(fd, EPOLL_CTL_DEL, events, NULL);
}

bool epoll_wrap::change_event(SOCKET fd, uint32_t events, void* context/* = NULL*/)
{
    return set_epoll_ctrl(fd, EPOLL_CTL_MOD, events, context);
}

bool epoll_wrap::set_epoll_ctrl(SOCKET fd, int op, uint32_t events, void* context)
{
    epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    ev.data.ptr = context != NULL ? context : (void*)(intptr_t)fd; // Windows不能用ev.data.fd = fd这种方式
    ev.events = events;

    if (epoll_ctl(epoll_fd_, op, fd, &ev) == -1)
    {
        plog(LOG_ERR, "[%d]epoll ctrl failed[%d:%s]\n", fd, errno, strerror(errno));
        return false;
    }

    return true;
}

bool epoll_wrap::do_epoll_wait()
{
    int event_count = epoll_wait(epoll_fd_, rcv_event_s_.data(), (int)rcv_event_s_.size(), timeout_ms_);
    if (event_count == -1)
    {
        if (errno == EINTR)
        {
            return true;
        }

        plog(LOG_ERR, "[%d]epoll wait failed[%d:%s]\n", epoll_fd_, errno, strerror(errno));
        return false;
    }

    before_handle_epoll_wait();

    if (event_count == 0)
    {
        return true;
    }

    for (size_t i = 0; i < (size_t)event_count; ++i)
    {
        handle_epoll_wait(rcv_event_s_[i].events, rcv_event_s_[i].data.ptr);
    }

    return true;
}

void epoll_wrap::before_handle_epoll_wait()
{
}

void epoll_wrap::handle_epoll_wait(uint32_t events, void* context)
{
    (void)events;
    (void)context;
}
