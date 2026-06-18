#pragma once
#ifdef  _WIN32
#else
#include <pthread.h>
#include <sched.h>
#endif
#include <cstring>
#include "sys_log.h"

inline void thread_attr_set()
{
    do
    {
        int err_no = 0;
        pthread_t tid = pthread_self();

        pthread_attr_t attr;
        memset(&attr, 0, sizeof(attr));

        if ((err_no = pthread_attr_init(&attr)) != 0)
        {
            plog(LOG_WARNING, "[%d]pthread_attr_init failed[%d:%s]\n", tid, err_no, strerror(err_no));
        }

        if ((err_no = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED)) != 0)
        {
            plog(LOG_WARNING, "[%d]pthread_attr_setinheritsched PTHREAD_EXPLICIT_SCHED failed[%d:%s]\n", tid, err_no, strerror(err_no));
        }

        if ((err_no = pthread_attr_setschedpolicy(&attr, SCHED_RR)) != 0)
        {
            plog(LOG_WARNING, "[%d]pthread_attr_setschedpolicy SCHED_RR failed[%d:%s]\n", tid, err_no, strerror(err_no));
        }

        //int sched_get_priority_max(int policy);
        //int sched_get_priority_min(int policy);
#if 0
        sched_param param;
        memset(&param, 0, sizeof(param));
        if ((err_no = pthread_attr_getschedparam(&attr, &param)) != 0)
        {
            plog(LOG_WARNING, "[%d]pthread_attr_getschedparam failed[%d:%s]\n", err_no, strerror(err_no));
        }
#endif

        param.sched_priority = 80;
        if ((err_no = pthread_attr_setschedparam(&attr, &param)) != 0)
        {
            plog(LOG_WARNING, "[%d]pthread_attr_setschedparam failed[%d][%d:%s]\n", tid, param.sched_priority, err_no, strerror(err_no));
        }

        //pthread_t tid = t.native_handle();
        if ((err_no = pthread_setschedparam(tid, SCHED_RR, &param)) != 0)
        {
            plog(LOG_WARNING, "[%d]pthread_setschedparam SCHED_RR failed[%d:%s]\n", tid, err_no, strerror(err_no));
        }

        //if ((err_no = pthread_setschedprio(tid, 80)) != 0)
        //{
        //    plog(LOG_WARNING, "[%d]pthread_setschedprio failed[%d:%s]\n", tid, err_no, strerror(err_no));
        //}

        if ((err_no = pthread_attr_destroy(&attr)) != 0)
        {
            plog(LOG_WARNING, "[%d]pthread_attr_destroy failed[%d:%s]\n", tid, err_no, strerror(err_no));
        }

    } while (0);
}
