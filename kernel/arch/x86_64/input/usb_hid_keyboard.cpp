// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Adapts USB HID keyboard events to the system input format.

#include "keyboard.hpp"

#include "arch/x86_64/mm/spinlock.hpp"

namespace welovec::x86_64::keyboard {

namespace {
welovec::x86_64::mm::IrqSpinLock gReportLock;
UsbBootKeyboardState gDefaultState{};
}

/* USB HID boot-keyboard reports are modifiers, reserved, then six usages. */
void submit_usb_boot_report(UsbBootKeyboardState *state, const uint8_t report[8]) {
    if (!state || !report) return;

    uint8_t previous[6];
    uint8_t current[6];
    uint8_t report_modifiers;
    const uint64_t irq = gReportLock.lock_irqsave();
    report_modifiers = report[0];
    for (uint32_t i = 0; i < 6; ++i) {
        previous[i] = state->previous_keys[i];
        current[i] = report[2 + i];
    }

    /* HID usages 1..3 mean rollover/error: keep the last valid key state. */
    for (uint8_t key : current) {
        if (key >= 1 && key <= 3) {
            gReportLock.unlock_irqrestore(irq);
            return;
        }
    }
    for (uint32_t i = 0; i < 6; ++i) state->previous_keys[i] = current[i];
    gReportLock.unlock_irqrestore(irq);

    uint8_t modifiers = 0;
    if (report_modifiers & (1u << 0)) modifiers |= kModifierLeftControl;
    if (report_modifiers & (1u << 1)) modifiers |= kModifierLeftShift;
    if (report_modifiers & (1u << 4)) modifiers |= kModifierRightControl;
    if (report_modifiers & (1u << 5)) modifiers |= kModifierRightShift;

    register_source(Source::UsbHid);
    for (uint8_t key : previous) {
        if (!key) continue;
        bool still_down = false;
        for (uint8_t candidate : current)
            if (candidate == key) { still_down = true; break; }
        if (!still_down) submit_hid_key(key, false, modifiers);
    }
    for (uint8_t key : current) {
        if (!key) continue;
        bool already_down = false;
        for (uint8_t candidate : previous)
            if (candidate == key) { already_down = true; break; }
        if (!already_down) submit_hid_key(key, true, modifiers);
    }
}

void submit_usb_boot_report(const uint8_t report[8]) {
    submit_usb_boot_report(&gDefaultState, report);
}

}
