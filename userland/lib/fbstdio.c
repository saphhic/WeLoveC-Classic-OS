// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Provides user text input and output through the framebuffer.

#include "user/stdio.h"

#include "user/framebuffer.h"

#define FB_TEXT_LIMIT 4096u
#define FB_MAX_PIXELS_PER_CALL (UINT64_C(16) * 1024u * 1024u)
#define FB_CELL_WIDTH 16
#define FB_CELL_HEIGHT 16
#define FB_DEFAULT_MARGIN 8

static uint32_t screen_width;
static uint32_t screen_height;
static int32_t cursor_x;
static int32_t cursor_y;
static int cursor_initialized;

static int32_t screen_margin(uint32_t dimension) {
    return dimension >= (uint32_t)(FB_DEFAULT_MARGIN * 2 + FB_CELL_WIDTH)
        ? FB_DEFAULT_MARGIN : 0;
}

static int64_t update_screen_info(void) {
    struct welovec_fb_info info;
    const int64_t result = welovec_fb_get_info(&info);
    if (result < 0) return result;
    if (!info.width || !info.height || info.width > INT32_MAX ||
        info.height > INT32_MAX)
        return -22;

    if (!cursor_initialized || screen_width != info.width ||
        screen_height != info.height) {
        screen_width = info.width;
        screen_height = info.height;
        cursor_x = screen_margin(screen_width);
        cursor_y = screen_margin(screen_height);
        cursor_initialized = 1;
    }
    return 0;
}

static int64_t measure_text(const char *text, uint32_t *length) {
    if (!text || !length) return -22;
    for (uint32_t i = 0; i <= FB_TEXT_LIMIT; ++i) {
        if (text[i] == '\0') {
            *length = i;
            return 0;
        }
    }
    return -7;
}

static int64_t scroll_screen(uint32_t background) {
    if (!screen_width || !screen_height || screen_height > FB_MAX_PIXELS_PER_CALL)
        return -7;

    uint32_t rows = screen_height < FB_CELL_HEIGHT
        ? screen_height : FB_CELL_HEIGHT;
    const uint32_t max_width = (uint32_t)(FB_MAX_PIXELS_PER_CALL / screen_height);
    if (!max_width) return -7;

    for (uint32_t x = 0; x < screen_width;) {
        uint32_t chunk_width = screen_width - x;
        if (chunk_width > max_width) chunk_width = max_width;
        const int64_t result = welovec_fb_scroll_up(
            (int32_t)x, 0, (int32_t)chunk_width, (int32_t)screen_height,
            rows, background);
        if (result < 0) return result;
        x += chunk_width;
    }
    return 0;
}

static int64_t next_line(uint32_t background) {
    const int32_t margin = screen_margin(screen_width);
    cursor_x = margin;
    cursor_y += FB_CELL_HEIGHT;

    const int64_t bottom = (int64_t)screen_height - margin;
    if ((int64_t)cursor_y + FB_CELL_HEIGHT > bottom) {
        const int64_t result = scroll_screen(background);
        if (result < 0) {
            cursor_y -= FB_CELL_HEIGHT;
            return result;
        }
        cursor_y -= FB_CELL_HEIGHT;
        if (cursor_y < margin) cursor_y = margin;
    }
    return 0;
}

static int64_t print_text(const char *text, uint32_t foreground,
                          uint32_t background) {
    uint32_t length = 0;
    int64_t result = measure_text(text, &length);
    if (result < 0) return result;
    result = update_screen_info();
    if (result < 0) return result;

    const int32_t margin_x = screen_margin(screen_width);
    const int64_t right = (int64_t)screen_width - margin_x;
    for (uint32_t i = 0; i < length; ++i) {
        unsigned char character = (unsigned char)text[i];
        if (character == '\n') {
            result = next_line(background);
            if (result < 0) return result;
            continue;
        }
        if (character == '\r') {
            cursor_x = margin_x;
            continue;
        }
        if (character == '\b') {
            if (cursor_x > margin_x) cursor_x -= FB_CELL_WIDTH;
            result = welovec_fb_rect(cursor_x, cursor_y,
                                     FB_CELL_WIDTH, FB_CELL_HEIGHT, background);
            if (result < 0) return result;
            continue;
        }
        if (character == '\t') {
            const int32_t tab_width = FB_CELL_WIDTH * 4;
            const int64_t relative = (int64_t)cursor_x - margin_x;
            cursor_x = (int32_t)(((relative / tab_width) + 1) * tab_width + margin_x);
            if ((int64_t)cursor_x + FB_CELL_WIDTH > right) {
                result = next_line(background);
                if (result < 0) return result;
            }
            continue;
        }

        if ((int64_t)cursor_x + FB_CELL_WIDTH > right) {
            result = next_line(background);
            if (result < 0) return result;
        }
        if (character > 0x7f) character = '?';
        result = welovec_fb_char(cursor_x, cursor_y, character, foreground,
                                 background, 1);
        if (result < 0) return result;
        cursor_x += FB_CELL_WIDTH;
        if ((int64_t)cursor_x + FB_CELL_WIDTH > right) {
            result = next_line(background);
            if (result < 0) return result;
        }
    }
    return length;
}

int64_t welovec_printf_fb(const char *text) {
    return print_text(text, UINT32_C(0xffffffff), 0);
}

int64_t welovec_printf_fb_color(const char *text, uint32_t foreground) {
    return print_text(text, foreground, 0);
}

int64_t welovec_printf_fb_colors(const char *text, uint32_t foreground,
                                 uint32_t background) {
    return print_text(text, foreground, background);
}

int64_t welovec_clear_screen_color(uint32_t color) {
    struct welovec_fb_info info;
    int64_t result = welovec_fb_get_info(&info);
    if (result < 0) return result;
    if (!info.width || !info.height || info.width > INT32_MAX ||
        info.height > INT32_MAX)
        return -22;

    if (info.width <= FB_MAX_PIXELS_PER_CALL) {
        const uint32_t rows_per_call =
            (uint32_t)(FB_MAX_PIXELS_PER_CALL / info.width);
        for (uint32_t y = 0; y < info.height;) {
            uint32_t rows = info.height - y;
            if (rows > rows_per_call) rows = rows_per_call;
            result = welovec_fb_rect(0, (int32_t)y, (int32_t)info.width,
                                     (int32_t)rows, color);
            if (result < 0) return result;
            y += rows;
        }
    } else {
        for (uint32_t y = 0; y < info.height; ++y) {
            for (uint32_t x = 0; x < info.width;) {
                uint32_t columns = info.width - x;
                if (columns > FB_MAX_PIXELS_PER_CALL)
                    columns = (uint32_t)FB_MAX_PIXELS_PER_CALL;
                result = welovec_fb_rect((int32_t)x, (int32_t)y,
                                         (int32_t)columns, 1, color);
                if (result < 0) return result;
                x += columns;
            }
        }
    }

    screen_width = info.width;
    screen_height = info.height;
    cursor_x = screen_margin(screen_width);
    cursor_y = screen_margin(screen_height);
    cursor_initialized = 1;
    return 0;
}

int64_t welovec_clear_screen(void) {
    return welovec_clear_screen_color(UINT32_C(0xff000000));
}

int64_t welovec_fb_print_set_cursor(int32_t x, int32_t y) {
    int64_t result = update_screen_info();
    if (result < 0) return result;
    if (x < 0 || y < 0 || (uint32_t)x >= screen_width ||
        (uint32_t)y >= screen_height)
        return -22;
    cursor_x = x;
    cursor_y = y;
    return 0;
}

void welovec_fb_print_reset_cursor(void) {
    cursor_initialized = 0;
}

int64_t printf_fb(const char *text) { return welovec_printf_fb(text); }
int64_t printf_fb_color(const char *text, uint32_t foreground) {
    return welovec_printf_fb_color(text, foreground);
}
int64_t printf_fb_colors(const char *text, uint32_t foreground,
                         uint32_t background) {
    return welovec_printf_fb_colors(text, foreground, background);
}
int64_t welovec_print(const char *text, const char *font_name) {
    return welovec_user_syscall2(WELOVEC_SYS_PRINT,
                                 (uint64_t)(uintptr_t)text,
                                 (uint64_t)(uintptr_t)font_name);
}
int64_t welovec_print_color(const char *text, uint32_t foreground,
                            const char *font_name) {
    return welovec_user_syscall3(WELOVEC_SYS_PRINT_COLOR,
                                 (uint64_t)(uintptr_t)text, foreground,
                                 (uint64_t)(uintptr_t)font_name);
}
int64_t print(const char *text, const char *font_name) {
    return welovec_print(text, font_name);
}
int64_t print_color(const char *text, uint32_t foreground,
                    const char *font_name) {
    return welovec_print_color(text, foreground, font_name);
}
int64_t welovec_print_reset_cursor(void) {
    return welovec_user_syscall0(WELOVEC_SYS_PRINT_RESET_CURSOR);
}
int64_t clear_screen(void) { return welovec_clear_screen(); }
int64_t clear_screen_color(uint32_t color) {
    return welovec_clear_screen_color(color);
}
