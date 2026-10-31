// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares basic conversion and memory functions for the freestanding environment.

#ifndef WELOVEC_FREESTANDING_STDLIB_H
#define WELOVEC_FREESTANDING_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *memory, size_t size);
void free(void *memory);
_Noreturn void abort(void);
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);

#endif
