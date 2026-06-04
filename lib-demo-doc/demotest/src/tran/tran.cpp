#include "tran.h"
#include "configreader.h"
#include "errorcode.h"
#include "macrodefine.h"
#include "goodtp.h"
#include "cos.h"

#ifdef __linux__
#include <pthread.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#endif

#if (_WIN32 || _WIN64)
#include <time.h>
#include <shellapi.h>
#include <WS2tcpip.h>
#include <process.h>
#include <winSock2.h>
#include <mswsock.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Kernel32.lib")
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <vector>
#include <string>

using namespace std;

#define FRAME_HEADER             (0xa5a55a5a)

#define C3PTP_TIMER_PERIOD_US    (1000)
#define SND_RCV_SOCKET_BUF_SZ    (1024 << 11)
#define CHECK_RESOURCE_PERIOD_US (1000)

enum class FrameType:u32 {
    kFrameDataType      = 1,
    kFrameEchoTypeReq   = 2,
    kFrameEchoTypeRes   = 3,
    kFrameFileTranStart = 4,
    kFrameFileTran      = 5,
    kFrameFileTranEnd   = 6,
    kFrameCmdCrtFile    = 7,
    kFrameCmdCrtFileOk  = 8,
    kFrameCmdReqTranDt  = 9,
    kFrameCmdResTranDt  = 10,

    kFrameTypeButt
};

typedef enum _TranMachinState {
    kIdle = 0,
    kReqCreateFile = 1,
    kResCreateFile = 2,
    kReqTranData   = 3,
    kResTranData   = 4,
    kTranDataRun   = 5
}TranMachinState;

typedef struct _GtpTesterFrame {
    u32 header_;
    u32 length_;
    u32 type_;
    u32 sn_;

    u64 sender_ts_us_;
    u64 check_sum_;

    u8  payload_[0];
}GtpTesterFrame;

typedef struct _EpollThreadMgr {
    vector<TranEpoll*> epoll_obj_;

    rolerEnum roler_;
    void *cos_mutex_;
    cos_tid tid_;
}EpollThreadMgr;

typedef void *(*linuxThrdFunc)(void*);

_StatResult::_StatResult() {
    Clear();
}

void _StatResult::Clear(void) {
    max_value_      = 0;
    min_value_      = 0;
    avg_value_      = 0;
    std_value_      = 0;
    one_std_rate_   = 0.0;
    two_std_rate_   = 0.0;
    three_std_rate_ = 0.0;
    other_std_rate_ = 0.0;

    return;
}

template <typename T1, typename T2>
void _StatResult::DoStat(const T1 data_vec[], const T2 &data_num) {
    Clear();

    if (0 == data_num) {
        return;
    }

    min_value_ = 0xFFFFFFFF;

    u32 temp_data;
    T2  nloop     = 0;
    for (nloop = 0; data_num > nloop; ++nloop) {
        temp_data = (u32)data_vec[nloop];

        avg_value_ += temp_data;

        if (temp_data < min_value_) {
            min_value_ = temp_data;
        }

        if (temp_data > max_value_) {
            max_value_ = temp_data;
        }
    }

    avg_value_ /= ((u32)data_num);

    u32 delta;
    for (nloop = 0; data_num > nloop; ++nloop) {
        temp_data = (u32)data_vec[nloop];

        if (temp_data < avg_value_) {
            delta = avg_value_ - temp_data;
        } else {
            delta = temp_data - avg_value_;
        }

        std_value_ += (delta * delta);
    }

    std_value_ /= data_num;
    std_value_  = (u32)sqrt(std_value_);

    u32 two_std_value     = std_value_ << 1;
    u32 three_std_value   = std_value_ * 3;

    for (nloop = 0; data_num > nloop; ++nloop) {
        temp_data = (u32)data_vec[nloop];

        if (temp_data < avg_value_) {
            delta = avg_value_ - temp_data;
        } else {
            delta = temp_data - avg_value_;
        }

        if (std_value_ >= delta) {
            one_std_rate_ += 1.0;
        } else if (two_std_value >= delta) {
            two_std_rate_ += 1.0;
        } else if (three_std_value >= delta) {
            three_std_rate_ += 1.0;
        } else {
            other_std_rate_ += 1.0;
        }
    }

    one_std_rate_   = (f32)((one_std_rate_ * 100.0) / ((f32)((u32)data_num)));
    two_std_rate_   = (f32)((two_std_rate_ * 100.0) / ((f32)((u32)data_num)));
    three_std_rate_ = (f32)((three_std_rate_ * 100.0) / ((f32)((u32)data_num)));
    other_std_rate_ = (f32)((other_std_rate_ * 100.0) / ((f32)((u32)data_num)));

    return;
}

const _StatResult _StatResult::operator +(const _StatResult &old) const {
    _StatResult temp;

    temp.max_value_      = this->max_value_ + old.max_value_;
    temp.min_value_      = this->min_value_ + old.min_value_;
    temp.avg_value_      = this->avg_value_ + old.avg_value_;
    temp.std_value_      = this->std_value_ + old.std_value_;
    temp.one_std_rate_   = this->one_std_rate_ + old.one_std_rate_;
    temp.two_std_rate_   = this->two_std_rate_ + old.two_std_rate_;
    temp.three_std_rate_ = this->three_std_rate_ + old.three_std_rate_;
    temp.other_std_rate_ = this->other_std_rate_ + old.other_std_rate_;

    return temp;
}

const _StatResult& _StatResult::operator +=(const _StatResult &old) {
    this->max_value_      += old.max_value_;
    this->min_value_      += old.min_value_;
    this->avg_value_      += old.avg_value_;
    this->std_value_      += old.std_value_;
    this->one_std_rate_   += old.one_std_rate_;
    this->two_std_rate_   += old.two_std_rate_;
    this->three_std_rate_ += old.three_std_rate_;
    this->other_std_rate_ += old.other_std_rate_;

    return *this;
}

const _StatResult _StatResult::operator /(const u32 &n) const {
    if (2 > n) {
        return *this;
    }

    _StatResult temp;

    temp.max_value_      = this->max_value_ / n;
    temp.min_value_      = this->min_value_ / n;
    temp.avg_value_      = this->avg_value_ / n;
    temp.std_value_      = this->std_value_ / n;
    temp.one_std_rate_   = this->one_std_rate_ / n;
    temp.two_std_rate_   = this->two_std_rate_ / n;
    temp.three_std_rate_ = this->three_std_rate_ / n;
    temp.other_std_rate_ = this->other_std_rate_ / n;

    return temp;
}

const _StatResult& _StatResult::operator /=(const u32 &n) {
    this->max_value_      /= n;
    this->min_value_      /= n;
    this->avg_value_      /= n;
    this->std_value_      /= n;
    this->one_std_rate_   /= n;
    this->two_std_rate_   /= n;
    this->three_std_rate_ /= n;
    this->other_std_rate_ /= n;

    return *this;
}
#ifdef __cplusplus
extern "C" {
#endif
EpollThreadMgr g_epoll_thread_mgr;
EpollThreadMgr g_single_epoll_thread_mgr;

FILE * g_frame_fp = NULL;

u64 g_tester_id_bit_map = 0;

_ConsumeTime::_ConsumeTime() {
    clear();
}

void _ConsumeTime::clear(void) {
    memset(used_time_us_, 0x00, (RCD_CONSUME_BUF_SIZE << 1));
    current_record_pos_ = 0;
    current_record_num_ = 0;
}

void _ConsumeTime::update(const u16 &delta_tm) {
    used_time_us_[current_record_pos_] = delta_tm;

    current_record_pos_ += 1;
    if (RCD_CONSUME_BUF_SIZE <= current_record_pos_) {
        current_record_pos_ = 0;
    }

    if (RCD_CONSUME_BUF_SIZE > current_record_num_) {
        current_record_num_ += 1;
    }
}

void _ConsumeTime::doStat(void) {
    stat_result_.DoStat(used_time_us_, current_record_num_);

    return;
}

const StatResult& _ConsumeTime::getStatResult(void) const {
    return stat_result_;
}

u64 CalcCheckSum(u8 *data, const u32 &size) {
    u64 check_sum = 0;

    u32 i = 0;
    while (size > i) {
        check_sum += (*(data + i));
        i += 1;
    }

    return check_sum;
}

u32 MonitorSingleTesterThreadEntry(void *param) {
    cos_lock(g_single_epoll_thread_mgr.cos_mutex_);

    auto itr = g_single_epoll_thread_mgr.epoll_obj_.begin();

    while (g_single_epoll_thread_mgr.epoll_obj_.end() != itr) {
        if (COS_YES == (*itr)->epoll_thread_exited_flag_) {
            delete (*itr);
            itr = g_single_epoll_thread_mgr.epoll_obj_.erase(itr);
            continue;
        }

        ++itr;
    }

    if (0 == g_single_epoll_thread_mgr.epoll_obj_.size()) {
        vector<TranEpoll*>tmp;
        g_single_epoll_thread_mgr.epoll_obj_.swap(tmp);
        g_single_epoll_thread_mgr.epoll_obj_.reserve(10240);
    }

    cos_unlock(g_single_epoll_thread_mgr.cos_mutex_);

    cos_threadSleep(50);

    return COS_OK;
}

u32 GenTesterId(void) {
    u64 bit_value = 1;
    u32 tester_id = 0;

    while ((64 > tester_id) && (0 != (g_tester_id_bit_map & bit_value))) {
        tester_id  += 1;
        bit_value <<= 1;
    }

    g_tester_id_bit_map |= bit_value;

    tester_id += 1;  // id isn't 0;

    return tester_id;
}

void DeleteTesterId(u32 tester_id) {
    if (0 < tester_id) {
        tester_id -= 1;
    }

    u64 bit_value = 1;
    bit_value <<= (tester_id);

    g_tester_id_bit_map &= (~bit_value);

    return;
}

u32 SendGtpPacket(GtpHandler_p c3ptp_hdl, void *pack, u32 size, GtpAddr *tran_addr) {
    TranEpoll *epoll_obj = reinterpret_cast<TranEpoll*>(tran_addr->context_);

    if (COS_YES == epoll_obj->consume_switch_) {
        u64 delta_us = epoll_obj->current_date_.timestamp_us;

        cos_getCurSysTime(&(epoll_obj->current_date_));
        delta_us = epoll_obj->current_date_.timestamp_us - delta_us;
        epoll_obj->goodtp_send_consume_.update((u16)delta_us);
    } else {
        #ifdef _SELFDEBUG
        cos_getCurSysTime(&(epoll_obj->current_date_));
        #endif
    }

    u32 nret = (u32)sendto(tran_addr->sfd_, (char*)pack, size, 0, (struct sockaddr *)(&(tran_addr->sock_addr_[0])),
                           tran_addr->sock_addr_len_);

    if (COS_YES == epoll_obj->consume_switch_) {
        u64 delta_us = epoll_obj->current_date_.timestamp_us;
        cos_getCurSysTime(&(epoll_obj->current_date_));
        delta_us = epoll_obj->current_date_.timestamp_us - delta_us;
        epoll_obj->sock_send_consume_.update((u16)delta_us);

        if (300 <= delta_us) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_warning,
                    "call sendto() too long(%lluus) in No.%u epoll thread.\r\n", delta_us, epoll_obj->thread_id_);
        }
    } else {
        #ifdef _SELFDEBUG
        u64 delta_us = epoll_obj->current_date_.timestamp_us;
        cos_getCurSysTime(&(epoll_obj->current_date_));
        delta_us = epoll_obj->current_date_.timestamp_us - delta_us;

        if (300 <= delta_us) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_warning,
                    "call sendto() too long(%lluus) in No.%u epoll thread.\r\n", delta_us, epoll_obj->thread_id_);
        }
        #endif
    }

    if (size != nret) {
        u8 s_ip[64] = {0};
        u16 s_port  = 0;

        cos_sockAddrToStrAddr(&(tran_addr->sock_addr_[0]), cos_ip_addr_type_ipv4, &(s_ip[0]), 64, &s_port);

        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error, "call sendto() failed(nret=%u size=%u 0x%08x "\
                "0x%08x 0x%04x sfd=%u server=%s:%u errorcode=%u(%s)) in No.%u epoll thread.\r\n", nret, size,
                *((u32*)pack), *(((u32*)pack) + 1), (u32)(*(((u16*)pack) + 4)), tran_addr->sfd_, (char*)(&(s_ip[0])),
                (u32)s_port, errno, strerror(errno), epoll_obj->thread_id_);
    }

    return COS_OK;
}

FILE *g_bin_wfp = NULL;
FILE *g_svr_wfp = NULL;

u32 ReceiveFrame(GtpHandler_p c3ptp_hdl, void *frame, u32 size, GtpAddr *tran_addr) {
    if (sizeof(GtpTesterFrame) > size) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error, "Invalid frame size(%u) too small.\r\n", size);
        return COS_OK;
    }

    TranEpoll *epoll_obj = reinterpret_cast<TranEpoll*>(tran_addr->context_);

    if (COS_YES == epoll_obj->consume_switch_) {
        u64 delta_us = epoll_obj->current_date_.timestamp_us;
        cos_getCurSysTime(&(epoll_obj->current_date_));
        delta_us = epoll_obj->current_date_.timestamp_us - delta_us;
        epoll_obj->goodtp_recv_consume_.update((u16)delta_us);
    }

    epoll_obj->proced_frame_flag_ = COS_YES;

    GtpTesterFrame *frame_header= (GtpTesterFrame*)frame;

    if (FRAME_HEADER != frame_header->header_) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "Invalid frame header(0x%08x).\r\n", frame_header->header_);
        return COS_OK;
    }

    if ((0 == frame_header->type_) || (((u32)(FrameType::kFrameTypeButt)) <= frame_header->type_)) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "Invalid frame type(%u).\r\n", frame_header->type_);
        return COS_OK;
    }

    if (size != frame_header->length_) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error, "Invalid frame size(%u != %u).\r\n",
        frame_header->length_, size);
        return COS_OK;
    }

    u64 old_check_sum = frame_header->check_sum_;

    frame_header->check_sum_ = 0;

    frame_header->check_sum_ = CalcCheckSum((u8*)frame_header, size);
    if (old_check_sum != frame_header->check_sum_) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error, "don't pass check sum(old=%llu now=%llu).\r\n",
                (unsigned long long)old_check_sum, (unsigned long long)(frame_header->check_sum_));
        return COS_OK;
    }

    if (((u32)(FrameType::kFrameCmdCrtFile)) == frame_header->type_) {
        if (0 == frame_header->sn_) {
            if (NULL != g_svr_wfp) {
                fclose(g_svr_wfp);
                g_svr_wfp = NULL;
            }
            g_svr_wfp = fopen("/run/frame.log", "w");
        }

        u8 *pack_mem = NULL;
        u32 mem_len  = 0;

        GtpAddr *tmp_addr = NULL;

        pack_mem = GtpMallocPackMem(epoll_obj->goodtp_hdl_, NULL, 0, &mem_len, (void**)(&tmp_addr), size);

        memcpy(pack_mem, frame, size);
        memcpy(tmp_addr, tran_addr, sizeof(GtpAddr));

        tmp_addr->qos_           = FEC_SWITCH;  // turn on fec.
        tmp_addr->stream_type_   = GTP_STREAM_TYPE;
        tmp_addr->smooth_jitter_ = GTP_REMOVE_JITTER;

        frame_header = (GtpTesterFrame*)pack_mem;

        frame_header->type_ = (u32)(FrameType::kFrameCmdCrtFileOk);

        if (COS_YES == epoll_obj->consume_switch_) {
            cos_getCurSysTime(&(epoll_obj->current_date_));
        }

        frame_header->check_sum_ = 0;
        frame_header->check_sum_ = CalcCheckSum((u8*)frame_header, size);

        u32 run_result = GtpFrameSend(epoll_obj->goodtp_hdl_, pack_mem, size, tmp_addr, 0, 0);
        
        if (COS_OK != run_result) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                    "Call GtpFrameSend() failed(0x%08x).\r\n", run_result);
        }

        GtpFreePackMem(epoll_obj->goodtp_hdl_, pack_mem);

        return COS_OK;
    }

    if (((u32)(FrameType::kFrameCmdCrtFileOk)) == frame_header->type_) {
        epoll_obj->state_machin_ = kResCreateFile;
        return COS_OK;
    }

    if (((u32)(FrameType::kFrameCmdResTranDt)) == frame_header->type_) {
        epoll_obj->state_machin_ = kTranDataRun;
        return COS_OK;
    }

    if (((u32)(FrameType::kFrameCmdReqTranDt)) == frame_header->type_) {
        u8 *pack_mem = NULL;
        u32 mem_len  = 0;

        GtpAddr *tmp_addr = NULL;

        pack_mem = GtpMallocPackMem(epoll_obj->goodtp_hdl_, NULL, 0, &mem_len, (void**)(&tmp_addr), size);

        memcpy(pack_mem, frame, size);
        memcpy(tmp_addr, tran_addr, sizeof(GtpAddr));

        tmp_addr->qos_           = FEC_SWITCH;  // turn on fec.
        tmp_addr->stream_type_   = GTP_STREAM_TYPE;
        tmp_addr->smooth_jitter_ = GTP_REMOVE_JITTER;

        frame_header = (GtpTesterFrame*)pack_mem;

        frame_header->type_ = (u32)(FrameType::kFrameCmdResTranDt);

        if (COS_YES == epoll_obj->consume_switch_) {
            cos_getCurSysTime(&(epoll_obj->current_date_));
        }

        frame_header->check_sum_ = 0;
        frame_header->check_sum_ = CalcCheckSum((u8*)frame_header, size);

        u32 run_result = GtpFrameSend(epoll_obj->goodtp_hdl_, pack_mem, size, tmp_addr, 0, 0);
        
        if (COS_OK != run_result) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                    "Call GtpFrameSend() failed(0x%08x).\r\n", run_result);
        }

        GtpFreePackMem(epoll_obj->goodtp_hdl_, pack_mem);

        return COS_OK;
    }

    u32 repeat_flag = 0;
    if (COS_ON == epoll_obj->app_entry_win_switch_) {
        repeat_flag = epoll_obj->AppSnEntrySlidWin(frame_header->sn_, tran_addr);
    }

    if (((u32)(FrameType::kFrameEchoTypeRes)) == frame_header->type_) {
        cos_date_time_stru cur_date;
        cos_getCurSysTime(&cur_date);

        u32 delay_ms = (u32)((cur_date.timestamp_us - frame_header->sender_ts_us_) / 1000);
        if (epoll_obj->max_delay_ms_ < delay_ms) {
            epoll_obj->max_delay_ms_ = delay_ms;
        }

        if (epoll_obj->min_delay_ms_ > delay_ms) {
            epoll_obj->min_delay_ms_ = delay_ms;
        }

        if (epoll_obj->rtt_ms_ <= (delay_ms >> 1)) {
            epoll_obj->too_late_counter_ += 1;
        }

        if (COS_YES == epoll_obj->single_test_flag_) {
            epoll_obj->receved_data_flag_ = COS_YES;
        }

        if (COS_YES == tran_addr->stream_type_) {
            if (epoll_obj->expect_recv_sn_ != frame_header->sn_) {
                epoll_obj->loss_frame_sum_ += (frame_header->sn_ - epoll_obj->expect_recv_sn_);
                epoll_obj->expect_recv_sn_  = frame_header->sn_ + 1;
            } else {
                epoll_obj->expect_recv_sn_ += 1;
            }
        }

        if (NULL != g_frame_fp) {
            if (COS_YES != tran_addr->stream_type_) {
                fprintf(g_frame_fp, "[%04u.%02u.%02u_%02u:%02u:%02u:%03u]sn=%u delay=%ums too_late=%u rtt=%ums"\
                        "(max=%ums)%s\r\n", cur_date.ulYear, cur_date.ulMonth, cur_date.ulMday, cur_date.ulHour,
                        cur_date.ulMin, cur_date.ulSec, cur_date.ulMSec, frame_header->sn_, delay_ms,
                        epoll_obj->too_late_counter_, epoll_obj->rtt_ms_, epoll_obj->max_delay_ms_,
                        ((1 == repeat_flag) ? "(DUP)": ""));
            } else {
                fprintf(g_frame_fp, "[%04u.%02u.%02u_%02u:%02u:%02u:%03u]sn=%u loss_num=%u delay=%ums rtt=%ums "\
                        "jitter=%ums(max=%ums min=%ums)%s\r\n", cur_date.ulYear, cur_date.ulMonth, cur_date.ulMday,
                        cur_date.ulHour, cur_date.ulMin, cur_date.ulSec, cur_date.ulMSec, frame_header->sn_,
                        epoll_obj->loss_frame_sum_, delay_ms, epoll_obj->rtt_ms_,
                        epoll_obj->max_delay_ms_ - epoll_obj->min_delay_ms_, epoll_obj->max_delay_ms_,
                        epoll_obj->min_delay_ms_, ((1 == repeat_flag) ? "(DUP)": ""));
            }

            fflush(g_frame_fp);

            if (1 == repeat_flag) {
                COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_warning,
                        "sn=%u delay=%ums too_late=%u rtt=%ums(DUP)\r\n", frame_header->sn_, (u32)delay_ms,
                        epoll_obj->too_late_counter_, epoll_obj->rtt_ms_);
            }
        }

        return COS_OK;
    }

    if (((u32)(FrameType::kFrameDataType)) == frame_header->type_) {
        cos_date_time_stru cur_date;
        cos_getCurSysTime(&cur_date);

        u32 delay_ms = (u32)((cur_date.timestamp_us - frame_header->sender_ts_us_) / 1000);
        if (epoll_obj->max_delay_ms_ < delay_ms) {
            epoll_obj->max_delay_ms_ = delay_ms;
        }

        if (epoll_obj->min_delay_ms_ > delay_ms) {
            epoll_obj->min_delay_ms_ = delay_ms;
        }

        if (epoll_obj->rtt_ms_ <= (delay_ms >> 1)) {
            epoll_obj->too_late_counter_ += 1;
        }

        if (COS_YES == tran_addr->stream_type_) {
            if (epoll_obj->expect_recv_sn_ != frame_header->sn_) {
                epoll_obj->loss_frame_sum_ += (frame_header->sn_ - epoll_obj->expect_recv_sn_);
                epoll_obj->expect_recv_sn_  = frame_header->sn_ + 1;
            } else {
                epoll_obj->expect_recv_sn_ += 1;
            }
        }

        if (NULL != g_svr_wfp) {
            if (COS_YES != tran_addr->stream_type_) {
                fprintf(g_svr_wfp, "[%04u.%02u.%02u_%02u:%02u:%02u:%03u]sn=%u delay=%ums too_late=%u rtt=%ums"\
                        "(max=%ums)%s\r\n", cur_date.ulYear, cur_date.ulMonth, cur_date.ulMday, cur_date.ulHour,
                        cur_date.ulMin, cur_date.ulSec, cur_date.ulMSec, frame_header->sn_, delay_ms,
                        epoll_obj->too_late_counter_, epoll_obj->rtt_ms_, epoll_obj->max_delay_ms_,
                        ((1 == repeat_flag) ? "(DUP)": ""));
            } else {
                fprintf(g_svr_wfp, "[%04u.%02u.%02u_%02u:%02u:%02u:%03u]sn=%u loss_num=%u delay=%ums rtt=%ums "\
                        "jitter=%ums(max=%ums min=%ums)%s\r\n", cur_date.ulYear, cur_date.ulMonth, cur_date.ulMday,
                        cur_date.ulHour, cur_date.ulMin, cur_date.ulSec, cur_date.ulMSec, frame_header->sn_,
                        epoll_obj->loss_frame_sum_, delay_ms, epoll_obj->rtt_ms_,
                        epoll_obj->max_delay_ms_ - epoll_obj->min_delay_ms_, epoll_obj->max_delay_ms_,
                        epoll_obj->min_delay_ms_, ((1 == repeat_flag) ? "(DUP)": ""));
            }

            fflush(g_svr_wfp);
        }

        return COS_OK;
    }

    if (((u32)(FrameType::kFrameEchoTypeReq)) == frame_header->type_) {
        #if (1 == SERVER_RECORD_SWITCH)
        cos_date_time_stru cur_date;
        cos_getCurSysTime(&cur_date);

        u32 delay_ms = (u32)((cur_date.timestamp_us - frame_header->sender_ts_us_) / 1000);
        if (epoll_obj->max_delay_ms_ < delay_ms) {
            epoll_obj->max_delay_ms_ = delay_ms;
        }

        if (epoll_obj->min_delay_ms_ > delay_ms) {
            epoll_obj->min_delay_ms_ = delay_ms;
        }

        if (epoll_obj->rtt_ms_ <= (delay_ms >> 1)) {
            epoll_obj->too_late_counter_ += 1;
        }

        if (COS_YES == tran_addr->stream_type_) {
            if (epoll_obj->expect_recv_sn_ != frame_header->sn_) {
                epoll_obj->loss_frame_sum_ += (frame_header->sn_ - epoll_obj->expect_recv_sn_);
                epoll_obj->expect_recv_sn_  = frame_header->sn_ + 1;
            } else {
                epoll_obj->expect_recv_sn_ += 1;
            }
        }

        if (NULL != g_svr_wfp) {
            if (COS_YES != tran_addr->stream_type_) {
                fprintf(g_svr_wfp, "[%04u.%02u.%02u_%02u:%02u:%02u:%03u]sn=%u delay=%ums too_late=%u rtt=%ums"\
                        "(max=%ums)%s\r\n", cur_date.ulYear, cur_date.ulMonth, cur_date.ulMday, cur_date.ulHour,
                        cur_date.ulMin, cur_date.ulSec, cur_date.ulMSec, frame_header->sn_, delay_ms,
                        epoll_obj->too_late_counter_, epoll_obj->rtt_ms_, epoll_obj->max_delay_ms_,
                        ((1 == repeat_flag) ? "(DUP)": ""));
            } else {
                fprintf(g_svr_wfp, "[%04u.%02u.%02u_%02u:%02u:%02u:%03u]sn=%u loss_num=%u delay=%ums rtt=%ums "\
                        "jitter=%ums(max=%ums min=%ums)%s\r\n", cur_date.ulYear, cur_date.ulMonth, cur_date.ulMday,
                        cur_date.ulHour, cur_date.ulMin, cur_date.ulSec, cur_date.ulMSec, frame_header->sn_,
                        epoll_obj->loss_frame_sum_, delay_ms, epoll_obj->rtt_ms_,
                        epoll_obj->max_delay_ms_ - epoll_obj->min_delay_ms_, epoll_obj->max_delay_ms_,
                        epoll_obj->min_delay_ms_, ((1 == repeat_flag) ? "(DUP)": ""));
            }

            fflush(g_svr_wfp);
        }
        #endif

        u8 *pack_mem = NULL;
        u32 mem_len  = 0;

        GtpAddr *tmp_addr = NULL;

        pack_mem = GtpMallocPackMem(epoll_obj->goodtp_hdl_, NULL, 0, &mem_len, (void**)(&tmp_addr), size);

        memcpy(pack_mem, frame, size);
        memcpy(tmp_addr, tran_addr, sizeof(GtpAddr));

        tmp_addr->qos_           = FEC_SWITCH;  // turn on fec.
        tmp_addr->stream_type_   = GTP_STREAM_TYPE;
        tmp_addr->smooth_jitter_ = GTP_REMOVE_JITTER;

        frame_header = (GtpTesterFrame*)pack_mem;

        frame_header->type_ = (u32)(FrameType::kFrameEchoTypeRes);

        if (COS_YES == epoll_obj->consume_switch_) {
            cos_getCurSysTime(&(epoll_obj->current_date_));
        }

        frame_header->check_sum_ = 0;
        frame_header->check_sum_ = CalcCheckSum((u8*)frame_header, size);

        u32 run_result = GtpFrameSend(epoll_obj->goodtp_hdl_, pack_mem, size, tmp_addr, 0, 0);
        
        if (COS_OK != run_result) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                    "Call GtpFrameSend() failed(0x%08x).\r\n", run_result);
        }

        GtpFreePackMem(epoll_obj->goodtp_hdl_, pack_mem);

        return COS_OK;
    }

    if (((u32)(FrameType::kFrameFileTran)) == frame_header->type_) {
        if (NULL == g_bin_wfp) {
            #ifdef __linux__
            g_bin_wfp = fopen("data.bin", "wb");
            #endif

            #if (_WIN32 || _WIN64)
            g_bin_wfp = _wfopen(L"..\\bin\\data.bin", L"wb");
            #endif

            if (NULL == g_bin_wfp) {
                COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error, "Create binary file failed.\r\n");
                return COS_OK;
            }
        }

        fwrite(frame_header->payload_, frame_header->length_ - sizeof(GtpTesterFrame), 1, g_bin_wfp);
        fflush(g_bin_wfp);
    }

    if (((u32)(FrameType::kFrameFileTranStart)) == frame_header->type_) {
        if (NULL != g_bin_wfp) {
            fclose(g_bin_wfp);
            g_bin_wfp = NULL;
        }

        #ifdef __linux__
        g_bin_wfp = fopen("data.bin", "wb");
        #endif

        #if (_WIN32 || _WIN64)
        g_bin_wfp = _wfopen(L"..\\bin\\data.bin", L"wb");
        #endif

        if (NULL == g_bin_wfp) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error, "Create binary file failed.\r\n");
        }

        return COS_OK;
    }

    if (((u32)(FrameType::kFrameFileTranEnd)) == frame_header->type_) {
        
        if (NULL != g_bin_wfp) {
            fclose(g_bin_wfp);
            g_bin_wfp = NULL;
        }
    }

    return COS_OK;
}

u32 ReportLinkerQuality(GtpHandler_p c3ptp_hdl, GtpLinkQuality vec[], u32 size) {
    TranEpoll *epoll_obj;
    u32 nloop = 0;

    while (size > nloop) {
        epoll_obj = (TranEpoll*)(vec[nloop].context_);

        if (COS_YES == epoll_obj->consume_switch_) {
            u64 delta_us = epoll_obj->current_date_.timestamp_us;
            cos_getCurSysTime(&(epoll_obj->current_date_));
            delta_us = epoll_obj->current_date_.timestamp_us - delta_us;
            epoll_obj->goodtp_recv_consume_.update((u16)delta_us);
        }

        epoll_obj->proced_frame_flag_ = COS_YES;

        epoll_obj->SaveLinkerQuality(vec[nloop]);

        nloop += 1;
    }

    return COS_OK;
}

u32 CreateListenSocket(const string &ip, u32 port, cos_sock *sfd, void *pepoll_id, u32 ip_port_reuse_flag) {
    u32 ip_addr_type = cos_checkStrIpAddrType((const u8*)ip.c_str());  // NOLINT
    if (cos_ip_addr_type_butt <= ip_addr_type) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "Invalid IP(%s) when create listen socket.\r\n", ip.c_str());

        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kIpFormatErr);
    }

    cos_sock tmp_sfd = cos_openSock(ip_addr_type, cos_socket_type_udp);
    if (COS_INVALID_HDL == tmp_sfd) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "Call cos_openSock() failed(errorcode= %u(%s)) when create listen socket(%s).\r\n",
                errno, strerror(errno), ip.c_str());
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, cos_open_sock_failed_err);
    }

    cos_setSockNoBlock(tmp_sfd);

    u32 ret = COS_OK;

    #ifdef __linux__
    if (COS_YES == ip_port_reuse_flag) {
        u32 reuse = 1;
        if (setsockopt(tmp_sfd, SOL_SOCKET, SO_REUSEPORT, (void *)&reuse, sizeof(reuse)) == -1) {  // NOLINT
            cos_closeSock(tmp_sfd);
            tmp_sfd = COS_INVALID_HDL;

            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                    "call setsockopt() failed(errorcode= %d(%s))\r\n", errno, strerror(errno));
            RETURN_ERRNO(moduleIdEnum::kModuleEpollId, ((u32)(C3coreErrorCodeEnum::kSetSocketReuseErr)));
        }
    }
    #endif

    u32 tmp_socket_buffer_size = SND_RCV_SOCKET_BUF_SZ;
    setsockopt(tmp_sfd, SOL_SOCKET, SO_RCVBUF, (const char*)(&tmp_socket_buffer_size),
               sizeof(tmp_socket_buffer_size));

    for (u32 i = 0; 128 > i; ++i) {
        ret = cos_bindSock(tmp_sfd, ip_addr_type, (u8*)ip.c_str(), port);  // NOLINT
        if (COS_OK == ret) {
            break;
        }

        cos_threadSleep(10);
    }

    if (COS_OK != ret) {
        cos_closeSock(tmp_sfd);
        tmp_sfd = COS_INVALID_HDL;

        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "Call cos_bindSock() failed(0x%08x(errorcode= %u(%s))) when create listen socket(%s).\r\n",
                ret, errno, strerror(errno), ip.c_str());
        return ret;
    }

    #ifdef __linux__
    struct epoll_event event;
    event.data.fd = (i32)(tmp_sfd);
    event.events  = EPOLLIN;

    ret = (u32)epoll_ctl(*((i32*)pepoll_id), EPOLL_CTL_ADD, (i32)(tmp_sfd), &event);
    #endif

    #if (_WIN32 || _WIN64)
    FD_SET(tmp_sfd, (fd_set*)pepoll_id);
    ret = COS_OK;
    #endif

    if (COS_OK != ret) {
        cos_closeSock(tmp_sfd);
        tmp_sfd = COS_INVALID_HDL;

        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call epoll_ctl() failed(%d errorcode= %d(%s)) when create listen socket(%s).\r\n",
                ret, errno, strerror(errno), ip.c_str());
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, cos_epoll_add_ctrl_failed_err);
    }

    *sfd = tmp_sfd;

    return COS_OK;
}

void AppReportQuality(slid_win_hdl win_hdl, void *cur_cntxt_hdl, const u32 &avg_rtt_us, const u32 &jitter_us,
                      const f32 &loss, const DiscardDirectU32 &discard_dir, const u32 &pre_congest_rank,
                      const u32 &rto_us, const u32 &avg_pps, const u32 &loss_num) {
    return;
    AppSlidWinInfo *app_slid_win_info = (AppSlidWinInfo*)cur_cntxt_hdl;

    app_slid_win_info->loss_ = loss;
    app_slid_win_info->pps_  = avg_pps;

    return;
}

void AppBitMapReceive(slid_win_hdl win_hdl, void *cur_cntxt_hdl,const void *bit_map, const u32 &ack_or_nack) {
    return;
    AppSlidWinInfo *app_slid_win_info = (AppSlidWinInfo*)cur_cntxt_hdl;

    if (kAckType == ack_or_nack) {
        const ReceivedSnBitMap *sn_bit_map = (ReceivedSnBitMap*)bit_map;

        app_slid_win_info->last_bitmap_header_sn_ = sn_bit_map->head_sn_;
        app_slid_win_info->last_bitmap_tail_sn_   = sn_bit_map->tail_sn_;

        if (sizeof(app_slid_win_info->bitmap_) >= sn_bit_map->mem_size_) {
            u32 delta_sn = 0;
            if (sn_bit_map->head_sn_ <= sn_bit_map->tail_sn_) {
                delta_sn = sn_bit_map->tail_sn_ - sn_bit_map->head_sn_;
            } else {
                delta_sn = 0xFFFFFFFF - sn_bit_map->head_sn_ + sn_bit_map->tail_sn_;
            }

            if (0 == (delta_sn & 0x00000003F)) {
                app_slid_win_info->bitmap_size_ = (delta_sn >> 6);
            } else {
                app_slid_win_info->bitmap_size_ = (delta_sn >> 6) + 1;
            }

            memcpy(app_slid_win_info->bitmap_, sn_bit_map->sn_bit_map_, sn_bit_map->mem_size_);
        } else {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_warning, "bit map size is too long!\r\n");
        }
        return;
    }

    return;
}

TranEpoll::TranEpoll(const rolerEnum &roler, const u32 &send_pps, const u32 &thread_id, const vector<string> &listen_ip,
                 const u32 &start_port, const u32 &end_port, const string &send_ip, FILE *pf, const u32 &auto_show):
    roler_(roler), send_pps_(send_pps), thread_id_(thread_id), goodtp_hdl_(INVALID_GTP_HANDLER),
    send_sfd_(COS_INVALID_HDL), listen_ip_(listen_ip), start_port_(start_port), end_port_(end_port),
    udp_receive_failed_dot_(NULL), gtp_buf_malloc_failed_dot_(NULL), goodtp_rcv_pack_err_dot_(NULL),
    goodtp_snd_frame_err_dot_(NULL), goodtp_timer_failed_dot_(NULL), socket_send_failed_dot_(NULL), bin_pf_(pf),
    sending_frame_ts_us_(0), send_period_ts_us_(0), call_goodtp_timer_ts_us_(0), auto_show_(auto_show),
    server_sock_addr_size_(SOCKET_ADDR_SIZE), backup_server_sock_addr_size_(SOCKET_ADDR_SIZE),
    consume_switch_(COS_OFF), send_ip_(send_ip), measure_pack_num_(10), application_sn_(0),
    proced_frame_flag_(COS_NO), app_entry_win_switch_(COS_ON), rtt_ms_(10000), too_late_counter_(0),
    max_delay_ms_(0), min_delay_ms_(0xFFFFFFFF), loss_frame_sum_(0), expect_recv_sn_(0) {
    single_test_flag_  = COS_NO;
    receved_data_flag_ = COS_NO;

    state_machin_ = kIdle;

    #ifdef __linux__
    epoll_id_ = -1;
    #endif

    #if (_WIN32 || _WIN64)
    FD_ZERO(&epoll_id_);
    #endif

    listen_sfd_.reserve((end_port - start_port + 1) << 1);

    if (0 != send_pps_) {
        send_period_ts_us_ = (u64)(1000000.0 / ((f64)send_pps_));

        cos_printf("send_period=%lluus\r\n", send_period_ts_us_);
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_warning, "The No.%u tester's send_period=%lluus\r\n",
                thread_id, send_period_ts_us_);
    }

    cache_64K_[0]               = 0;
    server_sock_addr_[0]        = 0;
    backup_server_sock_addr_[0] = 0;

    server_addr_size_ = &server_sock_addr_size_;
    server_addr_      = &(server_sock_addr_[0]);

    if (rolerEnum::kClientRoler == roler) {
        u32 run_result = cos_checkStrIpAddrType((u8*)(listen_ip[0].c_str()));
        switch (run_result) {
        case cos_ip_addr_type_ipv4:
        {
            struct sockaddr_in *addr = (struct sockaddr_in*)server_sock_addr_;

            inet_pton(AF_INET, (char*)(listen_ip[0].c_str()), (void*)(&(addr->sin_addr)));

            addr->sin_family = AF_INET;
            addr->sin_port   = htons((u16)start_port);

            server_sock_addr_size_ = sizeof(struct sockaddr_in);

            break;
        }

        case cos_ip_addr_type_ipv6:
        {
            struct sockaddr_in6 *addr = (struct sockaddr_in6*)server_sock_addr_;

            inet_pton(AF_INET6, (char*)(listen_ip[0].c_str()), (void *)(&(addr->sin6_addr)));

            addr->sin6_family = AF_INET6;
            addr->sin6_port   = htons((u16)start_port);

            server_sock_addr_size_ = sizeof(struct sockaddr_in6);

            break;
        }

        default:
        {
            cos_printf("The No.%u tester's the server's ip address is invalid(%s).\r\n", thread_id,
                       listen_ip[0].c_str());
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_warning,
                    "The No.%u tester's the server's ip address is invalid(%s).\r\n", thread_id, listen_ip[0].c_str());
        }
        }

        {
        struct sockaddr_in *addr = (struct sockaddr_in*)backup_server_sock_addr_;

        #if (1 == ENABLE_PATH_SWITCH)
        inet_pton(AF_INET, "172.20.2.156", (void*)(&(addr->sin_addr)));
        #else
        inet_pton(AF_INET, (char*)(listen_ip[0].c_str()), (void*)(&(addr->sin_addr)));
        #endif

        addr->sin_family = AF_INET;
        addr->sin_port   = htons((u16)start_port);

        backup_server_sock_addr_size_ = sizeof(struct sockaddr_in);
        }
    }

    linker_quality_map_.reserve(32);

    cos_date_time_stru date;
    cos_getCurSysTime(&date);
    win_hdl_ = CreateSlidWin(AppReportQuality, AppBitMapReceive, cos_accept3rdLogCallBack,
                             cos_getPrintLogLevel, 0, date.timestamp_us, 100000, kRecvSlidWinMode,
                             slid_win_mem_, this, (u8*)win_cache_, 1);
}

TranEpoll::~TranEpoll() {
    if (INVALID_GTP_HANDLER != goodtp_hdl_) {
        DeleteGtpInstance(goodtp_hdl_);
        goodtp_hdl_ = INVALID_GTP_HANDLER;
    }

    string dot_name;

    if (NULL != socket_send_failed_dot_) {
        dot_name = string("Epoll_") + to_string(thread_id_) + string("_socket_send_failed");
        cos_unRegisterRunningDot(dot_name.c_str());
        socket_send_failed_dot_ = NULL;
    }

    if (NULL != goodtp_tran_failed_dot_) {
        dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3ptp_tran_failed");
        cos_unRegisterRunningDot(dot_name.c_str());
        goodtp_tran_failed_dot_ = NULL;
    }

    if (NULL != goodtp_timer_failed_dot_) {
        dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3ptp_timer_failed");
        cos_unRegisterRunningDot(dot_name.c_str());
        goodtp_timer_failed_dot_ = NULL;
    }

    if (NULL != goodtp_snd_frame_err_dot_) {
        dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3ptp_frame_failed");
        cos_unRegisterRunningDot(dot_name.c_str());
        goodtp_snd_frame_err_dot_ = NULL;
    }

    if (NULL != goodtp_rcv_pack_err_dot_) {
        dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3ptp_pack_failed");
        cos_unRegisterRunningDot(dot_name.c_str());
        goodtp_rcv_pack_err_dot_ = NULL;
    }

    if (NULL != gtp_buf_malloc_failed_dot_) {
        dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3buf_failed");
        cos_unRegisterRunningDot(dot_name.c_str());
        gtp_buf_malloc_failed_dot_ = NULL;
    }

    if (NULL != udp_receive_failed_dot_) {
        dot_name = string("Epoll_") + to_string(thread_id_) + string("_udp_failed");
        cos_unRegisterRunningDot(dot_name.c_str());
        udp_receive_failed_dot_ = NULL;
    }

    if (NULL != bin_pf_) {
        fclose(bin_pf_);
        bin_pf_ = NULL;
    }

    if (NULL != g_frame_fp) {
        fclose(g_frame_fp);
        g_frame_fp = NULL;
    }

    if (NULL != g_bin_wfp) {
        fclose(g_bin_wfp);
        g_bin_wfp = NULL;
    }

    if (NULL != g_svr_wfp) {
        fclose(g_svr_wfp);
        g_svr_wfp = NULL;
    }
}

u32 TranEpoll::CreateGtpInst(void) {
    goodtp_hdl_ = CreateGtpInstance(1, 6000000, &reg_cb_, &mem_pool_cfg_);
    if (INVALID_GTP_HANDLER == goodtp_hdl_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtGtpInstErr);
    }

    return COS_OK;
}

u32 TranEpoll::Running(void) {
    reg_cb_.send_pack_cb_           = SendGtpPacket;
    reg_cb_.receive_frame_cb_       = ReceiveFrame;
    reg_cb_.report_link_quality_cb_ = /* NULL;  // */ ReportLinkerQuality;
    reg_cb_.write_log_cb_           = cos_accept3rdLoopLogCallBack;
    reg_cb_.cur_log_level_cb_       = cos_getPrintLogLevel;
    reg_cb_.close_session_cb_       = NULL;

    mem_pool_cfg_.m_256bytes_num_ = (1024 << 3);  // pc(4096) phone(1024)
    mem_pool_cfg_.m_512bytes_num_ = (1024 << 2);  // pc(2048) phone(1024)
    mem_pool_cfg_.m_1k_num_       = (1024 << 2);  // pc(512)  phone(2048)
    mem_pool_cfg_.m_1_5k_num_     = (1024 << 4);
    mem_pool_cfg_.m_4k_num_       = 512;
    mem_pool_cfg_.m_8k_num_       = 256;
    mem_pool_cfg_.m_64k_num_      = 128;

    u32 run_result = COS_OK;

    #ifdef __linux__
    pthread_t          sys_tid = 0;
    pthread_attr_t     attr;
    struct sched_param sch_param;

    run_result = pthread_attr_init(&attr);
    if (COS_OK != run_result) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call pthread_attr_init() failed(errorcode= %u(%s))\r\n", errno, strerror(errno));

        RETURN_ERRNO(((u32)(moduleIdEnum::kModuleEpollId)), cos_get_thrd_attr_failed_err);
    }

    run_result = pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
    if (COS_OK != run_result) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call pthread_attr_setscope() failed(errorcode= %u(%s))\r\n", errno, strerror(errno));
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, cos_set_thrd_attr_failed_err);
    }

    run_result = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (COS_OK != run_result) {
        pthread_attr_destroy(&attr);

        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call pthread_attr_setdetachstate() failed(errorcode= %u(%s))\r\n", errno, strerror(errno));

        RETURN_ERRNO(((u32)(moduleIdEnum::kModuleEpollId)), cos_set_thrd_attr_failed_err);
    }

    run_result = pthread_attr_setschedpolicy(&attr, SCHED_RR);
    if (COS_OK != run_result) {
        pthread_attr_destroy(&attr);

        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call pthread_attr_setschedpolicy() failed(errorcode= %u(%s))\r\n", errno, strerror(errno));

        RETURN_ERRNO(((u32)(moduleIdEnum::kModuleEpollId)), cos_set_thrd_attr_failed_err);
    }

    run_result = pthread_attr_getschedparam(&attr, &sch_param);
    if (COS_OK != run_result) {
        pthread_attr_destroy(&attr);

        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call pthread_attr_getschedparam() failed(errorcode= %u(%s))\r\n", errno, strerror(errno));

        RETURN_ERRNO(((u32)(moduleIdEnum::kModuleEpollId)), cos_get_thread_sch_info_err);
    }

    sch_param.sched_priority = 99;

    run_result = pthread_attr_setschedparam(&attr, &sch_param);
    if (COS_OK != run_result) {
        pthread_attr_destroy(&attr);

        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call pthread_attr_setschedparam() failed(errorcode= %u(%s))\r\n", errno, strerror(errno));

        RETURN_ERRNO(((u32)(moduleIdEnum::kModuleEpollId)), cos_set_thread_sch_info_err);
    }
    #endif

    string dot_name;

    dot_name = string("Epoll_") + to_string(thread_id_) + string("_udp_failed");
    udp_receive_failed_dot_ = cos_registerRunningDot(dot_name.c_str());
    if (NULL == udp_receive_failed_dot_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtRunningDotErr);
    }

    dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3buf_failed");
    gtp_buf_malloc_failed_dot_ = cos_registerRunningDot(dot_name.c_str());
    if (NULL == gtp_buf_malloc_failed_dot_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtRunningDotErr);
    }

    dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3ptp_pack_failed");
    goodtp_rcv_pack_err_dot_ = cos_registerRunningDot(dot_name.c_str());
    if (NULL == goodtp_rcv_pack_err_dot_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtRunningDotErr);
    }

    dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3ptp_frame_failed");
    goodtp_snd_frame_err_dot_ = cos_registerRunningDot(dot_name.c_str());
    if (NULL == goodtp_snd_frame_err_dot_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtRunningDotErr);
    }

    dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3ptp_timer_failed");
    goodtp_timer_failed_dot_ = cos_registerRunningDot(dot_name.c_str());
    if (NULL == goodtp_timer_failed_dot_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtRunningDotErr);
    }

    dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3ptp_tran_failed");
    goodtp_tran_failed_dot_ = cos_registerRunningDot(dot_name.c_str());
    if (NULL == goodtp_tran_failed_dot_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtRunningDotErr);
    }

    dot_name = string("Epoll_") + to_string(thread_id_) + string("_socket_send_failed");
    socket_send_failed_dot_ = cos_registerRunningDot(dot_name.c_str());
    if (NULL == socket_send_failed_dot_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtRunningDotErr);
    }

    epoll_thread_enable_flag_ = COS_YES;
    epoll_thread_runned_flag_ = COS_NO;
    epoll_thread_exited_flag_ = COS_NO;

    #ifdef __linux__
    measure_pack_num_    = 1;
    sending_frame_ts_us_ = 0;
    run_result = pthread_create(&sys_tid, &attr, (linuxThrdFunc)(TranEpoll::FileTranReceiveEntry), (void*)(this));
    pthread_attr_destroy(&attr);
    #endif

    #if (_WIN32 || _WIN64)
    cos_thread_param_stru  thrdParam;

    memset(&thrdParam, 0x00, sizeof(thrdParam));

    #ifdef _WIN64
    u64 obj_addr_buf = (u64)this;
    #else
    u32 obj_addr_buf = (u32)this;
    #endif

    thrdParam.argLen       = sizeof(obj_addr_buf);
    thrdParam.enInitStatus = cos_thread_init_status_running;
    thrdParam.nStackSize   = 0;
    thrdParam.nThrdId      = COS_INVALID_TID;
    thrdParam.nThreadPri   = 80;
    thrdParam.pThrdInParam = (void*)(&obj_addr_buf);
    thrdParam.pThrdFunc    = (cos_pThreadFunc)(TranEpoll::FileTranReceiveEntry);

    measure_pack_num_      = 1;
    sending_frame_ts_us_   = 0;

    epoll_thread_enable_flag_ = COS_YES;
    epoll_thread_runned_flag_ = COS_NO;
    epoll_thread_exited_flag_ = COS_NO;

    run_result = cos_createThread(&thrdParam, cos_thread_sched_rtos);
    #endif

    if (COS_OK != run_result) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call pthread_create() failed(errorcode= %u(%s))\r\n", errno, strerror(errno));
        return run_result;
    }

    do {
        cos_threadSleep(1);
    } while ((COS_NO == epoll_thread_runned_flag_) && (COS_NO == epoll_thread_exited_flag_));

    return COS_OK;
}

u32 TranEpoll::Running(const u32 &type) {
    reg_cb_.send_pack_cb_           = SendGtpPacket;
    reg_cb_.receive_frame_cb_       = ReceiveFrame;
    reg_cb_.report_link_quality_cb_ = /* NULL;  // */ ReportLinkerQuality;
    reg_cb_.write_log_cb_           = /* NULL;  // */ cos_accept3rdLoopLogCallBack;
    reg_cb_.cur_log_level_cb_       = /* NULL;  // */ cos_getPrintLogLevel;
    reg_cb_.close_session_cb_       = NULL;

    if (rolerEnum::kServerRoler == roler_) {
        mem_pool_cfg_.m_256bytes_num_ = (1024 << 6) + (1024 << 5);
        mem_pool_cfg_.m_512bytes_num_ = (1024 << 5) + (1024 << 4);
        mem_pool_cfg_.m_1k_num_       = (1024 << 5);
        mem_pool_cfg_.m_1_5k_num_     = (1024 << 3);
        mem_pool_cfg_.m_4k_num_       = 4096;
        mem_pool_cfg_.m_8k_num_       = 1024;
        mem_pool_cfg_.m_64k_num_      = 512;
    } else {
        mem_pool_cfg_.m_256bytes_num_ = 4096;  // pc(4096) phone(1024)
        mem_pool_cfg_.m_512bytes_num_ = 2048;  // pc(2048) phone(1024)
        mem_pool_cfg_.m_1k_num_       = 1024;  // pc(512)  phone(2048)
        mem_pool_cfg_.m_1_5k_num_     = 1024;
        mem_pool_cfg_.m_4k_num_       = 4096;
        mem_pool_cfg_.m_8k_num_       = 1024;
        mem_pool_cfg_.m_64k_num_      = 512;
    }

    u32 run_result = COS_OK;

    #ifdef __linux__
    pthread_t          sys_tid = 0;
    pthread_attr_t     attr;
    struct sched_param sch_param;

    run_result = pthread_attr_init(&attr);
    if (COS_OK != run_result) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call pthread_attr_init() failed(errorcode= %u(%s))\r\n", errno, strerror(errno));

        RETURN_ERRNO(((u32)(moduleIdEnum::kModuleEpollId)), cos_get_thrd_attr_failed_err);
    }

    run_result = pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
    if (COS_OK != run_result) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call pthread_attr_setscope() failed(errorcode= %u(%s))\r\n", errno, strerror(errno));
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, cos_set_thrd_attr_failed_err);
    }

    run_result = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (COS_OK != run_result) {
        pthread_attr_destroy(&attr);

        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call pthread_attr_setdetachstate() failed(errorcode= %u(%s))\r\n", errno, strerror(errno));

        RETURN_ERRNO(((u32)(moduleIdEnum::kModuleEpollId)), cos_set_thrd_attr_failed_err);
    }

    run_result = pthread_attr_setschedpolicy(&attr, SCHED_RR);
    if (COS_OK != run_result) {
        pthread_attr_destroy(&attr);

        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call pthread_attr_setschedpolicy() failed(errorcode= %u(%s))\r\n", errno, strerror(errno));

        RETURN_ERRNO(((u32)(moduleIdEnum::kModuleEpollId)), cos_set_thrd_attr_failed_err);
    }

    run_result = pthread_attr_getschedparam(&attr, &sch_param);
    if (COS_OK != run_result) {
        pthread_attr_destroy(&attr);

        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call pthread_attr_getschedparam() failed(errorcode= %u(%s))\r\n", errno, strerror(errno));

        RETURN_ERRNO(((u32)(moduleIdEnum::kModuleEpollId)), cos_get_thread_sch_info_err);
    }

    sch_param.sched_priority = 99;

    run_result = pthread_attr_setschedparam(&attr, &sch_param);
    if (COS_OK != run_result) {
        pthread_attr_destroy(&attr);

        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call pthread_attr_setschedparam() failed(errorcode= %u(%s))\r\n", errno, strerror(errno));

        RETURN_ERRNO(((u32)(moduleIdEnum::kModuleEpollId)), cos_set_thread_sch_info_err);
    }
    #endif

    string dot_name;

    dot_name = string("Epoll_") + to_string(thread_id_) + string("_udp_failed");
    udp_receive_failed_dot_ = cos_registerRunningDot(dot_name.c_str());
    if (NULL == udp_receive_failed_dot_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtRunningDotErr);
    }

    dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3buf_failed");
    gtp_buf_malloc_failed_dot_ = cos_registerRunningDot(dot_name.c_str());
    if (NULL == gtp_buf_malloc_failed_dot_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtRunningDotErr);
    }

    dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3ptp_pack_failed");
    goodtp_rcv_pack_err_dot_ = cos_registerRunningDot(dot_name.c_str());
    if (NULL == goodtp_rcv_pack_err_dot_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtRunningDotErr);
    }

    dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3ptp_frame_failed");
    goodtp_snd_frame_err_dot_ = cos_registerRunningDot(dot_name.c_str());
    if (NULL == goodtp_snd_frame_err_dot_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtRunningDotErr);
    }

    dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3ptp_timer_failed");
    goodtp_timer_failed_dot_ = cos_registerRunningDot(dot_name.c_str());
    if (NULL == goodtp_timer_failed_dot_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtRunningDotErr);
    }

    dot_name = string("Epoll_") + to_string(thread_id_) + string("_c3ptp_tran_failed");
    goodtp_tran_failed_dot_ = cos_registerRunningDot(dot_name.c_str());
    if (NULL == goodtp_tran_failed_dot_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtRunningDotErr);
    }

    dot_name = string("Epoll_") + to_string(thread_id_) + string("_socket_send_failed");
    socket_send_failed_dot_ = cos_registerRunningDot(dot_name.c_str());
    if (NULL == socket_send_failed_dot_) {
        RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kCrtRunningDotErr);
    }

    epoll_thread_enable_flag_ = COS_YES;
    epoll_thread_runned_flag_ = COS_NO;
    epoll_thread_exited_flag_ = COS_NO;

    #ifdef __linux__
    if (0 == type) {
        measure_pack_num_ = 10;
        run_result = pthread_create(&sys_tid, &attr, (linuxThrdFunc)(TranEpoll::ReceiveEntry), (void*)(this));
    } else {
        #if 0
        if ((0 < type) && (11 > type)) {
            measure_pack_num_ = type * 10;
        } else {
            measure_pack_num_ = 10;
        }
        #else
        measure_pack_num_ = type;
        #endif

        run_result = pthread_create(&sys_tid, &attr, (linuxThrdFunc)(TranEpoll::MeasureSpeedReceiveEnry), (void*)(this));
    }
    pthread_attr_destroy(&attr);
    #endif

    #if (_WIN32 || _WIN64)
    cos_thread_param_stru  thrdParam;

    memset(&thrdParam, 0x00, sizeof(thrdParam));

    #ifdef _WIN64
    u64 obj_addr_buf = (u64)this;
    #else
    u32 obj_addr_buf = (u32)this;
    #endif

    thrdParam.argLen       = sizeof(obj_addr_buf);
    thrdParam.enInitStatus = cos_thread_init_status_running;
    thrdParam.nStackSize   = 0;
    thrdParam.nThrdId      = COS_INVALID_TID;
    thrdParam.nThreadPri   = 80;
    thrdParam.pThrdInParam = (void*)(&obj_addr_buf);

    if (0 == type) {
        measure_pack_num_    = 10;
        thrdParam.pThrdFunc  = (cos_pThreadFunc)(TranEpoll::ReceiveEntry);
    } else {
        measure_pack_num_    = type;
        thrdParam.pThrdFunc  = (cos_pThreadFunc)(TranEpoll::MeasureSpeedReceiveEnry);
    }

    epoll_thread_enable_flag_ = COS_YES;
    epoll_thread_runned_flag_ = COS_NO;
    epoll_thread_exited_flag_ = COS_NO;

    run_result = cos_createThread(&thrdParam, cos_thread_sched_rtos);
    #endif

    if (COS_OK != run_result) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call pthread_create() failed(errorcode= %u(%s))\r\n", errno, strerror(errno));
        return run_result;
    }

    do {
        cos_threadSleep(1);
    } while ((COS_NO == epoll_thread_runned_flag_) && (COS_NO == epoll_thread_exited_flag_));

    return COS_OK;
}

void TranEpoll::Stop(void) {
    epoll_thread_enable_flag_ = COS_NO;
    return;
}

void TranEpoll::SaveLinkerQuality(const GtpLinkQuality &linker_quality) {
    LinkerKey linker_key(linker_quality);

    #ifdef _SELFDEBUG
    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_debug,
            "%s:%u<-->%s:%u rtt=%ums loss=%5.2f%% jitter=%ums.\r\n", linker_quality.self_ip_,
            (u32)(linker_quality.self_port_), linker_quality.peer_ip_, (u32)(linker_quality.peer_port_),
            (u32)(linker_quality.rtt_ms_), linker_quality.loss_, (u32)(linker_quality.rtt_jitter_ms_));
    #endif

    unordered_map<LinkerKey, LinkerQuality, LinkerKeyHash>::iterator itr = linker_quality_map_.find(linker_key);
    if (linker_quality_map_.end() == itr) {
        linker_quality_map_.emplace(linker_key, LinkerQuality(linker_quality));

        return;
    }

    if (0 != linker_quality.rtt_ms_) {
        rtt_ms_ = linker_quality.rtt_ms_;
    }

    cos_date_time_stru cur_date;
    cos_getCurSysTime(&cur_date);

    if (COS_YES == auto_show_) {
        cos_printf("%02u:%02u:%02u:%03ums rtt=%03ums loss=%5.2f rto_loss_sum=%u ack_loss_sum=%u jit=%03ums "\
                   "pre_jam_rank=%u dir=%s pos=%s\r\n", cur_date.ulHour, cur_date.ulMin, cur_date.ulSec,
                   cur_date.ulMSec, linker_quality.rtt_ms_, linker_quality.loss_, linker_quality.total_rto_loss_num_,
                   linker_quality.total_ack_loss_num_, linker_quality.rtt_jitter_ms_,linker_quality.pre_congest_rank_,
                   ((0 == linker_quality.loss_direct_) ? "up  " : "down"),
                   ((0 == linker_quality.report_pos_) ? "sender" : "receiver"));
    }

    itr->second.rtt_ms_             = linker_quality.rtt_ms_;
    itr->second.rtt_jitter_ms_      = linker_quality.rtt_jitter_ms_;
    itr->second.data_bitrate_bps_   = linker_quality.data_bitrate_bps_;
    itr->second.net_bitrate_bps_    = linker_quality.net_bitrate_bps_;
    itr->second.data_pack_pps_      = linker_quality.data_pack_pps_;
    itr->second.net_pack_pps_       = linker_quality.net_pack_pps_;
    itr->second.loss_               = linker_quality.loss_;
    itr->second.pre_congest_rank_   = linker_quality.pre_congest_rank_;
    itr->second.bitrage_chg_k_      = linker_quality.bitrage_chg_k_;
    itr->second.loss_direct_        = linker_quality.loss_direct_;
    itr->second.total_frame_num_    = linker_quality.total_frame_num_;
    itr->second.total_pack_num_     = linker_quality.total_pack_num_;
    itr->second.total_ack_num_      = linker_quality.total_ack_num_;
    itr->second.total_retran_num_   = linker_quality.total_retran_num_;
    itr->second.total_ack_loss_num_ = linker_quality.total_ack_loss_num_;
    itr->second.total_rto_loss_num_ = linker_quality.total_rto_loss_num_;
    itr->second.total_ack_err_num_  = linker_quality.total_ack_err_num_;
    itr->second.total_ai_repair_num_ = linker_quality.total_ai_repair_num_;
    itr->second.report_pos_         = linker_quality.report_pos_;
    itr->second.last_active_ts_us_  = current_date_.timestamp_us;

    if (0.000001 < linker_quality.loss_) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_debug,
                "%s:%u<-->%s:%u rtt=%ums loss=%5.2f%% jit=%ums dir=%s pos=%s\r\n",
                linker_quality.self_ip_, (u32)(linker_quality.self_port_), linker_quality.peer_ip_,
                (u32)(linker_quality.peer_port_), (u32)(linker_quality.rtt_ms_), linker_quality.loss_,
                (u32)(linker_quality.rtt_jitter_ms_), (kUplinkLoss == linker_quality.loss_direct_) ? "up":"down",
                (kSenderQuality == linker_quality.report_pos_) ? "send":"recv");

        u8  buf[8192] = {0};
        u32 nret = 0;

        nret = GetSlidWinBitMapInfo(goodtp_hdl_, (GtpLinkerKey*)(&(linker_quality.linker_key_)), &(buf[0]), 8192);

        if (GTP_OK == nret) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_debug, "%s\r\n", buf);
        } else {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error, "Call GetSlidWinBitMapInfo() failed(0x%08x)\r\n",
                    nret);
        }
    }

    return;
}

// return: 0: new frame sn, 1: repeat frame sn.
u32 TranEpoll::AppSnEntrySlidWin(const u32 &sn, GtpAddr *tran_addr) {
    u8  self_ip[MAX_ADDR_SIZE] = {0};
    u8  peer_ip[MAX_ADDR_SIZE] = {0};
    u16 self_port = 0;
    u16 peer_port = 0;

    u32 run_result = GtpAddrToHostAddr(tran_addr, self_ip, peer_ip, &self_port, &peer_port);
    if (COS_OK != run_result) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error, "Call GtpAddrToHostAddr() failed(0x%08x).\r\n",
                run_result);
        return 0;
    }

    LinkerKey linker_key(self_ip, peer_ip, self_port, peer_port, kReceiverQuality);

    unordered_map<LinkerKey, LinkerQuality, LinkerKeyHash>::iterator itr = linker_quality_map_.find(linker_key);
    if (linker_quality_map_.end() == itr) {
        linker_quality_map_.emplace(linker_key, LinkerQuality(sn));

        itr = linker_quality_map_.find(linker_key);
        if (linker_quality_map_.end() == itr) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error, "unordered_map is abnormal.\r\n");
            return 0;
        }
    }

    u32 repeat_flag = RepeatPacketFilter(itr->second.app_quality_.win_hdl_, sn, current_date_.timestamp_us);

    if (1 == repeat_flag) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_warning, "[%p]frame_sn=%u repeat_flag=%u\r\n",
            itr->second.app_quality_.win_hdl_, sn, repeat_flag);

        u8 buf[1024] = {0};
        u32 str_len  = 0;

        PrintWinBitMap(itr->second.app_quality_.win_hdl_, (u8*)buf, 1024, &str_len);

        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_warning, "[%p]bitmap=%s\r\n",
                itr->second.app_quality_.win_hdl_, buf);
    }

    run_result = SnEntrySlidWin(itr->second.app_quality_.win_hdl_, sn, current_date_.timestamp_us);
    if (COS_OK != run_result) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error, "Call SnEntrySlidWin() failed(0x%08x).\r\n",
                run_result);
    }

    itr->second.app_quality_.last_sn_ = sn;

    return repeat_flag;
}

void TranEpoll::PrintLinkerQuality(const u32 &bit_map_flag) {
    unordered_map<LinkerKey, LinkerQuality, LinkerKeyHash>::iterator itr = linker_quality_map_.begin();
    u32 counter = 0;
    i32 wrt_num = 0;

    u32 new_line = 0;

    char *wrt_pos = NULL;

    char  print_buf[1024] = {0};

    cos_printf("~~~~~~~~~~~~~~~~~~~~No.%u epoll thread:~~~~~~~~~~~~~~~~~~~~~\r\n", thread_id_);
    while (linker_quality_map_.end() != itr) {
        cos_printf("%s:%u<->%s:%u(%s)\r\n", itr->first.self_ip_, (u32)(itr->first.self_port_),
                   itr->first.peer_ip_, (u32)(itr->first.peer_port_),
                   ((kSenderQuality == itr->second.report_pos_) ? "sender" : "receiver"));

        cos_printf("     linker: rtt=%ums jitter=%ums pure=%ubps net=%ubps pure_pps=%u net_pps=%u loss=%0.2f%% "\
                   "congest_rank=%u btrt_k=%0.2f dir=%s\r\n", (u32)(itr->second.rtt_ms_),
                   (u32)(itr->second.rtt_jitter_ms_), itr->second.data_bitrate_bps_, itr->second.net_bitrate_bps_,
                   itr->second.data_pack_pps_, itr->second.net_pack_pps_, itr->second.loss_,
                   itr->second.pre_congest_rank_, itr->second.bitrage_chg_k_,
                   ((kUplinkLoss == itr->second.loss_direct_) ? "up" : "down"));

        cos_printf("     linker: frm_sum=%u pck_sum=%u ack_sum=%u retran_sum=%u rto_loss_sum=%u ack_loss_num=%u "\
                   "ack_err_sum=%u AI_repair=%u\r\n", itr->second.total_frame_num_, itr->second.total_pack_num_,
                   itr->second.total_ack_num_, itr->second.total_retran_num_, itr->second.total_rto_loss_num_,
                   itr->second.total_ack_loss_num_, itr->second.total_ack_err_num_, itr->second.total_ai_repair_num_);
        wrt_pos = &(print_buf[0]);

        wrt_num = sprintf(wrt_pos, "application: loss=%0.2f%% pps=%u header_sn=%u tail_sn=%u bitnum=%u bitmap=",
                          itr->second.app_quality_.loss_, itr->second.app_quality_.pps_,
                          itr->second.app_quality_.last_bitmap_header_sn_,
                          itr->second.app_quality_.last_bitmap_tail_sn_,
                 (itr->second.app_quality_.last_bitmap_tail_sn_ - itr->second.app_quality_.last_bitmap_header_sn_));

        wrt_pos += wrt_num;
        new_line = 0;

        if (0 != bit_map_flag) {
            for (u32 i = 0, j = 0; itr->second.app_quality_.bitmap_size_ > i; ++i, ++j) {
                if ((0 != i) && (((0 == new_line) && (0 == (i % 3))) || ((0 != new_line) && (0 == (j % 7))))) {
                    if (0 == new_line) {
                        j = 0;
                    }
                    new_line = 1;
                    #ifdef __linux__
                    wrt_num = sprintf(wrt_pos, "\r\n%016lx ", itr->second.app_quality_.bitmap_[i]);
                    #endif

                    #if (_WIN32 || _WIN64)
                    wrt_num = sprintf(wrt_pos, "\r\n%016llx ", itr->second.app_quality_.bitmap_[i]);
                    #endif
                } else {
                    #ifdef __linux__
                    wrt_num = sprintf(wrt_pos, "%016lx ", itr->second.app_quality_.bitmap_[i]);
                    #endif

                    #if (_WIN32 || _WIN64)
                    wrt_num = sprintf(wrt_pos, "%016llx ", itr->second.app_quality_.bitmap_[i]);
                    #endif
                }
                wrt_pos += wrt_num;
            }
        } else {
            wrt_num = sprintf(wrt_pos, " show off");
            wrt_pos += wrt_num;
        }
        sprintf(wrt_pos, "\r\n\n");
        cos_printf("%s", print_buf);

        ++itr;
        counter += 1;
    }
    cos_printf("~~~~~~~~~~~~~~~~~~~~Total: %u~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\r\n\n", counter);

    return;
}

void TranEpoll::CheckResource(void) {
    if (rolerEnum::kClientRoler == roler_) {
        return;
    }

    unordered_map<LinkerKey, LinkerQuality, LinkerKeyHash>::iterator itr = linker_quality_map_.begin();

    while (linker_quality_map_.end() != itr) {
        if ((itr->second.last_active_ts_us_ + 5000000) <= current_date_.timestamp_us) {
            itr = linker_quality_map_.erase(itr);
            continue;
        }

        ++itr;
    }

    return;
}

void TranEpoll::ShowLinkerQuality(const u32 &pos) {
    GtpLinkQuality *link_quality = (GtpLinkQuality*)cache_64K_;

    if (1 == pos) {  // sender's quality
        GtpLinkerKey link_key;

        link_key.sfd_                  = (u32)send_sfd_;
        link_key.direction_            = 1;
        link_key.peer_socket_addr_len_ = server_sock_addr_size_;
        link_key.self_socket_addr_len_ = 0;

        memcpy(link_key.peer_socket_addr_, server_sock_addr_, server_sock_addr_size_);

        u32 result = GetLinkerQuality(goodtp_hdl_, &link_key, link_quality);

        if (GTP_OK != result) {
            cos_printf("Call GetLinkerQuality() failed(0x%08x) in No.%u epoll\r\n", result, thread_id_);
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                    "Call GetLinkerQuality() failed(0x%08x) in No.%u epoll.\r\n", result, thread_id_);
            return;
        }
    }

    cos_printf("%s:%u<-->%s:%u(loss_dir=%s):\r\n", link_quality->self_ip_, (u32)(link_quality->self_port_),
               link_quality->peer_ip_, (u32)(link_quality->peer_port_),
               ((kUplinkLoss == link_quality->loss_direct_) ? "up":"down"));

    cos_printf("rtt=%ums jit=%ums loss=%5.2f%% data=%ubps net=%ubps data_pps=%u net_pps=%u\r\n",
               (u32)(link_quality->rtt_ms_), (u32)(link_quality->rtt_jitter_ms_), link_quality->loss_,
               link_quality->data_bitrate_bps_, link_quality->net_bitrate_bps_, link_quality->data_pack_pps_,
               link_quality->net_pack_pps_);

    cos_printf("frame_sum=%u pack_sum=%u ack_num=%u retran_sum=%u ack_resend=%u rto_resend=%u AI_repair_sum=%u "\
               "tran_failed_sum=%u\r\n\n", link_quality->total_frame_num_, link_quality->total_pack_num_,
               link_quality->total_ack_num_, link_quality->total_retran_num_, link_quality->total_ack_loss_num_,
               link_quality->total_rto_loss_num_, link_quality->total_ai_repair_num_,
               link_quality->total_ack_err_num_);

    return;
}

u32 TranEpoll::FileTranReceiveEntry(void *param) {
    #ifdef __linux__
    pthread_detach(pthread_self());
    #endif

    i32 active_num       = 0;
    i32 active_loop      = 0;
    i32 received_size    = 0;
    u32 run_result       = COS_OK;

    u32 sock_addr_len    = 0;
    u32 c3buf_len        = 0;
    u8 *c3buf_mem        = NULL;

    u64 calc_delta_us    = 0;

    GtpAddr *tran_addr    = NULL;
    GtpTesterFrame *frame = NULL;

    #ifdef __linux__
    TranEpoll *epoll_obj = reinterpret_cast<TranEpoll*>(param);
    #endif

    u8 sock_addr[sizeof(struct sockaddr_in6)] = {0};

    #if (_WIN32 || _WIN64)
    #ifdef _WIN64
    TranEpoll *epoll_obj = reinterpret_cast<TranEpoll*>((void*)(*((u64*)param)));
    #else
    TranEpoll *epoll_obj = reinterpret_cast<TranEpoll*>((void*)(*((u32*)param)));
    #endif

    struct timeval timeout;
    fd_set tmp_sfd;
    #endif

    u32 length = 0;
    u32 start_flag = COS_YES;

    {
    i64 stack_free_size = GtpGetStackFreeSize(&run_result);
    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_crit,
            "stack_free_size=%lld stack_size=%u.\r\n", stack_free_size, run_result);
    }

    run_result = epoll_obj->CreateGtpInst();
    if (COS_OK != run_result) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call CreateGtpInst() failed(0x%08x), Now exit epoll thread.\r\n", run_result);
        goto exit_thread_pos_;
    }

    #ifdef __linux__
    struct epoll_event pevents[MAX_EPOLL_EVENT_NUM];
    memset(pevents, 0x00, sizeof(pevents));

    snprintf((char*)(epoll_obj->cache_64K_), SIZE_64K, "MEpoll-%u", epoll_obj->thread_id_);

    pthread_setname_np(pthread_self(), (char*)(epoll_obj->cache_64K_));

    epoll_obj->epoll_id_ = epoll_create(MAX_EPOLL_SIZE);
    if (COS_INVALID_HDL == epoll_obj->epoll_id_) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call epoll_create() failed(errorcode= %u(%s)), Now exit epoll thread.\r\n", errno, strerror(errno));
        goto exit_thread_pos_;
    }
    #endif

    if (rolerEnum::kServerRoler == epoll_obj->roler_) {
        for (u32 i = 0; (u32)(epoll_obj->listen_ip_.size()) > i; ++i) {
            for (u32 port = epoll_obj->start_port_; epoll_obj->end_port_ >= port; ++port) {
                cos_sock sfd = COS_INVALID_HDL;
                run_result = CreateListenSocket(epoll_obj->listen_ip_[i], port, &sfd, &(epoll_obj->epoll_id_), COS_YES);
                if (COS_OK != run_result) {
                    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                            "call CreateListenSocket() failed(0x%08x)\r\n", run_result);
                    continue;
                }

                epoll_obj->listen_sfd_.emplace_back(sfd);
            }
        }
    } else {
        run_result = CreateListenSocket(epoll_obj->send_ip_, 0, &(epoll_obj->send_sfd_), &(epoll_obj->epoll_id_), COS_NO);
        if (COS_OK != run_result) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                    "call CreateListenSocket() failed(0x%08x)\r\n", run_result);
            goto exit_thread_pos_;
        }
    }

    epoll_obj->epoll_thread_runned_flag_ = COS_YES;

    cos_getCurSysTime(&(epoll_obj->current_date_));

    epoll_obj->call_goodtp_timer_ts_us_     = epoll_obj->current_date_.timestamp_us + C3PTP_TIMER_PERIOD_US;
    epoll_obj->check_resource_period_ts_us_ = epoll_obj->current_date_.timestamp_us + CHECK_RESOURCE_PERIOD_US;
    epoll_obj->switch_server_ip_ts_us_      = epoll_obj->current_date_.timestamp_us;

    epoll_obj->single_test_flag_  = COS_YES;
    epoll_obj->receved_data_flag_ = COS_NO;

    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_warning,
            "Start No.%u single epoll thread.\r\n", epoll_obj->thread_id_);

    while (COS_YES == epoll_obj->epoll_thread_enable_flag_) {
        #ifdef __linux__
        active_num = (i32)epoll_wait(epoll_obj->epoll_id_, pevents, MAX_EPOLL_EVENT_NUM, 0);
        if (-1 == active_num) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                    "call epoll_wait() failed(errorcode= %u(%s)), now exit No.%u epoll thread.\r\n",
                    errno, strerror(errno), epoll_obj->thread_id_);
            break;
        }
        #endif

        #if (_WIN32 || _WIN64)
        timeout.tv_sec  = 0;
        timeout.tv_usec = 1;

        FD_ZERO(&tmp_sfd);
        FD_SET(epoll_obj->send_sfd_, &tmp_sfd);

        active_num = (i32)select(((u32)epoll_obj->send_sfd_) + 1, &tmp_sfd, NULL, NULL, &timeout);
        if (SOCKET_ERROR == active_num) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                    "call epoll_wait() failed(errorcode= %u(%s)), now exit No.%u epoll thread.\r\n",
                    errno, strerror(errno), epoll_obj->thread_id_);
            break;
        }
        #endif

        cos_getCurSysTime(&(epoll_obj->current_date_));

        for (active_loop = 0; active_num > active_loop; ++active_loop) {
udp_receive_pos_:
            sock_addr_len = sizeof(struct sockaddr_in6);
            if (COS_YES == epoll_obj->consume_switch_) {
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us;
            }

            #ifdef __linux__
            received_size = recvfrom(pevents[active_loop].data.fd, (char*)epoll_obj->cache_64K_,
                                     SIZE_64K, 0, (struct sockaddr*)(&(sock_addr[0])), &sock_addr_len);
            #endif

            #if (_WIN32 || _WIN64)
            received_size = recvfrom(epoll_obj->send_sfd_, (char*)epoll_obj->cache_64K_, SIZE_64K, 0,
                                     (struct sockaddr*)(&(sock_addr[0])), (int*)(&sock_addr_len));
            #endif

            if (0 >= received_size) {
                if (errno == EINTR) {  // errno == EAGAIN
                    goto udp_receive_pos_;
                }

                *epoll_obj->udp_receive_failed_dot_ += 1;
                break;
            }
            if (COS_YES == epoll_obj->consume_switch_) {
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;
                epoll_obj->sock_recv_consume_.update((u16)calc_delta_us);
            }

            c3buf_mem = GtpMallocPackMem(epoll_obj->goodtp_hdl_, NULL, 0, &c3buf_len, (void**)(&tran_addr),
                                         (u32)received_size);

            if (NULL == c3buf_mem) {
                *epoll_obj->gtp_buf_malloc_failed_dot_ += 1;
                break;
            }

            tran_addr->context_ = epoll_obj;
            #ifdef __linux__
            tran_addr->sfd_     = (u32)(pevents[active_loop].data.fd);
            #endif

            #if (_WIN32 || _WIN64)
            tran_addr->sfd_     = (u32)(epoll_obj->send_sfd_);  // It's only client in windows.
            #endif

            tran_addr->sock_addr_len_ = sock_addr_len;
            tran_addr->self_addr_len_ = 0;
            tran_addr->stream_type_   = GTP_STREAM_TYPE;
            tran_addr->smooth_jitter_ = GTP_REMOVE_JITTER;
            tran_addr->enable_key_    = 0;

            memcpy(tran_addr->sock_addr_, sock_addr, sock_addr_len);
            memcpy(c3buf_mem, epoll_obj->cache_64K_, (u32)received_size);

            

            epoll_obj->proced_frame_flag_ = COS_NO;

            #if (1 == ENABLE_OUT_CHECK)
            run_result = GtpCheckPacketInvalid(c3buf_mem, (u32)received_size);
            if (GTP_OK != run_result) {
                COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                        "Call GtpCheckPacketInvalid() failed(0x%08x).\r\n", run_result);
                goto gtp_single_receive_next_pos_;
            }
            #endif

            if (COS_YES == epoll_obj->consume_switch_) {
                cos_getCurSysTime(&(epoll_obj->current_date_));

                calc_delta_us         = epoll_obj->current_date_.timestamp_us;
                tran_addr->timestamp_ = epoll_obj->current_date_.timestamp_us;
            } else {
                #ifdef _SELFDEBUG
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us;
                #endif
            }

            run_result = GtpPacketReceive(epoll_obj->goodtp_hdl_, c3buf_mem, (u32)received_size, tran_addr);

            if (COS_YES == epoll_obj->consume_switch_) {
                if (COS_NO == epoll_obj->proced_frame_flag_) {
                    cos_getCurSysTime(&(epoll_obj->current_date_));
                    calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;

                    epoll_obj->goodtp_recv_consume_.update((u16)calc_delta_us);
                } else {
                    #ifdef _SELFDEBUG
                    cos_getCurSysTime(&(epoll_obj->current_date_));
                    calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;
                    #endif
                }
            } else {
                #ifdef _SELFDEBUG
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;
                #endif
            }

            #ifdef _SELFDEBUG
            if (500 < calc_delta_us) {
                COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                        "call GtpPacketReceive() too long(%lluus) in No.%u epoll thread.\r\n",
                        calc_delta_us, epoll_obj->thread_id_);
            }
            #endif

#if (1 == ENABLE_OUT_CHECK)
gtp_single_receive_next_pos_:
#endif
            GtpFreePackMem(epoll_obj->goodtp_hdl_, c3buf_mem);

            if (COS_OK != run_result) {
                *epoll_obj->goodtp_rcv_pack_err_dot_ += 1;
                COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                        "call GtpPacketReceive() failed(0x%08x) in No.%u epoll thread.\r\n",
                        run_result, epoll_obj->thread_id_);
            }
        }

        if (epoll_obj->sending_frame_ts_us_ <= epoll_obj->current_date_.timestamp_us) {
            c3buf_mem = GtpMallocPackMem(epoll_obj->goodtp_hdl_, NULL, 0, &c3buf_len, (void**)(&tran_addr),
                                         1400);
            if (NULL == c3buf_mem) {
                *epoll_obj->gtp_buf_malloc_failed_dot_ += 1;
                goto epoll_loop_next_pos_;
            }

            if ((epoll_obj->switch_server_ip_ts_us_ + 30000000) <= epoll_obj->current_date_.timestamp_us) {
                epoll_obj->switch_server_ip_ts_us_ = epoll_obj->current_date_.timestamp_us;

                if (epoll_obj->server_addr_size_ == (&(epoll_obj->server_sock_addr_size_))) {
                    epoll_obj->server_addr_size_ = &(epoll_obj->backup_server_sock_addr_size_);
                    epoll_obj->server_addr_      = &(epoll_obj->backup_server_sock_addr_[0]);
                } else {
                    epoll_obj->server_addr_size_ = &(epoll_obj->server_sock_addr_size_);
                    epoll_obj->server_addr_      = &(epoll_obj->server_sock_addr_[0]);
                }
            }

            tran_addr->context_       = epoll_obj;
            tran_addr->sfd_           = (u32)(epoll_obj->send_sfd_);
            tran_addr->sock_addr_len_ = *(epoll_obj->server_addr_size_);
            tran_addr->self_addr_len_ = 0;
            tran_addr->qos_           = FEC_SWITCH;  // turn on fec.
            tran_addr->stream_type_   = GTP_STREAM_TYPE;
            tran_addr->smooth_jitter_ = GTP_REMOVE_JITTER;
            tran_addr->enable_key_    = 0;

            memcpy(tran_addr->sock_addr_, epoll_obj->server_addr_, tran_addr->sock_addr_len_);

            cos_getCurSysTime(&(epoll_obj->current_date_));

            frame = (GtpTesterFrame*)c3buf_mem;

            if (COS_YES == start_flag) {
                frame->type_ = (u32)(FrameType::kFrameFileTranStart);
                start_flag   = COS_NO;
                length       = 0;
            } else {
                length = fread(frame->payload_, 1, 1316, epoll_obj->bin_pf_);
                if (0 < length) {
                    frame->type_ = (u32)(FrameType::kFrameFileTran);
                } else {
                    frame->type_ = (u32)(FrameType::kFrameFileTranEnd);
                }
            }

            length += sizeof(GtpTesterFrame);

            frame->header_       = FRAME_HEADER;
            frame->length_       = length;
            frame->type_         = (u32)(FrameType::kFrameFileTran);
            frame->sn_           = epoll_obj->application_sn_;
            frame->sender_ts_us_ = epoll_obj->current_date_.timestamp_us;
            frame->check_sum_    = 0;

            frame->check_sum_    = CalcCheckSum((u8*)frame, length);

            epoll_obj->application_sn_ += 1;

            

            if (COS_YES == epoll_obj->consume_switch_) {
                cos_getCurSysTime(&(epoll_obj->current_date_));

                calc_delta_us         = epoll_obj->current_date_.timestamp_us;
                tran_addr->timestamp_ = epoll_obj->current_date_.timestamp_us;
            } else {
                #ifdef _SELFDEBUG
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us;
                #endif
            }

            run_result = GtpFrameSend(epoll_obj->goodtp_hdl_, frame, length, tran_addr, 0, 0);

            #ifdef _SELFDEBUG
            cos_getCurSysTime(&(epoll_obj->current_date_));
            calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;
            if (500 < calc_delta_us) {
                COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                        "call GtpFrameSend() too long(%lluus) in No.%u epoll thread.\r\n",
                        calc_delta_us, epoll_obj->thread_id_);
            }
            #endif

            GtpFreePackMem(epoll_obj->goodtp_hdl_, c3buf_mem);

            if (COS_OK != run_result) {
                *epoll_obj->goodtp_snd_frame_err_dot_ += 1;
                COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                        "call GtpFrameSend() failed(0x%08x) in No.%u epoll thread.\r\n",
                        run_result, epoll_obj->thread_id_);
            }

            epoll_obj->sending_frame_ts_us_ = epoll_obj->current_date_.timestamp_us + epoll_obj->send_period_ts_us_;
        }

        if (epoll_obj->call_goodtp_timer_ts_us_ <= epoll_obj->current_date_.timestamp_us) {
            if (COS_YES == epoll_obj->consume_switch_) {
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us;
            }

            run_result = PeriodGtpTimer(epoll_obj->goodtp_hdl_);

            if (COS_YES == epoll_obj->consume_switch_) {
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;

                epoll_obj->goodtp_timer_consume_.update((u16)calc_delta_us);
            }

            if (COS_OK != run_result) {
                *epoll_obj->goodtp_timer_failed_dot_ += 1;
                COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                        "call PeriodGtpTimer() failed(0x%08x) in No.%u epoll thread.\r\n",
                        run_result, epoll_obj->thread_id_);
            }

            epoll_obj->call_goodtp_timer_ts_us_ = epoll_obj->current_date_.timestamp_us + C3PTP_TIMER_PERIOD_US;
        }

epoll_loop_next_pos_:
        if (0 != active_num) {
            #ifdef __linux__
            memset(pevents, 0x00, (sizeof(struct epoll_event) * active_num));
            #endif
        }

        if (epoll_obj->check_resource_period_ts_us_ <= epoll_obj->current_date_.timestamp_us) {
            epoll_obj->check_resource_period_ts_us_ = epoll_obj->current_date_.timestamp_us + CHECK_RESOURCE_PERIOD_US;
            epoll_obj->CheckResource();
        }
    }

exit_thread_pos_:
    if (rolerEnum::kServerRoler == epoll_obj->roler_) {
        for (u32 i = 0; ((u32)(epoll_obj->listen_sfd_.size())) > i; ++i) {
            cos_closeSock(epoll_obj->listen_sfd_[i]);
            epoll_obj->listen_sfd_[i] = COS_INVALID_HDL;
        }

        {
            vector<cos_sock> temp_vec;
            epoll_obj->listen_sfd_.swap(temp_vec);
        }
    } else {
        if (COS_INVALID_HDL != epoll_obj->send_sfd_) {
            cos_closeSock(epoll_obj->send_sfd_);
            epoll_obj->send_sfd_ = COS_INVALID_HDL;
        }
    }

    #ifdef __linux__
    if (-1 != epoll_obj->epoll_id_) {
        close(epoll_obj->epoll_id_);
        epoll_obj->epoll_id_ = -1;
    }
    #endif

    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_warning, "Exit No.%u epoll thread(thread_enable=%u).\r\n",
            epoll_obj->thread_id_, epoll_obj->epoll_thread_enable_flag_);

    epoll_obj->epoll_thread_enable_flag_ = COS_NO;
    epoll_obj->epoll_thread_runned_flag_ = COS_NO;
    epoll_obj->epoll_thread_exited_flag_ = COS_YES;

    #ifdef __linux__
    pthread_exit(COS_OK);
    #endif

    #if (_WIN32 || _WIN64)
    return COS_EXIST_THREAD_CODE;
    #endif
}

u32 TranEpoll::MeasureSpeedReceiveEnry(void *param) {
    #ifdef __linux__
    pthread_detach(pthread_self());
    #endif

    i32 active_num       = 0;
    i32 active_loop      = 0;
    i32 received_size    = 0;
    u32 run_result       = COS_OK;

    u32 sock_addr_len    = 0;
    u32 c3buf_len        = 0;
    u8 *c3buf_mem        = NULL;

    u64 calc_delta_us    = 0;

    u32 cmd_counter      = 0;

    GtpAddr *tran_addr    = NULL;
    GtpTesterFrame *frame = NULL;

    #ifdef __linux__
    TranEpoll *epoll_obj = reinterpret_cast<TranEpoll*>(param);
    #endif

    u8 sock_addr[sizeof(struct sockaddr_in6)] = {0};

    #if (_WIN32 || _WIN64)
    #ifdef _WIN64
    TranEpoll *epoll_obj = reinterpret_cast<TranEpoll*>((void*)(*((u64*)param)));
    #else
    TranEpoll *epoll_obj = reinterpret_cast<TranEpoll*>((void*)(*((u32*)param)));
    #endif

    struct timeval timeout;
    fd_set tmp_sfd;
    #endif

    u32 length_ = 0;

    {
    i64 stack_free_size = GtpGetStackFreeSize(&run_result);
    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_crit,
            "stack_free_size=%lld stack_size=%u.\r\n", stack_free_size, run_result);
    }

    run_result = epoll_obj->CreateGtpInst();
    if (COS_OK != run_result) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call CreateGtpInst() failed(0x%08x), Now exit epoll thread.\r\n", run_result);
        goto exit_thread_pos_;
    }

    #ifdef __linux__
    struct epoll_event pevents[MAX_EPOLL_EVENT_NUM];
    memset(pevents, 0x00, sizeof(pevents));

    snprintf((char*)(epoll_obj->cache_64K_), SIZE_64K, "MEpoll-%u", epoll_obj->thread_id_);

    pthread_setname_np(pthread_self(), (char*)(epoll_obj->cache_64K_));

    epoll_obj->epoll_id_ = epoll_create(MAX_EPOLL_SIZE);
    if (COS_INVALID_HDL == epoll_obj->epoll_id_) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call epoll_create() failed(errorcode= %u(%s)), Now exit epoll thread.\r\n", errno, strerror(errno));
        goto exit_thread_pos_;
    }
    #endif

    if (rolerEnum::kServerRoler == epoll_obj->roler_) {
        for (u32 i = 0; (u32)(epoll_obj->listen_ip_.size()) > i; ++i) {
            for (u32 port = epoll_obj->start_port_; epoll_obj->end_port_ >= port; ++port) {
                cos_sock sfd = COS_INVALID_HDL;
                run_result = CreateListenSocket(epoll_obj->listen_ip_[i], port, &sfd, &(epoll_obj->epoll_id_), COS_YES);
                if (COS_OK != run_result) {
                    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                            "call CreateListenSocket() failed(0x%08x)\r\n", run_result);
                    continue;
                }

                epoll_obj->listen_sfd_.emplace_back(sfd);
            }
        }
    } else {
        run_result = CreateListenSocket(epoll_obj->send_ip_, 0, &(epoll_obj->send_sfd_), &(epoll_obj->epoll_id_), COS_NO);
        if (COS_OK != run_result) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                    "call CreateListenSocket() failed(0x%08x)\r\n", run_result);
            goto exit_thread_pos_;
        }
    }

    epoll_obj->epoll_thread_runned_flag_ = COS_YES;

    cos_getCurSysTime(&(epoll_obj->current_date_));

    epoll_obj->call_goodtp_timer_ts_us_     = epoll_obj->current_date_.timestamp_us + C3PTP_TIMER_PERIOD_US;
    epoll_obj->check_resource_period_ts_us_ = epoll_obj->current_date_.timestamp_us + CHECK_RESOURCE_PERIOD_US;
    epoll_obj->switch_server_ip_ts_us_      = epoll_obj->current_date_.timestamp_us;

    while (COS_YES == epoll_obj->epoll_thread_enable_flag_) {
        #ifdef __linux__
        active_num = (i32)epoll_wait(epoll_obj->epoll_id_, pevents, MAX_EPOLL_EVENT_NUM, 0);
        if (-1 == active_num) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                    "call epoll_wait() failed(errorcode= %u(%s)), now exit No.%u epoll thread.\r\n",
                    errno, strerror(errno), epoll_obj->thread_id_);
            break;
        }
        #endif

        #if (_WIN32 || _WIN64)
        timeout.tv_sec  = 0;
        timeout.tv_usec = 1;

        FD_ZERO(&tmp_sfd);
        FD_SET(epoll_obj->send_sfd_, &tmp_sfd);

        active_num = (i32)select(((u32)epoll_obj->send_sfd_) + 1, &tmp_sfd, NULL, NULL, &timeout);
        if (SOCKET_ERROR == active_num) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                    "call epoll_wait() failed(errorcode= %u(%s)), now exit No.%u epoll thread.\r\n",
                    errno, strerror(errno), epoll_obj->thread_id_);
            break;
        }
        #endif

        cos_getCurSysTime(&(epoll_obj->current_date_));

        for (active_loop = 0; active_num > active_loop; ++active_loop) {
udp_receive_pos_:
            sock_addr_len = sizeof(struct sockaddr_in6);
            if (COS_YES == epoll_obj->consume_switch_) {
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us;
            }

            #ifdef __linux__
            received_size = recvfrom(pevents[active_loop].data.fd, (char*)epoll_obj->cache_64K_,
                                     SIZE_64K, 0, (struct sockaddr*)(&(sock_addr[0])), &sock_addr_len);
            #endif

            #if (_WIN32 || _WIN64)
            received_size = recvfrom(epoll_obj->send_sfd_, (char*)epoll_obj->cache_64K_, SIZE_64K, 0,
                                     (struct sockaddr*)(&(sock_addr[0])), (int*)(&sock_addr_len));
            #endif

            if (0 >= received_size) {
                if (errno == EINTR) {  // errno == EAGAIN
                    goto udp_receive_pos_;
                }

                *epoll_obj->udp_receive_failed_dot_ += 1;
                break;
            }

            if (COS_YES == epoll_obj->consume_switch_) {
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;
                epoll_obj->sock_recv_consume_.update((u16)calc_delta_us);
            }

            c3buf_mem = GtpMallocPackMem(epoll_obj->goodtp_hdl_, NULL, 0, &c3buf_len, (void**)(&tran_addr),
                                         (u32)received_size);

            if (NULL == c3buf_mem) {
                *epoll_obj->gtp_buf_malloc_failed_dot_ += 1;
                break;
            }

            tran_addr->context_ = epoll_obj;
            #ifdef __linux__
            tran_addr->sfd_     = (u32)(pevents[active_loop].data.fd);
            #endif

            #if (_WIN32 || _WIN64)
            tran_addr->sfd_     = (u32)(epoll_obj->send_sfd_);  // It's only client in windows.
            #endif

            tran_addr->sock_addr_len_ = sock_addr_len;
            tran_addr->self_addr_len_ = 0;
            tran_addr->stream_type_   = GTP_STREAM_TYPE;
            tran_addr->smooth_jitter_ = GTP_REMOVE_JITTER;
            tran_addr->enable_key_    = 0;

            memcpy(tran_addr->sock_addr_, sock_addr, sock_addr_len);
            memcpy(c3buf_mem, epoll_obj->cache_64K_, (u32)received_size);

            #if (1 == ENABLE_OUT_CHECK)
            run_result = GtpCheckPacketInvalid(c3buf_mem, (u32)received_size);
            if (GTP_OK != run_result) {
                COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                        "Call GtpCheckPacketInvalid() failed(0x%08x).\r\n", run_result);
                goto gtp_measure_receive_next_pos_;
            }
            #endif

            if (COS_YES == epoll_obj->consume_switch_) {
                cos_getCurSysTime(&(epoll_obj->current_date_));

                calc_delta_us         = epoll_obj->current_date_.timestamp_us;
                tran_addr->timestamp_ = epoll_obj->current_date_.timestamp_us;
            } else {
                #ifdef _SELFDEBUG
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us;
                #endif
            }

            epoll_obj->proced_frame_flag_ = COS_NO;

            run_result = GtpPacketReceive(epoll_obj->goodtp_hdl_, c3buf_mem, (u32)received_size, tran_addr);

            if (COS_YES == epoll_obj->consume_switch_) {
                if (COS_NO == epoll_obj->proced_frame_flag_) {
                    cos_getCurSysTime(&(epoll_obj->current_date_));
                    calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;

                    epoll_obj->goodtp_recv_consume_.update((u16)calc_delta_us);
                } else {
                    #ifdef _SELFDEBUG
                    cos_getCurSysTime(&(epoll_obj->current_date_));
                    calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;
                    #endif
                }
            } else {
                #ifdef _SELFDEBUG
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;
                #endif
            }

            #ifdef _SELFDEBUG
            if (500 < calc_delta_us) {
                COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                        "call GtpPacketReceive() too long(%lluus) in No.%u epoll thread.\r\n",
                        calc_delta_us, epoll_obj->thread_id_);
            }
            #endif

#if (1 == ENABLE_OUT_CHECK)
gtp_measure_receive_next_pos_:
#endif
            GtpFreePackMem(epoll_obj->goodtp_hdl_, c3buf_mem);

            if (COS_OK != run_result) {
                *epoll_obj->goodtp_rcv_pack_err_dot_ += 1;
                COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                        "call GtpPacketReceive() failed(0x%08x) in No.%u epoll thread.\r\n",
                        run_result, epoll_obj->thread_id_);
            }
        }

        if (epoll_obj->sending_frame_ts_us_ <= epoll_obj->current_date_.timestamp_us) {
            u32 nloop = 0;

            do {
                #if (2 == LARGE_PACKET_TEST)
                length_ = sizeof(GtpTesterFrame) + (cos_genRand() & 0x00000FFF) + BASE_PAYLOAD_SIZE;
                #endif

                #if (1 == LARGE_PACKET_TEST)
                length_ = sizeof(GtpTesterFrame) + (cos_genRand() & 0x000000FF) + BASE_PAYLOAD_SIZE;
                #endif

                #if (0 == LARGE_PACKET_TEST)
                length_ = sizeof(GtpTesterFrame) + BASE_PAYLOAD_SIZE;
                #endif

                c3buf_mem = GtpMallocPackMem(epoll_obj->goodtp_hdl_, NULL, 0, &c3buf_len, (void**)(&tran_addr),
                                             length_);

                if (NULL == c3buf_mem) {
                    *epoll_obj->gtp_buf_malloc_failed_dot_ += 1;
                    goto epoll_loop_next_pos_;
                }

                if ((epoll_obj->switch_server_ip_ts_us_ + 30000000) <= epoll_obj->current_date_.timestamp_us) {
                    epoll_obj->switch_server_ip_ts_us_ = epoll_obj->current_date_.timestamp_us;

                    if (epoll_obj->server_addr_size_ == (&(epoll_obj->server_sock_addr_size_))) {
                        epoll_obj->server_addr_size_ = &(epoll_obj->backup_server_sock_addr_size_);
                        epoll_obj->server_addr_      = &(epoll_obj->backup_server_sock_addr_[0]);
                    } else {
                        epoll_obj->server_addr_size_ = &(epoll_obj->server_sock_addr_size_);
                        epoll_obj->server_addr_      = &(epoll_obj->server_sock_addr_[0]);
                    }
                }

                tran_addr->context_       = epoll_obj;
                tran_addr->sfd_           = (u32)(epoll_obj->send_sfd_);
                tran_addr->sock_addr_len_ = *(epoll_obj->server_addr_size_);
                tran_addr->self_addr_len_ = 0;
                tran_addr->qos_           = FEC_SWITCH;  // turn on fec.
                tran_addr->stream_type_   = GTP_STREAM_TYPE;
                tran_addr->smooth_jitter_ = GTP_REMOVE_JITTER;
                tran_addr->enable_key_    = 0;

                memcpy(tran_addr->sock_addr_, epoll_obj->server_addr_, tran_addr->sock_addr_len_);

                cos_getCurSysTime(&(epoll_obj->current_date_));

                frame = (GtpTesterFrame*)c3buf_mem;

                frame->header_       = FRAME_HEADER;
                frame->length_       = length_;
                
                switch (epoll_obj->state_machin_) {
                case kIdle: {
                    frame->type_ = (u32)(FrameType::kFrameCmdCrtFile);
                    frame->sn_   = cmd_counter;
                    cmd_counter += 1;
                    break;
                }

                case kResCreateFile: {
                    frame->type_ = (u32)(FrameType::kFrameCmdReqTranDt);
                    frame->sn_   = cmd_counter;
                    cmd_counter += 1;
                    break;
                }

                case kResTranData: {
                    frame->type_ = (u32)(FrameType::kFrameEchoTypeReq);
                    frame->sn_   = epoll_obj->application_sn_;

                    epoll_obj->application_sn_ += 1;
                    break;
                }

                default: {
                    frame->type_ = (u32)(FrameType::kFrameEchoTypeReq);
                    frame->sn_   = epoll_obj->application_sn_;

                    epoll_obj->application_sn_ += 1;
                }
                }
                
                frame->sender_ts_us_ = epoll_obj->current_date_.timestamp_us;
                frame->payload_[0]   = '1';
                frame->check_sum_    = 0;

                frame->check_sum_    = CalcCheckSum((u8*)frame, length_);

                if (COS_YES == epoll_obj->consume_switch_) {
                    cos_getCurSysTime(&(epoll_obj->current_date_));

                    calc_delta_us         = epoll_obj->current_date_.timestamp_us;
                    tran_addr->timestamp_ = epoll_obj->current_date_.timestamp_us;
                } else {
                    #ifdef _SELFDEBUG
                    cos_getCurSysTime(&(epoll_obj->current_date_));
                    calc_delta_us = epoll_obj->current_date_.timestamp_us;
                    #endif
                }

                run_result = GtpFrameSend(epoll_obj->goodtp_hdl_, frame, length_, tran_addr, 0, 0);

                #ifdef _SELFDEBUG
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;
                if (500 < calc_delta_us) {
                    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                            "call GtpFrameSend() too long(%lluus) in No.%u epoll thread.\r\n",
                            calc_delta_us, epoll_obj->thread_id_);
                }
                #endif

                GtpFreePackMem(epoll_obj->goodtp_hdl_, c3buf_mem);

                if (COS_OK != run_result) {
                    *epoll_obj->goodtp_snd_frame_err_dot_ += 1;
                    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                            "call GtpFrameSend() failed(0x%08x) in No.%u epoll thread.\r\n",
                            run_result, epoll_obj->thread_id_);
                }

                nloop += 1;
            } while (epoll_obj->measure_pack_num_ > nloop);

            epoll_obj->sending_frame_ts_us_ = epoll_obj->current_date_.timestamp_us + epoll_obj->send_period_ts_us_;
        }

        if (epoll_obj->call_goodtp_timer_ts_us_ <= epoll_obj->current_date_.timestamp_us) {
            if (COS_YES == epoll_obj->consume_switch_) {
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us;
            }

            run_result = PeriodGtpTimer(epoll_obj->goodtp_hdl_);

            if (COS_YES == epoll_obj->consume_switch_) {
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;

                epoll_obj->goodtp_timer_consume_.update((u16)calc_delta_us);
            }

            if (COS_OK != run_result) {
                *epoll_obj->goodtp_timer_failed_dot_ += 1;
                COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                        "call PeriodGtpTimer() failed(0x%08x) in No.%u epoll thread.\r\n",
                        run_result, epoll_obj->thread_id_);
            }

            epoll_obj->call_goodtp_timer_ts_us_ = epoll_obj->current_date_.timestamp_us + C3PTP_TIMER_PERIOD_US;
        }

epoll_loop_next_pos_:
        if (0 != active_num) {
            #ifdef __linux__
            memset(pevents, 0x00, (sizeof(struct epoll_event) * active_num));
            #endif
        }

        if (epoll_obj->check_resource_period_ts_us_ <= epoll_obj->current_date_.timestamp_us) {
            epoll_obj->check_resource_period_ts_us_ = epoll_obj->current_date_.timestamp_us + CHECK_RESOURCE_PERIOD_US;
            epoll_obj->CheckResource();
        }
    }

exit_thread_pos_:
    if (rolerEnum::kServerRoler == epoll_obj->roler_) {
        for (u32 i = 0; ((u32)(epoll_obj->listen_sfd_.size())) > i; ++i) {
            cos_closeSock(epoll_obj->listen_sfd_[i]);
            epoll_obj->listen_sfd_[i] = COS_INVALID_HDL;
        }

        {
            vector<cos_sock> temp_vec;
            epoll_obj->listen_sfd_.swap(temp_vec);
        }
    } else {
        if (COS_INVALID_HDL != epoll_obj->send_sfd_) {
            cos_closeSock(epoll_obj->send_sfd_);
            epoll_obj->send_sfd_ = COS_INVALID_HDL;
        }
    }

    #ifdef __linux__
    if (-1 != epoll_obj->epoll_id_) {
        close(epoll_obj->epoll_id_);
        epoll_obj->epoll_id_ = -1;
    }
    #endif

    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_warning, "Exit thread in No.%u epoll thread.\r\n",
            epoll_obj->thread_id_);

    epoll_obj->epoll_thread_enable_flag_ = COS_NO;
    epoll_obj->epoll_thread_runned_flag_ = COS_NO;
    epoll_obj->epoll_thread_exited_flag_ = COS_YES;

    #ifdef __linux__
    pthread_exit(COS_OK);
    #endif

    #if (_WIN32 || _WIN64)
    return COS_EXIST_THREAD_CODE;
    #endif
}

u32 TranEpoll::ReceiveEntry(void *param) {
    #ifdef __linux__
    pthread_detach(pthread_self());
    #endif

    i32 active_num    = 0;
    i32 active_loop   = 0;
    i32 received_size = 0;
    u32 run_result    = COS_OK;

    u32 sock_addr_len = 0;
    u32 c3buf_len     = 0;
    u8 *c3buf_mem     = NULL;

    u64 calc_delta_us = 0;

    u32 cmd_counter  = 0;

    GtpAddr *tran_addr    = NULL;
    GtpTesterFrame *frame = NULL;

    #ifdef __linux__
    TranEpoll *epoll_obj = reinterpret_cast<TranEpoll*>(param);
    #endif

    #if (_WIN32 || _WIN64)
    #ifdef _WIN64
    TranEpoll *epoll_obj = reinterpret_cast<TranEpoll*>((void*)(*((u64*)param)));
    #else
    TranEpoll *epoll_obj = reinterpret_cast<TranEpoll*>((void*)(*((u32*)param)));
    #endif

    struct timeval timeout;
    fd_set tmp_sfd;
    #endif

    u8 sock_addr[sizeof(struct sockaddr_in6)] = {0};

    u32 received_pack_counter = 0;
    u32 length_               = 0;

    {
    i64 stack_free_size = GtpGetStackFreeSize(&run_result);
    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_crit,
            "stack_free_size=%lld stack_size=%u.\r\n", stack_free_size, run_result);
    }

    run_result = epoll_obj->CreateGtpInst();
    if (COS_OK != run_result) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call CreateGtpInst() failed(0x%08x), Now exit epoll thread.\r\n", run_result);
        goto exit_thread_pos_;
    }

    #ifdef __linux__
    struct epoll_event pevents[MAX_EPOLL_EVENT_NUM];
    memset(pevents, 0x00, sizeof(pevents));

    snprintf((char*)(epoll_obj->cache_64K_), SIZE_64K, "Epoll-%u", epoll_obj->thread_id_);

    pthread_setname_np(pthread_self(), (char*)(epoll_obj->cache_64K_));

    epoll_obj->epoll_id_ = epoll_create(MAX_EPOLL_SIZE);
    if (COS_INVALID_HDL == epoll_obj->epoll_id_) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call epoll_create() failed(errorcode= %u(%s)), Now exit epoll thread.\r\n", errno, strerror(errno));
        goto exit_thread_pos_;
    }
    #endif

    if (rolerEnum::kServerRoler == epoll_obj->roler_) {
        for (u32 i = 0; (u32)(epoll_obj->listen_ip_.size()) > i; ++i) {
            for (u32 port = epoll_obj->start_port_; epoll_obj->end_port_ >= port; ++port) {
                cos_sock sfd = COS_INVALID_HDL;
                run_result = CreateListenSocket(epoll_obj->listen_ip_[i], port, &sfd, &(epoll_obj->epoll_id_), COS_YES);
                if (COS_OK != run_result) {
                    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                            "call CreateListenSocket() failed(0x%08x)\r\n", run_result);
                    continue;
                }

                epoll_obj->listen_sfd_.emplace_back(sfd);
            }
        }
    } else {
        run_result = CreateListenSocket(epoll_obj->send_ip_, 0, &(epoll_obj->send_sfd_), &(epoll_obj->epoll_id_), COS_NO);
        if (COS_OK != run_result) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                    "call CreateListenSocket() failed(0x%08x)\r\n", run_result);
            goto exit_thread_pos_;
        }
    }

    epoll_obj->epoll_thread_runned_flag_ = COS_YES;

    cos_getCurSysTime(&(epoll_obj->current_date_));

    epoll_obj->call_goodtp_timer_ts_us_     = epoll_obj->current_date_.timestamp_us + C3PTP_TIMER_PERIOD_US;
    epoll_obj->check_resource_period_ts_us_ = epoll_obj->current_date_.timestamp_us + CHECK_RESOURCE_PERIOD_US;
    epoll_obj->switch_server_ip_ts_us_      = epoll_obj->current_date_.timestamp_us;

    while (COS_YES == epoll_obj->epoll_thread_enable_flag_) {
        #ifdef __linux__
        if (rolerEnum::kServerRoler == epoll_obj->roler_) {
            active_num = (i32)epoll_wait(epoll_obj->epoll_id_, pevents, MAX_EPOLL_EVENT_NUM, 1);
        } else {
            active_num = (i32)epoll_wait(epoll_obj->epoll_id_, pevents, MAX_EPOLL_EVENT_NUM, 0);
        }
        if (-1 == active_num) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                    "call epoll_wait() failed(errorcode= %u(%s)), now exit No.%u epoll thread.\r\n",
                    errno, strerror(errno), epoll_obj->thread_id_);
            break;
        }
        #endif

        #if (_WIN32 || _WIN64)
        timeout.tv_sec  = 0;
        timeout.tv_usec = 1;

        FD_ZERO(&tmp_sfd);
        FD_SET(epoll_obj->send_sfd_, &tmp_sfd);

        active_num = (i32)select(((u32)epoll_obj->send_sfd_) + 1, &tmp_sfd, NULL, NULL, &timeout);
        if (SOCKET_ERROR == active_num) {
            COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                    "call epoll_wait() failed(errorcode= %u(%s)), now exit No.%u epoll thread.\r\n",
                    errno, strerror(errno), epoll_obj->thread_id_);
            break;
        }
        #endif

        cos_getCurSysTime(&(epoll_obj->current_date_));

        for (active_loop = 0; active_num > active_loop; ++active_loop) {
            do {
udp_receive_pos_:
                sock_addr_len = sizeof(struct sockaddr_in6);
                if (COS_YES == epoll_obj->consume_switch_) {
                    cos_getCurSysTime(&(epoll_obj->current_date_));
                    calc_delta_us = epoll_obj->current_date_.timestamp_us;
                }

                #ifdef __linux__
                received_size = recvfrom(pevents[active_loop].data.fd, (char*)epoll_obj->cache_64K_,
                                         SIZE_64K, 0, (struct sockaddr*)(&(sock_addr[0])), &sock_addr_len);
                #endif
                #if (_WIN32 || _WIN64)
                received_size = recvfrom(epoll_obj->send_sfd_, (char*)epoll_obj->cache_64K_, SIZE_64K, 0,
                                         (struct sockaddr*)(&(sock_addr[0])), (int*)&sock_addr_len);
                #endif
                if (0 >= received_size) {
                    if (errno == EINTR) {  // errno == EAGAIN
                        goto udp_receive_pos_;
                    }

                    if (errno == ENOTSOCK) {
                        *epoll_obj->udp_receive_failed_dot_ += 1;
                    }

                    break;
                }

                if (COS_YES == epoll_obj->consume_switch_) {
                    cos_getCurSysTime(&(epoll_obj->current_date_));
                    calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;

                    epoll_obj->sock_recv_consume_.update((u16)calc_delta_us);
                }

                c3buf_mem = GtpMallocPackMem(epoll_obj->goodtp_hdl_, NULL, 0, &c3buf_len, (void**)(&tran_addr),
                                             (u32)received_size);

                if (NULL == c3buf_mem) {
                    *epoll_obj->gtp_buf_malloc_failed_dot_ += 1;
                    break;
                }

                tran_addr->context_ = epoll_obj;
                #ifdef __linux__
                tran_addr->sfd_     = (u32)(pevents[active_loop].data.fd);
                #endif
                #if (_WIN32 || _WIN64)
                tran_addr->sfd_     = (u32)(epoll_obj->send_sfd_);
                #endif

                tran_addr->sock_addr_len_ = sock_addr_len;
                tran_addr->self_addr_len_ = 0;
                tran_addr->stream_type_   = GTP_STREAM_TYPE;
                tran_addr->smooth_jitter_ = GTP_REMOVE_JITTER;
                tran_addr->enable_key_    = 0;

                memcpy(tran_addr->sock_addr_, sock_addr, sock_addr_len);
                memcpy(c3buf_mem, epoll_obj->cache_64K_, (u32)received_size);

                #if (1 == ENABLE_OUT_CHECK)
                run_result = GtpCheckPacketInvalid(c3buf_mem, (u32)received_size);
                if (GTP_OK != run_result) {
                    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                            "Call GtpCheckPacketInvalid() failed(0x%08x).\r\n", run_result);
                    goto gtp_receive_next_pos_;
                }
                #endif

                if (COS_YES == epoll_obj->consume_switch_) {
                    cos_getCurSysTime(&(epoll_obj->current_date_));

                    calc_delta_us         = epoll_obj->current_date_.timestamp_us;
                    tran_addr->timestamp_ = epoll_obj->current_date_.timestamp_us;
                } else {
                    #ifdef _SELFDEBUG
                    cos_getCurSysTime(&(epoll_obj->current_date_));
                    calc_delta_us = epoll_obj->current_date_.timestamp_us;
                    #endif
                }

                epoll_obj->proced_frame_flag_ = COS_NO;

                run_result = GtpPacketReceive(epoll_obj->goodtp_hdl_, c3buf_mem, (u32)received_size, tran_addr);

                if (COS_YES == epoll_obj->consume_switch_) {
                    if (COS_NO == epoll_obj->proced_frame_flag_) {
                        cos_getCurSysTime(&(epoll_obj->current_date_));
                        calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;

                        epoll_obj->goodtp_recv_consume_.update((u16)calc_delta_us);
                    } else {
                        #ifdef _SELFDEBUG
                        cos_getCurSysTime(&(epoll_obj->current_date_));
                        calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;
                        #endif
                    }
                } else {
                    #ifdef _SELFDEBUG
                    cos_getCurSysTime(&(epoll_obj->current_date_));
                    calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;
                    #endif
                }

                #ifdef _SELFDEBUG
                if (500 < calc_delta_us) {
                    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_warning,
                            "call GtpPacketReceive() too long(%lluus) in No.%u epoll thread.\r\n",
                            calc_delta_us, epoll_obj->thread_id_);
                }
                #endif

#if (1 == ENABLE_OUT_CHECK)
gtp_receive_next_pos_:
#endif
                GtpFreePackMem(epoll_obj->goodtp_hdl_, c3buf_mem);

                if (COS_OK != run_result) {
                    *epoll_obj->goodtp_rcv_pack_err_dot_ += 1;
                    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                            "call GtpPacketReceive() failed(0x%08x) in No.%u epoll thread.\r\n",
                            run_result, epoll_obj->thread_id_);
                }

                received_pack_counter += 1;
            } while (4096 > received_pack_counter);
        }

        if (rolerEnum::kClientRoler == epoll_obj->roler_) {
            if (epoll_obj->sending_frame_ts_us_ <= epoll_obj->current_date_.timestamp_us) {
                #if (2 == LARGE_PACKET_TEST)
                length_ = sizeof(GtpTesterFrame) + (cos_genRand() & 0x00000FFF) + BASE_PAYLOAD_SIZE;
                #endif

                #if (1 == LARGE_PACKET_TEST)
                length_ = sizeof(GtpTesterFrame) + (cos_genRand() & 0x000000FF) + BASE_PAYLOAD_SIZE;
                #endif

                #if (0 == LARGE_PACKET_TEST)
                length_ = sizeof(GtpTesterFrame) + BASE_PAYLOAD_SIZE;
                #endif

                c3buf_mem = GtpMallocPackMem(epoll_obj->goodtp_hdl_, NULL, 0, &c3buf_len, (void**)(&tran_addr),
                                             length_);

                if (NULL == c3buf_mem) {
                    *epoll_obj->gtp_buf_malloc_failed_dot_ += 1;

                    goto epoll_loop_next_pos_;
                }

                if ((epoll_obj->switch_server_ip_ts_us_ + 30000000) <= epoll_obj->current_date_.timestamp_us) {
                    epoll_obj->switch_server_ip_ts_us_ = epoll_obj->current_date_.timestamp_us;

                    if (epoll_obj->server_addr_size_ == (&(epoll_obj->server_sock_addr_size_))) {
                        epoll_obj->server_addr_size_ = &(epoll_obj->backup_server_sock_addr_size_);
                        epoll_obj->server_addr_      = &(epoll_obj->backup_server_sock_addr_[0]);
                    } else {
                        epoll_obj->server_addr_size_ = &(epoll_obj->server_sock_addr_size_);
                        epoll_obj->server_addr_      = &(epoll_obj->server_sock_addr_[0]);
                    }
                }

                tran_addr->context_       = epoll_obj;
                tran_addr->sfd_           = (u32)(epoll_obj->send_sfd_);
                tran_addr->sock_addr_len_ = *(epoll_obj->server_addr_size_);
                tran_addr->self_addr_len_ = 0;
                tran_addr->qos_           = FEC_SWITCH;  // turn on fec.
                tran_addr->stream_type_   = GTP_STREAM_TYPE;
                tran_addr->smooth_jitter_ = GTP_REMOVE_JITTER;
                tran_addr->enable_key_    = 0;

                memcpy(tran_addr->sock_addr_, epoll_obj->server_addr_, tran_addr->sock_addr_len_);

                cos_getCurSysTime(&(epoll_obj->current_date_));

                frame = (GtpTesterFrame*)(c3buf_mem);

                frame->header_       = FRAME_HEADER;
                frame->length_       = length_;

                switch (epoll_obj->state_machin_) {
                case kIdle: {
                    frame->type_ = (u32)(FrameType::kFrameCmdCrtFile);
                    frame->sn_   = cmd_counter;
                    cmd_counter += 1;
                    break;
                }

                case kResCreateFile: {
                    frame->type_ = (u32)(FrameType::kFrameCmdReqTranDt);
                    frame->sn_   = cmd_counter;
                    cmd_counter += 1;
                    break;
                }

                case kResTranData: {
                    frame->type_ = (u32)(FrameType::kFrameDataType);
                    frame->sn_   = epoll_obj->application_sn_;

                    epoll_obj->application_sn_ += 1;
                    break;
                }

                default: {
                    frame->type_ = (u32)(FrameType::kFrameDataType);
                    frame->sn_   = epoll_obj->application_sn_;

                    epoll_obj->application_sn_ += 1;
                }
                }

                frame->sender_ts_us_ = epoll_obj->current_date_.timestamp_us;
                frame->check_sum_    = 0;
                frame->check_sum_    = CalcCheckSum((u8*)frame, length_);

                if (COS_YES == epoll_obj->consume_switch_) {
                    cos_getCurSysTime(&(epoll_obj->current_date_));

                    calc_delta_us         = epoll_obj->current_date_.timestamp_us;
                    tran_addr->timestamp_ = epoll_obj->current_date_.timestamp_us;
                } else {
                    #ifdef _SELFDEBUG
                    cos_getCurSysTime(&(epoll_obj->current_date_));
                    calc_delta_us = epoll_obj->current_date_.timestamp_us;
                    #endif
                }

                run_result = GtpFrameSend(epoll_obj->goodtp_hdl_, frame, length_, tran_addr, 0, 0);

                #ifdef _SELFDEBUG
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;
                if (500 < calc_delta_us) {
                    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                            "call GtpFrameSend() too long(%lluus) in No.%u epoll thread.\r\n",
                            calc_delta_us, epoll_obj->thread_id_);
                }
                #endif

                GtpFreePackMem(epoll_obj->goodtp_hdl_, c3buf_mem);

                if (COS_OK != run_result) {
                    *epoll_obj->goodtp_snd_frame_err_dot_ += 1;
                    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                            "call GtpFrameSend() failed(0x%08x) in No.%u epoll thread.\r\n",
                            run_result, epoll_obj->thread_id_);
                }

                epoll_obj->sending_frame_ts_us_ = epoll_obj->current_date_.timestamp_us + epoll_obj->send_period_ts_us_;
            }
        }

        if (epoll_obj->call_goodtp_timer_ts_us_ <= epoll_obj->current_date_.timestamp_us) {
            if (COS_YES == epoll_obj->consume_switch_) {
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us;
            }

            run_result = PeriodGtpTimer(epoll_obj->goodtp_hdl_);

            if (COS_YES == epoll_obj->consume_switch_) {
                cos_getCurSysTime(&(epoll_obj->current_date_));
                calc_delta_us = epoll_obj->current_date_.timestamp_us - calc_delta_us;

                epoll_obj->goodtp_timer_consume_.update((u16)calc_delta_us);
            }

            if (COS_OK != run_result) {
                *epoll_obj->goodtp_timer_failed_dot_ += 1;
                COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                        "call PeriodGtpTimer() failed(0x%08x) in No.%u epoll thread.\r\n",
                        run_result, epoll_obj->thread_id_);
            }

            epoll_obj->call_goodtp_timer_ts_us_ = epoll_obj->current_date_.timestamp_us + C3PTP_TIMER_PERIOD_US;
        }

epoll_loop_next_pos_:
        if (0 != active_num) {
            #ifdef __linux__
            memset(pevents, 0x00, (sizeof(struct epoll_event) * active_num));
            #endif
        }

        if (epoll_obj->check_resource_period_ts_us_ <= epoll_obj->current_date_.timestamp_us) {
            epoll_obj->check_resource_period_ts_us_ = epoll_obj->current_date_.timestamp_us + CHECK_RESOURCE_PERIOD_US;
            epoll_obj->CheckResource();
        }
    }

exit_thread_pos_:
    if (rolerEnum::kServerRoler == epoll_obj->roler_) {
        for (u32 i = 0; ((u32)(epoll_obj->listen_sfd_.size())) > i; ++i) {
            cos_closeSock(epoll_obj->listen_sfd_[i]);
            epoll_obj->listen_sfd_[i] = COS_INVALID_HDL;
        }

        {
            vector<cos_sock> temp_vec;
            epoll_obj->listen_sfd_.swap(temp_vec);
        }
    } else {
        if (COS_INVALID_HDL != epoll_obj->send_sfd_) {
            cos_closeSock(epoll_obj->send_sfd_);
            epoll_obj->send_sfd_ = COS_INVALID_HDL;
        }
    }

    #ifdef __linux__
    if (-1 != epoll_obj->epoll_id_) {
        close(epoll_obj->epoll_id_);
        epoll_obj->epoll_id_ = -1;
    }
    #endif

    COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_warning, "Exit thread in No.%u epoll thread.\r\n",
            epoll_obj->thread_id_);

    epoll_obj->epoll_thread_enable_flag_ = COS_NO;
    epoll_obj->epoll_thread_runned_flag_ = COS_NO;
    epoll_obj->epoll_thread_exited_flag_ = COS_YES;

    #ifdef __linux__
    pthread_exit(COS_OK);
    #endif

    #if (_WIN32 || _WIN64)
    return COS_EXIST_THREAD_CODE;
    #endif
}

void TranEpoll::ShowSessionBitMap(const u8 *src_ip, const u8 *dst_ip) {
    u8 buf[8192] = {0};

    GtpLinkerKey link_key;

    link_key.sfd_                  = (u32)send_sfd_;
    link_key.direction_            = 1;
    link_key.peer_socket_addr_len_ = server_sock_addr_size_;

    memcpy(link_key.peer_socket_addr_, server_sock_addr_, server_sock_addr_size_);

    u32 result = PrintSessionWinBitMap(goodtp_hdl_, src_ip, dst_ip, &(buf[0]), 8192);
    if (GTP_OK != result) {
        cos_printf("Call PrintSessionWinBitMap() failed(0x%08x) in No.%u epoll\r\n", result, thread_id_);
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "Call PrintSessionWinBitMap() failed(0x%08x) in No.%u epoll.\r\n", result, thread_id_);
        return;
    }

    cos_printf("%s\r\n", &(buf[0]));

    return;
}

void TranEpoll::ShowFecParameter(const u8 *src_ip, const u8 *dst_ip) {
    #if (1 == NEW_GOODTP_LIB)
    u8 buf[4096] = {0};

    GtpLinkerKey link_key;

    link_key.sfd_                  = (u32)send_sfd_;
    link_key.direction_            = 1;
    link_key.peer_socket_addr_len_ = server_sock_addr_size_;

    memcpy(link_key.peer_socket_addr_, server_sock_addr_, server_sock_addr_size_);

    u32 result = GetAlgorithmParam(goodtp_hdl_, src_ip, dst_ip, &(buf[0]), 4096);
    if (GTP_OK != result) {
        cos_printf("Call GetFecParameter() failed(0x%08x) in No.%u epoll\r\n", result, thread_id_);
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "Call GetFecParameter() failed(0x%08x) in No.%u epoll.\r\n", result, thread_id_);
        return;
    }

    cos_printf("%s\r\n", &(buf[0]));
    #endif

    return;
}

u32 CmdQueryLinkerQuality(u32 bit_map_flag) {
    u32 nloop = 0;

    while (g_epoll_thread_mgr.epoll_obj_.size() > nloop) {
        if (NULL != g_epoll_thread_mgr.epoll_obj_[nloop]) {
            g_epoll_thread_mgr.epoll_obj_[nloop]->PrintLinkerQuality(bit_map_flag);
        }
        nloop += 1;
    }

    return COS_OK;
}

u32 CmdCreateTester(u8 *client_ip, u8 *server_ip, u32 server_port, u32 auto_show, u32 pps, u32 type) {
    if (rolerEnum::kServerRoler == g_epoll_thread_mgr.roler_) {
        cos_printf("Current process is server, it can't create linker tester.\r\n");
        return COS_ERR;
    }

    if (0 == pps) {
        cos_printf("invalid pps(%u).\r\n", pps);
        return COS_ERR;
    }

    cos_printf("Current tester server is %s:%u(pps=%u)\r\n", server_ip, server_port, pps);

    u32 tester_id = GenTesterId();

    vector<string> tmp_ip;

    tmp_ip.emplace_back(string((char*)server_ip));

    const string send_ip = (char*)client_ip;

    #ifdef __linux__
    g_frame_fp = fopen("/run/frame.log", "w");
    #endif

    #if (_WIN32 || _WIN64)
    g_frame_fp = _wfopen(L"..\\log\\frame.log", L"w");
    #endif

    TranEpoll *tran_obj = new TranEpoll(rolerEnum::kClientRoler, pps, tester_id, tmp_ip, server_port,
                                        server_port, send_ip, NULL, auto_show);
    if (NULL == tran_obj) {
        cos_printf("Create No.%u tester failed.\r\n", tester_id);
        return COS_ERR;
    }

    tran_obj->Running(type);

    cos_lock(g_epoll_thread_mgr.cos_mutex_);

    g_epoll_thread_mgr.epoll_obj_.emplace_back(tran_obj);

    cos_unlock(g_epoll_thread_mgr.cos_mutex_);

    cos_printf("Create No.%u tester sucess.\r\n", tester_id);

    return COS_OK;
}

u32 CmdFileTranTester(u8 *client_ip, u8 *server_ip, u32 server_port, u32 speed_kbps) {
    if (rolerEnum::kServerRoler == g_epoll_thread_mgr.roler_) {
        cos_printf("Current process is server, it can't create linker tester.\r\n");
        return COS_ERR;
    }

    vector<string> tmp_ip;

    tmp_ip.emplace_back(string((char*)server_ip));

    const string send_ip = (char*)client_ip;

    FILE *bin_fp = NULL;

    #ifdef __linux__
    bin_fp = fopen("data.bin", "rb");
    #endif

    #if (_WIN32 || _WIN64)
    bin_fp = _wfopen(L"..\\bin\\data.bin", L"rb");
    #endif

    u32 tester_id = GenTesterId();

    u32 pps = 0;

    // change to byte per second.
    speed_kbps = ((speed_kbps << 10) >> 3);
    pps        = speed_kbps / 1316;  // the vedio frame's size is 1316 bytes.
    pps        = ((0 == pps) ? 1 : pps);

    TranEpoll *tran_obj = new TranEpoll(rolerEnum::kClientRoler, pps, tester_id, tmp_ip, server_port,
                                        server_port, send_ip, bin_fp, 0);
    if (NULL == tran_obj) {
        cos_printf("Create No.%u single tester failed.\r\n", tester_id);
        return COS_ERR;
    } 

    tran_obj->Running();

    cos_lock(g_epoll_thread_mgr.cos_mutex_);

    g_epoll_thread_mgr.epoll_obj_.emplace_back(tran_obj);

    cos_unlock(g_epoll_thread_mgr.cos_mutex_);

    cos_printf("Create No.%u tester sucess.\r\n", tester_id);

    return COS_OK;
}

u32 CmdDeleteTester(u32 tester_id) {
    if (rolerEnum::kServerRoler == g_epoll_thread_mgr.roler_) {
        cos_printf("Current process is server, it can't delete linker tester.\r\n");
        return COS_ERR;
    }

    u32 loop = 0;

    if (0 == tester_id) {
        for (loop = 0; g_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
            if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
                continue;
            }

            g_epoll_thread_mgr.epoll_obj_[loop]->Stop();
        }

        for (loop = 0; g_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
            if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
                continue;
            }

            while (COS_NO == g_epoll_thread_mgr.epoll_obj_[loop]->epoll_thread_exited_flag_) {
                cos_threadSleep(1);
            }
        }

        for (loop = 0; g_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
            if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
                continue;
            }

            cos_printf("Deleted No.%u tester\r\n", g_epoll_thread_mgr.epoll_obj_[loop]->thread_id_);

            delete g_epoll_thread_mgr.epoll_obj_[loop];
            g_epoll_thread_mgr.epoll_obj_[loop] = NULL;
        }

        vector<TranEpoll*> tmp;
        g_epoll_thread_mgr.epoll_obj_.swap(tmp);

        g_tester_id_bit_map = 0;

        if (NULL != g_frame_fp) {
            fclose(g_frame_fp);
            g_frame_fp = NULL;
        }

        return COS_OK;
    }

    for (loop = 0; g_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
        if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
            continue;
        }

        if (tester_id == g_epoll_thread_mgr.epoll_obj_[loop]->thread_id_) {
            g_epoll_thread_mgr.epoll_obj_[loop]->Stop();

            while (COS_NO == g_epoll_thread_mgr.epoll_obj_[loop]->epoll_thread_exited_flag_) {
                cos_threadSleep(1);
            }

            cos_printf("Deleted No.%u tester\r\n", g_epoll_thread_mgr.epoll_obj_[loop]->thread_id_);

            delete g_epoll_thread_mgr.epoll_obj_[loop];

            g_epoll_thread_mgr.epoll_obj_.erase(g_epoll_thread_mgr.epoll_obj_.begin() + loop);
            break;
        }
    }

    return COS_OK;
}

u32 CmdSetConsumeSwitch(u32 switch_value) {
    if (0 != switch_value) {
        switch_value = COS_YES;
    }

    u32 loop = 0;

    while (g_epoll_thread_mgr.epoll_obj_.size() > loop) {
        if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
            goto set_consume_switch_next_pos_;
        }

        g_epoll_thread_mgr.epoll_obj_[loop]->sock_send_consume_.clear();
        g_epoll_thread_mgr.epoll_obj_[loop]->sock_recv_consume_.clear();
        g_epoll_thread_mgr.epoll_obj_[loop]->goodtp_send_consume_.clear();
        g_epoll_thread_mgr.epoll_obj_[loop]->goodtp_recv_consume_.clear();
        g_epoll_thread_mgr.epoll_obj_[loop]->goodtp_timer_consume_.clear();

        g_epoll_thread_mgr.epoll_obj_[loop]->consume_switch_ = switch_value;

set_consume_switch_next_pos_:
        loop += 1;
    }

    return COS_OK;
}

u32 CmdShowConsumeStat(void) {
    u32 loop = 0;
    u32 counter = 0;

    StatResult sock_send_reslt;
    StatResult sock_recv_reslt;
    StatResult c3ptp_send_reslt;
    StatResult c3ptp_recv_reslt;
    StatResult c3ptp_timer_reslt;

    while (g_epoll_thread_mgr.epoll_obj_.size() > loop) {
        if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
            goto set_consume_switch_next_pos_;
        }

        if (0 == g_epoll_thread_mgr.epoll_obj_[loop]->linker_quality_map_.size()) {
            goto set_consume_switch_next_pos_;
        }

        cos_printf("~~~~~~~~~~~~~~~~~~~~~~~~~~No.%u epoll ~~~~~~~~~~~~~~~~~~~~~~~~~\r\n",
                   g_epoll_thread_mgr.epoll_obj_[loop]->thread_id_);
        cos_printf("avg(us) min(us) max(us) std_value 1std(%) 2std(%) 3std(%) 3std(%)<\r\n");

        g_epoll_thread_mgr.epoll_obj_[loop]->sock_send_consume_.doStat();
        {
            const StatResult &temp_reslt = g_epoll_thread_mgr.epoll_obj_[loop]->sock_send_consume_.getStatResult();
            sock_send_reslt += temp_reslt;

            cos_printf("socket send consume tm: %4u %4u %6u %4u %5.2f%% %5.2f%% %5.2f%% %5.2f%%\r\n",
                       temp_reslt.avg_value_, temp_reslt.min_value_, temp_reslt.max_value_,
                       temp_reslt.std_value_, temp_reslt.one_std_rate_, temp_reslt.two_std_rate_,
                       temp_reslt.three_std_rate_, temp_reslt.other_std_rate_);
        }

        g_epoll_thread_mgr.epoll_obj_[loop]->sock_recv_consume_.doStat();
        {
            const StatResult &temp_reslt = g_epoll_thread_mgr.epoll_obj_[loop]->sock_recv_consume_.getStatResult();
            sock_recv_reslt += temp_reslt;

            cos_printf("socket recv consume tm: %4u %4u %6u %4u %5.2f%% %5.2f%% %5.2f%% %5.2f%%\r\n",
                       temp_reslt.avg_value_, temp_reslt.min_value_, temp_reslt.max_value_,
                       temp_reslt.std_value_, temp_reslt.one_std_rate_, temp_reslt.two_std_rate_,
                       temp_reslt.three_std_rate_, temp_reslt.other_std_rate_);
        }

        g_epoll_thread_mgr.epoll_obj_[loop]->goodtp_send_consume_.doStat();
        {
            const StatResult &temp_reslt = g_epoll_thread_mgr.epoll_obj_[loop]->goodtp_send_consume_.getStatResult();
            c3ptp_send_reslt += temp_reslt;

            cos_printf("   gtp send consume tm: %4u %4u %6u %4u %5.2f%% %5.2f%% %5.2f%% %5.2f%%\r\n",
                       temp_reslt.avg_value_, temp_reslt.min_value_, temp_reslt.max_value_,
                       temp_reslt.std_value_, temp_reslt.one_std_rate_, temp_reslt.two_std_rate_,
                       temp_reslt.three_std_rate_, temp_reslt.other_std_rate_);
        }

        g_epoll_thread_mgr.epoll_obj_[loop]->goodtp_recv_consume_.doStat();
        {
            const StatResult &temp_reslt = g_epoll_thread_mgr.epoll_obj_[loop]->goodtp_recv_consume_.getStatResult();
            c3ptp_recv_reslt += temp_reslt;

            cos_printf("   gtp recv consume tm: %4u %4u %6u %4u %5.2f%% %5.2f%% %5.2f%% %5.2f%%\r\n",
                       temp_reslt.avg_value_, temp_reslt.min_value_, temp_reslt.max_value_,
                       temp_reslt.std_value_, temp_reslt.one_std_rate_, temp_reslt.two_std_rate_,
                       temp_reslt.three_std_rate_, temp_reslt.other_std_rate_);
        }

        g_epoll_thread_mgr.epoll_obj_[loop]->goodtp_timer_consume_.doStat();
        {
            const StatResult &temp_reslt = g_epoll_thread_mgr.epoll_obj_[loop]->goodtp_timer_consume_.getStatResult();
            c3ptp_timer_reslt += temp_reslt;

            cos_printf("  gtp timer consume tm: %4u %4u %6u %4u %5.2f%% %5.2f%% %5.2f%% %5.2f%%\r\n",
                       temp_reslt.avg_value_, temp_reslt.min_value_, temp_reslt.max_value_,
                       temp_reslt.std_value_, temp_reslt.one_std_rate_, temp_reslt.two_std_rate_,
                       temp_reslt.three_std_rate_, temp_reslt.other_std_rate_);
        }
        cos_printf("\r\n");

        counter += 1;

set_consume_switch_next_pos_:
        loop += 1;
    }

    if (0 != counter) {
        sock_send_reslt   /= counter;
        sock_recv_reslt   /= counter;
        c3ptp_send_reslt  /= counter;
        c3ptp_recv_reslt  /= counter;
        c3ptp_timer_reslt /= counter;
    }

    cos_printf("\r\nepoll threads average stat: avg(us) min(us) max(us) std_value 1std(%) 2std(%) 3std(%) 3std(%)<\r\n");

    cos_printf("socket send consume tm: %4u %4u %6u %4u %5.2f%% %5.2f%% %5.2f%% %5.2f%%\r\n",
               sock_send_reslt.avg_value_, sock_send_reslt.min_value_, sock_send_reslt.max_value_,
               sock_send_reslt.std_value_, sock_send_reslt.one_std_rate_, sock_send_reslt.two_std_rate_,
               sock_send_reslt.three_std_rate_, sock_send_reslt.other_std_rate_);

    cos_printf("socket recv consume tm: %4u %4u %6u %4u %5.2f%% %5.2f%% %5.2f%% %5.2f%%\r\n",
               sock_recv_reslt.avg_value_, sock_recv_reslt.min_value_, sock_recv_reslt.max_value_,
               sock_recv_reslt.std_value_, sock_recv_reslt.one_std_rate_, sock_recv_reslt.two_std_rate_,
               sock_recv_reslt.three_std_rate_, sock_recv_reslt.other_std_rate_);

    cos_printf("   gtp send consume tm: %4u %4u %6u %4u %5.2f%% %5.2f%% %5.2f%% %5.2f%%\r\n",
               c3ptp_send_reslt.avg_value_, c3ptp_send_reslt.min_value_, c3ptp_send_reslt.max_value_,
               c3ptp_send_reslt.std_value_, c3ptp_send_reslt.one_std_rate_, c3ptp_send_reslt.two_std_rate_,
               c3ptp_send_reslt.three_std_rate_, c3ptp_send_reslt.other_std_rate_);

    cos_printf("   gtp recv consume tm: %4u %4u %6u %4u %5.2f%% %5.2f%% %5.2f%% %5.2f%%\r\n",
               c3ptp_recv_reslt.avg_value_, c3ptp_recv_reslt.min_value_, c3ptp_recv_reslt.max_value_,
               c3ptp_recv_reslt.std_value_, c3ptp_recv_reslt.one_std_rate_, c3ptp_recv_reslt.two_std_rate_,
               c3ptp_recv_reslt.three_std_rate_, c3ptp_recv_reslt.other_std_rate_);

    cos_printf("  gtp timer consume tm: %4u %4u %6u %4u %5.2f%% %5.2f%% %5.2f%% %5.2f%%\r\n",
               c3ptp_timer_reslt.avg_value_, c3ptp_timer_reslt.min_value_, c3ptp_timer_reslt.max_value_,
               c3ptp_timer_reslt.std_value_, c3ptp_timer_reslt.one_std_rate_, c3ptp_timer_reslt.two_std_rate_,
               c3ptp_timer_reslt.three_std_rate_, c3ptp_timer_reslt.other_std_rate_);

    return COS_OK;
}

u32 CmdSetFecCfg(uint8_t mode, uint8_t horizonal_fec_flag, uint8_t vertical_fec_flag, uint8_t uphill_flag,
    uint8_t downhill_flag, uint8_t horizonal_power, uint8_t vertical_power) {
    if (mode == 0) {
        cos_printf("error param!");
        return COS_ERR;
    } else if(mode == 1) {
        cos_printf("Block ");
    } else if (mode == 2) {
        cos_printf("Convolution ");
    }

    if (horizonal_fec_flag) cos_printf("Horizional ");
    if (vertical_fec_flag) cos_printf("Vertical ");
    if (uphill_flag) cos_printf("Uphill ");
    if (downhill_flag) cos_printf("Downhill ");

    cos_printf("horizonal power %d vertical power %d \r\n", horizonal_power, vertical_power);

    // SetFecCfg(mode, horizonal_fec_flag, vertical_fec_flag, uphill_flag, downhill_flag, horizonal_power, vertical_power);

    return COS_OK;
}

u32 CmdSetAppSlidWin(u32 value) {
    u32 switch_val = COS_OFF;
    if (0 != value) {
        switch_val = COS_ON;
    }

    for (u32 loop = 0; g_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
        if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
            continue;
        }

        g_epoll_thread_mgr.epoll_obj_[loop]->app_entry_win_switch_ = switch_val;
    }

    return COS_OK;
}

u32 CmdGetLinkerQuality(u32 pos) {
    u32 quality_pos = 1;

    if (0 != pos) {
        quality_pos = 2;
    }

    for (u32 loop = 0; g_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
        if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
            continue;
        }

        g_epoll_thread_mgr.epoll_obj_[loop]->ShowLinkerQuality(quality_pos);
    }

    return COS_OK;
}

u32 CmdPrintBitMap(u8 *src_ip, u8 *dst_ip) {
    for (u32 loop = 0; g_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
        if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
            continue;
        }

        g_epoll_thread_mgr.epoll_obj_[loop]->ShowSessionBitMap(src_ip, dst_ip);
    }

    return COS_OK;
}

u32 CmdPrintFecParameter(u8 *src_ip, u8 *dst_ip) {
    for (u32 loop = 0; g_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
        if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
            continue;
        }

        g_epoll_thread_mgr.epoll_obj_[loop]->ShowFecParameter(src_ip, dst_ip);
    }

    return COS_OK;
}

u32 CmdPrintMemPoolStatus(void) {
    u8 mem[4096] = {0};
    u32 nret = COS_OK;

    for (u32 loop = 0; g_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
        if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
            continue;
        }

        nret = GetPackMemPoolStatus(g_epoll_thread_mgr.epoll_obj_[loop]->goodtp_hdl_, mem, 4096);
        if (COS_OK == nret) {
            cos_printf("No.%02u epool:\r\n%s\r\n", loop, mem);
            memset(mem, 0x00, 4096);
        } else {
            cos_printf("No.%02u epool:\r\nCall GetPackMemPoolStatus() failed(0x%08x)\r\n", loop, nret);
        }
    }

    return COS_OK;
}

u32 CmdShowSessionNumber(void) {
    u32 sum  = 0;
    u32 loop = 0;
    u32 num  = 0;

    for (loop = 0; g_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
        if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
            continue;
        }

        num  = GetSessionNumber(g_epoll_thread_mgr.epoll_obj_[loop]->goodtp_hdl_);
        sum += num;

        cos_printf("The No.%u install has %u sessions.\r\n", loop, num);
    }

    cos_printf("Total session number= %u.\r\n", sum);

    return COS_OK;
}

void RegisterCommand(void) {
    u32 com_variable = 0;
    u32 length       = 0;

    cos_symbol_item_stru symbItem;

    // registers query linker quality command.
    memset(&symbItem, 0x00, sizeof(symbItem));

    symbItem.enSymbolAuth = cos_symbol_auth_normal;
    symbItem.enSymbolType = cos_symbol_type_function;
    symbItem.symbolAddr   = (void*)CmdQueryLinkerQuality;

    strcpy((char*)(symbItem.symbolName), "pQueryLinkerQuality");

    sprintf((char*)(symbItem.helpInfo),
            "The command execution format is pQueryLinkerQuality [param]. "\
            "It shows all the current linkers quality.\r\n");

    com_variable = (u32)strlen((char*)(symbItem.helpInfo)) + 1;

    sprintf((char*)(&(symbItem.helpInfo[com_variable])),
            "\r\n The param parameter is optional, it maybe h or ? char:\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "  -h/?:\r\n\tIt shows this command help information.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "  -others or no input:\r\n\tIt shows all the current linkers quality.\r\n\n");

    cos_registerCodeSymbol(&symbItem);

    // registers create tester object command.
    memset(&symbItem, 0x00, sizeof(symbItem));

    symbItem.enSymbolAuth = cos_symbol_auth_normal;
    symbItem.enSymbolType = cos_symbol_type_function;
    symbItem.symbolAddr   = (void*)CmdCreateTester;

    strcpy((char*)(symbItem.symbolName), "dNewTester");

    sprintf((char*)(symbItem.helpInfo),
            "The command execution format is dNewTester send_ip server_ip server_port auto_show pps type. "\
            "It creates a tester.\r\n");

    com_variable = (u32)strlen((char*)(symbItem.helpInfo)) + 1;

    sprintf((char*)(&(symbItem.helpInfo[com_variable])),
            "\r\n The server_ip parameter is server's ip address, it maybe as follows:\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "  -h/?:\r\n\tIt shows this command help information.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "  -string:\r\n\tThe send ip address.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "\r\n The server_ip is the udp server ip\r\n\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "\r\n The server_port is the udp server port.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "\r\n The auto_show is configured for automaticly showing network quality, 0 for disable, 1 for enable.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "\r\n The pps is how many packets are sented per secondmaybe, the min value is one, and the "\
            "max value is 100000 for type=0, sending period for type!=0(period=10000000/pps us).\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "\r\n The type is testing mode, 0 means uniform send and unidir, others means block send and bidir(num=type*10, 0<type<11).\r\n");

    cos_registerCodeSymbol(&symbItem);

    // registers create tester object command.
    memset(&symbItem, 0x00, sizeof(symbItem));

    symbItem.enSymbolAuth = cos_symbol_auth_normal;
    symbItem.enSymbolType = cos_symbol_type_function;
    symbItem.symbolAddr   = (void*)CmdFileTranTester;

    strcpy((char*)(symbItem.symbolName), "dSend");

    sprintf((char*)(symbItem.helpInfo),
            "The command execution format is dTranFile send_ip server_ip server_port code_kbps. "\
            "It only send one packet testing.\r\n");

    com_variable = (u32)strlen((char*)(symbItem.helpInfo)) + 1;

    sprintf((char*)(&(symbItem.helpInfo[com_variable])),
            "\r\n The server_ip parameter is server's ip address, it maybe as follows:\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "  -h/?:\r\n\tIt shows this command help information.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "  -string:\r\n\tThe send ip address.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "\r\n The server_ip is the udp server ip\r\n\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "\r\n The server_port is the udp server port.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "\r\n The code_kbps is code rate, unit: kbps.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    cos_registerCodeSymbol(&symbItem);

    // registers delete tester object command.
    memset(&symbItem, 0x00, sizeof(symbItem));

    symbItem.enSymbolAuth = cos_symbol_auth_normal;
    symbItem.enSymbolType = cos_symbol_type_function;
    symbItem.symbolAddr   = (void*)CmdDeleteTester;

    strcpy((char*)(symbItem.symbolName), "dDelTester");

    sprintf((char*)(symbItem.helpInfo),
            "The command execution format is dDelTester tester_id. "\
            "It shows all the current linkers quality.\r\n");

    com_variable = (u32)strlen((char*)(symbItem.helpInfo)) + 1;

    sprintf((char*)(&(symbItem.helpInfo[com_variable])),
            "\r\n The tester_id parameter is optional, it maybe as follows:\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "  -h/?:\r\n\tIt shows this command help information.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "  -integer:\r\n\tThe tester id, 0 means delete all the testers.\r\n\n");

    cos_registerCodeSymbol(&symbItem);

    // registers delete tester object command.
    memset(&symbItem, 0x00, sizeof(symbItem));

    symbItem.enSymbolAuth = cos_symbol_auth_normal;
    symbItem.enSymbolType = cos_symbol_type_function;
    symbItem.symbolAddr   = (void*)CmdSetConsumeSwitch;

    strcpy((char*)(symbItem.symbolName), "sSetConsumeSwitch");

    sprintf((char*)(symbItem.helpInfo),
            "The command execution format is sSetConsumeSwitch switch_value. "\
            "It will turn on or turn off calc software running consume time.\r\n");

    com_variable = (u32)strlen((char*)(symbItem.helpInfo)) + 1;

    sprintf((char*)(&(symbItem.helpInfo[com_variable])),
            "\r\n The switch_value parameter maybe as follows:\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "  -h/?:\r\n\tIt shows this command help information.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "  -integer:\r\n\tTurn on or turn off switch(0:off, others:on).\r\n\n");

    cos_registerCodeSymbol(&symbItem);

    // registers delete tester object command.
    memset(&symbItem, 0x00, sizeof(symbItem));

    symbItem.enSymbolAuth = cos_symbol_auth_normal;
    symbItem.enSymbolType = cos_symbol_type_function;
    symbItem.symbolAddr   = (void*)CmdShowConsumeStat;

    strcpy((char*)(symbItem.symbolName), "pShowConsumeStat");

    sprintf((char*)(symbItem.helpInfo),
            "The command execution format is pShowConsumeStat param. "\
            "It shows the software running consume time.\r\n");

    com_variable = (u32)strlen((char*)(symbItem.helpInfo)) + 1;

    sprintf((char*)(&(symbItem.helpInfo[com_variable])),
            "\r\n The param parameter is optional, it maybe as follows:\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "  -h/?:\r\n\tIt shows this command help information.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "  -others or no input:\r\n\tIt shows the software running consume time.\r\n\n");

    cos_registerCodeSymbol(&symbItem);

    symbItem.enSymbolAuth = cos_symbol_auth_normal;
    symbItem.enSymbolType = cos_symbol_type_function;
    symbItem.symbolAddr   = (void*)CmdSetFecCfg;

    strcpy((char*)(symbItem.symbolName), "sSetFecCfg");

    sprintf((char*)(symbItem.helpInfo),
            "The command execution format is sSetFecCfg param [1:Block, 2:Convolution] [horizon flag] [vertical flag] "\
            "[uphill flag] [downhill flag] [horizon power] [vertical power]. "\
            "It sets fec config.\r\n");

    com_variable = (u32)strlen((char*)(symbItem.helpInfo)) + 1;

    sprintf((char*)(&(symbItem.helpInfo[com_variable])),
            "\r\n The param parameter is optional, it maybe as follows:\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "  -h/?:\r\n\tIt shows this command help information.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "  -others or no input:\r\n\tIt set fec config.\r\n\n");

    cos_registerCodeSymbol(&symbItem);

    // registers delete tester object command.
    memset(&symbItem, 0x00, sizeof(symbItem));

    symbItem.enSymbolAuth = cos_symbol_auth_normal;
    symbItem.enSymbolType = cos_symbol_type_function;
    symbItem.symbolAddr   = (void*)CmdSetAppSlidWin;

    strcpy((char*)(symbItem.symbolName), "sSetAppSlidWin");

    sprintf((char*)(symbItem.helpInfo),
            "The command execution format is sSetAppSlidWin [param]. "\
            "It can set application slid window enable or disable.\r\n");

    com_variable = (u32)strlen((char*)(symbItem.helpInfo)) + 1;

    sprintf((char*)(&(symbItem.helpInfo[com_variable])),
            "\r\n The param parameter is optional, it maybe as follows:\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "  -h/?:\r\n\tIt shows this command help information.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "  -others integer:\r\n\tIt set application slid window enable or disable(0(default): off, 1: on).\r\n\n");

    cos_registerCodeSymbol(&symbItem);

    // registers delete tester object command.
    memset(&symbItem, 0x00, sizeof(symbItem));

    symbItem.enSymbolAuth = cos_symbol_auth_normal;
    symbItem.enSymbolType = cos_symbol_type_function;
    symbItem.symbolAddr   = (void*)CmdGetLinkerQuality;

    strcpy((char*)(symbItem.symbolName), "pLinkerQuality");

    sprintf((char*)(symbItem.helpInfo),
            "The command execution format is pLinkerQuality [param]. "\
            "It view linker quality.\r\n");

    com_variable = (u32)strlen((char*)(symbItem.helpInfo)) + 1;

    sprintf((char*)(&(symbItem.helpInfo[com_variable])),
            "\r\n The param parameter is optional, it maybe as follows:\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "  -h/?:\r\n\tIt shows this command help information.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "  -0:\r\n\tIt view sender linker quality, default\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "  -1:\r\n\tIt view receiver linker quality\r\n");

    cos_registerCodeSymbol(&symbItem);

    // registers delete tester object command.
    memset(&symbItem, 0x00, sizeof(symbItem));

    symbItem.enSymbolAuth = cos_symbol_auth_normal;
    symbItem.enSymbolType = cos_symbol_type_function;
    symbItem.symbolAddr   = (void*)CmdPrintBitMap;

    strcpy((char*)(symbItem.symbolName), "pPrintBitMap");

    sprintf((char*)(symbItem.helpInfo),
            "The command execution format is pPrintBitMap src_ip dst_ip. "\
            "It shows the session window bitmap.\r\n");

    com_variable = (u32)strlen((char*)(symbItem.helpInfo)) + 1;

    sprintf((char*)(&(symbItem.helpInfo[com_variable])),
            "\r\n The param parameter is optional, it maybe as follows:\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "  -h/?:\r\n\tIt shows this command help information.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "  -others or no input:\r\n\tIt shows the software running consume time");

    cos_registerCodeSymbol(&symbItem);

    // registers delete tester object command.
    memset(&symbItem, 0x00, sizeof(symbItem));

    symbItem.enSymbolAuth = cos_symbol_auth_normal;
    symbItem.enSymbolType = cos_symbol_type_function;
    symbItem.symbolAddr   = (void*)CmdPrintFecParameter;

    strcpy((char*)(symbItem.symbolName), "pPrintFecParam");

    sprintf((char*)(symbItem.helpInfo),
            "The command execution format is pPrintFecParam src_ip dst_ip. "\
            "It shows the session fec current parameter.\r\n");

    com_variable = (u32)strlen((char*)(symbItem.helpInfo)) + 1;

    sprintf((char*)(&(symbItem.helpInfo[com_variable])),
            "\r\n The param parameter is optional, it maybe as follows:\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "  -h/?:\r\n\tIt shows this command help information.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "  -others or no input:\r\n\tIt shows the software running consume time");

    cos_registerCodeSymbol(&symbItem);

    // registers delete tester object command.
    memset(&symbItem, 0x00, sizeof(symbItem));

    symbItem.enSymbolAuth = cos_symbol_auth_normal;
    symbItem.enSymbolType = cos_symbol_type_function;
    symbItem.symbolAddr   = (void*)CmdPrintMemPoolStatus;

    strcpy((char*)(symbItem.symbolName), "pPrintMemPoolStatus");

    sprintf((char*)(symbItem.helpInfo),
            "The command execution format is pPrintMemPoolStatus [param]. "\
            "It shows the session window bitmap.\r\n");

    com_variable = (u32)strlen((char*)(symbItem.helpInfo)) + 1;

    sprintf((char*)(&(symbItem.helpInfo[com_variable])),
            "\r\n The param parameter is optional, it maybe as follows:\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "  -h/?:\r\n\tIt shows this command help information.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "  -others or no input:\r\n\tIt shows the packet memory pool current status.");

    cos_registerCodeSymbol(&symbItem);

    // registers showing session number command.
    memset(&symbItem, 0x00, sizeof(symbItem));

    symbItem.enSymbolAuth = cos_symbol_auth_normal;
    symbItem.enSymbolType = cos_symbol_type_function;
    symbItem.symbolAddr   = (void*)CmdShowSessionNumber;

    strcpy((char*)(symbItem.symbolName), "pShowSessionNumber");

    sprintf((char*)(symbItem.helpInfo),
            "The command execution format is pShowSessionNumber param. "\
            "It shows all the session number.\r\n");

    com_variable = (u32)strlen((char*)(symbItem.helpInfo)) + 1;

    sprintf((char*)(&(symbItem.helpInfo[com_variable])),
            "\r\n The param parameter is optional, it maybe as follows:\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
            "  -h/?:\r\n\tIt shows this command help information.\r\n");
    length = (u32)strlen((char*)(&(symbItem.helpInfo[com_variable])));

    sprintf((char*)(&(symbItem.helpInfo[com_variable + length])),
           "  -others or no input:\r\n\tIt shows all the session number.\r\n\n");

    cos_registerCodeSymbol(&symbItem);

    return;
}

u32 InitTran(const rolerEnum &roler) {
    g_tester_id_bit_map                  = 0;
    g_epoll_thread_mgr.roler_            = roler;
    g_epoll_thread_mgr.cos_mutex_        = NULL;
    g_epoll_thread_mgr.tid_              = COS_INVALID_TID;
    g_single_epoll_thread_mgr.roler_     = roler;
    g_single_epoll_thread_mgr.cos_mutex_ = NULL;
    g_single_epoll_thread_mgr.tid_       = COS_INVALID_TID;

    g_frame_fp = NULL;
    g_svr_wfp  = NULL;
    g_bin_wfp  = NULL;

    u32 nret = InsLoadGtpModule();
    if (COS_OK != nret) {
        return nret;
    }

    u32 cpu_core_num = cos_getCpuNumber();
    cpu_core_num = ((0 != cpu_core_num) ? cpu_core_num : 1);

    g_epoll_thread_mgr.epoll_obj_.reserve(cpu_core_num << 1);
    g_single_epoll_thread_mgr.epoll_obj_.reserve(10240);

    cos_thread_param_stru  thrdParam;
    memset(&thrdParam, 0x00, sizeof(thrdParam));

    if (rolerEnum::kServerRoler == roler) {
        ConfigReader reader;

        #ifdef __linux__
        reader.ReadConfigFile("../cfg/config.json");
        #endif

        #if (_WIN32 || _WIN64)
        reader.ReadConfigFile("..\\cfg\\config.json");
        #endif

        const vector<string> &ip = reader.GetUdpServerIp();
        const u32 &start_port    = reader.GetUdpServerStartPort();
        const u32 &end_port      = reader.GetUdpServerEndPort();
        const string &send_ip    = reader.GetSendIp();

        u32 loop = 0;

        TranEpoll *tran_obj = NULL;

        do {
            tran_obj = new TranEpoll(roler, 0, loop, ip, start_port, end_port, send_ip, NULL);
            if (NULL == tran_obj) {
                break;
            }

            nret = tran_obj->Running(0);
            if (COS_OK != nret) {
                break;
            }

            g_epoll_thread_mgr.epoll_obj_.emplace_back(tran_obj);

            loop += 1;
        } while (cpu_core_num > loop);

        if (cpu_core_num > loop) {
            RETURN_ERRNO(moduleIdEnum::kModuleEpollId, C3coreErrorCodeEnum::kNewEpollTranObjErr);
        }

        g_epoll_thread_mgr.cos_mutex_ = cos_createMutex();
        if (NULL == g_epoll_thread_mgr.cos_mutex_) {
            RETURN_ERRNO(cos_module_tran_id, cos_create_mutex_failed_err);
        }
    } else {
        g_single_epoll_thread_mgr.cos_mutex_ = cos_createMutex();
        if (NULL == g_single_epoll_thread_mgr.cos_mutex_) {
            RETURN_ERRNO(cos_module_tran_id, cos_create_mutex_failed_err);
        }

        thrdParam.argLen       = 0;
        thrdParam.enInitStatus = cos_thread_init_status_running;
        thrdParam.nStackSize   = 0;
        thrdParam.nThrdId      = COS_INVALID_TID;
        thrdParam.nThreadPri   = 60;
        thrdParam.pThrdInParam = NULL;
        thrdParam.pThrdFunc    = (cos_pThreadFunc)(MonitorSingleTesterThreadEntry);

        nret = cos_createThread(&thrdParam, cos_thread_sched_rtos);
        if (COS_OK != nret) {
            return nret;
        }

        g_single_epoll_thread_mgr.tid_ = thrdParam.nThrdId;
    }

    RegisterCommand();

    return COS_OK;
}

void RemoveTran(void) {
    u32 loop = 0;

    cos_terminateThread(g_single_epoll_thread_mgr.tid_);
    g_single_epoll_thread_mgr.tid_ = COS_INVALID_TID;

    for (loop = 0; g_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
        if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
            continue;
        }

        g_epoll_thread_mgr.epoll_obj_[loop]->Stop();
    }

    for (loop = 0; g_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
        if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
            continue;
        }

        while (COS_NO == g_epoll_thread_mgr.epoll_obj_[loop]->epoll_thread_exited_flag_) {
            cos_threadSleep(1);
        }
    }

    for (loop = 0; g_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
        if (NULL == g_epoll_thread_mgr.epoll_obj_[loop]) {
            continue;
        }

        delete g_epoll_thread_mgr.epoll_obj_[loop];
        g_epoll_thread_mgr.epoll_obj_[loop] = NULL;
    }

    for (loop = 0; g_single_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
        if (NULL == g_single_epoll_thread_mgr.epoll_obj_[loop]) {
            continue;
        }

        g_single_epoll_thread_mgr.epoll_obj_[loop]->Stop();
    }

    for (loop = 0; g_single_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
        if (NULL == g_single_epoll_thread_mgr.epoll_obj_[loop]) {
            continue;
        }

        while (COS_NO == g_single_epoll_thread_mgr.epoll_obj_[loop]->epoll_thread_exited_flag_) {
            cos_threadSleep(1);
        }
    }

    for (loop = 0; g_single_epoll_thread_mgr.epoll_obj_.size() > loop; ++loop) {
        if (NULL == g_single_epoll_thread_mgr.epoll_obj_[loop]) {
            continue;
        }

        delete g_single_epoll_thread_mgr.epoll_obj_[loop];
        g_single_epoll_thread_mgr.epoll_obj_[loop] = NULL;
    }

    if (NULL != g_single_epoll_thread_mgr.cos_mutex_) {
        cos_deleteMutex(g_single_epoll_thread_mgr.cos_mutex_);
        g_single_epoll_thread_mgr.cos_mutex_ = NULL;
    }

    if (NULL != g_epoll_thread_mgr.cos_mutex_) {
        cos_deleteMutex(g_epoll_thread_mgr.cos_mutex_);
        g_epoll_thread_mgr.cos_mutex_ = NULL;
    }

    loop = RmLoadGtpModule();
    if (COS_OK != loop) {
        COS_LOG(moduleIdEnum::kModuleEpollId, cos_log_level_error,
                "call RmLoadGtpModule() failed(0x%08x).\r\n", loop);
    }

    return;
}

#ifdef __cplusplus
}
#endif

