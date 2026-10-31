// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Manages the bitmaps used to allocate and free physical frames.

#include "frame_allocator.hpp"
#include "arch/x86_64/mm/physical/physical_memory.hpp"

namespace welovec::memory::pmm {

using welovec::x86_64::mm::PhysicalAddress;
using welovec::x86_64::mm::physical::kDirectMapSize;
using welovec::x86_64::mm::physical::direct_map;

namespace {
constexpr uint64_t kLowMemoryReserved = 1024 * 1024;
constexpr uint64_t kMaxDescriptorSize = 4096;
constinit FrameAllocator gFrameAllocator{};

void zero_bytes(void *memory, uint64_t bytes) {
    uint8_t *out = static_cast<uint8_t *>(memory);
    for (uint64_t i = 0; i < bytes; ++i) out[i] = 0;
}

uint64_t align_up(uint64_t value, uint64_t alignment, bool *ok) {
    if (!alignment || value > UINT64_MAX - (alignment - 1)) { *ok = false; return 0; }
    *ok = true;
    return ((value + alignment - 1) / alignment) * alignment;
}
}

bool FrameAllocator::eligible(uint32_t type) const {
    return type == WELOVEC_EFI_CONVENTIONAL_MEMORY ||
           type == WELOVEC_EFI_BOOT_SERVICES_CODE ||
           type == WELOVEC_EFI_BOOT_SERVICES_DATA;
}

bool FrameAllocator::validate_map(const WelovecBootInfo *info, uint64_t *map_address,
                                  uint64_t *descriptor_count, uint64_t *descriptor_size,
                                  uint64_t *max_address) const {
    if (!info || info->magic != WELOVEC_BOOTINFO_MAGIC ||
        info->version != WELOVEC_BOOTINFO_VERSION || info->size < sizeof(WelovecBootInfo) ||
        !(info->flags & WELOVEC_BOOT_FLAG_EXIT_BOOT_SERVICES) ||
        !(info->flags & WELOVEC_BOOT_FLAG_PAGING_ENABLED)) return false;
    const uint64_t size = info->memory_map_size;
    const uint64_t stride = info->memory_map_descriptor_size;
    if (!size || !stride || stride < sizeof(WelovecMemoryDescriptor) ||
        stride > kMaxDescriptorSize || size % stride ||
        size / stride != info->memory_map_descriptor_count ||
        !welovec::x86_64::mm::physical::contains(PhysicalAddress(info->memory_map_address), size)) return false;

    const auto *map = static_cast<const uint8_t *>(direct_map(PhysicalAddress(info->memory_map_address)));
    if (!map) return false;
    uint64_t highest = 0;
    const uint64_t count = size / stride;
    for (uint64_t i = 0; i < count; ++i) {
        const auto *descriptor = reinterpret_cast<const WelovecMemoryDescriptor *>(map + i * stride);
        if (!eligible(descriptor->type) || !descriptor->number_of_pages ||
            descriptor->physical_start >= kDirectMapSize ||
            descriptor->number_of_pages > UINT64_MAX / kPageSize) continue;
        const uint64_t bytes = descriptor->number_of_pages * kPageSize;
        const uint64_t available = kDirectMapSize - descriptor->physical_start;
        const uint64_t end = descriptor->physical_start + (bytes < available ? bytes : available);
        if (end > highest) highest = end;
    }
    if (highest <= kLowMemoryReserved) return false;
    *map_address = info->memory_map_address;
    *descriptor_count = count;
    *descriptor_size = stride;
    *max_address = highest & ~(kPageSize - 1);
    return true;
}

bool FrameAllocator::bit(const uint8_t *bitmap, uint64_t index) const {
    return (bitmap[index >> 3] & static_cast<uint8_t>(1u << (index & 7))) != 0;
}

void FrameAllocator::set_bit(uint8_t *bitmap, uint64_t index, bool value) {
    const uint8_t mask = static_cast<uint8_t>(1u << (index & 7));
    if (value) bitmap[index >> 3] |= mask;
    else bitmap[index >> 3] &= static_cast<uint8_t>(~mask);
}

void FrameAllocator::set_range(uint8_t *bitmap, uint64_t begin, uint64_t count, bool value) {
    uint64_t end = begin + count;
    while (begin < end && (begin & 7)) set_bit(bitmap, begin++, value);
    const uint8_t fill = value ? 0xff : 0;
    while (begin + 8 <= end) { bitmap[begin >> 3] = fill; begin += 8; }
    while (begin < end) set_bit(bitmap, begin++, value);
}

void FrameAllocator::reserve_range(uint64_t address, uint64_t size) {
    if (!size) return;
    const uint64_t limit = frame_count_ * kPageSize;
    if (address >= limit) return;
    const uint64_t bounded_size = size > limit - address ? limit - address : size;
    const uint64_t end = address + bounded_size;
    uint64_t first_frame = address / kPageSize;
    const uint64_t end_frame = end / kPageSize + ((end & (kPageSize - 1)) != 0);
    while (first_frame < end_frame && (first_frame & 7)) {
        const uint64_t frame = first_frame++;
        if (bit(free_bitmap_, frame)) {
            set_bit(free_bitmap_, frame, false);
            --free_count_;
        }
    }
    while (first_frame + 8 <= end_frame) {
        uint8_t &byte = free_bitmap_[first_frame >> 3];
        free_count_ -= static_cast<uint64_t>(__builtin_popcount(static_cast<unsigned>(byte)));
        byte = 0;
        first_frame += 8;
    }
    while (first_frame < end_frame) {
        const uint64_t frame = first_frame++;
        if (bit(free_bitmap_, frame)) {
            set_bit(free_bitmap_, frame, false);
            --free_count_;
        }
    }
}

uint64_t FrameAllocator::find_run(uint64_t begin, uint64_t end, uint64_t count, uint64_t alignment) const {
    if (!count || !alignment || begin >= end || count > end - begin) return UINT64_MAX;
    bool ok = false;
    uint64_t candidate = align_up(begin, alignment, &ok);
    if (!ok) return UINT64_MAX;
    while (candidate < end && count <= end - candidate) {
        uint64_t index = candidate;
        while (index < candidate + count && bit(free_bitmap_, index)) ++index;
        if (index == candidate + count) return candidate;
        candidate = align_up(index + 1, alignment, &ok);
        if (!ok) return UINT64_MAX;
    }
    return UINT64_MAX;
}

bool FrameAllocator::initialize(const WelovecBootInfo *info) {
    if (initialized_) return true;
    uint64_t map_address = 0, descriptor_count = 0, descriptor_size = 0, max_address = 0;
    if (!validate_map(info, &map_address, &descriptor_count, &descriptor_size, &max_address)) return false;

    frame_count_ = max_address / kPageSize;
    if (!frame_count_ || frame_count_ > UINT64_MAX - 7) return false;
    const uint64_t bitmap_bytes = (frame_count_ + 7) / 8;
    if (bitmap_bytes > UINT64_MAX / 2) return false;
    const uint64_t metadata_bytes = bitmap_bytes * 2;
    const uint64_t metadata_pages = (metadata_bytes + kPageSize - 1) / kPageSize;
    const uint64_t metadata_size = metadata_pages * kPageSize;

    const auto *map = static_cast<const uint8_t *>(direct_map(PhysicalAddress(map_address)));
    if (!map) return false;
    uint64_t metadata_base = UINT64_MAX;
    for (uint64_t i = 0; i < descriptor_count; ++i) {
        const auto *d = reinterpret_cast<const WelovecMemoryDescriptor *>(map + i * descriptor_size);
        if (d->type != WELOVEC_EFI_CONVENTIONAL_MEMORY || !d->number_of_pages ||
            d->physical_start >= max_address || d->number_of_pages > UINT64_MAX / kPageSize) continue;
        uint64_t bytes = d->number_of_pages * kPageSize;
        uint64_t available = max_address - d->physical_start;
        uint64_t end = d->physical_start + (bytes < available ? bytes : available);
        uint64_t begin = d->physical_start < kLowMemoryReserved ? kLowMemoryReserved : d->physical_start;
        bool ok = false;
        begin = align_up(begin, kPageSize, &ok);
        if (ok && begin < end && metadata_size <= end - begin) { metadata_base = begin; break; }
    }
    if (metadata_base == UINT64_MAX) return false;

    auto *metadata = static_cast<uint8_t *>(direct_map(PhysicalAddress(metadata_base)));
    if (!metadata) return false;
    free_bitmap_ = metadata;
    allocated_bitmap_ = metadata + bitmap_bytes;
    zero_bytes(free_bitmap_, bitmap_bytes);
    zero_bytes(allocated_bitmap_, bitmap_bytes);

    free_count_ = 0;
    for (uint64_t i = 0; i < descriptor_count; ++i) {
        const auto *d = reinterpret_cast<const WelovecMemoryDescriptor *>(map + i * descriptor_size);
        if (!eligible(d->type) || !d->number_of_pages || d->physical_start >= max_address ||
            d->number_of_pages > UINT64_MAX / kPageSize) continue;
        uint64_t bytes = d->number_of_pages * kPageSize;
        uint64_t available = max_address - d->physical_start;
        uint64_t end = d->physical_start + (bytes < available ? bytes : available);
        uint64_t begin = d->physical_start < kLowMemoryReserved ? kLowMemoryReserved : d->physical_start;
        begin = (begin + kPageSize - 1) & ~(kPageSize - 1);
        end &= ~(kPageSize - 1);
        if (begin >= end) continue;
        const uint64_t first_frame = begin / kPageSize;
        const uint64_t frames = (end - begin) / kPageSize;
        set_range(free_bitmap_, first_frame, frames, true);
        free_count_ += frames;
    }
    const uint64_t metadata_frame = metadata_base / kPageSize;
    set_range(free_bitmap_, metadata_frame, metadata_pages, false);
    free_count_ -= metadata_pages;

    // Treat the UEFI map as the source of usable RAM, then subtract every
    // allocation the loader handed to the kernel. This also protects us from
    // firmware maps which mistakenly still describe one of these ranges as
    // conventional memory.
    reserve_range(reinterpret_cast<uint64_t>(info), sizeof(*info));
    reserve_range(info->memory_map_address, info->memory_map_size);
    reserve_range(info->page_table_root, kPageSize);
    reserve_range(info->command_line_address, info->command_line_size);
    if (info->acpi_rsdp) reserve_range(info->acpi_rsdp, kPageSize);
    for (uint32_t i = 0; i < info->module_count && i < WELOVEC_BOOTINFO_MAX_MODULES; ++i)
        reserve_range(info->modules[i].address, info->modules[i].size);
    for (uint32_t i = 0; i < info->kernel_segment_count && i < WELOVEC_BOOTINFO_MAX_SEGMENTS; ++i) {
        const auto &segment = info->kernel_segments[i];
        if (segment.page_count <= UINT64_MAX / kPageSize)
            reserve_range(segment.physical_address, segment.page_count * kPageSize);
    }
    reserve_range(info->framebuffer.base, info->framebuffer.size);

    frame_count_ = max_address / kPageSize;
    scan_hint_ = kLowMemoryReserved / kPageSize;
    allocated_count_ = 0;
    stats_.maximum_physical_address = max_address;
    stats_.managed_frames = free_count_ + metadata_pages;
    stats_.free_frames = free_count_;
    stats_.allocated_frames = 0;
    stats_.boot_available_frames = free_count_;
    stats_.metadata_base = PhysicalAddress(metadata_base);
    stats_.metadata_size = metadata_size;
    initialized_ = true;
    return true;
}

bool FrameAllocator::allocate_frame(PhysicalAddress *out) {
    if (!out || !initialized_) return false;
    const uint64_t irq = lock_.lock_irqsave();
    uint64_t frame = find_run(scan_hint_, frame_count_, 1, 1);
    if (frame == UINT64_MAX && scan_hint_ > kLowMemoryReserved / kPageSize)
        frame = find_run(kLowMemoryReserved / kPageSize, scan_hint_, 1, 1);
    if (frame == UINT64_MAX) { lock_.unlock_irqrestore(irq); return false; }
    set_bit(free_bitmap_, frame, false);
    set_bit(allocated_bitmap_, frame, true);
    --free_count_; ++allocated_count_;
    scan_hint_ = frame + 1 < frame_count_ ? frame + 1 : kLowMemoryReserved / kPageSize;
    stats_.free_frames = free_count_;
    stats_.allocated_frames = allocated_count_;
    *out = PhysicalAddress(frame * kPageSize);
    lock_.unlock_irqrestore(irq);
    return true;
}

bool FrameAllocator::allocate_contiguous(uint64_t page_count, uint64_t alignment_pages, PhysicalAddress *out) {
    return allocate_contiguous_below(stats_.maximum_physical_address, page_count, alignment_pages, out);
}

bool FrameAllocator::allocate_contiguous_below(uint64_t max_address_exclusive, uint64_t page_count,
                                               uint64_t alignment_pages, PhysicalAddress *out) {
    if (!out || !initialized_ || !page_count || !alignment_pages) return false;
    uint64_t limit = max_address_exclusive / kPageSize;
    if (limit > frame_count_) limit = frame_count_;
    const uint64_t begin = kLowMemoryReserved / kPageSize;
    if (limit <= begin || page_count > limit - begin) return false;
    const uint64_t irq = lock_.lock_irqsave();
    const uint64_t hint = scan_hint_ < limit ? scan_hint_ : limit;
    uint64_t frame = find_run(hint, limit, page_count, alignment_pages);
    if (frame == UINT64_MAX && hint > begin) frame = find_run(begin, hint, page_count, alignment_pages);
    if (frame == UINT64_MAX) { lock_.unlock_irqrestore(irq); return false; }
    set_range(free_bitmap_, frame, page_count, false);
    set_range(allocated_bitmap_, frame, page_count, true);
    free_count_ -= page_count;
    allocated_count_ += page_count;
    scan_hint_ = frame + page_count < frame_count_ ? frame + page_count : kLowMemoryReserved / kPageSize;
    stats_.free_frames = free_count_;
    stats_.allocated_frames = allocated_count_;
    *out = PhysicalAddress(frame * kPageSize);
    lock_.unlock_irqrestore(irq);
    return true;
}

bool FrameAllocator::free_frame(PhysicalAddress address) {
    if (!initialized_ || !address.page_aligned()) return false;
    const uint64_t frame = address.value() / kPageSize;
    if (frame >= frame_count_) return false;
    const uint64_t irq = lock_.lock_irqsave();
    if (!bit(allocated_bitmap_, frame)) { lock_.unlock_irqrestore(irq); return false; }
    set_bit(allocated_bitmap_, frame, false);
    set_bit(free_bitmap_, frame, true);
    ++free_count_; --allocated_count_;
    if (frame < scan_hint_) scan_hint_ = frame;
    stats_.free_frames = free_count_;
    stats_.allocated_frames = allocated_count_;
    lock_.unlock_irqrestore(irq);
    return true;
}

bool FrameAllocator::free_contiguous(PhysicalAddress base, uint64_t page_count) {
    if (!initialized_ || !base.page_aligned() || !page_count) return false;
    const uint64_t first = base.value() / kPageSize;
    if (first >= frame_count_ || page_count > frame_count_ - first) return false;
    const uint64_t irq = lock_.lock_irqsave();
    for (uint64_t i = 0; i < page_count; ++i) {
        if (!bit(allocated_bitmap_, first + i)) { lock_.unlock_irqrestore(irq); return false; }
    }
    set_range(allocated_bitmap_, first, page_count, false);
    set_range(free_bitmap_, first, page_count, true);
    free_count_ += page_count;
    allocated_count_ -= page_count;
    if (first < scan_hint_) scan_hint_ = first;
    stats_.free_frames = free_count_;
    stats_.allocated_frames = allocated_count_;
    lock_.unlock_irqrestore(irq);
    return true;
}

Statistics FrameAllocator::statistics() const {
    const uint64_t irq = lock_.lock_irqsave();
    Statistics result = stats_;
    result.free_frames = free_count_;
    result.allocated_frames = allocated_count_;
    lock_.unlock_irqrestore(irq);
    return result;
}

FrameAllocator &allocator() {
    return gFrameAllocator;
}

}
