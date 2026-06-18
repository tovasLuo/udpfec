#include "sync_auth_info.h"
#include <cassert>
#include <fcntl.h>
#include <sys/fanotify.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "program_exists.h"
#include "socket_helper.h"
#include "string_helper.h"
#include "sys_log.h"

sync_auth_info::sync_auth_info(const std::string& auth_fpath)
    : epoll_wrap(100, 500)
    , stop_(false)
    , auth_fpath_(auth_fpath)
    , file_last_size_(0)
    , first_work_(true)
    , notify_fd_(-1)
    , buf_size_(UINT16_MAX)
    , buf_(buf_size_, 0)
    , data_len_(0)
    , rwlock_("/var/tmp/vppp_socks5.rwlock")
{
    memset(&last_update_time_, 0, sizeof(last_update_time_));
    //memset(&old_sys_ts_, 0, sizeof(old_sys_ts_));
    memset(&cur_sys_ts_, 0, sizeof(cur_sys_ts_));
    memset(&file_last_mtim_, 0, sizeof(file_last_mtim_));

    auth_info_s_ = std::make_shared<std::unordered_map<uint32_t, auth_info_t>>();
}

sync_auth_info::~sync_auth_info()
{
    stop();
    wait_stop();

    uninit_fanotify();
    auth_info_s_tmp_.reset();
    std::unique_lock<std::shared_mutex> wlk(auth_info_s_rw_mtx_);
    auth_info_s_.reset();
}

void sync_auth_info::start()
{
    thread_ = std::thread(&sync_auth_info::run, this);
}

void sync_auth_info::run()
{
    do
    {
        if (!valid())
        {
            break;
        }

        if (!init_fanotify())
        {
            break;
        }

        while (!stop_)
        {
            if (!do_epoll_wait())
            {
                //break;
            }
            //std::this_thread::yield();
        }

    } while (0);

    stop_ = true;
}

void sync_auth_info::stop()
{
    stop_ = true;
}

void sync_auth_info::wait_stop()
{
    if (thread_.joinable())
    {
        thread_.join();
    }
    stop_ = true;
}

bool sync_auth_info::init_fanotify()
{
    notify_fd_ = fanotify_init(FAN_CLASS_CONTENT | FAN_CLOEXEC | FAN_NONBLOCK, O_CLOEXEC | O_RDWR | O_LARGEFILE);
    if (notify_fd_ == -1)
    {
        plog(LOG_ERR, "[%s]fanotify_init failed[%d:%s]\n", sync_auth_get_print_prefix(), errno, strerror(errno));
        return false;
    }

    do
    {
        std::string dir = program_exists::get_file_dir(auth_fpath_);
        if (!dir.empty() && access(dir.c_str(), F_OK) == -1)
        {
            std::string mkdir_cmd = "mkdir -p " + dir;
            if (system(mkdir_cmd.c_str()) == -1) {}
        }

        if (!auth_fpath_.empty() && access(auth_fpath_.c_str(), F_OK | R_OK | W_OK) == -1)
        {
            std::string touch_cmd = "touch " + auth_fpath_;
            if (system(touch_cmd.c_str()) == -1) {}
        }

        if (fanotify_mark(notify_fd_, FAN_MARK_ADD/* | FAN_MARK_MOUNT*/, FAN_CLOSE_WRITE, /*AT_EMPTY_PATH*/AT_FDCWD, auth_fpath_.c_str()) == -1)
        {
            plog(LOG_ERR, "[%s]fanotify_mark failed[%d:%s]\n", sync_auth_get_print_prefix(), errno, strerror(errno));
            break;
        }

        if (!reg_event(notify_fd_, EPOLLIN))
        {
            plog(LOG_WARNING, "[%s]reg_event failed[%d:%s]\n", sync_auth_get_print_prefix(), errno, strerror(errno));
            break;
        }

        plog(LOG_INFO, "[%s]fanotify_init succeed\n", sync_auth_get_print_prefix());
        return true;
    } while (0);

    uninit_fanotify();
    return false;
}

void sync_auth_info::uninit_fanotify()
{
    if (notify_fd_ == -1)
    {
        return;
    }

    unreg_event(notify_fd_);
    close(notify_fd_);
    notify_fd_ = -1;
}

void sync_auth_info::before_handle_epoll_wait()
{
    if (clock_gettime(CLOCK_MONOTONIC, &cur_sys_ts_) == -1) {}

    if (first_work_)
    {
        first_work_ = false;
        auth_file_check_and_handle_change();
    }
}

std::string sync_auth_info::get_cur_date_string()
{
    time_t cur_time = time(NULL);
    assert(cur_time != (time_t)(-1));
    tm* p = localtime(&cur_time);
    assert(p != NULL);
    char buf[10] = { 0 };
    strftime(buf, sizeof(buf), "%Y%m%d-", p);
    buf[10 - 1] = '\0';

    return std::string(buf, 10 - 1);
}

void sync_auth_info::auth_file_check_and_handle_change()
{
    if (check_file_change(auth_fpath_, file_last_mtim_, file_last_size_) != 1)
    {
        return;
    }

    rwlock_.lock_shared();
    bool read_val = sync_auth_info_s();
    rwlock_.unlock();
    if (!read_val)
    {
        return;
    }

    auth_info_s_rw_mtx_.lock();
    auth_info_s_ = auth_info_s_tmp_;
    last_update_time_ = cur_sys_ts_;
    auth_info_s_rw_mtx_.unlock();
}

int sync_auth_info::check_file_change(const std::string& fpath, timespec& last_mtim, off_t& last_size)
{
    struct stat file_stat;
    if (stat(fpath.c_str(), &file_stat) == -1)
    {
        plog(LOG_WARNING, "[%s]stat file failed[%d:%s]\n", sync_auth_get_print_prefix(), errno, strerror(errno));
        memset(&last_mtim, 0, sizeof(last_mtim));
        return -1;
    }

    if (memcmp(&file_stat.st_mtim, &last_mtim, sizeof(last_mtim)) == 0 && file_stat.st_size == last_size)
    {
        return 0;
    }

    memcpy(&last_mtim, &file_stat.st_mtim, sizeof(last_mtim));
    last_size = file_stat.st_size;

    return 1;
}

void sync_auth_info::handle_epoll_wait(uint32_t events, void* context)
{
    assert(notify_fd_ == (int)(long)context);
    if (events & (EPOLLIN | EPOLLHUP | EPOLLERR))
    {
        (void)handle_fanotify_events();
    }
}

bool sync_auth_info::handle_fanotify_events()
{
    while (!stop_)
    {
        data_len_ = read(notify_fd_, buf_.data(), buf_size_);
        if (data_len_ == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }

            if (errno == EAGAIN/*EWOULDBLOCK*/)
            {
                break;
            }

            plog(LOG_WARNING, "[%s]read failed[%d:%s]\n", sync_auth_get_print_prefix(), errno, strerror(errno));
            return false;
        }

        if (data_len_ == 0)
        {
            break;
        }

        for (fanotify_event_metadata* metadata = (fanotify_event_metadata*)buf_.data();
            FAN_EVENT_OK(metadata, data_len_); metadata = FAN_EVENT_NEXT(metadata, data_len_))
        {
            if (metadata->fd == FAN_NOFD)
            {
                continue;
            }

            do
            {
                //plog(LOG_INFO, "[%s]metadata mask, 0x%08x\n", sync_auth_get_print_prefix(), metadata->mask);
                if ((metadata->mask & FAN_CLOSE_WRITE) == 0)
                {
                    break;
                }

                if (metadata->vers < /*2*/FANOTIFY_METADATA_VERSION)
                {
                    plog(LOG_ERR, "[%s]Kernel fanotify metadata version too old\n", sync_auth_get_print_prefix());
                    break;
                }

                char path[PATH_MAX] = { 0 };
                std::string fd_path = "/proc/self/fd/" + std::to_string(metadata->fd);
                ssize_t path_len = readlink(fd_path.c_str(), path, sizeof(path) - 1);
                if (path_len == -1)
                {
                    plog(LOG_WARNING, "[%s]readlink failed[%d:%s]\n", sync_auth_get_print_prefix(), errno, strerror(errno));
                    break;
                }

                path[path_len] = '\0';
                //plog(LOG_INFO, "[%s]file changed, %s\n", sync_auth_get_print_prefix(), path);

#if 0
                std::string date_str = get_cur_date_string();
                size_t pos_date = std::string(path).rfind(date_str);

                size_t pos = std::string::npos;
                if ((pos = std::string(path).rfind(auth_fpath_, pos_date)) != std::string::npos &&
                    ((pos_date == std::string::npos && (pos + auth_fpath_.size()) == strlen(path)) ||
                     (pos_date != std::string::npos && (pos + auth_fpath_.size()) == pos_date)))
#else
                if (std::string(path) == auth_fpath_)
#endif
                {
                    auth_file_check_and_handle_change();
                }
            } while (0);

            close(metadata->fd);
        }
    }

    return true;
}

const char* sync_auth_info::sync_auth_get_print_prefix(bool update /*= false*/)
{
    if (!auth_fpath_.empty() && !update)
    {
        return auth_fpath_.c_str();
    }

    return auth_fpath_.c_str();
}

bool sync_auth_info::sync_auth_info_s()
{
    auth_info_s_tmp_ = std::make_shared<std::unordered_map<uint32_t, auth_info_t>>();

    int fd = open(auth_fpath_.c_str(), O_CREAT | O_CLOEXEC | O_SYNC | O_DSYNC | O_RSYNC | O_RDONLY, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
    if (fd == -1)
    {
        plog(LOG_ERR, "[%s]open failed[%d:%s]\n", sync_auth_get_print_prefix(), errno, strerror(errno));
        return false;
    }

    std::vector<char> buffer;

    bool ret_val = true;
    do
    {
        off_t end = lseek(fd, 0, SEEK_END);
        if (end == -1)
        {
            plog(LOG_ERR, "[%s]lseek failed[SEEK_END][%d:%s]\n", sync_auth_get_print_prefix(), errno, strerror(errno));
            ret_val = false;
            break;
        }

        off_t begin = lseek(fd, 0, SEEK_SET);
        if (begin == -1)
        {
            plog(LOG_ERR, "[%s]lseek failed[SEEK_SET][%d:%s]\n", sync_auth_get_print_prefix(), errno, strerror(errno));
            ret_val = false;
            break;
        }

        ssize_t fsize = end - begin;
        if (fsize <= 0)
        {
            break;
        }

        buffer.resize((size_t)fsize + 1);

        ssize_t offset = 0;
        ssize_t r_size = 0;

        do
        {
            r_size = read(fd, buffer.data() + offset, buffer.size() - (size_t)offset);
            if (r_size == -1)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                if (errno == EWOULDBLOCK)
                {
                    continue;
                }

                plog(LOG_ERR, "[%s]read failed[%d:%s]\n", sync_auth_get_print_prefix(), errno, strerror(errno));
                ret_val = false;
                break;
            }

            if (r_size == 0)
            {
                break;
            }

            offset = offset + r_size;
        } while (offset < fsize);

        buffer.resize((size_t)offset);
    } while (0);

    if (close(fd) == -1)
    {
        plog(LOG_ERR, "[%s]close failed[%d:%s]\n", sync_auth_get_print_prefix(), errno, strerror(errno));
    }

    if (buffer.empty())
    {
        plog(LOG_DEBUG, "[%s]user_online info size is empty\n", sync_auth_get_print_prefix());
        return ret_val;
    }

    std::vector<std::string> temp_info = string_helperA::split(buffer.data(), "\r\n", false);
    for (auto& line : temp_info)
    {
        if (line.empty())
        {
            continue;
        }

        //plog(LOG_DEBUG, "[%s]user_online info[%s]\n", sync_auth_get_print_prefix(), line.c_str());
        (void)parse_auth_info(line);
    }

    //plog(LOG_DEBUG, "[%s]user_online info size is [%zu]\n", sync_auth_get_print_prefix(), auth_info_s_tmp_->size());

    return ret_val;
}

bool sync_auth_info::parse_auth_info(const std::string& line)
{
    assert(!line.empty());

    std::vector<std::string> lst = string_helperA::split(line, ' ', true);
    if (lst.size() < 12)
    {
        plog(LOG_WARNING, "[%s]user_online info item not enough[%s]\n", sync_auth_get_print_prefix(), line.c_str());
        return false;
    }

    auth_info_t auth_info;

    auth_info.user_name = lst[0];
    auth_info.user_pwd_str = lst[1];
    auth_info.user_pwd = string_helperA::to_arithmetic_default<uint32_t>(lst[1], 0);
#if 0
    sscanf(lst[2].c_str(), "%d", &auth_info.user_type);

    auto tmp_prep_verify_time = string_helperA::split(lst[3], ',');
    if (tmp_prep_verify_time.size() > 0)
    {
        sscanf(tmp_prep_verify_time[0].c_str(), "%ld", &auth_info.prep_verify_time.tv_sec);
    }
    if (tmp_prep_verify_time.size() > 1)
    {
        sscanf(tmp_prep_verify_time[1].c_str(), "%ld", &auth_info.prep_verify_time.tv_nsec);
    }
    auth_info.prep_verify_time = timespec_subtraction(auth_info.prep_verify_time, start_sys_ts_);
#endif

    sscanf(lst[4].c_str(), "%d", &auth_info.socks5_fd);
    auth_info.user_ip = lst[5];

#if 0
    auto tmp_user_up_time = string_helperA::split(lst[6], ',');
    if (tmp_user_up_time.size() > 0)
    {
        sscanf(tmp_user_up_time[0].c_str(), "%ld", &auth_info.user_up_time.tv_sec);
    }
    if (tmp_user_up_time.size() > 1)
    {
        sscanf(tmp_user_up_time[1].c_str(), "%ld", &auth_info.user_up_time.tv_nsec);
    }
    auth_info.user_up_time = timespec_subtraction(auth_info.user_up_time, start_sys_ts_);

    auto tmp_lask_check_time = string_helperA::split(lst[7], ',');
    if (tmp_lask_check_time.size() > 0)
    {
        sscanf(tmp_lask_check_time[0].c_str(), "%ld", &auth_info.lask_check_time.tv_sec);
    }
    if (tmp_lask_check_time.size() > 1)
    {
        sscanf(tmp_lask_check_time[1].c_str(), "%ld", &auth_info.lask_check_time.tv_nsec);
    }
    auth_info.lask_check_time = timespec_subtraction(auth_info.lask_check_time, start_sys_ts_);

    auth_info.area_svr_id = lst[8];

    auth_info.socks5_addr.ss_family = AF_UNIX;
    strcpy(((sockaddr_un*)&auth_info.socks5_addr)->sun_path, lst[9].c_str());
#endif

    std::vector<std::string> temp_ip = string_helperA::split(lst[10], ',', true);
    if (temp_ip.size() > 0)
    {
        auth_info.bind_local_ipv4_str = temp_ip[0];
    }

    if (temp_ip.size() > 1)
    {
        auth_info.bind_local_ipv6_str = temp_ip[1];
    }

    if (!temp_ip.empty())
    {
        auth_info.update_bind_local_addr();
    }

    sscanf(lst[11].c_str(), "%d", &auth_info.verify_type);

    auth_info_s_tmp_->insert(std::make_pair(auth_info.user_pwd, auth_info));
    return true;
}

bool sync_auth_info::update_auth_info_s(timespec& pre_update_time)
{
    if (pre_update_time.tv_nsec == last_update_time_.tv_nsec &&
        pre_update_time.tv_sec == last_update_time_.tv_sec)
    {
        return false;
    }

    auth_info_s_rw_mtx_.lock_shared();
    pre_update_time = last_update_time_;
    auth_info_s_rw_mtx_.unlock_shared();

    return true;
}

bool sync_auth_info::update_auth_info_s_custom(std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>>& auth_info_s, timespec& pre_update_time)
{
    if (pre_update_time.tv_nsec == last_update_time_.tv_nsec &&
        pre_update_time.tv_sec == last_update_time_.tv_sec)
    {
        return false;
    }

    assert(auth_info_s != nullptr);

    auth_info_s_rw_mtx_.lock_shared();
    pre_update_time = last_update_time_;
    *auth_info_s = *auth_info_s_;
    auth_info_s_rw_mtx_.unlock_shared();

    return true;
}

bool sync_auth_info::auth_pwd(uint32_t pwd)
{
    bool ret_val = false;

    //std::shared_lock<std::shared_mutex> rlk(auth_info_s_rw_mtx_);
    auth_info_s_rw_mtx_.lock_shared();
    ret_val = auth_pwd_custom(auth_info_s_, pwd);
    auth_info_s_rw_mtx_.unlock_shared();

    return ret_val;
}

bool sync_auth_info::auth_pwd_custom(const std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>>& auth_info_s, uint32_t pwd)
{
    return get_auth_info_custom(auth_info_s, pwd) != NULL;
}

#if 0
auth_info_t* sync_auth_info::get_auth_info(uint32_t pwd)
{
    return get_auth_info_custom(auth_info_s_, pwd);
}
#endif

auth_info_t* sync_auth_info::get_auth_info_custom(const std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>>& auth_info_s, uint32_t pwd)
{
    assert(auth_info_s != nullptr);
    auto it = auth_info_s->find(pwd);
    if (it == auth_info_s->end())
    {
        return NULL;
    }

    return &it->second;
}

bool sync_auth_info::auth_pwd_and_user_name(uint32_t pwd, const std::string& user_name)
{
    bool ret_val = false;

    //std::shared_lock<std::shared_mutex> rlk(auth_info_s_rw_mtx_);
    auth_info_s_rw_mtx_.lock_shared();
    ret_val = auth_pwd_and_user_name_custom(auth_info_s_, pwd, user_name);
    auth_info_s_rw_mtx_.unlock_shared();

    return ret_val;
}

bool sync_auth_info::auth_pwd_and_user_name_custom(const std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>>& auth_info_s, uint32_t pwd, const std::string& user_name)
{
    assert(auth_info_s != nullptr);

    auth_info_t* info = get_auth_info_custom(auth_info_s, pwd);
    if (info == NULL)
    {
        return false;
    }

    if (info->user_name != user_name)
    {
        return false;
    }

    return true;
}

bool sync_auth_info::get_bind_addr_and_user_name(uint32_t pwd, bool is_ipv6, sockaddr_storage& addr, std::string& user_name)
{
    bool ret_val = false;

    //std::shared_lock<std::shared_mutex> rlk(auth_info_s_rw_mtx_);
    auth_info_s_rw_mtx_.lock_shared();
    ret_val = get_bind_addr_and_user_name_custom(auth_info_s_, pwd, is_ipv6, addr, user_name);
    auth_info_s_rw_mtx_.unlock_shared();

    return ret_val;
}

bool sync_auth_info::get_bind_addr_and_user_name_custom(const std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>>& auth_info_s,
    uint32_t pwd, bool is_ipv6, sockaddr_storage& addr, std::string& user_name)
{
    assert(auth_info_s != nullptr);

    auth_info_t* info = get_auth_info_custom(auth_info_s, pwd);
    if (info == NULL)
    {
        return false;
    }

    addr = is_ipv6 ? info->bind_local_ipv6_addr : info->bind_local_ipv4_addr;
    user_name = info->user_name;
    return true;
}

std::string sync_auth_info::get_user_name(const std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>>& auth_info_s, uint32_t pwd)
{
    assert(auth_info_s != nullptr);

    auth_info_t* info = get_auth_info_custom(auth_info_s, pwd);
    if (info == NULL)
    {
        return std::string();
    }

    return info->user_name;
}

bool sync_auth_info::get_bind_addr(const std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>>& auth_info_s, uint32_t pwd, bool is_ipv6, sockaddr_storage& addr)
{
    assert(auth_info_s != nullptr);

    auth_info_t* info = get_auth_info_custom(auth_info_s, pwd);
    if (info == NULL)
    {
        return false;
    }

    addr = is_ipv6 ? info->bind_local_ipv6_addr : info->bind_local_ipv4_addr;
    return true;
}
