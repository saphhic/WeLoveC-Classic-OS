// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the process address space interface.

#ifndef WELOVEC_VMM_ADDRESS_SPACE_HPP
#define WELOVEC_VMM_ADDRESS_SPACE_HPP

#include <stdint.h>
#include "arch/x86_64/mm/physical/physical_address.hpp"
#include "arch/x86_64/mm/paging/page_table.hpp"

namespace welovec::memory::vmm {

class AddressSpace {
public:
    constexpr AddressSpace() : root_(), valid_(false) {}
    AddressSpace(const AddressSpace &) = delete;
    AddressSpace &operator=(const AddressSpace &) = delete;

    bool create();
    bool valid() const { return valid_; }
    welovec::x86_64::mm::PhysicalAddress root() const { return root_; }

    bool map(uint64_t user_virtual_address, welovec::x86_64::mm::PhysicalAddress physical,
             welovec::x86_64::mm::paging::PageFlags flags, bool take_ownership = false);
    bool allocate_anonymous(uint64_t user_virtual_address, uint64_t page_count,
                            welovec::x86_64::mm::paging::PageFlags flags);
    bool unmap(uint64_t user_virtual_address, bool release_owned_frame = false);
    bool activate();
    bool destroy();

private:
    bool valid_user_page(uint64_t address) const;

    welovec::x86_64::mm::PhysicalAddress root_;
    bool valid_;
};

}

#endif
