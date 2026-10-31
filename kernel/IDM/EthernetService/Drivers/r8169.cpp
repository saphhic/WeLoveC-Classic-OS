// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the Realtek RTL8169 Ethernet driver.

//written by saphhic.

#include "r8169.h"
#include "../../../arch/x86_64/io/io.h"
#include "../../../arch/x86_64/mm/paging/page_table.hpp"
#include "../../../arch/x86_64/mm/physical/physical_memory.hpp"

extern "C" void* memcpy(void* destination, const void* source, unsigned long long size);
extern "C" void* memset(void* destination, int value, unsigned long long size);

namespace {

constexpr uint16_t kPciCommandIo = 0x0001;
constexpr uint16_t kPciCommandMemory = 0x0002;
constexpr uint16_t kPciCommandBusMaster = 0x0004;

constexpr uint8_t kSupportedDeviceCount = 3;
constexpr uint16_t kSupportedVendorIds[kSupportedDeviceCount] = {
    0x10EC, 0x10EC, 0x10EC
};
constexpr uint16_t kSupportedDeviceIds[kSupportedDeviceCount] = {
    0x8169, 0x8168, 0x8161
};

constexpr uint16_t kRegisterMac0 = 0x00;
constexpr uint16_t kRegisterMar0 = 0x08;
constexpr uint16_t kRegisterTxDescStartAddrLow = 0x20;
constexpr uint16_t kRegisterTxDescStartAddrHigh = 0x24;
constexpr uint16_t kRegisterTxHighDescStartAddrLow = 0x28;
constexpr uint16_t kRegisterTxHighDescStartAddrHigh = 0x2C;
constexpr uint16_t kRegisterChipCommand = 0x37;
constexpr uint16_t kRegisterTxPoll = 0x38;
constexpr uint16_t kRegisterInterruptMask = 0x3C;
constexpr uint16_t kRegisterInterruptStatus = 0x3E;
constexpr uint16_t kRegisterTxConfig = 0x40;
constexpr uint16_t kRegisterRxConfig = 0x44;
constexpr uint16_t kRegisterRxMissed = 0x4C;
constexpr uint16_t kRegisterCfg9346 = 0x50;
constexpr uint16_t kRegisterConfig1 = 0x52;
constexpr uint16_t kRegisterMultiInterrupt = 0x5C;
constexpr uint16_t kRegisterPhyStatus = 0x6C;
constexpr uint16_t kRegisterRxMaxSize = 0xDA;
constexpr uint16_t kRegisterRxDescStartAddrLow = 0xE4;
constexpr uint16_t kRegisterRxDescStartAddrHigh = 0xE8;
constexpr uint16_t kRegisterEarlyTxThreshold = 0xEC;

constexpr uint8_t kCfg9346Lock = 0x00;
constexpr uint8_t kCfg9346Unlock = 0xC0;
constexpr uint8_t kConfig1Sleep = 0x02;
constexpr uint8_t kConfig1PowerDown = 0x01;

constexpr uint8_t kChipCommandReset = 0x10;
constexpr uint8_t kChipCommandReceiveEnable = 0x08;
constexpr uint8_t kChipCommandTransmitEnable = 0x04;

constexpr uint8_t kTxPollNormalPriority = 0x40;

constexpr uint16_t kInterruptSystemError = 0x8000;
constexpr uint16_t kInterruptPcsTimeout = 0x4000;
constexpr uint16_t kInterruptSoftware = 0x0100;
constexpr uint16_t kInterruptTxDescUnavailable = 0x0080;
constexpr uint16_t kInterruptRxFifoOverflow = 0x0040;
constexpr uint16_t kInterruptRxUnderrun = 0x0020;
constexpr uint16_t kInterruptRxOverflow = 0x0010;
constexpr uint16_t kInterruptTxError = 0x0008;
constexpr uint16_t kInterruptTxOk = 0x0004;
constexpr uint16_t kInterruptRxError = 0x0002;
constexpr uint16_t kInterruptRxOk = 0x0001;
constexpr uint16_t kInterruptAckMask =
    kInterruptSystemError |
    kInterruptPcsTimeout |
    kInterruptSoftware |
    kInterruptTxDescUnavailable |
    kInterruptRxFifoOverflow |
    kInterruptRxUnderrun |
    kInterruptRxOverflow |
    kInterruptTxError |
    kInterruptTxOk |
    kInterruptRxError |
    kInterruptRxOk;

constexpr uint32_t kReceiveAcceptBroadcast = 0x08;
constexpr uint32_t kReceiveAcceptMulticast = 0x04;
constexpr uint32_t kReceiveAcceptMyPhysicalAddress = 0x02;
constexpr uint32_t kReceiveFifoThresholdShift = 13;
constexpr uint32_t kReceiveDmaShift = 8;
constexpr uint32_t kReceiveConfig =
    (7u << kReceiveFifoThresholdShift) |
    (6u << kReceiveDmaShift) |
    kReceiveAcceptBroadcast |
    kReceiveAcceptMulticast |
    kReceiveAcceptMyPhysicalAddress;
constexpr uint32_t kReceiveConfigReservedMask = 0xFF7E1880u;

constexpr uint32_t kTransmitInterframeGapShift = 24;
constexpr uint32_t kTransmitDmaShift = 8;
constexpr uint32_t kTransmitConfig =
    (3u << kTransmitInterframeGapShift) |
    (6u << kTransmitDmaShift);

constexpr uint8_t kEarlyTxThresholdNoEarlyTx = 0x3F;
constexpr uint16_t kRxMaxSize = 0x0800;
constexpr uint8_t kPhyStatusLinkUp = 0x02;

constexpr uint32_t kDescriptorOwn = 0x80000000u;
constexpr uint32_t kDescriptorEndOfRing = 0x40000000u;
constexpr uint32_t kDescriptorFirstSegment = 0x20000000u;
constexpr uint32_t kDescriptorLastSegment = 0x10000000u;
constexpr uint32_t kReceiveDescriptorError = 0x00200000u;
constexpr uint32_t kReceiveDescriptorCrcError = 0x00080000u;
constexpr uint32_t kReceiveDescriptorRunt = 0x00100000u;
constexpr uint32_t kReceiveDescriptorWatchdog = 0x00400000u;
constexpr uint32_t kDescriptorLengthMask = 0x00001FFFu;

constexpr uint32_t kRxDescriptorCount = 8;
constexpr uint32_t kTxDescriptorCount = 4;
constexpr uint32_t kRxBufferSize = 2048;
constexpr uint32_t kTxBufferSize = 2048;
constexpr uint32_t kFrameQueueLength = 8;
constexpr uint16_t kMaximumHardwareFrameLength = 1600;
constexpr uint16_t kMinimumTransmitFrameLength = 60;

struct r8169_descriptor {
    volatile uint32_t status;
    volatile uint32_t vlan_tag;
    volatile uint32_t buffer_address_low;
    volatile uint32_t buffer_address_high;
};

static_assert(sizeof(r8169_descriptor) == 16, "r8169 descriptors must be 16 bytes");

struct r8169_runtime_state {
    bool present;
    bool initialized;
    bool link_up;
    bool using_mmio;
    uint16_t io_base;
    uint32_t mmio_base;
    uint64_t mmio_virtual_base;
    uint8_t mac[6];
    uint32_t current_receive_descriptor;
    uint32_t current_transmit_descriptor;
    uint32_t dirty_transmit_descriptor;
    uint32_t queued_frames;
    uint32_t queue_head;
    uint32_t queue_tail;
    eth_pci_device pci_device;
    r8169_stats stats;
};

alignas(256) static r8169_descriptor g_receive_descriptors[kRxDescriptorCount];
alignas(256) static r8169_descriptor g_transmit_descriptors[kTxDescriptorCount];
alignas(256) static uint8_t g_receive_buffers[kRxDescriptorCount][kRxBufferSize];
alignas(256) static uint8_t g_transmit_buffers[kTxDescriptorCount][kTxBufferSize];
static r8169_raw_frame g_received_frames[kFrameQueueLength];
static r8169_runtime_state g_state = {};

static inline void compiler_barrier() {
    asm volatile ("" : : : "memory");
}

static inline uint16_t io_port(uint16_t register_offset) {
    return static_cast<uint16_t>(g_state.io_base + register_offset);
}

static inline volatile uint8_t* mmio8(uint16_t register_offset) {
    return reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(g_state.mmio_virtual_base + register_offset));
}

static inline volatile uint16_t* mmio16(uint16_t register_offset) {
    return reinterpret_cast<volatile uint16_t*>(static_cast<uintptr_t>(g_state.mmio_virtual_base + register_offset));
}

static inline volatile uint32_t* mmio32(uint16_t register_offset) {
    return reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(g_state.mmio_virtual_base + register_offset));
}

static inline uint8_t r8169_read8(uint16_t register_offset) {
    if (g_state.using_mmio) {
        return *mmio8(register_offset);
    }

    return inb(io_port(register_offset));
}

static inline uint16_t r8169_read16(uint16_t register_offset) {
    if (g_state.using_mmio) {
        return *mmio16(register_offset);
    }

    return inw(io_port(register_offset));
}

static inline uint32_t r8169_read32(uint16_t register_offset) {
    if (g_state.using_mmio) {
        return *mmio32(register_offset);
    }

    return inl(io_port(register_offset));
}

static inline void r8169_write8(uint16_t register_offset, uint8_t value) {
    if (g_state.using_mmio) {
        *mmio8(register_offset) = value;
        return;
    }

    outb(io_port(register_offset), value);
}

static inline void r8169_write16(uint16_t register_offset, uint16_t value) {
    if (g_state.using_mmio) {
        *mmio16(register_offset) = value;
        return;
    }

    outw(io_port(register_offset), value);
}

static inline void r8169_write32(uint16_t register_offset, uint32_t value) {
    if (g_state.using_mmio) {
        *mmio32(register_offset) = value;
        return;
    }

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

static bool map_mmio_bar(uint32_t physical_base, uint64_t* out_virtual_base) {
    using namespace welovec::x86_64::mm;
    using namespace welovec::x86_64::mm::paging;
    using namespace welovec::x86_64::mm::physical;

    if (!out_virtual_base) return false;
    const PhysicalAddress physical_page = PhysicalAddress(physical_base).align_down(kPageSize);
    if (!contains(physical_page)) return false;
    const uint64_t virtual_page = kDirectMapBase + physical_page.value();
    Mapping existing{};
    if (translate(virtual_page, &existing)) {
        constexpr uint64_t kCacheFlags = (UINT64_C(1) << 3) | (UINT64_C(1) << 4);
        if (existing.physical.value() != physical_page.value() ||
            (existing.raw_flags & kCacheFlags) != kCacheFlags) {
            return false;
        }
    } else {
        PageFlags flags = PageFlags::Writable | PageFlags::WriteThrough | PageFlags::CacheDisable;
        if (nx_enabled()) flags = flags | PageFlags::NoExecute;
        if (!map_page(virtual_page, physical_page, flags)) return false;
    }

    *out_virtual_base = kDirectMapBase + physical_base;
    return true;
}

static void r8169_acknowledge_interrupts() {
    r8169_write16(kRegisterInterruptStatus, kInterruptAckMask);
}

static void r8169_prepare_power_state() {
    r8169_write8(kRegisterCfg9346, kCfg9346Unlock);

    uint8_t config1 = r8169_read8(kRegisterConfig1);
    config1 &= static_cast<uint8_t>(~(kConfig1Sleep | kConfig1PowerDown));
    r8169_write8(kRegisterConfig1, config1);

    r8169_write8(kRegisterCfg9346, kCfg9346Lock);
}

static bool r8169_reset_chip() {
    r8169_write8(kRegisterChipCommand, kChipCommandReset);

    for (uint32_t attempt = 0; attempt < 100000; ++attempt) {
        if ((r8169_read8(kRegisterChipCommand) & kChipCommandReset) == 0) {
            return true;
        }

        compiler_barrier();
    }

    return false;
}

static bool r8169_initialize_software_state() {
    g_state.current_receive_descriptor = 0;
    g_state.current_transmit_descriptor = 0;
    g_state.dirty_transmit_descriptor = 0;
    g_state.queue_head = 0;
    g_state.queue_tail = 0;
    g_state.queued_frames = 0;

    memset(g_receive_descriptors, 0, sizeof(g_receive_descriptors));
    memset(g_transmit_descriptors, 0, sizeof(g_transmit_descriptors));
    memset(g_receive_buffers, 0, sizeof(g_receive_buffers));
    memset(g_transmit_buffers, 0, sizeof(g_transmit_buffers));
    memset(g_received_frames, 0, sizeof(g_received_frames));

    for (uint32_t index = 0; index < kRxDescriptorCount; ++index) {
        uint32_t buffer_address = 0;
        if (!physical_address(g_receive_buffers[index], &buffer_address)) return false;
        const uint32_t end_of_ring = (index == (kRxDescriptorCount - 1)) ? kDescriptorEndOfRing : 0;
        g_receive_descriptors[index].vlan_tag = 0;
        g_receive_descriptors[index].buffer_address_low = buffer_address;
        g_receive_descriptors[index].buffer_address_high = 0;
        g_receive_descriptors[index].status = kDescriptorOwn | end_of_ring | kRxBufferSize;
    }

    for (uint32_t index = 0; index < kTxDescriptorCount; ++index) {
        uint32_t buffer_address = 0;
        if (!physical_address(g_transmit_buffers[index], &buffer_address)) return false;
        g_transmit_descriptors[index].vlan_tag = 0;
        g_transmit_descriptors[index].buffer_address_low = buffer_address;
        g_transmit_descriptors[index].buffer_address_high = 0;
        g_transmit_descriptors[index].status =
            (index == (kTxDescriptorCount - 1)) ? kDescriptorEndOfRing : 0;
    }
    return true;
}

static bool r8169_link_is_up() {
    return (r8169_read8(kRegisterPhyStatus) & kPhyStatusLinkUp) != 0;
}

static bool r8169_program_descriptor_rings() {
    uint32_t transmit_address = 0;
    uint32_t receive_address = 0;
    if (!physical_address(g_transmit_descriptors, &transmit_address) ||
        !physical_address(g_receive_descriptors, &receive_address)) return false;

    r8169_write32(kRegisterTxDescStartAddrLow, transmit_address);
    r8169_write32(kRegisterTxDescStartAddrHigh, 0);
    r8169_write32(kRegisterTxHighDescStartAddrLow, 0);
    r8169_write32(kRegisterTxHighDescStartAddrHigh, 0);
    r8169_write32(kRegisterRxDescStartAddrLow, receive_address);
    r8169_write32(kRegisterRxDescStartAddrHigh, 0);
    return true;
}

static bool r8169_apply_runtime_configuration() {
    if (!r8169_initialize_software_state()) return false;
    compiler_barrier();

    r8169_write16(kRegisterInterruptMask, 0);
    r8169_acknowledge_interrupts();

    r8169_write8(kRegisterCfg9346, kCfg9346Unlock);
    r8169_write8(kRegisterEarlyTxThreshold, kEarlyTxThresholdNoEarlyTx);
    r8169_write16(kRegisterRxMaxSize, kRxMaxSize);
    if (!r8169_program_descriptor_rings()) return false;

    const uint32_t receive_config = kReceiveConfig | (r8169_read32(kRegisterRxConfig) & kReceiveConfigReservedMask);

    r8169_write32(kRegisterMar0 + 0, 0xFFFFFFFFu);
    r8169_write32(kRegisterMar0 + 4, 0xFFFFFFFFu);
    r8169_write32(kRegisterRxConfig, receive_config);
    r8169_write32(kRegisterTxConfig, kTransmitConfig);
    r8169_write8(kRegisterChipCommand, kChipCommandTransmitEnable | kChipCommandReceiveEnable);
    r8169_write8(kRegisterCfg9346, kCfg9346Lock);

    r8169_write32(kRegisterRxMissed, 0);
    r8169_write16(kRegisterMultiInterrupt, static_cast<uint16_t>(r8169_read16(kRegisterMultiInterrupt) & 0xF000u));
    r8169_acknowledge_interrupts();

    g_state.link_up = r8169_link_is_up();
    return true;
}

static void r8169_read_mac_address() {
    for (uint8_t index = 0; index < 6; ++index) {
        g_state.mac[index] = r8169_read8(static_cast<uint16_t>(kRegisterMac0 + index));
    }
}

static void r8169_collect_transmit_completions() {
    const uint16_t interrupt_status = r8169_read16(kRegisterInterruptStatus);
    if ((interrupt_status & kInterruptTxError) != 0) {
        ++g_state.stats.tx_errors;
    }

    if ((interrupt_status & (kInterruptRxError | kInterruptRxOverflow | kInterruptRxFifoOverflow)) != 0) {
        ++g_state.stats.rx_errors;
    }

    while ((g_state.current_transmit_descriptor - g_state.dirty_transmit_descriptor) > 0) {
        const uint32_t entry = g_state.dirty_transmit_descriptor % kTxDescriptorCount;
        const uint32_t status = g_transmit_descriptors[entry].status;
        if ((status & kDescriptorOwn) != 0) {
            break;
        }

        if ((interrupt_status & kInterruptTxError) != 0) {
            ++g_state.stats.tx_errors;
        } else {
            ++g_state.stats.tx_packets;
        }

        ++g_state.dirty_transmit_descriptor;
    }
}

static bool r8169_requeue_receive_descriptor(uint32_t entry) {
    uint32_t buffer_address = 0;
    if (!physical_address(g_receive_buffers[entry], &buffer_address)) return false;
    const uint32_t end_of_ring = (entry == (kRxDescriptorCount - 1)) ? kDescriptorEndOfRing : 0;
    g_receive_descriptors[entry].vlan_tag = 0;
    g_receive_descriptors[entry].buffer_address_low = buffer_address;
    g_receive_descriptors[entry].buffer_address_high = 0;
    compiler_barrier();
    g_receive_descriptors[entry].status = kDescriptorOwn | end_of_ring | kRxBufferSize;
    compiler_barrier();
    return true;
}

static void r8169_enqueue_frame(uint32_t status, uint16_t frame_length, const uint8_t* frame_start) {
    if (frame_length > R8169_MAX_FRAME_SIZE) {
        ++g_state.stats.rx_dropped;
        return;
    }

    if (g_state.queued_frames >= kFrameQueueLength) {
        ++g_state.stats.rx_dropped;
        return;
    }

    r8169_raw_frame& frame = g_received_frames[g_state.queue_tail];
    frame.length = frame_length;
    frame.status = status;
    memcpy(frame.bytes, frame_start, frame_length);

    g_state.queue_tail = (g_state.queue_tail + 1) % kFrameQueueLength;
    ++g_state.queued_frames;
    ++g_state.stats.rx_packets;
}

static bool r8169_receive_packets() {
    for (uint32_t budget = 0; budget < kRxDescriptorCount; ++budget) {
        const uint32_t entry = g_state.current_receive_descriptor % kRxDescriptorCount;
        const uint32_t status = g_receive_descriptors[entry].status;
        if ((status & kDescriptorOwn) != 0) {
            break;
        }

        const bool descriptor_error =
            (status & (kReceiveDescriptorError | kReceiveDescriptorCrcError | kReceiveDescriptorRunt | kReceiveDescriptorWatchdog)) != 0;
        const bool whole_frame =
            (status & (kDescriptorFirstSegment | kDescriptorLastSegment)) ==
            (kDescriptorFirstSegment | kDescriptorLastSegment);
        const uint32_t hardware_length = status & kDescriptorLengthMask;

        if (descriptor_error || !whole_frame || hardware_length < 4 || hardware_length > kRxBufferSize) {
            ++g_state.stats.rx_errors;
        } else {
            const uint16_t frame_length = static_cast<uint16_t>(hardware_length - 4);
            r8169_enqueue_frame(status, frame_length, g_receive_buffers[entry]);
        }

        if (!r8169_requeue_receive_descriptor(entry)) return false;
        g_state.current_receive_descriptor = (g_state.current_receive_descriptor + 1) % kRxDescriptorCount;
    }
    return true;
}

} // namespace

bool r8169_supports_device(const eth_pci_device& device) {
    for (uint8_t index = 0; index < kSupportedDeviceCount; ++index) {
        if (device.vendor_id == kSupportedVendorIds[index] &&
            device.device_id == kSupportedDeviceIds[index]) {
            return true;
        }
    }

    return false;
}

bool r8169_init_device(const eth_pci_device& device) {
    if (g_state.initialized) {
        return true;
    }

    if (!r8169_supports_device(device)) {
        g_state.present = false;
        return false;
    }

    memset(&g_state, 0, sizeof(g_state));
    eth_pci_device candidate = device;

    uint32_t mmio_base = 0;
    for (uint8_t bar_index = 0; bar_index < 6; ++bar_index) {
        mmio_base = eth_pci_get_memory_bar_base(&candidate, bar_index);
        if (mmio_base != 0) {
            break;
        }
    }

    uint32_t io_base = 0;
    for (uint8_t bar_index = 0; bar_index < 6; ++bar_index) {
        io_base = eth_pci_get_io_bar_base(&candidate, bar_index);
        if (io_base != 0) {
            break;
        }
    }

    if (mmio_base == 0 && (io_base == 0 || io_base > 0xFFFFu)) {
        g_state.present = false;
        return false;
    }

    uint64_t mmio_virtual_base = 0;
    const bool use_mmio = mmio_base != 0 && map_mmio_bar(mmio_base, &mmio_virtual_base);
    if (!use_mmio && (io_base == 0 || io_base > 0xFFFFu)) {
        g_state.present = false;
        return false;
    }

    uint16_t command_bits = kPciCommandBusMaster;
    if (use_mmio) {
        command_bits = static_cast<uint16_t>(command_bits | kPciCommandMemory);
    }
    if (io_base != 0 && io_base <= 0xFFFFu) {
        command_bits = static_cast<uint16_t>(command_bits | kPciCommandIo);
    }
    eth_pci_set_command_bits(&candidate, command_bits);

    g_state.present = true;
    g_state.using_mmio = use_mmio;
    g_state.mmio_base = use_mmio ? mmio_base : 0;
    g_state.mmio_virtual_base = mmio_virtual_base;
    g_state.io_base = static_cast<uint16_t>((io_base <= 0xFFFFu) ? io_base : 0);
    g_state.pci_device = candidate;

    r8169_prepare_power_state();
    if (!r8169_reset_chip()) {
        g_state.present = false;
        g_state.initialized = false;
        return false;
    }

    r8169_read_mac_address();
    if (!r8169_apply_runtime_configuration()) {
        g_state.initialized = false;
        return false;
    }

    g_state.initialized = true;
    return true;
}

bool r8169_init() {
    if (g_state.initialized) {
        return true;
    }

    eth_pci_device candidate = {};
    for (uint8_t index = 0; index < kSupportedDeviceCount; ++index) {
        if (eth_pci_find_device(kSupportedVendorIds[index], kSupportedDeviceIds[index], &candidate) &&
            r8169_init_device(candidate)) {
            return true;
        }
    }

    g_state.present = false;
    return false;
}

bool r8169_is_ready() {
    return g_state.initialized;
}

void r8169_poll() {
    if (!g_state.initialized) {
        return;
    }

    r8169_collect_transmit_completions();
    if (!r8169_receive_packets()) {
        g_state.initialized = false;
        return;
    }
    r8169_acknowledge_interrupts();
    g_state.link_up = r8169_link_is_up();
}

bool r8169_send_raw(const uint8_t* frame, uint16_t length) {
    if (!g_state.initialized ||
        frame == nullptr ||
        length == 0 ||
        length > kMaximumHardwareFrameLength ||
        !g_state.link_up) {
        return false;
    }

    r8169_collect_transmit_completions();

    if ((g_state.current_transmit_descriptor - g_state.dirty_transmit_descriptor) >= kTxDescriptorCount) {
        ++g_state.stats.tx_dropped;
        return false;
    }

    const uint32_t entry = g_state.current_transmit_descriptor % kTxDescriptorCount;
    if ((g_transmit_descriptors[entry].status & kDescriptorOwn) != 0) {
        ++g_state.stats.tx_dropped;
        return false;
    }

    uint16_t transmit_length = length;
    memcpy(g_transmit_buffers[entry], frame, length);
    if (transmit_length < kMinimumTransmitFrameLength) {
        memset(&g_transmit_buffers[entry][transmit_length], 0, kMinimumTransmitFrameLength - transmit_length);
        transmit_length = kMinimumTransmitFrameLength;
    }

    uint32_t buffer_address = 0;
    if (!physical_address(g_transmit_buffers[entry], &buffer_address)) return false;
    g_transmit_descriptors[entry].buffer_address_low = buffer_address;
    g_transmit_descriptors[entry].buffer_address_high = 0;
    g_transmit_descriptors[entry].vlan_tag = 0;

    const uint32_t end_of_ring = (entry == (kTxDescriptorCount - 1)) ? kDescriptorEndOfRing : 0;
    compiler_barrier();
    g_transmit_descriptors[entry].status =
        kDescriptorOwn |
        end_of_ring |
        kDescriptorFirstSegment |
        kDescriptorLastSegment |
        transmit_length;
    compiler_barrier();

    r8169_write8(kRegisterTxPoll, kTxPollNormalPriority);
    ++g_state.current_transmit_descriptor;
    return true;
}

bool r8169_receive_raw(r8169_raw_frame* out_frame) {
    if (out_frame == nullptr || g_state.queued_frames == 0) {
        return false;
    }

    *out_frame = g_received_frames[g_state.queue_head];
    g_state.queue_head = (g_state.queue_head + 1) % kFrameQueueLength;
    --g_state.queued_frames;
    return true;
}

void r8169_get_info(r8169_info* out_info) {
    if (out_info == nullptr) {
        return;
    }

    memset(out_info, 0, sizeof(r8169_info));
    out_info->present = g_state.present;
    out_info->initialized = g_state.initialized;
    out_info->link_up = g_state.link_up;
    out_info->using_mmio = g_state.using_mmio;
    out_info->io_base = g_state.io_base;
    out_info->mmio_base = g_state.mmio_base;
    out_info->queued_frames = g_state.queued_frames;
    out_info->pci_device = g_state.pci_device;
    out_info->stats = g_state.stats;
    memcpy(out_info->mac, g_state.mac, sizeof(g_state.mac));
}
