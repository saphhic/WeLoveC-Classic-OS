// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Contains helper functions shared by desktop.

#include "desktop/include/desktop.h"

/*
 * Tiny libc replacement. Userland ELFs link no C library, yet the compiler may
 * still emit calls to memset/memcpy/memmove for aggregate copies and zeroing,
 * so the three symbols are provided here. Optimisation is switched off for them
 * so a later -O2 build cannot turn the loops back into recursive calls.
 */

#pragma clang optimize off
void *memset(void *dst, int value, __SIZE_TYPE__ size) {
    unsigned char *out = (unsigned char *)dst;
    while (size--) *out++ = (unsigned char)value;
    return dst;
}

void *memcpy(void *dst, const void *src, __SIZE_TYPE__ size) {
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;
    while (size--) *out++ = *in++;
    return dst;
}

void *memmove(void *dst, const void *src, __SIZE_TYPE__ size) {
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;
    if (out == in || !size) return dst;
    if (out < in) {
        while (size--) *out++ = *in++;
    } else {
        out += size;
        in += size;
        while (size--) *--out = *--in;
    }
    return dst;
}
#pragma clang optimize on

uint32_t d_strlen(const char *s) {
    uint32_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

int d_streq(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] && a[i] == b[i]) ++i;
    return a[i] == b[i];
}

void d_strcpy(char *dst, uint32_t cap, const char *src) {
    if (!cap) return;
    uint32_t i = 0;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

void d_strcat(char *dst, uint32_t cap, const char *src) {
    const uint32_t used = d_strlen(dst);
    if (used + 1 >= cap) return;
    d_strcpy(dst + used, cap - used, src);
}

uint32_t d_utoa(uint64_t value, char *out) {
    char tmp[21];
    uint32_t n = 0;
    do {
        tmp[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && n < sizeof(tmp));
    for (uint32_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
    return n;
}

void d_pad2(char *out, int32_t value) {
    if (value < 0) value = 0;
    if (value > 99) value = 99;
    out[0] = (char)('0' + value / 10);
    out[1] = (char)('0' + value % 10);
    out[2] = '\0';
}

void d_fmt_size(char *out, uint64_t bytes) {
    static const char *const units[] = {" B", " KB", " MB", " GB"};
    uint32_t unit = 0;
    while (bytes >= 1024 && unit < 3) {
        bytes /= 1024;
        ++unit;
    }
    const uint32_t n = d_utoa(bytes, out);
    d_strcpy(out + n, 8, units[unit]);
}

int d_join_path(char *out, uint32_t cap, const char *dir, const char *name) {
    const uint32_t dl = d_strlen(dir);
    const uint32_t nl = d_strlen(name);
    const int slash = !(dl && dir[dl - 1] == '/');
    if (dl + (slash ? 1u : 0u) + nl + 1 > cap) return 0;
    d_strcpy(out, cap, dir);
    if (slash) d_strcat(out, cap, "/");
    d_strcat(out, cap, name);
    return 1;
}

void d_parent_path(char *path) {
    uint32_t n = d_strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = '\0';
    while (n > 1 && path[n - 1] != '/') path[--n] = '\0';
    while (n > 1 && path[n - 1] == '/') path[--n] = '\0';
    if (!n) {
        path[0] = '/';
        path[1] = '\0';
    }
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

int d_ends_with(const char *s, const char *suffix) {
    const uint32_t sl = d_strlen(s);
    const uint32_t xl = d_strlen(suffix);
    if (xl > sl) return 0;
    for (uint32_t i = 0; i < xl; ++i)
        if (lower(s[sl - xl + i]) != lower(suffix[i])) return 0;
    return 1;
}
