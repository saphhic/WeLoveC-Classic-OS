// Bounded, hash-pinned firmware loading for GPU drivers.

#ifndef WELOVEC_DISPLAY_GPU_FIRMWARE_HPP
#define WELOVEC_DISPLAY_GPU_FIRMWARE_HPP

#include "../gpu.hpp"

namespace welovec::display::gpu {

constexpr uint32_t kMaximumGpuFirmwareBytes = 64u * 1024u * 1024u;

struct FirmwareBlob {
    uint8_t *data;
    uint32_t size;
    uint8_t sha256[32];
};

struct FirmwareManifestEntry {
    const char *path;
    uint32_t maximum_size;
    uint8_t sha256[32];
};

struct AmdFirmwareInfo {
    uint32_t image_size;
    uint32_t header_size;
    uint16_t header_version_major;
    uint16_t header_version_minor;
    uint16_t ip_version_major;
    uint16_t ip_version_minor;
    uint32_t ucode_version;
    uint32_t ucode_size;
    uint32_t ucode_offset;
    uint32_t ucode_crc32;
};

struct AmdFirmwareSlice {
    const uint8_t *data;
    uint32_t size;
    uint32_t version;
};

enum AmdPspFirmwareType : uint32_t {
    AMD_PSP_FW_UNKNOWN = 0,
    AMD_PSP_FW_SOS = 1,
    AMD_PSP_FW_SYSTEM_DRIVER = 2,
    AMD_PSP_FW_KDB = 3,
    AMD_PSP_FW_TOC = 4,
    AMD_PSP_FW_SPL = 5,
    AMD_PSP_FW_RL = 6,
    AMD_PSP_FW_SOC_DRIVER = 7,
    AMD_PSP_FW_INTERFACE_DRIVER = 8,
    AMD_PSP_FW_DEBUG_DRIVER = 9,
    AMD_PSP_FW_RAS_DRIVER = 10,
    AMD_PSP_FW_IP_KEY_MANAGER = 11,
    AMD_PSP_FW_SPDM_DRIVER = 12
};


WelovecGpuStatus load_firmware_blob(const char *path, uint32_t maximum_size,
                                    const uint8_t expected_sha256[32],
                                    FirmwareBlob *out_blob);
WelovecGpuStatus load_firmware_blob(const FirmwareManifestEntry &entry,
                                    FirmwareBlob *out_blob);
void release_firmware_blob(FirmwareBlob *blob);

WelovecGpuStatus inspect_amdgpu_firmware(const FirmwareBlob &blob,
                                         AmdFirmwareInfo *out_info,
                                         AmdFirmwareSlice *out_ucode);
WelovecGpuStatus validate_amdgpu_psp_v1_firmware(
    const FirmwareBlob &blob);
WelovecGpuStatus validate_amdgpu_ta_firmware(const FirmwareBlob &blob);
// PSP package header v2.0/v2.1 stores typed, bounded slices in the same file.
WelovecGpuStatus find_amdgpu_psp_component(
    const FirmwareBlob &blob, AmdPspFirmwareType type,
    AmdFirmwareSlice *out_component);

}

#endif
