// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for mouse in the input module.

#ifndef WELOVEC_X86_64_MOUSE_HPP
#define WELOVEC_X86_64_MOUSE_HPP

#include <stdint.h>
#include "include/user/input_event.h"

namespace welovec::x86_64::input::pointer {

enum class Source : uint32_t {
    Ps2 = WELOVEC_POINTER_SOURCE_PS2,
    UsbHid = WELOVEC_POINTER_SOURCE_USB_HID,
    Virtio = WELOVEC_POINTER_SOURCE_VIRTIO,
    I2cHid = WELOVEC_POINTER_SOURCE_I2C_HID
};

void register_source(Source source);
uint32_t detected_sources();
void submit_relative(int32_t delta_x, int32_t delta_y, int32_t wheel,
                     uint32_t buttons, Source source);
void submit_absolute(int32_t x, int32_t y, int32_t min_x, int32_t max_x,
                     int32_t min_y, int32_t max_y, uint32_t buttons,
                     Source source);
int64_t read_events(welovec_pointer_event *events, uint64_t capacity);

}

#endif
