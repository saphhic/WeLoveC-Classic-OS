// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for keyboard in the input module.

#ifndef WELOVEC_X86_64_KEYBOARD_H
#define WELOVEC_X86_64_KEYBOARD_H

#include <stdint.h>

#define WELOVEC_KEYBOARD_SOURCE_PS2 UINT64_C(1)
#define WELOVEC_KEYBOARD_SOURCE_USB_HID UINT64_C(2)
#define WELOVEC_KEYBOARD_SOURCE_VIRTIO UINT64_C(4)

struct WelovecBootInfo;

#ifdef __cplusplus
extern "C" {
#endif

int welovec_keyboard_initialize(const struct WelovecBootInfo *boot_info);
/* Blocks until at least one byte is available when wait_for_input is nonzero. */
int64_t welovec_keyboard_read(char *buffer, uint64_t capacity, int wait_for_input);
int welovec_keyboard_available(void);
uint64_t welovec_keyboard_sources(void);
/* Protocol adapters; xHCI and virtio PCI transports are started by initialize(). */
void welovec_usb_hid_keyboard_report(const uint8_t report[8]);
void welovec_virtio_keyboard_event(uint16_t type, uint16_t code, int32_t value);

#ifdef __cplusplus
}
#endif

#endif
