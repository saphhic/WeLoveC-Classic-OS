// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Draws primitives and images for the desktop interface.

#include "desktop/include/desktop.h"
#include "user/framebuffer.h"

/*
 * Clipped drawing. The kernel only clips to the screen, and the desktop
 * repaints damaged rectangles on top of a framebuffer it cannot read back,
 * so every primitive is clipped to the current repaint region *before* the
 * syscall is issued.
 *
 * Text uses the kernel's 16x16 cells (one syscall per run of whole cells). A
 * cell that the clip rectangle cuts through is drawn from the embedded 8x8
 * font instead, as 2x2-pixel runs, so a glyph never spills outside the region.
 */

extern const uint8_t g_font8x8[95][8];

static rect_t g_clip;

void gfx_init(void) { gfx_clip_screen(); }

void gfx_clip(rect_t r) {
    g_clip = rect_intersect(r, rect_make(0, 0, g_desk.width, g_desk.height));
}

void gfx_clip_screen(void) {
    g_clip = rect_make(0, 0, g_desk.width, g_desk.height);
}

rect_t gfx_clip_get(void) { return g_clip; }

void gfx_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    const rect_t r = rect_intersect(rect_make(x, y, w, h), g_clip);
    if (rect_empty(r)) return;
    (void)welovec_fb_rect(r.x, r.y, r.w, r.h, color);
}

void gfx_frame(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (w < 2 || h < 2) {
        gfx_fill(x, y, w, h, color);
        return;
    }
    gfx_fill(x, y, w, 1, color);
    gfx_fill(x, y + h - 1, w, 1, color);
    gfx_fill(x, y + 1, 1, h - 2, color);
    gfx_fill(x + w - 1, y + 1, 1, h - 2, color);
}

static void glyph_clipped(int32_t x, int32_t y, uint8_t c, uint32_t color) {
    if (c < 32 || c > 126) return;
    const uint8_t *glyph = g_font8x8[c - 32];
    for (int32_t row = 0; row < 8; ++row) {
        const int32_t ry = y + row * 2;
        if (ry + 2 <= g_clip.y || ry >= g_clip.y + g_clip.h) continue;
        const uint8_t bits = glyph[row];
        int32_t col = 0;
        while (col < 8) {
            if (!(bits & (1u << col))) {
                ++col;
                continue;
            }
            const int32_t start = col;
            while (col < 8 && (bits & (1u << col))) ++col;
            gfx_fill(x + start * 2, ry, (col - start) * 2, 2, color);
        }
    }
}

void gfx_text_n(int32_t x, int32_t y, const char *s, uint32_t n,
                uint32_t color) {
    if (!s || !n) return;
    if (y + FONT_H <= g_clip.y || y >= g_clip.y + g_clip.h) return;
    if (x >= g_clip.x + g_clip.w || x + (int32_t)n * FONT_W <= g_clip.x) return;

    int32_t first = 0;
    if (g_clip.x > x) first = (g_clip.x - x) / FONT_W;
    int32_t last = (g_clip.x + g_clip.w - 1 - x) / FONT_W;
    if (last >= (int32_t)n) last = (int32_t)n - 1;

    const int vertical_ok =
        y >= g_clip.y && y + FONT_H <= g_clip.y + g_clip.h;
    char run[129];
    uint32_t run_len = 0;
    int32_t run_x = 0;

    for (int32_t i = first; i <= last; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!c) break;
        if (c < 32) c = ' ';
        else if (c > 126) c = '?';
        const int32_t cx = x + i * FONT_W;
        const int inside = vertical_ok && cx >= g_clip.x &&
                           cx + FONT_W <= g_clip.x + g_clip.w;
        if (inside) {
            if (!run_len) run_x = cx;
            run[run_len++] = (char)c;
            if (run_len == 128) {
                run[run_len] = '\0';
                (void)welovec_fb_text(run_x, y, run, color);
                run_len = 0;
            }
        } else {
            if (run_len) {
                run[run_len] = '\0';
                (void)welovec_fb_text(run_x, y, run, color);
                run_len = 0;
            }
            glyph_clipped(cx, y, c, color);
        }
    }
    if (run_len) {
        run[run_len] = '\0';
        (void)welovec_fb_text(run_x, y, run, color);
    }
}

void gfx_text(int32_t x, int32_t y, const char *s, uint32_t color) {
    gfx_text_n(x, y, s, d_strlen(s), color);
}
