// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the framebuffer drawing and text API for user programs.

#ifndef WELOVEC_USER_FRAMEBUFFER_H
#define WELOVEC_USER_FRAMEBUFFER_H

#include <stdint.h>
#include "user/syscall.h"

struct welovec_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch_bytes;
    uint32_t bytes_per_pixel;
};

struct welovec_fb_text_cursor {
    int32_t x;
    int32_t y;
};

static inline int64_t welovec_print_cursor_get(
    struct welovec_fb_text_cursor *cursor) {
    return welovec_user_syscall1(WELOVEC_SYS_PRINT_CURSOR_GET,
                                 (uint64_t)(uintptr_t)cursor);
}

static inline int64_t welovec_print_cursor_set(int32_t x, int32_t y) {
    return welovec_user_syscall2(WELOVEC_SYS_PRINT_CURSOR_SET,
                                 (uint64_t)(int64_t)x,
                                 (uint64_t)(int64_t)y);
}

/* Returns 1 if the view changed; 0 when no older/newer page is available.
 * direction: 1 shows older output, -1 moves toward live output, 0 restores live. */
static inline int64_t welovec_print_scrollback(int32_t direction) {
    return welovec_user_syscall1(WELOVEC_SYS_PRINT_SCROLLBACK,
                                 (uint64_t)(int64_t)direction);
}

static inline uint32_t welovec_fb_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    return UINT32_C(0xff000000) | ((uint32_t)red << 16) |
           ((uint32_t)green << 8) | blue;
}

static inline int64_t welovec_fb_get_info(struct welovec_fb_info *info) {
    return welovec_user_syscall1(WELOVEC_SYS_FB_INFO, (uint64_t)(uintptr_t)info);
}

/* Draw a damaged screen region off-screen, then publish it with frame_end. */
static inline int64_t welovec_fb_frame_begin(int32_t x, int32_t y,
                                             int32_t width, int32_t height) {
    return welovec_user_syscall4(WELOVEC_SYS_FB_FRAME_BEGIN,
                                 (uint64_t)(int64_t)x,
                                 (uint64_t)(int64_t)y,
                                 (uint64_t)(int64_t)width,
                                 (uint64_t)(int64_t)height);
}

static inline int64_t welovec_fb_frame_end(void) {
    return welovec_user_syscall0(WELOVEC_SYS_FB_FRAME_END);
}

static inline int64_t welovec_fb_pixel(int32_t x, int32_t y, uint32_t color) {
    return welovec_user_syscall3(WELOVEC_SYS_FB_PIXEL, (uint64_t)(int64_t)x,
                                 (uint64_t)(int64_t)y, color);
}

static inline int64_t welovec_fb_rect(int32_t x, int32_t y, int32_t width,
                                      int32_t height, uint32_t color) {
    return welovec_user_syscall5(WELOVEC_SYS_FB_RECT, (uint64_t)(int64_t)x,
                                 (uint64_t)(int64_t)y, (uint64_t)(int64_t)width,
                                 (uint64_t)(int64_t)height, color);
}

static inline int64_t welovec_fb_rect_alpha(int32_t x, int32_t y,
                                            int32_t width, int32_t height,
                                            uint32_t color, uint8_t alpha) {
    return welovec_user_syscall6(WELOVEC_SYS_FB_RECT_ALPHA,
                                 (uint64_t)(int64_t)x, (uint64_t)(int64_t)y,
                                 (uint64_t)(int64_t)width,
                                 (uint64_t)(int64_t)height, color, alpha);
}

static inline int64_t welovec_fb_gradient(int32_t x, int32_t y,
                                          int32_t width, int32_t height,
                                          uint32_t top, uint32_t bottom) {
    return welovec_user_syscall6(WELOVEC_SYS_FB_GRADIENT,
                                 (uint64_t)(int64_t)x, (uint64_t)(int64_t)y,
                                 (uint64_t)(int64_t)width,
                                 (uint64_t)(int64_t)height, top, bottom);
}

static inline int64_t welovec_fb_outline(int32_t x, int32_t y,
                                         int32_t width, int32_t height,
                                         uint32_t color) {
    return welovec_user_syscall5(WELOVEC_SYS_FB_OUTLINE,
                                 (uint64_t)(int64_t)x, (uint64_t)(int64_t)y,
                                 (uint64_t)(int64_t)width,
                                 (uint64_t)(int64_t)height, color);
}

static inline int64_t welovec_fb_line(int32_t x0, int32_t y0,
                                      int32_t x1, int32_t y1,
                                      uint32_t color) {
    return welovec_user_syscall5(WELOVEC_SYS_FB_LINE,
                                 (uint64_t)(int64_t)x0, (uint64_t)(int64_t)y0,
                                 (uint64_t)(int64_t)x1, (uint64_t)(int64_t)y1,
                                 color);
}

static inline int64_t welovec_fb_circle(int32_t center_x, int32_t center_y,
                                        int32_t radius, uint32_t color) {
    return welovec_user_syscall4(WELOVEC_SYS_FB_CIRCLE,
                                 (uint64_t)(int64_t)center_x,
                                 (uint64_t)(int64_t)center_y,
                                 (uint64_t)(int64_t)radius, color);
}

static inline int64_t welovec_fb_fill_circle(int32_t center_x,
                                             int32_t center_y,
                                             int32_t radius,
                                             uint32_t color) {
    return welovec_user_syscall4(WELOVEC_SYS_FB_FILL_CIRCLE,
                                 (uint64_t)(int64_t)center_x,
                                 (uint64_t)(int64_t)center_y,
                                 (uint64_t)(int64_t)radius, color);
}

static inline int64_t welovec_fb_char(int32_t x, int32_t y, uint8_t character,
                                      uint32_t foreground, uint32_t background,
                                      uint32_t scale) {
    return welovec_user_syscall6(WELOVEC_SYS_FB_CHAR,
                                 (uint64_t)(int64_t)x, (uint64_t)(int64_t)y,
                                 character, foreground, background, scale);
}

static inline int64_t welovec_fb_text(int32_t x, int32_t y,
                                      const char *text, uint32_t color) {
    return welovec_user_syscall4(WELOVEC_SYS_FB_TEXT,
                                 (uint64_t)(int64_t)x, (uint64_t)(int64_t)y,
                                 (uint64_t)(uintptr_t)text, color);
}

static inline int64_t welovec_fb_scroll_up(int32_t x, int32_t y,
                                           int32_t width, int32_t height,
                                           uint32_t rows, uint32_t fill_color) {
    return welovec_user_syscall6(WELOVEC_SYS_FB_SCROLL_UP,
                                 (uint64_t)(int64_t)x, (uint64_t)(int64_t)y,
                                 (uint64_t)(int64_t)width,
                                 (uint64_t)(int64_t)height, rows, fill_color);
}

/* Blit a child process' window framebuffer into the caller's framebuffer. */
static inline int64_t welovec_fb_surface_blit(
    uint64_t process_id, int32_t dest_x, int32_t dest_y,
    int32_t source_x, int32_t source_y, uint32_t width, uint32_t height) {
    const uint64_t dimensions = ((uint64_t)width << 32) | height;
    return welovec_user_syscall6(
        WELOVEC_SYS_FB_SURFACE_BLIT, process_id,
        (uint64_t)(int64_t)dest_x, (uint64_t)(int64_t)dest_y,
        (uint64_t)(int64_t)source_x, (uint64_t)(int64_t)source_y, dimensions);
}

static inline int64_t welovec_fb_surface_resize(
    uint64_t process_id, uint32_t width, uint32_t height) {
    return welovec_user_syscall3(WELOVEC_SYS_FB_SURFACE_RESIZE, process_id,
                                 width, height);
}

static inline uint64_t welovec_fb_surface_generation(uint64_t process_id) {
    return (uint64_t)welovec_user_syscall1(
        WELOVEC_SYS_FB_SURFACE_GENERATION, process_id);
}

#endif
