// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for paging in the paging module.

#ifndef WELOVEC_X86_64_PAGING_C_API_H
#define WELOVEC_X86_64_PAGING_C_API_H

#include "boot/boot_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

int welovec_paging_initialize(const WelovecBootInfo *boot_info);

#ifdef __cplusplus
}
#endif

#endif
