// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for page table in the paging module.

#ifndef WELOVEC_X86_64_PAGE_TABLE_HPP
#define WELOVEC_X86_64_PAGE_TABLE_HPP

#include <stdint.h>
#include "../physical/physical_address.hpp"
#include "boot/boot_protocol.h"

namespace welovec::x86_64::mm::paging {

enum class PageFlags : uint64_t {
    None = 0,
    Writable = 1u << 0,
    User = 1u << 1,
    WriteThrough = 1u << 2,
    CacheDisable = 1u << 3,
    Global = 1u << 4,
    NoExecute = 1u << 5
};

constexpr PageFlags operator|(PageFlags a, PageFlags b) {
    return static_cast<PageFlags>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}
constexpr PageFlags operator&(PageFlags a, PageFlags b) {
    return static_cast<PageFlags>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
}
constexpr bool has_flag(PageFlags value, PageFlags flag) {
    return (static_cast<uint64_t>(value) & static_cast<uint64_t>(flag)) != 0;
}

struct Mapping {
    PhysicalAddress physical;
    uint64_t raw_flags;
    uint64_t page_size;
    bool owned_frame;
};

bool initialize(const WelovecBootInfo *boot_info);
bool ready();
PhysicalAddress active_root();
uint32_t active_levels();
bool nx_enabled();

bool map_page(uint64_t virtual_address, PhysicalAddress physical_address, PageFlags flags, bool owned_frame = false);
bool map_page_in(PhysicalAddress root, uint64_t virtual_address, PhysicalAddress physical_address,
                 PageFlags flags, bool owned_frame = false);
bool unmap_page(uint64_t virtual_address, Mapping *old_mapping = nullptr, bool release_owned_frame = false);
bool unmap_page_in(PhysicalAddress root, uint64_t virtual_address, Mapping *old_mapping = nullptr,
                   bool release_owned_frame = false);
bool protect_page(uint64_t virtual_address, PageFlags flags);
bool protect_page_in(PhysicalAddress root, uint64_t virtual_address, PageFlags flags);
bool translate(uint64_t virtual_address, Mapping *out);
bool translate_in(PhysicalAddress root, uint64_t virtual_address, Mapping *out);

bool prepare_kernel_range(uint64_t virtual_address);
bool create_address_space(PhysicalAddress *root_out);
bool destroy_address_space(PhysicalAddress root);
bool activate_address_space(PhysicalAddress root);

}

#endif
