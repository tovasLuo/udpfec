#pragma once
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#include <unistd.h>

class root_authority
{
public:
    root_authority();
    ~root_authority();

private:
    uid_t uid_;
};

inline root_authority::root_authority()
    : uid_(getuid())
{
    if (setuid(0) == -1)
    {
        printf("setuid failed[0][%d:%s]\n", errno, strerror(errno));
    }
}

inline root_authority::~root_authority()
{
    if (setuid(uid_) == -1)
    {
        printf("setuid failed[%d][%d:%s]\n", uid_, errno, strerror(errno));
    }
}
