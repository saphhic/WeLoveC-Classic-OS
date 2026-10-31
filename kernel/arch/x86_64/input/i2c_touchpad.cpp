// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Discovers and reads I²C-connected HID touchpads.

#include "i2c_touchpad.hpp"

#include "hid_touchpad.hpp"
#include "mouse.hpp"
#include "arch/common/MM/VMM/vmm.hpp"
#include "arch/x86_64/mm/physical/physical_address.hpp"
#include "arch/x86_64/scheduler/scheduler.hpp"
#include "include/boot/boot_protocol.h"

namespace {

using welovec::x86_64::input::hid_touchpad::State;

constexpr uint32_t kMaximumAcpiTableBytes = 1024 * 1024;
constexpr uint32_t kMaximumAcpiTables = 256;
constexpr uint32_t kMaximumDevices = 160;
constexpr uint32_t kMaximumDsmAddresses = 16;
constexpr uint32_t kMaximumHidReportBytes = 4096;
constexpr uint32_t kMaximumI2cTransfer = 4096;
constexpr uint32_t kMmioBytes = 0x1000;
constexpr uint32_t kDwWaitIterations = 50000;
constexpr uint32_t kPollIntervalTicks = 2;
constexpr uint32_t kPollErrorBackoffTicks = 50;
constexpr uint32_t kHidResetTimeoutTicks = 500; // HID-over-I2C allows up to 5 s.
constexpr uint64_t kSchedulerWaitSpinLimit = 100000000;

enum class TouchpadFailureStage : uint8_t {
    None,
    DescriptorRead,
    DescriptorInvalid,
    PowerOn,
    ResetCommand,
    ResetTimeout,
    ReportRead,
    ReportInvalid
};

struct AcpiDevice {
    const uint8_t *begin;
    const uint8_t *end;
    const uint8_t *crs;
    uint32_t crs_length;
    char name[4];
    char hid[9];
    char cid[9];
    uint16_t dsm_addresses[kMaximumDsmAddresses];
    uint16_t i2c_addresses[kMaximumDsmAddresses];
    uint8_t dsm_address_count;
    uint8_t i2c_address_count;
    bool has_crs;
};

enum class ProbeStatus : uint8_t {
    NotStarted,
    BootInfoInvalid,
    AcpiPointerMissing,
    RsdpInvalid,
    RootTableInvalid,
    FadtMissing,
    DsdtInvalid,
    NoHidDevice,
    NoI2cResource,
    NoSupportedI2cController,
    ControllerUnavailable,
    TouchpadUnavailable,
    WorkerUnavailable,
    Ready
};

struct AcpiTables {
    const uint8_t *aml[kMaximumAcpiTables];
    uint32_t lengths[kMaximumAcpiTables];
    uint32_t count;
};

struct I2cResource {
    uint16_t address;
    uint32_t frequency;
    char source[4];
    bool source_valid;
    bool address_10_bit;
    bool address_valid;
    bool frequency_valid;
    bool valid;
};

struct HidI2cDescriptor {
    uint16_t length;
    uint16_t version;
    uint16_t report_length;
    uint16_t report_register;
    uint16_t input_register;
    uint16_t maximum_input_length;
    uint16_t output_register;
    uint16_t maximum_output_length;
    uint16_t command_register;
    uint16_t data_register;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version;
};

struct DeviceState {
    volatile uint8_t *mmio;
    void *mapping;
    uint16_t address;
    uint16_t max_input;
    uint16_t input_register;
    bool initialized;
    uint8_t consecutive_errors;
    State hid;
};

AcpiDevice gDevices[kMaximumDevices]{};
uint32_t gDeviceCount;
DeviceState gTouchpad{};
uint8_t gReportDescriptor[kMaximumHidReportBytes]{};
char gNoResourceHid[9]{};
char gNoResourceName[5]{};
char gNoResourceStatus[88]{};
char gTouchpadFailureStatus[112]{};
bool gNoResourceHasCrs;
bool gAddressScanUsed;
bool gSchedulerTickWaitFailed;
TouchpadFailureStage gTouchpadFailureStage = TouchpadFailureStage::None;
uint16_t gTouchpadFailureAddress;
uint16_t gTouchpadFailureRegister;
bool gInitialized;
bool gWorkerStarted;
ProbeStatus gProbeStatus = ProbeStatus::NotStarted;

bool acpi_failure(ProbeStatus status) {
    gProbeStatus = status;
    return false;
}

uint16_t read_u16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t read_u32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64(const uint8_t *p) {
    return static_cast<uint64_t>(read_u32(p)) |
           (static_cast<uint64_t>(read_u32(p + 4)) << 32);
}

bool signature_is(const uint8_t *p, const char signature[4]) {
    return p && p[0] == static_cast<uint8_t>(signature[0]) &&
           p[1] == static_cast<uint8_t>(signature[1]) &&
           p[2] == static_cast<uint8_t>(signature[2]) &&
           p[3] == static_cast<uint8_t>(signature[3]);
}

bool checksum_valid(const uint8_t *p, uint32_t length) {
    if (!p || length < 20 || length > kMaximumAcpiTableBytes) return false;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; ++i) sum += p[i];
    return sum == 0;
}

bool acpi_table_valid(const uint8_t *table, const char signature[4]) {
    if (!signature_is(table, signature)) return false;
    const uint32_t length = read_u32(table + 4);
    return length >= 36 && checksum_valid(table, length);
}

bool parse_pkg_length(const uint8_t *aml, uint32_t length,
                      uint32_t offset, uint32_t *value,
                      uint32_t *encoded_bytes) {
    if (!aml || !value || !encoded_bytes || offset >= length) return false;
    const uint8_t first = aml[offset];
    const uint32_t follow = first >> 6;
    if (follow > 3 || follow > length - offset - 1) return false;
    uint32_t result = follow ? (first & 0x0fu) : (first & 0x3fu);
    for (uint32_t i = 0; i < follow; ++i)
        result |= static_cast<uint32_t>(aml[offset + 1 + i]) << (4 + 8 * i);
    const uint32_t bytes = 1 + follow;
    if (result < bytes || result > length - offset) return false;
    *value = result;
    *encoded_bytes = bytes;
    return true;
}

bool valid_name_char(uint8_t c, bool first) {
    if (c == '_') return true;
    if (c >= 'A' && c <= 'Z') return true;
    return !first && c >= '0' && c <= '9';
}

bool parse_namestring(const uint8_t *aml, uint32_t length, uint32_t *cursor,
                      char last_segment[4]) {
    if (!aml || !cursor || !last_segment || *cursor >= length) return false;
    while (*cursor < length &&
           (aml[*cursor] == 0x5c || aml[*cursor] == 0x5e)) ++*cursor;
    if (*cursor >= length) return false;
    uint32_t segments = 1;
    if (aml[*cursor] == 0x2e) {
        ++*cursor;
        segments = 2;
    } else if (aml[*cursor] == 0x2f) {
        ++*cursor;
        if (*cursor >= length) return false;
        segments = aml[(*cursor)++];
        if (!segments || segments > 16) return false;
    }
    if (segments > (length - *cursor) / 4) return false;
    for (uint32_t segment = 0; segment < segments; ++segment) {
        const uint8_t *name = aml + *cursor;
        for (uint32_t i = 0; i < 4; ++i)
            if (!valid_name_char(name[i], i == 0)) return false;
        if (segment == segments - 1)
            for (uint32_t i = 0; i < 4; ++i)
                last_segment[i] = static_cast<char>(name[i]);
        *cursor += 4;
    }
    return true;
}

bool collect_aml_table(const uint8_t *table, const char signature[4],
                       AcpiTables *out) {
    if (!out || !acpi_table_valid(table, signature) ||
        out->count == kMaximumAcpiTables) return false;
    const uint32_t length = read_u32(table + 4);
    out->aml[out->count] = table + 36;
    out->lengths[out->count] = length - 36;
    ++out->count;
    return true;
}

bool parse_i2c_resource(const AcpiDevice &device, I2cResource *out);
bool parse_controller_memory(const AcpiDevice &device, uint64_t *base,
                             uint64_t *length_out);
bool parse_buffer_object(const uint8_t *aml, uint32_t aml_length,
                         uint32_t offset, const uint8_t **buffer,
                         uint32_t *buffer_length);

bool find_aml_tables(const WelovecBootInfo *boot_info, AcpiTables *out) {
    if (!boot_info || !out || boot_info->size < sizeof(WelovecBootInfo) ||
        boot_info->magic != WELOVEC_BOOTINFO_MAGIC ||
        boot_info->version != WELOVEC_BOOTINFO_VERSION)
        return acpi_failure(ProbeStatus::BootInfoInvalid);
    if (!(boot_info->flags & WELOVEC_BOOT_FLAG_ACPI_VALID) ||
        !boot_info->acpi_rsdp)
        return acpi_failure(ProbeStatus::AcpiPointerMissing);
    const auto *rsdp = reinterpret_cast<const uint8_t *>(boot_info->acpi_rsdp);
    static constexpr char rsdp_signature[] = "RSD PTR ";
    for (uint32_t i = 0; i < 8; ++i)
        if (rsdp[i] != static_cast<uint8_t>(rsdp_signature[i]))
            return acpi_failure(ProbeStatus::RsdpInvalid);
    uint8_t rsdp_sum = 0;
    for (uint32_t i = 0; i < 20; ++i) rsdp_sum += rsdp[i];
    if (rsdp_sum) return acpi_failure(ProbeStatus::RsdpInvalid);

    uint64_t root_address = read_u32(rsdp + 16);
    uint32_t entry_size = 4;
    if (rsdp[15] >= 2) {
        const uint32_t rsdp_length = read_u32(rsdp + 20);
        if (rsdp_length < 36 || rsdp_length > 4096)
            return acpi_failure(ProbeStatus::RsdpInvalid);
        rsdp_sum = 0;
        for (uint32_t i = 0; i < rsdp_length; ++i) rsdp_sum += rsdp[i];
        if (rsdp_sum) return acpi_failure(ProbeStatus::RsdpInvalid);
        const uint64_t xsdt = read_u64(rsdp + 24);
        if (xsdt) {
            root_address = xsdt;
            entry_size = 8;
        }
    }
    if (!root_address) return acpi_failure(ProbeStatus::RootTableInvalid);
    const auto *root = reinterpret_cast<const uint8_t *>(root_address);
    const char *root_signature = entry_size == 8 ? "XSDT" : "RSDT";
    if (!acpi_table_valid(root, root_signature))
        return acpi_failure(ProbeStatus::RootTableInvalid);
    const uint32_t root_length = read_u32(root + 4);
    if ((root_length - 36) % entry_size)
        return acpi_failure(ProbeStatus::RootTableInvalid);
    const uint32_t entries = (root_length - 36) / entry_size;
    if (entries > kMaximumAcpiTables)
        return acpi_failure(ProbeStatus::RootTableInvalid);

    const uint8_t *fadt = nullptr;
    for (uint32_t i = 0; i < entries; ++i) {
        const uint8_t *entry = root + 36 + static_cast<uint64_t>(i) * entry_size;
        const uint64_t address = entry_size == 8 ? read_u64(entry) : read_u32(entry);
        if (!address) continue;
        const auto *table = reinterpret_cast<const uint8_t *>(address);
        if (signature_is(table, "FACP") && acpi_table_valid(table, "FACP")) {
            fadt = table;
            continue;
        }
        if (signature_is(table, "SSDT"))
            (void)collect_aml_table(table, "SSDT", out);
    }
    if (!fadt) return acpi_failure(ProbeStatus::FadtMissing);
    const uint32_t fadt_length = read_u32(fadt + 4);
    uint64_t dsdt_address = fadt_length >= 44 ? read_u32(fadt + 40) : 0;
    if (fadt_length >= 148) {
        const uint64_t extended = read_u64(fadt + 140);
        if (extended) dsdt_address = extended;
    }
    if (!dsdt_address) return acpi_failure(ProbeStatus::DsdtInvalid);
    const auto *dsdt = reinterpret_cast<const uint8_t *>(dsdt_address);
    AcpiTables ordered{};
    if (!collect_aml_table(dsdt, "DSDT", &ordered))
        return acpi_failure(ProbeStatus::DsdtInvalid);
    for (uint32_t i = 0; i < out->count; ++i) {
        if (ordered.count == kMaximumAcpiTables) return false;
        ordered.aml[ordered.count] = out->aml[i];
        ordered.lengths[ordered.count] = out->lengths[i];
        ++ordered.count;
    }
    *out = ordered;
    return true;
}

void collect_devices(const AcpiTables &tables) {
    gDeviceCount = 0;
    for (uint32_t table = 0; table < tables.count; ++table) {
        const uint8_t *aml = tables.aml[table];
        const uint32_t length = tables.lengths[table];
        for (uint32_t i = 0; i + 3 < length; ++i) {
            if (aml[i] != 0x5b || aml[i + 1] != 0x82 ||
                gDeviceCount == kMaximumDevices) continue;
            uint32_t package_length = 0, encoded = 0;
            const uint32_t package_start = i + 2;
            if (!parse_pkg_length(aml, length, package_start,
                                  &package_length, &encoded)) continue;
            const uint32_t package_end = package_start + package_length;
            uint32_t name_cursor = package_start + encoded;
            char name[4]{};
            if (package_end > length || name_cursor >= package_end ||
                !parse_namestring(aml, package_end, &name_cursor, name)) continue;
            AcpiDevice &device = gDevices[gDeviceCount++];
            device.begin = aml + i;
            device.end = aml + package_end;
            for (uint32_t j = 0; j < 4; ++j) device.name[j] = name[j];
        }
    }
}

AcpiDevice *innermost_device(const uint8_t *address) {
    AcpiDevice *best = nullptr;
    uintptr_t best_span = UINTPTR_MAX;
    const uintptr_t point = reinterpret_cast<uintptr_t>(address);
    for (uint32_t i = 0; i < gDeviceCount; ++i) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(gDevices[i].begin);
        const uintptr_t end = reinterpret_cast<uintptr_t>(gDevices[i].end);
        if (point < begin || point >= end || end - begin >= best_span) continue;
        best = &gDevices[i];
        best_span = end - begin;
    }
    return best;
}

bool parse_integer(const uint8_t *aml, uint32_t length, uint32_t *cursor,
                   uint64_t *value) {
    if (!aml || !cursor || !value || *cursor >= length) return false;
    const uint8_t opcode = aml[(*cursor)++];
    uint32_t bytes = 0;
    if (opcode == 0x00) { *value = 0; return true; }
    if (opcode == 0x01) { *value = 1; return true; }
    if (opcode == 0xff) { *value = UINT64_MAX; return true; }
    if (opcode == 0x0a) bytes = 1;
    else if (opcode == 0x0b) bytes = 2;
    else if (opcode == 0x0c) bytes = 4;
    else if (opcode == 0x0e) bytes = 8;
    else return false;
    if (bytes > length - *cursor) return false;
    uint64_t result = 0;
    for (uint32_t i = 0; i < bytes; ++i)
        result |= static_cast<uint64_t>(aml[*cursor + i]) << (8 * i);
    *cursor += bytes;
    *value = result;
    return true;
}

bool parse_buffer_object(const uint8_t *aml, uint32_t aml_length,
                         uint32_t offset, const uint8_t **buffer,
                         uint32_t *buffer_length) {
    if (!aml || !buffer || !buffer_length || offset >= aml_length ||
        aml[offset] != 0x11) return false;
    const uint32_t pkg_start = offset + 1;
    uint32_t pkg_length = 0, encoded = 0;
    if (!parse_pkg_length(aml, aml_length, pkg_start, &pkg_length, &encoded))
        return false;
    const uint32_t pkg_end = pkg_start + pkg_length;
    uint32_t data_cursor = pkg_start + encoded;
    uint64_t declared_size = 0;
    if (pkg_end > aml_length ||
        !parse_integer(aml, pkg_end, &data_cursor, &declared_size) ||
        declared_size > pkg_end - data_cursor) return false;
    *buffer = aml + data_cursor;
    *buffer_length = static_cast<uint32_t>(declared_size);
    return true;
}

bool copy_aml_string(const uint8_t *aml, uint32_t length, uint32_t offset,
                     char output[9], uint32_t *next_offset) {
    if (!aml || !output || !next_offset || offset >= length || aml[offset] != 0x0d)
        return false;
    uint32_t cursor = offset + 1;
    uint32_t copied = 0;
    while (cursor < length && aml[cursor]) {
        if (copied < 8) output[copied++] = static_cast<char>(aml[cursor]);
        ++cursor;
    }
    if (cursor >= length) return false;
    output[copied] = 0;
    *next_offset = cursor + 1;
    return true;
}

bool decode_eisa_id(uint64_t value, char output[9]) {
    if (!output || value > UINT32_MAX) return false;
    const uint32_t raw = static_cast<uint32_t>(value);
    const uint32_t id = ((raw & 0x000000ffu) << 24) |
        ((raw & 0x0000ff00u) << 8) |
        ((raw & 0x00ff0000u) >> 8) |
        ((raw & 0xff000000u) >> 24);
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t shift = 26 - i * 5;
        const uint8_t letter = static_cast<uint8_t>((id >> shift) & 0x1fu);
        if (!letter || letter > 26) return false;
        output[i] = static_cast<char>('A' + letter - 1);
    }
    for (uint32_t i = 0; i < 4; ++i) {
        const uint8_t digit = static_cast<uint8_t>((id >> (12 - i * 4)) & 0x0fu);
        output[3 + i] = static_cast<char>(digit < 10 ? '0' + digit : 'A' + digit - 10);
    }
    output[7] = 0;
    output[8] = 0;
    return true;
}

bool copy_aml_id(const uint8_t *aml, uint32_t length, uint32_t offset,
                 char output[9]) {
    if (!aml || !output || offset >= length) return false;
    for (uint32_t i = 0; i < 9; ++i) output[i] = 0;
    uint32_t next = 0;
    if (copy_aml_string(aml, length, offset, output, &next)) return true;
    uint32_t cursor = offset;
    uint64_t value = 0;
    return parse_integer(aml, length, &cursor, &value) &&
           decode_eisa_id(value, output);
}

bool is_hid_over_i2c_id(const char id[9]);

void collect_cid_string(const uint8_t *aml, uint32_t length, uint32_t value_offset,
                       char output[9]) {
    uint32_t next = 0;
    if (copy_aml_string(aml, length, value_offset, output, &next)) return;
    if (value_offset >= length) return;
    if (aml[value_offset] != 0x12) {
        uint32_t cursor = value_offset;
        uint64_t value = 0;
        char candidate[9]{};
        if (parse_integer(aml, length, &cursor, &value) &&
            decode_eisa_id(value, candidate) && is_hid_over_i2c_id(candidate)) {
            for (uint32_t i = 0; i < 9; ++i) output[i] = candidate[i];
        }
        return;
    }

    uint32_t package_length = 0, encoded = 0;
    const uint32_t package_start = value_offset + 1;
    if (!parse_pkg_length(aml, length, package_start, &package_length, &encoded))
        return;
    const uint32_t package_end = package_start + package_length;
    uint32_t cursor = package_start + encoded;
    if (package_end > length || cursor >= package_end) return;
    const uint32_t count = aml[cursor++];
    for (uint32_t item = 0; item < count && cursor < package_end; ++item) {
        if (copy_aml_string(aml, package_end, cursor, output, &next)) {
            if (is_hid_over_i2c_id(output)) return;
            cursor = next;
            continue;
        }
        // EISA IDs such as PNP0C50 are commonly encoded as AML integers.
        // Decode those as well as string IDs in _CID packages.
        uint32_t integer_cursor = cursor;
        uint64_t integer_value = 0;
        if (!parse_integer(aml, package_end, &integer_cursor, &integer_value)) return;
        char candidate[9]{};
        if (decode_eisa_id(integer_value, candidate) &&
            is_hid_over_i2c_id(candidate)) {
            for (uint32_t i = 0; i < 9; ++i) output[i] = candidate[i];
            return;
        }
        cursor = integer_cursor;
    }
}

bool id_equals(const char id[9], const char *expected) {
    if (!id || !expected) return false;
    uint32_t index = 0;
    while (id[index] && expected[index] && id[index] == expected[index]) ++index;
    return !id[index] && !expected[index];
}

bool id_has_prefix(const char id[9], const char *prefix) {
    if (!id || !prefix) return false;
    for (uint32_t index = 0; prefix[index]; ++index)
        if (!id[index] || id[index] != prefix[index]) return false;
    return true;
}

bool is_hid_over_i2c_id(const char id[9]) {
    return id_equals(id, "PNP0C50") || id_equals(id, "ACPI0C50") ||
           id_has_prefix(id, "ELAN") || id_has_prefix(id, "SYNA") ||
           id_has_prefix(id, "GDIX") || id_has_prefix(id, "ALP");
}

void collect_device_properties(const AcpiTables &tables) {
    for (uint32_t table = 0; table < tables.count; ++table) {
        const uint8_t *aml = tables.aml[table];
        const uint32_t length = tables.lengths[table];
        for (uint32_t i = 0; i + 5 < length; ++i) {
            if (aml[i] != 0x08) continue;
            AcpiDevice *device = innermost_device(aml + i);
            if (!device) continue;
            const bool is_hid = aml[i + 1] == '_' && aml[i + 2] == 'H' &&
                                aml[i + 3] == 'I' && aml[i + 4] == 'D';
            const bool is_cid = aml[i + 1] == '_' && aml[i + 2] == 'C' &&
                                aml[i + 3] == 'I' && aml[i + 4] == 'D';
            const bool is_crs = aml[i + 1] == '_' && aml[i + 2] == 'C' &&
                                aml[i + 3] == 'R' && aml[i + 4] == 'S';
            if ((is_hid || is_cid) && i + 5 < length) {
                char *id = is_hid ? device->hid : device->cid;
                if (is_hid) {
                    (void)copy_aml_id(aml, length, i + 5, id);
                } else {
                    collect_cid_string(aml, length, i + 5, id);
                }
            } else if (is_crs && i + 5 < length && aml[i + 5] == 0x11) {
                const uint8_t *buffer = nullptr;
                uint32_t buffer_length = 0;
                if (parse_buffer_object(aml, length, i + 5,
                                         &buffer, &buffer_length)) {
                    device->crs = buffer;
                    device->crs_length = buffer_length;
                    device->has_crs = true;
                }
            }
        }
    }
}

bool same_name(const char a[4], const char b[4]);

AcpiDevice *unique_device_named(const char name[4]) {
    AcpiDevice *match = nullptr;
    for (uint32_t i = 0; i < gDeviceCount; ++i) {
        if (!same_name(gDevices[i].name, name)) continue;
        if (match) return nullptr;
        match = &gDevices[i];
    }
    return match;
}

/* Some firmware extends a device from another AML table with Scope rather
 * than putting every Name object inside the original Device package. Resolve
 * those static properties when the target NameSeg is unambiguous. */
void collect_scoped_device_properties(const AcpiTables &tables) {
    for (uint32_t table = 0; table < tables.count; ++table) {
        const uint8_t *aml = tables.aml[table];
        const uint32_t length = tables.lengths[table];
        for (uint32_t i = 0; i + 2 < length; ++i) {
            if (aml[i] != 0x10) continue; // ScopeOp
            uint32_t package_length = 0, encoded = 0;
            const uint32_t package_start = i + 1;
            if (!parse_pkg_length(aml, length, package_start,
                                  &package_length, &encoded)) continue;
            const uint32_t package_end = package_start + package_length;
            if (package_end > length) continue;
            uint32_t name_cursor = package_start + encoded;
            char scope_name[4]{};
            if (!parse_namestring(aml, package_end, &name_cursor, scope_name))
                continue;
            AcpiDevice *device = unique_device_named(scope_name);
            if (!device) continue;
            const AcpiDevice *enclosing_device = innermost_device(aml + i);

            for (uint32_t property = name_cursor; property + 5 < package_end;
                 ++property) {
                // A Scope can reopen a child below its parent Device. In that
                // case AML's byte range is still inside the parent Device,
                // even though the Name belongs to the reopened child. Ignore
                // only a nested Device introduced inside this Scope.
                if (innermost_device(aml + property) != enclosing_device)
                    continue;
                if (aml[property] == 0x14) {
                    uint32_t method_length = 0, method_length_bytes = 0;
                    const uint32_t method_start = property + 1;
                    if (!parse_pkg_length(aml, package_end, method_start,
                                          &method_length,
                                          &method_length_bytes)) continue;
                    const uint32_t method_end = method_start + method_length;
                    uint32_t method_name_cursor = method_start +
                        method_length_bytes;
                    char method_name[4]{};
                    if (method_end <= package_end &&
                        parse_namestring(aml, method_end, &method_name_cursor,
                                         method_name)) {
                        const bool is_crs_method = method_name[0] == '_' &&
                            method_name[1] == 'C' && method_name[2] == 'R' &&
                            method_name[3] == 'S';
                        if (is_crs_method && !device->has_crs &&
                            method_name_cursor < method_end) {
                            // Some firmware places a literal ResourceTemplate
                            // buffer in a scoped _CRS method. Read that static
                            // template without executing arbitrary AML.
                            for (uint32_t buffer_offset = method_name_cursor + 1;
                                 buffer_offset + 1 < method_end; ++buffer_offset) {
                                if (aml[buffer_offset] != 0x11) continue;
                                const uint8_t *buffer = nullptr;
                                uint32_t buffer_length = 0;
                                if (!parse_buffer_object(aml, method_end,
                                                         buffer_offset, &buffer,
                                                         &buffer_length))
                                    continue;
                                AcpiDevice candidate = *device;
                                candidate.crs = buffer;
                                candidate.crs_length = buffer_length;
                                candidate.has_crs = true;
                                I2cResource resource{};
                                if (parse_i2c_resource(candidate, &resource)) {
                                    device->crs = buffer;
                                    device->crs_length = buffer_length;
                                    device->has_crs = true;
                                    break;
                                }
                            }
                        }
                        property = method_end - 1;
                    }
                    continue;
                }
                if (aml[property] != 0x08) continue;
                const bool is_hid = aml[property + 1] == '_' &&
                    aml[property + 2] == 'H' && aml[property + 3] == 'I' &&
                    aml[property + 4] == 'D';
                const bool is_cid = aml[property + 1] == '_' &&
                    aml[property + 2] == 'C' && aml[property + 3] == 'I' &&
                    aml[property + 4] == 'D';
                const bool is_crs = aml[property + 1] == '_' &&
                    aml[property + 2] == 'C' && aml[property + 3] == 'R' &&
                    aml[property + 4] == 'S';
                if (is_hid && !device->hid[0]) {
                    (void)copy_aml_id(aml, length, property + 5, device->hid);
                } else if (is_cid && !device->cid[0]) {
                    collect_cid_string(aml, length, property + 5, device->cid);
                } else if (is_crs && !device->has_crs &&
                           aml[property + 5] == 0x11) {
                    const uint8_t *buffer = nullptr;
                    uint32_t buffer_length = 0;
                    if (parse_buffer_object(aml, package_end, property + 5,
                                            &buffer, &buffer_length)) {
                        const uintptr_t buffer_address =
                            reinterpret_cast<uintptr_t>(buffer);
                        const uintptr_t scope_end =
                            reinterpret_cast<uintptr_t>(aml + package_end);
                        if (buffer_address <= scope_end &&
                            buffer_length <= scope_end - buffer_address) {
                            device->crs = buffer;
                            device->crs_length = buffer_length;
                            device->has_crs = true;
                        }
                    }
                }
            }
        }
    }
}

void add_dsm_candidate(AcpiDevice *device, uint16_t address);
void add_i2c_address_candidate(AcpiDevice *device, uint16_t address);

void collect_dsm_registers(const AcpiTables &tables, AcpiDevice *device) {
    if (!device) return;
    for (uint32_t table = 0; table < tables.count; ++table) {
        const uint8_t *aml = tables.aml[table];
        const uint32_t length = tables.lengths[table];
        const uintptr_t begin = reinterpret_cast<uintptr_t>(aml);
        const uintptr_t scope_begin = reinterpret_cast<uintptr_t>(device->begin);
        const uintptr_t scope_end = reinterpret_cast<uintptr_t>(device->end);
        if (scope_begin < begin || scope_end > begin + length) continue;
        const uint32_t first = static_cast<uint32_t>(scope_begin - begin);
        const uint32_t last = static_cast<uint32_t>(scope_end - begin);
        for (uint32_t i = first; i + 2 < last; ++i) {
            if (aml[i] != 0x14) continue; // MethodOp
            uint32_t pkg_length = 0, encoded = 0;
            const uint32_t package_start = i + 1;
            if (!parse_pkg_length(aml, last, package_start,
                                  &pkg_length, &encoded)) continue;
            const uint32_t pkg_end = package_start + pkg_length;
            if (pkg_end > last) continue;
            uint32_t name_cursor = package_start + encoded;
            char method_name[4]{};
            if (!parse_namestring(aml, pkg_end, &name_cursor, method_name) ||
                method_name[0] != '_' || method_name[1] != 'D' ||
                method_name[2] != 'S' || method_name[3] != 'M' ||
                name_cursor >= pkg_end) continue;
            // MethodFlags follows the NameString. Only inspect this method's
            // term list, avoiding unrelated Return opcodes in neighboring AML.
            const uint32_t method_body = name_cursor + 1;
            for (uint32_t j = method_body; j + 1 < pkg_end; ++j) {
                if (aml[j] != 0xa4) continue;
                uint32_t cursor = j + 1;
                uint64_t value = 0;
                if (!parse_integer(aml, pkg_end, &cursor, &value) ||
                    value > UINT16_MAX || device->dsm_address_count == kMaximumDsmAddresses)
                    continue;
                device->dsm_addresses[device->dsm_address_count++] =
                    static_cast<uint16_t>(value);
            }
            i = pkg_end ? pkg_end - 1 : i;
        }
    }
}

/* Some firmware returns the HID-over-I2C descriptor register from _DSM via
 * a field selected from a static device table at runtime. Recover the
 * table's I2C address and descriptor-register fields as bounded candidates;
 * the ordinary literal _DSM path above remains preferred. */
void collect_dynamic_dsm_registers(const AcpiTables &tables,
                                   AcpiDevice *device) {
    if (!device) return;
    const uintptr_t device_begin = reinterpret_cast<uintptr_t>(device->begin);
    const uintptr_t device_end = reinterpret_cast<uintptr_t>(device->end);

    for (uint32_t table = 0; table < tables.count; ++table) {
        const uint8_t *aml = tables.aml[table];
        const uint32_t length = tables.lengths[table];
        const uintptr_t table_begin = reinterpret_cast<uintptr_t>(aml);
        if (device_begin < table_begin || device_end > table_begin + length)
            continue;
        const uint32_t first = static_cast<uint32_t>(device_begin - table_begin);
        const uint32_t last = static_cast<uint32_t>(device_end - table_begin);

        for (uint32_t name = first; name + 5 < last; ++name) {
            if (aml[name] != 0x08 || aml[name + 1] != 'I' ||
                aml[name + 2] != 'T' || aml[name + 3] != 'M' ||
                aml[name + 4] != 'L' || innermost_device(aml + name) != device ||
                aml[name + 5] != 0x12) continue;

            uint32_t outer_length = 0, outer_encoded = 0;
            const uint32_t outer_start = name + 6;
            if (!parse_pkg_length(aml, last, outer_start, &outer_length,
                                  &outer_encoded)) continue;
            const uint32_t outer_end = outer_start + outer_length;
            uint32_t entry = outer_start + outer_encoded;
            if (outer_end > last || entry >= outer_end) continue;
            const uint32_t entry_count = aml[entry++];
            if (entry_count > 32) continue;

            for (uint32_t index = 0; index < entry_count && entry < outer_end;
                 ++index) {
                if (aml[entry] != 0x12) break;
                uint32_t item_length = 0, item_encoded = 0;
                const uint32_t item_start = entry + 1;
                if (!parse_pkg_length(aml, outer_end, item_start, &item_length,
                                      &item_encoded)) break;
                const uint32_t item_end = item_start + item_length;
                uint32_t item_cursor = item_start + item_encoded;
                if (item_end > outer_end || item_cursor >= item_end) break;
                const uint32_t field_count = aml[item_cursor++];
                if (field_count < 4) break;

                uint64_t address = 0, ignored = 0, descriptor_register = 0;
                if (!parse_integer(aml, item_end, &item_cursor, &ignored) ||
                    !parse_integer(aml, item_end, &item_cursor, &ignored) ||
                    !parse_integer(aml, item_end, &item_cursor, &address) ||
                    !parse_integer(aml, item_end, &item_cursor,
                                   &descriptor_register)) break;
                if (address <= 0x3ff && descriptor_register <= UINT16_MAX) {
                    add_i2c_address_candidate(device,
                        static_cast<uint16_t>(address));
                    add_dsm_candidate(device,
                        static_cast<uint16_t>(descriptor_register));
                }
                entry = item_end;
            }
            name = outer_end ? outer_end - 1 : name;
        }
    }
}

bool parse_i2c_resource(const AcpiDevice &device, I2cResource *out) {
    if (!device.has_crs || !device.crs || !out) return false;
    const uint8_t *bytes = device.crs;
    uint32_t cursor = 0;
    while (cursor < device.crs_length) {
        const uint8_t tag = bytes[cursor];
        if (tag == 0x79) break;
        if (!(tag & 0x80)) {
            const uint32_t total = 1 + (tag & 7u);
            if (total > device.crs_length - cursor) return false;
            cursor += total;
            continue;
        }
        if (device.crs_length - cursor < 3) return false;
        const uint32_t payload = read_u16(bytes + cursor + 1);
        const uint32_t total = payload + 3;
        if (total > device.crs_length - cursor) return false;
        if ((tag & 0x7fu) == 0x0eu && payload >= 15 &&
            bytes[cursor + 5] == 1) {
            I2cResource candidate{};
            const uint16_t type_data_length = read_u16(bytes + cursor + 10);
            const uint32_t source_offset = 12 + type_data_length;
            if (type_data_length < 6 || source_offset > total) {
                cursor += total;
                continue;
            }
            const uint32_t source_limit = total;
            uint32_t source_end = source_offset;
            while (source_end < source_limit && bytes[cursor + source_end]) ++source_end;
            if (source_end < source_limit && source_end > source_offset) {
                uint32_t segment_start = source_offset;
                for (uint32_t j = source_offset; j < source_end; ++j)
                    if (bytes[cursor + j] == '.' || bytes[cursor + j] == '\\')
                        segment_start = j + 1;
                if (source_end - segment_start >= 4) {
                    for (uint32_t j = 0; j < 4; ++j)
                        candidate.source[j] = static_cast<char>(
                            bytes[cursor + source_end - 4 + j]);
                    const bool null_source = candidate.source[0] == 'N' &&
                        candidate.source[1] == 'U' &&
                        candidate.source[2] == 'L' &&
                        candidate.source[3] == 'L';
                    candidate.source_valid = !null_source;
                }
            } else if (source_offset < source_limit && source_end == source_limit) {
                cursor += total;
                continue;
            }
            candidate.frequency = read_u32(bytes + cursor + 12);
            candidate.address = read_u16(bytes + cursor + 16);
            candidate.address_10_bit = (bytes[cursor + 7] & 1u) != 0;
            const uint16_t max_address = candidate.address_10_bit ? 0x3ff : 0x7f;
            // Some firmware returns an unpatched ResourceTemplate with zero
            // address/speed fields, then fills those fields inside _CRS at
            // runtime. Preserve that template so the caller can probe a
            // standards-compliant HID-over-I2C device instead of discarding it.
            candidate.address_valid = candidate.address != 0 &&
                candidate.address <= max_address;
            candidate.frequency_valid = candidate.frequency != 0;
            candidate.valid = candidate.address <= max_address;
            if (candidate.valid) {
                *out = candidate;
                return true;
            }
        }
        cursor += total;
    }
    return false;
}

bool parse_controller_memory(const AcpiDevice &device, uint64_t *base,
                             uint64_t *length_out) {
    if (!device.has_crs || !device.crs || !base || !length_out) return false;
    const uint8_t *bytes = device.crs;
    uint32_t cursor = 0;
    while (cursor < device.crs_length) {
        const uint8_t tag = bytes[cursor];
        if (tag == 0x79) break;
        if (!(tag & 0x80)) {
            const uint32_t total = 1 + (tag & 7u);
            if (total > device.crs_length - cursor) return false;
            cursor += total;
            continue;
        }
        if (device.crs_length - cursor < 3) return false;
        const uint32_t payload = read_u16(bytes + cursor + 1);
        const uint32_t total = payload + 3;
        if (total > device.crs_length - cursor) return false;
        const uint8_t kind = tag & 0x7fu;
        if (kind == 0x06 && payload >= 9) {
            *base = read_u32(bytes + cursor + 4);
            *length_out = read_u32(bytes + cursor + 8);
            return *base && *length_out >= kMmioBytes;
        }
        if (kind == 0x05 && payload >= 17) {
            *base = read_u32(bytes + cursor + 4);
            *length_out = read_u32(bytes + cursor + 16);
            if (*base && *length_out >= kMmioBytes) return true;
        }
        if (kind == 0x07 && payload >= 23 && bytes[cursor + 3] == 0) {
            *base = read_u32(bytes + cursor + 10);
            *length_out = read_u32(bytes + cursor + 22);
            if (*base && *length_out >= kMmioBytes) return true;
        }
        if (kind == 0x0a && payload >= 43 && bytes[cursor + 3] == 0) {
            *base = read_u64(bytes + cursor + 14);
            *length_out = read_u64(bytes + cursor + 38);
            if (*base && *length_out >= kMmioBytes) return true;
        }
        cursor += total;
    }
    return false;
}

bool find_resource_buffer(const AcpiTables &tables, const AcpiDevice &device,
                          uint32_t *scan_offset,
                          const uint8_t **buffer_out,
                          uint32_t *length_out) {
    if (!scan_offset || !buffer_out || !length_out) return false;
    const uintptr_t device_begin = reinterpret_cast<uintptr_t>(device.begin);
    const uintptr_t device_end = reinterpret_cast<uintptr_t>(device.end);
    for (uint32_t table = 0; table < tables.count; ++table) {
        const uint8_t *aml = tables.aml[table];
        const uintptr_t table_begin = reinterpret_cast<uintptr_t>(aml);
        const uintptr_t table_end = table_begin + tables.lengths[table];
        if (device_begin < table_begin || device_end > table_end) continue;
        const uint32_t first = static_cast<uint32_t>(device_begin - table_begin);
        const uint32_t last = static_cast<uint32_t>(device_end - table_begin);
        uint32_t offset = first + *scan_offset;
        for (; offset + 1 < last; ++offset) {
            if (aml[offset] != 0x11) continue;
            *scan_offset = offset - first + 1;
            const uint8_t *candidate = nullptr;
            uint32_t candidate_length = 0;
            if (!parse_buffer_object(aml, tables.lengths[table], offset,
                                     &candidate, &candidate_length)) continue;
            const uintptr_t candidate_begin = reinterpret_cast<uintptr_t>(candidate);
            if (candidate_begin < device_begin || candidate_begin >= device_end ||
                candidate_length > device_end - candidate_begin)
                continue;
            *buffer_out = candidate;
            *length_out = candidate_length;
            return true;
        }
    }
    return false;
}

bool parse_i2c_resource_in_scope(const AcpiTables &tables,
                                 const AcpiDevice &device,
                                 I2cResource *out) {
    if (parse_i2c_resource(device, out)) return true;
    uint32_t scan_offset = 0;
    const uint8_t *buffer = nullptr;
    uint32_t length = 0;
    while (find_resource_buffer(tables, device, &scan_offset, &buffer, &length)) {
        AcpiDevice candidate = device;
        candidate.crs = buffer;
        candidate.crs_length = length;
        candidate.has_crs = true;
        if (parse_i2c_resource(candidate, out)) return true;
    }

    // Firmware may declare a named resource buffer in a Scope that reopens
    // the touchpad, then have _CRS return that name from a method. Resolve
    // such static buffers in that device's Scope without running AML code.
    for (uint32_t table = 0; table < tables.count; ++table) {
        const uint8_t *aml = tables.aml[table];
        const uint32_t aml_length = tables.lengths[table];
        for (uint32_t offset = 0; offset + 2 < aml_length; ++offset) {
            if (aml[offset] != 0x10) continue;
            uint32_t package_length = 0, encoded = 0;
            const uint32_t package_start = offset + 1;
            if (!parse_pkg_length(aml, aml_length, package_start,
                                  &package_length, &encoded)) continue;
            const uint32_t package_end = package_start + package_length;
            if (package_end > aml_length) continue;
            uint32_t name_cursor = package_start + encoded;
            char scope_name[4]{};
            if (!parse_namestring(aml, package_end, &name_cursor, scope_name) ||
                unique_device_named(scope_name) != &device) continue;
            const AcpiDevice *enclosing_device = innermost_device(aml + offset);
            for (uint32_t buffer_offset = name_cursor;
                 buffer_offset + 1 < package_end; ++buffer_offset) {
                if (innermost_device(aml + buffer_offset) != enclosing_device ||
                    aml[buffer_offset] != 0x11) continue;
                const uint8_t *buffer = nullptr;
                uint32_t buffer_length = 0;
                if (!parse_buffer_object(aml, package_end, buffer_offset,
                                         &buffer, &buffer_length)) continue;
                AcpiDevice candidate = device;
                candidate.crs = buffer;
                candidate.crs_length = buffer_length;
                candidate.has_crs = true;
                if (parse_i2c_resource(candidate, out)) return true;
            }
        }
    }
    return false;
}

bool parse_controller_memory_in_scope(const AcpiTables &tables,
                                      const AcpiDevice &device,
                                      uint64_t *base,
                                      uint64_t *length_out) {
    if (parse_controller_memory(device, base, length_out)) return true;
    uint32_t scan_offset = 0;
    const uint8_t *buffer = nullptr;
    uint32_t length = 0;
    while (find_resource_buffer(tables, device, &scan_offset, &buffer, &length)) {
        AcpiDevice candidate = device;
        candidate.crs = buffer;
        candidate.crs_length = length;
        candidate.has_crs = true;
        if (parse_controller_memory(candidate, base, length_out)) return true;
    }
    return false;
}

bool same_name(const char a[4], const char b[4]) {
    for (uint32_t i = 0; i < 4; ++i) if (a[i] != b[i]) return false;
    return true;
}

bool is_supported_designware_controller(const AcpiDevice &device,
                                        bool *amd_clocked) {
    const bool amd = id_has_prefix(device.hid, "AMDI0") ||
                     id_has_prefix(device.hid, "AMD0");
    const bool intel = id_equals(device.hid, "INT33C2") ||
                       id_equals(device.hid, "INT33C3") ||
                       id_equals(device.hid, "INT3432") ||
                       id_equals(device.hid, "INT3433") ||
                       id_equals(device.hid, "INTC10EF") ||
                       id_has_prefix(device.hid, "INTC");
    if (amd_clocked) *amd_clocked = amd;
    return amd || intel;
}

uint32_t mmio_read(volatile uint8_t *mmio, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t *>(mmio + offset);
}

void mmio_write(volatile uint8_t *mmio, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(mmio + offset) = value;
    __asm__ volatile("mfence" ::: "memory");
}

bool wait_register(volatile uint8_t *mmio, uint32_t offset,
                   uint32_t mask, uint32_t expected) {
    for (uint32_t i = 0; i < kDwWaitIterations; ++i) {
        if ((mmio_read(mmio, offset) & mask) == expected) return true;
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

void record_touchpad_failure(TouchpadFailureStage stage, uint16_t address,
                             uint16_t descriptor_register) {
    if (stage < gTouchpadFailureStage) return;
    gTouchpadFailureStage = stage;
    gTouchpadFailureAddress = address;
    gTouchpadFailureRegister = descriptor_register;
}

bool check_i2c_abort(volatile uint8_t *mmio) {
    if (!(mmio_read(mmio, 0x34) & (1u << 6))) return false;
    (void)mmio_read(mmio, 0x54);
    return true;
}

bool setup_designware(volatile uint8_t *mmio, uint32_t frequency,
                      uint16_t address, bool address_10_bit,
                      bool preserve_firmware_timing) {
    if (!frequency || mmio_read(mmio, 0xfc) != 0x44570140u) return false;
    // AMDI0010 exposes the DesignWare IP with the 150 MHz functional clock.
    // Program Fast-mode high/low counts from its ACPI bus frequency.
    if (frequency > 400000) frequency = 400000;
    const bool standard_mode = frequency <= 100000;
    const uint32_t speed_mode = standard_mode ? 1 : 2;
    const uint64_t period_ns = 1000000000ull / frequency;
    const uint32_t high_register = standard_mode ? 0x14 : 0x1c;
    const uint32_t low_register = standard_mode ? 0x18 : 0x20;
    // Reuse timing values left by firmware on every controller when present.
    // AMD controller clock rates vary by generation, so derive timing only
    // when the AMD registers are uninitialized and the 150 MHz fallback is
    // needed. Intel Serial IO requires firmware-programmed timing.
    uint32_t high_timing = mmio_read(mmio, high_register);
    uint32_t low_timing = mmio_read(mmio, low_register);
    const bool use_firmware_timing = high_timing && low_timing;
    if (!use_firmware_timing && preserve_firmware_timing) return false;
    if (!use_firmware_timing) {
        uint64_t high_ns = period_ns * 45 / 100;
        uint64_t low_ns = period_ns - high_ns;
        if (standard_mode) {
            if (high_ns < 4000) high_ns = 4000;
            if (low_ns < 4700) low_ns = 4700;
        } else {
            if (high_ns < 600) high_ns = 600;
            if (low_ns < 1300) low_ns = 1300;
        }
        const uint64_t high_count = high_ns * 150ull / 1000ull;
        const uint64_t low_count = low_ns * 150ull / 1000ull;
        if (high_count <= 8 || low_count <= 1 ||
            high_count > UINT16_MAX || low_count > UINT16_MAX) return false;
        high_timing = static_cast<uint32_t>(high_count - 8);
        low_timing = static_cast<uint32_t>(low_count - 1);
    }
    const uint32_t old_enable = mmio_read(mmio, 0x6c);
    mmio_write(mmio, 0x6c, 0);
    if (!wait_register(mmio, 0x9c, 1, 0)) {
        if (old_enable & 1u) mmio_write(mmio, 0x6c, old_enable);
        return false;
    }
    (void)mmio_read(mmio, 0x40);
    (void)mmio_read(mmio, 0x54);
    mmio_write(mmio, 0x30, 0);
    if (!use_firmware_timing) {
        mmio_write(mmio, high_register, high_timing);
        mmio_write(mmio, low_register, low_timing);
    }
    // IC_CON requires MASTER_MODE as well as the speed, repeated-start, and
    // slave-disable bits before this controller can issue I2C transactions.
    mmio_write(mmio, 0x00, 1u | (speed_mode << 1) |
                          (address_10_bit ? (1u << 4) : 0) |
                          (1u << 5) | (1u << 6));
    mmio_write(mmio, 0x04, (address & 0x3ffu) |
                          (address_10_bit ? (1u << 12) : 0));
    mmio_write(mmio, 0x6c, 1);
    if (wait_register(mmio, 0x9c, 1, 1)) return true;
    if (old_enable & 1u) mmio_write(mmio, 0x6c, old_enable);
    return false;
}

bool wait_tx_space(volatile uint8_t *mmio) {
    for (uint32_t i = 0; i < kDwWaitIterations; ++i) {
        if (check_i2c_abort(mmio)) return false;
        if (mmio_read(mmio, 0x74) == 0) return true;
        __asm__ volatile("pause");
    }
    return false;
}

bool write_command_byte(volatile uint8_t *mmio, uint32_t command) {
    if (!wait_tx_space(mmio)) return false;
    mmio_write(mmio, 0x10, command);
    return true;
}

bool i2c_read_register(volatile uint8_t *mmio, uint16_t reg,
                       uint8_t *output, uint32_t length) {
    if (!mmio || !output || !length || length > kMaximumI2cTransfer) return false;
    if (!write_command_byte(mmio, reg & 0xffu) ||
        !write_command_byte(mmio, (reg >> 8) & 0xffu)) return false;
    for (uint32_t i = 0; i < length; ++i) {
        uint32_t command = 1u << 8;
        if (i == 0) command |= 1u << 10;
        if (i + 1 == length) command |= 1u << 9;
        if (!write_command_byte(mmio, command)) return false;
        bool received = false;
        for (uint32_t spin = 0; spin < kDwWaitIterations; ++spin) {
            if (check_i2c_abort(mmio)) return false;
            if (mmio_read(mmio, 0x78)) {
                output[i] = static_cast<uint8_t>(mmio_read(mmio, 0x10));
                received = true;
                break;
            }
            __asm__ volatile("pause");
        }
        if (!received) return false;
    }
    return true;
}

bool i2c_write_register(volatile uint8_t *mmio, uint16_t reg,
                        const uint8_t *payload, uint32_t length) {
    if (!mmio || length > kMaximumI2cTransfer - 2 || (length && !payload))
        return false;
    if (!write_command_byte(mmio, reg & 0xffu) ||
        !write_command_byte(mmio, (reg >> 8) & 0xffu)) return false;
    for (uint32_t i = 0; i < length; ++i) {
        uint32_t command = payload[i];
        if (i + 1 == length) command |= 1u << 9;
        if (!write_command_byte(mmio, command)) return false;
    }
    if (!length) {
        if (!write_command_byte(mmio, 1u << 9)) return false;
    }
    for (uint32_t i = 0; i < kDwWaitIterations; ++i) {
        if (check_i2c_abort(mmio)) return false;
        if ((mmio_read(mmio, 0x70) & (1u << 2)) &&
            !(mmio_read(mmio, 0x70) & 1u)) return true;
        __asm__ volatile("pause");
    }
    return false;
}

void add_dsm_candidate(AcpiDevice *device, uint16_t address) {
    if (!device || address == UINT16_MAX) return;
    for (uint32_t i = 0; i < device->dsm_address_count; ++i)
        if (device->dsm_addresses[i] == address) return;
    if (device->dsm_address_count < kMaximumDsmAddresses)
        device->dsm_addresses[device->dsm_address_count++] = address;
}

void add_i2c_address_candidate(AcpiDevice *device, uint16_t address) {
    if (!device || address < 0x08 || address > 0x77) return;
    for (uint32_t i = 0; i < device->i2c_address_count; ++i)
        if (device->i2c_addresses[i] == address) return;
    if (device->i2c_address_count < kMaximumDsmAddresses)
        device->i2c_addresses[device->i2c_address_count++] = address;
}

enum class DescriptorReadResult : uint8_t { Success, BusError, Invalid };

DescriptorReadResult read_hid_descriptor(volatile uint8_t *mmio,
                                         uint16_t descriptor_register,
                                         HidI2cDescriptor *descriptor) {
    if (!descriptor) return DescriptorReadResult::Invalid;
    uint8_t raw[30]{};
    if (!i2c_read_register(mmio, descriptor_register, raw, sizeof(raw)))
        return DescriptorReadResult::BusError;
    descriptor->length = read_u16(raw + 0);
    descriptor->version = read_u16(raw + 2);
    descriptor->report_length = read_u16(raw + 4);
    descriptor->report_register = read_u16(raw + 6);
    descriptor->input_register = read_u16(raw + 8);
    descriptor->maximum_input_length = read_u16(raw + 10);
    descriptor->output_register = read_u16(raw + 12);
    descriptor->maximum_output_length = read_u16(raw + 14);
    descriptor->command_register = read_u16(raw + 16);
    descriptor->data_register = read_u16(raw + 18);
    descriptor->vendor_id = read_u16(raw + 20);
    descriptor->product_id = read_u16(raw + 22);
    descriptor->device_version = read_u16(raw + 24);
    const bool valid = descriptor->length == 30 && descriptor->version == 0x0100 &&
        descriptor->report_length &&
        descriptor->report_length <= kMaximumHidReportBytes &&
        descriptor->report_register && descriptor->input_register &&
        descriptor->maximum_input_length >= 3 &&
        descriptor->maximum_input_length <= kMaximumI2cTransfer &&
        descriptor->command_register;
    return valid ? DescriptorReadResult::Success : DescriptorReadResult::Invalid;
}

bool start_touchpad(volatile uint8_t *mmio, uint16_t address,
                    AcpiDevice *acpi_device, DeviceState *state) {
    if (!mmio || !acpi_device || !state) return false;
    add_dsm_candidate(acpi_device, 1);
    add_dsm_candidate(acpi_device, 0);
    for (uint32_t i = 0; i < acpi_device->dsm_address_count; ++i) {
        HidI2cDescriptor descriptor{};
        const uint16_t descriptor_register = acpi_device->dsm_addresses[i];
        const DescriptorReadResult descriptor_result =
            read_hid_descriptor(mmio, descriptor_register, &descriptor);
        if (descriptor_result != DescriptorReadResult::Success) {
            record_touchpad_failure(
                descriptor_result == DescriptorReadResult::BusError
                    ? TouchpadFailureStage::DescriptorRead
                    : TouchpadFailureStage::DescriptorInvalid,
                address, descriptor_register);
            continue;
        }
        state->address = address;
        state->input_register = descriptor.input_register;
        state->max_input = descriptor.maximum_input_length;
        // SetPower(On), encoded as report type/id followed by SET_POWER.
        const uint8_t power_on[] = {0x00, 0x08};
        if (!i2c_write_register(mmio, descriptor.command_register,
                                power_on, sizeof(power_on))) {
            if (!wait_scheduler_ticks(1)) {
                record_touchpad_failure(TouchpadFailureStage::PowerOn,
                                        address, descriptor_register);
                return false;
            }
            if (!i2c_write_register(mmio, descriptor.command_register,
                                    power_on, sizeof(power_on))) {
                record_touchpad_failure(TouchpadFailureStage::PowerOn,
                                        address, descriptor_register);
                return false;
            }
        }
        if (!wait_scheduler_ticks(6)) {
            record_touchpad_failure(TouchpadFailureStage::PowerOn,
                                    address, descriptor_register);
            return false;
        }
        const uint8_t reset[] = {0x00, 0x01};
        if (!i2c_write_register(mmio, descriptor.command_register,
                                reset, sizeof(reset))) {
            record_touchpad_failure(TouchpadFailureStage::ResetCommand,
                                    address, descriptor_register);
            return false;
        }
        // Reset completion is reported as a zero-length input packet. Since
        // GPIO interrupt routing is not available yet, poll the HID input
        // register for that acknowledgement, with a bounded fallback window.
        bool reset_acknowledged = false;
        for (uint32_t waited = 0; waited < kHidResetTimeoutTicks;
             waited += kPollIntervalTicks) {
            // A reset-complete indication is exactly the two-byte zero
            // length field at the input register. Read just that field so
            // devices that expose no report payload during reset can ACK.
            uint8_t reset_packet[2]{};
            if (i2c_read_register(mmio, state->input_register, reset_packet,
                                  sizeof(reset_packet)) &&
                read_u16(reset_packet) == 0) {
                reset_acknowledged = true;
                break;
            }
            if (!wait_scheduler_ticks(kPollIntervalTicks)) break;
        }
        if (!reset_acknowledged) {
            record_touchpad_failure(TouchpadFailureStage::ResetTimeout,
                                    address, descriptor_register);
            continue;
        }
        if (!i2c_write_register(mmio, descriptor.command_register,
                                power_on, sizeof(power_on))) {
            if (!wait_scheduler_ticks(1)) {
                record_touchpad_failure(TouchpadFailureStage::PowerOn,
                                        address, descriptor_register);
                return false;
            }
            if (!i2c_write_register(mmio, descriptor.command_register,
                                    power_on, sizeof(power_on))) {
                record_touchpad_failure(TouchpadFailureStage::PowerOn,
                                        address, descriptor_register);
                return false;
            }
        }
        if (!wait_scheduler_ticks(6)) {
            record_touchpad_failure(TouchpadFailureStage::PowerOn,
                                    address, descriptor_register);
            return false;
        }
        if (!i2c_read_register(mmio, descriptor.report_register,
                               gReportDescriptor, descriptor.report_length)) {
            record_touchpad_failure(TouchpadFailureStage::ReportRead,
                                    address, descriptor_register);
            continue;
        }
        if (!welovec::x86_64::input::hid_touchpad::parse_report_descriptor(
                &state->hid, gReportDescriptor, descriptor.report_length)) {
            record_touchpad_failure(TouchpadFailureStage::ReportInvalid,
                                    address, descriptor_register);
            continue;
        }
        state->mmio = mmio;
        state->initialized = true;
        return true;
    }
    return false;
}

void touchpad_worker(void *) {
    for (;;) {
        uint32_t poll_interval = kPollIntervalTicks;
        if (gTouchpad.initialized) {
            uint8_t packet[kMaximumI2cTransfer]{};
            if (i2c_read_register(gTouchpad.mmio, gTouchpad.input_register,
                                  packet, gTouchpad.max_input)) {
                gTouchpad.consecutive_errors = 0;
                const uint16_t size = read_u16(packet);
                if (size && size <= gTouchpad.max_input - 2)
                    (void)welovec::x86_64::input::hid_touchpad::process_report(
                        &gTouchpad.hid, packet + 2, size,
                        welovec::x86_64::input::pointer::Source::I2cHid);
            } else if (gTouchpad.consecutive_errors < 255) {
                ++gTouchpad.consecutive_errors;
            }
            if (gTouchpad.consecutive_errors >= 8)
                poll_interval = kPollErrorBackoffTicks;
        }
        (void)welovec::x86_64::scheduler::sleep(poll_interval);
    }
}

bool initialize_hardware(const AcpiTables &tables) {
    for (uint32_t i = 0; i < sizeof(gNoResourceHid); ++i)
        gNoResourceHid[i] = 0;
    for (uint32_t i = 0; i < sizeof(gNoResourceName); ++i)
        gNoResourceName[i] = 0;
    gNoResourceHasCrs = false;
    bool found_hid_device = false;
    bool found_controller = false;
    bool found_controller_ready = false;
    for (uint32_t i = 0; i < gDeviceCount; ++i) {
        AcpiDevice &touchpad = gDevices[i];
        if (id_equals(touchpad.hid, "CHPN0001") ||
            id_equals(touchpad.hid, "IDEA5002")) continue;
        if (!is_hid_over_i2c_id(touchpad.hid) &&
            !is_hid_over_i2c_id(touchpad.cid)) continue;
        found_hid_device = true;
        I2cResource target{};
        const bool has_resource =
            parse_i2c_resource_in_scope(tables, touchpad, &target);
        if (!has_resource) {
            if (!gNoResourceHid[0]) {
                const char *id = touchpad.hid[0] ? touchpad.hid : touchpad.cid;
                for (uint32_t j = 0; j < sizeof(gNoResourceHid) - 1 && id[j]; ++j)
                    gNoResourceHid[j] = id[j];
                for (uint32_t j = 0; j < sizeof(gNoResourceName) - 1; ++j)
                    gNoResourceName[j] = touchpad.name[j];
                gNoResourceHasCrs = touchpad.has_crs;
            }
            // A firmware _CRS method can build its I2cSerialBusV2 resource
            // from runtime objects that this small AML reader cannot safely
            // evaluate. The HID compatibility ID still identifies the
            // protocol, so probe supported ACPI I2C controllers using the
            // standard HID-over-I2C descriptor registers and validate the
            // descriptor/report before binding to any address.
            target = {};
            target.frequency = 400000;
            target.frequency_valid = false;
            target.address_valid = false;
            target.source_valid = false;
            target.address_10_bit = false;
        }
        if (!target.address_valid) gAddressScanUsed = true;
        add_dsm_candidate(&touchpad, 1);
        add_dsm_candidate(&touchpad, 0);
        collect_dsm_registers(tables, &touchpad);
        collect_dynamic_dsm_registers(tables, &touchpad);
        for (uint32_t j = 0; j < gDeviceCount; ++j) {
            AcpiDevice &controller = gDevices[j];
            bool amd_clocked = false;
            if (!is_supported_designware_controller(controller, &amd_clocked) ||
                (target.source_valid &&
                 !same_name(controller.name, target.source))) continue;
            found_controller = true;
            uint64_t base = 0, span = 0;
            if (!parse_controller_memory_in_scope(tables, controller, &base, &span) ||
                base > UINT64_MAX - kMmioBytes) continue;
            (void)span;
            void *mapping = welovec::memory::vmm::map_physical(
                welovec::x86_64::mm::PhysicalAddress(base), kMmioBytes,
                welovec::memory::vmm::CachePolicy::Uncacheable);
            if (!mapping) continue;
            auto *mmio = static_cast<volatile uint8_t *>(mapping);
            const uint32_t frequencies[] = {
                target.frequency_valid ? target.frequency : 100000,
                400000
            };
            const uint32_t frequency_count = target.frequency_valid ? 1 : 2;
            const uint16_t first_address = target.address_valid
                ? target.address : target.address_10_bit ? 1 : 0x08;
            const uint16_t last_address = target.address_valid
                ? target.address : target.address_10_bit ? 0x3ff : 0x77;
            const uint32_t address_count = target.address_valid ? 1 :
                touchpad.i2c_address_count ? touchpad.i2c_address_count :
                static_cast<uint32_t>(last_address - first_address + 1);
            for (uint32_t frequency_index = 0;
                 frequency_index < frequency_count; ++frequency_index) {
                const uint32_t frequency = frequencies[frequency_index];
                for (uint32_t address_index = 0;
                     address_index < address_count; ++address_index) {
                    const uint16_t address = target.address_valid
                        ? target.address
                        : touchpad.i2c_address_count
                            ? touchpad.i2c_addresses[address_index]
                            : static_cast<uint16_t>(first_address + address_index);
                    if (!setup_designware(mmio, frequency, address,
                                          target.address_10_bit, !amd_clocked))
                        continue;
                    found_controller_ready = true;
                    DeviceState candidate_state{};
                    if (start_touchpad(mmio, address, &touchpad,
                                       &candidate_state)) {
                        candidate_state.mapping = mapping;
                        gTouchpad = candidate_state;
                        return true;
                    }
                    mmio_write(mmio, 0x6c, 0);
                }
            }
            (void)welovec::memory::vmm::unmap_physical(mapping, kMmioBytes);
        }
    }
    if (!found_hid_device) gProbeStatus = ProbeStatus::NoHidDevice;
    else if (!found_controller)
        gProbeStatus = ProbeStatus::NoSupportedI2cController;
    else if (!found_controller_ready) gProbeStatus = ProbeStatus::ControllerUnavailable;
    else gProbeStatus = ProbeStatus::TouchpadUnavailable;
    return false;
}

const char *no_resource_status() {
    uint32_t cursor = 0;
    const char *prefix = "I2C resource missing: ";
    while (prefix[cursor] && cursor + 1 < sizeof(gNoResourceStatus)) {
        gNoResourceStatus[cursor] = prefix[cursor];
        ++cursor;
    }
    const char *id = gNoResourceHid[0] ? gNoResourceHid : "unknown";
    for (uint32_t i = 0; id[i] && cursor + 1 < sizeof(gNoResourceStatus); ++i)
        gNoResourceStatus[cursor++] = id[i];
    if (cursor + 1 < sizeof(gNoResourceStatus))
        gNoResourceStatus[cursor++] = '/';
    for (uint32_t i = 0; i < sizeof(gNoResourceName) - 1 &&
         gNoResourceName[i] && cursor + 1 < sizeof(gNoResourceStatus); ++i)
        gNoResourceStatus[cursor++] = gNoResourceName[i];
    const char *reason = gNoResourceHasCrs
        ? " (_CRS buffer has no recognized I2C resource)"
        : " (_CRS could not be resolved statically)";
    for (uint32_t i = 0; reason[i] && cursor + 1 < sizeof(gNoResourceStatus); ++i)
        gNoResourceStatus[cursor++] = reason[i];
    gNoResourceStatus[cursor] = '\0';
    return gNoResourceStatus;
}

const char *touchpad_failure_status() {
    const char *message = nullptr;
    switch (gTouchpadFailureStage) {
        case TouchpadFailureStage::DescriptorRead:
            message = gAddressScanUsed
                ? "I2C address scan found no HID descriptor response"
                : "I2C HID descriptor register did not respond";
            break;
        case TouchpadFailureStage::DescriptorInvalid:
            message = "I2C device replied but HID descriptor is invalid";
            break;
        case TouchpadFailureStage::PowerOn:
            message = "I2C HID SET_POWER command failed";
            break;
        case TouchpadFailureStage::ResetCommand:
            message = "I2C HID RESET command failed";
            break;
        case TouchpadFailureStage::ResetTimeout:
            message = "I2C HID RESET was not acknowledged within 5 seconds";
            break;
        case TouchpadFailureStage::ReportRead:
            message = "I2C HID report descriptor read failed";
            break;
        case TouchpadFailureStage::ReportInvalid:
            message = "I2C HID report descriptor was rejected";
            break;
        case TouchpadFailureStage::None:
            return "I2C bus initialized; no usable HID touchpad found";
    }

    uint32_t cursor = 0;
    while (message[cursor] && cursor + 1 < sizeof(gTouchpadFailureStatus)) {
        gTouchpadFailureStatus[cursor] = message[cursor];
        ++cursor;
    }
    {
        const char *address_label = " (I2C address 0x";
        for (uint32_t i = 0; address_label[i] &&
             cursor + 1 < sizeof(gTouchpadFailureStatus); ++i)
            gTouchpadFailureStatus[cursor++] = address_label[i];
        static constexpr char digits[] = "0123456789abcdef";
        for (int32_t shift = 12; shift >= 0 &&
             cursor + 1 < sizeof(gTouchpadFailureStatus); shift -= 4)
            gTouchpadFailureStatus[cursor++] =
                digits[(gTouchpadFailureAddress >> shift) & 0xf];
        const char *register_label = ", HID register 0x";
        for (uint32_t i = 0; register_label[i] &&
             cursor + 1 < sizeof(gTouchpadFailureStatus); ++i)
            gTouchpadFailureStatus[cursor++] = register_label[i];
        for (int32_t shift = 12; shift >= 0 &&
             cursor + 1 < sizeof(gTouchpadFailureStatus); shift -= 4)
            gTouchpadFailureStatus[cursor++] =
                digits[(gTouchpadFailureRegister >> shift) & 0xf];
        if (cursor + 1 < sizeof(gTouchpadFailureStatus))
            gTouchpadFailureStatus[cursor++] = ')';
    }
    gTouchpadFailureStatus[cursor] = '\0';
    return gTouchpadFailureStatus;
}

}

namespace welovec::x86_64::input::i2c_touchpad {

bool initialize(const WelovecBootInfo *boot_info) {
    if (gInitialized) return gWorkerStarted;
    gInitialized = true;
    AcpiTables tables{};
    gProbeStatus = ProbeStatus::BootInfoInvalid;
    if (!find_aml_tables(boot_info, &tables)) return false;
    gProbeStatus = ProbeStatus::NoHidDevice;
    collect_devices(tables);
    collect_device_properties(tables);
    collect_scoped_device_properties(tables);
    if (!initialize_hardware(tables)) return false;
    gWorkerStarted = welovec::x86_64::scheduler::create_kernel_thread(
        "i2c-hid-touchpad", touchpad_worker, nullptr,
        welovec::x86_64::scheduler::Priority::Background) != 0;
    if (!gWorkerStarted) {
        if (gTouchpad.mmio) mmio_write(gTouchpad.mmio, 0x6c, 0);
        if (gTouchpad.mapping)
            (void)welovec::memory::vmm::unmap_physical(gTouchpad.mapping, kMmioBytes);
        gTouchpad = {};
        gProbeStatus = ProbeStatus::WorkerUnavailable;
        return false;
    }
    welovec::x86_64::input::pointer::register_source(
        welovec::x86_64::input::pointer::Source::I2cHid);
    gProbeStatus = ProbeStatus::Ready;
    return true;
}

}

extern "C" const char *welovec_i2c_touchpad_status(void) {
    switch (gProbeStatus) {
        case ProbeStatus::NotStarted: return "not started";
        case ProbeStatus::BootInfoInvalid: return "invalid boot information";
        case ProbeStatus::AcpiPointerMissing: return "firmware did not provide an ACPI pointer";
        case ProbeStatus::RsdpInvalid: return "ACPI RSDP signature or checksum invalid";
        case ProbeStatus::RootTableInvalid: return "ACPI RSDT/XSDT invalid";
        case ProbeStatus::FadtMissing: return "ACPI FADT not found";
        case ProbeStatus::DsdtInvalid: return "ACPI DSDT invalid or unavailable";
        case ProbeStatus::NoHidDevice: return "no supported I2C-HID ACPI device";
        case ProbeStatus::NoI2cResource: return no_resource_status();
        case ProbeStatus::NoSupportedI2cController:
            return "supported I2C controller not matched";
        case ProbeStatus::ControllerUnavailable:
            return "matched I2C controller did not start";
        case ProbeStatus::TouchpadUnavailable:
            return touchpad_failure_status();
        case ProbeStatus::WorkerUnavailable: return "input polling worker did not start";
        case ProbeStatus::Ready: return "ready";
    }
    return "unknown";
}
