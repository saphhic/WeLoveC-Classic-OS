// Registry-driven GPU discovery and common display-operation dispatch.

#include "gpu.hpp"

#include "Drivers/gpu_driver.hpp"
#include "arch/x86_64/pci/pci.hpp"
#include "arch/x86_64/scheduler/task_lock.hpp"
#include "framebuffer.hpp"

namespace {

using welovec::display::gpu::Device;
using welovec::display::gpu::Driver;
using PciDevice = welovec::x86_64::pci::Device;

constexpr uint32_t kMaximumGpuDevices = 8;
constexpr uint32_t kBytesPerPixel32 = 4;
constexpr uint32_t kBytesPerPixel16 = 2;

Device gDevices[kMaximumGpuDevices]{};
uint32_t gDeviceCount = 0;
uint32_t gScanState = WELOVEC_GPU_SCAN_NOT_STARTED;
welovec::x86_64::scheduler::TaskLock gGpuTaskLock;

class GpuTaskLockGuard {
public:
    GpuTaskLockGuard()
        : flags_(gGpuTaskLock.lock()) {}
    ~GpuTaskLockGuard() { gGpuTaskLock.unlock(flags_); }
    GpuTaskLockGuard(const GpuTaskLockGuard &) = delete;
    GpuTaskLockGuard &operator=(const GpuTaskLockGuard &) = delete;
private:
    uint64_t flags_;
};

uint32_t bytes_per_pixel(uint32_t format) {
    switch (format) {
        case WELOVEC_GPU_PIXEL_XRGB8888:
        case WELOVEC_GPU_PIXEL_ARGB8888:
            return kBytesPerPixel32;
        case WELOVEC_GPU_PIXEL_RGB565:
            return kBytesPerPixel16;
        default:
            return 0;
    }
}

bool valid_mode(const WelovecGpuDisplayMode &mode) {
    return mode.width != 0 && mode.height != 0 &&
           bytes_per_pixel(mode.pixel_format) != 0;
}

bool valid_surface(const WelovecGpuSurface &surface) {
    const uint32_t pixel_bytes = bytes_per_pixel(surface.pixel_format);
    if (!surface.pixels || !surface.width || !surface.height || !pixel_bytes ||
        surface.width > UINT32_MAX / pixel_bytes) return false;
    const uint32_t minimum_pitch = surface.width * pixel_bytes;
    if (surface.pitch_bytes < minimum_pitch ||
        surface.height > UINT64_MAX / surface.pitch_bytes) return false;
    return surface.size_bytes >=
        static_cast<uint64_t>(surface.pitch_bytes) * surface.height;
}

bool driver_can_present(Device &device, uint32_t width,
                        uint32_t height, uint32_t pixel_format) {
    const Driver *driver = device.driver;
    if (!device.initialized ||
        !(device.info.capabilities & WELOVEC_GPU_CAP_PRESENT) || !driver ||
        !driver->present || !driver->get_current_display_mode) return false;

    WelovecGpuDisplayMode mode{};
    if (driver->get_current_display_mode(device, &mode) != WELOVEC_GPU_OK ||
        !valid_mode(mode)) return false;
    return mode.width == width && mode.height == height &&
           mode.pixel_format == pixel_format;
}

WelovecGpuStatus start_bound_drivers(bool retry_failed) {
    WelovecGpuStatus result = WELOVEC_GPU_OK;
    for (uint32_t i = 0; i < gDeviceCount; ++i) {
        Device &device = gDevices[i];
        if (device.initialized) continue;
        if (device.info.state == WELOVEC_GPU_DEVICE_FAILED) {
            if (!retry_failed) continue;
            if (!welovec::display::gpu::release_mmio_regions(device)) {
                result = WELOVEC_GPU_HARDWARE_ERROR;
                continue;
            }
            welovec::display::gpu::release_dma_regions(device);
            device.info.state = WELOVEC_GPU_DEVICE_PROBED;
            if (device.firmware_status != WELOVEC_GPU_OK) {
                device.firmware_checked = false;
                device.info.flags &=
                    ~WELOVEC_GPU_DEVICE_FLAG_FIRMWARE_VALID;
            }
        }
        const Driver *driver = device.driver;
        if (!driver) {
            device.info.state = WELOVEC_GPU_DEVICE_FAILED;
            result = WELOVEC_GPU_HARDWARE_ERROR;
            continue;
        }

        if (driver->firmware_preflight && !device.firmware_checked) {
            device.firmware_status = driver->firmware_preflight(device);
            device.firmware_checked = true;
            if (device.firmware_status == WELOVEC_GPU_OK) {
                device.info.flags |=
                    WELOVEC_GPU_DEVICE_FLAG_FIRMWARE_VALID;
            } else {
                device.info.flags &=
                    ~WELOVEC_GPU_DEVICE_FLAG_FIRMWARE_VALID;
            }
        }

        if (driver->initialize && driver->firmware_preflight &&
            device.firmware_status != WELOVEC_GPU_OK) {
            device.info.capabilities = 0;
            device.info.state = WELOVEC_GPU_DEVICE_FAILED;
            result = device.firmware_status;
            continue;
        }
        if (!driver->initialize) continue;

        const WelovecGpuStatus status = driver->initialize(device);
        if (status == WELOVEC_GPU_OK) {
            device.initialized = true;
            device.info.state = WELOVEC_GPU_DEVICE_INITIALIZED;
        } else {
            if (driver->shutdown) driver->shutdown(device);
            const bool resources_released =
                welovec::display::gpu::release_mmio_regions(device);
            welovec::display::gpu::release_dma_regions(device);
            device.info.capabilities = 0;
            device.info.state = WELOVEC_GPU_DEVICE_FAILED;
            result = resources_released ? status : WELOVEC_GPU_HARDWARE_ERROR;
        }
    }
    return result;
}

struct ScanContext {
    const WelovecBootInfo *boot_info;
};

void apply_firmware_display_resources(Device &device,
                                      const WelovecBootInfo *boot_info) {
    if (!boot_info ||
        !(boot_info->flags & WELOVEC_BOOT_FLAG_DISPLAY_PCI_VALID) ||
        !boot_info->display_pci.valid || boot_info->display_pci.segment != 0 ||
        boot_info->display_pci.bus != device.pci.bus ||
        boot_info->display_pci.slot != device.pci.slot ||
        boot_info->display_pci.function != device.pci.function) return;

    device.info.flags |= WELOVEC_GPU_DEVICE_FLAG_GOP_PROVIDER;
    const WelovecFramebuffer &framebuffer = boot_info->framebuffer;
    if (framebuffer.base && framebuffer.size &&
        framebuffer.base <= UINT64_MAX - framebuffer.size) {
        device.firmware_framebuffer_base = framebuffer.base;
        device.firmware_framebuffer_size = framebuffer.size;
    }

    uint8_t valid_size_count = 0;
    const uint32_t bar_count = boot_info->display_pci.bar_count <
            welovec::x86_64::pci::kMaximumBars
        ? boot_info->display_pci.bar_count
        : welovec::x86_64::pci::kMaximumBars;
    for (uint32_t index = 0; index < bar_count; ++index) {
        const WelovecPciBarResource &resource =
            boot_info->display_pci.bars[index];
        const auto &bar = device.pci_bars[index];
        if (!(resource.flags & WELOVEC_PCI_RESOURCE_VALID) ||
            resource.type != WELOVEC_PCI_RESOURCE_MEMORY || !resource.size ||
            resource.base != bar.address || !bar.assigned ||
            (bar.type != welovec::x86_64::pci::BarType::Memory32 &&
             bar.type != welovec::x86_64::pci::BarType::Memory64) ||
            resource.base > UINT64_MAX - resource.size) continue;
        device.pci_bar_sizes[index] = resource.size;
        device.pci_bar_size_valid_mask |= static_cast<uint8_t>(1u << index);
        ++valid_size_count;
    }
    if (valid_size_count)
        device.info.flags |= WELOVEC_GPU_DEVICE_FLAG_UEFI_BAR_SIZES;
}

bool gpu_pci_visitor(const PciDevice &pci_device, void *context) {
    auto *scan_context = static_cast<ScanContext *>(context);
    uint32_t driver_count = 0;
    const Driver *const *drivers =
        welovec::display::gpu::registered_drivers(&driver_count);
    for (uint32_t driver_index = 0; driver_index < driver_count;
         ++driver_index) {
        const Driver *driver = drivers[driver_index];
        if (!driver || !driver->probe ||
            driver->vendor_id != pci_device.vendor_id ||
            driver->device_id != pci_device.device_id) continue;

        Device candidate{};
        candidate.pci = pci_device;
        candidate.driver = driver;
        candidate.info.version = WELOVEC_GPU_INFO_VERSION;
        candidate.info.struct_size = sizeof(candidate.info);
        candidate.info.vendor_id = pci_device.vendor_id;
        candidate.info.device_id = pci_device.device_id;
        candidate.info.bus = pci_device.bus;
        candidate.info.slot = pci_device.slot;
        candidate.info.function = pci_device.function;
        candidate.info.class_code = pci_device.class_code;
        candidate.info.subclass = pci_device.subclass;
        candidate.info.prog_if = pci_device.prog_if;

        candidate.pci_bars_valid = welovec::x86_64::pci::read_bars(
            pci_device, candidate.pci_bars, &candidate.pci_bar_count);
        if (!candidate.pci_bars_valid) candidate.pci_bar_count = 0;
        if (scan_context)
            apply_firmware_display_resources(candidate,
                                              scan_context->boot_info);

        if (!driver->probe(candidate)) continue;
        candidate.info.version = WELOVEC_GPU_INFO_VERSION;
        candidate.info.struct_size = sizeof(candidate.info);
        candidate.info.state = WELOVEC_GPU_DEVICE_PROBED;
        candidate.info.capabilities = 0;
        candidate.info.driver_name = driver->name;

        gDevices[gDeviceCount++] = candidate;
        if (gDeviceCount >= kMaximumGpuDevices) return true;
        return false;
    }
    return false;
}

WelovecGpuStatus ready_device(uint32_t index, Device **out_device) {
    const uint32_t scan_state = __atomic_load_n(&gScanState, __ATOMIC_ACQUIRE);
    if (scan_state != WELOVEC_GPU_SCAN_COMPLETE)
        return scan_state == WELOVEC_GPU_SCAN_IN_PROGRESS
            ? WELOVEC_GPU_BUSY : WELOVEC_GPU_NOT_READY;
    if (index >= gDeviceCount) return WELOVEC_GPU_NO_DEVICE;
    if (out_device) *out_device = &gDevices[index];
    return WELOVEC_GPU_OK;
}

}

extern "C" WelovecGpuStatus welovec_gpu_initialize(
    const WelovecBootInfo *boot_info) {
    uint32_t expected = WELOVEC_GPU_SCAN_NOT_STARTED;
    if (!__atomic_compare_exchange_n(&gScanState, &expected,
                                     WELOVEC_GPU_SCAN_IN_PROGRESS, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (expected == WELOVEC_GPU_SCAN_COMPLETE) {
            GpuTaskLockGuard guard;
            return start_bound_drivers(true);
        }
        if (expected == WELOVEC_GPU_SCAN_IN_PROGRESS) return WELOVEC_GPU_BUSY;
        return WELOVEC_GPU_HARDWARE_ERROR;
    }

    uint32_t driver_count = 0;
    if (!welovec::display::gpu::registered_drivers(&driver_count) ||
        driver_count == 0) {
        __atomic_store_n(&gScanState, WELOVEC_GPU_SCAN_FAILED, __ATOMIC_RELEASE);
        return WELOVEC_GPU_HARDWARE_ERROR;
    }

    /* PCI enumerate returns whether a visitor stopped early. A complete scan
       with zero matches is valid and is reported as SCAN_COMPLETE. */
    ScanContext scan_context{};
    if (boot_info && boot_info->magic == WELOVEC_BOOTINFO_MAGIC &&
        boot_info->version == WELOVEC_BOOTINFO_VERSION &&
        boot_info->size >= sizeof(WelovecBootInfo))
        scan_context.boot_info = boot_info;
    static_cast<void>(welovec::x86_64::pci::enumerate(
        gpu_pci_visitor, &scan_context));
    WelovecGpuStatus start_status;
    {
        GpuTaskLockGuard guard;
        start_status = start_bound_drivers(false);
    }
    __atomic_store_n(&gScanState, WELOVEC_GPU_SCAN_COMPLETE, __ATOMIC_RELEASE);
    return start_status;
}

extern "C" WelovecGpuStatus welovec_gpu_shutdown(void) {
    GpuTaskLockGuard guard;
    if (__atomic_load_n(&gScanState, __ATOMIC_ACQUIRE) !=
        WELOVEC_GPU_SCAN_COMPLETE) return WELOVEC_GPU_NOT_READY;

    WelovecGpuStatus result = WELOVEC_GPU_OK;
    for (uint32_t i = gDeviceCount; i > 0; --i) {
        Device &device = gDevices[i - 1];
        const Driver *driver = device.driver;
        if (device.initialized && driver && driver->shutdown)
            driver->shutdown(device);
        device.initialized = false;
        device.info.capabilities = 0;
        const bool resources_released =
            welovec::display::gpu::release_mmio_regions(device);
        welovec::display::gpu::release_dma_regions(device);
        device.info.state = resources_released
            ? WELOVEC_GPU_DEVICE_PROBED : WELOVEC_GPU_DEVICE_FAILED;
        if (!resources_released) result = WELOVEC_GPU_HARDWARE_ERROR;
    }
    return result;
}

extern "C" WelovecGpuStatus welovec_gpu_get_system_info(
    WelovecGpuSystemInfo *out_info) {
    if (!out_info) return WELOVEC_GPU_INVALID_ARGUMENT;
    GpuTaskLockGuard guard;
    if (__atomic_load_n(&gScanState, __ATOMIC_ACQUIRE) !=
        WELOVEC_GPU_SCAN_COMPLETE) return WELOVEC_GPU_NOT_READY;

    WelovecGpuSystemInfo info{};
    info.version = WELOVEC_GPU_INFO_VERSION;
    info.struct_size = sizeof(info);
    info.scan_state = WELOVEC_GPU_SCAN_COMPLETE;
    info.device_count = gDeviceCount;
    if (framebuffer_available() &&
        framebuffer_get_resolution(&info.display_width, &info.display_height)) {
        info.display_backend = WELOVEC_GPU_DISPLAY_UEFI_GOP;
        info.display_pitch = pitch;
    }
    for (uint32_t i = 0; i < gDeviceCount; ++i) {
        Device &device = gDevices[i];
        if (!device.initialized) continue;
        const Driver *driver = device.driver;
        if (!driver || !driver->get_current_display_mode) continue;
        WelovecGpuDisplayMode current_mode{};
        if (driver->get_current_display_mode(device, &current_mode) !=
                WELOVEC_GPU_OK || !valid_mode(current_mode)) continue;
        info.display_backend = WELOVEC_GPU_DISPLAY_NATIVE;
        info.display_width = current_mode.width;
        info.display_height = current_mode.height;
        info.display_pitch = 0;
        break;
    }
    *out_info = info;
    return WELOVEC_GPU_OK;
}

extern "C" WelovecGpuStatus welovec_gpu_get_device_info(
    uint32_t index, WelovecGpuDeviceInfo *out_info) {
    if (!out_info) return WELOVEC_GPU_INVALID_ARGUMENT;
    GpuTaskLockGuard guard;
    Device *device = nullptr;
    const WelovecGpuStatus status = ready_device(index, &device);
    if (status != WELOVEC_GPU_OK) return status;
    *out_info = device->info;
    return WELOVEC_GPU_OK;
}

extern "C" WelovecGpuStatus welovec_gpu_get_display_modes(
    uint32_t device_index, WelovecGpuDisplayMode *modes,
    uint32_t mode_capacity, uint32_t *out_mode_count) {
    if (!out_mode_count || (mode_capacity && !modes))
        return WELOVEC_GPU_INVALID_ARGUMENT;
    *out_mode_count = 0;
    GpuTaskLockGuard guard;
    Device *device = nullptr;
    const WelovecGpuStatus status = ready_device(device_index, &device);
    if (status != WELOVEC_GPU_OK) return status;
    const Driver *driver = device->driver;
    if (!driver || !driver->get_display_modes) return WELOVEC_GPU_UNSUPPORTED;
    if (!device->initialized) return WELOVEC_GPU_NOT_READY;
    return driver->get_display_modes(*device, modes, mode_capacity,
                                     out_mode_count);
}

extern "C" WelovecGpuStatus welovec_gpu_set_display_mode(
    uint32_t device_index, const WelovecGpuDisplayMode *mode) {
    if (!mode || !valid_mode(*mode)) return WELOVEC_GPU_INVALID_ARGUMENT;
    GpuTaskLockGuard guard;
    Device *device = nullptr;
    const WelovecGpuStatus status = ready_device(device_index, &device);
    if (status != WELOVEC_GPU_OK) return status;
    const Driver *driver = device->driver;
    if (!driver || !driver->set_display_mode) return WELOVEC_GPU_UNSUPPORTED;
    if (!device->initialized) return WELOVEC_GPU_NOT_READY;
    return driver->set_display_mode(*device, *mode);
}

extern "C" WelovecGpuStatus welovec_gpu_get_current_display_mode(
    uint32_t device_index, WelovecGpuDisplayMode *out_mode) {
    if (!out_mode) return WELOVEC_GPU_INVALID_ARGUMENT;
    GpuTaskLockGuard guard;
    Device *device = nullptr;
    const WelovecGpuStatus status = ready_device(device_index, &device);
    if (status != WELOVEC_GPU_OK) return status;
    const Driver *driver = device->driver;
    if (!driver || !driver->get_current_display_mode)
        return WELOVEC_GPU_UNSUPPORTED;
    if (!device->initialized) return WELOVEC_GPU_NOT_READY;
    WelovecGpuDisplayMode mode{};
    const WelovecGpuStatus mode_status =
        driver->get_current_display_mode(*device, &mode);
    if (mode_status != WELOVEC_GPU_OK) return mode_status;
    if (!valid_mode(mode)) return WELOVEC_GPU_HARDWARE_ERROR;
    *out_mode = mode;
    return WELOVEC_GPU_OK;
}

extern "C" WelovecGpuStatus welovec_gpu_present(
    uint32_t device_index, const WelovecGpuSurface *surface) {
    if (!surface || !valid_surface(*surface))
        return WELOVEC_GPU_INVALID_ARGUMENT;
    GpuTaskLockGuard guard;
    Device *device = nullptr;
    const WelovecGpuStatus status = ready_device(device_index, &device);
    if (status != WELOVEC_GPU_OK) return status;
    const Driver *driver = device->driver;
    if (!driver || !driver->present ||
        !(device->info.capabilities & WELOVEC_GPU_CAP_PRESENT))
        return WELOVEC_GPU_UNSUPPORTED;
    if (!device->initialized) return WELOVEC_GPU_NOT_READY;
    return driver->present(*device, *surface);
}

extern "C" bool welovec_gpu_can_present(uint32_t surface_width,
                                         uint32_t surface_height,
                                         uint32_t pixel_format) {
    const uint32_t pixel_bytes = bytes_per_pixel(pixel_format);
    if (!surface_width || !surface_height || !pixel_bytes ||
        surface_width > UINT32_MAX / pixel_bytes) return false;
    GpuTaskLockGuard guard;
    if (__atomic_load_n(&gScanState, __ATOMIC_ACQUIRE) !=
        WELOVEC_GPU_SCAN_COMPLETE) return false;
    for (uint32_t i = 0; i < gDeviceCount; ++i)
        if (driver_can_present(gDevices[i], surface_width, surface_height,
                               pixel_format)) return true;
    return false;
}

extern "C" WelovecGpuStatus welovec_gpu_present_active(
    const WelovecGpuSurface *surface) {
    if (!surface || !valid_surface(*surface))
        return WELOVEC_GPU_INVALID_ARGUMENT;
    GpuTaskLockGuard guard;
    if (__atomic_load_n(&gScanState, __ATOMIC_ACQUIRE) !=
        WELOVEC_GPU_SCAN_COMPLETE) return WELOVEC_GPU_NOT_READY;

    WelovecGpuStatus last_status = WELOVEC_GPU_UNSUPPORTED;
    for (uint32_t i = 0; i < gDeviceCount; ++i) {
        Device &device = gDevices[i];
        if (!driver_can_present(device, surface->width, surface->height,
                                surface->pixel_format)) continue;
        const WelovecGpuStatus status =
            device.driver->present(device, *surface);
        if (status == WELOVEC_GPU_OK) return status;
        last_status = status;
    }
    return last_status;
}
