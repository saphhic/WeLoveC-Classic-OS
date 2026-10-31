// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Defines the shared registry and operations for block devices.

#include "block_device.hpp"

#include "arch/x86_64/mm/physical/physical_memory.hpp"
#include "arch/x86_64/mm/spinlock.hpp"

namespace welovec::block {

namespace {
using welovec::x86_64::mm::IrqSpinLock;

struct MemoryDisk {
    const uint8_t *bytes;
    uint8_t *mutable_bytes;
    uint64_t byte_count;
};

struct Registry {
    constexpr Registry() : devices{}, memory_disks{}, count(0), initialized(false), lock{} {}
    Device *devices[kMaximumDevices];
    MemoryDisk memory_disks[WELOVEC_BOOTINFO_MAX_MODULES];
    uint32_t count;
    bool initialized;
    IrqSpinLock lock;
};

constinit Registry gRegistry{};
Device gMemoryDevices[WELOVEC_BOOTINFO_MAX_MODULES]{};

bool memory_read(Device *device, uint64_t first, uint32_t count, void *buffer) {
    if (!device || !buffer || !device->context || !count) return false;
    auto *disk = static_cast<MemoryDisk *>(device->context);
    if (first > device->block_count || count > device->block_count - first) return false;
    const uint64_t offset = first * device->block_size;
    const uint64_t length = static_cast<uint64_t>(count) * device->block_size;
    auto *out = static_cast<uint8_t *>(buffer);
    for (uint64_t i = 0; i < length; ++i) out[i] = disk->bytes[offset + i];
    return true;
}

bool memory_write(Device *device, uint64_t first, uint32_t count, const void *buffer) {
    if (!device || !buffer || !device->context || device->read_only || !count) return false;
    auto *disk = static_cast<MemoryDisk *>(device->context);
    if (first > device->block_count || count > device->block_count - first) return false;
    const uint64_t offset = first * device->block_size;
    const uint64_t length = static_cast<uint64_t>(count) * device->block_size;
    const auto *in = static_cast<const uint8_t *>(buffer);
    for (uint64_t i = 0; i < length; ++i) disk->mutable_bytes[offset + i] = in[i];
    return true;
}

void copy_name(char *destination, const char *source) {
    uint32_t i = 0;
    while (source[i] && i + 1 < 32) {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

}

bool register_device(Device *device) {
    if (!device || !device->block_count || !device->block_size || !device->read) return false;
    const uint64_t irq = gRegistry.lock.lock_irqsave();
    if (gRegistry.count >= kMaximumDevices) {
        gRegistry.lock.unlock_irqrestore(irq);
        return false;
    }
    for (uint32_t i = 0; i < gRegistry.count; ++i) {
        if (gRegistry.devices[i] == device) {
            gRegistry.lock.unlock_irqrestore(irq);
            return false;
        }
    }
    gRegistry.devices[gRegistry.count++] = device;
    gRegistry.lock.unlock_irqrestore(irq);
    return true;
}

bool initialize(const WelovecBootInfo *boot_info) {
    if (gRegistry.initialized) return true;
    if (boot_info && boot_info->magic == WELOVEC_BOOTINFO_MAGIC &&
        boot_info->version == WELOVEC_BOOTINFO_VERSION) {
        uint32_t memory_index = 0;
        for (uint32_t i = 0; i < boot_info->module_count &&
             i < WELOVEC_BOOTINFO_MAX_MODULES && memory_index < WELOVEC_BOOTINFO_MAX_MODULES; ++i) {
            const WelovecModule &module = boot_info->modules[i];
            if (module.kind != WELOVEC_MODULE_FAT32_IMAGE || !module.address ||
                !module.size || module.address > UINT64_MAX - module.size) continue;
            welovec::x86_64::mm::PhysicalAddress physical(module.address);
            if (!welovec::x86_64::mm::physical::contains(physical, module.size)) continue;
            auto *bytes = static_cast<uint8_t *>(
                welovec::x86_64::mm::physical::direct_map(physical));
            if (!bytes) continue;

            MemoryDisk &memory = gRegistry.memory_disks[memory_index];
            Device &device = gMemoryDevices[memory_index];
            memory.bytes = bytes;
            memory.mutable_bytes = bytes;
            memory.byte_count = module.size;
            for (uint32_t j = 0; j < sizeof(device.name); ++j) device.name[j] = 0;
            if (memory_index < 10) {
                copy_name(device.name, "bootfs0");
                device.name[6] = static_cast<char>('0' + memory_index);
                device.name[7] = '\0';
            } else copy_name(device.name, "bootfs");
            device.context = &memory;
            device.block_size = 512;
            device.block_count = module.size / device.block_size;
            device.read_only = false;
            device.read = memory_read;
            device.write = memory_write;
            device.flush = nullptr;
            if (device.block_count && register_device(&device)) ++memory_index;
        }
    }
    ata_pio::initialize();
    const uint64_t irq = gRegistry.lock.lock_irqsave();
    gRegistry.initialized = true;
    gRegistry.lock.unlock_irqrestore(irq);
    return true;
}

uint32_t device_count() {
    const uint64_t irq = gRegistry.lock.lock_irqsave();
    const uint32_t result = gRegistry.count;
    gRegistry.lock.unlock_irqrestore(irq);
    return result;
}

Device *device_at(uint32_t index) {
    const uint64_t irq = gRegistry.lock.lock_irqsave();
    Device *result = index < gRegistry.count ? gRegistry.devices[index] : nullptr;
    gRegistry.lock.unlock_irqrestore(irq);
    return result;
}

bool read(Device *device, uint64_t first_block, uint32_t count, void *buffer) {
    if (!device || !device->read || !buffer || !count || first_block > device->block_count ||
        count > device->block_count - first_block) return false;
    return device->read(device, first_block, count, buffer);
}

bool write(Device *device, uint64_t first_block, uint32_t count, const void *buffer) {
    if (!device || !device->write || device->read_only || !buffer || !count ||
        first_block > device->block_count || count > device->block_count - first_block) return false;
    return device->write(device, first_block, count, buffer);
}

bool flush(Device *device) {
    return device && (!device->flush || device->flush(device));
}

}
