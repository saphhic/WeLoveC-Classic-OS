// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements dynamic memory allocation and release for the kernel.

#include "heap.hpp"
#include "../VMM/vmm.hpp"
#include "arch/x86_64/mm/spinlock.hpp"

namespace welovec::memory::heap {

namespace {
using welovec::x86_64::mm::IrqSpinLock;

constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kSlabMagic = UINT64_C(0x574c534c41425047); // WLSLABPG
constexpr uint64_t kObjectMagic = UINT64_C(0x574c4f424a454354); // WLOBJECT
constexpr uint64_t kLargeMagic = UINT64_C(0x574c4c4152474541); // WLLARGEA
constexpr uint32_t kBitmapBits = 512;
constexpr uint32_t kCacheCount = 13;
constexpr uint32_t kDirectoryCapacity = 8192;
constexpr uint32_t kPartialList = 0;
constexpr uint32_t kFullList = 1;
constexpr uint32_t kEmptyList = 2;
constexpr uint8_t kDirectorySlab = 1;
constexpr uint8_t kDirectoryLarge = 2;

constexpr uint32_t kPayloadSizes[kCacheCount] = {
    16, 32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048
};

struct List;

struct alignas(16) SlabPage {
    uint64_t magic;
    uint32_t cache_index;
    uint16_t capacity;
    uint16_t free_count;
    SlabPage *previous;
    SlabPage *next;
    uint8_t list_index;
    uint8_t reserved[7];
    uint8_t allocation_bitmap[kBitmapBits / 8];
};

struct ObjectHeader {
    uint64_t magic;
    uint32_t requested_size;
    uint16_t cache_index;
    uint16_t reserved;
};

struct alignas(16) LargeAllocation {
    uint64_t magic;
    uint64_t requested_size;
    uint64_t page_count;
    void *returned_pointer;
};

struct DirectoryEntry {
    uint64_t key;
    void *allocation;
    uint8_t kind;
    uint8_t reserved[7];
};

struct List {
    SlabPage *head;
    uint32_t count;
};

struct Cache {
    uint32_t payload_size;
    uint32_t stride;
    List lists[3];
};

struct Manager {
    constexpr Manager() : caches{}, directory{}, lock{}, stats{}, initialized(false) {}
    Cache caches[kCacheCount];
    DirectoryEntry directory[kDirectoryCapacity];
    IrqSpinLock lock;
    Statistics stats;
    bool initialized;
};

static_assert(sizeof(ObjectHeader) == 16);
static_assert(sizeof(SlabPage) % 16 == 0);
static_assert(sizeof(LargeAllocation) % 16 == 0);
static_assert((kDirectoryCapacity & (kDirectoryCapacity - 1)) == 0);

constinit Manager gHeap{};

uint64_t align_up(uint64_t value, uint64_t alignment, bool *ok) {
    if (!alignment || value > UINT64_MAX - (alignment - 1)) {
        *ok = false;
        return 0;
    }
    *ok = true;
    return (value + alignment - 1) & ~(alignment - 1);
}

uint64_t slab_object_area() {
    bool ok = false;
    return align_up(sizeof(SlabPage), 16, &ok);
}

uint64_t object_data_offset() {
    bool ok = false;
    return align_up(sizeof(ObjectHeader), 16, &ok);
}

uint64_t large_data_offset() {
    bool ok = false;
    return align_up(sizeof(LargeAllocation), 16, &ok);
}

void zero_bytes(void *address, uint64_t size) {
    auto *bytes = static_cast<uint8_t *>(address);
    for (uint64_t i = 0; i < size; ++i) bytes[i] = 0;
}

void copy_bytes(void *destination, const void *source, uint64_t size) {
    auto *out = static_cast<uint8_t *>(destination);
    const auto *in = static_cast<const uint8_t *>(source);
    for (uint64_t i = 0; i < size; ++i) out[i] = in[i];
}

int cache_for_size(size_t size) {
    for (uint32_t i = 0; i < kCacheCount; ++i)
        if (size <= kPayloadSizes[i]) return static_cast<int>(i);
    return -1;
}

bool is_allocated(const SlabPage *slab, uint32_t index) {
    return (slab->allocation_bitmap[index >> 3] & static_cast<uint8_t>(1u << (index & 7))) != 0;
}

void set_allocated(SlabPage *slab, uint32_t index, bool allocated) {
    const uint8_t mask = static_cast<uint8_t>(1u << (index & 7));
    if (allocated) slab->allocation_bitmap[index >> 3] |= mask;
    else slab->allocation_bitmap[index >> 3] &= static_cast<uint8_t>(~mask);
}

void list_remove(Cache &cache, SlabPage *slab) {
    List &list = cache.lists[slab->list_index];
    if (slab->previous) slab->previous->next = slab->next;
    else list.head = slab->next;
    if (slab->next) slab->next->previous = slab->previous;
    if (list.count) --list.count;
    slab->previous = nullptr;
    slab->next = nullptr;
}

void list_add(Cache &cache, SlabPage *slab, uint8_t list_index) {
    List &list = cache.lists[list_index];
    slab->list_index = list_index;
    slab->previous = nullptr;
    slab->next = list.head;
    if (list.head) list.head->previous = slab;
    list.head = slab;
    ++list.count;
}

void move_slab(Cache &cache, SlabPage *slab, uint8_t list_index) {
    if (slab->list_index == list_index) return;
    list_remove(cache, slab);
    list_add(cache, slab, list_index);
}

uint32_t directory_hash(uint64_t key) {
    uint64_t value = key >> 4;
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return static_cast<uint32_t>(value) & (kDirectoryCapacity - 1);
}

DirectoryEntry *directory_find(uint64_t key) {
    if (key <= 1) return nullptr;
    uint32_t index = directory_hash(key);
    for (uint32_t probe = 0; probe < kDirectoryCapacity; ++probe) {
        DirectoryEntry &entry = gHeap.directory[index];
        if (entry.key == 0) return nullptr;
        if (entry.key == key) return &entry;
        index = (index + 1) & (kDirectoryCapacity - 1);
    }
    return nullptr;
}

bool directory_insert(uint64_t key, void *allocation, uint8_t kind) {
    if (key <= 1 || !allocation || !kind) return false;
    uint32_t index = directory_hash(key);
    uint32_t first_tombstone = kDirectoryCapacity;
    for (uint32_t probe = 0; probe < kDirectoryCapacity; ++probe) {
        DirectoryEntry &entry = gHeap.directory[index];
        if (entry.key == key)
            return entry.allocation == allocation && entry.kind == kind;
        if (entry.key == 1 && first_tombstone == kDirectoryCapacity) first_tombstone = index;
        if (entry.key == 0) {
            DirectoryEntry &destination = gHeap.directory[
                first_tombstone == kDirectoryCapacity ? index : first_tombstone];
            destination.key = key;
            destination.allocation = allocation;
            destination.kind = kind;
            return true;
        }
        index = (index + 1) & (kDirectoryCapacity - 1);
    }
    if (first_tombstone == kDirectoryCapacity) return false;
    DirectoryEntry &destination = gHeap.directory[first_tombstone];
    destination.key = key;
    destination.allocation = allocation;
    destination.kind = kind;
    return true;
}

void directory_remove(DirectoryEntry *entry) {
    if (!entry) return;
    entry->key = 1; // tombstone; lookups must continue through this slot.
    entry->allocation = nullptr;
    entry->kind = 0;
}

void initialize_slab(SlabPage *slab, uint32_t cache_index) {
    zero_bytes(slab, sizeof(*slab));
    Cache &cache = gHeap.caches[cache_index];
    const uint64_t capacity = (kPageSize - slab_object_area()) / cache.stride;
    slab->magic = kSlabMagic;
    slab->cache_index = cache_index;
    slab->capacity = static_cast<uint16_t>(capacity);
    slab->free_count = static_cast<uint16_t>(capacity);
    slab->list_index = kEmptyList;
}

void update_live_allocated(size_t requested_size) {
    ++gHeap.stats.allocations;
    ++gHeap.stats.live_allocations;
    gHeap.stats.live_bytes += requested_size;
    if (gHeap.stats.live_bytes > gHeap.stats.peak_bytes)
        gHeap.stats.peak_bytes = gHeap.stats.live_bytes;
}

void *allocate_from_slab_locked(uint32_t cache_index, uint32_t requested_size) {
    Cache &cache = gHeap.caches[cache_index];
    SlabPage *slab = cache.lists[kPartialList].head;
    if (!slab) slab = cache.lists[kEmptyList].head;
    if (!slab || slab->magic != kSlabMagic || slab->cache_index != cache_index || !slab->free_count)
        return nullptr;

    uint32_t index = 0;
    while (index < slab->capacity && is_allocated(slab, index)) ++index;
    if (index == slab->capacity) return nullptr;

    set_allocated(slab, index, true);
    --slab->free_count;
    const uint8_t new_list = slab->free_count == 0 ? kFullList : kPartialList;
    move_slab(cache, slab, new_list);

    auto *object = reinterpret_cast<ObjectHeader *>(
        reinterpret_cast<uintptr_t>(slab) + slab_object_area() + static_cast<uint64_t>(index) * cache.stride);
    object->magic = kObjectMagic;
    object->requested_size = requested_size;
    object->cache_index = static_cast<uint16_t>(cache_index);
    object->reserved = 0;
    void *result = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(object) + object_data_offset());
    return result;
}

SlabPage *find_slab_locked(uint64_t page_address) {
    DirectoryEntry *entry = directory_find(page_address);
    return entry && entry->kind == kDirectorySlab
        ? static_cast<SlabPage *>(entry->allocation) : nullptr;
}

bool locate_slab_object_locked(void *pointer, SlabPage **slab_out, ObjectHeader **object_out,
                               uint32_t *slot_out, uint32_t *cache_index_out) {
    const uint64_t address = reinterpret_cast<uint64_t>(pointer);
    const uint64_t page_address = address & ~(kPageSize - 1);
    SlabPage *slab = find_slab_locked(page_address);
    if (!slab || slab->magic != kSlabMagic || slab->cache_index >= kCacheCount) return false;

    Cache &cache = gHeap.caches[slab->cache_index];
    const uint64_t first_data = page_address + slab_object_area() + object_data_offset();
    if (address < first_data) return false;
    const uint64_t delta = address - first_data;
    if (delta % cache.stride) return false;
    const uint64_t index = delta / cache.stride;
    if (index >= slab->capacity || index >= kBitmapBits) return false;

    auto *object = reinterpret_cast<ObjectHeader *>(
        page_address + slab_object_area() + index * cache.stride);
    if (reinterpret_cast<uint64_t>(pointer) != reinterpret_cast<uint64_t>(object) + object_data_offset() ||
        object->magic != kObjectMagic || object->cache_index != slab->cache_index ||
        object->requested_size > cache.payload_size || !is_allocated(slab, static_cast<uint32_t>(index))) return false;
    *slab_out = slab;
    *object_out = object;
    *slot_out = static_cast<uint32_t>(index);
    *cache_index_out = slab->cache_index;
    return true;
}

LargeAllocation *find_large_locked(void *pointer) {
    DirectoryEntry *entry = directory_find(reinterpret_cast<uint64_t>(pointer));
    return entry && entry->kind == kDirectoryLarge
        ? static_cast<LargeAllocation *>(entry->allocation) : nullptr;
}

void *allocate_large(size_t size) {
    if (size > UINT64_MAX - large_data_offset()) return nullptr;
    const uint64_t total = large_data_offset() + static_cast<uint64_t>(size);
    if (total > UINT64_MAX - (kPageSize - 1)) return nullptr;
    const uint64_t page_count = (total + kPageSize - 1) / kPageSize;
    if (!page_count || page_count > UINT64_MAX / kPageSize) return nullptr;

    void *base = welovec::memory::vmm::allocate_pages(page_count);
    if (!base) return nullptr;
    auto *large = static_cast<LargeAllocation *>(base);
    large->magic = kLargeMagic;
    large->requested_size = size;
    large->page_count = page_count;
    large->returned_pointer = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(base) + large_data_offset());

    const uint64_t irq = gHeap.lock.lock_irqsave();
    if (!directory_insert(reinterpret_cast<uint64_t>(large->returned_pointer), large, kDirectoryLarge)) {
        gHeap.lock.unlock_irqrestore(irq);
        welovec::memory::vmm::free_pages(base);
        return nullptr;
    }
    gHeap.stats.large_pages += page_count;
    update_live_allocated(size);
    void *result = large->returned_pointer;
    gHeap.lock.unlock_irqrestore(irq);
    return result;
}

bool inspect_allocation_locked(void *pointer, uint64_t *requested_size) {
    if (LargeAllocation *large = find_large_locked(pointer)) {
        if (large->magic != kLargeMagic || large->returned_pointer != pointer) return false;
        *requested_size = large->requested_size;
        return true;
    }
    SlabPage *slab = nullptr;
    ObjectHeader *object = nullptr;
    uint32_t slot = 0, cache_index = 0;
    if (!locate_slab_object_locked(pointer, &slab, &object, &slot, &cache_index)) return false;
    (void)slab;
    (void)slot;
    (void)cache_index;
    *requested_size = object->requested_size;
    return true;
}

}

bool initialize() {
    if (!welovec::memory::vmm::ready()) return false;
    const uint64_t irq = gHeap.lock.lock_irqsave();
    if (!gHeap.initialized) {
        for (uint32_t i = 0; i < kCacheCount; ++i) {
            gHeap.caches[i].payload_size = kPayloadSizes[i];
            bool ok = false;
            const uint64_t stride = align_up(sizeof(ObjectHeader) + kPayloadSizes[i], 16, &ok);
            if (!ok || stride > UINT32_MAX) {
                gHeap.lock.unlock_irqrestore(irq);
                return false;
            }
            gHeap.caches[i].stride = static_cast<uint32_t>(stride);
        }
        gHeap.initialized = true;
    }
    gHeap.lock.unlock_irqrestore(irq);
    return true;
}

bool ready() { return gHeap.initialized; }

void *allocate(size_t size) {
    if (!size || !gHeap.initialized) return nullptr;
    const int selected_cache = cache_for_size(size);
    if (selected_cache < 0) return allocate_large(size);
    const uint32_t cache_index = static_cast<uint32_t>(selected_cache);

    for (;;) {
        const uint64_t irq = gHeap.lock.lock_irqsave();
        Cache &cache = gHeap.caches[cache_index];
        if (cache.lists[kPartialList].head || cache.lists[kEmptyList].head) {
            void *result = allocate_from_slab_locked(cache_index, static_cast<uint32_t>(size));
            if (result) {
                update_live_allocated(size);
                gHeap.lock.unlock_irqrestore(irq);
                return result;
            }
            gHeap.lock.unlock_irqrestore(irq);
            return nullptr;
        }
        gHeap.lock.unlock_irqrestore(irq);

        auto *new_slab = static_cast<SlabPage *>(welovec::memory::vmm::allocate_pages(1));
        if (!new_slab) return nullptr;
        initialize_slab(new_slab, cache_index);

        const uint64_t attach_irq = gHeap.lock.lock_irqsave();
        if (!directory_insert(reinterpret_cast<uint64_t>(new_slab), new_slab, kDirectorySlab)) {
            gHeap.lock.unlock_irqrestore(attach_irq);
            welovec::memory::vmm::free_pages(new_slab);
            return nullptr;
        }
        list_add(gHeap.caches[cache_index], new_slab, kEmptyList);
        ++gHeap.stats.slab_pages;
        gHeap.lock.unlock_irqrestore(attach_irq);
    }
}

void *allocate_zeroed(size_t size) {
    void *result = allocate(size);
    if (result) zero_bytes(result, size);
    return result;
}

bool deallocate(void *pointer) {
    if (!pointer) return true;
    if (!gHeap.initialized) return false;

    const uint64_t irq = gHeap.lock.lock_irqsave();
    if (LargeAllocation *large = find_large_locked(pointer)) {
        if (large->magic != kLargeMagic || large->returned_pointer != pointer || !large->page_count) {
            gHeap.lock.unlock_irqrestore(irq);
            return false;
        }
        const uint64_t size = large->requested_size;
        const uint64_t pages = large->page_count;
        DirectoryEntry *entry = directory_find(reinterpret_cast<uint64_t>(pointer));
        directory_remove(entry);
        if (!welovec::memory::vmm::free_pages(large)) {
            directory_insert(reinterpret_cast<uint64_t>(pointer), large, kDirectoryLarge);
            gHeap.lock.unlock_irqrestore(irq);
            return false;
        }
        ++gHeap.stats.frees;
        --gHeap.stats.live_allocations;
        gHeap.stats.live_bytes -= size;
        gHeap.stats.large_pages -= pages;
        gHeap.lock.unlock_irqrestore(irq);
        return true;
    }

    SlabPage *slab = nullptr;
    ObjectHeader *object = nullptr;
    uint32_t slot = 0, cache_index = 0;
    if (!locate_slab_object_locked(pointer, &slab, &object, &slot, &cache_index)) {
        gHeap.lock.unlock_irqrestore(irq);
        return false;
    }
    Cache &cache = gHeap.caches[cache_index];
    const uint64_t size = object->requested_size;
    if (!slab->free_count || !is_allocated(slab, slot)) {
        gHeap.lock.unlock_irqrestore(irq);
        return false;
    }
    set_allocated(slab, slot, false);
    ++slab->free_count;
    object->magic = 0;
    object->requested_size = 0;
    const uint8_t new_list = slab->free_count == slab->capacity ? kEmptyList : kPartialList;
    move_slab(cache, slab, new_list);

    ++gHeap.stats.frees;
    --gHeap.stats.live_allocations;
    gHeap.stats.live_bytes -= size;

    if (new_list == kEmptyList && cache.lists[kEmptyList].count > 1) {
        DirectoryEntry *entry = directory_find(reinterpret_cast<uint64_t>(slab));
        list_remove(cache, slab);
        directory_remove(entry);
        if (welovec::memory::vmm::free_pages(slab)) --gHeap.stats.slab_pages;
        else {
            directory_insert(reinterpret_cast<uint64_t>(slab), slab, kDirectorySlab);
            list_add(cache, slab, kEmptyList);
        }
    }
    gHeap.lock.unlock_irqrestore(irq);
    return true;
}

void *reallocate(void *pointer, size_t new_size) {
    if (!pointer) return allocate(new_size);
    if (!new_size) {
        deallocate(pointer);
        return nullptr;
    }
    if (!gHeap.initialized) return nullptr;

    uint64_t old_size = 0;
    const uint64_t irq = gHeap.lock.lock_irqsave();
    const bool valid = inspect_allocation_locked(pointer, &old_size);
    gHeap.lock.unlock_irqrestore(irq);
    if (!valid) return nullptr;
    if (old_size == new_size) return pointer;

    void *replacement = allocate(new_size);
    if (!replacement) return nullptr;
    const uint64_t copy_size = old_size < new_size ? old_size : new_size;
    copy_bytes(replacement, pointer, copy_size);
    if (!deallocate(pointer)) {
        deallocate(replacement);
        return nullptr;
    }
    return replacement;
}

Statistics statistics() {
    const uint64_t irq = gHeap.lock.lock_irqsave();
    const Statistics result = gHeap.stats;
    gHeap.lock.unlock_irqrestore(irq);
    return result;
}

}

extern "C" void *kmalloc(size_t size) { return welovec::memory::heap::allocate(size); }
extern "C" void *kzalloc(size_t size) { return welovec::memory::heap::allocate_zeroed(size); }
extern "C" void *kcalloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return nullptr;
    return welovec::memory::heap::allocate_zeroed(count * size);
}
extern "C" void *krealloc(void *pointer, size_t new_size) {
    return welovec::memory::heap::reallocate(pointer, new_size);
}
extern "C" void kfree(void *pointer) { (void)welovec::memory::heap::deallocate(pointer); }
