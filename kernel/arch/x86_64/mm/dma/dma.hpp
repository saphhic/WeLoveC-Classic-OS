// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for dma in the dma module.

#ifndef WELOVEC_X86_64_DMA_HPP
#define WELOVEC_X86_64_DMA_HPP

#include <stdint.h>
#include "arch/x86_64/mm/physical/physical_address.hpp"

namespace welovec::x86_64::mm::dma {

constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kLegacyDmaLimit = UINT64_C(0x100000000);

struct Region {
    PhysicalAddress physical;
    uint8_t *virtual_address;
    uint64_t bytes;
    uint64_t pages;
};

bool allocate(uint64_t bytes, Region *out,
              uint64_t alignment_pages = 1,
              uint64_t max_address_exclusive = kLegacyDmaLimit);
void release(Region *region);

}

#endif
