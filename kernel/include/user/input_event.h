// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Defines the event format for keyboards and pointer devices.

#ifndef WELOVEC_USER_INPUT_EVENT_H
#define WELOVEC_USER_INPUT_EVENT_H

#include <stdint.h>

enum {
    WELOVEC_POINTER_SOURCE_PS2 = 1u << 0,
    WELOVEC_POINTER_SOURCE_USB_HID = 1u << 1,
    WELOVEC_POINTER_SOURCE_VIRTIO = 1u << 2,
    WELOVEC_POINTER_SOURCE_I2C_HID = 1u << 3
};

enum {
    WELOVEC_POINTER_EVENT_RELATIVE = 1u << 0,
    WELOVEC_POINTER_EVENT_ABSOLUTE = 1u << 1
};

enum {
    WELOVEC_POINTER_BUTTON_LEFT = 1u << 0,
    WELOVEC_POINTER_BUTTON_RIGHT = 1u << 1,
    WELOVEC_POINTER_BUTTON_MIDDLE = 1u << 2,
    WELOVEC_POINTER_BUTTON_BACK = 1u << 3,
    WELOVEC_POINTER_BUTTON_FORWARD = 1u << 4
};

typedef struct welovec_pointer_event {
    int32_t delta_x;
    int32_t delta_y;
    int32_t wheel;
    int32_t absolute_x; /* Normalized to 0..65535 when ABSOLUTE is set. */
    int32_t absolute_y;
    uint32_t buttons;
    uint32_t flags;
    uint32_t source;
    uint32_t reserved;
} welovec_pointer_event;

#endif
