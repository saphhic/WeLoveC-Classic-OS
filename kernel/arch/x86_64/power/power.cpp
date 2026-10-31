// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements system reboot and shutdown operations.

#include "power.h"

#include "arch/x86_64/cpu/cpuid.h"
#include "arch/x86_64/io/io.h"
#include "arch/x86_64/pci/pci.hpp"
#include "include/boot/boot_protocol.h"

namespace {
const uint8_t *gRsdp = nullptr;

struct AcpiTableHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

struct AcpiDescription {
    const uint8_t *fadt;
    uint32_t length;
};

struct GenericAddress {
    uint8_t address_space;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
};

uint32_t read_u32(const uint8_t *bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t read_u64(const uint8_t *bytes) {
    return static_cast<uint64_t>(read_u32(bytes)) |
           (static_cast<uint64_t>(read_u32(bytes + 4)) << 32);
}

bool signature_is(const uint8_t *bytes, const char signature[4]) {
    return bytes && bytes[0] == static_cast<uint8_t>(signature[0]) &&
           bytes[1] == static_cast<uint8_t>(signature[1]) &&
           bytes[2] == static_cast<uint8_t>(signature[2]) &&
           bytes[3] == static_cast<uint8_t>(signature[3]);
}

bool checksum_valid(const uint8_t *bytes, uint32_t length) {
    if (!bytes || !length || length > 1024 * 1024) return false;
    uint8_t sum = 0;
    for (uint32_t index = 0; index < length; ++index) sum += bytes[index];
    return sum == 0;
}

bool acpi_table_valid(const uint8_t *table, const char signature[4]) {
    if (!signature_is(table, signature)) return false;
    const uint32_t length = read_u32(table + 4);
    return length >= sizeof(AcpiTableHeader) && checksum_valid(table, length);
}

bool find_fadt(AcpiDescription *out) {
    if (!gRsdp || !out) return false;
    static constexpr char rsdp_signature[] = "RSD PTR ";
    for (uint32_t index = 0; index < 8; ++index)
        if (gRsdp[index] != static_cast<uint8_t>(rsdp_signature[index])) return false;
    if (!checksum_valid(gRsdp, 20)) return false;

    uint64_t root_address = read_u32(gRsdp + 16);
    uint32_t entry_size = sizeof(uint32_t);
    if (gRsdp[15] >= 2) {
        const uint32_t rsdp_length = read_u32(gRsdp + 20);
        if (rsdp_length < 36 || rsdp_length > 4096 ||
            !checksum_valid(gRsdp, rsdp_length)) return false;
        const uint64_t xsdt = read_u64(gRsdp + 24);
        if (xsdt) {
            root_address = xsdt;
            entry_size = sizeof(uint64_t);
        }
    }
    if (!root_address) return false;
    const auto *root = reinterpret_cast<const uint8_t *>(root_address);
    const char *root_signature = entry_size == sizeof(uint64_t) ? "XSDT" : "RSDT";
    if (!acpi_table_valid(root, root_signature)) return false;
    const uint32_t root_length = read_u32(root + 4);
    const uint32_t entries = (root_length - sizeof(AcpiTableHeader)) / entry_size;
    if (entries > 256) return false;
    const uint8_t *entry = root + sizeof(AcpiTableHeader);
    for (uint32_t index = 0; index < entries; ++index) {
        const uint64_t address = entry_size == sizeof(uint64_t)
            ? read_u64(entry + static_cast<uint64_t>(index) * entry_size)
            : read_u32(entry + static_cast<uint64_t>(index) * entry_size);
        if (!address) continue;
        const auto *table = reinterpret_cast<const uint8_t *>(address);
        if (signature_is(table, "FACP") && acpi_table_valid(table, "FACP")) {
            out->fadt = table;
            out->length = read_u32(table + 4);
            return true;
        }
    }
    return false;
}

bool fadt_dsdt(const AcpiDescription &description, const uint8_t **out_dsdt) {
    if (!out_dsdt || description.length < 44) return false;
    uint64_t address = read_u32(description.fadt + 40);
    if (description.length >= 148) {
        const uint64_t extended = read_u64(description.fadt + 140);
        if (extended) address = extended;
    }
    if (!address) return false;
    const auto *table = reinterpret_cast<const uint8_t *>(address);
    if (!acpi_table_valid(table, "DSDT")) return false;
    *out_dsdt = table;
    return true;
}

bool parse_aml_integer(const uint8_t *aml, uint32_t length,
                       uint32_t *cursor, uint8_t *value) {
    if (!aml || !cursor || !value || *cursor >= length) return false;
    const uint8_t opcode = aml[(*cursor)++];
    if (opcode == 0x00 || opcode == 0x01) {
        *value = opcode;
        return true;
    }
    if (opcode == 0x0a && *cursor < length) {
        *value = aml[(*cursor)++];
        return true;
    }
    if (opcode == 0x0b && *cursor + 2 <= length) {
        *value = aml[*cursor];
        *cursor += 2;
        return true;
    }
    if (opcode == 0x0c && *cursor + 4 <= length) {
        *value = aml[*cursor];
        *cursor += 4;
        return true;
    }
    return false;
}

bool find_sleep_types(const uint8_t *dsdt, uint8_t *sleep_a, uint8_t *sleep_b) {
    if (!dsdt || !sleep_a || !sleep_b) return false;
    const uint32_t length = read_u32(dsdt + 4);
    const uint8_t *aml = dsdt + sizeof(AcpiTableHeader);
    const uint32_t aml_length = length - sizeof(AcpiTableHeader);
    for (uint32_t index = 0; index + 8 < aml_length; ++index) {
        if (aml[index] != 0x08 || aml[index + 1] != 0x5c ||
            aml[index + 2] != '_' || aml[index + 3] != 'S' ||
            aml[index + 4] != '5' || aml[index + 5] != '_') continue;
        uint32_t cursor = index + 6;
        if (aml[cursor++] != 0x12 || cursor >= aml_length) return false;
        const uint8_t package_length = aml[cursor++];
        if (package_length & 0xc0) return false;
        if (cursor >= aml_length || aml[cursor++] < 2) return false;
        return parse_aml_integer(aml, aml_length, &cursor, sleep_a) &&
               parse_aml_integer(aml, aml_length, &cursor, sleep_b);
    }
    return false;
}

GenericAddress read_gas(const uint8_t *bytes) {
    return GenericAddress{bytes[0], bytes[1], bytes[2], bytes[3], read_u64(bytes + 4)};
}

bool gas_is_8bit_register(const GenericAddress &gas) {
    return gas.address && gas.bit_width == 8 && gas.bit_offset == 0;
}

bool write_pci_config8(uint64_t address, uint8_t value) {
    // ACPI encodes PCI config addresses as bus/device/function/register.
    // The reset and sleep registers are restricted to bus zero.
    if ((address >> 48) != 0) return false;
    const uint16_t device_number = static_cast<uint16_t>((address >> 32) & 0xffffu);
    const uint16_t function_number = static_cast<uint16_t>((address >> 16) & 0xffffu);
    const uint16_t reg = static_cast<uint16_t>(address & 0xffffu);
    if (device_number >= 32 || function_number >= 8 || reg > 0xff) return false;
    welovec::x86_64::pci::Device device{
        0, static_cast<uint8_t>(device_number),
        static_cast<uint8_t>(function_number), 0, 0, 0, 0, 0
    };
    welovec::x86_64::pci::write8(device, static_cast<uint8_t>(reg), value);
    return true;
}

bool write_gas8(const GenericAddress &gas, uint8_t value) {
    if (!gas_is_8bit_register(gas)) return false;
    if (gas.address_space == 0) {
        *reinterpret_cast<volatile uint8_t *>(gas.address) = value;
        return true;
    }
    if (gas.address_space == 1 && gas.address <= UINT16_MAX) {
        outb(static_cast<uint16_t>(gas.address), value);
        return true;
    }
    if (gas.address_space == 2) return write_pci_config8(gas.address, value);
    return false;
}

bool write_pm1_gas(const uint8_t *bytes, uint8_t control_length,
                   uint8_t sleep_type) {
    const GenericAddress gas = read_gas(bytes);
    if (!gas.address || gas.bit_width < 16 || gas.bit_offset != 0 ||
        control_length < 2) return false;
    uint16_t current = 0;
    if (gas.address_space == 1 && gas.address <= UINT16_MAX - 1) {
        current = inw(static_cast<uint16_t>(gas.address));
        const uint16_t value = static_cast<uint16_t>(
            (current & ~static_cast<uint16_t>((7u << 10) | (1u << 13))) |
            ((sleep_type & 7u) << 10) | (1u << 13));
        outw(static_cast<uint16_t>(gas.address), value);
        return true;
    }
    if (gas.address_space == 0) {
        auto *reg = reinterpret_cast<volatile uint16_t *>(gas.address);
        current = *reg;
        *reg = static_cast<uint16_t>(
            (current & ~static_cast<uint16_t>((7u << 10) | (1u << 13))) |
            ((sleep_type & 7u) << 10) | (1u << 13));
        return true;
    }
    return false;
}

bool write_pm1_legacy(uint32_t address, uint8_t control_length,
                      uint8_t sleep_type) {
    if (!address || address > UINT16_MAX - 1 || control_length < 2) return false;
    const uint16_t port = static_cast<uint16_t>(address);
    const uint16_t current = inw(port);
    const uint16_t value = static_cast<uint16_t>(
        (current & ~static_cast<uint16_t>((7u << 10) | (1u << 13))) |
        ((sleep_type & 7u) << 10) | (1u << 13));
    outw(port, value);
    return true;
}

bool write_pm1_block(const AcpiDescription &description,
                     uint32_t gas_offset, uint32_t legacy_offset,
                     uint8_t control_length, uint8_t sleep_type) {
    if (description.length >= gas_offset + 12) {
        const GenericAddress gas = read_gas(description.fadt + gas_offset);
        if (gas.address && write_pm1_gas(description.fadt + gas_offset,
                                         control_length, sleep_type)) return true;
    }
    return write_pm1_legacy(read_u32(description.fadt + legacy_offset),
                            control_length, sleep_type);
}

bool write_reset_register(const AcpiDescription &description) {
    if (description.length < 129 ||
        !(read_u32(description.fadt + 112) & (1u << 10))) return false;
    const GenericAddress gas = read_gas(description.fadt + 116);
    const uint8_t value = description.fadt[128];
    return write_gas8(gas, value);
}

bool acpi_shutdown() {
    AcpiDescription description{};
    const uint8_t *dsdt = nullptr;
    uint8_t sleep_a = 0, sleep_b = 0;
    if (!find_fadt(&description) || !fadt_dsdt(description, &dsdt) ||
        !find_sleep_types(dsdt, &sleep_a, &sleep_b) || description.length < 116)
        return false;
    const uint32_t flags = read_u32(description.fadt + 112);
    if ((flags & (1u << 20)) && description.length >= 256) {
        const GenericAddress sleep_control = read_gas(description.fadt + 244);
        const uint8_t command = static_cast<uint8_t>(((sleep_a & 7u) << 2) | (1u << 5));
        return write_gas8(sleep_control, command);
    }

    const uint8_t control_length = description.fadt[89];
    const bool a_written = write_pm1_block(description, 172, 64,
                                            control_length, sleep_a);
    const bool b_written = write_pm1_block(description, 184, 68,
                                            control_length, sleep_b);
    return a_written || b_written;
}

bool virtual_machine_present() {
    uint32_t eax = 1, ebx = 0, ecx = 0, edx = 0;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    return (ecx & (1u << 31)) != 0;
}

void reset_wait() {
    for (uint32_t count = 0; count < 1000000; ++count)
        __asm__ volatile("pause" ::: "memory");
}

[[noreturn]] void halt_after_power_request() {
    for (;;) __asm__ volatile("cli; hlt" ::: "memory");
}

[[noreturn]] void triple_fault_reset() {
    struct __attribute__((packed)) IdtPointer {
        uint16_t limit;
        uint64_t base;
    } invalid_idt{0, 0};
    __asm__ volatile("cli; lidt %0; int3" : : "m"(invalid_idt) : "memory");
    for (;;) __asm__ volatile("cli; hlt");
}

[[noreturn]] void reboot_system(bool vm_present) {
    AcpiDescription description{};
    if (find_fadt(&description) && write_reset_register(description)) reset_wait();

    // Common chipset reset control used by PC-compatible firmware and VMs.
    outb(0xcf9, 0x02);
    io_wait();
    outb(0xcf9, 0x06);
    reset_wait();

    outb(0x64, 0xfe);
    reset_wait();

    if (vm_present) {
        outw(0x0604, 0x2000);
        outw(0xb004, 0x2000);
        outw(0x4004, 0x3400);
        reset_wait();
    }

    // Last-resort x86 reset: an exception with no valid IDT causes a CPU reset.
    triple_fault_reset();
}

bool shutdown_system(bool vm_present) {
    if (acpi_shutdown()) halt_after_power_request();
    if (!vm_present) return false;
    outw(0x0604, 0x2000);
    outw(0xb004, 0x2000);
    outw(0x4004, 0x3400);
    halt_after_power_request();
}
} // namespace

extern "C" void welovec_power_initialize(const WelovecBootInfo *boot_info) {
    gRsdp = boot_info && boot_info->magic == WELOVEC_BOOTINFO_MAGIC &&
                    boot_info->version == WELOVEC_BOOTINFO_VERSION &&
                    (boot_info->flags & WELOVEC_BOOT_FLAG_ACPI_VALID)
        ? reinterpret_cast<const uint8_t *>(boot_info->acpi_rsdp) : nullptr;
}

extern "C" int welovec_system_reboot(int virtual_machine) {
    reboot_system(virtual_machine || virtual_machine_present());
}

extern "C" int welovec_system_shutdown(int virtual_machine) {
    return shutdown_system(virtual_machine || virtual_machine_present());
}
