// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for physical address in the physical module.

#ifndef WELOVEC_X86_64_PHYSICAL_ADDRESS_HPP
#define WELOVEC_X86_64_PHYSICAL_ADDRESS_HPP

#include <stdint.h>

namespace welovec::x86_64::mm {

class PhysicalAddress {
public:
    static constexpr uint64_t kInvalidValue = UINT64_MAX;

    constexpr PhysicalAddress() : value_(kInvalidValue) {}
    explicit constexpr PhysicalAddress(uint64_t value) : value_(value) {}

    constexpr uint64_t value() const { return value_; }
    constexpr bool valid() const { return value_ != kInvalidValue; }
    constexpr bool page_aligned() const { return valid() && (value_ & 0xfff) == 0; }

    constexpr PhysicalAddress align_down(uint64_t alignment) const {
        return alignment && !(alignment & (alignment - 1)) && valid()
                   ? PhysicalAddress(value_ & ~(alignment - 1))
                   : PhysicalAddress();
    }

    constexpr bool add(uint64_t amount, PhysicalAddress *out) const {
        if (!out || !valid() || value_ > UINT64_MAX - amount) return false;
        *out = PhysicalAddress(value_ + amount);
        return true;
    }

    friend constexpr bool operator==(PhysicalAddress a, PhysicalAddress b) { return a.value_ == b.value_; }
    friend constexpr bool operator!=(PhysicalAddress a, PhysicalAddress b) { return !(a == b); }
    friend constexpr bool operator<(PhysicalAddress a, PhysicalAddress b) { return a.value_ < b.value_; }

private:
    uint64_t value_;
};

}

#endif
