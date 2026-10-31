// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the allocator that tracks the state of physical frames.

#ifndef WELOVEC_PMM_FRAME_ALLOCATOR_HPP
#define WELOVEC_PMM_FRAME_ALLOCATOR_HPP

#include <stdint.h>
#include <stddef.h>
#include "arch/x86_64/mm/physical/physical_address.hpp"
#include "arch/x86_64/mm/spinlock.hpp"
#include "boot/boot_protocol.h"

namespace welovec::memory::pmm {

struct Statistics {
    uint64_t maximum_physical_address;
    uint64_t managed_frames;
    uint64_t free_frames;
    uint64_t allocated_frames;
    uint64_t boot_available_frames;
    welovec::x86_64::mm::PhysicalAddress metadata_base;
    uint64_t metadata_size;
};

class FrameAllocator {
public:
    constexpr FrameAllocator()
        : free_bitmap_(nullptr), allocated_bitmap_(nullptr), frame_count_(0), free_count_(0),
          allocated_count_(0), scan_hint_(256), stats_{}, initialized_(false), lock_{} {}
    bool initialize(const WelovecBootInfo *boot_info);
    bool initialized() const { return initialized_; }

    bool allocate_frame(welovec::x86_64::mm::PhysicalAddress *out);
    bool allocate_contiguous(uint64_t page_count, uint64_t alignment_pages,
                             welovec::x86_64::mm::PhysicalAddress *out);
    bool allocate_contiguous_below(uint64_t max_address_exclusive, uint64_t page_count,
                                   uint64_t alignment_pages, welovec::x86_64::mm::PhysicalAddress *out);
    bool free_frame(welovec::x86_64::mm::PhysicalAddress address);
    bool free_contiguous(welovec::x86_64::mm::PhysicalAddress base, uint64_t page_count);
    Statistics statistics() const;

private:
    static constexpr uint64_t kPageSize = 4096;

    bool validate_map(const WelovecBootInfo *boot_info, uint64_t *map_address,
                      uint64_t *descriptor_count, uint64_t *descriptor_size,
                      uint64_t *max_address) const;
    bool eligible(uint32_t type) const;
    uint64_t find_run(uint64_t begin, uint64_t end, uint64_t count, uint64_t alignment) const;
    void reserve_range(uint64_t address, uint64_t size);
    void set_range(uint8_t *bitmap, uint64_t begin, uint64_t count, bool set);
    bool bit(const uint8_t *bitmap, uint64_t index) const;
    void set_bit(uint8_t *bitmap, uint64_t index, bool value);

    uint8_t *free_bitmap_;
    uint8_t *allocated_bitmap_;
    uint64_t frame_count_;
    uint64_t free_count_;
    uint64_t allocated_count_;
    uint64_t scan_hint_;
    Statistics stats_;
    bool initialized_;
    mutable welovec::x86_64::mm::IrqSpinLock lock_;
};

FrameAllocator &allocator();

}

#endif
