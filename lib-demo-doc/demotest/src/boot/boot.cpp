#include "configreader.h"
#include "oam.h"
#include "init.h"
#include "macrodefine.h"
#include "goodtp.h"
#include "cos.h"

#ifdef __linux__
#include <sys/stat.h>
#include <sys/types.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>

#define MAX_STACK_SIZE   (512)

using namespace std;

#ifdef __cplusplus
extern "C" {
#endif

volatile u32 g_nProcRunningFlag = (u32)COS_TRUE;

#ifdef __linux__
const char * getProcessName(const char * argv0) {
    const char * p = strrchr(argv0, '/') + 1;
    return p;
}

void SetWorkDir(const char *pPath) {
    cos_asserta(NULL == pPath);

    char buffer[512] = {0};

    snprintf(&(buffer[0]), sizeof(buffer), "%s", pPath);

    int i = strlen(buffer);

    for (; 0 < i; i--) {
        if (buffer[i] == '/') {
            buffer[i] = '\0';
            break;
        }
    }

    if (0 >= i) {
        return;
    }

    if (0 != chdir(buffer)) {
        printf("set dir failed(errorcode= %d(%s)).\r\n", errno, strerror(errno));
    }

    return;
}

void InitDaemon(void) {
    int pid;

    if ((pid = fork())) {
        exit(0);
    } else if (pid < 0) {
        exit(1);
    }

    setsid();

    if ((pid = fork())) {
        exit(0);
    } else if (pid < 0) {
        exit(1);
    }

    for (int i = 0; 3 > i; ++i) {
        close(i);
    }

    umask(0);

    return;
}

void SignalHandlerCallBack(int nSignal) {
    g_nProcRunningFlag = (u32)COS_FALSE;

    u8 signal_name[256] = {0};

    void  *stack_buffer[MAX_STACK_SIZE] = {NULL};
    char **stack_trace                  = NULL;

    int nSize   = backtrace(stack_buffer, MAX_STACK_SIZE);
    stack_trace = backtrace_symbols(stack_buffer, nSize);

    switch (nSignal) {
        case SIGTERM:
            strcpy((char *)signal_name, "SIGTERM(start-stop-daemo --stop)"); // NOLINT
            break;

        case SIGINT:
            strcpy((char *)signal_name, "SIGINT(CTRL+C)"); // NOLINT
            break;

        case SIGTSTP:
            strcpy((char *)signal_name, "SIGTSTP(CTRL+Z)"); // NOLINT
            break;

        case SIGSEGV:
            strcpy((char *)signal_name, "SIGSEGV(segment fault)"); // NOLINT
            break;

        case SIGBUS:
            strcpy((char *)signal_name, "SIGBUS(ram bus error)"); // NOLINT
            break;

        case SIGABRT:
            strcpy((char *)signal_name, "SIGABRT(abort process)"); // NOLINT
            break;

        case SIGILL:
            strcpy((char *)signal_name, "SIGILL(stack overflows)"); // NOLINT
            break;

        case SIGFPE:
            strcpy((char *)signal_name, "SIGFPE(error instruction)"); // NOLINT
            break;

        case SIGSYS:
            strcpy((char *)signal_name, "SIGSYS(invlaid system API)"); // NOLINT
            break;

        case SIGTTIN:
            strcpy((char *)signal_name, "SIGTTIN(daemon process read front desk)"); // NOLINT
            break;

        case SIGTTOU:
            strcpy((char *)signal_name, "SIGTTOU(daemon process write front desk)"); // NOLINT
            break;

        case SIGPIPE:
            strcpy((char *)signal_name, "SIGPIPE(tcp socket RST error)"); // NOLINT
            break;

        case SIGHUP:
            strcpy((char *)signal_name, "SIGHUP(The terminal started process exited)"); // NOLINT
            break;

        case SIGQUIT:
            strcpy((char *)signal_name, "SIGQUIT(CTRL+break)"); // NOLINT
            break;

        case SIGALRM:
            strcpy((char *)signal_name, "SIGALRM(alarm(2)"); // NOLINT
            break;

        case SIGTRAP:
            strcpy((char *)signal_name, "SIGTRAP(break point)"); // NOLINT
            break;

        case SIGUSR1:
            strcpy((char *)signal_name, "SIGUSR1(fork() process exited)"); // NOLINT
            break;

        case SIGUSR2:
            strcpy((char *)signal_name, "SIGUSR2(fork() process exited)"); // NOLINT
            break;

        case SIGSTKFLT:
            strcpy((char *)signal_name, "SIGSTKFLT(Math coprocessor abnormal)"); // NOLINT
            break;

        case SIGPWR:
            strcpy((char *)signal_name, "SIGPWR(power abnormal)"); // NOLINT
            break;

        case SIGURG:
            strcpy((char *)signal_name, "SIGURG(Out of band data for sockets)"); // NOLINT
            break;

        default:
            strcpy((char *)signal_name, "unknown"); // NOLINT
            break;
    }

    COS_LOG(moduleIdEnum::kModuleBootId, cos_log_level_error,
            "receiced terminate the process signal: %d(%s)!\r\n",
            nSignal, (char *)signal_name);

    cos_log("*********************Dump stack start**************************\r\n");

    for (u32 nLoop = 0 ; ((u32)nSize) > nLoop ; ++nLoop) {
        cos_log("%s\r\n", stack_trace[nLoop]);
    }

    cos_log("*********************Dump stack end**************************\r\n");

    free(stack_trace);
    stack_trace = NULL;

    cos_setPrintLogLevel(cos_log_level_info);

    COS_LOG(moduleIdEnum::kModuleBootId, cos_log_level_info,
            "now exit c3core process.\r\n");

    cos_threadSleep(100);

    RemoveGtpcore();

    cos_removePlatform();

    signal(nSignal, SIG_DFL);

    return;
}

void DoHookSignalHandler(void) {
    u32 nLoop      = 0;
    i32 aSignals[] = {
        SIGTERM,
        SIGINT,
        SIGTSTP,
        SIGSEGV,
        SIGBUS,
        SIGABRT,
        SIGILL,
        SIGFPE,
        SIGSYS,
        SIGTTIN,
        SIGTTOU,
        SIGHUP,
        SIGQUIT,
        SIGALRM,
        SIGTRAP,
        SIGUSR1,
        SIGUSR2,
        SIGSTKFLT,
        SIGPWR,
        SIGURG,
        SIGTTIN,
        SIGTTOU
    };

    for (nLoop = 0; (sizeof(aSignals) / sizeof(i32)) > nLoop; ++nLoop) {
        signal(aSignals[nLoop], SignalHandlerCallBack);
    }

    struct sigaction action;

    action.sa_handler = SignalHandlerCallBack;
    action.sa_flags   = 0;

    sigemptyset(&action.sa_mask);
    sigaction(SIGPIPE, &action, NULL);

    return;
}
#endif

u32 g_counter = 0;
void stack_test() {
    if (10 < g_counter) {
        return;
    }

    g_counter += 1;

    u8 mem[1024] = {0};
    for (u32 i = 0; 1024 > i; i += 1) {
        mem[i] = (i % 256);
    }

    u32 stack_size      = 0;
    i64 stack_free_size = GtpGetStackFreeSize(&stack_size);

    printf("stack_free_size=%lld stack_size=%u g_counter=%u\n",
           (long long)stack_free_size, stack_size, g_counter);

    stack_test();

    for (u32 i = 0; 1024 > i; i += 1) {
        if ((i % 256) != mem[i]) {
            printf("abnormal(i=%u mem[%u]=%u)\r\n", i, i, (u32)mem[i]);
        }
    }

    return;
}

int main(int argc, char *argv[]) {
    #ifdef __linux__
    // setting current folder path as startup binary path.
    SetWorkDir(argv[0]);

    DoHookSignalHandler();

    u32 daemon_flag         = COS_FALSE;
    u32 system_id           = 0;  // 1:ENODE(Edge node), 2:ROUTER(Center node)
    u32 roler_id            = 0;  // rolerEnum
    u32 necessary_param_num = 0;  // Counter for must parameters

    // Get startup parameters:-d(daemon process) -s sysId(system id)
    char boot_param = 0;
    while (-1 != (boot_param = getopt(argc, argv, "ds:t:"))) {
        switch (boot_param) {
            case 'd': {
                daemon_flag = COS_TRUE;
                break;
            }

            case 's': {
                system_id = (u32)atoi(optarg);
                necessary_param_num += 1;
                break;
            }

            case 't': {
                roler_id = (u32)atoi(optarg);
                necessary_param_num += 1;
                break;
            }
            default:
                break;
        }
    }

    if (2 != necessary_param_num) {
        printf(" missing required startup parameters, pls startup agin...\r\n");
        printf(" the startup format is c3core [-d] -s sysId -t type\r\n");
        printf(" -d: it's optional parameters, means running as daemon mode.\r\n");
        printf(" -s sysId: it's required parameters, edge node is 1, center node is 2.\r\n");
        printf(" -t type : it's required parameters, client is 1, server is 2.\r\n");
        fflush(stdout);

        return COS_ERR;
    }

    if (COS_TRUE == daemon_flag) {
        InitDaemon();
    }

    u32 runing_result      = COS_OK;
    u8  log_path[MAX_PATH] = {0};

    snprintf((char*)log_path, MAX_PATH, "/run/%s.log", getProcessName(argv[0])); // NOLINT

    runing_result = cos_loadPlatform(system_id, MAX_LOG_SIZE, &(log_path[0]));
    #endif

    #if (_WIN32 || _WIN64)
    u32 roler_id      = 1;  // rolerEnum
    u32 runing_result = cos_loadPlatform(1, MAX_LOG_SIZE, (u16*)L"..\\log\\goodtptest.log");
    #endif

    if (COS_OK != runing_result) {
        printf("call cos_loadPlatform() failed(0x%08x).\r\n", runing_result);
        fflush(stdout);

        return COS_ERR;
    }

    {
    cos_date_time_stru cur_data;
    cos_getCurSysTime(&cur_data);
    cos_setRandSeed((u32)((cur_data.timestamp_us) & 0x00000000FFFFFFFF));
    }

    cos_setPrintLogLevel(cos_log_level_info);

    COS_LOG(moduleIdEnum::kModuleBootId, cos_log_level_info,
            "loaded cos platform successfully!\r\n");

    ConfigReader configure_reader;

    #ifdef __linux__
    configure_reader.ReadConfigFile("../cfg/config.json");
    #endif

    #if (_WIN32 || _WIN64)
    configure_reader.ReadConfigFile("..\\cfg\\config.json");
    #endif

    const string &user_name = configure_reader.GetCliUserName();
    const string &listen_ip = configure_reader.GetCliListenIp();
    u32 listen_port         = configure_reader.GetCliListenPort();

    COS_LOG(moduleIdEnum::kModuleBootId, cos_log_level_info,
            "cli's parameter:%s %s %u.\r\n", user_name.c_str(),
            listen_ip.c_str(), (const int)listen_port);

    runing_result = cos_startCli((void*)(user_name.c_str()), // NOLINT
                                 (u32)(user_name.length()),
                                 (void*)(listen_ip.c_str()), listen_port); // NOLINT
    if (COS_OK == runing_result) {
        COS_LOG(moduleIdEnum::kModuleBootId, cos_log_level_info,
                "started cli's service sucessfully.\r\n");
    } else {
        COS_LOG(moduleIdEnum::kModuleBootId, cos_log_level_error,
                "call cos_startCli() failed(0x%08x)\r\n", runing_result);
    }

    runing_result = InitGtpcore((rolerEnum)roler_id);
    if (COS_OK != runing_result) {
        COS_LOG(moduleIdEnum::kModuleBootId, cos_log_level_error,
                "call InitGtpcore() failed(0x%08x).\r\n", runing_result);

        cos_removePlatform();

        return COS_ERR;
    }

    u8 cos_version[128] = {0};
    u8 app_version[128] = {0};

    cos_getVersion(&(cos_version[0]));  // The cos platform version.
    GetAppVersion(&(app_version[0]));   // The applicationo version.

    COS_LOG(moduleIdEnum::kModuleBootId, cos_log_level_info,
            "c3core version: %s\r\n", (char*)app_version);
    COS_LOG(moduleIdEnum::kModuleBootId, cos_log_level_info,
            "cos version   : %s\r\n", (char*)cos_version);

    COS_LOG(moduleIdEnum::kModuleBootId, cos_log_level_info,
            "initialized the c3core's application sucessfuly!\r\n");

    cos_setPrintLogLevel(configure_reader.GetCurLogLevel());

    while (((u32)COS_TRUE) == g_nProcRunningFlag) {
        // It can't use conditinal variabe to wait, because of maybe maintain public variable by main thread.
        cos_threadSleep(100);
    }
    return COS_OK;
}

#ifdef __cplusplus
}
#endif

