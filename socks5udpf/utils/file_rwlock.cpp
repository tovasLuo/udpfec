#include "file_rwlock.h"
#include <cassert>
#include <cstring>
#ifndef FCNTL
#include <sys/file.h>
#endif
#include <unistd.h>
#include "sys_log.h"

file_rwlock::file_rwlock(const char* fpath)
    : fpath_(fpath)
    , fd_(-1)
{
#ifdef FCNTL
    memset(&lck_, 0, sizeof(lck_));
    lck_.l_whence = SEEK_SET;
    lck_.l_start = 0;
    lck_.l_len = 0;
#endif
    create();
}

file_rwlock::~file_rwlock()
{
    destroy();
}

void file_rwlock::create()
{
    assert(fd_ == -1);

    fd_ = open(fpath_.c_str(), O_CREAT | O_CLOEXEC | O_SYNC | O_DSYNC | O_RSYNC | O_RDWR, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
    if (fd_ == -1)
    {
        plog(LOG_ERR, "[file_rwlock]open failed[%d:%s]\n", errno, strerror(errno));
        return;
    }
}

void file_rwlock::destroy()
{
    if (fd_ == -1)
    {
        return;
    }

    close(fd_);
    fd_ = -1;
}

void file_rwlock::lock_shared()
{
    rd_lock();
}

void file_rwlock::lock()
{
    wr_lock();
}

void file_rwlock::rd_lock()
{
    if (fd_ == -1)
    {
        return;
    }

#ifdef FCNTL
    lck_.l_type = F_RDLCK;
    lck_.l_pid = getpid();
    if (fcntl(fd_, F_SETLKW, &lck_) == -1)
    {
        plog(LOG_ERR, "[file_rwlock]fcntl F_RDLCK failed[%d:%s]\n",errno, strerror(errno));
    }
#else
    if (flock(fd_, LOCK_SH) == -1)
    {
        plog(LOG_ERR, "[file_rwlock]flock LOCK_SH failed[%d:%s]\n", errno, strerror(errno));
    }
#endif
}

void file_rwlock::wr_lock()
{
    if (fd_ == -1)
    {
        return;
    }

#ifdef FCNTL
    lck_.l_type = F_WRLCK;
    lck_.l_pid = getpid();
    if (fcntl(fd_, F_SETLKW, &lck_) == -1)
    {
        plog(LOG_ERR, "[file_rwlock]fcntl F_WRLCK failed[%d:%s]\n", errno, strerror(errno));
    }
#else
    if (flock(fd_, LOCK_EX) == -1)
    {
        plog(LOG_ERR, "[file_rwlock]flock LOCK_EX failed[%d:%s]\n", errno, strerror(errno));
    }
#endif
}

void file_rwlock::unlock()
{
    if (fd_ == -1)
    {
        return;
    }

#ifdef FCNTL
    lck_.l_type = F_UNLCK;
    lck_.l_pid = getpid();
    if (fcntl(fd_, F_SETLKW, &lck_) == -1)
    {
        plog(LOG_ERR, "[file_rwlock]fcntl F_UNLCK failed[%d:%s]\n", errno, strerror(errno));
    }
#else
    if (flock(fd_, LOCK_UN) == -1)
    {
        plog(LOG_ERR, "[file_rwlock]flock LOCK_UN failed[%d:%s]\n", errno, strerror(errno));
    }
#endif
}
