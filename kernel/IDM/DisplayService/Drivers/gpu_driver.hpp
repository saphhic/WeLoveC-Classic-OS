// Internal contract between the GPU manager and PCI device drivers.

#ifndef WELOVEC_DISPLAY_GPU_DRIVER_HPP
#define WELOVEC_DISPLAY_GPU_DRIVER_HPP

#include "../gpu.hpp"
#include "arch/x86_64/mm/dma/dma.hpp"
#include "arch/x86_64/pci/pci.hpp"

namespace welovec::display::gpu {

struct Driver;

struct GpuMmioRegion {
    void *mapping;
    volatile uint8_t *base;
    uint64_t physical_address;
    uint64_t size;
    uint8_t bar_index;
    uint8_t active;
    uint8_t reserved[6];
};

constexpr uint8_t kMaximumGpuMmioRegions = 16;
constexpr uint8_t kMaximumGpuDmaRegions = 16;

struct GpuDmaRegion {
    welovec::x86_64::mm::dma::Region allocation;
    uint64_t requested_size;
    uint8_t active;
    uint8_t reserved[7];
};

struct Device {
    welovec::x86_64::pci::Device pci;
    const Driver *driver;
    welovec::x86_64::pci::Bar pci_bars[
        welovec::x86_64::pci::kMaximumBars];
    uint64_t pci_bar_sizes[welovec::x86_64::pci::kMaximumBars];
    uint8_t pci_bar_size_valid_mask;
    uint8_t pci_bar_count;
    bool pci_bars_valid;
    bool initialized;
    bool firmware_checked;
    WelovecGpuStatus firmware_status;
    uint64_t firmware_framebuffer_base;
    uint64_t firmware_framebuffer_size;
    GpuMmioRegion mmio_regions[kMaximumGpuMmioRegions];
    GpuDmaRegion dma_regions[kMaximumGpuDmaRegions];
    void *driver_data;
    WelovecGpuDeviceInfo info;
};

/* These helpers only map regions with a validated size supplied by UEFI.
   Their callers are driver lifecycle callbacks serialized by the GPU service. */
WelovecGpuStatus map_mmio_region(Device &device, uint8_t bar_index,
                                 uint64_t offset, uint64_t size,
                                 GpuMmioRegion *out_region);
WelovecGpuStatus unmap_mmio_region(Device &device,
                                   GpuMmioRegion *region);
bool release_mmio_regions(Device &device);
WelovecGpuStatus allocate_dma_region(Device &device, uint64_t size_bytes,
                                     uint64_t alignment_pages,
                                     GpuDmaRegion *out_region);
WelovecGpuStatus release_dma_region(Device &device,
                                    GpuDmaRegion *region);
void release_dma_regions(Device &device);
bool mmio_read32(const GpuMmioRegion &region, uint64_t offset,
                 uint32_t *out_value);
bool mmio_write32(const GpuMmioRegion &region, uint64_t offset,
                  uint32_t value);

using Probe = bool (*)(Device &device);
using FirmwarePreflight = WelovecGpuStatus (*)(Device &device);
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
    FirmwarePreflight firmware_preflight;
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
