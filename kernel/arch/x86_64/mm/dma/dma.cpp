// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Reserves contiguous memory for device DMA transfers.

#include "dma.hpp"

#include "arch/common/MM/PMM/pmm.hpp"
#include "arch/x86_64/mm/physical/physical_memory.hpp"

namespace welovec::x86_64::mm::dma {

bool allocate(uint64_t bytes, Region *out, uint64_t alignment_pages,
              uint64_t max_address_exclusive) {
    if (!out || !bytes || !alignment_pages ||
        bytes > UINT64_MAX - (kPageSize - 1)) return false;
    const uint64_t pages = (bytes + kPageSize - 1) / kPageSize;
    PhysicalAddress physical;
    if (!welovec::memory::pmm::allocate_contiguous_below(
            max_address_exclusive, pages, alignment_pages, &physical)) return false;
    auto *memory = static_cast<uint8_t *>(
        welovec::x86_64::mm::physical::direct_map(physical));
    if (!memory) {
        welovec::memory::pmm::free_contiguous(physical, pages);
        return false;
    }
    const uint64_t span = pages * kPageSize;
    for (uint64_t i = 0; i < span; ++i) memory[i] = 0;
    *out = {physical, memory, span, pages};
    return true;
}

void release(Region *region) {
    if (!region || !region->physical.valid() || !region->pages) return;
    welovec::memory::pmm::free_contiguous(region->physical, region->pages);
    *region = {};
}

}
