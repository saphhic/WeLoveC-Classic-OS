// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for framebuffer in the display service module.

#ifndef WELOVEC_FRAMEBUFFER_HPP
#define WELOVEC_FRAMEBUFFER_HPP

#include <stdbool.h>
#include <stdint.h>
#include "boot/boot_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uintptr_t FRAMEBUFFER_ADDR;
extern uint32_t FRAMEBUFFER_WIDTH;
extern uint32_t FRAMEBUFFER_HEIGHT;
extern uint32_t FRAMEBUFFER_PITCH;

extern uint32_t *framebuffer;
extern uint32_t width;
extern uint32_t height;
extern uint32_t pitch;

typedef struct WelovecFramebufferSurface {
    uint32_t *pixels;
    uint32_t width, height, pitch;
    int cursor_x, cursor_y, scale;
    uint32_t foreground, background;
    uint64_t generation;
    uint32_t references;
} WelovecFramebufferSurface;

typedef struct WelovecFramebufferTargetState {
    WelovecFramebufferSurface *surface;
    uint32_t *framebuffer;
    uint32_t width, height, pitch;
    int cursor_x, cursor_y, scale;
    uint32_t foreground, background;
    uint64_t lock_state;
    bool active;
} WelovecFramebufferTargetState;

typedef struct WelovecFramebufferRedirectState {
    uint32_t *framebuffer;
    uint64_t interrupt_flags;
    bool active;
} WelovecFramebufferRedirectState;

enum welovec_framebuffer_font {
    WELOVEC_FB_FONT_SYSTEM16 = 0,
    WELOVEC_FB_FONT_SYSTEM8 = 1,
    WELOVEC_FB_FONT_UBUNTU = 2
};

bool framebuffer_init(const WelovecBootInfo *boot_info);
uint32_t fb_color(uint8_t r, uint8_t g, uint8_t b);
bool framebuffer_available(void);
bool framebuffer_get_resolution(uint32_t *out_width, uint32_t *out_height);
bool framebuffer_surface_create(WelovecFramebufferSurface *surface,
                               uint32_t surface_width, uint32_t surface_height);
bool framebuffer_surface_resize(WelovecFramebufferSurface *surface,
                                uint32_t surface_width, uint32_t surface_height);
void framebuffer_surface_destroy(WelovecFramebufferSurface *surface);
bool framebuffer_surface_begin(WelovecFramebufferSurface *surface,
                               WelovecFramebufferTargetState *state);
void framebuffer_surface_end(WelovecFramebufferTargetState *state);
bool framebuffer_surface_blit(const WelovecFramebufferSurface *surface,
                              int32_t dest_x, int32_t dest_y,
                              int32_t source_x, int32_t source_y,
                              int32_t blit_width, int32_t blit_height);
uint64_t framebuffer_surface_generation(
    const WelovecFramebufferSurface *surface);
bool framebuffer_frame_begin(uint64_t owner, int32_t x, int32_t y,
                             int32_t frame_width, int32_t frame_height);
bool framebuffer_frame_end(uint64_t owner);
void framebuffer_frame_cancel_owner(uint64_t owner);
bool framebuffer_frame_redirect_begin(
    uint64_t owner, WelovecFramebufferRedirectState *state);
void framebuffer_frame_redirect_end(WelovecFramebufferRedirectState *state);
void fb_set_scale(int scale);
void fb_set_colors(uint32_t fg, uint32_t bg);
int fb_get_cursor_x(void);
int fb_get_cursor_y(void);
void fb_set_cursor(int x, int y);
bool fb_scrollback_page(int direction);
void fb_scrollback_return_live(void);

#define FB_BLACK       fb_color(0,   0,   0  )
#define FB_WHITE       fb_color(255, 255, 255)
#define FB_RED         fb_color(220, 50,  50 )
#define FB_GREEN       fb_color(50,  200, 80 )
#define FB_BLUE        fb_color(60,  120, 220)
#define FB_YELLOW      fb_color(240, 220, 60 )
#define FB_CYAN        fb_color(60,  200, 220)
#define FB_MAGENTA     fb_color(200, 60,  200)
#define FB_ORANGE      fb_color(240, 140, 40 )
#define FB_GRAY        fb_color(128, 128, 128)
#define FB_DARK_GRAY   fb_color(64,  64,  64 )
#define FB_LIGHT_GRAY  fb_color(192, 192, 192)
#define FB_PURPLE      fb_color(120, 60,  200)
#define FB_PINK        fb_color(220, 120, 180)
#define FB_LIME        fb_color(120, 220, 60 )
#define FB_BROWN       fb_color(140, 80,  40 )

void put_pixel(int x, int y, uint32_t color);
void draw_rect(int x, int y, int w, int h, uint32_t color);
void draw_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
void draw_rect_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void draw_rect_outline(int x, int y, int w, int h, uint32_t color);
void draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void draw_circule(int cx, int cy, int r, uint32_t color);
void draw_filled_circle(int cx, int cy, int r, uint32_t color);
void draw_char(int px, int py, char c, uint32_t fg, uint32_t bg, int scale);
void draw_char_font(int px, int py, char c, uint32_t fg, uint32_t bg,
                    int scale, uint32_t font);
void fb_scroll_up_region(int x, int y, int w, int h, int rows, uint32_t fill_color);
void print(int x, int y, const char *str, uint32_t color);
void putchar_fb(char c);
void putchar_fb_font(char c, uint32_t color, uint32_t font);
void print_fb_font(const char *text, uint32_t color, uint32_t font);
void fb_reset_print_cursor(void);
bool framebuffer_font_valid(uint32_t font);
void fb_clear_screen_with_color(uint32_t color);
void fb_clear_screen(void);
void printf_fb(const char *str);
void printf_fb_color(const char *str, uint32_t color);
void printf_fb_s(const char *fmt, const char *arg);
void printf_fb_d(const char *fmt, int32_t arg);

#ifdef __cplusplus
}
#endif

#endif
