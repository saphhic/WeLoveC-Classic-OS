// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares virtual memory mapping and reservation operations.

#ifndef WELOVEC_VMM_HPP
#define WELOVEC_VMM_HPP

#include <stdint.h>
#include "arch/x86_64/mm/physical/physical_address.hpp"
#include "arch/x86_64/mm/paging/page_table.hpp"

namespace welovec::memory::vmm {

enum class CachePolicy : uint8_t { WriteBack, Uncacheable };

struct Statistics {
    uint64_t kernel_virtual_pages;
    uint64_t io_virtual_pages;
    uint32_t active_mappings;
};

bool initialize();
bool ready();
void *allocate_pages(uint64_t page_count,
                     welovec::x86_64::mm::paging::PageFlags flags =
                         welovec::x86_64::mm::paging::PageFlags::Writable |
                         welovec::x86_64::mm::paging::PageFlags::NoExecute);
bool free_pages(void *base);

void *map_physical(welovec::x86_64::mm::PhysicalAddress physical, uint64_t size,
                   CachePolicy cache = CachePolicy::Uncacheable);
bool unmap_physical(void *mapping, uint64_t size);
Statistics statistics();

}

#endif
