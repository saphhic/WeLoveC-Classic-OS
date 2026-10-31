// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements keyboard and pointer input from virtio devices.

#include "virtio_input.hpp"

#include "keyboard.hpp"
#include "mouse.hpp"
#include "arch/x86_64/mm/dma/dma.hpp"
#include "arch/x86_64/mm/physical/physical_memory.hpp"
#include "arch/common/MM/VMM/vmm.hpp"
#include "arch/x86_64/cpu/barrier.h"
#include "arch/x86_64/pci/pci.hpp"
#include "arch/x86_64/scheduler/scheduler.hpp"

namespace welovec::x86_64::input::virtio {

namespace {
constexpr uint16_t kVirtioVendor = 0x1af4;
constexpr uint16_t kModernInputDevice = 0x1052;
constexpr uint16_t kTransitionalInputDevice = 0x1012;
constexpr uint8_t kPciCapabilityId = 0x09;
constexpr uint8_t kCapCommon = 1;
constexpr uint8_t kCapNotify = 2;
constexpr uint8_t kCapDevice = 4;
constexpr uint32_t kFeatureVersion1 = 1;
constexpr uint8_t kStatusAcknowledge = 1;
constexpr uint8_t kStatusDriver = 2;
constexpr uint8_t kStatusFeaturesOk = 8;
constexpr uint8_t kStatusDriverOk = 4;
constexpr uint16_t kQueueMaximum = 64;
constexpr uint16_t kMaximumDevices = 4;
constexpr uint32_t kMaximumMappings = 8;
constexpr uint32_t kMaximumCapabilityMapping = 0x10000;
constexpr uint32_t kCommonConfigMinimum = 56;
constexpr uint32_t kInputConfigDataOffset = 8;
constexpr uint32_t kEventSize = 8;
constexpr uint32_t kEvKey = 1;
constexpr uint32_t kEvSyn = 0;
constexpr uint32_t kEvRel = 2;
constexpr uint32_t kEvAbs = 3;
constexpr uint32_t kKeyA = 30;
constexpr uint32_t kKeyEnter = 28;
constexpr uint32_t kKeySpace = 57;
constexpr uint32_t kBtnLeft = 272;
constexpr uint32_t kBtnRight = 273;
constexpr uint32_t kBtnMiddle = 274;
constexpr uint32_t kBtnSide = 275;
constexpr uint32_t kBtnExtra = 276;
constexpr uint32_t kBtnToolFinger = 325;
constexpr uint32_t kBtnTouch = 330;
constexpr uint32_t kRelX = 0;
constexpr uint32_t kRelY = 1;
constexpr uint32_t kRelWheel = 8;
constexpr uint32_t kAbsX = 0;
constexpr uint32_t kAbsY = 1;
constexpr uint8_t kConfigSelectEvBits = 0x11;
constexpr uint8_t kConfigSelectAbsInfo = 0x12;

struct PointerState {
    int32_t delta_x;
    int32_t delta_y;
    int32_t wheel;
    int32_t absolute_x;
    int32_t absolute_y;
    uint32_t buttons;
    bool dirty;
    bool absolute_dirty;
};

struct DmaDescriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct AvailableRing {
    uint16_t flags;
    volatile uint16_t index;
    uint16_t ring[128];
} __attribute__((packed));

struct UsedElement { uint32_t id; uint32_t length; } __attribute__((packed));
struct UsedRing {
    uint16_t flags;
    volatile uint16_t index;
    UsedElement ring[128];
} __attribute__((packed));

struct Queue {
    welovec::x86_64::mm::dma::Region descriptors;
    welovec::x86_64::mm::dma::Region available;
    welovec::x86_64::mm::dma::Region used;
    uint16_t size;
    uint16_t last_used;
    uint16_t notify_offset;
    volatile uint16_t *notify;
};

struct Mapping { void *address; uint64_t size; };

struct DeviceState {
    welovec::x86_64::pci::Device pci;
    welovec::x86_64::keyboard::VirtioKeyboardState keyboard_state;
    PointerState pointer_state;
    volatile uint8_t *common;
    uint32_t common_region_length;
    volatile uint8_t *notify_base;
    uint32_t notify_region_length;
    uint32_t notify_multiplier;
    volatile uint8_t *device_config;
    uint32_t device_config_size;
    int32_t absolute_min_x;
    int32_t absolute_max_x;
    int32_t absolute_min_y;
    int32_t absolute_max_y;
    bool has_keyboard;
    bool has_pointer;
    bool absolute_pointer;
    Mapping mappings[kMaximumMappings];
    uint32_t mapping_count;
    Queue queues[2];
    welovec::x86_64::mm::dma::Region event_buffers;
    bool active;
};

DeviceState gDevices[kMaximumDevices]{};
uint32_t gDeviceCount;
bool gInitialized;
bool gWorkerStarted;
bool gHasKeyboard;
bool gHasPointer;

uint16_t mmio_read16(volatile uint8_t *base, uint32_t offset) {
    return *reinterpret_cast<volatile uint16_t *>(base + offset);
}
uint32_t mmio_read32(volatile uint8_t *base, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t *>(base + offset);
}
void mmio_write8(volatile uint8_t *base, uint32_t offset, uint8_t value) {
    *reinterpret_cast<volatile uint8_t *>(base + offset) = value;
    mfence();
}
void mmio_write16(volatile uint8_t *base, uint32_t offset, uint16_t value) {
    *reinterpret_cast<volatile uint16_t *>(base + offset) = value;
    mfence();
}
void mmio_write32(volatile uint8_t *base, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
    mfence();
}
void mmio_write64(volatile uint8_t *base, uint32_t offset, uint64_t value) {
    *reinterpret_cast<volatile uint64_t *>(base + offset) = value;
    mfence();
}

void release_queue(Queue *queue) {
    if (!queue) return;
    welovec::x86_64::mm::dma::release(&queue->descriptors);
    welovec::x86_64::mm::dma::release(&queue->available);
    welovec::x86_64::mm::dma::release(&queue->used);
    *queue = {};
}

void release_device(DeviceState *device) {
    if (!device) return;
    if (device->common) {
        mmio_write8(device->common, 20, 0); // Reset before releasing DMA memory.
        for (uint32_t spin = 0; spin < 100000; ++spin) {
            if (*reinterpret_cast<volatile uint8_t *>(device->common + 20) == 0) break;
            __asm__ volatile("pause");
        }
    }
    for (uint32_t i = 0; i < device->mapping_count; ++i)
        welovec::memory::vmm::unmap_physical(
            device->mappings[i].address, device->mappings[i].size);
    release_queue(&device->queues[0]);
    release_queue(&device->queues[1]);
    welovec::x86_64::mm::dma::release(&device->event_buffers);
    *device = {};
}

volatile uint8_t *map_capability(DeviceState *device, uint8_t bar,
                                 uint32_t offset, uint32_t length) {
    if (!device || bar >= 6 || !length || length > kMaximumCapabilityMapping ||
        device->mapping_count == kMaximumMappings)
        return nullptr;
    const uint64_t bar_base = welovec::x86_64::pci::memory_bar_address(device->pci, bar);
    if (!bar_base || bar_base > UINT64_MAX - offset ||
        offset > UINT32_MAX - length) return nullptr;
    const uint64_t physical = bar_base + offset;
    void *mapping = welovec::memory::vmm::map_physical(
        welovec::x86_64::mm::PhysicalAddress(physical), length,
        welovec::memory::vmm::CachePolicy::Uncacheable);
    if (!mapping) return nullptr;
    device->mappings[device->mapping_count++] = {mapping, length};
    return static_cast<volatile uint8_t *>(mapping);
}

bool find_capabilities(DeviceState *state) {
    const uint16_t status = welovec::x86_64::pci::read16(state->pci, 0x06);
    if (!(status & (1u << 4))) return false;
    uint8_t pointer = welovec::x86_64::pci::read8(state->pci, 0x34) & 0xfcu;
    for (uint32_t count = 0; pointer && count < 48; ++count) {
        if (pointer < 0x40 || pointer > 0xf0) break;
        const uint8_t id = welovec::x86_64::pci::read8(state->pci, pointer);
        const uint8_t next = welovec::x86_64::pci::read8(state->pci, pointer + 1) & 0xfcu;
        const uint8_t capability_length =
            welovec::x86_64::pci::read8(state->pci, pointer + 2);
        if (id == kPciCapabilityId && capability_length >= 16) {
            const uint8_t type = welovec::x86_64::pci::read8(state->pci, pointer + 3);
            const uint8_t bar = welovec::x86_64::pci::read8(state->pci, pointer + 4);
            const uint32_t offset = welovec::x86_64::pci::read32(state->pci, pointer + 8);
            const uint32_t length = welovec::x86_64::pci::read32(state->pci, pointer + 12);
            if (type == kCapCommon && !state->common &&
                length >= kCommonConfigMinimum) {
                state->common = map_capability(state, bar, offset, length);
                state->common_region_length = length;
            } else if (type == kCapDevice && !state->device_config) {
                state->device_config = map_capability(state, bar, offset, length);
                state->device_config_size = length;
            } else if (type == kCapNotify && !state->notify_base &&
                       capability_length >= 20 && pointer <= 0xec && length >= 2) {
                state->notify_base = map_capability(state, bar, offset, length);
                state->notify_region_length = length;
                state->notify_multiplier =
                    welovec::x86_64::pci::read32(state->pci, pointer + 16);
            }
        }
        pointer = next;
    }
    return state->common && state->common_region_length >= kCommonConfigMinimum &&
           state->notify_base && state->device_config &&
           state->device_config_size >= kInputConfigDataOffset;
}

bool config_has_code(DeviceState *state, uint8_t event_type, uint32_t code) {
    volatile uint8_t *config = state->device_config;
    mmio_write8(config, 0, kConfigSelectEvBits);
    mmio_write8(config, 1, event_type);
    const uint8_t size = *reinterpret_cast<volatile uint8_t *>(config + 2);
    const uint32_t available = state->device_config_size - kInputConfigDataOffset;
    if (size > available || size > 128 || code / 8 >= size) return false;
    const uint8_t bits = *reinterpret_cast<volatile uint8_t *>(
        config + kInputConfigDataOffset + code / 8);
    return (bits & (1u << (code & 7u))) != 0;
}

bool read_absolute_range(DeviceState *state, uint8_t axis,
                        int32_t *minimum, int32_t *maximum) {
    volatile uint8_t *config = state->device_config;
    mmio_write8(config, 0, kConfigSelectAbsInfo);
    mmio_write8(config, 1, axis);
    const uint8_t size = *reinterpret_cast<volatile uint8_t *>(config + 2);
    if (size < 20 || state->device_config_size < kInputConfigDataOffset + 20)
        return false;
    *minimum = static_cast<int32_t>(mmio_read32(config, kInputConfigDataOffset));
    *maximum = static_cast<int32_t>(mmio_read32(config, kInputConfigDataOffset + 4));
    return *maximum > *minimum;
}

bool detect_device(DeviceState *state) {
    const bool key_a = config_has_code(state, kEvKey, kKeyA);
    const bool enter = config_has_code(state, kEvKey, kKeyEnter);
    const bool space = config_has_code(state, kEvKey, kKeySpace);
    state->has_keyboard = key_a && enter && space;

    const bool relative_x = config_has_code(state, kEvRel, kRelX);
    const bool relative_y = config_has_code(state, kEvRel, kRelY);
    const bool absolute_x = config_has_code(state, kEvAbs, kAbsX);
    const bool absolute_y = config_has_code(state, kEvAbs, kAbsY);
    const bool mouse_buttons = config_has_code(state, kEvKey, kBtnLeft) ||
        config_has_code(state, kEvKey, kBtnRight) ||
        config_has_code(state, kEvKey, kBtnMiddle) ||
        config_has_code(state, kEvKey, kBtnTouch) ||
        config_has_code(state, kEvKey, kBtnToolFinger);
    state->absolute_pointer = absolute_x && absolute_y &&
        read_absolute_range(state, kAbsX, &state->absolute_min_x,
                            &state->absolute_max_x) &&
        read_absolute_range(state, kAbsY, &state->absolute_min_y,
                            &state->absolute_max_y);
    state->has_pointer = (mouse_buttons && relative_x && relative_y) ||
                          state->absolute_pointer;
    return state->has_keyboard || state->has_pointer;
}

uint16_t choose_queue_size(uint16_t maximum) {
    uint16_t size = 1;
    const uint16_t limit = maximum < kQueueMaximum ? maximum : kQueueMaximum;
    while (static_cast<uint16_t>(size << 1) <= limit) size <<= 1;
    return size;
}

bool allocate_queue(DeviceState *device, uint16_t index, Queue *queue,
                    bool post_event_buffers) {
    volatile uint8_t *common = device->common;
    mmio_write16(common, 22, index); // queue_select
    const uint16_t maximum = mmio_read16(common, 24);
    const uint16_t size = choose_queue_size(maximum);
    if (!size) return false;

    using welovec::x86_64::mm::dma::Region;
    if (!welovec::x86_64::mm::dma::allocate(size * sizeof(DmaDescriptor),
                                             &queue->descriptors) ||
        !welovec::x86_64::mm::dma::allocate(sizeof(AvailableRing), &queue->available) ||
        !welovec::x86_64::mm::dma::allocate(sizeof(UsedRing), &queue->used)) return false;
    queue->size = size;
    queue->last_used = 0;
    queue->notify_offset = mmio_read16(common, 30);
    const uint64_t notify_byte_offset =
        static_cast<uint64_t>(queue->notify_offset) * device->notify_multiplier;
    if (notify_byte_offset > UINT32_MAX - 2 ||
        notify_byte_offset + 2 > device->notify_region_length) return false;
    queue->notify = reinterpret_cast<volatile uint16_t *>(
        device->notify_base + notify_byte_offset);

    if (post_event_buffers) {
        if (!welovec::x86_64::mm::dma::allocate(
                static_cast<uint64_t>(size) * kEventSize, &device->event_buffers)) return false;
        auto *descriptors = reinterpret_cast<DmaDescriptor *>(queue->descriptors.virtual_address);
        auto *available = reinterpret_cast<AvailableRing *>(queue->available.virtual_address);
        for (uint16_t i = 0; i < size; ++i) {
            descriptors[i] = {device->event_buffers.physical.value() +
                                  static_cast<uint64_t>(i) * kEventSize,
                              kEventSize, 2, 0}; // device-writable
            available->ring[i] = i;
        }
        available->index = size;
    }

    mmio_write16(common, 24, size);
    mmio_write16(common, 26, 0xffff); // No MSI-X vector; use polling.
    mmio_write64(common, 32, queue->descriptors.physical.value());
    mmio_write64(common, 40, queue->available.physical.value());
    mmio_write64(common, 48, queue->used.physical.value());
    mmio_write16(common, 28, 1);
    if (mmio_read16(common, 28) != 1) return false;
    if (post_event_buffers) {
        mfence();
        *queue->notify = 0;
        mfence();
    }
    return true;
}

bool initialize_device(const welovec::x86_64::pci::Device &pci_device) {
    if (gDeviceCount == kMaximumDevices ||
        !welovec::x86_64::pci::enable_memory_and_bus_master(pci_device)) return false;
    DeviceState *state = &gDevices[gDeviceCount];
    *state = {};
    state->pci = pci_device;
    if (!find_capabilities(state) || !detect_device(state)) {
        release_device(state);
        return false;
    }

    volatile uint8_t *common = state->common;
    mmio_write8(common, 20, 0); // Device reset.
    mmio_write8(common, 20, kStatusAcknowledge);
    mmio_write8(common, 20, kStatusAcknowledge | kStatusDriver);

    mmio_write32(common, 0, 0);
    const uint32_t feature_low = mmio_read32(common, 4);
    mmio_write32(common, 0, 1);
    const uint32_t feature_high = mmio_read32(common, 4);
    (void)feature_low;
    if (!(feature_high & kFeatureVersion1)) {
        release_device(state);
        return false;
    }
    mmio_write32(common, 8, 0);
    mmio_write32(common, 12, 0);
    mmio_write32(common, 8, 1);
    mmio_write32(common, 12, kFeatureVersion1);
    mmio_write8(common, 20, kStatusAcknowledge | kStatusDriver | kStatusFeaturesOk);
    if (!( *reinterpret_cast<volatile uint8_t *>(common + 20) & kStatusFeaturesOk)) {
        release_device(state);
        return false;
    }

    const uint16_t queues = mmio_read16(common, 18);
    if (queues < 2 || !allocate_queue(state, 0, &state->queues[0], true) ||
        !allocate_queue(state, 1, &state->queues[1], false)) {
        release_device(state);
        return false;
    }
    mmio_write8(common, 20, kStatusAcknowledge | kStatusDriver |
                           kStatusFeaturesOk | kStatusDriverOk);
    if (!(*reinterpret_cast<volatile uint8_t *>(common + 20) & kStatusDriverOk)) {
        release_device(state);
        return false;
    }
    state->active = true;
    ++gDeviceCount;
    return true;
}

bool pci_visitor(const welovec::x86_64::pci::Device &device, void *) {
    if (device.vendor_id != kVirtioVendor ||
        (device.device_id != kModernInputDevice &&
         device.device_id != kTransitionalInputDevice)) return false;
    (void)initialize_device(device);
    return false;
}

int32_t add_saturated(int32_t current, int32_t delta) {
    const int64_t sum = static_cast<int64_t>(current) + delta;
    if (sum > INT32_MAX) return INT32_MAX;
    if (sum < INT32_MIN) return INT32_MIN;
    return static_cast<int32_t>(sum);
}

void consume_pointer_event(DeviceState *device, uint16_t type,
                           uint16_t code, int32_t value) {
    if (!device->has_pointer) return;
    PointerState &state = device->pointer_state;
    if (type == kEvRel) {
        if (code == kRelX) state.delta_x = add_saturated(state.delta_x, value);
        else if (code == kRelY) state.delta_y = add_saturated(state.delta_y, value);
        else if (code == kRelWheel) state.wheel = add_saturated(state.wheel, value);
        else return;
        state.dirty = true;
        return;
    }
    if (type == kEvAbs) {
        if (code == kAbsX) state.absolute_x = value;
        else if (code == kAbsY) state.absolute_y = value;
        else return;
        state.dirty = true;
        state.absolute_dirty = true;
        return;
    }
    if (type == kEvKey) {
        uint32_t mask = 0;
        if (code == kBtnLeft) mask = WELOVEC_POINTER_BUTTON_LEFT;
        else if (code == kBtnRight) mask = WELOVEC_POINTER_BUTTON_RIGHT;
        else if (code == kBtnMiddle) mask = WELOVEC_POINTER_BUTTON_MIDDLE;
        else if (code == kBtnSide) mask = WELOVEC_POINTER_BUTTON_BACK;
        else if (code == kBtnExtra) mask = WELOVEC_POINTER_BUTTON_FORWARD;
        else if (code == kBtnTouch || code == kBtnToolFinger)
            mask = WELOVEC_POINTER_BUTTON_LEFT;
        if (!mask) return;
        if (value) state.buttons |= mask;
        else state.buttons &= ~mask;
        state.dirty = true;
        return;
    }
    if (type != kEvSyn || code != 0 || !state.dirty) return;

    if (device->absolute_pointer && state.absolute_dirty) {
        welovec::x86_64::input::pointer::submit_absolute(
            state.absolute_x, state.absolute_y,
            device->absolute_min_x, device->absolute_max_x,
            device->absolute_min_y, device->absolute_max_y,
            state.buttons, welovec::x86_64::input::pointer::Source::Virtio);
    } else {
        welovec::x86_64::input::pointer::submit_relative(
            state.delta_x, state.delta_y, state.wheel, state.buttons,
            welovec::x86_64::input::pointer::Source::Virtio);
    }
    state.delta_x = 0;
    state.delta_y = 0;
    state.wheel = 0;
    state.dirty = false;
    state.absolute_dirty = false;
}

void poll_device(DeviceState *device) {
    if (!device || !device->active) return;
    Queue &queue = device->queues[0];
    auto *used = reinterpret_cast<UsedRing *>(queue.used.virtual_address);
    auto *available = reinterpret_cast<AvailableRing *>(queue.available.virtual_address);
    mfence();
    while (queue.last_used != used->index) {
        const UsedElement item = used->ring[queue.last_used % queue.size];
        ++queue.last_used;
        if (item.id < queue.size) {
            if (item.length >= kEventSize) {
                const auto *event = reinterpret_cast<const uint32_t *>(
                    device->event_buffers.virtual_address +
                    static_cast<uint64_t>(item.id) * kEventSize);
                const uint16_t type = static_cast<uint16_t>(event[0]);
                const uint16_t code = static_cast<uint16_t>(event[0] >> 16);
                const int32_t value = static_cast<int32_t>(event[1]);
                welovec::x86_64::keyboard::submit_virtio_event(
                    &device->keyboard_state, type, code, value);
                consume_pointer_event(device, type, code, value);
            }
            const uint16_t index = available->index;
            available->ring[index % queue.size] = static_cast<uint16_t>(item.id);
            available->index = static_cast<uint16_t>(index + 1);
        }
    }
    mfence();
    *queue.notify = 0;
    mfence();
}

void input_worker(void *) {
    for (;;) {
        for (uint32_t i = 0; i < gDeviceCount; ++i) poll_device(&gDevices[i]);
        (void)welovec::x86_64::scheduler::sleep(1);
    }
}

}

bool initialize() {
    if (gInitialized) return (gHasKeyboard || gHasPointer) && gWorkerStarted;
    gInitialized = true;
    (void)welovec::x86_64::pci::enumerate(pci_visitor, nullptr);
    for (uint32_t i = 0; i < gDeviceCount; ++i) {
        gHasKeyboard |= gDevices[i].has_keyboard;
        gHasPointer |= gDevices[i].has_pointer;
    }
    if (gDeviceCount && !gWorkerStarted) {
        gWorkerStarted = welovec::x86_64::scheduler::create_kernel_thread(
            "virtio-input", input_worker, nullptr,
            welovec::x86_64::scheduler::Priority::Background) != 0;
    }
    if (!gWorkerStarted) {
        while (gDeviceCount) release_device(&gDevices[--gDeviceCount]);
        return false;
    }
    if (gHasKeyboard)
        welovec::x86_64::keyboard::register_source(
            welovec::x86_64::keyboard::Source::Virtio);
    if (gHasPointer)
        welovec::x86_64::input::pointer::register_source(
            welovec::x86_64::input::pointer::Source::Virtio);
    return gHasKeyboard;
}

}
