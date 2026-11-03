// GPU discovery and display-driver interface for the DisplayService.

#ifndef WELOVEC_DISPLAY_GPU_HPP
#define WELOVEC_DISPLAY_GPU_HPP

#include <stdint.h>
#include "boot/boot_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WELOVEC_GPU_INFO_VERSION 2u
#define WELOVEC_GPU_MAX_PCI_BARS 6u

enum {
    WELOVEC_GPU_DEVICE_FLAG_GOP_PROVIDER = 1u << 0,
    WELOVEC_GPU_DEVICE_FLAG_UEFI_BAR_SIZES = 1u << 1,
    WELOVEC_GPU_DEVICE_FLAG_FIRMWARE_VALID = 1u << 2,
    WELOVEC_GPU_DEVICE_FLAG_PSP_BOOTLOADER_READY = 1u << 3,
    WELOVEC_GPU_DEVICE_FLAG_PSP_TOS_READY = 1u << 4
};

typedef enum WelovecGpuStatus {
    WELOVEC_GPU_OK = 0,
    WELOVEC_GPU_NOT_READY = 1,
    WELOVEC_GPU_INVALID_ARGUMENT = 2,
    WELOVEC_GPU_NO_DEVICE = 3,
    WELOVEC_GPU_UNSUPPORTED = 4,
    WELOVEC_GPU_BUSY = 5,
    WELOVEC_GPU_HARDWARE_ERROR = 6,
    WELOVEC_GPU_NO_RESOURCES = 7,
    WELOVEC_GPU_INTEGRITY_ERROR = 8
} WelovecGpuStatus;

typedef enum WelovecGpuScanState {
    WELOVEC_GPU_SCAN_NOT_STARTED = 0,
    WELOVEC_GPU_SCAN_IN_PROGRESS = 1,
    WELOVEC_GPU_SCAN_COMPLETE = 2,
    WELOVEC_GPU_SCAN_FAILED = 3
} WelovecGpuScanState;

typedef enum WelovecGpuDeviceState {
    WELOVEC_GPU_DEVICE_PROBED = 1,
    WELOVEC_GPU_DEVICE_INITIALIZED = 2,
    WELOVEC_GPU_DEVICE_FAILED = 3
} WelovecGpuDeviceState;

typedef enum WelovecGpuDisplayBackend {
    WELOVEC_GPU_DISPLAY_NONE = 0,
    WELOVEC_GPU_DISPLAY_UEFI_GOP = 1,
    WELOVEC_GPU_DISPLAY_NATIVE = 2
} WelovecGpuDisplayBackend;

typedef enum WelovecGpuBarType {
    WELOVEC_GPU_BAR_ABSENT = 0,
    WELOVEC_GPU_BAR_IO = 1,
    WELOVEC_GPU_BAR_MEMORY_BELOW_1M = 2,
    WELOVEC_GPU_BAR_MEMORY_32 = 3,
    WELOVEC_GPU_BAR_MEMORY_64 = 4,
    WELOVEC_GPU_BAR_CONTINUATION = 5,
    WELOVEC_GPU_BAR_RESERVED = 6
} WelovecGpuBarType;

typedef enum WelovecGpuCapability {
    WELOVEC_GPU_CAP_MODE_QUERY = 1u << 0,
    WELOVEC_GPU_CAP_MODESET = 1u << 1,
    WELOVEC_GPU_CAP_PRESENT = 1u << 2,
    WELOVEC_GPU_CAP_RENDER = 1u << 3,
    WELOVEC_GPU_CAP_COMPUTE = 1u << 4,
    WELOVEC_GPU_CAP_VIDEO_DECODE = 1u << 5,
    WELOVEC_GPU_CAP_VIDEO_ENCODE = 1u << 6
} WelovecGpuCapability;

typedef enum WelovecGpuPixelFormat {
    WELOVEC_GPU_PIXEL_XRGB8888 = 1,
    WELOVEC_GPU_PIXEL_ARGB8888 = 2,
    WELOVEC_GPU_PIXEL_RGB565 = 3
} WelovecGpuPixelFormat;

typedef struct WelovecGpuPciBarInfo {
    uint64_t address; // PCI physical address; map through an MMIO manager.
    uint64_t size;
    uint32_t type;
    uint8_t index;
    uint8_t assigned;
    uint8_t prefetchable;
    uint8_t size_valid;
} WelovecGpuPciBarInfo;

typedef struct WelovecGpuSystemInfo {
    uint32_t version;
    uint32_t struct_size;
    uint32_t scan_state;
    uint32_t device_count;
    uint32_t display_backend;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t display_pitch; // bytes; zero when the native driver omits it.
} WelovecGpuSystemInfo;

typedef struct WelovecGpuDeviceInfo {
    uint32_t version;
    uint32_t struct_size;
    uint32_t state;
    uint32_t capabilities;
    uint32_t flags;
    uint32_t bar_count; // PCI BAR slots, including absent/continuation slots.
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t revision;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t reserved;
    const char *device_name;
    const char *driver_name;
    WelovecGpuPciBarInfo bars[WELOVEC_GPU_MAX_PCI_BARS];
} WelovecGpuDeviceInfo;

typedef struct WelovecGpuDisplayMode {
    uint32_t width;
    uint32_t height;
    uint32_t refresh_millihz; // zero means the driver could not report it.
    uint32_t pixel_format;
} WelovecGpuDisplayMode;

typedef struct WelovecGpuSurface {
    const void *pixels; // kernel virtual address; never a user pointer.
    uint64_t size_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_bytes;
    uint32_t pixel_format;
} WelovecGpuSurface;

WelovecGpuStatus welovec_gpu_initialize(const WelovecBootInfo *boot_info);

WelovecGpuStatus welovec_gpu_shutdown(void);
WelovecGpuStatus welovec_gpu_get_system_info(WelovecGpuSystemInfo *out_info);
WelovecGpuStatus welovec_gpu_get_device_info(uint32_t index,
                                             WelovecGpuDeviceInfo *out_info);

WelovecGpuStatus welovec_gpu_get_display_modes(
    uint32_t device_index, WelovecGpuDisplayMode *modes,
    uint32_t mode_capacity, uint32_t *out_mode_count);
WelovecGpuStatus welovec_gpu_get_current_display_mode(
    uint32_t device_index, WelovecGpuDisplayMode *out_mode);
WelovecGpuStatus welovec_gpu_set_display_mode(
    uint32_t device_index, const WelovecGpuDisplayMode *mode);
WelovecGpuStatus welovec_gpu_present(uint32_t device_index,
                                     const WelovecGpuSurface *surface);
/* Present only through an initialized driver whose current mode matches the
   surface. Drivers must consume the kernel surface synchronously. */
bool welovec_gpu_can_present(uint32_t width, uint32_t height,
                            uint32_t pixel_format);
WelovecGpuStatus welovec_gpu_present_active(
    const WelovecGpuSurface *surface);

#ifdef __cplusplus
}
#endif

#endif
