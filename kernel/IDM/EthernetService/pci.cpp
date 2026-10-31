// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements PCI access used by the ethernet service component.


//written by saphhic.
#include "pci.h"
#include "../../arch/x86_64/io/io.h"

namespace {

constexpr uint16_t kPciConfigAddressPort = 0xCF8;
constexpr uint16_t kPciConfigDataPort = 0xCFC;

static uint32_t pci_config_address(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    return 0x80000000u |
           (static_cast<uint32_t>(bus) << 16) |
           (static_cast<uint32_t>(slot) << 11) |
           (static_cast<uint32_t>(function) << 8) |
           (static_cast<uint32_t>(offset) & 0xFCu);
}

static uint8_t pci_function_limit(uint8_t bus, uint8_t slot) {
    const uint16_t vendor_id = eth_pci_read_word(bus, slot, 0, 0x00);
    if (vendor_id == 0xFFFFu) {
        return 0;
    }

    const uint8_t header_type = eth_pci_read_byte(bus, slot, 0, 0x0E);
    return (header_type & 0x80u) != 0 ? 8 : 1;
}

} // namespace

uint32_t eth_pci_read_dword(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    outl(kPciConfigAddressPort, pci_config_address(bus, slot, function, offset));
    return inl(kPciConfigDataPort);
}

uint16_t eth_pci_read_word(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    const uint32_t value = eth_pci_read_dword(bus, slot, function, static_cast<uint8_t>(offset & 0xFCu));
    const uint32_t shift = static_cast<uint32_t>((offset & 0x02u) * 8u);
    return static_cast<uint16_t>((value >> shift) & 0xFFFFu);
}

uint8_t eth_pci_read_byte(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    const uint32_t value = eth_pci_read_dword(bus, slot, function, static_cast<uint8_t>(offset & 0xFCu));
    const uint32_t shift = static_cast<uint32_t>((offset & 0x03u) * 8u);
    return static_cast<uint8_t>((value >> shift) & 0xFFu);
}

void eth_pci_write_dword(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value) {
    outl(kPciConfigAddressPort, pci_config_address(bus, slot, function, offset));
    outl(kPciConfigDataPort, value);
}

void eth_pci_write_word(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value) {
    const uint8_t aligned_offset = static_cast<uint8_t>(offset & 0xFCu);
    const uint32_t shift = static_cast<uint32_t>((offset & 0x02u) * 8u);
    uint32_t current_value = eth_pci_read_dword(bus, slot, function, aligned_offset);
    current_value &= ~(0xFFFFu << shift);
    current_value |= static_cast<uint32_t>(value) << shift;
    eth_pci_write_dword(bus, slot, function, aligned_offset, current_value);
}

bool eth_pci_read_device(uint8_t bus, uint8_t slot, uint8_t function, eth_pci_device* out_device) {
    if (out_device == nullptr) {
        return false;
    }

    const uint16_t vendor_id = eth_pci_read_word(bus, slot, function, 0x00);
    if (vendor_id == 0xFFFFu) {
        return false;
    }

    out_device->bus = bus;
    out_device->slot = slot;
    out_device->function = function;
    out_device->vendor_id = vendor_id;
    out_device->device_id = eth_pci_read_word(bus, slot, function, 0x02);
    out_device->command = eth_pci_read_word(bus, slot, function, 0x04);
    out_device->status = eth_pci_read_word(bus, slot, function, 0x06);
    out_device->revision_id = eth_pci_read_byte(bus, slot, function, 0x08);
    out_device->prog_if = eth_pci_read_byte(bus, slot, function, 0x09);
    out_device->subclass = eth_pci_read_byte(bus, slot, function, 0x0A);
    out_device->class_code = eth_pci_read_byte(bus, slot, function, 0x0B);
    out_device->cache_line_size = eth_pci_read_byte(bus, slot, function, 0x0C);
    out_device->latency_timer = eth_pci_read_byte(bus, slot, function, 0x0D);
    out_device->header_type = eth_pci_read_byte(bus, slot, function, 0x0E);
    out_device->bist = eth_pci_read_byte(bus, slot, function, 0x0F);

    for (uint8_t bar_index = 0; bar_index < 6; ++bar_index) {
        out_device->bar[bar_index] = eth_pci_read_dword(
            bus,
            slot,
            function,
            static_cast<uint8_t>(0x10u + (bar_index * 4u))
        );
    }

    out_device->subsystem_vendor_id = eth_pci_read_word(bus, slot, function, 0x2C);
    out_device->subsystem_id = eth_pci_read_word(bus, slot, function, 0x2E);
    out_device->interrupt_line = eth_pci_read_byte(bus, slot, function, 0x3C);
    out_device->interrupt_pin = eth_pci_read_byte(bus, slot, function, 0x3D);
    return true;
}

bool eth_pci_for_each_device(eth_pci_device_visitor visitor, void* context) {
    if (visitor == nullptr) {
        return false;
    }

    eth_pci_device candidate = {};
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            const uint8_t function_limit = pci_function_limit(static_cast<uint8_t>(bus), slot);
            for (uint8_t function = 0; function < function_limit; ++function) {
                if (!eth_pci_read_device(static_cast<uint8_t>(bus), slot, function, &candidate)) {
                    continue;
                }

                if (visitor(candidate, context)) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool eth_pci_find_device(uint16_t vendor_id, uint16_t device_id, eth_pci_device* out_device) {
    eth_pci_device candidate = {};

    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            const uint8_t function_limit = pci_function_limit(static_cast<uint8_t>(bus), slot);
            for (uint8_t function = 0; function < function_limit; ++function) {
                if (!eth_pci_read_device(static_cast<uint8_t>(bus), slot, function, &candidate)) {
                    continue;
                }

                if (candidate.vendor_id == vendor_id && candidate.device_id == device_id) {
                    *out_device = candidate;
                    return true;
                }
            }
        }
    }

    return false;
}

bool eth_pci_find_class(uint8_t class_code, uint8_t subclass, eth_pci_device* out_device) {
    eth_pci_device candidate = {};

    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            const uint8_t function_limit = pci_function_limit(static_cast<uint8_t>(bus), slot);
            for (uint8_t function = 0; function < function_limit; ++function) {
                if (!eth_pci_read_device(static_cast<uint8_t>(bus), slot, function, &candidate)) {
                    continue;
                }

                if (candidate.class_code == class_code && candidate.subclass == subclass) {
                    *out_device = candidate;
                    return true;
                }
            }
        }
    }

    return false;
}

void eth_pci_set_command_bits(const eth_pci_device* device, uint16_t command_bits) {
    if (device == nullptr) {
        return;
    }

    const uint16_t current_command = eth_pci_read_word(device->bus, device->slot, device->function, 0x04);
    const uint16_t next_command = static_cast<uint16_t>(current_command | command_bits);
    if (next_command != current_command) {
        eth_pci_write_word(device->bus, device->slot, device->function, 0x04, next_command);
    }
}

uint32_t eth_pci_get_io_bar_base(const eth_pci_device* device, uint8_t bar_index) {
    if (device == nullptr || bar_index >= 6) {
        return 0;
    }

    const uint32_t raw_bar = device->bar[bar_index];
    if ((raw_bar & 0x01u) == 0) {
        return 0;
    }

    return raw_bar & ~0x03u;
}

uint32_t eth_pci_get_memory_bar_base(const eth_pci_device* device, uint8_t bar_index) {
    if (device == nullptr || bar_index >= 6) {
        return 0;
    }

    const uint32_t raw_bar = device->bar[bar_index];
    if ((raw_bar & 0x01u) != 0) {
        return 0;
    }

    const uint32_t memory_type = raw_bar & 0x06u;
    if (memory_type == 0x04u) {
        if (bar_index + 1 >= 6 || device->bar[bar_index + 1] != 0) {
            return 0;
        }
    } else if (memory_type == 0x02u) {
        return 0;
    }

    return raw_bar & ~0x0Fu;
}
