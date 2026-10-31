// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements xHCI controller discovery and USB HID input.

#include "xhci.hpp"

#include "keyboard.hpp"
#include "mouse.hpp"
#include "hid_touchpad.hpp"
#include "arch/x86_64/mm/dma/dma.hpp"
#include "arch/common/MM/VMM/vmm.hpp"
#include "arch/x86_64/cpu/barrier.h"
#include "arch/x86_64/pci/pci.hpp"
#include "arch/x86_64/scheduler/scheduler.hpp"
#include "arch/x86_64/io/console.h"

namespace welovec::x86_64::input::usb {

namespace {
constexpr uint8_t kPciClassSerialBus = 0x0c;
constexpr uint8_t kPciSubclassUsb = 0x03;
constexpr uint8_t kPciProgIfXhci = 0x30;
constexpr uint32_t kMmioWindowSize = 0x10000;
constexpr uint32_t kRingEntries = 256;
// HCSParams2 encodes up to 1023 scratchpad buffers. Keep enough bookkeeping
// for every value the controller can report; a smaller array made failure
// cleanup walk past the end of Controller on some real xHCI hardware.
constexpr uint32_t kMaximumScratchpads = 1024;
constexpr uint32_t kMaximumHidDevices = 8;
constexpr uint32_t kMaximumHidInterfaces = 8;
constexpr uint32_t kBootKeyboardReportSize = 8;
constexpr uint32_t kBootMouseReportSize = 3;
constexpr uint32_t kMaximumReportDescriptorSize = 4096;
constexpr uint32_t kWaitIterations = 10000000;
constexpr uint64_t kSchedulerWaitSpinLimit = 100000000;
constexpr uint32_t kXhciEventCommandCompletion = 33;
constexpr uint32_t kXhciEventTransfer = 32;
constexpr uint32_t kXhciEventPortStatus = 34;
constexpr uint32_t kXhciCompletionSuccess = 1;
constexpr uint32_t kXhciCompletionShortPacket = 13;
constexpr uint32_t kPortConnected = 1u << 0;
constexpr uint32_t kPortEnabled = 1u << 1;
constexpr uint32_t kPortPowered = 1u << 9;
constexpr uint32_t kPortReset = 1u << 4;
constexpr uint32_t kPortWarmReset = 1u << 31;
constexpr uint32_t kPortSpeedMask = 0xfu << 10;
constexpr uint32_t kPortColdAttachStatus = 1u << 24;
constexpr uint32_t kXhciCapabilityLegacySupport = 1;
constexpr uint32_t kXhciCapabilitySupportedProtocol = 2;
constexpr uint8_t kPortProtocolUnknown = 0;
constexpr uint8_t kPortProtocolUsb2 = 2;
constexpr uint8_t kPortProtocolUsb3 = 3;
constexpr uint32_t kRunStop = 1u << 0;
constexpr uint32_t kHostControllerReset = 1u << 1;
constexpr uint32_t kControllerHalted = 1u << 0;
constexpr uint32_t kControllerNotReady = 1u << 11;
constexpr uint32_t kTrbCycle = 1u << 0;
constexpr uint32_t kTrbToggleCycle = 1u << 1;
constexpr uint32_t kTrbChain = 1u << 4;
constexpr uint32_t kTrbInterruptOnCompletion = 1u << 5;
constexpr uint32_t kTrbImmediateData = 1u << 6;
constexpr uint32_t kTrbDirectionIn = 1u << 16;
constexpr uint32_t kTrbLink = 6;
constexpr uint32_t kTrbNormal = 1;
constexpr uint32_t kTrbSetupStage = 2;
constexpr uint32_t kTrbDataStage = 3;
constexpr uint32_t kTrbStatusStage = 4;
constexpr uint32_t kTrbEnableSlot = 9;
constexpr uint32_t kTrbDisableSlot = 10;
constexpr uint32_t kTrbAddressDevice = 11;
constexpr uint32_t kTrbConfigureEndpoint = 12;
constexpr uint32_t kTrbEvaluateContext = 13;
constexpr uint32_t kTrbResetEndpoint = 14;

struct Trb { uint64_t parameter; uint32_t status; uint32_t control; };
struct EventSegmentTableEntry { uint64_t address; uint32_t size; uint32_t reserved; };
struct Event {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
};
struct Ring {
    welovec::x86_64::mm::dma::Region memory;
    uint16_t enqueue;
    uint8_t cycle;
};

struct Mapping { void *address; uint64_t size; };
struct Controller;
enum class HidKind : uint8_t { Keyboard, BootMouse, Touchpad };
enum class ControlStage : uint8_t { Unknown, Setup, Data, Status };
struct ControlFailure {
    uint8_t request;
    uint8_t port;
    uint8_t speed;
    uint8_t completion_code;
    ControlStage stage;
    bool valid;
    bool completion_seen;
};
struct HidDevice {
    Controller *controller;
    uint8_t slot;
    uint8_t port;
    uint8_t interface_count;
    Ring control_ring;
    welovec::x86_64::mm::dma::Region device_context;
    welovec::x86_64::mm::dma::Region input_context;
    welovec::x86_64::mm::dma::Region control_buffer;
    struct Interface {
        welovec::x86_64::keyboard::UsbBootKeyboardState report_state;
        welovec::x86_64::input::hid_touchpad::State touchpad_state;
        uint8_t endpoint_id;
        uint8_t previous_buttons;
        bool first_interrupt_logged;
        uint16_t report_size;
        uint8_t interface_number;
        HidKind kind;
        Ring interrupt_ring;
        welovec::x86_64::mm::dma::Region report_buffer;
    } interfaces[kMaximumHidInterfaces];
};
using HidInterface = HidDevice::Interface;
struct InterfaceCandidate {
    HidKind kind;
    uint8_t interface_number;
    uint8_t endpoint_address;
    uint8_t max_burst;
    uint8_t interval;
    uint16_t max_packet;
    uint16_t report_descriptor_size;
    uint32_t max_esit_payload;
};

struct Controller {
    welovec::x86_64::pci::Device pci;
    volatile uint8_t *mmio;
    uint64_t mmio_physical;
    uint8_t *capability;
    uint32_t operational_offset;
    uint32_t runtime_offset;
    uint32_t doorbell_offset;
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t context_size;
    uint8_t port_protocol[256];
    uint8_t port_slot_type[256];
    Mapping mmio_mapping;
    welovec::x86_64::mm::dma::Region device_context_array;
    welovec::x86_64::mm::dma::Region scratchpad_array;
    welovec::x86_64::mm::dma::Region scratchpads[kMaximumScratchpads];
    uint32_t scratchpad_count;
    Ring command_ring;
    welovec::x86_64::mm::dma::Region event_ring;
    welovec::x86_64::mm::dma::Region erst;
    uint16_t event_index;
    uint8_t event_cycle;
    HidDevice devices[kMaximumHidDevices];
    uint32_t device_count;
    bool active;
};

enum class ProbeStatus : uint8_t {
    NotStarted,
    Probing,
    NoController,
    ControllerUnavailable,
    NoConnectedHidDevice,
    PortResetFailed,
    DeviceSlotUnavailable,
    SlotEnableFailed,
    DeviceMemoryUnavailable,
    InvalidSlotId,
    AddressDeviceFailed,
    UsbDescriptorUnavailable,
    HidDeviceUnavailable,
    HidConfigurationUnavailable,
    HidReportUnavailable,
    HidEndpointUnavailable,
    WorkerUnavailable,
    Ready
};

Controller gControllers[2]{};
uint32_t gControllerCount;
bool gInitialized;
bool gProbeQueued;
bool gWorkerStarted;
bool gHasKeyboard;
bool gHasPointer;
uint32_t gPciControllerCount;
uint32_t gFailedControllerCount;
uint32_t gConnectedPortCount;
ProbeStatus gProbeStatus = ProbeStatus::NotStarted;
ProbeStatus gLastEnumerationFailure = ProbeStatus::HidDeviceUnavailable;
uint8_t gLastCommandCompletionCode;
bool gLastCommandCompletionSeen;
bool gAddressDeviceRetryAllowed;
char gProbeStatusBuffer[128];
uint8_t gLastTransferCompletionCode;
bool gLastTransferCompletionSeen;
uint8_t gLastControlRequest;
uint8_t gLastControlPort;
uint8_t gLastControlSpeed;
bool gLastControlTransferFailed;
bool gSchedulerTickWaitFailed;
ControlStage gLastTransferStage = ControlStage::Unknown;
ControlFailure gFirstControlFailure{};

template <typename T>
__attribute__((noinline)) void clear_object(T *object) {
    if (!object) return;
    auto *bytes = reinterpret_cast<volatile uint8_t *>(object);
    for (uint64_t i = 0; i < sizeof(*object); ++i) bytes[i] = 0;
}

uint32_t read_mmio32(volatile uint8_t *base, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t *>(base + offset);
}
uint16_t read_le16(const uint8_t *bytes) {
    return static_cast<uint16_t>(bytes[0] |
        (static_cast<uint16_t>(bytes[1]) << 8));
}
void write_mmio32(volatile uint8_t *base, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
    mfence();
}
void write_mmio64(volatile uint8_t *base, uint32_t offset, uint64_t value) {
    *reinterpret_cast<volatile uint64_t *>(base + offset) = value;
    mfence();
}

bool wait_mask(volatile uint8_t *base, uint32_t offset,
               uint32_t mask, uint32_t expected) {
    for (uint32_t i = 0; i < kWaitIterations; ++i) {
        if ((read_mmio32(base, offset) & mask) == expected) return true;
        __asm__ volatile("pause");
    }
    return false;
}

bool wait_scheduler_ticks(uint64_t ticks) {
    if (gSchedulerTickWaitFailed) return false;
    const uint64_t start = welovec::x86_64::scheduler::tick_count();
    for (uint64_t spin = 0; spin < kSchedulerWaitSpinLimit; ++spin) {
        if (welovec::x86_64::scheduler::tick_count() - start >= ticks)
            return true;
        __asm__ volatile("pause");
    }
    gSchedulerTickWaitFailed = true;
    return false;
}

bool allocate_dma(uint64_t bytes, welovec::x86_64::mm::dma::Region *out) {
    return welovec::x86_64::mm::dma::allocate(bytes, out);
}

void release_ring(Ring *ring) {
    if (!ring) return;
    welovec::x86_64::mm::dma::release(&ring->memory);
    *ring = {};
}

void release_hid_device(HidDevice *device) {
    if (!device) return;
    release_ring(&device->control_ring);
    welovec::x86_64::mm::dma::release(&device->device_context);
    welovec::x86_64::mm::dma::release(&device->input_context);
    welovec::x86_64::mm::dma::release(&device->control_buffer);
    for (uint32_t i = 0; i < kMaximumHidInterfaces; ++i) {
        release_ring(&device->interfaces[i].interrupt_ring);
        welovec::x86_64::mm::dma::release(&device->interfaces[i].report_buffer);
    }
    clear_object(device);
}

uint64_t bar_address(const welovec::x86_64::pci::Device &device) {
    const uint64_t low = welovec::x86_64::pci::memory_bar_address(device, 0);
    return low;
}

bool acquire_mmio(Controller *controller) {
    const uint64_t physical = bar_address(controller->pci);
    if (!physical || physical > UINT64_MAX - kMmioWindowSize) return false;
    void *mapped = welovec::memory::vmm::map_physical(
        welovec::x86_64::mm::PhysicalAddress(physical), kMmioWindowSize,
        welovec::memory::vmm::CachePolicy::Uncacheable);
    if (!mapped) return false;
    controller->mmio = static_cast<volatile uint8_t *>(mapped);
    controller->mmio_physical = physical;
    controller->mmio_mapping = {mapped, kMmioWindowSize};
    const uint8_t cap_length = *reinterpret_cast<volatile uint8_t *>(controller->mmio);
    const uint16_t interface_version = *reinterpret_cast<volatile uint16_t *>(controller->mmio + 2);
    const uint32_t hcs1 = read_mmio32(controller->mmio, 4);
    const uint32_t hcc1 = read_mmio32(controller->mmio, 0x10);
    controller->max_slots = hcs1 & 0xff;
    controller->max_ports = hcs1 >> 24;
    controller->context_size = (hcc1 & (1u << 2)) ? 64 : 32;
    controller->operational_offset = cap_length;
    controller->doorbell_offset = read_mmio32(controller->mmio, 0x14) & ~3u;
    controller->runtime_offset = read_mmio32(controller->mmio, 0x18) & ~0x1fu;
    const uint64_t ports_end = static_cast<uint64_t>(controller->operational_offset) +
        0x400 + static_cast<uint64_t>(controller->max_ports) * 0x10;
    const uint64_t doorbells_end = static_cast<uint64_t>(controller->doorbell_offset) +
        (static_cast<uint64_t>(controller->max_slots) + 1) * sizeof(uint32_t);
    return cap_length >= 0x20 && interface_version >= 0x0090 &&
        controller->max_slots && controller->max_ports &&
        doorbells_end <= kMmioWindowSize &&
        controller->runtime_offset <= kMmioWindowSize - 0x40 &&
        ports_end <= kMmioWindowSize;
}

void discover_port_protocols(Controller *controller) {
    if (!controller || !controller->mmio) return;
    const uint32_t hcc1 = read_mmio32(controller->mmio, 0x10);
    uint32_t offset = ((hcc1 >> 16) & 0xffffu) * 4u;
    for (uint32_t count = 0; offset && count < 256; ++count) {
        if (offset > kMmioWindowSize - 4) return;
        const uint32_t header = read_mmio32(controller->mmio, offset);
        const uint8_t id = header & 0xffu;
        const uint8_t next = (header >> 8) & 0xffu;
        if (id == kXhciCapabilitySupportedProtocol &&
            offset <= kMmioWindowSize - 16) {
            const uint8_t major_revision = static_cast<uint8_t>(header >> 24);
            const uint32_t name = read_mmio32(controller->mmio, offset + 4);
            const uint32_t ports = read_mmio32(controller->mmio, offset + 8);
            const uint32_t slot_type = read_mmio32(controller->mmio, offset + 12) & 0x1fu;
            const uint32_t first_port = ports & 0xffu;
            const uint32_t port_count = (ports >> 8) & 0xffu;
            if (name == UINT32_C(0x20425355) && first_port && port_count &&
                first_port <= controller->max_ports &&
                port_count <= controller->max_ports - first_port + 1) {
                const uint8_t protocol = major_revision >= 3
                    ? kPortProtocolUsb3 : kPortProtocolUsb2;
                for (uint32_t port = first_port;
                     port < first_port + port_count; ++port) {
                    controller->port_protocol[port] = protocol;
                    controller->port_slot_type[port] =
                        static_cast<uint8_t>(slot_type);
                }
            }
        }
        if (!next) break;
        offset += static_cast<uint32_t>(next) * 4;
    }
}

bool hand_over_from_firmware(Controller *controller) {
    const uint32_t hcc1 = read_mmio32(controller->mmio, 0x10);
    uint32_t offset = ((hcc1 >> 16) & 0xffffu) * 4u;
    for (uint32_t count = 0; offset && count < 128; ++count) {
        if (offset + 4 > kMmioWindowSize) return false;
        const uint32_t header = read_mmio32(controller->mmio, offset);
        const uint8_t id = header & 0xff;
        const uint8_t next = (header >> 8) & 0xff;
        if (id == kXhciCapabilityLegacySupport) {
            // Legacy Support: request ownership and wait briefly for BIOS release.
            const uint32_t legacy = (header & 0xffffu) | (1u << 24);
            write_mmio32(controller->mmio, offset, legacy);
            for (uint32_t i = 0; i < kWaitIterations; ++i) {
                if (!(read_mmio32(controller->mmio, offset) & (1u << 16))) return true;
                __asm__ volatile("pause");
            }
            return false;
        }
        if (!next) break;
        offset += static_cast<uint32_t>(next) * 4;
    }
    return true;
}

bool allocate_controller_rings(Controller *controller) {
    if (!allocate_dma((controller->max_slots + 1u) * sizeof(uint64_t),
                      &controller->device_context_array) ||
        !allocate_dma(kRingEntries * sizeof(Trb), &controller->command_ring.memory) ||
        !allocate_dma(kRingEntries * sizeof(Event), &controller->event_ring) ||
        !allocate_dma(sizeof(EventSegmentTableEntry), &controller->erst)) return false;
    controller->command_ring.enqueue = 0;
    controller->command_ring.cycle = 1;
    controller->event_index = 0;
    controller->event_cycle = 1;

    const uint32_t hcs2 = read_mmio32(controller->mmio, 8);
    const uint32_t scratchpad_count = (((hcs2 >> 27) & 0x1fu) << 5) |
                                       ((hcs2 >> 21) & 0x1fu);
    if (scratchpad_count > kMaximumScratchpads) return false;
    if (scratchpad_count) {
        if (!allocate_dma(scratchpad_count * sizeof(uint64_t),
                          &controller->scratchpad_array)) return false;
        auto *pointers = reinterpret_cast<uint64_t *>(
            controller->scratchpad_array.virtual_address);
        for (uint32_t i = 0; i < scratchpad_count; ++i) {
            if (!allocate_dma(4096, &controller->scratchpads[i])) return false;
            pointers[i] = controller->scratchpads[i].physical.value();
            // Track only successfully allocated entries so partial failure
            // cleanup releases exactly the regions that exist.
            controller->scratchpad_count = i + 1;
        }
        reinterpret_cast<uint64_t *>(controller->device_context_array.virtual_address)[0] =
            controller->scratchpad_array.physical.value();
    }
    auto *erst = reinterpret_cast<EventSegmentTableEntry *>(controller->erst.virtual_address);
    erst[0] = {controller->event_ring.physical.value(), kRingEntries, 0};
    return true;
}

bool initialize_controller_registers(Controller *controller) {
    const uint32_t op = controller->operational_offset;
    write_mmio32(controller->mmio, op, read_mmio32(controller->mmio, op) & ~kRunStop);
    if (!wait_mask(controller->mmio, op + 4, kControllerHalted, kControllerHalted))
        return false;
    write_mmio32(controller->mmio, op, read_mmio32(controller->mmio, op) | kHostControllerReset);
    if (!wait_mask(controller->mmio, op, kHostControllerReset, 0) ||
        !wait_mask(controller->mmio, op + 4, kControllerNotReady, 0)) return false;
    if (!(read_mmio32(controller->mmio, op + 8) & 1u)) return false; // 4 KiB pages.
    if (!allocate_controller_rings(controller)) return false;

    const uint32_t runtime = controller->runtime_offset;
    write_mmio64(controller->mmio, op + 0x30,
                 controller->device_context_array.physical.value());
    write_mmio32(controller->mmio, op + 0x38, controller->max_slots & 0xff);
    write_mmio64(controller->mmio, op + 0x18,
        controller->command_ring.memory.physical.value() | 1u);

    // Interrupts are masked; the event ring is drained by a kernel polling task.
    write_mmio32(controller->mmio, runtime + 0x20, 0);
    write_mmio32(controller->mmio, runtime + 0x28, 1);
    write_mmio64(controller->mmio, runtime + 0x30,
                 controller->erst.physical.value());
    write_mmio64(controller->mmio, runtime + 0x38,
                 controller->event_ring.physical.value());
    write_mmio32(controller->mmio, op, read_mmio32(controller->mmio, op) | kRunStop);
    return wait_mask(controller->mmio, op + 4, kControllerHalted, 0);
}

bool next_event(Controller *controller, Event *out) {
    auto *events = reinterpret_cast<volatile Event *>(controller->event_ring.virtual_address);
    mfence();
    const volatile Event &source = events[controller->event_index];
    const Event event{source.parameter, source.status, source.control};
    if ((event.control & kTrbCycle) != controller->event_cycle) return false;
    if (out) *out = event;
    ++controller->event_index;
    if (controller->event_index == kRingEntries) {
        controller->event_index = 0;
        controller->event_cycle ^= 1;
    }
    const uint64_t dequeue = controller->event_ring.physical.value() +
        static_cast<uint64_t>(controller->event_index) * sizeof(Event) | (1u << 3);
    write_mmio64(controller->mmio, controller->runtime_offset + 0x38, dequeue);
    return true;
}

uint64_t enqueue_trb(Ring *ring, const Trb &source, bool chain_link) {
    auto *trbs = reinterpret_cast<Trb *>(ring->memory.virtual_address);
    if (ring->enqueue == kRingEntries - 1) {
        uint32_t control = (kTrbLink << 10) | kTrbToggleCycle | ring->cycle;
        if (chain_link) control |= kTrbChain;
        trbs[kRingEntries - 1] = {
            ring->memory.physical.value(), 0, control
        };
        ring->enqueue = 0;
        ring->cycle ^= 1;
    }
    Trb entry = source;
    entry.control = (entry.control & ~kTrbCycle) | ring->cycle;
    const uint64_t physical = ring->memory.physical.value() +
        static_cast<uint64_t>(ring->enqueue) * sizeof(Trb);
    trbs[ring->enqueue++] = entry;
    mfence();
    return physical;
}

bool issue_command(Controller *controller, Trb command, Event *completion) {
    const uint64_t command_pointer = enqueue_trb(&controller->command_ring, command, false);
    write_mmio32(controller->mmio,
                 controller->doorbell_offset, 0);
    for (uint32_t i = 0; i < kWaitIterations; ++i) {
        Event event{};
        if (next_event(controller, &event) &&
            ((event.control >> 10) & 0x3f) == kXhciEventCommandCompletion &&
            (event.parameter & ~UINT64_C(0xf)) == command_pointer) {
            if (completion) *completion = event;
            return ((event.status >> 24) & 0xff) == kXhciCompletionSuccess;
        }
        __asm__ volatile("pause");
    }
    return false;
}

bool wait_transfer(Controller *controller, uint8_t slot, uint8_t endpoint,
                   bool accept_short, Event *completion) {
    for (uint32_t i = 0; i < kWaitIterations; ++i) {
        Event event{};
        if (next_event(controller, &event) &&
            ((event.control >> 10) & 0x3f) == kXhciEventTransfer &&
            static_cast<uint8_t>(event.control >> 24) == slot &&
            ((event.control >> 16) & 0x1f) == endpoint) {
            if (completion) *completion = event;
            const uint32_t code = (event.status >> 24) & 0xff;
            return code == kXhciCompletionSuccess ||
                   (accept_short && code == kXhciCompletionShortPacket);
        }
        __asm__ volatile("pause");
    }
    return false;
}

ControlStage transfer_stage(const Event &event, uint64_t setup_pointer,
                           uint64_t data_pointer, uint64_t status_pointer) {
    const uint64_t pointer = event.parameter & ~UINT64_C(0xf);
    if (pointer == setup_pointer) return ControlStage::Setup;
    if (data_pointer && pointer == data_pointer) return ControlStage::Data;
    if (pointer == status_pointer) return ControlStage::Status;
    return ControlStage::Unknown;
}

void record_transfer_failure(const Event &event, uint64_t setup_pointer,
                             uint64_t data_pointer, uint64_t status_pointer) {
    gLastTransferCompletionSeen =
        ((event.control >> 10) & 0x3fu) == kXhciEventTransfer;
    gLastTransferCompletionCode = gLastTransferCompletionSeen
        ? static_cast<uint8_t>(event.status >> 24) : 0;
    gLastTransferStage = gLastTransferCompletionSeen
        ? transfer_stage(event, setup_pointer, data_pointer, status_pointer)
        : ControlStage::Unknown;
}

bool control_transfer(HidDevice *keyboard, uint8_t request_type, uint8_t request,
                      uint16_t value, uint16_t index, void *buffer,
                      uint16_t length, bool input) {
    Controller *controller = keyboard->controller;
    gLastControlTransferFailed = false;
    gLastControlPort = keyboard->port;
    const uint32_t port_offset = controller->operational_offset + 0x400u +
        static_cast<uint32_t>(keyboard->port - 1) * 0x10u;
    gLastControlSpeed = static_cast<uint8_t>((read_mmio32(
        controller->mmio, port_offset) & kPortSpeedMask) >> 10);
    const uint64_t buffer_physical = keyboard->control_buffer.physical.value();
    const uint64_t setup = static_cast<uint64_t>(request_type) |
        (static_cast<uint64_t>(request) << 8) |
        (static_cast<uint64_t>(value) << 16) |
        (static_cast<uint64_t>(index) << 32) |
        (static_cast<uint64_t>(length) << 48);
    const uint32_t transfer_type = length ? (input ? 3u : 2u) : 0u;
    Trb stage[3]{};
    uint32_t count = 0;
    stage[count++] = {setup, 8, (kTrbSetupStage << 10) |
        kTrbImmediateData | kTrbChain | (transfer_type << 16)};
    if (length) {
        if (!buffer || length > keyboard->control_buffer.bytes) return false;
        stage[count++] = {buffer_physical, length,
            (kTrbDataStage << 10) | kTrbChain | (input ? kTrbDirectionIn : 0)};
    }
    const bool status_in = !length || !input;
    stage[count++] = {0, 0, (kTrbStatusStage << 10) |
        kTrbInterruptOnCompletion | (status_in ? kTrbDirectionIn : 0)};

    uint64_t setup_pointer = 0, data_pointer = 0, status_pointer = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t pointer = enqueue_trb(&keyboard->control_ring,
                                             stage[i], true);
        switch ((stage[i].control >> 10) & 0x3fu) {
            case kTrbSetupStage: setup_pointer = pointer; break;
            case kTrbDataStage: data_pointer = pointer; break;
            case kTrbStatusStage: status_pointer = pointer; break;
        }
    }
    const uint32_t doorbell = controller->doorbell_offset +
        static_cast<uint32_t>(keyboard->slot) * 4;
    write_mmio32(controller->mmio, doorbell, 1); // EP0 is DCI 1.
    Event completion{};
    gLastTransferCompletionSeen = false;
    gLastTransferCompletionCode = 0;
    gLastTransferStage = ControlStage::Unknown;
    if (!wait_transfer(controller, keyboard->slot, 1, true, &completion)) {
        record_transfer_failure(completion, setup_pointer, data_pointer,
                                status_pointer);
        bool recovered = false;
        if (gLastTransferCompletionSeen &&
            gLastTransferCompletionCode == 4) {
            // xHCI's soft-retry procedure preserves the partially completed
            // control TD while releasing EP0 from its Halted state.
            Trb reset{};
            reset.control = (kTrbResetEndpoint << 10) | (1u << 9) |
                (1u << 16) | (static_cast<uint32_t>(keyboard->slot) << 24);
            if (issue_command(controller, reset, nullptr)) {
                if (wait_scheduler_ticks(1)) {
                    write_mmio32(controller->mmio, doorbell, 1);
                    Event retry_completion{};
                    if (wait_transfer(controller, keyboard->slot, 1, true,
                                      &retry_completion)) {
                        completion = retry_completion;
                        recovered = true;
                    } else {
                        record_transfer_failure(retry_completion, setup_pointer,
                                                data_pointer, status_pointer);
                    }
                }
            }
        }
        if (!recovered) {
            gLastControlRequest = request;
            gLastControlTransferFailed = true;
            if (!gFirstControlFailure.valid) {
                gFirstControlFailure = {
                    request,
                    gLastControlPort,
                    gLastControlSpeed,
                    gLastTransferCompletionCode,
                    gLastTransferStage,
                    true,
                    gLastTransferCompletionSeen
                };
            }
            return false;
        }
    }
    if (length && input && buffer)
        for (uint32_t i = 0; i < length; ++i)
            static_cast<uint8_t *>(buffer)[i] = keyboard->control_buffer.virtual_address[i];
    (void)status_pointer;
    return true;
}

bool prepare_root_port(Controller *controller, uint8_t port, uint8_t *speed_out) {
    if (!controller || !speed_out || !port || port > controller->max_ports)
        return false;
    const uint32_t port_offset = controller->operational_offset + 0x400u +
        static_cast<uint32_t>(port - 1) * 0x10u;
    uint32_t portsc = read_mmio32(controller->mmio, port_offset);
    if (!(portsc & kPortConnected)) return false;
    if (!(portsc & kPortPowered)) {
        write_mmio32(controller->mmio, port_offset, kPortPowered);
        if (!wait_scheduler_ticks(10)) return false; // Allow 100 ms for VBUS.
        portsc = read_mmio32(controller->mmio, port_offset);
        if (!(portsc & kPortConnected) || !(portsc & kPortPowered)) return false;
    }

    uint8_t protocol = controller->port_protocol[port];
    if (protocol == kPortProtocolUnknown) {
        const uint8_t reported_speed = static_cast<uint8_t>(
            (portsc & kPortSpeedMask) >> 10);
        protocol = reported_speed >= 4 ? kPortProtocolUsb3 : kPortProtocolUsb2;
    }

    if (protocol == kPortProtocolUsb3) {
        // SuperSpeed ports become Enabled after automatic link training. Avoid
        // resetting a healthy link; only try a warm reset when firmware reports
        // the spec-defined Cold Attach Status after training failed.
        for (uint32_t i = 0; i < kWaitIterations; ++i) {
            portsc = read_mmio32(controller->mmio, port_offset);
            if (!(portsc & kPortConnected)) return false;
            const uint8_t speed = static_cast<uint8_t>(
                (portsc & kPortSpeedMask) >> 10);
            if ((portsc & kPortEnabled) && speed >= 4) {
                *speed_out = speed;
                return true;
            }
            __asm__ volatile("pause");
        }
        if (!(portsc & kPortColdAttachStatus)) return false;

        // PORTSC status-change bits are write-one-to-clear. Retain only PP and
        // assert WPR so unrelated status bits are not acknowledged by accident.
        write_mmio32(controller->mmio, port_offset,
                     (portsc & kPortPowered) | kPortWarmReset);
    } else {
        // USB 2 ports require PR before the speed field is valid. Preserve PP
        // and do not echo any read-one/write-one-to-clear status bits.
        write_mmio32(controller->mmio, port_offset,
                     (portsc & kPortPowered) | kPortReset);
    }

    for (uint32_t i = 0; i < kWaitIterations; ++i) {
        portsc = read_mmio32(controller->mmio, port_offset);
        if (!(portsc & kPortConnected)) return false;
        const uint8_t speed = static_cast<uint8_t>(
            (portsc & kPortSpeedMask) >> 10);
        if (!(portsc & kPortReset) && (portsc & kPortEnabled) &&
            (protocol == kPortProtocolUsb3 ? speed >= 4 :
             speed >= 1 && speed <= 3)) {
            if (!wait_scheduler_ticks(1)) return false; // 10 ms recovery.
            portsc = read_mmio32(controller->mmio, port_offset);
            if (!(portsc & kPortConnected) || !(portsc & kPortEnabled))
                return false;
            *speed_out = static_cast<uint8_t>(
                (portsc & kPortSpeedMask) >> 10);
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

bool disable_slot(Controller *controller, uint8_t slot) {
    Trb command{};
    command.control = (kTrbDisableSlot << 10) | (static_cast<uint32_t>(slot) << 24);
    return issue_command(controller, command, nullptr);
}

bool create_slot_attempt(Controller *controller, HidDevice *keyboard,
                         uint8_t port, uint8_t speed) {
    gAddressDeviceRetryAllowed = false;
    Trb command{};
    command.control = (static_cast<uint32_t>(controller->port_slot_type[port]) << 16) |
                      (kTrbEnableSlot << 10);
    Event completion{};
    gLastEnumerationFailure = ProbeStatus::SlotEnableFailed;
    gLastCommandCompletionCode = 0;
    gLastCommandCompletionSeen = false;
    if (!issue_command(controller, command, &completion)) {
        if (((completion.control >> 10) & 0x3fu) == kXhciEventCommandCompletion) {
            gLastCommandCompletionCode =
                static_cast<uint8_t>(completion.status >> 24);
            gLastCommandCompletionSeen = true;
        }
        return false;
    }
    const uint8_t slot = static_cast<uint8_t>(completion.control >> 24);
    if (!slot || slot > controller->max_slots) {
        gLastEnumerationFailure = ProbeStatus::InvalidSlotId;
        return false;
    }

    clear_object(keyboard);
    keyboard->controller = controller;
    keyboard->slot = slot;
    keyboard->port = port;
    if (!allocate_dma(kRingEntries * sizeof(Trb), &keyboard->control_ring.memory) ||
        !allocate_dma(controller->context_size * 33u, &keyboard->device_context) ||
        !allocate_dma(controller->context_size * 33u, &keyboard->input_context) ||
        !allocate_dma(4096, &keyboard->control_buffer)) {
        gLastEnumerationFailure = ProbeStatus::DeviceMemoryUnavailable;
        (void)disable_slot(controller, slot);
        release_hid_device(keyboard);
        return false;
    }
    keyboard->control_ring.cycle = 1;
    auto *dcbaa = reinterpret_cast<uint64_t *>(
        controller->device_context_array.virtual_address);
    dcbaa[slot] = keyboard->device_context.physical.value();

    const uint32_t stride = controller->context_size / sizeof(uint32_t);
    auto *input = reinterpret_cast<uint32_t *>(keyboard->input_context.virtual_address);
    // Input Context slot 0 is the Input Control Context, then the Slot Context,
    // then endpoint context DCI 1 (EP0).
    auto *slot_context = input + stride;
    auto *ep0_context = input + stride * 2;
    input[1] = 0x3; // Add Slot Context and Endpoint 0 Context.
    slot_context[0] = (static_cast<uint32_t>(speed) << 20) | (1u << 27);
    slot_context[1] = static_cast<uint32_t>(port) << 16;
    const uint16_t max_packet = speed >= 4 ? 512 : speed == 3 ? 64 : 8;
    ep0_context[1] = (3u << 1) | (4u << 3) | (static_cast<uint32_t>(max_packet) << 16);
    ep0_context[2] = static_cast<uint32_t>(keyboard->control_ring.memory.physical.value()) | 1u;
    ep0_context[3] = static_cast<uint32_t>(keyboard->control_ring.memory.physical.value() >> 32);
    ep0_context[4] = 8;

    command = {};
    command.parameter = keyboard->input_context.physical.value();
    command.control = (kTrbAddressDevice << 10) | (static_cast<uint32_t>(slot) << 24);
    gLastEnumerationFailure = ProbeStatus::AddressDeviceFailed;
    gLastCommandCompletionCode = 0;
    gLastCommandCompletionSeen = false;
    Event address_completion{};
    if (!issue_command(controller, command, &address_completion)) {
        if (((address_completion.control >> 10) & 0x3fu) ==
            kXhciEventCommandCompletion) {
            gLastCommandCompletionCode =
                static_cast<uint8_t>(address_completion.status >> 24);
            gLastCommandCompletionSeen = true;
        }
        const bool address_transaction_error =
            gLastCommandCompletionSeen && gLastCommandCompletionCode == 4;
        const bool slot_disabled = disable_slot(controller, slot);
        gAddressDeviceRetryAllowed = address_transaction_error && slot_disabled;
        dcbaa[slot] = 0;
        release_hid_device(keyboard);
        return false;
    }
    // The xHC owns SET_ADDRESS, but software still has to wait for the USB
    // address-recovery interval before sending the first request to it.
    (void)wait_scheduler_ticks(1);
    return true;
}

bool create_slot(Controller *controller, HidDevice *keyboard, uint8_t port,
                 uint8_t speed) {
    if (create_slot_attempt(controller, keyboard, port, speed)) return true;
    if (!gAddressDeviceRetryAllowed) return false;

    // xHCI section 4.6.5 permits a transaction error during Address Device
    // when the device stalls the initial SetAddress request. Recover by
    // disabling that slot and issuing a fresh Enable Slot/Address Device pair.
    return create_slot_attempt(controller, keyboard, port, speed);
}

bool get_usb_descriptor(HidDevice *keyboard, uint8_t descriptor_type,
                        uint8_t descriptor_index, uint16_t language,
                        uint16_t length) {
    return control_transfer(keyboard, 0x80, 6,
        static_cast<uint16_t>((descriptor_type << 8) | descriptor_index),
        language, keyboard->control_buffer.virtual_address, length, true);
}

bool find_boot_interface(HidDevice *keyboard, uint8_t protocol,
                         uint8_t *configuration,
                                  uint8_t *interface_number,
                                  uint8_t *endpoint_address,
                                  uint16_t *max_packet, uint8_t *max_burst,
                                  uint32_t *max_esit_payload,
                                  uint8_t *interval) {
    uint8_t *descriptor = keyboard->control_buffer.virtual_address;
    uint16_t total_length = static_cast<uint16_t>(descriptor[2] |
        (static_cast<uint16_t>(descriptor[3]) << 8));
    if (total_length < 9 || total_length > keyboard->control_buffer.bytes) return false;
    uint8_t config_value = descriptor[5];
    bool boot_interface = false;
    uint8_t found_interface = 0;
    uint8_t found_endpoint = 0;
    uint16_t found_packet = 0;
    uint8_t found_interval = 0;
    const uint16_t minimum_packet = protocol == 1
        ? kBootKeyboardReportSize : kBootMouseReportSize;
    for (uint32_t offset = 0; offset + 2 <= total_length;) {
        const uint8_t length = descriptor[offset];
        const uint8_t type = descriptor[offset + 1];
        if (length < 2 || offset + length > total_length) return false;
        if (type == 4 && length >= 9) {
            boot_interface = descriptor[offset + 3] == 0 &&
                descriptor[offset + 5] == 3 &&
                descriptor[offset + 6] == 1 && descriptor[offset + 7] == protocol;
            if (boot_interface) found_interface = descriptor[offset + 2];
        } else if (type == 5 && length >= 7 && boot_interface &&
                   (descriptor[offset + 2] & 0x80) &&
                   (descriptor[offset + 3] & 0x3) == 3) {
            found_endpoint = descriptor[offset + 2];
            const uint16_t packet_field = read_le16(descriptor + offset + 4);
            found_packet = packet_field & 0x7ff;
            const uint8_t speed = static_cast<uint8_t>((read_mmio32(
                keyboard->controller->mmio,
                keyboard->controller->operational_offset + 0x400u +
                static_cast<uint32_t>(keyboard->port - 1) * 0x10u) &
                kPortSpeedMask) >> 10);
            const uint8_t transaction_code =
                static_cast<uint8_t>((packet_field >> 11) & 0x03u);
            *max_burst = speed == 3 && transaction_code <= 2
                ? transaction_code : 0;
            *max_esit_payload = static_cast<uint32_t>(found_packet) *
                (static_cast<uint32_t>(*max_burst) + 1);
            found_interval = descriptor[offset + 6];
            const uint32_t companion_offset = offset + length;
            if (speed >= 4 && companion_offset + 6 <= total_length &&
                descriptor[companion_offset] >= 6 &&
                descriptor[companion_offset + 1] == 0x30) {
                const uint8_t companion_burst = descriptor[companion_offset + 2];
                const uint16_t companion_payload = read_le16(
                    descriptor + companion_offset + 4);
                if (companion_burst <= 15 && companion_payload >= found_packet) {
                    *max_burst = companion_burst;
                    *max_esit_payload = companion_payload;
                }
            }
            if (found_packet >= minimum_packet && found_interval) break;
        }
        offset += length;
    }
    if (!found_endpoint || found_packet < minimum_packet || !found_interval) return false;
    *configuration = config_value;
    *interface_number = found_interface;
    *endpoint_address = found_endpoint;
    *max_packet = found_packet;
    *interval = found_interval;
    return true;
}

bool find_touchpad_interface(HidDevice *keyboard, uint8_t *configuration,
                             uint8_t *interface_number,
                             uint8_t *endpoint_address, uint16_t *max_packet,
                             uint8_t *max_burst, uint32_t *max_esit_payload,
                             uint8_t *interval, uint16_t *report_descriptor_size,
                             uint16_t start_offset, uint16_t *next_offset) {
    if (!keyboard || !configuration || !interface_number || !endpoint_address ||
        !max_packet || !max_burst || !max_esit_payload || !interval ||
        !report_descriptor_size || !next_offset) return false;
    const uint8_t *descriptor = keyboard->control_buffer.virtual_address;
    const uint16_t total_length = static_cast<uint16_t>(descriptor[2] |
        (static_cast<uint16_t>(descriptor[3]) << 8));
    if (total_length < 9 || total_length > keyboard->control_buffer.bytes) return false;

    bool generic_hid_interface = false;
    uint8_t found_interface = 0;
    uint8_t found_endpoint = 0;
    uint16_t found_packet = 0;
    uint8_t found_interval = 0;
    uint16_t found_report_size = 0;
    for (uint32_t offset = start_offset; offset + 2 <= total_length;) {
        const uint8_t length = descriptor[offset];
        const uint8_t type = descriptor[offset + 1];
        if (length < 2 || offset + length > total_length) return false;
        if (type == 4 && length >= 9) {
            generic_hid_interface = descriptor[offset + 5] == 3 &&
                descriptor[offset + 6] == 0 &&
                (descriptor[offset + 7] == 0 || descriptor[offset + 7] == 2);
            found_interface = generic_hid_interface ? descriptor[offset + 2] : 0;
            found_endpoint = 0;
            found_packet = 0;
            found_interval = 0;
            found_report_size = 0;
        } else if (generic_hid_interface && type == 0x21 && length >= 9) {
            const uint8_t descriptor_count = descriptor[offset + 5];
            for (uint32_t entry = 0; entry < descriptor_count; ++entry) {
                const uint32_t entry_offset = offset + 6 + entry * 3;
                if (entry_offset + 3 > offset + length) break;
                if (descriptor[entry_offset] == 0x22) {
                    found_report_size = static_cast<uint16_t>(
                        descriptor[entry_offset + 1] |
                        (static_cast<uint16_t>(descriptor[entry_offset + 2]) << 8));
                }
            }
        } else if (generic_hid_interface && type == 5 && length >= 7 &&
                   (descriptor[offset + 2] & 0x80) &&
                   (descriptor[offset + 3] & 0x3) == 3) {
            const uint16_t packet_field = read_le16(descriptor + offset + 4);
            const uint16_t packet = packet_field & 0x7ff;
            const uint8_t endpoint_interval_value = descriptor[offset + 6];
            if (packet && endpoint_interval_value && found_report_size) {
                const uint8_t speed = static_cast<uint8_t>((read_mmio32(
                    keyboard->controller->mmio,
                    keyboard->controller->operational_offset + 0x400u +
                    static_cast<uint32_t>(keyboard->port - 1) * 0x10u) &
                    kPortSpeedMask) >> 10);
                const uint8_t transaction_code =
                    static_cast<uint8_t>((packet_field >> 11) & 0x03u);
                *max_burst = speed == 3 && transaction_code <= 2
                    ? transaction_code : 0;
                *max_esit_payload = static_cast<uint32_t>(packet) *
                    (static_cast<uint32_t>(*max_burst) + 1);
                const uint32_t companion_offset = offset + length;
                if (speed >= 4 && companion_offset + 6 <= total_length &&
                    descriptor[companion_offset] >= 6 &&
                    descriptor[companion_offset + 1] == 0x30) {
                    const uint8_t companion_burst = descriptor[companion_offset + 2];
                    const uint16_t companion_payload = read_le16(
                        descriptor + companion_offset + 4);
                    if (companion_burst <= 15 && companion_payload >= packet) {
                        *max_burst = companion_burst;
                        *max_esit_payload = companion_payload;
                    }
                }
                found_endpoint = descriptor[offset + 2];
                found_packet = packet;
                found_interval = endpoint_interval_value;
                *configuration = descriptor[5];
                *interface_number = found_interface;
                *endpoint_address = found_endpoint;
                *max_packet = found_packet;
                *interval = found_interval;
                *report_descriptor_size = found_report_size;
                *next_offset = static_cast<uint16_t>(offset + length);
                return true;
            }
        }
        offset += length;
    }
    return false;
}

uint8_t endpoint_interval(uint8_t speed, uint8_t b_interval) {
    if (speed == 1 || speed == 2) {
        uint32_t microframes = static_cast<uint32_t>(b_interval) * 8;
        uint8_t exponent = 0;
        uint32_t span = 1;
        while (span < microframes && exponent < 15) { span <<= 1; ++exponent; }
        return exponent;
    }
    return b_interval > 0 ? static_cast<uint8_t>(b_interval - 1) : 0;
}

bool configure_interrupt_endpoint(HidDevice *keyboard, HidInterface *interface,
                                  uint8_t endpoint_address,
                                  uint16_t max_packet, uint8_t max_burst,
                                  uint32_t max_esit_payload, uint8_t interval) {
    if (!keyboard || !interface || !max_packet || max_burst > 15 || !max_esit_payload ||
        max_esit_payload > 0x00ffffffu) return false;
    const uint8_t endpoint_number = endpoint_address & 0x0f;
    const uint8_t endpoint_id = static_cast<uint8_t>(endpoint_number * 2 +
        ((endpoint_address & 0x80) ? 1 : 0));
    if (!endpoint_id || endpoint_id > 31) return false;
    interface->endpoint_id = endpoint_id;
    Controller *controller = keyboard->controller;
    const uint32_t stride = controller->context_size / sizeof(uint32_t);
    auto *input = reinterpret_cast<uint32_t *>(keyboard->input_context.virtual_address);
    auto *output = reinterpret_cast<uint32_t *>(keyboard->device_context.virtual_address);
    auto *input_slot = input + stride;
    auto *output_slot = output;
    for (uint32_t i = 0; i < stride; ++i) input_slot[i] = output_slot[i];
    const uint32_t previous_context_entries = (input_slot[0] >> 27) & 0x1fu;
    const uint32_t context_entries = previous_context_entries > endpoint_id
        ? previous_context_entries : endpoint_id;
    input_slot[0] = (input_slot[0] & ~(0x1fu << 27)) |
        (context_entries << 27);
    input[1] = 1u | (1u << endpoint_id); // Add the slot and endpoint contexts.

    auto *endpoint = input + stride * (endpoint_id + 1);
    endpoint[0] = static_cast<uint32_t>(endpoint_interval(
        static_cast<uint8_t>((read_mmio32(controller->mmio,
            controller->operational_offset + 0x400u +
            static_cast<uint32_t>(keyboard->port - 1) * 0x10u) & kPortSpeedMask) >> 10),
        interval)) << 16;
    endpoint[0] |= ((max_esit_payload >> 16) & 0xffu) << 24;
    endpoint[1] = (static_cast<uint32_t>(max_burst) << 8) |
        (3u << 1) | (7u << 3) |
        (static_cast<uint32_t>(max_packet) << 16);
    endpoint[2] = static_cast<uint32_t>(interface->interrupt_ring.memory.physical.value()) | 1u;
    endpoint[3] = static_cast<uint32_t>(interface->interrupt_ring.memory.physical.value() >> 32);
    endpoint[4] = ((max_esit_payload & 0xffffu) << 16) |
                  interface->report_size;

    Trb command{};
    command.parameter = keyboard->input_context.physical.value();
    command.control = (kTrbConfigureEndpoint << 10) |
        (static_cast<uint32_t>(keyboard->slot) << 24);
    return issue_command(controller, command, nullptr);
}

bool update_ep0_max_packet(HidDevice *keyboard, uint16_t max_packet) {
    Controller *controller = keyboard->controller;
    const uint32_t stride = controller->context_size / sizeof(uint32_t);
    auto *input = reinterpret_cast<uint32_t *>(keyboard->input_context.virtual_address);
    auto *output = reinterpret_cast<uint32_t *>(keyboard->device_context.virtual_address);
    auto *input_slot = input + stride;
    auto *input_ep0 = input + stride * 2;
    auto *output_slot = output;
    auto *output_ep0 = output + stride;
    for (uint32_t i = 0; i < stride; ++i) {
        input_slot[i] = output_slot[i];
        input_ep0[i] = output_ep0[i];
    }
    input[1] = 0x3; // Include the unchanged Slot Context and updated EP0.
    input_ep0[1] = (input_ep0[1] & 0x0000ffffu) |
                   (static_cast<uint32_t>(max_packet) << 16);

    Trb command{};
    command.parameter = keyboard->input_context.physical.value();
    command.control = (kTrbEvaluateContext << 10) |
        (static_cast<uint32_t>(keyboard->slot) << 24);
    return issue_command(controller, command, nullptr);
}

void release_hid_interface(HidInterface *interface) {
    if (!interface) return;
    release_ring(&interface->interrupt_ring);
    welovec::x86_64::mm::dma::release(&interface->report_buffer);
    clear_object(interface);
}

bool allocate_hid_interface(HidInterface *interface, HidKind kind,
                            uint8_t interface_number, uint16_t report_size) {
    if (!interface || !report_size ||
        !allocate_dma(kRingEntries * sizeof(Trb), &interface->interrupt_ring.memory) ||
        !allocate_dma(welovec::x86_64::input::hid_touchpad::kMaximumReportBytes,
                      &interface->report_buffer)) {
        release_hid_interface(interface);
        return false;
    }
    interface->interrupt_ring.cycle = 1;
    interface->kind = kind;
    interface->interface_number = interface_number;
    interface->report_size = report_size;
    return true;
}

bool start_hid_device(HidDevice *keyboard, uint8_t speed) {
    uint8_t device_descriptor[18]{};
    gLastEnumerationFailure = ProbeStatus::UsbDescriptorUnavailable;
    kernel_console_write("[xHCI] requesting 8-byte device descriptor\n");
    if (!get_usb_descriptor(keyboard, 1, 0, 0, 8)) {
        kernel_console_write("[xHCI] device descriptor request failed\n");
        return false;
    }
    kernel_console_write("[xHCI] first device descriptor received\n");
    const uint8_t ep0_max_packet = keyboard->control_buffer.virtual_address[7];
    if (speed == 1) {
        if ((ep0_max_packet != 8 && ep0_max_packet != 16 &&
             ep0_max_packet != 32 && ep0_max_packet != 64) ||
            !update_ep0_max_packet(keyboard, ep0_max_packet)) return false;
    } else if (speed == 2 && ep0_max_packet != 8) {
        return false;
    }
    kernel_console_write("[xHCI] requesting full device descriptor\n");
    if (!get_usb_descriptor(keyboard, 1, 0, 0, sizeof(device_descriptor))) {
        kernel_console_write("[xHCI] full device descriptor request failed\n");
        return false;
    }
    for (uint32_t i = 0; i < sizeof(device_descriptor); ++i)
        device_descriptor[i] = keyboard->control_buffer.virtual_address[i];
    if (device_descriptor[0] < 18 || device_descriptor[1] != 1) return false;

    kernel_console_write("[xHCI] requesting configuration header\n");
    if (!get_usb_descriptor(keyboard, 2, 0, 0, 9)) {
        kernel_console_write("[xHCI] configuration header request failed\n");
        return false;
    }
    uint8_t *data = keyboard->control_buffer.virtual_address;
    const uint16_t total_length = static_cast<uint16_t>(data[2] |
        (static_cast<uint16_t>(data[3]) << 8));
    if (total_length < 9 || total_length > keyboard->control_buffer.bytes)
        return false;
    kernel_console_write("[xHCI] requesting full configuration descriptor\n");
    if (!get_usb_descriptor(keyboard, 2, 0, 0, total_length)) {
        kernel_console_write("[xHCI] full configuration descriptor request failed\n");
        return false;
    }
    kernel_console_write("[xHCI] configuration descriptor received\n");

    InterfaceCandidate candidates[kMaximumHidInterfaces]{};
    uint32_t candidate_count = 0;
    uint8_t configuration = 0, interface_number = 0, endpoint_address = 0;
    uint8_t max_burst = 0, interval = 0;
    uint32_t max_esit_payload = 0;
    uint16_t max_packet = 0;
    gLastEnumerationFailure = ProbeStatus::HidDeviceUnavailable;

    // Gather candidates before issuing control requests: those requests reuse
    // EP0's buffer, so parsing the configuration descriptor must finish first.
    if (find_boot_interface(keyboard, 2, &configuration, &interface_number,
                            &endpoint_address, &max_packet, &max_burst,
                            &max_esit_payload, &interval)) {
        candidates[candidate_count++] = {HidKind::BootMouse, interface_number,
            endpoint_address, max_burst, interval, max_packet, 0, max_esit_payload};
    }
    if (find_boot_interface(keyboard, 1, &configuration, &interface_number,
                            &endpoint_address, &max_packet, &max_burst,
                            &max_esit_payload, &interval) &&
        candidate_count < kMaximumHidInterfaces) {
        candidates[candidate_count++] = {HidKind::Keyboard, interface_number,
            endpoint_address, max_burst, interval, max_packet, 0, max_esit_payload};
    }

    uint16_t report_descriptor_size = 0;
    uint16_t scan_offset = 0;
    while (candidate_count < kMaximumHidInterfaces &&
           find_touchpad_interface(keyboard, &configuration, &interface_number,
                                   &endpoint_address, &max_packet, &max_burst,
                                   &max_esit_payload, &interval,
                                   &report_descriptor_size, scan_offset,
                                   &scan_offset)) {
        if (report_descriptor_size > kMaximumReportDescriptorSize) continue;
        candidates[candidate_count++] = {HidKind::Touchpad, interface_number,
            endpoint_address, max_burst, interval, max_packet,
            report_descriptor_size, max_esit_payload};
    }
    if (!candidate_count) return false;

    gLastEnumerationFailure = ProbeStatus::HidConfigurationUnavailable;
    kernel_console_write("[xHCI] selecting HID configuration\n");
    if (!control_transfer(keyboard, 0x00, 9, configuration, 0,
                          nullptr, 0, false)) return false;

    for (uint32_t i = 0; i < candidate_count; ++i) {
        const InterfaceCandidate &candidate = candidates[i];
        if (keyboard->interface_count == kMaximumHidInterfaces) break;
        HidInterface *interface = &keyboard->interfaces[keyboard->interface_count];
        clear_object(interface);
        if (candidate.kind == HidKind::Touchpad) {
            gLastEnumerationFailure = ProbeStatus::HidReportUnavailable;
            if (!control_transfer(keyboard, 0x81, 6, 0x2200,
                                  candidate.interface_number,
                                  keyboard->control_buffer.virtual_address,
                                  candidate.report_descriptor_size, true) ||
                !welovec::x86_64::input::hid_touchpad::parse_report_descriptor(
                    &interface->touchpad_state,
                    keyboard->control_buffer.virtual_address,
                    candidate.report_descriptor_size)) {
                clear_object(interface);
                continue;
            }
            const uint16_t report_size =
                welovec::x86_64::input::hid_touchpad::maximum_report_size(
                    &interface->touchpad_state);
            if (!report_size || report_size > candidate.max_esit_payload ||
                report_size > welovec::x86_64::input::hid_touchpad::kMaximumReportBytes) {
                clear_object(interface);
                continue;
            }
            if (!allocate_hid_interface(interface, candidate.kind,
                                        candidate.interface_number, report_size)) {
                gLastEnumerationFailure = ProbeStatus::DeviceMemoryUnavailable;
                continue;
            }
        } else {
            const uint16_t report_size = candidate.kind == HidKind::Keyboard
                ? kBootKeyboardReportSize : kBootMouseReportSize;
            if (!allocate_hid_interface(interface, candidate.kind,
                                        candidate.interface_number, report_size)) {
                gLastEnumerationFailure = ProbeStatus::DeviceMemoryUnavailable;
                continue;
            }
            gLastEnumerationFailure = ProbeStatus::HidConfigurationUnavailable;
            if (!control_transfer(keyboard, 0x21, 0x0b, 0,
                                  candidate.interface_number,
                                  nullptr, 0, false)) {
                release_hid_interface(interface);
                continue;
            }
        }

        gLastEnumerationFailure = ProbeStatus::HidEndpointUnavailable;
        if (!configure_interrupt_endpoint(keyboard, interface,
                                          candidate.endpoint_address,
                                          candidate.max_packet,
                                          candidate.max_burst,
                                          candidate.max_esit_payload,
                                          candidate.interval)) {
            release_hid_interface(interface);
            continue;
        }
        kernel_console_write(candidate.kind == HidKind::BootMouse
            ? "[xHCI] boot mouse interface configured\n"
            : candidate.kind == HidKind::Touchpad
                ? "[xHCI] generic HID pointer interface configured\n"
                : "[xHCI] boot keyboard interface configured\n");
        ++keyboard->interface_count;
    }
    return keyboard->interface_count != 0;
}

bool enumerate_port(Controller *controller, uint8_t port) {
    const uint32_t port_offset = controller->operational_offset + 0x400u +
        static_cast<uint32_t>(port - 1) * 0x10u;
    const uint32_t portsc = read_mmio32(controller->mmio, port_offset);
    if (!(portsc & kPortConnected)) return false;
    ++gConnectedPortCount;
    kernel_console_write("[xHCI] connected port; resetting link\n");
    uint8_t speed = 0;
    if (!prepare_root_port(controller, port, &speed)) {
        gLastEnumerationFailure = ProbeStatus::PortResetFailed;
        return false;
    }
    kernel_console_write("[xHCI] port enabled; enabling device slot\n");
    if (controller->device_count == kMaximumHidDevices) {
        gLastEnumerationFailure = ProbeStatus::DeviceSlotUnavailable;
        return false;
    }

    HidDevice *keyboard = &controller->devices[controller->device_count];
    // create_slot records whether Enable Slot, DMA allocation, or Address
    // Device failed. Keep that detail for the startup diagnostic.
    if (!create_slot(controller, keyboard, port, speed)) return false;
    kernel_console_write("[xHCI] slot addressed; reading HID descriptors\n");
    if (!start_hid_device(keyboard, speed)) {
        (void)disable_slot(controller, keyboard->slot);
        auto *dcbaa = reinterpret_cast<uint64_t *>(
            controller->device_context_array.virtual_address);
        dcbaa[keyboard->slot] = 0;
        release_hid_device(keyboard);
        return false;
    }
    ++controller->device_count;
    kernel_console_write("[xHCI] HID device configured\n");
    return true;
}

void queue_interrupt_read(HidDevice *keyboard, HidInterface *interface) {
    if (!keyboard || !interface) return;
    Trb normal{};
    normal.parameter = interface->report_buffer.physical.value();
    normal.status = interface->report_size;
    normal.control = (kTrbNormal << 10) | kTrbInterruptOnCompletion;
    (void)enqueue_trb(&interface->interrupt_ring, normal, false);
    const uint32_t doorbell = keyboard->controller->doorbell_offset +
        static_cast<uint32_t>(keyboard->slot) * 4;
    write_mmio32(keyboard->controller->mmio, doorbell, interface->endpoint_id);
}

bool initialize_controller(Controller *controller,
                           const welovec::x86_64::pci::Device &device) {
    clear_object(controller);
    controller->pci = device;
    kernel_console_write("[xHCI] enabling PCI bus mastering\n");
    if (!welovec::x86_64::pci::enable_memory_and_bus_master(device)) return false;
    kernel_console_write("[xHCI] mapping controller registers\n");
    if (!acquire_mmio(controller)) return false;
    kernel_console_write("[xHCI] requesting firmware handoff\n");
    if (!hand_over_from_firmware(controller)) return false;
    kernel_console_write("[xHCI] resetting controller\n");
    if (!initialize_controller_registers(controller)) return false;
    discover_port_protocols(controller);
    controller->active = true;
    kernel_console_write("[xHCI] controller ready; scanning ports\n");
    for (uint32_t port = 1; port <= controller->max_ports; ++port)
        (void)enumerate_port(controller, static_cast<uint8_t>(port));
    kernel_console_write("[xHCI] port scan finished\n");
    return true;
}

void release_controller(Controller *controller) {
    if (!controller) return;
    for (uint32_t i = 0; i < controller->device_count; ++i)
        release_hid_device(&controller->devices[i]);
    release_ring(&controller->command_ring);
    welovec::x86_64::mm::dma::release(&controller->event_ring);
    welovec::x86_64::mm::dma::release(&controller->erst);
    welovec::x86_64::mm::dma::release(&controller->scratchpad_array);
    for (uint32_t i = 0; i < controller->scratchpad_count; ++i)
        welovec::x86_64::mm::dma::release(&controller->scratchpads[i]);
    welovec::x86_64::mm::dma::release(&controller->device_context_array);
    if (controller->mmio_mapping.address)
        (void)welovec::memory::vmm::unmap_physical(
            controller->mmio_mapping.address, controller->mmio_mapping.size);
    clear_object(controller);
}

bool pci_visitor(const welovec::x86_64::pci::Device &device, void *) {
    if (device.class_code != kPciClassSerialBus ||
        device.subclass != kPciSubclassUsb || device.prog_if != kPciProgIfXhci)
        return false;
    ++gPciControllerCount;
    if (gControllerCount == 2) return false;
    kernel_console_write("[xHCI] controller found; initializing\n");
    Controller *controller = &gControllers[gControllerCount];
    if (initialize_controller(controller, device)) {
        ++gControllerCount;
    } else {
        ++gFailedControllerCount;
        release_controller(controller);
    }
    return false;
}

void poll_controller(Controller *controller) {
    if (!controller || !controller->active) return;
    Event event{};
    while (next_event(controller, &event)) {
        const uint32_t type = (event.control >> 10) & 0x3f;
        if (type == kXhciEventPortStatus) continue;
        if (type != kXhciEventTransfer) continue;
        const uint8_t slot = static_cast<uint8_t>(event.control >> 24);
        const uint8_t endpoint = (event.control >> 16) & 0x1f;
        for (uint32_t i = 0; i < controller->device_count; ++i) {
            HidDevice &device = controller->devices[i];
            if (device.slot != slot) continue;
            for (uint32_t j = 0; j < device.interface_count; ++j) {
                HidInterface &interface = device.interfaces[j];
                if (interface.endpoint_id != endpoint) continue;
                const uint32_t completion = (event.status >> 24) & 0xff;
                const uint32_t remaining = event.status & 0x00ffffff;
                const uint32_t received = remaining <= interface.report_size
                    ? interface.report_size - remaining : 0;
                const uint32_t minimum_report = interface.kind == HidKind::Keyboard
                    ? kBootKeyboardReportSize : interface.kind == HidKind::BootMouse
                    ? kBootMouseReportSize : 1;
                if ((completion == kXhciCompletionSuccess ||
                     completion == kXhciCompletionShortPacket) &&
                    received >= minimum_report) {
                    if (!interface.first_interrupt_logged) {
                        kernel_console_write("[xHCI] USB HID interrupt report received\n");
                        interface.first_interrupt_logged = true;
                    }
                    const uint8_t *report = interface.report_buffer.virtual_address;
                    if (interface.kind == HidKind::Keyboard) {
                        welovec::x86_64::keyboard::submit_usb_boot_report(
                            &interface.report_state, report);
                    } else if (interface.kind == HidKind::BootMouse) {
                        const uint8_t buttons = report[0] & 0x07u;
                        const int32_t delta_x = static_cast<int8_t>(report[1]);
                        const int32_t delta_y = static_cast<int8_t>(report[2]);
                        if (buttons != interface.previous_buttons || delta_x || delta_y) {
                            uint32_t mapped_buttons = 0;
                            if (buttons & 1u) mapped_buttons |= WELOVEC_POINTER_BUTTON_LEFT;
                            if (buttons & 2u) mapped_buttons |= WELOVEC_POINTER_BUTTON_RIGHT;
                            if (buttons & 4u) mapped_buttons |= WELOVEC_POINTER_BUTTON_MIDDLE;
                            interface.previous_buttons = buttons;
                            welovec::x86_64::input::pointer::submit_relative(
                                delta_x, delta_y, 0, mapped_buttons,
                                welovec::x86_64::input::pointer::Source::UsbHid);
                        }
                    } else {
                        (void)welovec::x86_64::input::hid_touchpad::process_report(
                            &interface.touchpad_state, report, received,
                            welovec::x86_64::input::pointer::Source::UsbHid);
                    }
                }
                queue_interrupt_read(&device, &interface);
                return;
            }
        }
    }
}

void usb_input_worker(void *) {
    for (;;) {
        for (uint32_t i = 0; i < gControllerCount; ++i)
            poll_controller(&gControllers[i]);
        (void)welovec::x86_64::scheduler::sleep(1);
    }
}

void probe_worker(void *) {
    kernel_console_write("[xHCI] PCI scan started\n");
    (void)initialize();
    kernel_console_write("[xHCI] probe task finished\n");
}

}

bool initialize() {
    if (gInitialized) return (gHasKeyboard || gHasPointer) && gWorkerStarted;
    gInitialized = true;
    (void)welovec::x86_64::pci::enumerate(pci_visitor, nullptr);
    kernel_console_write("[xHCI] PCI scan finished\n");
    for (uint32_t i = 0; i < gControllerCount; ++i) {
        for (uint32_t j = 0; j < gControllers[i].device_count; ++j) {
            HidDevice &device = gControllers[i].devices[j];
            for (uint32_t k = 0; k < device.interface_count; ++k) {
                if (device.interfaces[k].kind == HidKind::Keyboard)
                    gHasKeyboard = true;
                else gHasPointer = true;
            }
        }
    }
    if (!gHasKeyboard && !gHasPointer) {
        if (!gPciControllerCount) gProbeStatus = ProbeStatus::NoController;
        else if (!gControllerCount || gFailedControllerCount)
            gProbeStatus = ProbeStatus::ControllerUnavailable;
        else if (!gConnectedPortCount)
            gProbeStatus = ProbeStatus::NoConnectedHidDevice;
        else gProbeStatus = gLastEnumerationFailure;
        return false;
    }
    if ((gHasKeyboard || gHasPointer) && !gWorkerStarted) {
        gWorkerStarted = welovec::x86_64::scheduler::create_kernel_thread(
            "xhci-usb-input", usb_input_worker, nullptr,
            welovec::x86_64::scheduler::Priority::Background) != 0;
    }
    if (!gWorkerStarted) {
        gProbeStatus = ProbeStatus::WorkerUnavailable;
        return false;
    }
    for (uint32_t i = 0; i < gControllerCount; ++i) {
        Controller &controller = gControllers[i];
        for (uint32_t j = 0; j < controller.device_count; ++j) {
            HidDevice &device = controller.devices[j];
            for (uint32_t k = 0; k < device.interface_count; ++k) {
                HidInterface &interface = device.interfaces[k];
                queue_interrupt_read(&device, &interface);
                if (interface.kind != HidKind::Keyboard) {
                    welovec::x86_64::input::pointer::register_source(
                        welovec::x86_64::input::pointer::Source::UsbHid);
                } else {
                    welovec::x86_64::keyboard::register_source(
                        welovec::x86_64::keyboard::Source::UsbHid);
                }
            }
        }
    }
    gProbeStatus = ProbeStatus::Ready;
    return true;
}

bool start_probe() {
    if (gProbeQueued || gInitialized) return true;
    gProbeStatus = ProbeStatus::Probing;
    const auto task = welovec::x86_64::scheduler::create_kernel_thread(
        "xhci-usb-probe", probe_worker, nullptr,
        welovec::x86_64::scheduler::Priority::Normal);
    if (!task) {
        gProbeStatus = ProbeStatus::WorkerUnavailable;
        return false;
    }
    gProbeQueued = true;
    return true;
}

const char *format_command_failure(const char *message) {
    uint32_t cursor = 0;
    while (message[cursor] && cursor + 1 < sizeof(gProbeStatusBuffer)) {
        gProbeStatusBuffer[cursor] = message[cursor];
        ++cursor;
    }
    const char *suffix = gLastCommandCompletionSeen ? " (" : " (timeout)";
    uint32_t suffix_index = 0;
    while (suffix[suffix_index] && cursor + 1 < sizeof(gProbeStatusBuffer)) {
        gProbeStatusBuffer[cursor++] = suffix[suffix_index++];
    }
    if (gLastCommandCompletionSeen) {
        const char *description = nullptr;
        switch (gLastCommandCompletionCode) {
            case 2: description = "data buffer error, code "; break;
            case 3: description = "babble detected, code "; break;
            case 4: description = "USB transaction error, code "; break;
            case 5: description = "TRB error, code "; break;
            case 6: description = "stall error, code "; break;
            case 7: description = "resource error, code "; break;
            case 9: description = "no slots available, code "; break;
            case 17: description = "parameter error, code "; break;
            case 19: description = "context state error, code "; break;
            default: description = "completion code "; break;
        }
        for (uint32_t i = 0; description[i] &&
             cursor + 1 < sizeof(gProbeStatusBuffer); ++i)
            gProbeStatusBuffer[cursor++] = description[i];
        char digits[3];
        uint32_t count = 0;
        uint32_t value = gLastCommandCompletionCode;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value && count < sizeof(digits));
        while (count && cursor + 1 < sizeof(gProbeStatusBuffer))
            gProbeStatusBuffer[cursor++] = digits[--count];
        if (cursor + 1 < sizeof(gProbeStatusBuffer))
            gProbeStatusBuffer[cursor++] = ')';
    }
    gProbeStatusBuffer[cursor] = '\0';
    return gProbeStatusBuffer;
}

const char *format_control_failure() {
    const uint8_t request = gFirstControlFailure.valid
        ? gFirstControlFailure.request : gLastControlRequest;
    const uint8_t port = gFirstControlFailure.valid
        ? gFirstControlFailure.port : gLastControlPort;
    const uint8_t speed = gFirstControlFailure.valid
        ? gFirstControlFailure.speed : gLastControlSpeed;
    const uint8_t completion_code = gFirstControlFailure.valid
        ? gFirstControlFailure.completion_code : gLastTransferCompletionCode;
    const ControlStage stage = gFirstControlFailure.valid
        ? gFirstControlFailure.stage : gLastTransferStage;
    const bool completion_seen = gFirstControlFailure.valid
        ? gFirstControlFailure.completion_seen : gLastTransferCompletionSeen;
    const char *message = "USB control transfer failed";
    if (request == 6) message = "USB GET_DESCRIPTOR failed";
    else if (request == 9) message = "USB SET_CONFIGURATION failed";
    else if (request == 11) message = "USB HID SET_PROTOCOL failed";
    uint32_t cursor = 0;
    while (message[cursor] && cursor + 1 < sizeof(gProbeStatusBuffer)) {
        gProbeStatusBuffer[cursor] = message[cursor];
        ++cursor;
    }
    const char *port_label = " [port ";
    for (uint32_t i = 0; port_label[i] &&
         cursor + 1 < sizeof(gProbeStatusBuffer); ++i)
        gProbeStatusBuffer[cursor++] = port_label[i];
    char port_digits[3];
    uint32_t port_count = 0;
    uint32_t port_value = port;
    do {
        port_digits[port_count++] = static_cast<char>('0' + port_value % 10);
        port_value /= 10;
    } while (port_value && port_count < sizeof(port_digits));
    while (port_count && cursor + 1 < sizeof(gProbeStatusBuffer))
        gProbeStatusBuffer[cursor++] = port_digits[--port_count];
    const char *speed_label = ", speed ";
    for (uint32_t i = 0; speed_label[i] &&
         cursor + 1 < sizeof(gProbeStatusBuffer); ++i)
        gProbeStatusBuffer[cursor++] = speed_label[i];
    if (cursor + 1 < sizeof(gProbeStatusBuffer))
        gProbeStatusBuffer[cursor++] = static_cast<char>('0' + speed);
    if (cursor + 1 < sizeof(gProbeStatusBuffer))
        gProbeStatusBuffer[cursor++] = ']';

    const char *detail = " (transfer timeout)";
    if (completion_seen) {
        switch (stage) {
            case ControlStage::Setup: detail = " (setup stage, completion "; break;
            case ControlStage::Data: detail = " (data stage, completion "; break;
            case ControlStage::Status: detail = " (status stage, completion "; break;
            default: detail = " (completion "; break;
        }
    }
    for (uint32_t i = 0; detail[i] && cursor + 1 < sizeof(gProbeStatusBuffer); ++i)
        gProbeStatusBuffer[cursor++] = detail[i];
    if (completion_seen) {
        char digits[3];
        uint32_t count = 0;
        uint32_t value = completion_code;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value && count < sizeof(digits));
        while (count && cursor + 1 < sizeof(gProbeStatusBuffer))
            gProbeStatusBuffer[cursor++] = digits[--count];
        const char *description = nullptr;
        switch (completion_code) {
            case 2: description = ": data buffer error)"; break;
            case 3: description = ": babble detected)"; break;
            case 4: description = ": USB transaction error)"; break;
            case 5: description = ": TRB error)"; break;
            case 6: description = ": stall error)"; break;
            case 7: description = ": resource error)"; break;
            case 8: description = ": bandwidth error)"; break;
            case 9: description = ": no device slots)"; break;
            case 11: description = ": slot disabled)"; break;
            case 12: description = ": endpoint disabled)"; break;
            case 17: description = ": parameter error)"; break;
            case 19: description = ": context state error)"; break;
            case 20: description = ": no ping response)"; break;
            case 23: description = ": missed service)"; break;
            default: description = ")"; break;
        }
        for (uint32_t i = 0; description[i] &&
             cursor + 1 < sizeof(gProbeStatusBuffer); ++i)
            gProbeStatusBuffer[cursor++] = description[i];
    }
    gProbeStatusBuffer[cursor] = '\0';
    return gProbeStatusBuffer;
}

const char *probe_status() {
    switch (gProbeStatus) {
        case ProbeStatus::NotStarted: return "not started";
        case ProbeStatus::Probing: return "probe in progress";
        case ProbeStatus::NoController: return "no xHCI controller found";
        case ProbeStatus::ControllerUnavailable:
            return "xHCI controller failed to initialize";
        case ProbeStatus::NoConnectedHidDevice:
            return "no USB device connected during startup";
        case ProbeStatus::PortResetFailed:
            return "connected USB port did not reach Enabled state";
        case ProbeStatus::DeviceSlotUnavailable:
            return "xHCI HID-device limit reached during startup";
        case ProbeStatus::SlotEnableFailed:
            return format_command_failure("xHCI Enable Slot failed");
        case ProbeStatus::DeviceMemoryUnavailable:
            return "not enough DMA memory to configure the USB device";
        case ProbeStatus::InvalidSlotId:
            return "xHCI returned an invalid device slot ID";
        case ProbeStatus::AddressDeviceFailed:
            return format_command_failure("xHCI Address Device failed");
        case ProbeStatus::UsbDescriptorUnavailable:
            return gLastControlTransferFailed ? format_control_failure()
                : "USB control transfer/device descriptor failed";
        case ProbeStatus::HidDeviceUnavailable:
            return "USB device has no supported HID pointer/keyboard interface";
        case ProbeStatus::HidConfigurationUnavailable:
            return gLastControlTransferFailed ? format_control_failure()
                : "USB HID configuration or protocol selection failed";
        case ProbeStatus::HidReportUnavailable:
            return gLastControlTransferFailed ? format_control_failure()
                : "USB HID report descriptor was rejected or unreadable";
        case ProbeStatus::HidEndpointUnavailable:
            return "USB HID interrupt endpoint could not be configured";
        case ProbeStatus::WorkerUnavailable:
            return "USB input polling worker unavailable";
        case ProbeStatus::Ready: return "ready";
    }
    return "unknown";
}

}

extern "C" const char *welovec_xhci_probe_status(void) {
    return welovec::x86_64::input::usb::probe_status();
}
