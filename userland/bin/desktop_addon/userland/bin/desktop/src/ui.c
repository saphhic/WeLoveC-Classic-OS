// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Contains shared desktop interface utilities.

#include "desktop/include/desktop.h"

/* Stateless drawing helpers shared by the apps. Hit-testing stays in the apps. */

int32_t ui_button_w(const char *label) {
    return (int32_t)d_strlen(label) * FONT_W + 16;
}

void ui_button(rect_t r, const char *label, int enabled) {
    gfx_fill(r.x, r.y, r.w, r.h, C_BTN);
    gfx_fill(r.x, r.y, r.w, 1, C_WHITE);
    gfx_fill(r.x, r.y, 1, r.h, C_WHITE);
    gfx_fill(r.x, r.y + r.h - 1, r.w, 1, C_SHADOW);
    gfx_fill(r.x + r.w - 1, r.y, 1, r.h, C_SHADOW);
    const int32_t tw = (int32_t)d_strlen(label) * FONT_W;
    gfx_text(r.x + (r.w - tw) / 2, r.y + (r.h - FONT_H) / 2, label,
             enabled ? C_TEXT : C_SHADOW);
}

void ui_sunken(rect_t r, uint32_t background) {
    gfx_fill(r.x, r.y, r.w, r.h, background);
    gfx_frame(r.x, r.y, r.w, r.h, C_SHADOW);
}

void ui_scrollbar(rect_t track, int32_t top, int32_t visible, int32_t total) {
    gfx_fill(track.x, track.y, track.w, track.h, C_BTN);
    if (total <= visible || total <= 0 || track.h < 8) return;
    int32_t thumb = (int32_t)((int64_t)track.h * visible / total);
    if (thumb < 12) thumb = 12;
    if (thumb > track.h) thumb = track.h;
    const int32_t span = total - visible;
    const int32_t pos =
        span > 0 ? (int32_t)((int64_t)(track.h - thumb) * top / span) : 0;
    gfx_fill(track.x + 1, track.y + pos, track.w - 2, thumb, C_SHADOW);
}

void ui_text_right(int32_t right, int32_t y, const char *s, uint32_t color) {
    gfx_text(right - (int32_t)d_strlen(s) * FONT_W, y, s, color);
}
