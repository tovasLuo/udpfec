#pragma once
#include <cassert>
#include <cstring>
#ifdef _WIN32
#include <direct.h>
#endif
#include <fcntl.h>
#include <linux/limits.h>
#include <sys/file.h>
#include <unistd.h>
#include <string>

class program_exists
{
public:
    program_exists(const std::string& program_name);
    program_exists();
    ~program_exists();

    operator bool() const;
    static std::string get_program_path();
    static std::string get_program_name();
    static std::string get_program_dir();
    static std::string get_file_dir(const std::string& file_path);
    static std::string get_file_name(const std::string& file_path);
    static std::string get_abs_path(const std::string& relative_path);

private:
    int init();
    void un_init();
    bool is_exists() const;

private:
    int fd_pid_;
    bool locked_;
    std::string pid_fpath_;
};

inline program_exists::program_exists()
#if 1
    : program_exists(get_program_name())
#else
    : fd_pid_(-1)
    , locked_(false)
    , pid_fpath_("/var/run/" + get_program_name() + ".pid")
#endif
{
}

inline program_exists::program_exists(const std::string& program_name)
    : fd_pid_(-1)
    , locked_(false)
    , pid_fpath_("/var/run/" + program_name + ".pid")
{
    (void)init();
}

inline program_exists::~program_exists()
{
    un_init();
}

inline std::string program_exists::get_program_path()
{
    char program_path[PATH_MAX] = { 0 };
    ssize_t n = readlink("/proc/self/exe", program_path, sizeof(program_path));
    if (n <= 0)
    {
        printf("readlink /proc/self/exe failed[%ld][%d:%s]\n", n, errno, strerror(errno));
        return "";
    }

    return program_path;
}

inline std::string program_exists::get_program_name()
{
    std::string program_path = get_program_path();
    return get_file_name(program_path);
}

inline std::string program_exists::get_file_name(const std::string& file_path)
{
    if (file_path.empty())
    {
        return "";
    }

    auto pos = file_path.rfind('/');
    assert(pos != std::string::npos);
    return file_path.substr(pos + 1);
}

inline std::string program_exists::get_program_dir()
{
    std::string program_path = get_program_path();
    return get_file_dir(program_path);
}

inline std::string program_exists::get_file_dir(const std::string& file_path)
{
    if (file_path.empty())
    {
        return "";
    }

    auto pos = file_path.rfind('/');
    assert(pos != std::string::npos);
    return file_path.substr(0, pos);
}

inline std::string program_exists::get_abs_path(const std::string& relative_path)
{
    std::string abs_path;

#ifdef _WIN32
    char* p_abs_path = _fullpath(NULL, relative_path.c_str(), 0/*_MAX_PATH*/);
#else
    char* p_abs_path = realpath(relative_path.c_str(), NULL);//PATH_MAX
#endif
    if (p_abs_path == NULL)
    {
#ifndef _WIN32
        printf("realpath failed[%s][%d:%s]\n", relative_path.c_str(), errno, strerror(errno));
#endif
    }
    else
    {
        abs_path = p_abs_path;
        free(p_abs_path);
        p_abs_path = NULL;
    }

    return abs_path;
}

inline int program_exists::init()
{
    fd_pid_ = open(pid_fpath_.c_str(), O_CREAT | O_CLOEXEC | O_SYNC | O_WRONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd_pid_ == -1)
    {
        printf("open failed[%s][%d:%s]\n", pid_fpath_.c_str(), errno, strerror(errno));
        return -1;
    }

    if (lseek(fd_pid_, 0, SEEK_SET) == -1)
    {
        printf("lseek failed[%d][%s][%d:%s]\n", fd_pid_, pid_fpath_.c_str(), errno, strerror(errno));
        return -1;
    }

    if (flock(fd_pid_, LOCK_EX | LOCK_NB) == -1)
    {
        if (errno == EWOULDBLOCK)
        {
            printf("the file is locked[%d][%s]\n", fd_pid_, pid_fpath_.c_str());
            return 1;
        }

        printf("flock failed[LOCK_EX|LOCK_NB][%d][%s][%d:%s]\n", fd_pid_, pid_fpath_.c_str(), errno, strerror(errno));
        return -1;
    }

    locked_ = true;
    std::string pid_str = std::to_string(getpid());
    if (write(fd_pid_, pid_str.c_str(), pid_str.size()) == -1)
    {
        printf("write failed[%d][%s][%s][%d:%s]\n", fd_pid_, pid_fpath_.c_str(), pid_str.c_str(), errno, strerror(errno));
        return -1;
    }

    if (fsync(fd_pid_) == -1)
    {
        printf("fsync failed[%d][%s][%s][%d:%s]\n", fd_pid_, pid_fpath_.c_str(), pid_str.c_str(), errno, strerror(errno));
    }

    return 0;
}

inline void program_exists::un_init()
{
    if (fd_pid_ == -1)
    {
        return;
    }

    if (locked_ && flock(fd_pid_, LOCK_UN) == -1)
    {
        printf("flock failed[LOCK_UN][%d][%s][%d:%s]\n", fd_pid_, pid_fpath_.c_str(), errno, strerror(errno));
    }

    if (close(fd_pid_) == -1)
    {
        printf("close failed[%d][%s][%d:%s]\n", fd_pid_, pid_fpath_.c_str(), errno, strerror(errno));
    }
    fd_pid_ = -1;

    if (locked_ && !pid_fpath_.empty() && unlink(pid_fpath_.c_str()) == -1)
    {
        printf("unlink failed[%s][%d:%s]\n", pid_fpath_.c_str(), errno, strerror(errno));
    }

    locked_ = false;
}

inline program_exists::operator bool() const
{
    return is_exists();
}

inline bool program_exists::is_exists() const
{
    return fd_pid_ == -1 || !locked_;
}
