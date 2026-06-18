#pragma once
#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "auth_info.h"
#include "epoll_wrap.h"
#include "file_rwlock.h"

class sync_auth_info : public epoll_wrap
{
public:
    sync_auth_info(const std::string& auth_fpath);
    ~sync_auth_info();

    void start();
    void run();
    void stop();
    void wait_stop();

    virtual void before_handle_epoll_wait();
    virtual void handle_epoll_wait(uint32_t events, void* context);

    bool update_auth_info_s(timespec& pre_update_time);
    bool auth_pwd(uint32_t pwd);
    bool auth_pwd_and_user_name(uint32_t pwd, const std::string& user_name);
    bool get_bind_addr_and_user_name(uint32_t pwd, bool is_ipv6, sockaddr_storage& addr, std::string& user_name);

    bool update_auth_info_s_custom(std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>>& auth_info_s, timespec& pre_update_time);
    static bool auth_pwd_custom(const std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>>& auth_info_s, uint32_t pwd);
    static bool auth_pwd_and_user_name_custom(const std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>>& auth_info_s, uint32_t pwd, const std::string& user_name);
    static bool get_bind_addr_and_user_name_custom(const std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>>& auth_info_s,
        uint32_t pwd, bool is_ipv6, sockaddr_storage& addr, std::string& user_name);
    static std::string get_user_name(const std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>>& auth_info_s, uint32_t pwd);
    static bool get_bind_addr(const std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>>& auth_info_s, uint32_t pwd, bool is_ipv6, sockaddr_storage& addr);

private:
    bool init_fanotify();
    void uninit_fanotify();
    bool handle_fanotify_events();
    const char* sync_auth_get_print_prefix(bool update = false);
    static std::string get_cur_date_string();
    void auth_file_check_and_handle_change();
    int check_file_change(const std::string& fpath, timespec& last_mtim, off_t& last_size);
    bool sync_auth_info_s();
    bool parse_auth_info(const std::string& line);
#if 0
    auth_info_t* get_auth_info(uint32_t pwd);
#endif
    static auth_info_t* get_auth_info_custom(const std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>>& auth_info_s, uint32_t pwd);
private:
    std::thread       thread_;
    std::atomic<bool> stop_;
    const std::string auth_fpath_;
    timespec          last_update_time_;
    //timespec          old_sys_ts_;
    timespec          cur_sys_ts_;

    timespec          file_last_mtim_;
    off_t             file_last_size_;

    std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>> auth_info_s_;
    std::shared_ptr<std::unordered_map<uint32_t, auth_info_t>> auth_info_s_tmp_;
    mutable std::shared_mutex auth_info_s_rw_mtx_;

    bool first_work_;
    int notify_fd_;

    size_t                  buf_size_;
    std::vector<uint8_t>    buf_;
    ssize_t                 data_len_;

    file_rwlock             rwlock_;
};
