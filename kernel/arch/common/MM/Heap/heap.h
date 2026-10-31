// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for heap in the heap module.

#ifndef WELOVEC_KERNEL_HEAP_C_API_H
#define WELOVEC_KERNEL_HEAP_C_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *pointer, size_t new_size);
void kfree(void *pointer);

#ifdef __cplusplus
}
#endif

#endif
