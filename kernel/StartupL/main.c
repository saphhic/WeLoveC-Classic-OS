// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Is the entry point for the startup component.

#include "../arch/x86_64/cpu/cpu.h"
#include "../arch/x86_64/io/console.h"
#include "../arch/x86_64/power/power.h"
#include "../include/boot/boot_protocol.h"
#include "../arch/common/MM/memory_manager.h"
#include "../arch/common/Process/manager.h"
#include "../arch/x86_64/scheduler/scheduler.h"
#include "../IDM/DisplayService/framebuffer.hpp"
#include "../arch/common/Time/wait_func.h"
#include "startup.h"

void kernel_main(const WelovecBootInfo *boot_info) {
    welovec_power_initialize(boot_info);
    kernel_console_initialize(boot_info);
    
    printf_fb_color("WeLoveC kernel v1.3.1 Classic.\n\n", FB_YELLOW);
    kernel_console_write("The system is provided AS IS without warranty of any kind.\n");
    kernel_console_write("If you encounter any problems visit https://wlc.lawnetworking.org for more info\n");
    wait_seconds(3);
    fb_clear_screen();
    printf_fb_color("#kernel/StartupL/main.c\n\n", FB_CYAN);

    kernel_console_write("Initializing memory manager.\n");
    int resl1 = welovec_memory_manager_initialize(boot_info);
    if (resl1 == 1) { printf_fb_color(" [OK]\n", FB_GREEN); } else { printf_fb_color(" [ERROR] System halting.\n", FB_RED); for (;;) { hlt(); }}

    kernel_console_write("Initializing scheduler.");
    int resl2 = scheduler_initialize();
    if (resl2 == 1) { printf_fb_color(" [OK]\n", FB_GREEN); } else { printf_fb_color(" [ERROR] System halting.\n", FB_RED); for (;;) { hlt(); }}

    kernel_console_write("Initializing process manager.");
    int resl3 = welovec_process_manager_initialize();
    if (resl3 == 1) { printf_fb_color(" [OK]\n", FB_GREEN); } else { printf_fb_color(" [ERROR] System halting.\n", FB_RED); for (;;) { hlt(); }}

    kernel_console_write("Starting scheduler.");
    scheduler_start();
    printf_fb_color("Exiting main.c ...\n\n", FB_LIME);
    
    startup_main(boot_info);

    for (;;) hlt();
}
