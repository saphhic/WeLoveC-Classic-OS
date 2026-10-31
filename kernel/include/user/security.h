// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares user login, account, and privilege functions.

#ifndef WELOVEC_USER_SECURITY_H
#define WELOVEC_USER_SECURITY_H

#include <stdint.h>
#include "user/syscall.h"
#include "user/security_types.h"

#ifndef WELOVEC_SECURITY_CAPABILITIES
#define WELOVEC_SECURITY_CAPABILITIES
#define WELOVEC_UID_ROOT UINT32_C(0)
#define WELOVEC_UID_GUEST UINT32_C(1000)
#define WELOVEC_CAP_EXEC_AS_ADMIN (UINT64_C(1) << 0)
#define WELOVEC_CAP_MANAGE_USERS (UINT64_C(1) << 1)
#define WELOVEC_CAP_MANAGE_PROCESSES (UINT64_C(1) << 2)
#define WELOVEC_CAP_SYSTEM_CONFIG (UINT64_C(1) << 3)
#define WELOVEC_CAP_ALL (WELOVEC_CAP_EXEC_AS_ADMIN | WELOVEC_CAP_MANAGE_USERS | \
                         WELOVEC_CAP_MANAGE_PROCESSES | WELOVEC_CAP_SYSTEM_CONFIG)
#define WELOVEC_SECURITY_NAME_MAX 32
#endif

enum welovec_user_priority {
    WELOVEC_PRIORITY_HIGH = 0,
    WELOVEC_PRIORITY_NORMAL = 1,
    WELOVEC_PRIORITY_BACKGROUND = 2,
    WELOVEC_PRIORITY_LOW = 3
};

#define WELOVEC_EXEC_ERROR_BASE 100

#if defined(__GNUC__) || defined(__clang__)
#define WELOVEC_USER_SECURITY_FN static inline __attribute__((unused))
#else
#define WELOVEC_USER_SECURITY_FN static inline
#endif

WELOVEC_USER_SECURITY_FN int64_t welovec_getuid(void) {
    return welovec_user_syscall0(WELOVEC_SYS_GETUID);
}

WELOVEC_USER_SECURITY_FN int64_t welovec_getgid(void) {
    return welovec_user_syscall0(WELOVEC_SYS_GETGID);
}

WELOVEC_USER_SECURITY_FN int64_t welovec_get_capabilities(void) {
    return welovec_user_syscall0(WELOVEC_SYS_GETCAPS);
}

WELOVEC_USER_SECURITY_FN int welovec_has_capability(uint64_t capability) {
    return welovec_user_syscall1(WELOVEC_SYS_HAS_CAPABILITY, capability) == 1;
}

WELOVEC_USER_SECURITY_FN int welovec_is_admin(void) {
    return welovec_has_capability(WELOVEC_CAP_EXEC_AS_ADMIN);
}

WELOVEC_USER_SECURITY_FN int64_t welovec_login(const char *username,
                                                const char *password) {
    return welovec_user_syscall2(WELOVEC_SYS_LOGIN,
        (uint64_t)(uintptr_t)username, (uint64_t)(uintptr_t)password);
}

WELOVEC_USER_SECURITY_FN int64_t welovec_logout(void) {
    return welovec_user_syscall0(WELOVEC_SYS_LOGOUT);
}

WELOVEC_USER_SECURITY_FN int welovec_get_user_status(
    struct welovec_user_status *status) {
    return (int)welovec_user_syscall1(WELOVEC_SYS_USER_STATUS,
                                      (uint64_t)(uintptr_t)status);
}

WELOVEC_USER_SECURITY_FN int welovec_check_logged_in(void) {
    struct welovec_user_status status;
    return welovec_get_user_status(&status) == 0 && status.authenticated != 0;
}

/* Returns the authenticated user's capability mask, or zero while logged out. */
WELOVEC_USER_SECURITY_FN uint64_t welovec_user_privilege_status(void) {
    struct welovec_user_status status;
    if (welovec_get_user_status(&status) < 0 || !status.authenticated) return 0;
    return status.capabilities;
}

WELOVEC_USER_SECURITY_FN int64_t welovec_create_user(
    const char *username, const char *password, uint32_t flags) {
    return welovec_user_syscall3(WELOVEC_SYS_USER_CREATE,
        (uint64_t)(uintptr_t)username, (uint64_t)(uintptr_t)password, flags);
}

WELOVEC_USER_SECURITY_FN int64_t welovec_list_users(
    struct welovec_user_record *users, uint64_t capacity) {
    return welovec_user_syscall2(WELOVEC_SYS_USER_LIST,
        (uint64_t)(uintptr_t)users, capacity);
}

/* Fills output with cryptographic random bytes; fails if kernel entropy is unavailable. */
WELOVEC_USER_SECURITY_FN int64_t welovec_secure_random(void *output, uint64_t size) {
    return welovec_user_syscall2(WELOVEC_SYS_SECURE_RANDOM,
        (uint64_t)(uintptr_t)output, size);
}

/* Returns a process ID, -errno, or -100-loader_error. Inherits this user's identity. */
WELOVEC_USER_SECURITY_FN int64_t welovec_exec(
    const char *path, uint32_t argc, const char *const argv[], uint8_t priority) {
    return welovec_user_syscall4(
        WELOVEC_SYS_EXEC, (uint64_t)(uintptr_t)path,
        (uint64_t)(uintptr_t)argv, argc, priority);
}

/* Starts a child with a private framebuffer and input queue sized for a window. */
WELOVEC_USER_SECURITY_FN int64_t welovec_exec_windowed(
    const char *path, uint32_t argc, const char *const argv[], uint8_t priority,
    uint32_t framebuffer_width, uint32_t framebuffer_height) {
    return welovec_user_syscall6(
        WELOVEC_SYS_EXEC_WINDOWED, (uint64_t)(uintptr_t)path,
        (uint64_t)(uintptr_t)argv, argc, priority,
        framebuffer_width, framebuffer_height);
}

/* Starts the requested ELF with administrator credentials; requires a logged-in administrator. */
WELOVEC_USER_SECURITY_FN int64_t welovec_exec_admin(
    const char *path, uint32_t argc, const char *const argv[], uint8_t priority) {
    return welovec_user_syscall4(
        WELOVEC_SYS_EXEC_ADMIN, (uint64_t)(uintptr_t)path,
        (uint64_t)(uintptr_t)argv, argc, priority);
}

/* Starts the requested ELF as an existing account; requires MANAGE_USERS. */
WELOVEC_USER_SECURITY_FN int64_t welovec_exec_user(
    const char *path, uint32_t uid, uint32_t argc,
    const char *const argv[], uint8_t priority) {
    return welovec_user_syscall5(
        WELOVEC_SYS_EXEC_AS_USER, (uint64_t)(uintptr_t)path,
        (uint64_t)(uintptr_t)argv, argc, priority, uid);
}

#endif

#undef WELOVEC_USER_SECURITY_FN
