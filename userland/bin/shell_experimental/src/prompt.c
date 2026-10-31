// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Draws and updates the experimental shell prompt.

#include "../include/shell.h"
#include "user/stdio.h"

void shell_print(const char *text, uint32_t color) {
    (void)print_color(text, color, WELOVEC_FONT_SYSTEM8);
}

void shell_print_unsigned(uint64_t value, uint32_t color) {
    char digits[21];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));
    while (count) {
        const char digit[2] = {digits[--count], '\0'};
        shell_print(digit, color);
    }
}

void shell_print_prompt(const struct shell_state *state) {
    shell_print(state->username, UINT32_C(0xffa8c7ff));
    shell_print("@", UINT32_C(0xffffffff));
    shell_print(state->cwd, UINT32_C(0xffa8c7ff));
    shell_print(":> ", UINT32_C(0xffa8c7ff));
}
