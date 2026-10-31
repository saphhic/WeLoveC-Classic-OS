// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for platform in the ethernet service module.

#ifndef WELOVEC_ETHERNET_PLATFORM_H
#define WELOVEC_ETHERNET_PLATFORM_H

#include "arch/x86_64/io/console.h"

static inline void eth_platform_write(const char *text) {
    kernel_console_write(text);
}

static inline void eth_platform_putc(char character) {
    char text[2] = {character, '\0'};
    kernel_console_write(text);
}

#define printf eth_platform_write
#define uprint eth_platform_write
#define putchar eth_platform_putc

#endif
