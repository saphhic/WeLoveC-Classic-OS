// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Defines the structures and constants for the loader-to-kernel handoff protocol.

#ifndef WELOVEC_BOOT_PROTOCOL_H
#define WELOVEC_BOOT_PROTOCOL_H

#include <stdint.h>

#define WELOVEC_BOOTINFO_MAGIC UINT64_C(0x57454C4F56454249)
#define WELOVEC_BOOTINFO_VERSION 2u
#define WELOVEC_BOOTINFO_MAX_MODULES 8u
#define WELOVEC_BOOTINFO_MAX_SEGMENTS 32u
#define WELOVEC_BOOTINFO_MAX_STORAGE 64u

enum {
    WELOVEC_MODULE_INITRD = 1u,
    WELOVEC_MODULE_FAT32_IMAGE = 2u
};

enum {
    WELOVEC_BOOT_FLAG_UEFI = 1u << 0,
    WELOVEC_BOOT_FLAG_EXIT_BOOT_SERVICES = 1u << 1,
    WELOVEC_BOOT_FLAG_GOP_VALID = 1u << 2,
    WELOVEC_BOOT_FLAG_ACPI_VALID = 1u << 3,
    WELOVEC_BOOT_FLAG_SMBIOS_VALID = 1u << 4,
    WELOVEC_BOOT_FLAG_RNG_VALID = 1u << 5,
    WELOVEC_BOOT_FLAG_NX_ENABLED = 1u << 6,
    WELOVEC_BOOT_FLAG_PAGING_ENABLED = 1u << 7,
    WELOVEC_BOOT_FLAG_INTERRUPTS_DISABLED = 1u << 8,
    WELOVEC_BOOT_FLAG_GDT_READY = 1u << 9,
    WELOVEC_BOOT_FLAG_IDT_READY = 1u << 10,
    WELOVEC_BOOT_FLAG_DISPLAY_PCI_VALID = 1u << 11
};

enum {
    WELOVEC_PCI_RESOURCE_MEMORY = 0u,
    WELOVEC_PCI_RESOURCE_IO = 1u,
    WELOVEC_PCI_RESOURCE_VALID = 1u << 0
};

enum {
    WELOVEC_ACPI_RSDP_SOURCE_NONE = 0,
    WELOVEC_ACPI_RSDP_SOURCE_EFI_ACPI_2 = 1,
    WELOVEC_ACPI_RSDP_SOURCE_EFI_ACPI_1 = 2,
    WELOVEC_ACPI_RSDP_SOURCE_EBDA = 3,
    WELOVEC_ACPI_RSDP_SOURCE_BIOS_ROM = 4,
    WELOVEC_ACPI_RSDP_SOURCE_COPY_FAILED = 5,
    WELOVEC_ACPI_RSDP_SOURCE_EFI_CONFIG_SCAN = 6
};

enum {
    WELOVEC_EFI_RESERVED_MEMORY = 0,
    WELOVEC_EFI_LOADER_CODE = 1,
    WELOVEC_EFI_LOADER_DATA = 2,
    WELOVEC_EFI_BOOT_SERVICES_CODE = 3,
    WELOVEC_EFI_BOOT_SERVICES_DATA = 4,
    WELOVEC_EFI_RUNTIME_SERVICES_CODE = 5,
    WELOVEC_EFI_RUNTIME_SERVICES_DATA = 6,
    WELOVEC_EFI_CONVENTIONAL_MEMORY = 7,
    WELOVEC_EFI_UNUSABLE_MEMORY = 8,
    WELOVEC_EFI_ACPI_RECLAIM_MEMORY = 9,
    WELOVEC_EFI_ACPI_MEMORY_NVS = 10,
    WELOVEC_EFI_MEMORY_MAPPED_IO = 11,
    WELOVEC_EFI_MEMORY_MAPPED_IO_PORT = 12,
    WELOVEC_EFI_PAL_CODE = 13,
    WELOVEC_EFI_PERSISTENT_MEMORY = 14,
    WELOVEC_EFI_UNACCEPTED_MEMORY = 15
};

// Prefix layout shared with EFI_MEMORY_DESCRIPTOR; later UEFI versions may append fields.
typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} WelovecMemoryDescriptor;

typedef struct {
    uint64_t base;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    uint32_t pixel_format;
    uint32_t pixel_masks[4];
} WelovecFramebuffer;

typedef struct {
    uint64_t base;
    uint64_t size;
    uint32_t type;
    uint32_t flags;
} WelovecPciBarResource;

typedef struct {
    uint16_t segment;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t valid;
    uint8_t bar_count;
    uint8_t reserved;
    WelovecPciBarResource bars[6];
} WelovecDisplayPciInfo;

typedef struct {
    uint64_t physical_address;
    uint64_t virtual_address;
    uint64_t page_count;
    uint32_t flags;
    uint32_t reserved;
} WelovecKernelSegment;

typedef struct {
    uint64_t address;
    uint64_t size;
    uint32_t kind;
    uint32_t reserved;
} WelovecModule;

typedef struct {
    uint32_t media_id;
    uint32_t block_size;
    uint64_t last_block;
    uint32_t flags;
    uint32_t reserved;
} WelovecStorageDevice;

typedef struct WelovecBootInfo {
    uint64_t magic;
    uint32_t version;
    uint32_t size;
    uint64_t flags;

    uint64_t kernel_entry_virtual;
    uint64_t kernel_physical_min;
    uint64_t kernel_physical_max;
    uint64_t page_table_root;
    uint32_t page_table_levels;
    uint32_t acpi_rsdp_source;

    uint64_t memory_map_address;
    uint64_t memory_map_size;
    uint64_t memory_map_descriptor_size;
    uint32_t memory_map_descriptor_version;
    uint32_t memory_map_descriptor_count;

    WelovecFramebuffer framebuffer;
    WelovecDisplayPciInfo display_pci;
    uint64_t acpi_rsdp;
    uint64_t smbios_entry;

    uint64_t command_line_address;
    uint32_t command_line_size;
    uint32_t module_count;
    WelovecModule modules[WELOVEC_BOOTINFO_MAX_MODULES];

    uint32_t kernel_segment_count;
    uint32_t storage_device_count;
    WelovecKernelSegment kernel_segments[WELOVEC_BOOTINFO_MAX_SEGMENTS];
    WelovecStorageDevice storage_devices[WELOVEC_BOOTINFO_MAX_STORAGE];

    uint8_t entropy[32];
    uint8_t reserved[32];
} WelovecBootInfo;

#if defined(__cplusplus)
static_assert(sizeof(WelovecMemoryDescriptor) == 40, "UEFI descriptor prefix ABI changed");
static_assert(sizeof(WelovecBootInfo) <= 4096, "BootInfo must fit in its loader allocation");
#else
_Static_assert(sizeof(WelovecMemoryDescriptor) == 40, "UEFI descriptor prefix ABI changed");
_Static_assert(sizeof(WelovecBootInfo) <= 4096, "BootInfo must fit in its loader allocation");
#endif

#endif
