// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Initializes the physical frame allocator and exposes its operations to the kernel.

#include "pmm.hpp"

namespace welovec::memory::pmm {

bool initialize(const WelovecBootInfo *boot_info) { return allocator().initialize(boot_info); }
bool allocate_frame(welovec::x86_64::mm::PhysicalAddress *out) { return allocator().allocate_frame(out); }
bool allocate_contiguous(uint64_t pages, uint64_t alignment, welovec::x86_64::mm::PhysicalAddress *out) {
    return allocator().allocate_contiguous(pages, alignment, out);
}
bool allocate_contiguous_below(uint64_t max_address, uint64_t pages, uint64_t alignment,
                               welovec::x86_64::mm::PhysicalAddress *out) {
    return allocator().allocate_contiguous_below(max_address, pages, alignment, out);
}
bool free_frame(welovec::x86_64::mm::PhysicalAddress address) { return allocator().free_frame(address); }
bool free_contiguous(welovec::x86_64::mm::PhysicalAddress base, uint64_t pages) {
    return allocator().free_contiguous(base, pages);
}
Statistics statistics() { return allocator().statistics(); }

}
