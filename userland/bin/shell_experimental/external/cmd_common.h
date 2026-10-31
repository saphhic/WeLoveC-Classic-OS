// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for cmd common in the shell experimental module.

#ifndef WELOVEC_SHELL_COMMAND_COMMON_H
#define WELOVEC_SHELL_COMMAND_COMMON_H

#include <stdint.h>
#include "user/filesystem.h"
#include "user/stdio.h"
#include "user/syscall.h"

#define CMD_WHITE UINT32_C(0xffffffff)
#define CMD_GREEN UINT32_C(0xff70e080)
#define CMD_RED UINT32_C(0xffff6666)
#define CMD_YELLOW UINT32_C(0xffffd166)

static inline uint32_t cmd_length(const char *text) {
    uint32_t length = 0;
    while (text && text[length]) ++length;
    return length;
}

static inline int cmd_equal(const char *left, const char *right) {
    uint32_t index = 0;
    while (left[index] && right[index] && left[index] == right[index]) ++index;
    return left[index] == '\0' && right[index] == '\0';
}

static inline void cmd_print(const char *text, uint32_t color) {
    (void)print_color(text, color, WELOVEC_FONT_SYSTEM8);
}

static inline void cmd_print_number(uint64_t number, uint32_t color) {
    char digits[21];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + number % 10);
        number /= 10;
    } while (number && count < sizeof(digits));
    while (count) {
        const char digit[2] = {digits[--count], '\0'};
        cmd_print(digit, color);
    }
}

static inline void cmd_print_error(const char *command, int64_t error) {
    cmd_print(command, CMD_RED);
    cmd_print(": operation failed (error ", CMD_RED);
    if (error < 0) cmd_print_number((uint64_t)(-error), CMD_RED);
    else cmd_print_number((uint64_t)error, CMD_RED);
    cmd_print(")\n", CMD_RED);
}

static inline int cmd_has_extension(const char *name, const char *extension) {
    const uint32_t name_length = cmd_length(name);
    const uint32_t extension_length = cmd_length(extension);
    if (name_length < extension_length) return 0;
    for (uint32_t index = 0; index < extension_length; ++index)
        if (name[name_length - extension_length + index] != extension[index])
            return 0;
    return 1;
}

static inline uint8_t cmd_hex_digit(uint8_t value) {
    value &= 0x0f;
    return (uint8_t)(value < 10 ? '0' + value : 'a' + value - 10);
}

static inline void cmd_print_hex_byte(uint8_t value, uint32_t color) {
    char output[3] = {
        (char)cmd_hex_digit(value >> 4), (char)cmd_hex_digit(value), '\0'
    };
    cmd_print(output, color);
}

static inline int cmd_valid_ipv4(const char *text, uint8_t address[4]) {
    uint32_t part = 0;
    uint32_t value = 0;
    uint32_t digits = 0;
    for (uint32_t index = 0;; ++index) {
        const char c = text[index];
        if (c >= '0' && c <= '9') {
            if (++digits > 3) return 0;
            value = value * 10 + (uint32_t)(c - '0');
            if (value > 255) return 0;
        } else if (c == '.' || c == '\0') {
            if (!digits || part >= 4) return 0;
            address[part++] = (uint8_t)value;
            value = 0;
            digits = 0;
            if (!c) return part == 4;
        } else {
            return 0;
        }
    }
}

static inline int64_t cmd_copy_file(const char *source, const char *destination) {
    if (cmd_equal(source, destination)) return -22;
    const int64_t input = welovec_open(source, WELOVEC_OPEN_READ);
    if (input < 0) return input;
    const int64_t output = welovec_open(
        destination, WELOVEC_OPEN_WRITE | WELOVEC_OPEN_CREATE | WELOVEC_OPEN_TRUNCATE);
    if (output < 0) {
        (void)welovec_close((int)input);
        return output;
    }
    char buffer[1024];
    int64_t result = 0;
    for (;;) {
        const int64_t count = welovec_read((int)input, buffer, sizeof(buffer));
        if (count < 0) {
            result = count;
            break;
        }
        if (!count) break;
        const int64_t written = welovec_write_fd((int)output, buffer,
                                                  (uint64_t)count);
        if (written != count) {
            result = written < 0 ? written : -5;
            break;
        }
    }
    (void)welovec_close((int)input);
    (void)welovec_close((int)output);
    if (result < 0) (void)welovec_fs_unlink(destination);
    return result;
}

#endif
