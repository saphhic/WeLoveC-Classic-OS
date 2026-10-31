// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for power in the power module.

#ifndef WELOVEC_X86_64_POWER_H
#define WELOVEC_X86_64_POWER_H

#include <stdint.h>

struct WelovecBootInfo;

#ifdef __cplusplus
extern "C" {
#endif

void welovec_power_initialize(const WelovecBootInfo *boot_info);
int welovec_system_reboot(int virtual_machine);
int welovec_system_shutdown(int virtual_machine);

#ifdef __cplusplus
}
#endif

#endif
