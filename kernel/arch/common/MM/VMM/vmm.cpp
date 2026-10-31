// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the kernel virtual memory mappings.

#include "vmm.hpp"
#include "../PMM/pmm.hpp"
#include "arch/x86_64/mm/physical/physical_memory.hpp"
#include "arch/x86_64/mm/spinlock.hpp"

namespace welovec::memory::vmm {

namespace {
using welovec::x86_64::mm::IrqSpinLock;
using welovec::x86_64::mm::PhysicalAddress;
using welovec::x86_64::mm::paging::Mapping;
using welovec::x86_64::mm::paging::PageFlags;
using welovec::x86_64::mm::physical::kPageSize;

constexpr uint64_t kKernelArenaBase = UINT64_C(0xffff900000000000);
constexpr uint64_t kIoArenaBase = UINT64_C(0xffff904000000000);
constexpr uint64_t kArenaEnd = UINT64_C(0xffff908000000000);
constexpr uint32_t kMaxFreeRanges = 4096;
constexpr uint32_t kMaxMappings = 4096;

class VirtualRangeAllocator {
public:
    constexpr VirtualRangeAllocator() : ranges_{}, count_(0) {}

    bool initialize(uint64_t start_address, uint64_t end_address) {
        if ((start_address & (kPageSize - 1)) || (end_address & (kPageSize - 1)) || start_address >= end_address)
            return false;
        ranges_[0] = Range{start_address / kPageSize, (end_address - start_address) / kPageSize};
        count_ = 1;
        return true;
    }

    bool allocate(uint64_t page_count, uint64_t alignment_pages, uint64_t *address_out) {
        if (!page_count || !alignment_pages || !address_out) return false;
        for (uint32_t i = 0; i < count_; ++i) {
            const Range current = ranges_[i];
            bool ok = false;
            const uint64_t start = align_up(current.start_page, alignment_pages, &ok);
            if (!ok || start < current.start_page || start - current.start_page > current.page_count ||
                page_count > current.page_count - (start - current.start_page)) continue;
            const uint64_t end = start + page_count;
            const uint64_t old_end = current.start_page + current.page_count;
            if (start == current.start_page && end == old_end) remove(i);
            else if (start == current.start_page) {
                ranges_[i].start_page = end;
                ranges_[i].page_count = old_end - end;
            } else if (end == old_end) {
                ranges_[i].page_count = start - current.start_page;
            } else {
                if (count_ == kMaxFreeRanges) return false;
                for (uint32_t j = count_; j > i + 1; --j) ranges_[j] = ranges_[j - 1];
                ranges_[i].page_count = start - current.start_page;
                ranges_[i + 1] = Range{end, old_end - end};
                ++count_;
            }
            *address_out = start * kPageSize;
            return true;
        }
        return false;
    }

    bool release(uint64_t address, uint64_t page_count) {
        if ((address & (kPageSize - 1)) || !page_count || page_count > UINT64_MAX / kPageSize) return false;
        const uint64_t start = address / kPageSize;
        if (start > UINT64_MAX - page_count) return false;
        const uint64_t end = start + page_count;
        uint32_t position = 0;
        while (position < count_ && ranges_[position].start_page < start) ++position;
        if (position && ranges_[position - 1].start_page + ranges_[position - 1].page_count > start) return false;
        if (position < count_ && end > ranges_[position].start_page) return false;

        const bool joins_previous = position &&
            ranges_[position - 1].start_page + ranges_[position - 1].page_count == start;
        const bool joins_next = position < count_ && end == ranges_[position].start_page;
        if (joins_previous && joins_next) {
            ranges_[position - 1].page_count += page_count + ranges_[position].page_count;
            remove(position);
        } else if (joins_previous) ranges_[position - 1].page_count += page_count;
        else if (joins_next) {
            ranges_[position].start_page = start;
            ranges_[position].page_count += page_count;
        } else {
            if (count_ == kMaxFreeRanges) return false;
            for (uint32_t i = count_; i > position; --i) ranges_[i] = ranges_[i - 1];
            ranges_[position] = Range{start, page_count};
            ++count_;
        }
        return true;
    }

private:
    struct Range { uint64_t start_page; uint64_t page_count; };
    static uint64_t align_up(uint64_t value, uint64_t alignment, bool *ok) {
        if (!alignment || value > UINT64_MAX - (alignment - 1)) { *ok = false; return 0; }
        *ok = true;
        return ((value + alignment - 1) / alignment) * alignment;
    }
    void remove(uint32_t index) {
        for (uint32_t i = index + 1; i < count_; ++i) ranges_[i - 1] = ranges_[i];
        --count_;
    }

    Range ranges_[kMaxFreeRanges];
    uint32_t count_;
};

enum class MappingKind : uint8_t { None, OwnedPages, Physical };
struct MappingRecord {
    uint64_t virtual_base;
    uint64_t returned_address;
    uint64_t physical_base;
    uint64_t requested_size;
    uint64_t page_count;
    MappingKind kind;
};

struct Manager {
    constexpr Manager() : kernel_ranges{}, io_ranges{}, mappings{}, lock{}, initialized(false) {}
    VirtualRangeAllocator kernel_ranges;
    VirtualRangeAllocator io_ranges;
    MappingRecord mappings[kMaxMappings];
    IrqSpinLock lock;
    bool initialized;
};

constinit Manager gManager{};

bool get_record_slot(uint32_t *slot) {
    for (uint32_t i = 0; i < kMaxMappings; ++i) {
        if (gManager.mappings[i].kind == MappingKind::None) { *slot = i; return true; }
    }
    return false;
}

void rollback_owned(uint64_t virtual_base, uint64_t mapped_pages, PhysicalAddress pending = PhysicalAddress()) {
    if (pending.valid()) welovec::memory::pmm::free_frame(pending);
    for (uint64_t i = 0; i < mapped_pages; ++i) {
        Mapping discarded{};
        welovec::x86_64::mm::paging::unmap_page(virtual_base + i * kPageSize, &discarded, true);
    }
}

}

bool initialize() {
    if (gManager.initialized) return true;
    if (!welovec::x86_64::mm::paging::ready() || !welovec::memory::pmm::allocator().initialized()) return false;
    if (!gManager.kernel_ranges.initialize(kKernelArenaBase, kIoArenaBase) ||
        !gManager.io_ranges.initialize(kIoArenaBase, kArenaEnd) ||
        !welovec::x86_64::mm::paging::prepare_kernel_range(kKernelArenaBase)) return false;
    gManager.initialized = true;
    return true;
}

bool ready() { return gManager.initialized; }

void *allocate_pages(uint64_t page_count, PageFlags flags) {
    if (!gManager.initialized || !page_count || page_count > UINT64_MAX / kPageSize) return nullptr;
    if (!welovec::x86_64::mm::paging::nx_enabled())
        flags = static_cast<PageFlags>(static_cast<uint64_t>(flags) & ~static_cast<uint64_t>(PageFlags::NoExecute));
    const uint64_t irq = gManager.lock.lock_irqsave();
    uint32_t slot;
    uint64_t virtual_base;
    if (!get_record_slot(&slot) || !gManager.kernel_ranges.allocate(page_count, 1, &virtual_base)) {
        gManager.lock.unlock_irqrestore(irq); return nullptr;
    }
    uint64_t mapped = 0;
    PhysicalAddress pending;
    for (; mapped < page_count; ++mapped) {
        if (!welovec::memory::pmm::allocate_frame(&pending)) break;
        if (!welovec::x86_64::mm::paging::map_page(virtual_base + mapped * kPageSize, pending, flags, true)) break;
        pending = PhysicalAddress();
    }
    if (mapped != page_count) {
        rollback_owned(virtual_base, mapped, pending);
        gManager.kernel_ranges.release(virtual_base, page_count);
        gManager.lock.unlock_irqrestore(irq);
        return nullptr;
    }
    gManager.mappings[slot] = MappingRecord{virtual_base, virtual_base, 0, page_count * kPageSize,
                                            page_count, MappingKind::OwnedPages};
    gManager.lock.unlock_irqrestore(irq);
    return reinterpret_cast<void *>(static_cast<uintptr_t>(virtual_base));
}

bool free_pages(void *base_pointer) {
    if (!gManager.initialized || !base_pointer) return false;
    const uint64_t base = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(base_pointer));
    const uint64_t irq = gManager.lock.lock_irqsave();
    uint32_t slot = kMaxMappings;
    for (uint32_t i = 0; i < kMaxMappings; ++i)
        if (gManager.mappings[i].kind == MappingKind::OwnedPages && gManager.mappings[i].virtual_base == base) { slot = i; break; }
    if (slot == kMaxMappings) { gManager.lock.unlock_irqrestore(irq); return false; }
    const MappingRecord record = gManager.mappings[slot];
    bool ok = true;
    for (uint64_t i = 0; i < record.page_count; ++i) {
        if (!welovec::x86_64::mm::paging::unmap_page(record.virtual_base + i * kPageSize, nullptr, true)) ok = false;
    }
    if (ok && gManager.kernel_ranges.release(record.virtual_base, record.page_count))
        gManager.mappings[slot] = MappingRecord{};
    else ok = false;
    gManager.lock.unlock_irqrestore(irq);
    return ok;
}

void *map_physical(PhysicalAddress physical, uint64_t size, CachePolicy cache) {
    if (!gManager.initialized || !physical.valid() || !size ||
        (cache != CachePolicy::WriteBack && cache != CachePolicy::Uncacheable) ||
        physical.value() > UINT64_MAX - size) return nullptr;
    const uint64_t offset = physical.value() & (kPageSize - 1);
    if (size > UINT64_MAX - offset) return nullptr;
    const uint64_t span = size + offset;
    if (span > UINT64_MAX - (kPageSize - 1)) return nullptr;
    const uint64_t page_count = (span + kPageSize - 1) / kPageSize;
    if (!page_count || page_count > UINT64_MAX / kPageSize) return nullptr;
    const PhysicalAddress physical_base = physical.align_down(kPageSize);
    const uint64_t irq = gManager.lock.lock_irqsave();
    uint32_t slot;
    uint64_t virtual_base;
    if (!get_record_slot(&slot) || !gManager.io_ranges.allocate(page_count, 1, &virtual_base)) {
        gManager.lock.unlock_irqrestore(irq); return nullptr;
    }
    PageFlags flags = PageFlags::Writable | PageFlags::NoExecute;
    if (cache == CachePolicy::Uncacheable) flags = flags | PageFlags::WriteThrough | PageFlags::CacheDisable;
    uint64_t mapped = 0;
    for (; mapped < page_count; ++mapped) {
        PhysicalAddress current;
        if (!physical_base.add(mapped * kPageSize, &current) ||
            !welovec::x86_64::mm::paging::map_page(virtual_base + mapped * kPageSize, current, flags, false)) break;
    }
    if (mapped != page_count) {
        for (uint64_t i = 0; i < mapped; ++i) welovec::x86_64::mm::paging::unmap_page(virtual_base + i * kPageSize);
        gManager.io_ranges.release(virtual_base, page_count);
        gManager.lock.unlock_irqrestore(irq);
        return nullptr;
    }
    const uint64_t returned = virtual_base + offset;
    gManager.mappings[slot] = MappingRecord{virtual_base, returned, physical_base.value(), size,
                                            page_count, MappingKind::Physical};
    gManager.lock.unlock_irqrestore(irq);
    return reinterpret_cast<void *>(static_cast<uintptr_t>(returned));
}

bool unmap_physical(void *mapping, uint64_t size) {
    if (!gManager.initialized || !mapping || !size) return false;
    const uint64_t returned = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mapping));
    const uint64_t irq = gManager.lock.lock_irqsave();
    uint32_t slot = kMaxMappings;
    for (uint32_t i = 0; i < kMaxMappings; ++i)
        if (gManager.mappings[i].kind == MappingKind::Physical && gManager.mappings[i].returned_address == returned &&
            gManager.mappings[i].requested_size == size) { slot = i; break; }
    if (slot == kMaxMappings) { gManager.lock.unlock_irqrestore(irq); return false; }
    const MappingRecord record = gManager.mappings[slot];
    bool ok = true;
    for (uint64_t i = 0; i < record.page_count; ++i)
        if (!welovec::x86_64::mm::paging::unmap_page(record.virtual_base + i * kPageSize)) ok = false;
    if (ok && gManager.io_ranges.release(record.virtual_base, record.page_count))
        gManager.mappings[slot] = MappingRecord{};
    else ok = false;
    gManager.lock.unlock_irqrestore(irq);
    return ok;
}

Statistics statistics() {
    Statistics out{};
    if (!gManager.initialized) return out;
    const uint64_t irq = gManager.lock.lock_irqsave();
    for (uint32_t i = 0; i < kMaxMappings; ++i) {
        const MappingRecord &record = gManager.mappings[i];
        if (record.kind == MappingKind::OwnedPages) out.kernel_virtual_pages += record.page_count;
        else if (record.kind == MappingKind::Physical) out.io_virtual_pages += record.page_count;
        if (record.kind != MappingKind::None) ++out.active_mappings;
    }
    gManager.lock.unlock_irqrestore(irq);
    return out;
}

}
