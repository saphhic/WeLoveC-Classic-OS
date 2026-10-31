// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for privileges in the security module.

#ifndef WELOVEC_SECURITY_PRIVILEGES_H
#define WELOVEC_SECURITY_PRIVILEGES_H

#include <stdint.h>
#include "user/security_types.h"

#ifndef WELOVEC_SECURITY_CAPABILITIES
#define WELOVEC_SECURITY_CAPABILITIES
#define WELOVEC_UID_ROOT UINT32_C(0)
#define WELOVEC_UID_GUEST UINT32_C(1000)

#define WELOVEC_CAP_EXEC_AS_ADMIN  (UINT64_C(1) << 0)
#define WELOVEC_CAP_MANAGE_USERS   (UINT64_C(1) << 1)
#define WELOVEC_CAP_MANAGE_PROCESSES (UINT64_C(1) << 2)
#define WELOVEC_CAP_SYSTEM_CONFIG  (UINT64_C(1) << 3)
#define WELOVEC_CAP_ALL (WELOVEC_CAP_EXEC_AS_ADMIN | WELOVEC_CAP_MANAGE_USERS | \
                         WELOVEC_CAP_MANAGE_PROCESSES | WELOVEC_CAP_SYSTEM_CONFIG)

#define WELOVEC_SECURITY_NAME_MAX 32
#endif

struct welovec_security_identity {
    uint32_t uid;
    uint32_t gid;
    uint64_t capabilities;
};

struct welovec_security_user {
    uint32_t uid;
    uint32_t gid;
    uint64_t capabilities;
    char name[WELOVEC_SECURITY_NAME_MAX];
};

#ifdef __cplusplus
extern "C" {
#endif

struct WelovecBootInfo;

/* Kernel startup calls this after the VFS has been mounted. */
int welovec_security_initialize(const struct WelovecBootInfo *boot_info);
/* A trusted kernel setup screen uses these before starting user processes. */
int welovec_security_setup_required(void);
int64_t welovec_security_create_initial_admin(const char *name, const char *password);

#ifdef __cplusplus
}
#endif

#endif
