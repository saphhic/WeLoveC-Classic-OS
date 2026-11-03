// PSP firmware and readiness preflight for Rembrandt Radeon 680M (1002:1681).

#include "../gpu_driver.hpp"
#include "../gpu_firmware.hpp"
#include "rembrandt_firmware_manifest.hpp"

namespace welovec::display::gpu {

namespace {
constexpr uint16_t kAmdVendorId = 0x1002;
constexpr uint16_t kRembrandtRadeon680mDeviceId = 0x1681;
constexpr uint8_t kDisplayControllerClass = 0x03;
constexpr uint8_t kVgaControllerSubclass = 0x00;

/* These common MMIO register indices are documented by AMDGPU for modern
   SoCs. The BAR is not trusted until IP Discovery and PSP mailbox values
   identify the register aperture. Indices are DWORD-addressed. */
constexpr uint64_t kMmioWindowOffset = 0x50000;
constexpr uint64_t kMmioWindowSize = 0x0b000;
constexpr uint64_t kIpDiscoveryVersionOffset = 0x16a00 * 4 -
                                                kMmioWindowOffset;
constexpr uint64_t kPspC2pmsg81Offset = 0x16091 * 4 - kMmioWindowOffset;
constexpr uint32_t kTocFirmwareIndex = 0;
constexpr uint32_t kTaFirmwareIndex = 1;
constexpr uint32_t kAsdFirmwareIndex = 2;

void probe_rembrandt_psp_read_only(Device &device) {
    device.info.flags &=
        ~(WELOVEC_GPU_DEVICE_FLAG_PSP_BOOTLOADER_READY |
          WELOVEC_GPU_DEVICE_FLAG_PSP_TOS_READY);

    for (uint32_t index = 0; index < device.pci_bar_count; ++index) {
        if (!(device.pci_bar_size_valid_mask & (1u << index))) continue;

        GpuMmioRegion registers{};
        if (map_mmio_region(device, static_cast<uint8_t>(index),
                            kMmioWindowOffset, kMmioWindowSize,
                            &registers) != WELOVEC_GPU_OK) continue;

        uint32_t discovery_version = 0;
        uint32_t tos_sign_of_life = 0;
        const bool readable =
            mmio_read32(registers, kIpDiscoveryVersionOffset,
                        &discovery_version) &&
            mmio_read32(registers, kPspC2pmsg81Offset,
                        &tos_sign_of_life);

        if (readable && discovery_version == 4) {
            /* AMDGPU treats a nonzero C2PMSG_81 value as the PSP secure-OS
               sign of life on this PSP 13 family. This does not mean that a
               GPCOM command ring has been created. */
            if (tos_sign_of_life != 0)
                device.info.flags |= WELOVEC_GPU_DEVICE_FLAG_PSP_TOS_READY;
        }

        const WelovecGpuStatus unmap_status =
            unmap_mmio_region(device, &registers);
        if (unmap_status != WELOVEC_GPU_OK) return;
        if (readable && discovery_version == 4) return;
    }
}

WelovecGpuStatus validate_rembrandt_firmware(Device &device) {
    for (uint32_t index = 0;
         index < kRembrandtRequiredFirmwareCount; ++index) {
        FirmwareBlob blob{};
        const WelovecGpuStatus load_status = load_firmware_blob(
            kRembrandtFirmwareManifest[index], &blob);
        if (load_status != WELOVEC_GPU_OK) return load_status;

        AmdFirmwareInfo info{};
        WelovecGpuStatus inspect_status =
            inspect_amdgpu_firmware(blob, &info, nullptr);
        if (inspect_status == WELOVEC_GPU_OK &&
            index == kTocFirmwareIndex)
            inspect_status = validate_amdgpu_psp_v1_firmware(blob);
        else if (inspect_status == WELOVEC_GPU_OK &&
                 index == kTaFirmwareIndex)
            inspect_status = validate_amdgpu_ta_firmware(blob);
        else if (inspect_status == WELOVEC_GPU_OK &&
                 index == kAsdFirmwareIndex)
            inspect_status = validate_amdgpu_psp_v1_firmware(blob);
        release_firmware_blob(&blob);
        if (inspect_status != WELOVEC_GPU_OK) return inspect_status;
    }
    probe_rembrandt_psp_read_only(device);
    return WELOVEC_GPU_OK;
}

bool probe_rembrandt(Device &device) {
    if (device.pci.vendor_id != kAmdVendorId ||
        device.pci.device_id != kRembrandtRadeon680mDeviceId ||
        device.pci.class_code != kDisplayControllerClass ||
        device.pci.subclass != kVgaControllerSubclass) {
        return false;
    }

    device.info.state = WELOVEC_GPU_DEVICE_PROBED;
    device.info.capabilities = 0;
    device.info.vendor_id = device.pci.vendor_id;
    device.info.device_id = device.pci.device_id;
    device.info.bus = device.pci.bus;
    device.info.slot = device.pci.slot;
    device.info.function = device.pci.function;
    device.info.class_code = device.pci.class_code;
    device.info.subclass = device.pci.subclass;
    device.info.prog_if = device.pci.prog_if;
    device.info.revision = welovec::x86_64::pci::read8(device.pci, 0x08);
    device.info.device_name = "AMD Radeon 680M (Rembrandt)";

    device.info.bar_count = device.pci_bars_valid
        ? device.pci_bar_count : 0;
    for (uint32_t index = 0; index < device.info.bar_count; ++index) {
        const auto &bar = device.pci_bars[index];
        WelovecGpuPciBarInfo &reported = device.info.bars[index];
        reported.index = bar.index;
        reported.assigned = bar.assigned ? 1 : 0;
        reported.prefetchable = bar.prefetchable ? 1 : 0;
        reported.address = bar.address;
        if (device.pci_bar_size_valid_mask & (1u << index)) {
            reported.size = device.pci_bar_sizes[index];
            reported.size_valid = 1;
        }
        switch (bar.type) {
            case welovec::x86_64::pci::BarType::Io:
                reported.type = WELOVEC_GPU_BAR_IO;
                break;
            case welovec::x86_64::pci::BarType::MemoryBelow1MiB:
                reported.type = WELOVEC_GPU_BAR_MEMORY_BELOW_1M;
                break;
            case welovec::x86_64::pci::BarType::Memory32:
                reported.type = WELOVEC_GPU_BAR_MEMORY_32;
                break;
            case welovec::x86_64::pci::BarType::Memory64:
                reported.type = WELOVEC_GPU_BAR_MEMORY_64;
                break;
            case welovec::x86_64::pci::BarType::Continuation:
                reported.type = WELOVEC_GPU_BAR_CONTINUATION;
                break;
            case welovec::x86_64::pci::BarType::Reserved:
                reported.type = WELOVEC_GPU_BAR_RESERVED;
                break;
            case welovec::x86_64::pci::BarType::Absent:
            default:
                reported.type = WELOVEC_GPU_BAR_ABSENT;
                break;
        }
    }

    /* Do not reset the GPU, take over the display engine, enable bus mastering,
       map BARs, or load firmware during the discovery stage. */
    return true;
}
}

const Driver kRembrandtDriver = {
    kAmdVendorId,
    kRembrandtRadeon680mDeviceId,
    "amdgpu-rembrandt",
    probe_rembrandt,
    validate_rembrandt_firmware,
    nullptr, // GPU-visible DMA addressing is not implemented yet.
    nullptr,
    nullptr, // Native display-mode enumeration is not implemented yet.
    nullptr,
    nullptr,
    nullptr
};

}
