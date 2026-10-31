// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares text input and output for user programs.

#ifndef WELOVEC_USER_STDIO_H
#define WELOVEC_USER_STDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Text output returns the number of input bytes, or a negative errno. */
int64_t welovec_printf_fb(const char *text);
int64_t welovec_printf_fb_color(const char *text, uint32_t foreground);
int64_t welovec_printf_fb_colors(const char *text, uint32_t foreground,
                                 uint32_t background);

/* Font names accepted by print() and print_color(). NULL selects system16. */
#define WELOVEC_FONT_SYSTEM16 "system16"
#define WELOVEC_FONT_SYSTEM8 "system8"
#define WELOVEC_FONT_UBUNTU "ubuntu"
#define WELOVEC_FONT_DEFAULT ((const char *)0)
int64_t welovec_print(const char *text, const char *font_name);
int64_t welovec_print_color(const char *text, uint32_t foreground,
                            const char *font_name);
/* Reset the shared kernel print()/print_color() cursor to the screen margin. */
int64_t welovec_print_reset_cursor(void);

/* Clear the shared framebuffer to opaque black or the supplied ARGB color. */
int64_t welovec_clear_screen(void);
int64_t welovec_clear_screen_color(uint32_t color);

/* Set the cursor in pixels; reset makes the next print start at the margin. */
int64_t welovec_fb_print_set_cursor(int32_t x, int32_t y);
void welovec_fb_print_reset_cursor(void);

/* Short compatibility names for small C programs and terminal code. */
int64_t printf_fb(const char *text);
int64_t printf_fb_color(const char *text, uint32_t foreground);
int64_t printf_fb_colors(const char *text, uint32_t foreground,
                         uint32_t background);
int64_t print(const char *text, const char *font_name);
int64_t print_color(const char *text, uint32_t foreground,
                    const char *font_name);
int64_t clear_screen(void);
int64_t clear_screen_color(uint32_t color);

#ifdef __cplusplus
}
#endif

#endif
