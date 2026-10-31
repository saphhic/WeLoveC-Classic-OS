// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Defines types and capabilities used by the security API.

#ifndef WELOVEC_USER_SECURITY_TYPES_H
#define WELOVEC_USER_SECURITY_TYPES_H

#include <stdint.h>

#define WELOVEC_USER_NAME_MAX 32u
#define WELOVEC_USER_PASSWORD_MIN 7u
#define WELOVEC_USER_PASSWORD_MAX 128u
#define WELOVEC_USER_MAX_ACCOUNTS 32u
#define WELOVEC_USER_CREATE_ADMIN 1u

struct welovec_user_record {
    uint32_t uid;
    uint32_t gid;
    uint64_t capabilities;
    char name[WELOVEC_USER_NAME_MAX];
};

struct welovec_user_status {
    uint32_t uid;
    uint32_t gid;
    uint32_t authenticated;
    uint32_t reserved;
    uint64_t capabilities;
    char name[WELOVEC_USER_NAME_MAX];
};

#endif
