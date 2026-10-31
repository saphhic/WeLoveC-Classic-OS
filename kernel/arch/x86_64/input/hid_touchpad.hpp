// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for hid touchpad in the input module.

#ifndef WELOVEC_X86_64_HID_TOUCHPAD_HPP
#define WELOVEC_X86_64_HID_TOUCHPAD_HPP

#include <stdint.h>
#include "mouse.hpp"

namespace welovec::x86_64::input::hid_touchpad {

constexpr uint32_t kMaximumReportBytes = 1024;

struct Field {
    uint16_t bit_offset;
    uint16_t usage;
    int32_t logical_minimum;
    uint8_t report_id;
    uint8_t report_size;
    uint8_t usage_page;
    uint8_t contact_slot;
    uint8_t relative;
};

struct GestureState {
    int32_t previous_x[2];
    int32_t previous_y[2];
    int32_t motion_remainder_x;
    int32_t motion_remainder_y;
    int32_t scroll_remainder;
    uint32_t buttons;
    uint8_t previous_contacts;
    uint8_t tap_frames;
    bool tap_candidate;
    bool touch_report_seen;
    bool mouse_x_valid;
    bool mouse_y_valid;
};

struct State {
    Field fields[80];
    uint16_t report_bytes[256];
    uint16_t field_count;
    uint16_t maximum_report_bytes;
    uint8_t uses_report_ids;
    uint8_t touch_report[256];
    uint8_t mouse_report[256];
    uint8_t has_tip_switch[256];
    uint8_t has_contact_id[256];
    GestureState gesture;
};

bool parse_report_descriptor(State *state, const uint8_t *descriptor,
                             uint32_t length);
uint16_t maximum_report_size(const State *state);
bool process_report(State *state, const uint8_t *report, uint32_t length,
                    pointer::Source source);

}

#endif
