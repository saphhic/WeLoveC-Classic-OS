// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the virtio rng component in the rng module.

#include "virtio_rng.hpp"

#include "arch/x86_64/cpu/barrier.h"
#include "arch/x86_64/mm/dma/dma.hpp"
#include "arch/x86_64/mm/spinlock.hpp"
#include "arch/x86_64/pci/pci.hpp"
#include "arch/x86_64/scheduler/scheduler.hpp"
#include "arch/common/MM/VMM/vmm.hpp"
#include "arch/x86_64/mm/physical/physical_memory.hpp"

namespace welovec::x86_64::rng {

namespace {
constexpr uint16_t kVirtioVendor = 0x1af4;
constexpr uint16_t kModernRngDevice = 0x1044;
constexpr uint16_t kTransitionalRngDevice = 0x1005;
constexpr uint8_t kPciCapabilityId = 0x09;
constexpr uint8_t kCapCommon = 1;
constexpr uint8_t kCapNotify = 2;
constexpr uint32_t kFeatureVersion1 = 1;
constexpr uint8_t kStatusAcknowledge = 1;
constexpr uint8_t kStatusDriver = 2;
constexpr uint8_t kStatusFeaturesOk = 8;
constexpr uint8_t kStatusDriverOk = 4;
constexpr uint32_t kCommonConfigMinimum = 56;
constexpr uint32_t kMaximumMappings = 4;
constexpr uint32_t kMaximumCapabilityMapping = 0x10000;
constexpr uint32_t kRandomBufferSize = 256;
constexpr uint64_t kWaitTicks = 200;

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

struct UsedElement { uint32_t id; uint32_t length; };
struct UsedRing {
    uint16_t flags;
    volatile uint16_t index;
    UsedElement ring[128];
} __attribute__((packed));

struct Mapping { void *address; uint64_t size; };

struct Device {
    welovec::x86_64::pci::Device pci;
    volatile uint8_t *common;
    uint32_t common_region_length;
    volatile uint8_t *notify_base;
    uint32_t notify_region_length;
    uint32_t notify_multiplier;
    Mapping mappings[kMaximumMappings];
    uint32_t mapping_count;
    welovec::x86_64::mm::dma::Region descriptors;
    welovec::x86_64::mm::dma::Region available;
    welovec::x86_64::mm::dma::Region used;
    welovec::x86_64::mm::dma::Region random_bytes;
    volatile uint16_t *notify;
    uint16_t queue_size;
    uint16_t last_used;
    bool active;
};

Device gDevice{};
welovec::x86_64::mm::IrqSpinLock gLock;
bool gInitialized;

uint16_t read16(volatile uint8_t *base, uint32_t offset) {
    return *reinterpret_cast<volatile uint16_t *>(base + offset);
}
uint32_t read32(volatile uint8_t *base, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t *>(base + offset);
}
void write8(volatile uint8_t *base, uint32_t offset, uint8_t value) {
    *reinterpret_cast<volatile uint8_t *>(base + offset) = value;
    mfence();
}
void write16(volatile uint8_t *base, uint32_t offset, uint16_t value) {
    *reinterpret_cast<volatile uint16_t *>(base + offset) = value;
    mfence();
}
void write32(volatile uint8_t *base, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
    mfence();
}
void write64(volatile uint8_t *base, uint32_t offset, uint64_t value) {
    *reinterpret_cast<volatile uint64_t *>(base + offset) = value;
    mfence();
}

volatile uint8_t *map_capability(Device *device, uint8_t bar,
                                 uint32_t offset, uint32_t length) {
    if (!device || bar >= 6 || !length ||
        length > kMaximumCapabilityMapping ||
        device->mapping_count == kMaximumMappings)
        return nullptr;
    const uint64_t base = welovec::x86_64::pci::memory_bar_address(device->pci, bar);
    if (!base || base > UINT64_MAX - offset || offset > UINT32_MAX - length)
        return nullptr;
    void *mapping = welovec::memory::vmm::map_physical(
        welovec::x86_64::mm::PhysicalAddress(base + offset), length,
        welovec::memory::vmm::CachePolicy::Uncacheable);
    if (!mapping) return nullptr;
    device->mappings[device->mapping_count++] = {mapping, length};
    return static_cast<volatile uint8_t *>(mapping);
}

bool find_capabilities(Device *device) {
    const uint16_t status =
        welovec::x86_64::pci::read16(device->pci, 0x06);
    if (!(status & (1u << 4))) return false;
    uint8_t pointer = welovec::x86_64::pci::read8(device->pci, 0x34) & 0xfcu;
    for (uint32_t count = 0; pointer && count < 48; ++count) {
        if (pointer < 0x40 || pointer > 0xf0) break;
        const uint8_t id = welovec::x86_64::pci::read8(device->pci, pointer);
        const uint8_t next =
            welovec::x86_64::pci::read8(device->pci, pointer + 1) & 0xfcu;
        const uint8_t cap_length =
            welovec::x86_64::pci::read8(device->pci, pointer + 2);
        if (id == kPciCapabilityId && cap_length >= 16) {
            const uint8_t type =
                welovec::x86_64::pci::read8(device->pci, pointer + 3);
            const uint8_t bar =
                welovec::x86_64::pci::read8(device->pci, pointer + 4);
            const uint32_t offset =
                welovec::x86_64::pci::read32(device->pci, pointer + 8);
            const uint32_t length =
                welovec::x86_64::pci::read32(device->pci, pointer + 12);
            if (type == kCapCommon && !device->common &&
                length >= kCommonConfigMinimum) {
                device->common = map_capability(device, bar, offset, length);
                device->common_region_length = length;
            } else if (type == kCapNotify && !device->notify_base &&
                       cap_length >= 20 && pointer <= 0xec && length >= 2) {
                device->notify_base = map_capability(device, bar, offset, length);
                device->notify_region_length = length;
                device->notify_multiplier =
                    welovec::x86_64::pci::read32(device->pci, pointer + 16);
            }
        }
        pointer = next;
    }
    return device->common && device->notify_base;
}

void release_device(Device *device) {
    if (!device) return;
    if (device->common) {
        write8(device->common, 20, 0);
        for (uint32_t spin = 0; spin < 100000; ++spin) {
            if (*reinterpret_cast<volatile uint8_t *>(device->common + 20) == 0)
                break;
            __asm__ volatile("pause");
        }
    }
    for (uint32_t i = 0; i < device->mapping_count; ++i)
        welovec::memory::vmm::unmap_physical(
            device->mappings[i].address, device->mappings[i].size);
    using welovec::x86_64::mm::dma::release;
    release(&device->descriptors);
    release(&device->available);
    release(&device->used);
    release(&device->random_bytes);
    *device = {};
}

bool initialize_device(const welovec::x86_64::pci::Device &pci_device) {
    if (!welovec::x86_64::pci::enable_memory_and_bus_master(pci_device))
        return false;
    gDevice = {};
    gDevice.pci = pci_device;
    if (!find_capabilities(&gDevice)) {
        release_device(&gDevice);
        return false;
    }

    volatile uint8_t *common = gDevice.common;
    write8(common, 20, 0);
    write8(common, 20, kStatusAcknowledge);
    write8(common, 20, kStatusAcknowledge | kStatusDriver);

    write32(common, 0, 0);
    (void)read32(common, 4);
    write32(common, 0, 1);
    const uint32_t feature_high = read32(common, 4);
    if (!(feature_high & kFeatureVersion1)) {
        release_device(&gDevice);
        return false;
    }
    write32(common, 8, 0);
    write32(common, 12, 0);
    write32(common, 8, 1);
    write32(common, 12, kFeatureVersion1);
    write8(common, 20,
           kStatusAcknowledge | kStatusDriver | kStatusFeaturesOk);
    if (!(*reinterpret_cast<volatile uint8_t *>(common + 20) &
          kStatusFeaturesOk)) {
        release_device(&gDevice);
        return false;
    }

    if (read16(common, 18) < 1) {
        release_device(&gDevice);
        return false;
    }
    write16(common, 22, 0);
    const uint16_t maximum_queue = read16(common, 24);
    if (!maximum_queue) {
        release_device(&gDevice);
        return false;
    }
    uint16_t queue_size = maximum_queue < 8 ? maximum_queue : 8;
    uint16_t power_of_two = 1;
    while (static_cast<uint16_t>(power_of_two << 1) <= queue_size)
        power_of_two <<= 1;
    queue_size = power_of_two;

    using welovec::x86_64::mm::dma::allocate;
    if (!allocate(queue_size * sizeof(DmaDescriptor), &gDevice.descriptors) ||
        !allocate(sizeof(AvailableRing), &gDevice.available) ||
        !allocate(sizeof(UsedRing), &gDevice.used) ||
        !allocate(kRandomBufferSize, &gDevice.random_bytes)) {
        release_device(&gDevice);
        return false;
    }
    gDevice.queue_size = queue_size;
    auto *descriptors =
        reinterpret_cast<DmaDescriptor *>(gDevice.descriptors.virtual_address);
    auto *available =
        reinterpret_cast<AvailableRing *>(gDevice.available.virtual_address);
    descriptors[0] = {gDevice.random_bytes.physical.value(), kRandomBufferSize,
                      2, 0}; // Device writes entropy into this buffer.
    available->ring[0] = 0;
    available->index = 1;

    const uint16_t notify_offset = read16(common, 30);
    const uint64_t notify_byte_offset =
        static_cast<uint64_t>(notify_offset) * gDevice.notify_multiplier;
    if (notify_byte_offset > UINT32_MAX - 2 ||
        notify_byte_offset + 2 > gDevice.notify_region_length) {
        release_device(&gDevice);
        return false;
    }
    gDevice.notify = reinterpret_cast<volatile uint16_t *>(
        gDevice.notify_base + notify_byte_offset);

    write16(common, 24, queue_size);
    write16(common, 26, 0xffff);
    write64(common, 32, gDevice.descriptors.physical.value());
    write64(common, 40, gDevice.available.physical.value());
    write64(common, 48, gDevice.used.physical.value());
    write16(common, 28, 1);
    if (read16(common, 28) != 1) {
        release_device(&gDevice);
        return false;
    }
    write8(common, 20, kStatusAcknowledge | kStatusDriver |
                            kStatusFeaturesOk | kStatusDriverOk);
    if (!(*reinterpret_cast<volatile uint8_t *>(common + 20) &
          kStatusDriverOk)) {
        release_device(&gDevice);
        return false;
    }
    gDevice.active = true;
    mfence();
    *gDevice.notify = 0;
    mfence();
    return true;
}

bool pci_visitor(const welovec::x86_64::pci::Device &device, void *) {
    if (device.vendor_id != kVirtioVendor ||
        (device.device_id != kModernRngDevice &&
         device.device_id != kTransitionalRngDevice)) return false;
    return initialize_device(device);
}

void zero_bytes(uint8_t *output, uint64_t size) {
    if (!output) return;
    for (uint64_t i = 0; i < size; ++i) output[i] = 0;
}
}

bool fill_from_virtio(uint8_t *output, uint64_t size) {
    if (!output && size) return false;
    if (!size) return true;
    if (size > UINT32_MAX) return false;
    if (!gInitialized) {
        gInitialized = true;
        (void)welovec::x86_64::pci::enumerate(pci_visitor, nullptr);
    }
    if (!gDevice.active) return false;

    auto *used = reinterpret_cast<UsedRing *>(gDevice.used.virtual_address);
    auto *available =
        reinterpret_cast<AvailableRing *>(gDevice.available.virtual_address);
    uint64_t copied = 0;
    const uint64_t start_tick =
        welovec::x86_64::scheduler::tick_count();
    while (copied < size) {
        bool completed = false;
        const uint64_t irq = gLock.lock_irqsave();
        mfence();
        if (gDevice.active && gDevice.last_used != used->index) {
            const UsedElement item = used->ring[gDevice.last_used % gDevice.queue_size];
            ++gDevice.last_used;
            if (item.id == 0 && item.length) {
                uint64_t amount = item.length;
                if (amount > kRandomBufferSize) amount = kRandomBufferSize;
                if (amount > size - copied) amount = size - copied;
                const uint8_t *source = gDevice.random_bytes.virtual_address;
                for (uint64_t i = 0; i < amount; ++i)
                    output[copied + i] = source[i];
                copied += amount;
                const uint16_t available_index = available->index;
                available->ring[available_index % gDevice.queue_size] = 0;
                available->index = static_cast<uint16_t>(available_index + 1);
                mfence();
                *gDevice.notify = 0;
                mfence();
                completed = true;
            }
        }
        gLock.unlock_irqrestore(irq);
        if (completed) continue;

        const uint64_t now = welovec::x86_64::scheduler::tick_count();
        if (now - start_tick >= kWaitTicks) {
            zero_bytes(output, size);
            return false;
        }
        if (!welovec::x86_64::scheduler::sleep(1)) {
            welovec::x86_64::scheduler::yield();
            __asm__ volatile("pause");
        }
    }
    return true;
}

}
