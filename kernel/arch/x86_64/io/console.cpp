// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the kernel early text console.

#include "console.h"
#include "serial.h"
#include "IDM/DisplayService/framebuffer.hpp"

extern "C" void kernel_console_initialize(const WelovecBootInfo *boot_info) {
    serial_init();
    if (framebuffer_init(boot_info))
        serial_write("Console: UEFI GOP framebuffer active; COM1 mirror enabled.\n");
    else
        serial_write("Console: GOP framebuffer unavailable; using COM1.\n");
}

extern "C" void kernel_console_write(const char *message) {
    if (!message) return;
    serial_write(message);
    if (framebuffer_available()) printf_fb(message);
}
