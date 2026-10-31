// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Adapts virtio-input events to the system keyboard interface.

#include "keyboard.hpp"

#include "arch/x86_64/mm/spinlock.hpp"

namespace welovec::x86_64::keyboard {

namespace {
constexpr uint16_t kEvKey = 0x01;
constexpr uint16_t kKeyLeftShift = 42;
constexpr uint16_t kKeyRightShift = 54;
constexpr uint16_t kKeyLeftControl = 29;
constexpr uint16_t kKeyRightControl = 97;
welovec::x86_64::mm::IrqSpinLock gStateLock;
VirtioKeyboardState gDefaultState{};

uint8_t linux_key_to_hid(uint16_t code) {
    if (code >= 2 && code <= 10) return static_cast<uint8_t>(0x1e + code - 2);
    if (code >= 59 && code <= 68) return static_cast<uint8_t>(0x3a + code - 59);
    switch (code) {
        case 1: return 0x29; // Escape
        case 11: return 0x27; // 0
        case 12: return 0x2d; case 13: return 0x2e;
        case 14: return 0x2a; case 15: return 0x2b;
        case 16: return 0x14; case 17: return 0x1a; case 18: return 0x08;
        case 19: return 0x15; case 20: return 0x17; case 21: return 0x1c;
        case 22: return 0x18; case 23: return 0x0c; case 24: return 0x12;
        case 25: return 0x13; case 26: return 0x2f; case 27: return 0x30;
        case 28: return 0x28; case 29: return 0xe0;
        case 30: return 0x04; case 31: return 0x16; case 32: return 0x07;
        case 33: return 0x09; case 34: return 0x0a; case 35: return 0x0b;
        case 36: return 0x0d; case 37: return 0x0e; case 38: return 0x0f;
        case 39: return 0x33; case 40: return 0x34; case 41: return 0x35;
        case 42: return 0xe1; case 43: return 0x31;
        case 44: return 0x1d; case 45: return 0x1b; case 46: return 0x06;
        case 47: return 0x19; case 48: return 0x05; case 49: return 0x11;
        case 50: return 0x10; case 51: return 0x36; case 52: return 0x37;
        case 53: return 0x38; case 54: return 0xe5; case 55: return 0x55;
        case 56: return 0xe2; case 57: return 0x2c; case 58: return 0x39;
        case 87: return 0x44; case 88: return 0x45;
        case 96: return 0x58; case 98: return 0x54; case 99: return 0x63;
        case 97: return 0xe4;
        case 102: return 0x4a; case 103: return 0x52; case 104: return 0x4b;
        case 105: return 0x50; case 106: return 0x4f; case 107: return 0x4d;
        case 108: return 0x51; case 109: return 0x4e; case 110: return 0x49;
        case 111: return 0x4c;
        default: return 0;
    }
}
}

/* virtio-input reports Linux input EV_KEY events; value 2 is key repeat. */
void submit_virtio_event(VirtioKeyboardState *state, uint16_t type,
                         uint16_t code, int32_t value) {
    if (!state || type != kEvKey || value < 0 || value > 2) return;
    const uint8_t usage = linux_key_to_hid(code);
    if (!usage) return;

    const bool pressed = value != 0;
    uint8_t modifiers = 0;
    const uint64_t irq = gStateLock.lock_irqsave();
    if (code == kKeyLeftShift) state->left_shift = pressed;
    if (code == kKeyRightShift) state->right_shift = pressed;
    if (code == kKeyLeftControl) state->left_control = pressed;
    if (code == kKeyRightControl) state->right_control = pressed;
    if (state->left_shift) modifiers |= kModifierLeftShift;
    if (state->right_shift) modifiers |= kModifierRightShift;
    if (state->left_control) modifiers |= kModifierLeftControl;
    if (state->right_control) modifiers |= kModifierRightControl;
    gStateLock.unlock_irqrestore(irq);

    register_source(Source::Virtio);
    /* The lock key toggles only on a press, never on autorepeat. */
    if (code == 58 && value == 2) return;
    submit_hid_key(usage, pressed, modifiers);
}

void submit_virtio_event(uint16_t type, uint16_t code, int32_t value) {
    submit_virtio_event(&gDefaultState, type, code, value);
}

}
