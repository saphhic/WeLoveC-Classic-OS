// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the memory component in the runtime module.

#include <stddef.h>
#include <stdint.h>

extern "C" __attribute__((noinline)) void *memset(void *destination, int value, size_t size) {
    auto *bytes = static_cast<volatile uint8_t *>(destination);
    for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(value);
    return destination;
}

extern "C" __attribute__((noinline)) void *memcpy(void *destination, const void *source,
                                                   size_t size) {
    auto *out = static_cast<volatile uint8_t *>(destination);
    const auto *in = static_cast<const volatile uint8_t *>(source);
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
    return destination;
}

extern "C" __attribute__((noinline)) void *memmove(void *destination, const void *source,
                                                    size_t size) {
    auto *out = static_cast<volatile uint8_t *>(destination);
    const auto *in = static_cast<const volatile uint8_t *>(source);
    const uintptr_t destination_address = reinterpret_cast<uintptr_t>(destination);
    const uintptr_t source_address = reinterpret_cast<uintptr_t>(source);
    if (destination_address < source_address) {
        for (size_t i = 0; i < size; ++i) out[i] = in[i];
    } else if (destination_address > source_address) {
        for (size_t i = size; i; --i) out[i - 1] = in[i - 1];
    }
    return destination;
}

extern "C" __attribute__((noinline)) int memcmp(const void *left, const void *right,
                                                  size_t size) {
    const auto *a = static_cast<const volatile uint8_t *>(left);
    const auto *b = static_cast<const volatile uint8_t *>(right);
    for (size_t i = 0; i < size; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}
