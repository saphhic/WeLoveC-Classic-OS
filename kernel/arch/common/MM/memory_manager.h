// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for memory manager in the memory management module.

#ifndef WELOVEC_MEMORY_MANAGER_C_API_H
#define WELOVEC_MEMORY_MANAGER_C_API_H

#include "boot/boot_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

int welovec_memory_manager_initialize(const WelovecBootInfo *boot_info);

#ifdef __cplusplus
}
#endif

#endif
