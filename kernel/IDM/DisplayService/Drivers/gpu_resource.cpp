// GPU BAR mapping helpers. Drivers receive bounded, uncacheable MMIO views.

#include "gpu_driver.hpp"

#include "arch/common/MM/VMM/vmm.hpp"
#include "arch/x86_64/mm/physical/physical_address.hpp"

namespace welovec::display::gpu {

namespace {
constexpr uint64_t kPageSize = 4096;

bool page_span(uint64_t base, uint64_t size,
               uint64_t *out_start, uint64_t *out_end) {
    if (!size || base > UINT64_MAX - size) return false;
    const uint64_t end = base + size;
    if (end > UINT64_MAX - (kPageSize - 1)) return false;
    *out_start = base & ~(kPageSize - 1);
    *out_end = (end + kPageSize - 1) & ~(kPageSize - 1);
    return *out_start < *out_end;
}

bool overlaps(uint64_t a_start, uint64_t a_end,
              uint64_t b_start, uint64_t b_end) {
    return a_start < b_end && b_start < a_end;
}
}

WelovecGpuStatus map_mmio_region(Device &device, uint8_t bar_index,
                                 uint64_t offset, uint64_t size,
                                 GpuMmioRegion *out_region) {
    if (!out_region) return WELOVEC_GPU_INVALID_ARGUMENT;
    *out_region = {};
    if (bar_index >= device.pci_bar_count || !size)
        return WELOVEC_GPU_INVALID_ARGUMENT;
    if (!(device.pci_bar_size_valid_mask & (1u << bar_index)))
        return WELOVEC_GPU_UNSUPPORTED;

    const auto &bar = device.pci_bars[bar_index];
    const uint64_t bar_size = device.pci_bar_sizes[bar_index];
    if (!bar.assigned || !bar_size || offset > bar_size ||
        size > bar_size - offset ||
        (bar.type != welovec::x86_64::pci::BarType::Memory32 &&
         bar.type != welovec::x86_64::pci::BarType::Memory64) ||
        bar.address > UINT64_MAX - offset) return WELOVEC_GPU_INVALID_ARGUMENT;

    uint32_t free_slot = kMaximumGpuMmioRegions;
    for (uint32_t index = 0; index < kMaximumGpuMmioRegions; ++index) {
        if (!device.mmio_regions[index].active) {
            if (free_slot == kMaximumGpuMmioRegions) free_slot = index;
            continue;
        }
        const auto &active = device.mmio_regions[index];
        uint64_t active_start = 0, active_end = 0;
        uint64_t requested_start = 0, requested_end = 0;
        if (!page_span(active.physical_address, active.size,
                       &active_start, &active_end) ||
            !page_span(bar.address + offset, size,
                       &requested_start, &requested_end))
            return WELOVEC_GPU_INVALID_ARGUMENT;
        if (overlaps(active_start, active_end, requested_start, requested_end))
            return WELOVEC_GPU_BUSY;
    }
    if (free_slot == kMaximumGpuMmioRegions) return WELOVEC_GPU_NO_RESOURCES;

    uint64_t requested_start = 0, requested_end = 0;
    if (!page_span(bar.address + offset, size,
                   &requested_start, &requested_end))
        return WELOVEC_GPU_INVALID_ARGUMENT;
    if (device.firmware_framebuffer_size) {
        uint64_t framebuffer_start = 0, framebuffer_end = 0;
        if (!page_span(device.firmware_framebuffer_base,
                       device.firmware_framebuffer_size,
                       &framebuffer_start, &framebuffer_end))
            return WELOVEC_GPU_INVALID_ARGUMENT;
        if (overlaps(requested_start, requested_end,
                     framebuffer_start, framebuffer_end))
            return WELOVEC_GPU_BUSY;
    }

    void *mapping = welovec::memory::vmm::map_physical(
        welovec::x86_64::mm::PhysicalAddress(bar.address + offset), size,
        welovec::memory::vmm::CachePolicy::Uncacheable);
    if (!mapping) return WELOVEC_GPU_NO_RESOURCES;

    GpuMmioRegion region{};
    region.mapping = mapping;
    region.base = static_cast<volatile uint8_t *>(mapping);
    region.physical_address = bar.address + offset;
    region.size = size;
    region.bar_index = bar_index;
    region.active = 1;
    device.mmio_regions[free_slot] = region;
    *out_region = region;
    return WELOVEC_GPU_OK;
}

WelovecGpuStatus unmap_mmio_region(Device &device,
                                   GpuMmioRegion *region) {
    if (!region || !region->active || !region->mapping || !region->size)
        return WELOVEC_GPU_INVALID_ARGUMENT;
    for (uint32_t index = 0; index < kMaximumGpuMmioRegions; ++index) {
        auto &owned = device.mmio_regions[index];
        if (!owned.active || owned.mapping != region->mapping ||
            owned.physical_address != region->physical_address ||
            owned.size != region->size || owned.bar_index != region->bar_index)
            continue;
        if (!welovec::memory::vmm::unmap_physical(owned.mapping, owned.size))
            return WELOVEC_GPU_HARDWARE_ERROR;
        owned = {};
        *region = {};
        return WELOVEC_GPU_OK;
    }
    return WELOVEC_GPU_INVALID_ARGUMENT;
}

bool release_mmio_regions(Device &device) {
    bool released_all = true;
    for (uint32_t index = 0; index < kMaximumGpuMmioRegions; ++index) {
        GpuMmioRegion &region = device.mmio_regions[index];
        if (!region.active) continue;
        if (!region.mapping || !region.size ||
            !welovec::memory::vmm::unmap_physical(region.mapping, region.size)) {
            released_all = false;
            continue;
        }
        region = {};
    }
    return released_all;
}

WelovecGpuStatus allocate_dma_region(Device &device, uint64_t size_bytes,
                                     uint64_t alignment_pages,
                                     GpuDmaRegion *out_region) {
    if (!out_region || !size_bytes || !alignment_pages)
        return WELOVEC_GPU_INVALID_ARGUMENT;
    *out_region = {};

    uint32_t free_slot = kMaximumGpuDmaRegions;
    for (uint32_t index = 0; index < kMaximumGpuDmaRegions; ++index) {
        if (!device.dma_regions[index].active) {
            free_slot = index;
            break;
        }
    }
    if (free_slot == kMaximumGpuDmaRegions)
        return WELOVEC_GPU_NO_RESOURCES;

    GpuDmaRegion region{};
    if (!welovec::x86_64::mm::dma::allocate(
            size_bytes, &region.allocation, alignment_pages,
            welovec::x86_64::mm::dma::kLegacyDmaLimit))
        return WELOVEC_GPU_NO_RESOURCES;
    region.requested_size = size_bytes;
    region.active = 1;
    device.dma_regions[free_slot] = region;
    *out_region = region;
    return WELOVEC_GPU_OK;
}

WelovecGpuStatus release_dma_region(Device &device, GpuDmaRegion *region) {
    if (!region || !region->active || !region->allocation.physical.valid() ||
        !region->allocation.virtual_address || !region->allocation.pages ||
        !region->requested_size)
        return WELOVEC_GPU_INVALID_ARGUMENT;
    for (uint32_t index = 0; index < kMaximumGpuDmaRegions; ++index) {
        GpuDmaRegion &owned = device.dma_regions[index];
        if (!owned.active ||
            owned.allocation.physical != region->allocation.physical ||
            owned.allocation.virtual_address !=
                region->allocation.virtual_address ||
            owned.allocation.pages != region->allocation.pages ||
            owned.requested_size != region->requested_size)
            continue;
        welovec::x86_64::mm::dma::release(&owned.allocation);
        owned = {};
        *region = {};
        return WELOVEC_GPU_OK;
    }
    return WELOVEC_GPU_INVALID_ARGUMENT;
}

void release_dma_regions(Device &device) {
    for (uint32_t index = 0; index < kMaximumGpuDmaRegions; ++index) {
        GpuDmaRegion &region = device.dma_regions[index];
        if (!region.active) continue;
        welovec::x86_64::mm::dma::release(&region.allocation);
        region = {};
    }
}

bool mmio_read32(const GpuMmioRegion &region, uint64_t offset,
                 uint32_t *out_value) {
    if (!out_value || !region.active || !region.base || (offset & 3u) ||
        offset > region.size || sizeof(uint32_t) > region.size - offset)
        return false;
    __asm__ volatile("mfence" ::: "memory");
    *out_value = *reinterpret_cast<volatile const uint32_t *>(region.base + offset);
    __asm__ volatile("mfence" ::: "memory");
    return true;
}

bool mmio_write32(const GpuMmioRegion &region, uint64_t offset,
                  uint32_t value) {
    if (!region.active || !region.base || (offset & 3u) ||
        offset > region.size || sizeof(uint32_t) > region.size - offset)
        return false;
    __asm__ volatile("mfence" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(region.base + offset) = value;
    __asm__ volatile("mfence" ::: "memory");
    return true;
}

}
