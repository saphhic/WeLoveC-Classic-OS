// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the Realtek RTL8139 Ethernet driver.


//written by saphhic.
#include "rtl8139.h"
#include "../../../arch/x86_64/io/io.h"
#include "../../../arch/x86_64/mm/paging/page_table.hpp"

extern "C" void* memcpy(void* destination, const void* source, unsigned long long size);
extern "C" void* memset(void* destination, int value, unsigned long long size);

namespace {

constexpr uint16_t kPciCommandIo = 0x0001;
constexpr uint16_t kPciCommandBusMaster = 0x0004;

constexpr uint8_t kSupportedDeviceCount = 4;
constexpr uint16_t kSupportedVendorIds[kSupportedDeviceCount] = {
    0x10EC, 0x10EC, 0x1113, 0x1186
};
constexpr uint16_t kSupportedDeviceIds[kSupportedDeviceCount] = {
    0x8139, 0x8138, 0x1211, 0x1300
};

constexpr uint16_t kRegisterMac0 = 0x00;
constexpr uint16_t kRegisterMar0 = 0x08;
constexpr uint16_t kRegisterTxStatus0 = 0x10;
constexpr uint16_t kRegisterTxAddress0 = 0x20;
constexpr uint16_t kRegisterRxBuffer = 0x30;
constexpr uint16_t kRegisterChipCommand = 0x37;
constexpr uint16_t kRegisterCurrentAddressOfPacketRead = 0x38;
constexpr uint16_t kRegisterInterruptMask = 0x3C;
constexpr uint16_t kRegisterInterruptStatus = 0x3E;
constexpr uint16_t kRegisterTxConfig = 0x40;
constexpr uint16_t kRegisterRxConfig = 0x44;
constexpr uint16_t kRegisterRxMissed = 0x4C;
constexpr uint16_t kRegisterCfg9346 = 0x50;
constexpr uint16_t kRegisterConfig1 = 0x52;
constexpr uint16_t kRegisterMediaStatus = 0x58;
constexpr uint16_t kRegisterHaltClock = 0x5B;

constexpr uint8_t kCfg9346Lock = 0x00;
constexpr uint8_t kCfg9346Unlock = 0xC0;
constexpr uint8_t kConfig1DriverLoaded = 0x20;
constexpr uint8_t kConfig1Sleep = 0x02;
constexpr uint8_t kConfig1PowerDown = 0x01;

constexpr uint8_t kChipCommandReset = 0x10;
constexpr uint8_t kChipCommandReceiveEnable = 0x08;
constexpr uint8_t kChipCommandTransmitEnable = 0x04;
constexpr uint8_t kChipCommandReceiveBufferEmpty = 0x01;

constexpr uint16_t kInterruptReceiveOk = 0x0001;
constexpr uint16_t kInterruptReceiveError = 0x0002;
constexpr uint16_t kInterruptTransmitOk = 0x0004;
constexpr uint16_t kInterruptTransmitError = 0x0008;
constexpr uint16_t kInterruptReceiveOverflow = 0x0010;
constexpr uint16_t kInterruptReceiveFifoOverflow = 0x0040;
constexpr uint16_t kInterruptAckMask =
    kInterruptReceiveOk |
    kInterruptReceiveError |
    kInterruptTransmitOk |
    kInterruptTransmitError |
    kInterruptReceiveOverflow |
    kInterruptReceiveFifoOverflow;

constexpr uint16_t kReceiveStatusOk = 0x0001;

constexpr uint32_t kReceiveAcceptBroadcast = 0x08;
constexpr uint32_t kReceiveAcceptMyPhysicalAddress = 0x02;
constexpr uint32_t kReceiveFifoThresholdShift = 13;
constexpr uint32_t kReceiveDmaShift = 8;
constexpr uint32_t kReceive32KBuffer = (1u << 12);
constexpr uint32_t kReceiveNoWrap = (1u << 7);
constexpr uint32_t kReceiveConfig =
    kReceive32KBuffer |
    kReceiveNoWrap |
    (7u << kReceiveFifoThresholdShift) |
    (7u << kReceiveDmaShift) |
    kReceiveAcceptBroadcast |
    kReceiveAcceptMyPhysicalAddress;

constexpr uint32_t kTransmitInterframeGap96 = (3u << 24);
constexpr uint32_t kTransmitDmaShift = 8;
constexpr uint32_t kTransmitRetryShift = 4;
constexpr uint32_t kTransmitConfig =
    kTransmitInterframeGap96 |
    (6u << kTransmitDmaShift) |
    (8u << kTransmitRetryShift);

constexpr uint32_t kTransmitThreshold = 256;
constexpr uint32_t kTransmitFlags = (kTransmitThreshold << 11) & 0x003F0000u;
constexpr uint32_t kTransmitStatusOk = 0x00008000u;
constexpr uint32_t kTransmitStatusUnderrun = 0x00004000u;
constexpr uint32_t kTransmitStatusAborted = 0x40000000u;
constexpr uint32_t kTransmitStatusOutOfWindow = 0x20000000u;
constexpr uint32_t kTransmitStatusCarrierLost = 0x80000000u;

constexpr uint32_t kMediaStatusLinkFail = 0x04;

constexpr uint32_t kRxBufferIndex = 2;
constexpr uint32_t kRxBufferLength = 8192u << kRxBufferIndex;
constexpr uint32_t kRxBufferPad = 16;
constexpr uint32_t kRxBufferWrapPad = 2048;
constexpr uint32_t kRxBufferTotalLength = kRxBufferLength + kRxBufferPad + kRxBufferWrapPad;

constexpr uint32_t kTxBufferCount = 4;
constexpr uint32_t kTxBufferLength = 2048;
constexpr uint32_t kFrameQueueLength = 8;
constexpr uint16_t kMaximumHardwareFrameLength = 1792;
constexpr uint16_t kMinimumTransmitFrameLength = 60;

struct rtl8139_runtime_state {
    bool present;
    bool initialized;
    bool link_up;
    uint16_t io_base;
    uint8_t mac[6];
    uint32_t current_receive_offset;
    uint32_t current_transmit_descriptor;
    uint32_t dirty_transmit_descriptor;
    uint32_t queued_frames;
    uint32_t queue_head;
    uint32_t queue_tail;
    eth_pci_device pci_device;
    rtl8139_stats stats;
};

alignas(16) static uint8_t g_receive_buffer[kRxBufferTotalLength];
alignas(16) static uint8_t g_transmit_buffers[kTxBufferCount][kTxBufferLength];
static rtl8139_raw_frame g_received_frames[kFrameQueueLength];
static rtl8139_runtime_state g_state = {};

static inline void compiler_barrier() {
    asm volatile ("" : : : "memory");
}

static inline uint16_t io_port(uint16_t register_offset) {
    return static_cast<uint16_t>(g_state.io_base + register_offset);
}

static inline uint8_t rtl8139_read8(uint16_t register_offset) {
    return inb(io_port(register_offset));
}

static inline uint16_t rtl8139_read16(uint16_t register_offset) {
    return inw(io_port(register_offset));
}

static inline uint32_t rtl8139_read32(uint16_t register_offset) {
    return inl(io_port(register_offset));
}

static inline void rtl8139_write8(uint16_t register_offset, uint8_t value) {
    outb(io_port(register_offset), value);
}

static inline void rtl8139_write16(uint16_t register_offset, uint16_t value) {
    outw(io_port(register_offset), value);
}

static inline void rtl8139_write32(uint16_t register_offset, uint32_t value) {
    outl(io_port(register_offset), value);
}

static bool physical_address(const void* address, uint32_t* out_address) {
    if (!address || !out_address) return false;
    welovec::x86_64::mm::paging::Mapping mapping{};
    if (!welovec::x86_64::mm::paging::translate(
            reinterpret_cast<uintptr_t>(address), &mapping) ||
        mapping.physical.value() > UINT32_MAX) {
        return false;
    }
    *out_address = static_cast<uint32_t>(mapping.physical.value());
    return true;
}

static void rtl8139_acknowledge_interrupts() {
    rtl8139_write16(kRegisterInterruptStatus, kInterruptAckMask);
}

static void rtl8139_prepare_power_state() {
    rtl8139_write8(kRegisterHaltClock, 'R');
    rtl8139_write8(kRegisterCfg9346, kCfg9346Unlock);

    uint8_t config1 = rtl8139_read8(kRegisterConfig1);
    config1 &= static_cast<uint8_t>(~(kConfig1Sleep | kConfig1PowerDown));
    config1 |= kConfig1DriverLoaded;
    rtl8139_write8(kRegisterConfig1, config1);

    rtl8139_write8(kRegisterCfg9346, kCfg9346Lock);
}

static bool rtl8139_reset_chip() {
    rtl8139_write8(kRegisterChipCommand, kChipCommandReset);

    for (uint32_t attempt = 0; attempt < 100000; ++attempt) {
        if ((rtl8139_read8(kRegisterChipCommand) & kChipCommandReset) == 0) {
            return true;
        }

        compiler_barrier();
    }

    return false;
}

static bool rtl8139_program_dma_buffers() {
    uint32_t receive_address = 0;
    uint32_t transmit_addresses[kTxBufferCount];
    if (!physical_address(g_receive_buffer, &receive_address)) return false;
    for (uint32_t index = 0; index < kTxBufferCount; ++index) {
        if (!physical_address(g_transmit_buffers[index], &transmit_addresses[index])) return false;
    }

    rtl8139_write32(kRegisterRxBuffer, receive_address);
    for (uint32_t index = 0; index < kTxBufferCount; ++index) {
        rtl8139_write32(
            static_cast<uint16_t>(kRegisterTxAddress0 + (index * sizeof(uint32_t))),
            transmit_addresses[index]
        );
    }
    return true;
}

static void rtl8139_initialize_software_state() {
    g_state.current_receive_offset = 0;
    g_state.current_transmit_descriptor = 0;
    g_state.dirty_transmit_descriptor = 0;
    g_state.queue_head = 0;
    g_state.queue_tail = 0;
    g_state.queued_frames = 0;
    memset(g_receive_buffer, 0, kRxBufferTotalLength);
    memset(g_transmit_buffers, 0, sizeof(g_transmit_buffers));
    memset(g_received_frames, 0, sizeof(g_received_frames));
}

static bool rtl8139_link_is_up() {
    return (rtl8139_read8(kRegisterMediaStatus) & kMediaStatusLinkFail) == 0;
}

static bool rtl8139_apply_runtime_configuration() {
    rtl8139_initialize_software_state();
    if (!rtl8139_program_dma_buffers()) return false;

    rtl8139_write32(kRegisterMar0 + 0, 0);
    rtl8139_write32(kRegisterMar0 + 4, 0);
    rtl8139_write16(kRegisterInterruptMask, 0);
    rtl8139_acknowledge_interrupts();
    rtl8139_write8(kRegisterChipCommand, kChipCommandReceiveEnable | kChipCommandTransmitEnable);
    rtl8139_write32(kRegisterRxConfig, kReceiveConfig);
    rtl8139_write32(kRegisterTxConfig, kTransmitConfig);
    rtl8139_write32(kRegisterRxMissed, 0);
    g_state.link_up = rtl8139_link_is_up();
    return true;
}

static void rtl8139_read_mac_address() {
    for (uint8_t index = 0; index < 6; ++index) {
        g_state.mac[index] = rtl8139_read8(static_cast<uint16_t>(kRegisterMac0 + index));
    }
}

static void rtl8139_collect_transmit_completions() {
    const uint16_t interrupt_status = rtl8139_read16(kRegisterInterruptStatus);
    if ((interrupt_status & (kInterruptTransmitError | kInterruptReceiveOverflow | kInterruptReceiveFifoOverflow)) != 0) {
        g_state.stats.rx_errors += (interrupt_status & (kInterruptReceiveOverflow | kInterruptReceiveFifoOverflow)) != 0 ? 1u : 0u;
    }

    while ((g_state.current_transmit_descriptor - g_state.dirty_transmit_descriptor) > 0) {
        const uint32_t entry = g_state.dirty_transmit_descriptor % kTxBufferCount;
        const uint32_t status = rtl8139_read32(static_cast<uint16_t>(kRegisterTxStatus0 + (entry * sizeof(uint32_t))));
        const uint32_t completion_bits =
            kTransmitStatusOk |
            kTransmitStatusUnderrun |
            kTransmitStatusAborted |
            kTransmitStatusOutOfWindow |
            kTransmitStatusCarrierLost;

        if ((status & completion_bits) == 0) {
            break;
        }

        if ((status & (kTransmitStatusAborted | kTransmitStatusOutOfWindow)) != 0) {
            ++g_state.stats.tx_errors;
        } else {
            if ((status & kTransmitStatusUnderrun) != 0) {
                ++g_state.stats.tx_errors;
            }
            ++g_state.stats.tx_packets;
        }

        ++g_state.dirty_transmit_descriptor;
    }

    rtl8139_acknowledge_interrupts();
}

static void rtl8139_recover_from_receive_error() {
    ++g_state.stats.resets;
    rtl8139_prepare_power_state();
    if (rtl8139_reset_chip()) {
        rtl8139_apply_runtime_configuration();
    } else {
        g_state.initialized = false;
    }
}

static void rtl8139_enqueue_frame(uint16_t status, uint16_t frame_length, const uint8_t* frame_start) {
    if (frame_length > RTL8139_MAX_FRAME_SIZE) {
        ++g_state.stats.rx_dropped;
        return;
    }

    if (g_state.queued_frames >= kFrameQueueLength) {
        ++g_state.stats.rx_dropped;
        return;
    }

    rtl8139_raw_frame& frame = g_received_frames[g_state.queue_tail];
    frame.length = frame_length;
    frame.status = status;
    memcpy(frame.bytes, frame_start, frame_length);

    g_state.queue_tail = (g_state.queue_tail + 1) % kFrameQueueLength;
    ++g_state.queued_frames;
    ++g_state.stats.rx_packets;
}

static void rtl8139_receive_packets() {
    while ((rtl8139_read8(kRegisterChipCommand) & kChipCommandReceiveBufferEmpty) == 0) {
        const uint32_t ring_offset = g_state.current_receive_offset % kRxBufferLength;
        const uint8_t* header = &g_receive_buffer[ring_offset];
        const uint16_t receive_status =
            static_cast<uint16_t>(header[0]) |
            static_cast<uint16_t>(header[1] << 8);
        const uint16_t receive_size =
            static_cast<uint16_t>(header[2]) |
            static_cast<uint16_t>(header[3] << 8);

        if (receive_size == 0xFFF0u) {
            break;
        }

        if (receive_size < 4 ||
            receive_size > static_cast<uint16_t>(kMaximumHardwareFrameLength + 4) ||
            (receive_status & kReceiveStatusOk) == 0) {
            ++g_state.stats.rx_errors;
            rtl8139_recover_from_receive_error();
            return;
        }

        const uint16_t frame_length = static_cast<uint16_t>(receive_size - 4);
        rtl8139_enqueue_frame(receive_status, frame_length, &g_receive_buffer[ring_offset + 4]);

        g_state.current_receive_offset =
            (g_state.current_receive_offset + receive_size + 4 + 3) & ~0x3u;
        rtl8139_write16(
            kRegisterCurrentAddressOfPacketRead,
            static_cast<uint16_t>(g_state.current_receive_offset - 16u)
        );
    }

    rtl8139_acknowledge_interrupts();
}

} // namespace

bool rtl8139_supports_device(const eth_pci_device& device) {
    for (uint8_t index = 0; index < kSupportedDeviceCount; ++index) {
        if (device.vendor_id == kSupportedVendorIds[index] &&
            device.device_id == kSupportedDeviceIds[index]) {
            return true;
        }
    }

    return false;
}

bool rtl8139_init_device(const eth_pci_device& device) {
    if (g_state.initialized) {
        return true;
    }

    if (!rtl8139_supports_device(device)) {
        g_state.present = false;
        return false;
    }

    memset(&g_state, 0, sizeof(g_state));
    eth_pci_device candidate = device;

    uint32_t io_base = 0;
    for (uint8_t bar_index = 0; bar_index < 6; ++bar_index) {
        io_base = eth_pci_get_io_bar_base(&candidate, bar_index);
        if (io_base != 0) {
            break;
        }
    }

    if (io_base == 0 || io_base > 0xFFFFu) {
        g_state.present = false;
        return false;
    }

    eth_pci_set_command_bits(&candidate, static_cast<uint16_t>(kPciCommandIo | kPciCommandBusMaster));

    g_state.present = true;
    g_state.io_base = static_cast<uint16_t>(io_base);
    g_state.pci_device = candidate;

    rtl8139_prepare_power_state();
    if (!rtl8139_reset_chip()) {
        g_state.present = false;
        g_state.initialized = false;
        return false;
    }

    rtl8139_read_mac_address();
    if (!rtl8139_apply_runtime_configuration()) {
        g_state.initialized = false;
        return false;
    }

    g_state.initialized = true;
    return true;
}

bool rtl8139_init() {
    if (g_state.initialized) {
        return true;
    }

    eth_pci_device candidate = {};
    for (uint8_t index = 0; index < kSupportedDeviceCount; ++index) {
        if (eth_pci_find_device(kSupportedVendorIds[index], kSupportedDeviceIds[index], &candidate) &&
            rtl8139_init_device(candidate)) {
            return true;
        }
    }

    g_state.present = false;
    return false;
}

bool rtl8139_is_ready() {
    return g_state.initialized;
}

void rtl8139_poll() {
    if (!g_state.initialized) {
        return;
    }

    rtl8139_collect_transmit_completions();
    rtl8139_receive_packets();
    g_state.link_up = rtl8139_link_is_up();
}

bool rtl8139_send_raw(const uint8_t* frame, uint16_t length) {
    if (!g_state.initialized ||
        frame == nullptr ||
        length == 0 ||
        length > kMaximumHardwareFrameLength ||
        !g_state.link_up) {
        return false;
    }

    rtl8139_collect_transmit_completions();

    if ((g_state.current_transmit_descriptor - g_state.dirty_transmit_descriptor) >= kTxBufferCount) {
        ++g_state.stats.tx_dropped;
        return false;
    }

    const uint32_t entry = g_state.current_transmit_descriptor % kTxBufferCount;
    uint16_t transmit_length = length;

    memcpy(g_transmit_buffers[entry], frame, length);
    if (transmit_length < kMinimumTransmitFrameLength) {
        memset(&g_transmit_buffers[entry][transmit_length], 0, kMinimumTransmitFrameLength - transmit_length);
        transmit_length = kMinimumTransmitFrameLength;
    }

    compiler_barrier();
    rtl8139_write32(
        static_cast<uint16_t>(kRegisterTxStatus0 + (entry * sizeof(uint32_t))),
        kTransmitFlags | transmit_length
    );

    ++g_state.current_transmit_descriptor;
    rtl8139_acknowledge_interrupts();
    return true;
}

bool rtl8139_receive_raw(rtl8139_raw_frame* out_frame) {
    if (out_frame == nullptr || g_state.queued_frames == 0) {
        return false;
    }

    *out_frame = g_received_frames[g_state.queue_head];
    g_state.queue_head = (g_state.queue_head + 1) % kFrameQueueLength;
    --g_state.queued_frames;
    return true;
}

void rtl8139_get_info(rtl8139_info* out_info) {
    if (out_info == nullptr) {
        return;
    }

    memset(out_info, 0, sizeof(rtl8139_info));
    out_info->present = g_state.present;
    out_info->initialized = g_state.initialized;
    out_info->link_up = g_state.link_up;
    out_info->io_base = g_state.io_base;
    out_info->queued_frames = g_state.queued_frames;
    out_info->pci_device = g_state.pci_device;
    out_info->stats = g_state.stats;
    memcpy(out_info->mac, g_state.mac, sizeof(g_state.mac));
}
