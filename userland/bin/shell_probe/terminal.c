// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements a test terminal for text input and output.

#include <stdint.h>
#include "user/framebuffer.h"
#include "user/input.h"
#include "user/syscall.h"
#include "user/stdio.h"
#include "lib/fbcolorsheet.h"

#define SHELL_LINE_CAPACITY 128

static void set_caret(uint32_t color) {
    struct welovec_fb_text_cursor cursor;
    if (welovec_print_cursor_get(&cursor) < 0) return;
    (void)welovec_fb_rect(cursor.x, cursor.y, 2, 16, color);
}

static int text_equal(const char *left, const char *right) {
    uint32_t index = 0;
    while (left[index] && right[index] && left[index] == right[index]) ++index;
    return left[index] == '\0' && right[index] == '\0';
}

static void show_prompt(void) {
    (void)print_color("wlc> ", FB_LIME, WELOVEC_FONT_UBUNTU);
}

static void run_command(const char *line) {
    if (!line[0]) return;
    if (text_equal(line, "help")) {
        (void)print_color("Commands: help, echo <text>, exit\n",
                          FB_WHITE, WELOVEC_FONT_UBUNTU);
    } else if (line[0] == 'e' && line[1] == 'c' && line[2] == 'h' &&
               line[3] == 'o' && line[4] == ' ') {
        (void)print_color(line + 5, FB_WHITE, WELOVEC_FONT_UBUNTU);
        (void)print_color("\n", FB_WHITE, WELOVEC_FONT_UBUNTU);
    } else if (text_equal(line, "exit")) {
        welovec_exit(0);
    } else {
        (void)print_color("Command not found. Type help.\n",
                          FB_RED, WELOVEC_FONT_UBUNTU);
    }
}

void cmd_main() {
    (void)clear_screen();
    (void)welovec_print_reset_cursor();
    (void)print_color("WeLoveC shell v1.0\n", FB_LIME, WELOVEC_FONT_UBUNTU);
    show_prompt();
    set_caret(FB_WHITE);

    char line[SHELL_LINE_CAPACITY];
    line[0] = '\0';
    uint32_t line_length = 0;
    for (;;) {
        char input[32];
        const int64_t count = welovec_input_read(input, sizeof(input));
        if (count <= 0) {
            (void)welovec_sleep(10);
            continue;
        }

        for (int64_t index = 0; index < count; ++index) {
            const unsigned char character = (unsigned char)input[index];
            set_caret(FB_BLACK);
            if (character == '\r' || character == '\n') {
                (void)print_color("\n", FB_WHITE, WELOVEC_FONT_UBUNTU);
                line[line_length] = '\0';
                run_command(line);
                line_length = 0;
                line[0] = '\0';
                show_prompt();
            } else if (character == '\b' || character == 0x7f) {
                if (line_length) {
                    line[--line_length] = '\0';
                    (void)print_color("\b", FB_WHITE, WELOVEC_FONT_UBUNTU);
                }
            } else if (character >= 0x20 && character <= 0x7e &&
                       line_length + 1 < sizeof(line)) {
                line[line_length++] = (char)character;
                line[line_length] = '\0';
                char visible[2] = {(char)character, '\0'};
                (void)print_color(visible, FB_WHITE, WELOVEC_FONT_UBUNTU);
            }
            set_caret(FB_WHITE);
        }
    }
}

void userland_main(uint64_t argc, char **argv) {
    (void)argc;
    (void)argv;
    cmd_main();
}
