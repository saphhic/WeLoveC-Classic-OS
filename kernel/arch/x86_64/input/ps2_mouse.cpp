// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Initializes and reads PS/2 mice and compatible Synaptics devices.

#include "ps2_mouse.hpp"

#include "mouse.hpp"
#include "ps2_controller.hpp"

namespace {
constexpr uint8_t kBasicPacketSize = 3;
constexpr uint8_t kWheelPacketSize = 4;
constexpr uint8_t kPs2SetResolution = 0xe8;
constexpr uint8_t kPs2GetInformation = 0xe9;
constexpr uint8_t kPs2SetScaling11 = 0xe6;
constexpr uint8_t kPs2SetSampleRate = 0xf3;
constexpr uint8_t kSynapticsSetModeRelative = 0x14;

uint8_t gDeviceId;
uint8_t gPacket[kWheelPacketSize];
uint8_t gPacketSize = kBasicPacketSize;
uint8_t gPacketIndex;
uint32_t gButtons;
bool gInitialized;
bool gSynapticsDetected;

void submit_packet() {
    uint32_t buttons = 0;
    if (gPacket[0] & 1u) buttons |= WELOVEC_POINTER_BUTTON_LEFT;
    if (gPacket[0] & 2u) buttons |= WELOVEC_POINTER_BUTTON_RIGHT;
    if (gPacket[0] & 4u) buttons |= WELOVEC_POINTER_BUTTON_MIDDLE;

    int32_t wheel = 0;
    if (gPacketSize == kWheelPacketSize) {
        const uint8_t nibble = gPacket[3] & 0x0fu;
        wheel = (nibble & 0x08u) ? static_cast<int32_t>(nibble) - 16 : nibble;
        if (gDeviceId == 4) {
            if (gPacket[3] & 0x10u) buttons |= WELOVEC_POINTER_BUTTON_BACK;
            if (gPacket[3] & 0x20u) buttons |= WELOVEC_POINTER_BUTTON_FORWARD;
        }
    }

    const int32_t delta_x = static_cast<int8_t>(gPacket[1]);
    const int32_t delta_y = -static_cast<int32_t>(static_cast<int8_t>(gPacket[2]));
    if (buttons == gButtons && !delta_x && !delta_y && !wheel) return;
    gButtons = buttons;
    welovec::x86_64::input::pointer::submit_relative(
        delta_x, delta_y, wheel, buttons,
        welovec::x86_64::input::pointer::Source::Ps2);
}

void handle_auxiliary_byte(uint8_t byte) {
    if (!gPacketIndex && !(byte & 0x08u)) return;
    gPacket[gPacketIndex++] = byte;
    if (gPacketIndex < gPacketSize) return;
    gPacketIndex = 0;
    submit_packet();
}

bool send_rate(uint8_t rate) {
    return welovec::x86_64::input::ps2::send_auxiliary_byte(0xf3) &&
           welovec::x86_64::input::ps2::send_auxiliary_byte(rate);
}

bool query_device_id(uint8_t *out_id) {
    return out_id &&
        welovec::x86_64::input::ps2::send_auxiliary_byte(0xf2) &&
        welovec::x86_64::input::ps2::read_auxiliary_response(out_id);
}

bool set_synaptics_mode(uint8_t mode) {
    // Linux's ps2_sliced_command encoding: E6, followed by four E8
    // resolution writes carrying successive two-bit chunks of the mode.
    if (!welovec::x86_64::input::ps2::send_auxiliary_byte(kPs2SetScaling11))
        return false;
    for (int shift = 6; shift >= 0; shift -= 2) {
        if (!welovec::x86_64::input::ps2::send_auxiliary_byte(kPs2SetResolution) ||
            !welovec::x86_64::input::ps2::send_auxiliary_byte(
                static_cast<uint8_t>((mode >> shift) & 0x03u))) return false;
    }
    return welovec::x86_64::input::ps2::send_auxiliary_byte(kPs2SetSampleRate) &&
           welovec::x86_64::input::ps2::send_auxiliary_byte(
               kSynapticsSetModeRelative);
}

bool probe_synaptics() {
    uint8_t information[3]{};
    for (uint32_t i = 0; i < 4; ++i) {
        if (!welovec::x86_64::input::ps2::send_auxiliary_byte(kPs2SetResolution) ||
            !welovec::x86_64::input::ps2::send_auxiliary_byte(0)) {
            (void)welovec::x86_64::input::ps2::send_auxiliary_byte(0xf6);
            return false;
        }
    }
    if (!welovec::x86_64::input::ps2::send_auxiliary_byte(kPs2GetInformation) ||
        !welovec::x86_64::input::ps2::read_auxiliary_response(&information[0]) ||
        !welovec::x86_64::input::ps2::read_auxiliary_response(&information[1]) ||
        !welovec::x86_64::input::ps2::read_auxiliary_response(&information[2])) {
        (void)welovec::x86_64::input::ps2::send_auxiliary_byte(0xf6);
        return false;
    }
    if (information[1] != 0x47) {
        (void)welovec::x86_64::input::ps2::send_auxiliary_byte(0xf6);
        return false;
    }
    if (!set_synaptics_mode(0)) {
        (void)welovec::x86_64::input::ps2::send_auxiliary_byte(0xf6);
        return false;
    }
    return true;
}
}

namespace welovec::x86_64::input::ps2 {

bool initialize_mouse() {
    if (gInitialized) return true;
    if (!initialize_controller() || !enable_auxiliary_port()) return false;

    if (!send_auxiliary_byte(0xf5) || // Disable reporting during setup.
        !send_auxiliary_byte(0xf6) || // Restore standard defaults.
        !query_device_id(&gDeviceId)) goto no_mouse;

    // Probe Synaptics before IntelliMouse rate sequences. Its identify
    // command is the same four-resolution handshake used by Linux; when
    // found, switch it back to relative mode with gestures enabled so the
    // existing standard packet decoder can move the pointer.
    gSynapticsDetected = probe_synaptics();
    if (!gSynapticsDetected) {
        // Standard sample-rate sequences request optional wheel and
        // five-button packet formats supported by common PS/2 devices.
        if (send_rate(200) && send_rate(100) && send_rate(80)) {
            uint8_t extended_id = 0;
            if (query_device_id(&extended_id) &&
                (extended_id == 3 || extended_id == 4)) gDeviceId = extended_id;
        }
        if (gDeviceId == 3 || gDeviceId == 4) {
            gPacketSize = kWheelPacketSize;
            if (gDeviceId == 3 && send_rate(200) && send_rate(200) && send_rate(80)) {
                uint8_t extended_id = 0;
                if (query_device_id(&extended_id) && extended_id == 4) gDeviceId = 4;
            }
        }
    }

    if (!configure_auxiliary(true)) goto no_mouse;
    install_auxiliary_handler(handle_auxiliary_byte);
    if (!send_auxiliary_byte(0xf4)) goto no_mouse;
    (void)start_polling_worker();
    gInitialized = true;
    return true;

no_mouse:
    install_auxiliary_handler(nullptr);
    (void)configure_auxiliary(false);
    return false;
}

}

extern "C" int welovec_ps2_synaptics_detected(void) {
    return gSynapticsDetected ? 1 : 0;
}
