// Internal contract between the GPU manager and PCI device drivers.

#ifndef WELOVEC_DISPLAY_GPU_DRIVER_HPP
#define WELOVEC_DISPLAY_GPU_DRIVER_HPP

#include "../gpu.hpp"
#include "arch/x86_64/pci/pci.hpp"

namespace welovec::display::gpu {

struct Driver;

struct Device {
    welovec::x86_64::pci::Device pci;
    const Driver *driver;
    welovec::x86_64::pci::Bar pci_bars[
        welovec::x86_64::pci::kMaximumBars];
    uint8_t pci_bar_count;
    bool pci_bars_valid;
    bool initialized;
    void *driver_data;
    WelovecGpuDeviceInfo info;
};

using Probe = bool (*)(Device &device);
using Initialize = WelovecGpuStatus (*)(Device &device);
using Shutdown = void (*)(Device &device);
using GetDisplayModes = WelovecGpuStatus (*)(
    Device &device, WelovecGpuDisplayMode *modes, uint32_t capacity,
    uint32_t *out_count);
using GetCurrentDisplayMode = WelovecGpuStatus (*)(
    Device &device, WelovecGpuDisplayMode *out_mode);
using SetDisplayMode = WelovecGpuStatus (*)(
    Device &device, const WelovecGpuDisplayMode &mode);
using Present = WelovecGpuStatus (*)(
    Device &device, const WelovecGpuSurface &surface);

struct Driver {
    uint16_t vendor_id;
    uint16_t device_id;
    const char *name;
    Probe probe;
    Initialize initialize;
    Shutdown shutdown;
    GetDisplayModes get_display_modes;
    GetCurrentDisplayMode get_current_display_mode;
    SetDisplayMode set_display_mode;
    Present present;
};

const Driver *const *registered_drivers(uint32_t *out_count);
extern const Driver kRembrandtDriver;

}

#endif
