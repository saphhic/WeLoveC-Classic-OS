// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares reboot and shutdown requests for user programs.

#ifndef WELOVEC_USER_POWER_H
#define WELOVEC_USER_POWER_H

#include <stdint.h>
#include "user/syscall.h"

static inline int64_t welovec_system_reboot(uint32_t virtual_machine) {
    return welovec_user_syscall1(WELOVEC_SYS_SYSTEM_REBOOT, virtual_machine);
}

static inline int64_t welovec_system_shutdown(uint32_t virtual_machine) {
    return welovec_user_syscall1(WELOVEC_SYS_SYSTEM_SHUTDOWN, virtual_machine);
}

/* CPUID hypervisor-present bit; a clear bit means use the hardware path. */
static inline uint32_t welovec_running_in_virtual_machine(void) {
    uint32_t eax = 1, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "c"(0));
    return (ecx >> 31) & 1u;
}

#endif
