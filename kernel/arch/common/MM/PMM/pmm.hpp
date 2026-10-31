// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the kernel physical memory allocator interface.

#ifndef WELOVEC_PMM_HPP
#define WELOVEC_PMM_HPP

#include "frame_allocator.hpp"

namespace welovec::memory::pmm {

bool initialize(const WelovecBootInfo *boot_info);
bool allocate_frame(welovec::x86_64::mm::PhysicalAddress *out);
bool allocate_contiguous(uint64_t page_count, uint64_t alignment_pages,
                         welovec::x86_64::mm::PhysicalAddress *out);
bool allocate_contiguous_below(uint64_t max_address_exclusive, uint64_t page_count,
                               uint64_t alignment_pages, welovec::x86_64::mm::PhysicalAddress *out);
bool free_frame(welovec::x86_64::mm::PhysicalAddress address);
bool free_contiguous(welovec::x86_64::mm::PhysicalAddress base, uint64_t page_count);
Statistics statistics();

}

#endif
