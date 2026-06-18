#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if (defined(TARGET_OS_IOS) && TARGET_OS_IOS) || \
    (defined(TARGET_OS_TV) && TARGET_OS_TV) || \
    (defined(TARGET_OS_WATCH) && TARGET_OS_WATCH)
#define APPLE_USE_OSLOG
#endif
#endif
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(__ANDROID__)
#include <android/log.h>
#elif defined(APPLE_USE_OSLOG)
#include <os/log.h>
#else
#include <sys/syslog.h>
#endif
#if defined(_WIN32)
#include <process.h>
#elif defined(__linux__)
#include <unistd.h>
#include <sys/syscall.h>
#else
#include <pthread.h>
#endif
#if 0
#include <iomanip>
#include <sstream>
#include <thread>
#endif
#include "timespec_helper.h"

#ifndef WEAK
#if defined(_WIN32)
#define WEAK __declspec(selectany)
#else
#define WEAK __attribute__((weak))
#endif
#endif

#if defined(_WIN32) || defined(__ANDROID__) || defined(APPLE_USE_OSLOG)
#define LOG_EMERG   0   /* system is unusable */
#define LOG_ALERT   1   /* action must be taken immediately */
#define LOG_CRIT    2   /* critical conditions */
#define LOG_ERR     3   /* error conditions */
#define LOG_WARNING 4   /* warning conditions */
#define LOG_NOTICE  5   /* normal but significant condition */
#define LOG_INFO    6   /* informational */
#define LOG_DEBUG   7   /* debug-level messages */

#define LOG_PRIMASK 0x07    /* mask to extract priority part (internal) */
/* extract priority */
#define LOG_PRI(p)  ((p) & LOG_PRIMASK)
#define LOG_MAKEPRI(fac, pri)   ((fac) | (pri))
//#define LOG_MAKEPRI(fac, pri) (((fac)<<3)|(pri))

/* facility codes */
#define LOG_KERN        (0<<3)  /* kernel messages */
#define LOG_USER        (1<<3)  /* random user-level messages */
#define LOG_MAIL        (2<<3)  /* mail system */
#define LOG_DAEMON      (3<<3)  /* system daemons */
#define LOG_AUTH        (4<<3)  /* security/authorization messages */
#define LOG_SYSLOG      (5<<3)  /* messages generated internally by syslogd */
#define LOG_LPR         (6<<3)  /* line printer subsystem */
#define LOG_NEWS        (7<<3)  /* network news subsystem */
#define LOG_UUCP        (8<<3)  /* UUCP subsystem */
#define LOG_CRON        (9<<3)  /* clock daemon */
#define LOG_AUTHPRIV    (10<<3) /* security/authorization messages (private) */
/* Facility #10 clashes in DEC UNIX, where */
/* it's defined as LOG_MEGASAFE for AdvFS  */
/* event logging.                          */
#define LOG_FTP         (11<<3) /* ftp daemon */
#if defined(APPLE_USE_OSLOG)
//#define LOG_NTP         (12<<3) /* NTP subsystem */
//#define LOG_SECURITY    (13<<3) /* security subsystems (firewalling, etc.) */
//#define LOG_CONSOLE     (14<<3) /* /dev/console output */
#define LOG_NETINFO     (12<<3) /* NetInfo */
#define LOG_REMOTEAUTH  (13<<3) /* remote authentication/authorization */
#define LOG_INSTALL     (14<<3) /* installer subsystem */
#define LOG_RAS         (15<<3) /* Remote Access Service (VPN / PPP) */
#endif

/* other codes through 15 reserved for system use */
#define LOG_LOCAL0      (16<<3) /* reserved for local use */
#define LOG_LOCAL1      (17<<3) /* reserved for local use */
#define LOG_LOCAL2      (18<<3) /* reserved for local use */
#define LOG_LOCAL3      (19<<3) /* reserved for local use */
#define LOG_LOCAL4      (20<<3) /* reserved for local use */
#define LOG_LOCAL5      (21<<3) /* reserved for local use */
#define LOG_LOCAL6      (22<<3) /* reserved for local use */
#define LOG_LOCAL7      (23<<3) /* reserved for local use */
#if defined(APPLE_USE_OSLOG)
#define LOG_LAUNCHD     (24<<3) /* launchd - general bootstrap daemon */

#define LOG_NFACILITIES 25      /* current number of facilities */
#else
#define LOG_NFACILITIES 24      /* current number of facilities */
#endif

#define LOG_FACMASK 0x03f8      /* mask to extract facility part */
/* facility of pri */
#define LOG_FAC(p)  (((p) & LOG_FACMASK) >> 3)

/*
 * Option flags for openlog.
 *
 * LOG_ODELAY no longer does anything.
 * LOG_NDELAY is the inverse of what it used to be.
 */
#define LOG_PID     0x01    /* log the pid with each message */
#define LOG_CONS    0x02    /* log on the console if errors in sending */
#define LOG_ODELAY  0x04    /* delay open until first syslog() (default) */
#define LOG_NDELAY  0x08    /* don't delay open */
#define LOG_NOWAIT  0x10    /* don't wait for console forks: DEPRECATED */
#define LOG_PERROR  0x20    /* log to stderr as well */

/*
 * arguments to setlogmask.
 */
#define LOG_MASK(pri)   (1 << (pri))        /* mask for one priority */
#define LOG_UPTO(pri)   ((1 << ((pri)+1)) - 1)  /* all priorities through pri */

#if defined(__ANDROID__)
#if 0
/**
 * Android log priority values, in increasing order of priority.
 */
enum android_LogPriority
{
    /** For internal use only.  */
    ANDROID_LOG_UNKNOWN = 0,
    /** The default priority, for internal use only.  */
    ANDROID_LOG_DEFAULT, /* only for SetMinPriority() */
    /** Verbose logging. Should typically be disabled for a release apk. */
    ANDROID_LOG_VERBOSE,
    /** Debug logging. Should typically be disabled for a release apk. */
    ANDROID_LOG_DEBUG,
    /** Informational logging. Should typically be disabled for a release apk. */
    ANDROID_LOG_INFO,
    /** Warning logging. For use with recoverable failures. */
    ANDROID_LOG_WARN,
    /** Error logging. For use with unrecoverable failures. */
    ANDROID_LOG_ERROR,
    /** Fatal logging. For use when aborting. */
    ANDROID_LOG_FATAL,
    /** For internal use only.  */
    ANDROID_LOG_SILENT, /* only for SetMinPriority(); must be last */
};
#endif
inline int to_android_log(int pri)
{
    switch (pri)
    {
    case LOG_EMERG:   return ANDROID_LOG_FATAL;
    case LOG_ALERT:   return ANDROID_LOG_FATAL;
    case LOG_CRIT:    return ANDROID_LOG_FATAL;
    case LOG_ERR:     return ANDROID_LOG_ERROR;
    case LOG_WARNING: return ANDROID_LOG_WARN;
    case LOG_NOTICE:  return ANDROID_LOG_INFO;
    case LOG_INFO:    return ANDROID_LOG_INFO;
    case LOG_DEBUG:   return ANDROID_LOG_DEBUG;
    }
    return ANDROID_LOG_UNKNOWN;
}
#elif defined(APPLE_USE_OSLOG)
//!TARGET_OS_MAC

//TARGET_OS_IPHONE
//TARGET_OS_IOS
//TARGET_OS_TV
//TARGET_OS_WATCH
//TARGET_OS_OSX
//TARGET_OS_MACCATALYST
//TARGET_OS_SIMULATOR
//TARGET_OS_BRIDGE*
//TARGET_OS_XROS*
//TARGET_OS_VISION
//TARGET_OS_MACCATALYST
#if 0
enum os_log_type_t
{
    OS_LOG_TYPE_DEFAULT = 0x00,
    OS_LOG_TYPE_INFO    = 0x01,
    OS_LOG_TYPE_DEBUG   = 0x02,
    OS_LOG_TYPE_ERROR   = 0x10,
    OS_LOG_TYPE_FAULT   = 0x11,
};
#endif
inline os_log_type_t to_os_log(int pri)
{
    switch (pri)
    {
    case LOG_EMERG:   return OS_LOG_TYPE_FAULT;
    case LOG_ALERT:   return OS_LOG_TYPE_FAULT;
    case LOG_CRIT:    return OS_LOG_TYPE_FAULT;
    case LOG_ERR:     return OS_LOG_TYPE_ERROR;
    case LOG_WARNING: return OS_LOG_TYPE_DEFAULT;
    case LOG_NOTICE:  return OS_LOG_TYPE_INFO;
    case LOG_INFO:    return OS_LOG_TYPE_INFO;
    case LOG_DEBUG:   return OS_LOG_TYPE_DEBUG;
    }
    return OS_LOG_TYPE_DEFAULT;
}
#endif
#endif

WEAK std::string g_log_ident = "";
WEAK int g_log_option = LOG_ODELAY;
WEAK int g_log_facility = LOG_USER;

WEAK int g_log_level = LOG_WARNING;
WEAK int g_log_mask = LOG_UPTO(g_log_level);

#if defined(_WIN32) || defined(__ANDROID__) || defined(APPLE_USE_OSLOG)
#if defined(_WIN32)
WEAK SOCKET g_log_sock = INVALID_SOCKET;
#elif defined(APPLE_USE_OSLOG)
WEAK os_log_t g_os_log = OS_LOG_DEFAULT;
#endif

inline void openlog(const char* ident, int option, int facility)
{
    g_log_ident = ident == NULL ? "" : ident;
    g_log_option = option == 0 ? LOG_ODELAY : option;
    g_log_facility = facility == 0 ? LOG_USER : facility;

#if defined(_WIN32)
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        fprintf(stderr, "[log_sock]WSAStartup failed[%d:%s]\n", WSAGetLastError(), strerror(WSAGetLastError()));
        return;
    }
    g_log_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_log_sock == INVALID_SOCKET)
    {
        fprintf(stderr, "[log_sock]creation failed[%d:%s]\n", WSAGetLastError(), strerror(WSAGetLastError()));
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(514); // default syslog port
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(g_log_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        fprintf(stderr, "[log_sock]connect failed[%d:%s]\n", WSAGetLastError(), strerror(WSAGetLastError()));
        closesocket(g_log_sock);
        g_log_sock = INVALID_SOCKET;
        return;
    }

#elif defined(__ANDROID__) && __ANDROID_API__ >= 30
    __android_log_set_default_tag(g_log_ident.c_str());

#elif defined(APPLE_USE_OSLOG)
    g_os_log = os_log_create(ident, "default");

#endif
}

inline void closelog(void)
{
#if defined(_WIN32)
    if (g_log_sock != INVALID_SOCKET)
    {
        closesocket(g_log_sock);
        g_log_sock = INVALID_SOCKET;
    }
    WSACleanup();

#elif defined(APPLE_USE_OSLOG)
    os_release(g_os_log);

#endif
}

inline int setlogmask(int mask)
{
    int omask = g_log_mask;
    if (mask != 0)
    {
        g_log_mask = mask;
    }
    return omask;
}

inline void vsyslog(int pri, const char* fmt, va_list ap)
{
    if (!(LOG_MASK(LOG_PRI(pri)) & g_log_mask))
    {
        return;
    }

#if defined(_WIN32)
    thread_local char buffer[4096] = { 0 };
    thread_local size_t offset = 0;
    thread_local timespec curr_time_tp{};
    if (clock_gettime(CLOCK_REALTIME, &curr_time_tp) != -1)
    {
        std::string curr_time_string = timespec_helper::timespec_string(curr_time_tp);
        static const char* log_pre[]{ "EMERG   ","ALERT   ","CRITICAL","ERROR   ","WARNING ","NOTICE  ","INFO    ","DEBUG   " };
        offset = (size_t)snprintf(buffer, sizeof(buffer), "%s[%s][0x%08x]", (char*)curr_time_string.c_str(), log_pre[LOG_PRI(pri)], GetCurrentThreadId());
    }
    vsnprintf(buffer + offset, sizeof(buffer) - offset, fmt, ap);
    if (g_log_sock == INVALID_SOCKET)
    {
        fprintf(stderr, "%s", buffer);
    }
    else if (send(g_log_sock, buffer, (int)strlen(buffer), 0/*MSG_DONTWAIT*/) == SOCKET_ERROR)
    {
        fprintf(stderr, "[log_sock]send failed[%d:%s]%s\n", WSAGetLastError(), strerror(WSAGetLastError()), buffer);
    }

#elif defined(__ANDROID__)
    __android_log_vprint(to_android_log(pri), g_log_ident.c_str(), fmt, ap);

#elif defined(APPLE_USE_OSLOG)
    thread_local char buffer[4096] = { 0 };
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    os_log_with_type(g_os_log, to_os_log(pri), "%{public}s", buffer);

#endif
}

inline void syslog(int pri, const char* fmt, ...)
{
#if 0
#if defined(__ANDROID__)
    __android_log_print(to_android_log(pri), g_log_ident.c_str(), fmt, ##__VA_ARGS__);
#endif
#endif

    va_list ap;
    va_start(ap, fmt);
    vsyslog(pri, fmt, ap);
    va_end(ap);
}
#endif

#ifndef SYSLOG_NAMES
#define INTERNAL_NOPRI  0x10                /* the "no priority" priority */
/* mark "facility" */
#define INTERNAL_MARK   LOG_MAKEPRI(LOG_NFACILITIES << 3, 0)
//#define INTERNAL_MARK (LOG_NFACILITIES<<3)
    typedef struct _code {
        const char* c_name;
        int         c_val;
    } CODE;

    WEAK CODE prioritynames[] =
    {
      { "alert",   LOG_ALERT },
      { "crit",    LOG_CRIT },
      { "debug",   LOG_DEBUG },
      { "emerg",   LOG_EMERG },
      { "err",     LOG_ERR },
      { "error",   LOG_ERR },               /* DEPRECATED */
      { "info",    LOG_INFO },
      { "none",    INTERNAL_NOPRI },        /* INTERNAL */
      { "notice",  LOG_NOTICE },
      { "panic",   LOG_EMERG },             /* DEPRECATED */
      { "warn",    LOG_WARNING },           /* DEPRECATED */
      { "warning", LOG_WARNING },
      { NULL,      -1 }
    };

    WEAK CODE facilitynames[] =
    {
      { "auth",     LOG_AUTH },
      { "authpriv", LOG_AUTHPRIV },
      { "cron",     LOG_CRON },
      { "daemon",   LOG_DAEMON },
      { "ftp",      LOG_FTP },
#if defined(APPLE_USE_OSLOG)
      { "install",  LOG_INSTALL},
#endif
      { "kern",     LOG_KERN },
      { "lpr",      LOG_LPR },
      { "mail",     LOG_MAIL },
      { "mark",     INTERNAL_MARK },        /* INTERNAL */
#if defined(APPLE_USE_OSLOG)
      { "netinfo",  LOG_NETINFO, },
      { "ras",      LOG_RAS },
      { "remoteauth", LOG_REMOTEAUTH },
#endif
      { "news",     LOG_NEWS },
      { "security", LOG_AUTH },             /* DEPRECATED */
      { "syslog",   LOG_SYSLOG },
      { "user",     LOG_USER },
      { "uucp",     LOG_UUCP },
      { "local0",   LOG_LOCAL0 },
      { "local1",   LOG_LOCAL1 },
      { "local2",   LOG_LOCAL2 },
      { "local3",   LOG_LOCAL3 },
      { "local4",   LOG_LOCAL4 },
      { "local5",   LOG_LOCAL5 },
      { "local6",   LOG_LOCAL6 },
      { "local7",   LOG_LOCAL7 },
#if defined(APPLE_USE_OSLOG)
      { "launchd",  LOG_LAUNCHD },
#endif
      { NULL,       -1 }
    };
#endif

#if 1
//https://blog.csdn.net/qq_43279097/article/details/136078218
//https://blog.csdn.net/u013391094/article/details/127143727
//https://blog.csdn.net/q1003675852/article/details/134999871
/*重置Reset          */#define  RST "\x1B[0m"
/*  黑Black          */#define  BLK "\x1B[30m"
/*  红Red            */#define  RED "\x1B[31m"
/*  绿Green          */#define  GRN "\x1B[32m"
/*  黄Yellow         */#define  YLW "\x1B[33m"
/*  蓝Blue           */#define  BLU "\x1B[34m"
/*洋红Magenta        */#define  MAG "\x1B[35m"
/*  青Cyan           */#define  CYN "\x1B[36m"
/*  白White          */#define  WHT "\x1B[37m"
/*默认default        */#define  DEF "\x1B[39m"
/*Bright Black (Gray)*/#define BBLK "\x1B[90m"
/*Bright Red         */#define BRED "\x1B[91m"
/*Bright Green       */#define BGRN "\x1B[92m"
/*Bright Yellow      */#define BYLW "\x1B[93m"
/*Bright Blue        */#define BBLU "\x1B[94m"
/*Bright Magenta     */#define BMAG "\x1B[95m"
/*Bright Cyan        */#define BCYN "\x1B[96m"
/*Bright White       */#define BWHT "\x1B[97m"
#endif

WEAK bool g_enable_console_log = false;
WEAK bool g_enable_syslog = false;

inline void init_sys_log(const std::string& log_name, bool to_stderr = false);
inline void uninit_sys_log();
inline void change_log_level(int log_level);
inline void change_log_level_4_string(const std::string& log_level_str);
inline void plog_inner(int pri, const char* fmt, ...);
inline void vplog_inner(int pri, const char* fmt, va_list ap);

void init_sys_log(const std::string& log_name, bool to_stderr/* = false*/)
{
    int option = /*LOG_CONS | */LOG_PID;
    if (to_stderr)
    {
        option |= LOG_PERROR;
    }

    openlog(log_name.c_str(), option | LOG_NDELAY, LOG_USER/*LOG_DAEMON*/);
    change_log_level(g_log_level);
}

void uninit_sys_log()
{
    closelog();
}

void change_log_level(int log_level)
{
    g_log_level = log_level;
    if (log_level == INTERNAL_NOPRI)
    {
        (void)setlogmask(LOG_MASK(g_log_level));
    }
    else
    {
#if defined(__ANDROID__) && __ANDROID_API__ >= 30
        //int32_t old_priority = __android_log_get_minimum_priority();
        /*int32_t old_priority = */(void)__android_log_set_minimum_priority(to_android_log(log_level));
#endif
        (void)setlogmask(LOG_UPTO(g_log_level));
    }
}

void change_log_level_4_string(const std::string& log_level_str)
{
    for (size_t i = 0; i < (sizeof(prioritynames) / sizeof(prioritynames[0]) - 1); ++i)
    {
        if (prioritynames[i].c_name == log_level_str)
        {
            change_log_level(prioritynames[i].c_val);
            break;
        }
    }
}

#define plog(pri, fmt, ...) \
do { \
    if ((g_enable_syslog || g_enable_console_log) && LOG_PRI(g_log_level) && (pri) <= g_log_level) \
    { \
        plog_inner((pri), fmt, ##__VA_ARGS__); \
    } \
} while(0)

void plog_inner(int pri, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vplog_inner(pri, fmt, ap);
    va_end(ap);
}

void vplog_inner(int pri, const char* fmt, va_list ap)
{
    if (g_enable_syslog)
    {
        vsyslog(pri, fmt, ap);
    }
    else if (g_enable_console_log)
    {
#if defined(__ANDROID__)
        __android_log_vprint(to_android_log(pri), g_log_ident.c_str(), fmt, ap);

#elif defined(APPLE_USE_OSLOG)
        thread_local char buffer[4096] = { 0 };
        vsnprintf(buffer, sizeof(buffer), fmt, ap);
        os_log_with_type(g_os_log, to_os_log(pri), "%{public}s", buffer);

#else
        thread_local timespec curr_time_tp{};
        if (clock_gettime(CLOCK_REALTIME, &curr_time_tp) != -1)
        {
            //最紧急, 紧急, 重要, 出错, 警告, 普通但重要, 通知性, 调试
            static const char* log_pre[]{ "EMERG   ","ALERT   ","CRITICAL","ERROR   ","WARNING ","NOTICE  ","INFO    ","DEBUG   " };
            static const char* log_clr[]{ RED, RED, RED, RED, YLW, BLU, CYN, GRN };
            fprintf(stderr, "%s", log_clr[LOG_PRI(pri)]);

            std::string curr_time_string = timespec_helper::timespec_string(curr_time_tp);
#if 0
            std::stringstream ss;
            ss << std::hex << std::showbase << std::internal << std::this_thread::get_id();
            ss << std::noshowbase << std::dec << std::right;
            fprintf(stderr, "%s[%s][%s]", curr_time_string.c_str(), log_pre[LOG_PRI(pri)], ss.str().c_str());
            vfprintf(stderr, fmt, ap);
            ss.clear();
#else
#if defined(_WIN32)
#define k_current_thread_id GetCurrentThreadId()
#elif defined(__linux__) //gettid()
#define k_current_thread_id syscall(SYS_gettid)
#else
#define k_current_thread_id pthread_self()
#endif
            fprintf(stderr, "%s[%s][0x%08lx]", curr_time_string.c_str(), log_pre[LOG_PRI(pri)], k_current_thread_id);
            vfprintf(stderr, fmt, ap);
#endif
            fprintf(stderr, "%s", RST);
            fflush(stderr);
        }
#endif
    }
}

#define log(...) do { printf(##__VA_ARGS__); putchar('\n'); fflush(stdout); } while(0)
