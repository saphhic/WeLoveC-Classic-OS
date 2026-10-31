// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for keyboard in the input module.

#ifndef WELOVEC_X86_64_KEYBOARD_HPP
#define WELOVEC_X86_64_KEYBOARD_HPP

#include <stdint.h>

struct WelovecBootInfo;

namespace welovec::x86_64::keyboard {

enum class Source : uint8_t { Ps2 = 1u << 0, UsbHid = 1u << 1, Virtio = 1u << 2 };
constexpr uint8_t kModifierLeftShift = 1u << 0;
constexpr uint8_t kModifierRightShift = 1u << 1;
constexpr uint8_t kModifierCapsLock = 1u << 2;
constexpr uint8_t kModifierLeftControl = 1u << 3;
constexpr uint8_t kModifierRightControl = 1u << 4;

struct UsbBootKeyboardState { uint8_t previous_keys[6]; };
struct VirtioKeyboardState {
    bool left_shift;
    bool right_shift;
    bool left_control;
    bool right_control;
};

bool initialize(const WelovecBootInfo *boot_info = nullptr);
void register_source(Source source);
void submit_hid_key(uint8_t usage, bool pressed, uint8_t modifiers);
void submit_usb_boot_report(const uint8_t report[8]);
void submit_usb_boot_report(UsbBootKeyboardState *state, const uint8_t report[8]);
void submit_virtio_event(uint16_t type, uint16_t code, int32_t value);
void submit_virtio_event(VirtioKeyboardState *state, uint16_t type,
                         uint16_t code, int32_t value);
bool try_read_char(char *out);
bool read_char(char *out);
uint64_t detected_sources();

}

#endif
