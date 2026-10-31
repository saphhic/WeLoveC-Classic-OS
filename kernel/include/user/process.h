// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares user APIs for creating, inspecting, and waiting for processes.

#ifndef WELOVEC_USER_PROCESS_H
#define WELOVEC_USER_PROCESS_H

#include <stdint.h>
#include "user/syscall.h"

enum welovec_user_process_state {
    WELOVEC_USER_PROCESS_ACTIVE = 0,
    WELOVEC_USER_PROCESS_TERMINATING = 1,
    WELOVEC_USER_PROCESS_ZOMBIE = 2
};

struct welovec_user_process_info {
    uint64_t id;
    uint64_t parent_id;
    uint64_t live_threads;
    int64_t exit_status;
    uint32_t state;
    uint32_t reserved;
    uint32_t uid;
    uint32_t gid;
    uint64_t capabilities;
    char name[32];
};

#define WELOVEC_WAIT_NOHANG UINT32_C(1)
#define WELOVEC_SIGNAL_INTERRUPT UINT32_C(2)
#define WELOVEC_SIGNAL_KILL UINT32_C(9)
#define WELOVEC_SIGNAL_TERMINATE UINT32_C(15)

typedef void (*welovec_signal_handler)(int signal_number);
#define WELOVEC_SIG_DFL ((welovec_signal_handler)0)
#define WELOVEC_SIG_IGN ((welovec_signal_handler)(uintptr_t)1)

static inline int64_t welovec_process_query(
    uint64_t pid, struct welovec_user_process_info *out_info) {
    return welovec_user_syscall2(WELOVEC_SYS_PROCESS_QUERY, pid,
                                 (uint64_t)(uintptr_t)out_info);
}

static inline int64_t welovec_sigaction(uint32_t signal_number,
                                        welovec_signal_handler handler,
                                        welovec_signal_handler *old_handler) {
    return welovec_user_syscall3(WELOVEC_SYS_SIGACTION, signal_number,
                                 (uint64_t)(uintptr_t)handler,
                                 (uint64_t)(uintptr_t)old_handler);
}

static inline int64_t welovec_terminate_windowed_tree(uint64_t pid,
                                                       int64_t status) {
    return welovec_user_syscall2(WELOVEC_SYS_TERMINATE_WINDOWED_TREE, pid,
                                 (uint64_t)status);
}

#endif
