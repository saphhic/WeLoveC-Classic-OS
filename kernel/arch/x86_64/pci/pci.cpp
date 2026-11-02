// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements PCI access used by the pci component.

#include "pci.hpp"

#include "arch/x86_64/io/io.h"
#include "arch/x86_64/mm/spinlock.hpp"

namespace welovec::x86_64::pci {

namespace {
constexpr uint16_t kAddressPort = 0xcf8;
constexpr uint16_t kDataPort = 0xcfc;
welovec::x86_64::mm::IrqSpinLock gConfigLock;

uint32_t address(const Device &device, uint8_t offset) {
    return UINT32_C(0x80000000) |
        (static_cast<uint32_t>(device.bus) << 16) |
        (static_cast<uint32_t>(device.slot) << 11) |
        (static_cast<uint32_t>(device.function) << 8) |
        (offset & 0xfcu);
}

uint32_t read32_locked(const Device &device, uint8_t offset) {
    outl(kAddressPort, address(device, offset));
    return inl(kDataPort);
}

uint8_t function_count(uint8_t bus, uint8_t slot) {
    Device device{bus, slot, 0, 0, 0, 0, 0, 0};
    const uint64_t irq = gConfigLock.lock_irqsave();
    const uint16_t vendor = static_cast<uint16_t>(read32_locked(device, 0));
    if (vendor == 0xffff) {
        gConfigLock.unlock_irqrestore(irq);
        return 0;
    }
    const uint8_t header = static_cast<uint8_t>(read32_locked(device, 0x0c) >> 16);
    gConfigLock.unlock_irqrestore(irq);
    return (header & 0x80) ? 8 : 1;
}

bool read_device(uint8_t bus, uint8_t slot, uint8_t function, Device *out) {
    if (!out) return false;
    Device device{bus, slot, function, 0, 0, 0, 0, 0};
    const uint64_t irq = gConfigLock.lock_irqsave();
    const uint32_t id = read32_locked(device, 0x00);
    if (static_cast<uint16_t>(id) == 0xffff) {
        gConfigLock.unlock_irqrestore(irq);
        return false;
    }
    const uint32_t class_revision = read32_locked(device, 0x08);
    device.vendor_id = static_cast<uint16_t>(id);
    device.device_id = static_cast<uint16_t>(id >> 16);
    device.prog_if = static_cast<uint8_t>(class_revision >> 8);
    device.subclass = static_cast<uint8_t>(class_revision >> 16);
    device.class_code = static_cast<uint8_t>(class_revision >> 24);
    gConfigLock.unlock_irqrestore(irq);
    *out = device;
    return true;
}
}

bool enumerate(Visitor visitor, void *context) {
    if (!visitor) return false;
    bool stopped = false;
    for (uint16_t bus = 0; bus < 256 && !stopped; ++bus) {
        for (uint8_t slot = 0; slot < 32 && !stopped; ++slot) {
            const uint8_t count = function_count(static_cast<uint8_t>(bus), slot);
            for (uint8_t function = 0; function < count; ++function) {
                Device device{};
                if (read_device(static_cast<uint8_t>(bus), slot, function, &device) &&
                    visitor(device, context)) {
                    stopped = true;
                    break;
                }
            }
        }
    }
    return stopped;
}

uint8_t read8(const Device &device, uint8_t offset) {
    const uint64_t irq = gConfigLock.lock_irqsave();
    const uint32_t value = read32_locked(device, offset);
    gConfigLock.unlock_irqrestore(irq);
    return static_cast<uint8_t>(value >> ((offset & 3u) * 8));
}

uint16_t read16(const Device &device, uint8_t offset) {
    const uint64_t irq = gConfigLock.lock_irqsave();
    const uint32_t value = read32_locked(device, offset);
    gConfigLock.unlock_irqrestore(irq);
    return static_cast<uint16_t>(value >> ((offset & 2u) * 8));
}

uint32_t read32(const Device &device, uint8_t offset) {
    const uint64_t irq = gConfigLock.lock_irqsave();
    const uint32_t value = read32_locked(device, offset);
    gConfigLock.unlock_irqrestore(irq);
    return value;
}

void write8(const Device &device, uint8_t offset, uint8_t value) {
    const uint64_t irq = gConfigLock.lock_irqsave();
    outl(kAddressPort, address(device, offset));
    outb(static_cast<uint16_t>(kDataPort + (offset & 3u)), value);
    gConfigLock.unlock_irqrestore(irq);
}

void write16(const Device &device, uint8_t offset, uint16_t value) {
    const uint64_t irq = gConfigLock.lock_irqsave();
    const uint8_t aligned = offset & 0xfcu;
    const uint32_t shift = (offset & 2u) * 8u;
    uint32_t current = read32_locked(device, aligned);
    current = (current & ~(UINT32_C(0xffff) << shift)) |
              (static_cast<uint32_t>(value) << shift);
    outl(kAddressPort, address(device, aligned));
    outl(kDataPort, current);
    gConfigLock.unlock_irqrestore(irq);
}

void write32(const Device &device, uint8_t offset, uint32_t value) {
    const uint64_t irq = gConfigLock.lock_irqsave();
    outl(kAddressPort, address(device, offset));
    outl(kDataPort, value);
    gConfigLock.unlock_irqrestore(irq);
}

uint64_t memory_bar_address(const Device &device, uint8_t index) {
    if (index >= 6) return 0;
    const uint32_t low = read32(device, static_cast<uint8_t>(0x10 + index * 4));
    if (low & 1u) return 0; // I/O BAR
    const uint8_t type = static_cast<uint8_t>((low >> 1) & 3u);
    uint64_t result = low & ~UINT32_C(0xf);
    if (type == 2) {
        if (index == 5) return 0;
        const uint32_t high = read32(device, static_cast<uint8_t>(0x14 + index * 4));
        result |= static_cast<uint64_t>(high) << 32;
    } else if (type != 0) return 0;
    return result;
}

bool read_bars(const Device &device, Bar out[kMaximumBars], uint8_t *out_count) {
    if (!out || !out_count) return false;
    for (uint8_t i = 0; i < kMaximumBars; ++i)
        out[i] = Bar{i, BarType::Absent, false, false, 0};

    const uint64_t irq = gConfigLock.lock_irqsave();
    const uint8_t header_type = static_cast<uint8_t>(
        (read32_locked(device, 0x0c) >> 16) & 0x7f);
    uint8_t bar_slots = 0;
    switch (header_type) {
        case 0: bar_slots = 6; break; // endpoint
        case 1: bar_slots = 2; break; // PCI-to-PCI bridge
        case 2: bar_slots = 1; break; // CardBus bridge
        default:
            *out_count = 0;
            gConfigLock.unlock_irqrestore(irq);
            return false;
    }

    for (uint8_t index = 0; index < bar_slots; ++index) {
        const uint8_t offset = static_cast<uint8_t>(0x10 + index * 4);
        const uint32_t low = read32_locked(device, offset);
        if (low == 0) continue;

        if (low & 1u) {
            const uint64_t address = low & ~UINT32_C(0x3);
            out[index] = Bar{index, BarType::Io, address != 0, false, address};
            continue;
        }

        const uint8_t memory_type = static_cast<uint8_t>((low >> 1) & 3u);
        const bool prefetchable = (low & (1u << 3)) != 0;
        if (memory_type == 0) {
            const uint64_t address = low & ~UINT32_C(0xf);
            out[index] = Bar{index, BarType::Memory32, address != 0,
                             prefetchable, address};
        } else if (memory_type == 1) {
            const uint64_t address = low & UINT32_C(0x000ffff0);
            out[index] = Bar{index, BarType::MemoryBelow1MiB, address != 0,
                             prefetchable, address};
        } else if (memory_type == 2 && index + 1 < bar_slots) {
            const uint32_t high = read32_locked(
                device, static_cast<uint8_t>(offset + 4));
            const uint64_t address =
                (static_cast<uint64_t>(high) << 32) |
                (low & ~UINT32_C(0xf));
            out[index] = Bar{index, BarType::Memory64, address != 0,
                             prefetchable, address};
            ++index;
            out[index] = Bar{index, BarType::Continuation, false, false, 0};
        } else {
            out[index] = Bar{index, BarType::Reserved, false,
                             prefetchable, 0};
        }
    }

    *out_count = bar_slots;
    gConfigLock.unlock_irqrestore(irq);
    return true;
}

bool enable_memory_and_bus_master(const Device &device) {
    const uint16_t command = read16(device, 0x04);
    write16(device, 0x04, static_cast<uint16_t>(command | (1u << 1) | (1u << 2)));
    const uint16_t updated = read16(device, 0x04);
    return (updated & ((1u << 1) | (1u << 2))) == ((1u << 1) | (1u << 2));
}

}
