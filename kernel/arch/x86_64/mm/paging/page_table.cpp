// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Manages x86-64 page table entries and mappings.

#include "page_table.hpp"
#include "../physical/physical_memory.hpp"
#include "../spinlock.hpp"
#include "arch/common/MM/PMM/pmm.hpp"

namespace welovec::x86_64::mm::paging {

namespace {
using welovec::memory::pmm::allocate_frame;
using welovec::memory::pmm::free_frame;
using welovec::x86_64::mm::IrqSpinLock;
using welovec::x86_64::mm::physical::direct_map;
using welovec::x86_64::mm::physical::kPageSize;

constexpr uint64_t kPresent = UINT64_C(1) << 0;
constexpr uint64_t kWritable = UINT64_C(1) << 1;
constexpr uint64_t kUser = UINT64_C(1) << 2;
constexpr uint64_t kWriteThrough = UINT64_C(1) << 3;
constexpr uint64_t kCacheDisable = UINT64_C(1) << 4;
constexpr uint64_t kAccessed = UINT64_C(1) << 5;
constexpr uint64_t kDirty = UINT64_C(1) << 6;
constexpr uint64_t kPageSizeOrPat = UINT64_C(1) << 7;
constexpr uint64_t kGlobal = UINT64_C(1) << 8;
constexpr uint64_t kOwnedFrame = UINT64_C(1) << 9;  // software bit: leaf frame belongs to PMM
constexpr uint64_t kOwnedTable = UINT64_C(1) << 10; // software bit: child table belongs to PMM
constexpr uint64_t kPatLarge = UINT64_C(1) << 12;
constexpr uint64_t kNoExecute = UINT64_C(1) << 63;
constexpr uint64_t kAddressMask = UINT64_C(0x000ffffffffff000);
constexpr uint64_t kAddress2M = UINT64_C(0x000fffffffe00000);
constexpr uint64_t kAddress1G = UINT64_C(0x000fffffc0000000);
constexpr uint64_t kSize2M = UINT64_C(1) << 21;
constexpr uint64_t kSize1G = UINT64_C(1) << 30;

IrqSpinLock gPageTableLock;
PhysicalAddress gActiveRoot{};
uint32_t gLevels = 0;
bool gNxEnabled = false;
bool gInitialized = false;

uint64_t read_cr3() {
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

uint64_t read_cr4() {
    uint64_t value;
    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

void write_cr3(uint64_t value) { __asm__ volatile("mov %0, %%cr3" :: "r"(value) : "memory"); }

uint64_t read_efer() {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(UINT32_C(0xc0000080)));
    return (static_cast<uint64_t>(high) << 32) | low;
}

void flush_page(uint64_t virtual_address) {
    __asm__ volatile("invlpg (%0)" :: "r"(virtual_address) : "memory");
}

bool canonical(uint64_t address) {
    if (gLevels == 5) return (address >> 57) == 0 || (address >> 57) == 0x7f;
    return (address >> 48) == 0 || (address >> 48) == 0xffff;
}

uint16_t index_for(uint64_t address, uint32_t level) {
    return static_cast<uint16_t>((address >> (12 + 9 * (level - 1))) & 0x1ff);
}

uint64_t *table_pointer(uint64_t physical) {
    return static_cast<uint64_t *>(direct_map(PhysicalAddress(physical & kAddressMask)));
}

bool allocate_table(uint64_t *physical_out, uint64_t **pointer_out) {
    PhysicalAddress frame;
    if (!allocate_frame(&frame)) return false;
    auto *table = static_cast<uint64_t *>(direct_map(frame));
    if (!table) { free_frame(frame); return false; }
    for (uint64_t i = 0; i < 512; ++i) table[i] = 0;
    *physical_out = frame.value();
    *pointer_out = table;
    return true;
}

bool table_empty(const uint64_t *table) {
    for (uint64_t i = 0; i < 512; ++i) if (table[i] & kPresent) return false;
    return true;
}

uint64_t hardware_flags(PageFlags flags) {
    uint64_t raw = kPresent;
    if (has_flag(flags, PageFlags::Writable)) raw |= kWritable;
    if (has_flag(flags, PageFlags::User)) raw |= kUser;
    if (has_flag(flags, PageFlags::WriteThrough)) raw |= kWriteThrough;
    if (has_flag(flags, PageFlags::CacheDisable)) raw |= kCacheDisable;
    if (has_flag(flags, PageFlags::Global)) raw |= kGlobal;
    if (has_flag(flags, PageFlags::NoExecute)) raw |= kNoExecute;
    return raw;
}

bool split_huge(uint64_t *parent_entry, uint32_t level) {
    if (level != 2 && level != 3) return false;
    const uint64_t old = *parent_entry;
    if (!(old & kPresent) || !(old & kPageSizeOrPat)) return false;
    uint64_t table_physical;
    uint64_t *table;
    if (!allocate_table(&table_physical, &table)) return false;

    if (level == 3) {
        const uint64_t base = old & kAddress1G;
        const uint64_t attributes = old & (kPresent | kWritable | kUser | kWriteThrough | kCacheDisable |
                                             kAccessed | kDirty | kPageSizeOrPat | kGlobal | kPatLarge | kNoExecute);
        for (uint64_t i = 0; i < 512; ++i) table[i] = (base + i * kSize2M) | attributes;
    } else {
        const uint64_t base = old & kAddress2M;
        uint64_t attributes = old & (kPresent | kWritable | kUser | kWriteThrough | kCacheDisable |
                                      kAccessed | kDirty | kGlobal | kNoExecute);
        if (old & kPatLarge) attributes |= kPageSizeOrPat; // PAT moves from bit 12 to bit 7.
        for (uint64_t i = 0; i < 512; ++i) table[i] = (base + i * kPageSize) | attributes;
    }

    const uint64_t parent_flags = kPresent | kWritable | (old & kUser) | kOwnedTable;
    *parent_entry = table_physical | parent_flags;
    return true;
}

bool ensure_child(uint64_t *entry, bool user, uint64_t **child_out) {
    if (!(*entry & kPresent)) {
        uint64_t child_physical;
        uint64_t *child;
        if (!allocate_table(&child_physical, &child)) return false;
        *entry = child_physical | kPresent | kWritable | (user ? kUser : 0) | kOwnedTable;
        *child_out = child;
        return true;
    }
    if (*entry & kPageSizeOrPat) return false;
    if (user) *entry |= kUser;
    *entry |= kWritable;
    *child_out = table_pointer(*entry);
    return *child_out != nullptr;
}

void prune_empty_path(uint64_t root_physical, uint64_t virtual_address) {
    uint64_t *tables[6]{};
    uint16_t indexes[6]{};
    uint64_t *root = table_pointer(root_physical);
    if (!root) return;
    tables[0] = root;
    for (uint32_t depth = 0; depth + 1 < gLevels; ++depth) {
        const uint32_t level = gLevels - depth;
        indexes[depth] = index_for(virtual_address, level);
        const uint64_t entry = tables[depth][indexes[depth]];
        if (!(entry & kPresent) || (entry & kPageSizeOrPat)) break;
        tables[depth + 1] = table_pointer(entry);
        if (!tables[depth + 1]) break;
    }
    for (int depth = static_cast<int>(gLevels) - 2; depth >= 0; --depth) {
        if (!tables[depth] || !tables[depth + 1]) break;
        uint64_t &parent = tables[depth][indexes[depth]];
        if (!(parent & kPresent) || !(parent & kOwnedTable) || !tables[depth + 1] ||
            !table_empty(tables[depth + 1])) break;
        PhysicalAddress child(parent & kAddressMask);
        parent = 0;
        free_frame(child);
    }
}

bool walk_to_leaf(uint64_t root_physical, uint64_t virtual_address, bool split,
                  bool user, bool writable, uint64_t **leaf_out,
                  uint64_t **parent_tables, uint16_t *parent_indexes) {
    uint64_t *table = table_pointer(root_physical);
    if (!table) return false;
    for (uint32_t level = gLevels; level > 1; --level) {
        const uint16_t index = index_for(virtual_address, level);
        uint64_t *entry = &table[index];
        if (parent_tables) parent_tables[gLevels - level] = table;
        if (parent_indexes) parent_indexes[gLevels - level] = index;
        if (!(*entry & kPresent)) {
            uint64_t *child;
            if (!ensure_child(entry, user, &child)) return false;
            table = child;
            continue;
        }
        if ((*entry & kPageSizeOrPat) && (level == 2 || level == 3)) {
            if (!split || !split_huge(entry, level)) return false;
        } else if (*entry & kPageSizeOrPat) return false;
        if (user) *entry |= kUser;
        if (writable) *entry |= kWritable;
        table = table_pointer(*entry);
        if (!table) return false;
    }
    if (parent_tables) parent_tables[gLevels - 1] = table;
    *leaf_out = &table[index_for(virtual_address, 1)];
    return true;
}

bool valid_root(PhysicalAddress root) {
    return root.page_aligned() && (root.value() & ~kAddressMask) == 0 &&
           welovec::x86_64::mm::physical::contains(root, kPageSize) && table_pointer(root.value());
}

void destroy_user_table(uint64_t table_physical, uint32_t level) {
    uint64_t *table = table_pointer(table_physical);
    if (!table) return;
    for (uint64_t i = 0; i < 512; ++i) {
        uint64_t entry = table[i];
        if (!(entry & kPresent)) continue;
        if (level == 1) {
            if (entry & kOwnedFrame) free_frame(PhysicalAddress(entry & kAddressMask));
            table[i] = 0;
            continue;
        }
        if (entry & kPageSizeOrPat) { table[i] = 0; continue; }
        if (entry & kOwnedTable) {
            const uint64_t child = entry & kAddressMask;
            destroy_user_table(child, level - 1);
            table[i] = 0;
            free_frame(PhysicalAddress(child));
        }
    }
}

}

bool initialize(const WelovecBootInfo *info) {
    if (gInitialized) return true;
    if (!info || info->magic != WELOVEC_BOOTINFO_MAGIC || info->version != WELOVEC_BOOTINFO_VERSION ||
        info->size < sizeof(WelovecBootInfo) || !(info->flags & WELOVEC_BOOT_FLAG_PAGING_ENABLED) ||
        (info->page_table_levels != 4 && info->page_table_levels != 5)) return false;
    const uint64_t cr4 = read_cr4();
    const bool la57 = (cr4 & (UINT64_C(1) << 12)) != 0;
    if (la57 != (info->page_table_levels == 5)) return false;
    const uint64_t root = info->page_table_root & kAddressMask;
    if (info->page_table_root != root || !valid_root(PhysicalAddress(root)) ||
        (read_cr3() & kAddressMask) != root) return false;
    gLevels = info->page_table_levels;
    gActiveRoot = PhysicalAddress(root);
    gNxEnabled = (info->flags & WELOVEC_BOOT_FLAG_NX_ENABLED) && (read_efer() & (UINT64_C(1) << 11));
    gInitialized = true;
    return true;
}

bool ready() { return gInitialized; }
PhysicalAddress active_root() { return gActiveRoot; }
uint32_t active_levels() { return gLevels; }
bool nx_enabled() { return gNxEnabled; }

bool map_page(uint64_t virtual_address, PhysicalAddress physical_address, PageFlags flags, bool owned_frame) {
    return map_page_in(gActiveRoot, virtual_address, physical_address, flags, owned_frame);
}

bool map_page_in(PhysicalAddress root, uint64_t virtual_address, PhysicalAddress physical_address,
                 PageFlags flags, bool owned_frame) {
    if (!gInitialized || !valid_root(root) || !canonical(virtual_address) || (virtual_address & (kPageSize - 1)) ||
        !physical_address.page_aligned() ||
        (physical_address.value() & ~kAddressMask) ||
        (has_flag(flags, PageFlags::NoExecute) && !gNxEnabled)) return false;
    const uint64_t irq = gPageTableLock.lock_irqsave();
    uint64_t *leaf = nullptr;
    const bool user = has_flag(flags, PageFlags::User);
    const bool writable = has_flag(flags, PageFlags::Writable);
    if (!walk_to_leaf(root.value(), virtual_address, true, user, writable, &leaf, nullptr, nullptr)) {
        prune_empty_path(root.value(), virtual_address);
        gPageTableLock.unlock_irqrestore(irq);
        return false;
    }
    if (*leaf & kPresent) {
        gPageTableLock.unlock_irqrestore(irq);
        return false;
    }
    *leaf = physical_address.value() | hardware_flags(flags) | (owned_frame ? kOwnedFrame : 0);
    if (root == gActiveRoot) flush_page(virtual_address);
    gPageTableLock.unlock_irqrestore(irq);
    return true;
}

bool unmap_page(uint64_t virtual_address, Mapping *old_mapping, bool release_owned_frame) {
    return unmap_page_in(gActiveRoot, virtual_address, old_mapping, release_owned_frame);
}

bool unmap_page_in(PhysicalAddress root, uint64_t virtual_address, Mapping *old_mapping,
                   bool release_owned_frame) {
    if (!gInitialized || !valid_root(root) || !canonical(virtual_address) || (virtual_address & (kPageSize - 1))) return false;
    const uint64_t irq = gPageTableLock.lock_irqsave();
    uint64_t *leaf = nullptr;
    uint64_t *path_tables[6]{};
    uint16_t path_indexes[6]{};
    if (!walk_to_leaf(root.value(), virtual_address, true, false, false, &leaf, path_tables, path_indexes) ||
        !(*leaf & kPresent)) {
        gPageTableLock.unlock_irqrestore(irq);
        return false;
    }
    const uint64_t old = *leaf;
    const bool owned = (old & kOwnedFrame) != 0;
    const PhysicalAddress physical(old & kAddressMask);
    *leaf = 0;
    if (old_mapping) *old_mapping = Mapping{physical, old & ~kAddressMask, kPageSize, owned};
    if (root == gActiveRoot) flush_page(virtual_address);
    if (release_owned_frame && owned) free_frame(physical);
    for (int depth = static_cast<int>(gLevels) - 2; depth >= 0; --depth) {
        uint64_t &parent = path_tables[depth][path_indexes[depth]];
        uint64_t *child = table_pointer(parent);
        if (!(parent & kPresent) || !(parent & kOwnedTable) || !child || !table_empty(child)) break;
        const PhysicalAddress child_physical(parent & kAddressMask);
        parent = 0;
        free_frame(child_physical);
    }
    gPageTableLock.unlock_irqrestore(irq);
    return true;
}

bool protect_page(uint64_t virtual_address, PageFlags flags) {
    return protect_page_in(gActiveRoot, virtual_address, flags);
}

bool protect_page_in(PhysicalAddress root, uint64_t virtual_address, PageFlags flags) {
    if (!gInitialized || !valid_root(root) || !canonical(virtual_address) ||
        (virtual_address & (kPageSize - 1)) ||
        (has_flag(flags, PageFlags::NoExecute) && !gNxEnabled)) return false;
    const uint64_t irq = gPageTableLock.lock_irqsave();
    uint64_t *leaf = nullptr;
    if (!walk_to_leaf(root.value(), virtual_address, true, has_flag(flags, PageFlags::User),
                      has_flag(flags, PageFlags::Writable), &leaf, nullptr, nullptr) || !(*leaf & kPresent)) {
        gPageTableLock.unlock_irqrestore(irq);
        return false;
    }
    const uint64_t address = *leaf & kAddressMask;
    const uint64_t ownership = *leaf & kOwnedFrame;
    *leaf = address | hardware_flags(flags) | ownership;
    if (root == gActiveRoot) flush_page(virtual_address);
    gPageTableLock.unlock_irqrestore(irq);
    return true;
}

bool translate(uint64_t virtual_address, Mapping *out) {
    return translate_in(gActiveRoot, virtual_address, out);
}

bool translate_in(PhysicalAddress root, uint64_t virtual_address, Mapping *out) {
    if (!gInitialized || !out || !valid_root(root) || !canonical(virtual_address)) return false;
    const uint64_t irq = gPageTableLock.lock_irqsave();
    uint64_t *table = table_pointer(root.value());
    bool result = false;
    for (uint32_t level = gLevels; table && level > 0; --level) {
        const uint64_t entry = table[index_for(virtual_address, level)];
        if (!(entry & kPresent)) break;
        if ((entry & kPageSizeOrPat) && (level == 3 || level == 2)) {
            const uint64_t page_size = level == 3 ? kSize1G : kSize2M;
            const uint64_t mask = level == 3 ? kAddress1G : kAddress2M;
            out->physical = PhysicalAddress((entry & mask) | (virtual_address & (page_size - 1)));
            out->raw_flags = entry & ~mask;
            out->page_size = page_size;
            out->owned_frame = false;
            result = true;
            break;
        }
        if (level == 1) {
            out->physical = PhysicalAddress((entry & kAddressMask) | (virtual_address & (kPageSize - 1)));
            out->raw_flags = entry & ~kAddressMask;
            out->page_size = kPageSize;
            out->owned_frame = (entry & kOwnedFrame) != 0;
            result = true;
            break;
        }
        table = table_pointer(entry);
    }
    gPageTableLock.unlock_irqrestore(irq);
    return result;
}

bool prepare_kernel_range(uint64_t virtual_address) {
    if (!gInitialized || !canonical(virtual_address) || !(virtual_address >> 63)) return false;
    const uint64_t irq = gPageTableLock.lock_irqsave();
    uint64_t *table = table_pointer(gActiveRoot.value());
    if (gLevels == 5) {
        uint64_t *child = nullptr;
        if (!ensure_child(&table[index_for(virtual_address, 5)], false, &child)) {
            gPageTableLock.unlock_irqrestore(irq); return false;
        }
        table = child;
    }
    uint64_t *child = nullptr;
    const bool ok = ensure_child(&table[index_for(virtual_address, 4)], false, &child);
    gPageTableLock.unlock_irqrestore(irq);
    return ok;
}

bool create_address_space(PhysicalAddress *root_out) {
    if (!gInitialized || !root_out) return false;
    const uint64_t irq = gPageTableLock.lock_irqsave();
    PhysicalAddress new_root;
    if (!allocate_frame(&new_root)) { gPageTableLock.unlock_irqrestore(irq); return false; }
    auto *destination = static_cast<uint64_t *>(direct_map(new_root));
    auto *source = table_pointer(gActiveRoot.value());
    if (!destination || !source) {
        free_frame(new_root); gPageTableLock.unlock_irqrestore(irq); return false;
    }
    for (uint64_t i = 0; i < 512; ++i) destination[i] = 0;
    if (gLevels == 4) {
        for (uint64_t i = 256; i < 512; ++i) destination[i] = source[i];
    } else {
        destination[511] = source[511];
    }
    *root_out = new_root;
    gPageTableLock.unlock_irqrestore(irq);
    return true;
}

bool destroy_address_space(PhysicalAddress root) {
    if (!gInitialized || !valid_root(root) || root == gActiveRoot) return false;
    const uint64_t irq = gPageTableLock.lock_irqsave();
    auto *table = static_cast<uint64_t *>(direct_map(root));
    const uint64_t user_limit = 256;
    for (uint64_t i = 0; i < user_limit; ++i) {
        const uint64_t entry = table[i];
        if (!(entry & kPresent) || !(entry & kOwnedTable)) continue;
        const uint64_t child = entry & kAddressMask;
        destroy_user_table(child, gLevels - 1);
        table[i] = 0;
        free_frame(PhysicalAddress(child));
    }
    free_frame(root);
    gPageTableLock.unlock_irqrestore(irq);
    return true;
}

bool activate_address_space(PhysicalAddress root) {
    if (!gInitialized || !valid_root(root)) return false;
    const uint64_t irq = gPageTableLock.lock_irqsave();
    write_cr3(root.value());
    gActiveRoot = root;
    gPageTableLock.unlock_irqrestore(irq);
    return true;
}

}

extern "C" int welovec_paging_initialize(const WelovecBootInfo *boot_info) {
    return welovec::x86_64::mm::paging::initialize(boot_info) ? 1 : 0;
}
