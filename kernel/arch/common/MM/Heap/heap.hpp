// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares kernel heap functions such as kmalloc and kfree.

#ifndef WELOVEC_KERNEL_HEAP_HPP
#define WELOVEC_KERNEL_HEAP_HPP

#include <stdint.h>
#include "heap.h"

namespace welovec::memory::heap {

struct Statistics {
    uint64_t allocations;
    uint64_t frees;
    uint64_t live_allocations;
    uint64_t live_bytes;
    uint64_t peak_bytes;
    uint64_t slab_pages;
    uint64_t large_pages;
};

bool initialize();
bool ready();
void *allocate(size_t size);
void *allocate_zeroed(size_t size);
void *reallocate(void *pointer, size_t new_size);
bool deallocate(void *pointer);
Statistics statistics();

}

#endif
