// Passive PCI binding for the Rembrandt Radeon 680M (1002:1681).

#include "../gpu_driver.hpp"

namespace welovec::display::gpu {

namespace {
constexpr uint16_t kAmdVendorId = 0x1002;
constexpr uint16_t kRembrandtRadeon680mDeviceId = 0x1681;
constexpr uint8_t kDisplayControllerClass = 0x03;
constexpr uint8_t kVgaControllerSubclass = 0x00;

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
    nullptr, // Native hardware initialization is added in a later driver stage.
    nullptr,
    nullptr, // Native display-mode enumeration is not implemented yet.
    nullptr,
    nullptr,
    nullptr
};

}
