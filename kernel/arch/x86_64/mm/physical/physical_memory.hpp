// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for physical memory in the physical module.

#ifndef WELOVEC_X86_64_PHYSICAL_MEMORY_HPP
#define WELOVEC_X86_64_PHYSICAL_MEMORY_HPP

#include <stdint.h>
#include "physical_address.hpp"

namespace welovec::x86_64::mm::physical {

constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kPageMask = ~(kPageSize - 1);
constexpr uint64_t kDirectMapBase = UINT64_C(0xffff800000000000);
constexpr uint64_t kDirectMapSize = UINT64_C(512) * 1024 * 1024 * 1024;

inline bool contains(PhysicalAddress address, uint64_t length = 1) {
    return address.valid() && address.value() < kDirectMapSize &&
           length <= kDirectMapSize - address.value();
}

inline void *direct_map(PhysicalAddress address) {
    if (!contains(address)) return nullptr;
    return reinterpret_cast<void *>(static_cast<uintptr_t>(kDirectMapBase + address.value()));
}

inline const void *direct_map_const(PhysicalAddress address) {
    return direct_map(address);
}

inline bool physical_from_direct_map(const void *pointer, PhysicalAddress *out) {
    const uint64_t address = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer));
    if (!out || address < kDirectMapBase || address - kDirectMapBase >= kDirectMapSize) return false;
    *out = PhysicalAddress(address - kDirectMapBase);
    return true;
}

}

#endif
