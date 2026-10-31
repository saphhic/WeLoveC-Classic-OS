// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Initializes and reads keyboards connected through the PS/2 controller.

#include "keyboard.hpp"
#include "ps2_controller.hpp"

#include "arch/x86_64/inter/isr.h"
#include "arch/x86_64/inter/pic.h"
#include "arch/x86_64/io/io.h"
#include "arch/x86_64/scheduler/scheduler.hpp"
#include "arch/x86_64/mm/spinlock.hpp"

namespace {
constexpr uint16_t kDataPort = 0x60;
constexpr uint16_t kStatusCommandPort = 0x64;
constexpr uint8_t kOutputFull = 1u << 0;
constexpr uint8_t kInputFull = 1u << 1;
constexpr uint8_t kAuxiliaryData = 1u << 5;
constexpr uint8_t kIrq1Enable = 1u << 0;
constexpr uint8_t kIrq12Enable = 1u << 1;
constexpr uint8_t kTranslationEnable = 1u << 6;
constexpr uint32_t kWaitLimit = 1000000;
constexpr uint32_t kMouseWaitLimit = 100000;

welovec::x86_64::mm::IrqSpinLock gControllerLock;
bool gExtended;
bool gBreak;
bool gLeftShift;
bool gRightShift;
bool gLeftControl;
bool gRightControl;
uint8_t gPauseBytesToSkip;
bool gControllerReady;
bool gControllerProbeDone;
bool gKeyboardInitialized;
bool gPollWorkerStarted;
welovec::x86_64::input::ps2::AuxiliaryByteHandler gAuxiliaryHandler;

bool wait_input_empty() {
    for (uint32_t i = 0; i < kWaitLimit; ++i)
        if (!(inb(kStatusCommandPort) & kInputFull)) return true;
    return false;
}

bool wait_output_full() {
    for (uint32_t i = 0; i < kWaitLimit; ++i)
        if (inb(kStatusCommandPort) & kOutputFull) return true;
    return false;
}

void flush_output() {
    for (uint32_t i = 0; i < 256 && (inb(kStatusCommandPort) & kOutputFull); ++i)
        (void)inb(kDataPort);
}

bool controller_command(uint8_t command) {
    return wait_input_empty() && (outb(kStatusCommandPort, command), true);
}

bool write_config(uint8_t config) {
    return controller_command(0x60) && wait_input_empty() &&
           (outb(kDataPort, config), true);
}

bool keyboard_send_byte(uint8_t value) {
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        if (!wait_input_empty()) return false;
        outb(kDataPort, value);
        bool got_resend = false;
        for (uint32_t poll = 0; poll < kWaitLimit; ++poll) {
            const uint8_t status = inb(kStatusCommandPort);
            if (!(status & kOutputFull)) continue;
            const uint8_t data = inb(kDataPort);
            // The i8042 multiplexes keyboard and TrackPoint/mouse bytes on
            // the same output port. Ignore auxiliary traffic while waiting
            // for the keyboard ACK/RESEND byte.
            if (status & kAuxiliaryData) continue;
            if (data == 0xfa) return true;
            if (data == 0xfe) {
                got_resend = true;
                break;
            }
            // Discard scan bytes already in flight while the command is pending.
        }
        if (!got_resend) return false;
    }
    return false;
}

bool ensure_controller() {
    if (gControllerReady) return true;
    if (gControllerProbeDone) return false;
    gControllerProbeDone = true;
    if (!controller_command(0xad) || !controller_command(0xa7)) return false;
    flush_output();
    if (!controller_command(0x20) || !wait_output_full()) return false;
    uint8_t config = inb(kDataPort);
    config = static_cast<uint8_t>((config | 0x30u) &
        ~(kIrq1Enable | kIrq12Enable | kTranslationEnable));
    if (!write_config(config) || !controller_command(0xae)) return false;
    gControllerReady = true;
    return true;
}

bool update_controller_config(bool keyboard_enabled, bool mouse_enabled) {
    if (!controller_command(0x20) || !wait_output_full()) return false;
    uint8_t config = inb(kDataPort);
    config &= static_cast<uint8_t>(~(kIrq1Enable | kIrq12Enable | 0x30u));
    if (keyboard_enabled) config |= kIrq1Enable;
    if (mouse_enabled) config |= kIrq12Enable;
    if (!keyboard_enabled) config |= 0x10u;
    if (!mouse_enabled) config |= 0x20u;
    config &= static_cast<uint8_t>(~kTranslationEnable);
    return write_config(config);
}

uint8_t set2_to_hid(uint8_t scan, bool extended) {
    if (extended) {
        switch (scan) {
            case 0x14: return 0xe4; // right control
            case 0x11: return 0xe6; // right alt
            case 0x1f: return 0xe3; // left GUI
            case 0x27: return 0xe7; // right GUI
            case 0x2f: return 0xe8; // application
            case 0x7c: return 0x46; // print screen
            case 0x5a: return 0x58; // keypad enter
            case 0x4a: return 0x54; // keypad divide
            case 0x75: return 0x52; // up
            case 0x72: return 0x51; // down
            case 0x6b: return 0x50; // left
            case 0x74: return 0x4f; // right
            case 0x6c: return 0x4a; // home
            case 0x69: return 0x4d; // end
            case 0x7d: return 0x4b; // page up
            case 0x7a: return 0x4e; // page down
            case 0x70: return 0x49; // insert
            case 0x71: return 0x4c; // delete
            default: return 0;
        }
    }
    switch (scan) {
        case 0x16: return 0x1e; case 0x1e: return 0x1f;
        case 0x26: return 0x20; case 0x25: return 0x21;
        case 0x2e: return 0x22; case 0x36: return 0x23;
        case 0x3d: return 0x24; case 0x3e: return 0x25;
        case 0x46: return 0x26; case 0x45: return 0x27;
        case 0x66: return 0x2a; case 0x0d: return 0x2b;
        case 0x5a: return 0x28; case 0x76: return 0x29;
        case 0x29: return 0x2c;
        case 0x4e: return 0x2d; case 0x55: return 0x2e;
        case 0x54: return 0x2f; case 0x5b: return 0x30;
        case 0x5d: return 0x31; case 0x4c: return 0x33;
        case 0x52: return 0x34; case 0x0e: return 0x35;
        case 0x41: return 0x36; case 0x49: return 0x37;
        case 0x4a: return 0x38;
        case 0x1c: return 0x04; case 0x32: return 0x05;
        case 0x21: return 0x06; case 0x23: return 0x07;
        case 0x24: return 0x08; case 0x2b: return 0x09;
        case 0x34: return 0x0a; case 0x33: return 0x0b;
        case 0x43: return 0x0c; case 0x3b: return 0x0d;
        case 0x42: return 0x0e; case 0x4b: return 0x0f;
        case 0x3a: return 0x10; case 0x31: return 0x11;
        case 0x44: return 0x12; case 0x4d: return 0x13;
        case 0x15: return 0x14; case 0x2d: return 0x15;
        case 0x1b: return 0x16; case 0x2c: return 0x17;
        case 0x3c: return 0x18; case 0x2a: return 0x19;
        case 0x1d: return 0x1a; case 0x22: return 0x1b;
        case 0x35: return 0x1c; case 0x1a: return 0x1d;
        case 0x12: return 0xe1; case 0x59: return 0xe5;
        case 0x14: return 0xe0; case 0x11: return 0xe2;
        case 0x58: return 0x39;
        case 0x05: return 0x3a; case 0x06: return 0x3b;
        case 0x04: return 0x3c; case 0x0c: return 0x3d;
        case 0x03: return 0x3e; case 0x0b: return 0x3f;
        case 0x83: return 0x40; case 0x0a: return 0x41;
        case 0x01: return 0x42; case 0x09: return 0x43;
        case 0x78: return 0x44; case 0x07: return 0x45;
        case 0x77: return 0x53; case 0x7e: return 0x47;
        case 0x7c: return 0x55; case 0x7b: return 0x56;
        case 0x79: return 0x57;
        case 0x6c: return 0x5f; case 0x75: return 0x60;
        case 0x7d: return 0x61; case 0x6b: return 0x5c;
        case 0x73: return 0x5d; case 0x74: return 0x5e;
        case 0x69: return 0x59; case 0x72: return 0x5a;
        case 0x7a: return 0x5b; case 0x70: return 0x62;
        case 0x71: return 0x63;
        default: return 0;
    }
}

void process_scancode(uint8_t scan) {
    if (gPauseBytesToSkip) { --gPauseBytesToSkip; return; }
    if (scan == 0xe1) {
        gExtended = false;
        gBreak = false;
        gPauseBytesToSkip = 7;
        return;
    }
    if (scan == 0xe0) { gExtended = true; return; }
    if (scan == 0xf0) { gBreak = true; return; }
    const bool released = gBreak;
    const uint8_t key = set2_to_hid(scan, gExtended);
    gExtended = false;
    gBreak = false;
    if (!key) return;

    if (key == 0xe1) gLeftShift = !released;
    else if (key == 0xe5) gRightShift = !released;
    else if (key == 0xe0) gLeftControl = !released;
    else if (key == 0xe4) gRightControl = !released;
    const uint8_t modifiers = (gLeftShift ? welovec::x86_64::keyboard::kModifierLeftShift : 0) |
        (gRightShift ? welovec::x86_64::keyboard::kModifierRightShift : 0) |
        (gLeftControl ? welovec::x86_64::keyboard::kModifierLeftControl : 0) |
        (gRightControl ? welovec::x86_64::keyboard::kModifierRightControl : 0);
    welovec::x86_64::keyboard::submit_hid_key(key, !released, modifiers);
}

bool read_aux_byte(uint8_t *value) {
    if (!value) return false;
    for (uint32_t poll = 0; poll < kMouseWaitLimit; ++poll) {
        const uint8_t status = inb(kStatusCommandPort);
        if (!(status & kOutputFull)) continue;
        const uint8_t data = inb(kDataPort);
        if (status & kAuxiliaryData) {
            *value = data;
            return true;
        }
        process_scancode(data);
    }
    return false;
}

bool mouse_send_byte(uint8_t value) {
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        if (!wait_input_empty() || !controller_command(0xd4) ||
            !wait_input_empty()) return false;
        outb(kDataPort, value);
        for (uint32_t poll = 0; poll < kMouseWaitLimit; ++poll) {
            const uint8_t status = inb(kStatusCommandPort);
            if (!(status & kOutputFull)) continue;
            const uint8_t data = inb(kDataPort);
            if (!(status & kAuxiliaryData)) {
                process_scancode(data);
                continue;
            }
            if (data == 0xfa) return true;
            if (data == 0xfe) break;
        }
    }
    return false;
}

void drain_controller_output() {
    const uint8_t status = inb(kStatusCommandPort);
    if (!(status & kOutputFull)) return;
    const uint8_t data = inb(kDataPort);
    if (status & kAuxiliaryData) {
        if (gAuxiliaryHandler) gAuxiliaryHandler(data);
    } else {
        process_scancode(data);
    }
}

void keyboard_irq(registers_t *) {
    const uint64_t irq = gControllerLock.lock_irqsave();
    drain_controller_output();
    gControllerLock.unlock_irqrestore(irq);
}

void mouse_irq(registers_t *) {
    const uint64_t irq = gControllerLock.lock_irqsave();
    drain_controller_output();
    gControllerLock.unlock_irqrestore(irq);
}

void ps2_poll_worker(void *) {
    for (;;) {
        const uint64_t irq = gControllerLock.lock_irqsave();
        // Poll more than one queued byte per tick. This also covers machines
        // whose firmware routes IRQ1 through an IOAPIC path we do not yet own.
        for (uint32_t i = 0; i < 32; ++i) {
            if (!(inb(kStatusCommandPort) & kOutputFull)) break;
            drain_controller_output();
        }
        gControllerLock.unlock_irqrestore(irq);
        (void)welovec::x86_64::scheduler::sleep(1);
    }
}

bool start_poll_worker() {
    if (gPollWorkerStarted) return true;
    gPollWorkerStarted = welovec::x86_64::scheduler::create_kernel_thread(
        "ps2-input-poll", ps2_poll_worker, nullptr,
        welovec::x86_64::scheduler::Priority::Background) != 0;
    return gPollWorkerStarted;
}
}

namespace welovec::x86_64::input::ps2 {
bool initialize_controller() {
    const uint64_t irq = gControllerLock.lock_irqsave();
    const bool initialized = ensure_controller();
    gControllerLock.unlock_irqrestore(irq);
    return initialized;
}

bool enable_auxiliary_port() {
    const uint64_t irq = gControllerLock.lock_irqsave();
    const bool enabled = ensure_controller() && controller_command(0xa8);
    gControllerLock.unlock_irqrestore(irq);
    return enabled;
}

bool configure_auxiliary(bool enabled) {
    const uint64_t irq = gControllerLock.lock_irqsave();
    if (!ensure_controller()) {
        gControllerLock.unlock_irqrestore(irq);
        return false;
    }
    if (!enabled) (void)controller_command(0xa7);
    const bool configured = update_controller_config(gKeyboardInitialized, enabled);
    gControllerLock.unlock_irqrestore(irq);
    return configured;
}

bool send_auxiliary_byte(uint8_t value) {
    const uint64_t irq = gControllerLock.lock_irqsave();
    const bool sent = ensure_controller() && mouse_send_byte(value);
    gControllerLock.unlock_irqrestore(irq);
    return sent;
}

bool read_auxiliary_response(uint8_t *value) {
    const uint64_t irq = gControllerLock.lock_irqsave();
    const bool received = ensure_controller() && read_aux_byte(value);
    gControllerLock.unlock_irqrestore(irq);
    return received;
}

void install_auxiliary_handler(AuxiliaryByteHandler handler) {
    const uint64_t irq = gControllerLock.lock_irqsave();
    gAuxiliaryHandler = handler;
    gControllerLock.unlock_irqrestore(irq);
    if (handler) {
        irq_install_handler(12, mouse_irq);
        pic_clear_mask(2);
        pic_clear_mask(12);
    } else {
        pic_set_mask(12);
    }
}

bool start_polling_worker() { return start_poll_worker(); }
}

namespace welovec::x86_64::keyboard {
bool initialize_ps2_keyboard() {
    if (gKeyboardInitialized) return true;
    if (!ensure_controller()) return false;

    if (!keyboard_send_byte(0xf5)) return false; // Stop scanning.
    flush_output(); // Drop make/break bytes queued before scanning stopped.
    if (!keyboard_send_byte(0xf0) || !keyboard_send_byte(0x02)) return false;
    // Select native Set 2; the host parses its make/break sequences directly.
    if (!keyboard_send_byte(0xf4)) return false; // Enable scanning.
    if (!update_controller_config(true, false)) return false;

    irq_install_handler(1, keyboard_irq);
    pic_clear_mask(1);

    // Polling is a fallback for firmware that leaves the legacy PIC/IRQ1
    // disconnected. IRQ-driven input remains active when the route exists.
    (void)welovec::x86_64::input::ps2::start_polling_worker();
    gKeyboardInitialized = true;
    return true;
}

}
