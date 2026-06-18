#pragma once
#include <fcntl.h>
#include <string>

class file_rwlock
{
public:
    file_rwlock(const char* fpath);
    ~file_rwlock();

    void lock_shared();
    void lock();

    void rd_lock();
    void wr_lock();
    void unlock();

private:
    void create();
    void destroy();

private:
    std::string fpath_;
    int fd_;
#ifdef FCNTL
    struct flock lck_;
#endif
};
