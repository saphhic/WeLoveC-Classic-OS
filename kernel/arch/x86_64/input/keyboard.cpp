// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements keyboard input and key event normalization.

#include "keyboard.hpp"
#include "mouse.hpp"
#include "ps2_mouse.hpp"
#include "virtio_input.hpp"
#include "xhci.hpp"
#include "i2c_touchpad.hpp"

#include "arch/x86_64/scheduler/scheduler.hpp"
#include "arch/x86_64/mm/spinlock.hpp"
#include "arch/x86_64/io/console.h"

namespace welovec::x86_64::keyboard {

namespace {
constexpr uint32_t kCharacterQueueSize = 512;
constexpr uint32_t kEventQueueSize = 256;
struct KeyEvent { uint8_t usage; uint8_t modifiers; bool pressed; };

welovec::x86_64::mm::IrqSpinLock gLock;
char gCharacters[kCharacterQueueSize];
uint32_t gCharacterRead;
uint32_t gCharacterWrite;
KeyEvent gEvents[kEventQueueSize];
uint32_t gEventRead;
uint32_t gEventWrite;
uint64_t gSources;
bool gCapsLock;
bool gCapsLockKeyDown;
bool gInitialized;

/* Every input source is normalized to USB HID usages, then translated with
 * the fixed default US QWERTY layout. */
char translate_usage(uint8_t usage, uint8_t modifiers) {
    const bool shift = (modifiers & (kModifierLeftShift | kModifierRightShift)) != 0;
    const bool caps = (modifiers & kModifierCapsLock) != 0;
    if (usage >= 0x04 && usage <= 0x1d) {
        char letter = static_cast<char>('a' + usage - 0x04);
        if (shift != caps) letter = static_cast<char>(letter - 'a' + 'A');
        return letter;
    }
    if (usage >= 0x1e && usage <= 0x27) {
        static const char normal[] = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        const uint8_t index = usage - 0x1e;
        return shift ? shifted[index] : normal[index];
    }
    switch (usage) {
        case 0x28: return '\n';
        case 0x29: return 0x1b;
        case 0x2a: return '\b';
        case 0x2b: return '\t';
        case 0x2c: return ' ';
        case 0x2d: return shift ? '_' : '-';
        case 0x2e: return shift ? '+' : '=';
        case 0x2f: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        case 0x54: return '/';
        case 0x55: return '*';
        case 0x56: return '-';
        case 0x57: return '+';
        case 0x58: return '\n';
        case 0x59: return '1';
        case 0x5a: return '2';
        case 0x5b: return '3';
        case 0x5c: return '4';
        case 0x5d: return '5';
        case 0x5e: return '6';
        case 0x5f: return '7';
        case 0x60: return '8';
        case 0x61: return '9';
        case 0x62: return '0';
        case 0x63: return '.';
        default: return 0;
    }
}

void push_character_locked(char value) {
    const uint32_t next = (gCharacterWrite + 1) % kCharacterQueueSize;
    if (next == gCharacterRead) gCharacterRead = (gCharacterRead + 1) % kCharacterQueueSize;
    gCharacters[gCharacterWrite] = value;
    gCharacterWrite = next;
}

void push_sequence_locked(const char *sequence, uint32_t length) {
    for (uint32_t index = 0; index < length; ++index)
        push_character_locked(sequence[index]);
}
}

void submit_hid_key(uint8_t usage, bool pressed, uint8_t modifiers) {
    if (!usage) return;
    const uint64_t irq = gLock.lock_irqsave();
    if (usage == 0x39) {
        if (pressed && !gCapsLockKeyDown) gCapsLock = !gCapsLock;
        gCapsLockKeyDown = pressed;
    }
    if (gCapsLock) modifiers |= kModifierCapsLock;
    else modifiers &= static_cast<uint8_t>(~kModifierCapsLock);
    const uint32_t next = (gEventWrite + 1) % kEventQueueSize;
    if (next == gEventRead) gEventRead = (gEventRead + 1) % kEventQueueSize;
    gEvents[gEventWrite] = {usage, modifiers, pressed};
    gEventWrite = next;
    if (pressed) {
        const bool control = (modifiers &
            (kModifierLeftControl | kModifierRightControl)) != 0;
        if (usage == 0x50 || usage == 0x4f || usage == 0x52 || usage == 0x51) {
            char final = usage == 0x50 ? 'D' : usage == 0x4f ? 'C' :
                         usage == 0x52 ? 'A' : 'B';
            if (control && usage == 0x52) {
                static const char ctrl_up[] = "\x1b[1;5A";
                push_sequence_locked(ctrl_up, sizeof(ctrl_up) - 1);
            } else if (control && usage == 0x51) {
                static const char ctrl_down[] = "\x1b[1;5B";
                push_sequence_locked(ctrl_down, sizeof(ctrl_down) - 1);
            } else {
                const char sequence[] = {'\x1b', '[', final};
                push_sequence_locked(sequence, sizeof(sequence));
            }
        } else {
        const char value = translate_usage(usage, modifiers);
        if (value) push_character_locked(value);
        }
    }
    gLock.unlock_irqrestore(irq);
}

void register_source(Source source) {
    const uint64_t irq = gLock.lock_irqsave();
    gSources |= static_cast<uint64_t>(source);
    gLock.unlock_irqrestore(irq);
}

bool try_read_char(char *out) {
    if (!out) return false;
    const uint64_t irq = gLock.lock_irqsave();
    if (gCharacterRead == gCharacterWrite) {
        gLock.unlock_irqrestore(irq);
        return false;
    }
    *out = gCharacters[gCharacterRead];
    gCharacterRead = (gCharacterRead + 1) % kCharacterQueueSize;
    gLock.unlock_irqrestore(irq);
    return true;
}

bool read_char(char *out) {
    while (!try_read_char(out)) {
        if (!welovec::x86_64::scheduler::sleep(1)) return false;
    }
    return true;
}

uint64_t detected_sources() {
    const uint64_t irq = gLock.lock_irqsave();
    const uint64_t sources = gSources;
    gLock.unlock_irqrestore(irq);
    return sources;
}

bool initialize(const WelovecBootInfo *boot_info) {
    if (gInitialized) return detected_sources() != 0;
    gInitialized = true;
    extern bool initialize_ps2_keyboard();
    kernel_console_write("\n[input] PS/2 keyboard probe\n");
    if (initialize_ps2_keyboard()) register_source(Source::Ps2);
    kernel_console_write("[input] PS/2 mouse probe\n");
    if (welovec::x86_64::input::ps2::initialize_mouse())
        welovec::x86_64::input::pointer::register_source(
            welovec::x86_64::input::pointer::Source::Ps2);
    kernel_console_write("[input] virtio input probe\n");
    (void)welovec::x86_64::input::virtio::initialize();
    kernel_console_write("[input] USB xHCI probe\n");
    (void)welovec::x86_64::input::usb::start_probe();
    kernel_console_write("[input] I2C HID probe\n");
    (void)welovec::x86_64::input::i2c_touchpad::initialize(boot_info);
    kernel_console_write("[input] probes complete\n");
    return detected_sources() != 0;
}

}

extern "C" int welovec_keyboard_initialize(const WelovecBootInfo *boot_info) {
    return welovec::x86_64::keyboard::initialize(boot_info) ? 1 : 0;
}

extern "C" int64_t welovec_keyboard_read(char *buffer, uint64_t capacity,
                                           int wait_for_input) {
    if (!buffer || !capacity) return 0;
    uint64_t count = 0;
    char value;
    if (wait_for_input) {
        if (!welovec::x86_64::keyboard::read_char(&value)) return -1;
        buffer[count++] = value;
    }
    while (count < capacity && welovec::x86_64::keyboard::try_read_char(&value))
        buffer[count++] = value;
    return static_cast<int64_t>(count);
}

extern "C" int welovec_keyboard_available(void) {
    return welovec::x86_64::keyboard::detected_sources() != 0 ? 1 : 0;
}

extern "C" uint64_t welovec_keyboard_sources(void) {
    return welovec::x86_64::keyboard::detected_sources();
}

extern "C" void welovec_usb_hid_keyboard_report(const uint8_t report[8]) {
    welovec::x86_64::keyboard::submit_usb_boot_report(report);
}

extern "C" void welovec_virtio_keyboard_event(uint16_t type, uint16_t code,
                                               int32_t value) {
    welovec::x86_64::keyboard::submit_virtio_event(type, code, value);
}
