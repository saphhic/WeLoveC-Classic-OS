// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Creates and destroys process address spaces.

#include "address_space.hpp"
#include "../PMM/pmm.hpp"

namespace welovec::memory::vmm {

using welovec::x86_64::mm::PhysicalAddress;
using welovec::x86_64::mm::paging::PageFlags;

bool AddressSpace::create() {
    if (valid_) return false;
    if (!welovec::x86_64::mm::paging::create_address_space(&root_)) return false;
    valid_ = true;
    return true;
}

bool AddressSpace::valid_user_page(uint64_t address) const {
    if (!valid_ || (address & 0xfff)) return false;
    const uint64_t user_limit = welovec::x86_64::mm::paging::active_levels() == 5
        ? UINT64_C(0x0100000000000000) : UINT64_C(0x0000800000000000);
    return address < user_limit;
}

bool AddressSpace::map(uint64_t virtual_address, PhysicalAddress physical, PageFlags flags, bool take_ownership) {
    if (!valid_user_page(virtual_address) || !physical.page_aligned()) return false;
    flags = flags | PageFlags::User;
    if (!welovec::x86_64::mm::paging::nx_enabled())
        flags = static_cast<PageFlags>(static_cast<uint64_t>(flags) & ~static_cast<uint64_t>(PageFlags::NoExecute));
    return welovec::x86_64::mm::paging::map_page_in(root_, virtual_address, physical, flags, take_ownership);
}

bool AddressSpace::allocate_anonymous(uint64_t virtual_address, uint64_t page_count, PageFlags flags) {
    if (!page_count || page_count > UINT64_MAX / 4096 || !valid_user_page(virtual_address)) return false;
    const uint64_t user_limit = welovec::x86_64::mm::paging::active_levels() == 5
        ? UINT64_C(0x0100000000000000) : UINT64_C(0x0000800000000000);
    if (page_count > (user_limit - virtual_address) / 4096) return false;
    uint64_t mapped = 0;
    for (; mapped < page_count; ++mapped) {
        PhysicalAddress frame;
        if (!welovec::memory::pmm::allocate_frame(&frame)) break;
        if (!map(virtual_address + mapped * 4096, frame, flags, true)) {
            welovec::memory::pmm::free_frame(frame);
            break;
        }
    }
    if (mapped == page_count) return true;
    while (mapped) {
        --mapped;
        welovec::x86_64::mm::paging::unmap_page_in(root_, virtual_address + mapped * 4096, nullptr, true);
    }
    return false;
}

bool AddressSpace::unmap(uint64_t virtual_address, bool release_owned_frame) {
    if (!valid_user_page(virtual_address)) return false;
    return welovec::x86_64::mm::paging::unmap_page_in(root_, virtual_address, nullptr, release_owned_frame);
}

bool AddressSpace::activate() {
    return valid_ && welovec::x86_64::mm::paging::activate_address_space(root_);
}

bool AddressSpace::destroy() {
    if (!valid_ || !welovec::x86_64::mm::paging::destroy_address_space(root_)) return false;
    root_ = PhysicalAddress();
    valid_ = false;
    return true;
}

}
