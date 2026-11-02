// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Initializes security and the initial account setup during kernel startup.

#include "../arch/x86_64/io/console.h"
#include "../IDM/DisplayService/framebuffer.hpp"
#include "../IDM/DisplayService/gpu.hpp"
#include "../filesystem/vfs.h"
#include "../arch/x86_64/input/keyboard.h"
#include "../arch/x86_64/input/mouse.h"
#include "../IDM/EthernetService/service.h"
#include "security/privileges.h"
#include "../arch/common/Time/wait_func.h"
#include "../security/createuser.h"
#include "startup.h"
#include "startupd.h"
#include "../arch/x86_64/io/serial.h"

extern const char *welovec_xhci_probe_status(void);

static int xhci_probe_pending(void) {
    const char *status = welovec_xhci_probe_status();
    return status[0] == 'p' && status[1] == 'r' && status[2] == 'o' &&
           status[3] == 'b' && status[4] == 'e' && status[5] == ' ';
}

void startup_main(const WelovecBootInfo *boot_info) {
    printf_fb_color("#kernel/StartupL/startup.c\n\n", FB_CYAN);

    kernel_console_write("Initializing GPU driver framework and probing PCI.");
    const WelovecGpuStatus gpu_status = welovec_gpu_initialize();
    if (gpu_status != WELOVEC_GPU_OK) {
        printf_fb_color(" [PROBE FRAMEWORK ERROR; UEFI DISPLAY LEFT ACTIVE]\n",
                        FB_ORANGE);
    } else {
        WelovecGpuSystemInfo gpu_system = {0};
        if (welovec_gpu_get_system_info(&gpu_system) != WELOVEC_GPU_OK) {
            printf_fb_color(" [GPU STATUS UNAVAILABLE; UEFI DISPLAY LEFT ACTIVE]\n",
                            FB_ORANGE);
        } else if (gpu_system.device_count == 0) {
            printf_fb_color(" [NO REGISTERED GPU MATCH; UEFI GOP LEFT ACTIVE]\n",
                            FB_ORANGE);
        } else {
            WelovecGpuDeviceInfo gpu_device = {0};
            if (welovec_gpu_get_device_info(0, &gpu_device) != WELOVEC_GPU_OK) {
                printf_fb_color(" [DEVICE INFO UNAVAILABLE; UEFI GOP LEFT ACTIVE]\n",
                                FB_ORANGE);
            } else if (gpu_device.vendor_id == 0x1002 &&
                       gpu_device.device_id == 0x1681) {
                printf_fb_color(" [AMD REMBRANDT MATCHED; PASSIVE PCI PROBE; NATIVE INIT PENDING]\n",
                                FB_GREEN);
            } else {
                printf_fb_color(" [GPU DRIVER MATCHED; NATIVE INIT PENDING]\n",
                                FB_GREEN);
            }
        }
    }

    kernel_console_write("Initializing filesystem.");
    if (welovec_filesystem_initialize(boot_info) == 1) {
        printf_fb_color(" [OK]\n", FB_GREEN);
    } else {
        printf_fb_color(" [ERROR] Filesystem unavailable; stopping startup.\n", FB_RED);
        return;
    }

    kernel_console_write("Initializing user and privilege manager.");
    if (welovec_security_initialize(boot_info) == 1) {
        printf_fb_color(" [OK]\n", FB_GREEN);
    } else {
        printf_fb_color(" [ERROR] Security database unavailable or corrupt; stopping startup.\n",
                        FB_RED);
        return;
    }

    kernel_console_write("Initializing keyboard input drivers.");
    int keyboard_ready = welovec_keyboard_initialize(boot_info) == 1;
    if (keyboard_ready) {
        const uint64_t keyboard_sources = welovec_keyboard_sources();
        printf_fb_color(" [", FB_GREEN);
        if (keyboard_sources & WELOVEC_KEYBOARD_SOURCE_PS2)
            printf_fb_color("PS/2 ", FB_GREEN);
        if (keyboard_sources & WELOVEC_KEYBOARD_SOURCE_USB_HID)
            printf_fb_color("USB HID ", FB_GREEN);
        if (keyboard_sources & WELOVEC_KEYBOARD_SOURCE_VIRTIO)
            printf_fb_color("virtio-input ", FB_GREEN);
        printf_fb_color("READY]\n", FB_GREEN);
    } else if (xhci_probe_pending()) {
        printf_fb_color(" [USB PROBE PENDING]\n", FB_ORANGE);
    } else {
        printf_fb_color(" [NO KEYBOARD DRIVER FOUND]\n", FB_RED);
    }

    const uint32_t pointer_sources = welovec_pointer_sources();
    if (pointer_sources) {
        printf_fb_color("Pointing devices detected: ", FB_GREEN);
        if (pointer_sources & WELOVEC_POINTER_SOURCE_PS2) {
            printf_fb_color("PS/2 ", FB_GREEN);
            if (welovec_ps2_synaptics_detected())
                printf_fb_color("(Synaptics relative mode) ", FB_GREEN);
        }
        if (pointer_sources & WELOVEC_POINTER_SOURCE_USB_HID)
            printf_fb_color("USB HID ", FB_GREEN);
        if (pointer_sources & WELOVEC_POINTER_SOURCE_VIRTIO)
            printf_fb_color("virtio-input ", FB_GREEN);
        if (pointer_sources & WELOVEC_POINTER_SOURCE_I2C_HID)
            printf_fb_color("I2C HID ", FB_GREEN);
        printf_fb_color("\n", FB_GREEN);
    } else if (xhci_probe_pending()) {
        printf_fb_color("USB pointer probe pending.\n", FB_ORANGE);
    } else {
        printf_fb_color("No supported pointing device detected.\n", FB_ORANGE);
    }
    printf_fb_color("I2C touchpad probe: ", FB_CYAN);
    printf_fb_color(welovec_i2c_touchpad_status(), FB_CYAN);
    printf_fb_color("\n", FB_CYAN);
    printf_fb_color("ACPI RSDP handoff: ", FB_CYAN);
    switch (boot_info ? boot_info->acpi_rsdp_source
                      : WELOVEC_ACPI_RSDP_SOURCE_NONE) {
        case WELOVEC_ACPI_RSDP_SOURCE_EFI_ACPI_2:
            printf_fb_color("EFI ACPI 2.x table\n", FB_CYAN);
            break;
        case WELOVEC_ACPI_RSDP_SOURCE_EFI_ACPI_1:
            printf_fb_color("EFI ACPI 1.0 table\n", FB_CYAN);
            break;
        case WELOVEC_ACPI_RSDP_SOURCE_EFI_CONFIG_SCAN:
            printf_fb_color("EFI configuration-table scan\n", FB_CYAN);
            break;
        case WELOVEC_ACPI_RSDP_SOURCE_EBDA:
            printf_fb_color("EBDA search\n", FB_CYAN);
            break;
        case WELOVEC_ACPI_RSDP_SOURCE_BIOS_ROM:
            printf_fb_color("BIOS ROM search\n", FB_CYAN);
            break;
        case WELOVEC_ACPI_RSDP_SOURCE_COPY_FAILED:
            printf_fb_color("found, but could not copy it\n", FB_ORANGE);
            break;
        default:
            printf_fb_color("not found by EFI or legacy scan\n", FB_ORANGE);
            break;
    }
    printf_fb_color("USB HID probe: ", FB_CYAN);
    printf_fb_color(welovec_xhci_probe_status(), FB_CYAN);
    printf_fb_color("\n", FB_CYAN);

    wait_seconds(3);

    // USB probing runs independently so a slow controller cannot hold startup.
    // Recheck after the existing delay before first-run account setup.
    if (!keyboard_ready && welovec_keyboard_sources() != 0)
        keyboard_ready = 1;

    kernel_console_write("Checking for an administrator account.");
    if (welovec_security_setup_required()) {
        printf_fb_color(" [NOT FOUND] Initial administrator setup required.\n",
                        FB_ORANGE);
        if (!keyboard_ready) {
            printf_fb_color("[security] Cannot create the administrator without a keyboard; startup stopped.\n",
                            FB_RED);
            return;
        }
        if (!welovec_create_user_setup()) {
            printf_fb_color("[security] Administrator setup did not complete; startup stopped.\n",
                            FB_RED);
            return;
        }
        if (welovec_security_setup_required()) {
            printf_fb_color("[security] Administrator is still missing; startup stopped.\n",
                            FB_RED);
            return;
        }
    } else {
        printf_fb_color(" [FOUND]\n", FB_GREEN);
    }

    kernel_console_write("Initializing Ethernet Service.");
    if (!welovec_ethernet_initialize()) {
        printf_fb_color(" [NOT FOUND; continuing without network]\n", FB_ORANGE);
    } else {
        printf_fb_color(" [OK]\n", FB_GREEN);

        kernel_console_write("Initializing network stack (DHCP).");
        if (!welovec_network_initialize()) {
            printf_fb_color(" [ERROR] Network stack unavailable; continuing startup.\n", FB_RED);
        } else {
            printf_fb_color(" [OK]\n", FB_GREEN);

            kernel_console_write("Starting network polling service.");
            if (welovec_network_start_polling()) {
                printf_fb_color(" [OK]\n", FB_GREEN);
            } else {
                printf_fb_color(" [ERROR] Network polling unavailable.\n", FB_RED);
            }
        }
    }

    kernel_console_write("Starting startupd / user space.\n\n");
    kernel_console_write("5 seconds until start..\n");
    wait_seconds(5);
    fb_clear_screen();
    if (welovec_startupd_run()) serial_write("[startup] user program started\n");
    else printf_fb_color(" [NO USER PROGRAM]\n", FB_RED);
}
