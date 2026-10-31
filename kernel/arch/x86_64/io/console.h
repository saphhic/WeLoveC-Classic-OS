// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for console in the io module.

#ifndef WELOVEC_X86_64_CONSOLE_H
#define WELOVEC_X86_64_CONSOLE_H

#include "boot/boot_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

void kernel_console_initialize(const WelovecBootInfo *boot_info);
void kernel_console_write(const char *message);

#ifdef __cplusplus
}
#endif

#endif
