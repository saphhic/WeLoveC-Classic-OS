// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Adapts Mbed TLS platform functions to the freestanding environment.

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "user/security.h"
#include "user/syscall.h"
#include "user/time.h"
#include <mbedtls/platform_time.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>
#include <psa/crypto_extra.h>
#include <psa/crypto_platform.h>

#define TLS_HEAP_SIZE (16u * 1024u * 1024u)
#define TLS_HEAP_ALIGNMENT 16u

struct tls_heap_block {
    uint64_t capacity;
    uint64_t requested;
    struct tls_heap_block *next;
    uint64_t free;
};

static uint8_t *tls_heap;
static struct tls_heap_block *tls_heap_first;

void *memcpy(void *destination, const void *source, size_t size) {
    uint8_t *output = (uint8_t *)destination;
    const uint8_t *input = (const uint8_t *)source;
    for (size_t index = 0; index < size; ++index) output[index] = input[index];
    return destination;
}

static uint64_t align_heap_size(uint64_t size) {
    return (size + TLS_HEAP_ALIGNMENT - 1) & ~(uint64_t)(TLS_HEAP_ALIGNMENT - 1);
}

static int tls_heap_initialize(void) {
    if (tls_heap) return 1;
    tls_heap = (uint8_t *)welovec_mmap(TLS_HEAP_SIZE);
    if (!tls_heap) return 0;
    tls_heap_first = (struct tls_heap_block *)tls_heap;
    tls_heap_first->capacity = TLS_HEAP_SIZE - sizeof(*tls_heap_first);
    tls_heap_first->requested = 0;
    tls_heap_first->next = 0;
    tls_heap_first->free = 1;
    return 1;
}

void *malloc(size_t size) {
    if (!size) size = 1;
    const uint64_t wanted = align_heap_size(size);
    if (wanted < size || !tls_heap_initialize()) return 0;

    for (struct tls_heap_block *block = tls_heap_first; block; block = block->next) {
        if (!block->free || block->capacity < wanted) continue;
        const uint64_t remainder = block->capacity - wanted;
        if (remainder >= sizeof(*block) + TLS_HEAP_ALIGNMENT) {
            struct tls_heap_block *split = (struct tls_heap_block *)
                ((uint8_t *)(block + 1) + wanted);
            split->capacity = remainder - sizeof(*split);
            split->requested = 0;
            split->next = block->next;
            split->free = 1;
            block->next = split;
            block->capacity = wanted;
        }
        block->requested = size;
        block->free = 0;
        return block + 1;
    }
    return 0;
}

void free(void *memory) {
    if (!memory || !tls_heap) return;
    struct tls_heap_block *block = (struct tls_heap_block *)memory - 1;
    const uint8_t *address = (const uint8_t *)block;
    if (address < tls_heap || address >= tls_heap + TLS_HEAP_SIZE || block->free)
        return;
    volatile uint8_t *bytes = (volatile uint8_t *)(block + 1);
    for (uint64_t index = 0; index < block->capacity; ++index) bytes[index] = 0;
    block->requested = 0;
    block->free = 1;

    for (struct tls_heap_block *cursor = tls_heap_first; cursor; cursor = cursor->next) {
        if (cursor->free && cursor->next && cursor->next->free) {
            cursor->capacity += sizeof(*cursor->next) + cursor->next->capacity;
            cursor->next = cursor->next->next;
            cursor = tls_heap_first;
        }
    }
}

void *calloc(size_t count, size_t size) {
    if (count && size > SIZE_MAX / count) return 0;
    const size_t total = count * size;
    void *memory = malloc(total);
    if (memory) {
        uint8_t *bytes = (uint8_t *)memory;
        for (size_t index = 0; index < total; ++index) bytes[index] = 0;
    }
    return memory;
}

void *realloc(void *memory, size_t size) {
    if (!memory) return malloc(size);
    if (!size) { free(memory); return 0; }
    struct tls_heap_block *block = (struct tls_heap_block *)memory - 1;
    if (size <= block->capacity) {
        block->requested = size;
        return memory;
    }
    void *replacement = malloc(size);
    if (!replacement) return 0;
    size_t copy_size = block->requested < size ? (size_t)block->requested : size;
    (void)memcpy(replacement, memory, copy_size);
    free(memory);
    return replacement;
}

void *welovec_tls_calloc(size_t count, size_t size) {
    return calloc(count, size);
}

void welovec_tls_free(void *memory) {
    free(memory);
}

void welovec_tls_heap_release(void) {
    if (!tls_heap) return;
    volatile uint8_t *bytes = tls_heap;
    for (uint64_t index = 0; index < TLS_HEAP_SIZE; ++index) bytes[index] = 0;
    (void)welovec_munmap(tls_heap, TLS_HEAP_SIZE);
    tls_heap = 0;
    tls_heap_first = 0;
}

void *memset(void *destination, int value, size_t size) {
    uint8_t *bytes = (uint8_t *)destination;
    for (size_t index = 0; index < size; ++index) bytes[index] = (uint8_t)value;
    return destination;
}

void *memmove(void *destination, const void *source, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    if (out < in) {
        for (size_t index = 0; index < size; ++index) out[index] = in[index];
    } else if (out > in) {
        while (size) { --size; out[size] = in[size]; }
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t size) {
    const uint8_t *a = (const uint8_t *)left;
    const uint8_t *b = (const uint8_t *)right;
    for (size_t index = 0; index < size; ++index)
        if (a[index] != b[index]) return a[index] < b[index] ? -1 : 1;
    return 0;
}

size_t strlen(const char *text) {
    size_t length = 0;
    while (text[length]) ++length;
    return length;
}

size_t strnlen(const char *text, size_t maximum) {
    size_t length = 0;
    while (length < maximum && text[length]) ++length;
    return length;
}

int strcmp(const char *left, const char *right) {
    while (*left && *left == *right) { ++left; ++right; }
    return (uint8_t)*left < (uint8_t)*right ? -1 : (uint8_t)*left != (uint8_t)*right;
}

int strncmp(const char *left, const char *right, size_t maximum) {
    for (size_t index = 0; index < maximum; ++index) {
        const uint8_t a = (uint8_t)left[index];
        const uint8_t b = (uint8_t)right[index];
        if (a != b) return a < b ? -1 : 1;
        if (!a) return 0;
    }
    return 0;
}

char *strncpy(char *destination, const char *source, size_t maximum) {
    size_t index = 0;
    while (index < maximum && source[index]) {
        destination[index] = source[index];
        ++index;
    }
    while (index < maximum) destination[index++] = '\0';
    return destination;
}

char *strchr(const char *text, int character) {
    const char target = (char)character;
    for (;;) {
        if (*text == target) return (char *)text;
        if (!*text) return 0;
        ++text;
    }
}

struct tls_format_output {
    char *buffer;
    size_t capacity;
    size_t length;
};

static void format_character(struct tls_format_output *output, char value) {
    if (output->capacity && output->length + 1 < output->capacity)
        output->buffer[output->length] = value;
    ++output->length;
}

static void format_unsigned(struct tls_format_output *output, uint64_t value,
                            unsigned base, int uppercase, unsigned width,
                            int zero_pad, int left_align, char sign) {
    char digits[32];
    unsigned count = 0;
    do {
        const unsigned digit = (unsigned)(value % base);
        digits[count++] = (char)(digit < 10 ? '0' + digit :
            (uppercase ? 'A' : 'a') + digit - 10);
        value /= base;
    } while (value && count < sizeof(digits));

    const unsigned content = count + (sign != 0);
    const unsigned padding = width > content ? width - content : 0;
    if (!left_align && !zero_pad)
        for (unsigned index = 0; index < padding; ++index)
            format_character(output, ' ');
    if (sign) format_character(output, sign);
    if (!left_align && zero_pad)
        for (unsigned index = 0; index < padding; ++index)
            format_character(output, '0');
    while (count) format_character(output, digits[--count]);
    if (left_align)
        for (unsigned index = 0; index < padding; ++index)
            format_character(output, ' ');
}

int vsnprintf(char *buffer, size_t capacity, const char *format,
              va_list arguments) {
    if (!format || (!buffer && capacity)) return -1;
    struct tls_format_output output = {buffer, capacity, 0};
    while (*format) {
        if (*format != '%') {
            format_character(&output, *format++);
            continue;
        }
        ++format;
        if (*format == '%') {
            format_character(&output, *format++);
            continue;
        }

        int left_align = 0, zero_pad = 0;
        while (*format == '-' || *format == '0') {
            if (*format == '-') left_align = 1;
            if (*format == '0') zero_pad = 1;
            ++format;
        }
        unsigned width = 0;
        while (*format >= '0' && *format <= '9') {
            if (width < 10000) width = width * 10 + (unsigned)(*format - '0');
            ++format;
        }
        int precision = -1;
        if (*format == '.') {
            ++format;
            precision = 0;
            while (*format >= '0' && *format <= '9') {
                if (precision < 10000) precision = precision * 10 + (*format - '0');
                ++format;
            }
        }
        enum { TLS_FMT_INT, TLS_FMT_LONG, TLS_FMT_LLONG, TLS_FMT_SIZE } length = TLS_FMT_INT;
        if (*format == 'z') { length = TLS_FMT_SIZE; ++format; }
        else if (*format == 'l') {
            ++format;
            length = *format == 'l' ? TLS_FMT_LLONG : TLS_FMT_LONG;
            if (*format == 'l') ++format;
        }

        const char conversion = *format ? *format++ : '\0';
        if (conversion == 's') {
            const char *text = va_arg(arguments, const char *);
            if (!text) text = "(null)";
            size_t size = strlen(text);
            if (precision >= 0 && size > (size_t)precision) size = (size_t)precision;
            const unsigned padding = width > size ? width - (unsigned)size : 0;
            if (!left_align)
                for (unsigned index = 0; index < padding; ++index)
                    format_character(&output, ' ');
            for (size_t index = 0; index < size; ++index)
                format_character(&output, text[index]);
            if (left_align)
                for (unsigned index = 0; index < padding; ++index)
                    format_character(&output, ' ');
        } else if (conversion == 'c') {
            format_character(&output, (char)va_arg(arguments, int));
        } else if (conversion == 'd' || conversion == 'i') {
            int64_t value;
            if (length == TLS_FMT_LLONG) value = va_arg(arguments, long long);
            else if (length == TLS_FMT_LONG) value = va_arg(arguments, long);
            else if (length == TLS_FMT_SIZE) value = (int64_t)va_arg(arguments, ptrdiff_t);
            else value = va_arg(arguments, int);
            const char sign = value < 0 ? '-' : 0;
            const uint64_t magnitude = value < 0
                ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
            format_unsigned(&output, magnitude, 10, 0, width, zero_pad,
                            left_align, sign);
        } else if (conversion == 'u' || conversion == 'x' || conversion == 'X' ||
                   conversion == 'o' || conversion == 'p') {
            uint64_t value;
            if (conversion == 'p') value = (uintptr_t)va_arg(arguments, void *);
            else if (length == TLS_FMT_LLONG) value = va_arg(arguments, unsigned long long);
            else if (length == TLS_FMT_LONG) value = va_arg(arguments, unsigned long);
            else if (length == TLS_FMT_SIZE) value = va_arg(arguments, size_t);
            else value = va_arg(arguments, unsigned int);
            const unsigned base = conversion == 'x' || conversion == 'X' ||
                                  conversion == 'p' ? 16 :
                                  conversion == 'o' ? 8 : 10;
            const unsigned minimum = precision > 0 ? (unsigned)precision : 0;
            format_unsigned(&output, value, base, conversion == 'X',
                            width > minimum ? width : minimum,
                            zero_pad && precision < 0, left_align, 0);
        } else {
            format_character(&output, '%');
            if (conversion) format_character(&output, conversion);
        }
    }
    if (capacity) {
        const size_t end = output.length < capacity ? output.length : capacity - 1;
        output.buffer[end] = '\0';
    }
    return output.length > INT_MAX ? -1 : (int)output.length;
}

int snprintf(char *buffer, size_t capacity, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int result = vsnprintf(buffer, capacity, format, arguments);
    va_end(arguments);
    return result;
}

static unsigned __int128 divide_u128(unsigned __int128 numerator,
                                     unsigned __int128 denominator,
                                     unsigned __int128 *remainder) {
    if (!denominator) __builtin_trap();
    unsigned __int128 quotient = 0;
    unsigned __int128 rest = 0;
    for (int bit = 127; bit >= 0; --bit) {
        const unsigned carry = (unsigned)(rest >> 127);
        rest = (rest << 1) | ((numerator >> bit) & 1u);
        if (carry || rest >= denominator) {
            rest -= denominator;
            quotient |= (unsigned __int128)1 << bit;
        }
    }
    if (remainder) *remainder = rest;
    return quotient;
}

unsigned __int128 __udivmodti4(unsigned __int128 numerator,
                               unsigned __int128 denominator,
                               unsigned __int128 *remainder) {
    return divide_u128(numerator, denominator, remainder);
}

unsigned __int128 __udivti3(unsigned __int128 numerator,
                            unsigned __int128 denominator) {
    return divide_u128(numerator, denominator, 0);
}

unsigned __int128 __umodti3(unsigned __int128 numerator,
                            unsigned __int128 denominator) {
    unsigned __int128 remainder;
    (void)divide_u128(numerator, denominator, &remainder);
    return remainder;
}

static unsigned __int128 signed_magnitude(__int128 value) {
    return value < 0 ? (unsigned __int128)(-(value + 1)) + 1u
                     : (unsigned __int128)value;
}

__int128 __divti3(__int128 numerator, __int128 denominator) {
    const int negative = (numerator < 0) != (denominator < 0);
    const unsigned __int128 quotient = divide_u128(
        signed_magnitude(numerator), signed_magnitude(denominator), 0);
    return negative ? -(__int128)quotient : (__int128)quotient;
}

__int128 __modti3(__int128 numerator, __int128 denominator) {
    unsigned __int128 remainder;
    (void)divide_u128(signed_magnitude(numerator), signed_magnitude(denominator),
                      &remainder);
    return numerator < 0 ? -(__int128)remainder : (__int128)remainder;
}

char *strstr(const char *text, const char *needle) {
    if (!needle[0]) return (char *)text;
    for (; *text; ++text) {
        size_t index = 0;
        while (needle[index] && text[index] == needle[index]) ++index;
        if (!needle[index]) return (char *)text;
    }
    return 0;
}

_Noreturn void abort(void) {
    __builtin_trap();
    for (;;) __asm__ volatile("pause");
}

static int leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
    const unsigned day_of_year = (153 * adjusted_month + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 -
                                year_of_era / 100 + day_of_year;
    return (int64_t)era * 146097 + day_of_era - 719468;
}

long welovec_tls_time(long *result) {
    struct welovec_user_datetime date;
    if (welovec_rtc_get_utc(&date) < 0 || date.month < 1 || date.month > 12 ||
        date.day < 1 || date.day > 31 || date.hour < 0 || date.hour > 23 ||
        date.minute < 0 || date.minute > 59 || date.second < 0 || date.second > 60)
        return -1;
    static const uint8_t month_days[] = {31, 28, 31, 30, 31, 30,
                                         31, 31, 30, 31, 30, 31};
    uint8_t maximum_day = month_days[date.month - 1];
    if (date.month == 2 && leap_year(date.year)) ++maximum_day;
    if (date.day > maximum_day) return -1;
    const int64_t seconds = days_from_civil(date.year, (unsigned)date.month,
                                             (unsigned)date.day) * 86400 +
                            date.hour * 3600 + date.minute * 60 + date.second;
    if (seconds < LONG_MIN || seconds > LONG_MAX) return -1;
    if (result) *result = (long)seconds;
    return (long)seconds;
}

struct tm *mbedtls_platform_gmtime_r(const long *timestamp, struct tm *out) {
    if (!timestamp || !out) return 0;
    int64_t days = *timestamp / 86400;
    int64_t seconds = *timestamp % 86400;
    if (seconds < 0) { seconds += 86400; --days; }
    int64_t shifted = days + 719468;
    const int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const unsigned day_of_era = (unsigned)(shifted - era * 146097);
    const unsigned year_of_era = (day_of_era - day_of_era / 1460 +
        day_of_era / 36524 - day_of_era / 146096) / 365;
    int year = (int)year_of_era + (int)(era * 400);
    const unsigned day_of_year = day_of_era -
        (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const unsigned month_prime = (5 * day_of_year + 2) / 153;
    const unsigned day = day_of_year - (153 * month_prime + 2) / 5 + 1;
    const unsigned month = month_prime < 10 ? month_prime + 3 : month_prime - 9;
    year += month <= 2;
    if (year < 1900 || year > 9999) return 0;

    out->tm_year = year - 1900;
    out->tm_mon = (int)month - 1;
    out->tm_mday = (int)day;
    out->tm_hour = (int)(seconds / 3600);
    out->tm_min = (int)((seconds % 3600) / 60);
    out->tm_sec = (int)(seconds % 60);
    out->tm_wday = (int)((days + 4) % 7);
    if (out->tm_wday < 0) out->tm_wday += 7;
    out->tm_yday = (int)(days - days_from_civil(year, 1, 1));
    out->tm_isdst = 0;
    return out;
}

int64_t mbedtls_ms_time(void) {
    const uint64_t ticks = welovec_getticks();
    return ticks > (uint64_t)INT64_MAX / 10 ? INT64_MAX : (int64_t)(ticks * 10);
}

psa_status_t mbedtls_psa_external_get_random(
    mbedtls_psa_external_random_context_t *context, uint8_t *output,
    size_t output_size, size_t *output_length) {
    (void)context;
    if (!output_length || (!output && output_size)) return PSA_ERROR_INVALID_ARGUMENT;
    *output_length = 0;
    size_t offset = 0;
    while (offset < output_size) {
        size_t chunk = output_size - offset;
        if (chunk > 256) chunk = 256;
        if (welovec_secure_random(output + offset, chunk) != (int64_t)chunk) {
            volatile uint8_t *bytes = output;
            for (size_t index = 0; index < offset; ++index) bytes[index] = 0;
            return PSA_ERROR_INSUFFICIENT_ENTROPY;
        }
        offset += chunk;
    }
    *output_length = output_size;
    return PSA_SUCCESS;
}
