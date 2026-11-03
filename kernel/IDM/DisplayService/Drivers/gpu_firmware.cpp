// Bounded, hash-pinned firmware loading for GPU drivers.

#include "gpu_firmware.hpp"

#include "filesystem/vfs.h"
#include "security/password_hash.hpp"

namespace welovec::display::gpu {

namespace {
constexpr uint32_t kMaximumFirmwarePathLength = 256;
constexpr uint32_t kAmdCommonHeaderSize = 32;
constexpr uint32_t kAmdPspDescriptorSize = 16;
constexpr uint32_t kAmdPspMaximumDescriptors = 64;
constexpr uint32_t kAmdPspV1HeaderSize = 44;
constexpr uint32_t kAmdTaV1DescriptorSize = 12;
constexpr uint32_t kAmdTaV1DescriptorCount = 5;
constexpr uint32_t kAmdTaV1HeaderSize =
    kAmdCommonHeaderSize + kAmdTaV1DescriptorSize *
        kAmdTaV1DescriptorCount;

uint16_t read_le16(const uint8_t *bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t read_le32(const uint8_t *bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

bool read_common_header(const FirmwareBlob &blob, AmdFirmwareInfo *out_info) {
    if (!blob.data || blob.size < kAmdCommonHeaderSize || !out_info)
        return false;

    AmdFirmwareInfo info{};
    info.image_size = read_le32(blob.data);
    info.header_size = read_le32(blob.data + 4);
    info.header_version_major = read_le16(blob.data + 8);
    info.header_version_minor = read_le16(blob.data + 10);
    info.ip_version_major = read_le16(blob.data + 12);
    info.ip_version_minor = read_le16(blob.data + 14);
    info.ucode_version = read_le32(blob.data + 16);
    info.ucode_size = read_le32(blob.data + 20);
    info.ucode_offset = read_le32(blob.data + 24);
    info.ucode_crc32 = read_le32(blob.data + 28);

    if (info.image_size != blob.size ||
        info.header_size < kAmdCommonHeaderSize ||
        info.header_size > blob.size ||
        info.header_version_major == 0 || info.header_version_major > 2)
        return false;
    *out_info = info;
    return true;
}

bool valid_ucode_range(const AmdFirmwareInfo &info, uint32_t image_size) {
    if (info.ucode_size) {
        return info.ucode_offset >= info.header_size &&
               info.ucode_offset <= image_size &&
               info.ucode_size <= image_size - info.ucode_offset;
    }
    return info.ucode_offset <= image_size;
}

bool valid_payload_range(const AmdFirmwareInfo &info, uint32_t offset,
                         uint32_t size) {
    return size && info.ucode_size && offset <= info.ucode_size &&
           size <= info.ucode_size - offset;
}

bool valid_firmware_path(const char *path) {
    if (!path || path[0] != '/') return false;
    uint32_t length = 0;
    uint32_t component_start = 1;
    for (; length < kMaximumFirmwarePathLength && path[length]; ++length) {
        const char value = path[length];
        const bool allowed =
            (value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') ||
            value == '/' || value == '_' || value == '-' || value == '.';
        if (!allowed) return false;
        if (value == '/') {
            const uint32_t component_size = length - component_start;
            if ((component_size == 1 && path[component_start] == '.') ||
                (component_size == 2 && path[component_start] == '.' &&
                 path[component_start + 1] == '.')) return false;
            component_start = length + 1;
        }
    }
    if (length == 0 || length == kMaximumFirmwarePathLength ||
        component_start == length) return false;
    const uint32_t component_size = length - component_start;
    return !(component_size == 1 && path[component_start] == '.') &&
           !(component_size == 2 && path[component_start] == '.' &&
             path[component_start + 1] == '.');
}
}

WelovecGpuStatus load_firmware_blob(const char *path, uint32_t maximum_size,
                                    const uint8_t expected_sha256[32],
                                    FirmwareBlob *out_blob) {
    if (!out_blob) return WELOVEC_GPU_INVALID_ARGUMENT;
    *out_blob = {};
    if (!valid_firmware_path(path) || !expected_sha256 || !maximum_size ||
        maximum_size > kMaximumGpuFirmwareBytes)
        return WELOVEC_GPU_INVALID_ARGUMENT;

    const uint32_t reported_size = fs_file_size(path);
    if (!reported_size) return WELOVEC_GPU_NOT_READY;
    if (reported_size > maximum_size) return WELOVEC_GPU_NO_RESOURCES;

    uint8_t *data = nullptr;
    uint32_t size = 0;
    if (!vfs_read_file(path, &data, &size)) {
        if (data) vfs_free_file(data);
        return WELOVEC_GPU_NOT_READY;
    }
    if (!data) return WELOVEC_GPU_NOT_READY;
    if (!size || size > maximum_size) {
        vfs_free_file(data);
        return WELOVEC_GPU_NO_RESOURCES;
    }

    uint8_t digest[32]{};
    welovec::security::crypto::sha256(data, size, digest);
    if (!welovec::security::crypto::constant_time_equal(
            digest, expected_sha256, sizeof(digest))) {
        vfs_free_file(data);
        welovec::security::crypto::secure_zero(digest, sizeof(digest));
        return WELOVEC_GPU_INTEGRITY_ERROR;
    }

    out_blob->data = data;
    out_blob->size = size;
    for (uint32_t index = 0; index < sizeof(digest); ++index)
        out_blob->sha256[index] = digest[index];
    welovec::security::crypto::secure_zero(digest, sizeof(digest));
    return WELOVEC_GPU_OK;
}

WelovecGpuStatus load_firmware_blob(const FirmwareManifestEntry &entry,
                                    FirmwareBlob *out_blob) {
    return load_firmware_blob(entry.path, entry.maximum_size, entry.sha256,
                              out_blob);
}

void release_firmware_blob(FirmwareBlob *blob) {
    if (!blob) return;
    if (blob->data) vfs_free_file(blob->data);
    *blob = {};
}

WelovecGpuStatus inspect_amdgpu_firmware(const FirmwareBlob &blob,
                                         AmdFirmwareInfo *out_info,
                                         AmdFirmwareSlice *out_ucode) {
    if (!out_info)
        return WELOVEC_GPU_INVALID_ARGUMENT;
    *out_info = {};
    if (out_ucode) *out_ucode = {};
    AmdFirmwareInfo info{};
    if (!read_common_header(blob, &info) ||
        !valid_ucode_range(info, blob.size))
        return WELOVEC_GPU_INTEGRITY_ERROR;

    *out_info = info;
    if (out_ucode && info.ucode_size) {
        out_ucode->data = blob.data + info.ucode_offset;
        out_ucode->size = info.ucode_size;
        out_ucode->version = info.ucode_version;
    }
    return WELOVEC_GPU_OK;
}

WelovecGpuStatus validate_amdgpu_psp_v1_firmware(
    const FirmwareBlob &blob) {
    AmdFirmwareInfo info{};
    if (!read_common_header(blob, &info) ||
        info.header_version_major != 1 ||
        info.header_size < kAmdPspV1HeaderSize || !info.ucode_size ||
        !valid_ucode_range(info, blob.size))
        return WELOVEC_GPU_INTEGRITY_ERROR;

    /* PSP v1.0 standalone images store the legacy SoS descriptor after the
       common header. Its payload offsets are relative to the ucode array. */
    const uint32_t component_offset = read_le32(blob.data + 36);
    const uint32_t component_size = read_le32(blob.data + 40);
    if (component_size &&
        !valid_payload_range(info, component_offset, component_size))
        return WELOVEC_GPU_INTEGRITY_ERROR;
    return WELOVEC_GPU_OK;
}

WelovecGpuStatus validate_amdgpu_ta_firmware(const FirmwareBlob &blob) {
    AmdFirmwareInfo info{};
    if (!read_common_header(blob, &info) || !info.ucode_size ||
        !valid_ucode_range(info, blob.size))
        return WELOVEC_GPU_INTEGRITY_ERROR;

    if (info.header_version_major == 1) {
        if (info.header_size < kAmdTaV1HeaderSize)
            return WELOVEC_GPU_INTEGRITY_ERROR;
        for (uint32_t index = 0; index < kAmdTaV1DescriptorCount; ++index) {
            const uint8_t *descriptor = blob.data +
                kAmdCommonHeaderSize + index * kAmdTaV1DescriptorSize;
            const uint32_t offset = read_le32(descriptor + 4);
            const uint32_t size = read_le32(descriptor + 8);
            if (size && !valid_payload_range(info, offset, size))
                return WELOVEC_GPU_INTEGRITY_ERROR;
        }
        return WELOVEC_GPU_OK;
    }

    if (info.header_version_major != 2 ||
        info.header_version_minor != 0)
        return WELOVEC_GPU_UNSUPPORTED;
    if (blob.size < 36 || info.header_size < 36)
        return WELOVEC_GPU_INTEGRITY_ERROR;

    const uint32_t component_count = read_le32(blob.data + 32);
    if (!component_count || component_count > kAmdPspMaximumDescriptors)
        return WELOVEC_GPU_INTEGRITY_ERROR;
    const uint64_t descriptor_end = 36u +
        static_cast<uint64_t>(component_count) * kAmdPspDescriptorSize;
    /* The common header's header_size stops before this flexible table in
       AMD's v2 packages; the descriptors must fit before the payload. */
    if (descriptor_end > info.ucode_offset)
        return WELOVEC_GPU_INTEGRITY_ERROR;

    for (uint32_t index = 0; index < component_count; ++index) {
        const uint8_t *descriptor = blob.data + 36u +
            index * kAmdPspDescriptorSize;
        const uint32_t offset = read_le32(descriptor + 8);
        const uint32_t size = read_le32(descriptor + 12);
        if (!valid_payload_range(info, offset, size))
            return WELOVEC_GPU_INTEGRITY_ERROR;
    }
    return WELOVEC_GPU_OK;
}

WelovecGpuStatus find_amdgpu_psp_component(
    const FirmwareBlob &blob, AmdPspFirmwareType type,
    AmdFirmwareSlice *out_component) {
    if (!out_component || type <= AMD_PSP_FW_UNKNOWN ||
        type > AMD_PSP_FW_SPDM_DRIVER)
        return WELOVEC_GPU_INVALID_ARGUMENT;
    *out_component = {};

    AmdFirmwareInfo info{};
    if (!read_common_header(blob, &info) ||
        !valid_ucode_range(info, blob.size))
        return WELOVEC_GPU_INTEGRITY_ERROR;
    if (info.header_version_major != 2 ||
        (info.header_version_minor != 0 && info.header_version_minor != 1))
        return WELOVEC_GPU_UNSUPPORTED;
    const uint32_t fixed_header_size =
        info.header_version_minor == 0 ? 36u : 40u;
    if (info.header_size < fixed_header_size)
        return WELOVEC_GPU_INTEGRITY_ERROR;
    if (blob.size < kAmdCommonHeaderSize + sizeof(uint32_t))
        return WELOVEC_GPU_INTEGRITY_ERROR;

    const uint32_t component_count = read_le32(blob.data + 32);
    if (!component_count || component_count > kAmdPspMaximumDescriptors)
        return WELOVEC_GPU_INTEGRITY_ERROR;
    const uint32_t descriptor_offset = info.header_version_minor == 0 ? 36 : 40;
    const uint64_t descriptor_end =
        static_cast<uint64_t>(descriptor_offset) +
        static_cast<uint64_t>(component_count) * kAmdPspDescriptorSize;
    /* PSP package descriptors follow the fixed header and precede the
       ucode_array_offset payload. header_size only covers the fixed header. */
    if (descriptor_end > info.ucode_offset)
        return WELOVEC_GPU_INTEGRITY_ERROR;

    bool found = false;
    AmdFirmwareSlice component{};
    for (uint32_t index = 0; index < component_count; ++index) {
        const uint8_t *descriptor = blob.data + descriptor_offset +
            index * kAmdPspDescriptorSize;
        const uint32_t firmware_type = read_le32(descriptor);
        const uint32_t firmware_version = read_le32(descriptor + 4);
        const uint32_t offset = read_le32(descriptor + 8);
        const uint32_t size = read_le32(descriptor + 12);
        if (!valid_payload_range(info, offset, size))
            return WELOVEC_GPU_INTEGRITY_ERROR;
        if (firmware_type != static_cast<uint32_t>(type)) continue;
        if (found) return WELOVEC_GPU_INTEGRITY_ERROR;
        component.data = blob.data + info.ucode_offset + offset;
        component.size = size;
        component.version = firmware_version;
        found = true;
    }

    if (!found) return WELOVEC_GPU_UNSUPPORTED;
    *out_component = component;
    return WELOVEC_GPU_OK;
}

}
