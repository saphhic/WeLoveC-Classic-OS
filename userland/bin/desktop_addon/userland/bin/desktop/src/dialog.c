// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements desktop dialog boxes.

#include "desktop/include/desktop.h"

/*
 * One modal dialog at a time, drawn above everything else. While it is open the
 * window system routes all keyboard and mouse input here. The callback runs
 * only on OK, after the dialog has been dismissed, so it may open another one.
 */

enum { MODE_NONE = 0, MODE_MESSAGE, MODE_CONFIRM, MODE_PROMPT };
#define DLG_TEXT_MAX 128

static struct {
    int mode;
    char title[160];
    char text[DLG_TEXT_MAX];
    uint32_t len;
    dlg_callback done;
    void *context;
} g_dlg;

static rect_t dlg_rect(void) {
    int32_t w = 480;
    if (w > g_desk.width - 24) w = g_desk.width - 24;
    const int32_t h = g_dlg.mode == MODE_PROMPT ? 150 : 128;
    return rect_make((g_desk.width - w) / 2, (g_desk.work.h - h) / 2, w, h);
}

static rect_t ok_rect(rect_t d) {
    return rect_make(d.x + d.w - 2 * 88 - 24, d.y + d.h - 36, 80, 26);
}

static rect_t cancel_rect(rect_t d) {
    return rect_make(d.x + d.w - 88 - 12, d.y + d.h - 36, 80, 26);
}

static void open_common(int mode, const char *title) {
    g_dlg.mode = mode;
    d_strcpy(g_dlg.title, sizeof(g_dlg.title), title);
    g_dlg.len = 0;
    g_dlg.text[0] = '\0';
    g_dlg.done = 0;
    g_dlg.context = 0;
}

void dlg_prompt(const char *title, const char *initial, dlg_callback done,
                void *context) {
    open_common(MODE_PROMPT, title);
    d_strcpy(g_dlg.text, sizeof(g_dlg.text), initial ? initial : "");
    g_dlg.len = d_strlen(g_dlg.text);
    g_dlg.done = done;
    g_dlg.context = context;
    wm_repaint(dlg_rect());
}

void dlg_confirm(const char *text, dlg_callback done, void *context) {
    open_common(MODE_CONFIRM, text);
    g_dlg.done = done;
    g_dlg.context = context;
    wm_repaint(dlg_rect());
}

void dlg_message(const char *text) {
    open_common(MODE_MESSAGE, text);
    wm_repaint(dlg_rect());
}

int dlg_active(void) { return g_dlg.mode != MODE_NONE; }

static void finish(int accepted) {
    const rect_t area = dlg_rect();
    const int mode = g_dlg.mode;
    dlg_callback done = g_dlg.done;
    void *context = g_dlg.context;
    char text[DLG_TEXT_MAX];
    d_strcpy(text, sizeof(text), g_dlg.text);
    g_dlg.mode = MODE_NONE;
    g_dlg.done = 0;
    wm_repaint(area);
    if (accepted && done && mode != MODE_MESSAGE) done(context, text);
}

void dlg_key(int key) {
    if (key == 0x1b) {
        finish(0);
    } else if (key == '\n') {
        finish(1);
    } else if (g_dlg.mode == MODE_PROMPT) {
        if ((key == '\b' || key == 0x7f) && g_dlg.len) {
            g_dlg.text[--g_dlg.len] = '\0';
        } else if (key >= 32 && key < 127 && g_dlg.len + 1 < DLG_TEXT_MAX) {
            g_dlg.text[g_dlg.len++] = (char)key;
            g_dlg.text[g_dlg.len] = '\0';
        } else {
            return;
        }
        wm_repaint(dlg_rect());
    }
}

void dlg_mouse(const struct mouse_event *e) {
    if (e->type != MOUSE_DOWN) return;
    const rect_t d = dlg_rect();
    if (g_dlg.mode == MODE_MESSAGE) {
        const rect_t b = rect_make(d.x + d.w - 92, d.y + d.h - 36, 80, 26);
        if (rect_hit(b, e->x, e->y)) finish(1);
        return;
    }
    if (rect_hit(ok_rect(d), e->x, e->y)) finish(1);
    else if (rect_hit(cancel_rect(d), e->x, e->y)) finish(0);
}

void dlg_draw(rect_t area) {
    if (g_dlg.mode == MODE_NONE) return;
    const rect_t d = dlg_rect();
    const rect_t clip = rect_intersect(area, d);
    if (rect_empty(clip)) return;
    gfx_clip(clip);
    gfx_fill(d.x, d.y, d.w, d.h, C_WIN_BG);
    gfx_frame(d.x, d.y, d.w, d.h, C_BORDER);
    gfx_fill(d.x + 1, d.y + 1, d.w - 2, 4, C_ACCENT);

    /* Wrap the title/message at the box width, at most three lines. */
    const int32_t cols = (d.w - 24) / FONT_W;
    const uint32_t total = d_strlen(g_dlg.title);
    const int32_t max_lines = g_dlg.mode == MODE_PROMPT ? 2 : 3;
    for (int32_t line = 0; line < max_lines; ++line) {
        const uint32_t from = (uint32_t)(line * cols);
        if (from >= total) break;
        uint32_t n = total - from;
        if (n > (uint32_t)cols) n = (uint32_t)cols;
        gfx_text_n(d.x + 12, d.y + 14 + line * 20, g_dlg.title + from, n, C_TEXT);
    }

    if (g_dlg.mode == MODE_PROMPT) {
        const rect_t box = rect_make(d.x + 12, d.y + 58, d.w - 24, 28);
        ui_sunken(box, C_WHITE);
        const int32_t vis = (box.w - 12) / FONT_W - 1;
        uint32_t start = 0;
        if ((int32_t)g_dlg.len > vis) start = g_dlg.len - (uint32_t)vis;
        gfx_text(box.x + 6, box.y + 6, g_dlg.text + start, C_TEXT);
        gfx_fill(box.x + 6 + (int32_t)(g_dlg.len - start) * FONT_W, box.y + 5, 2,
                 18, C_ACCENT);
    }
    if (g_dlg.mode == MODE_MESSAGE) {
        ui_button(rect_make(d.x + d.w - 92, d.y + d.h - 36, 80, 26), "OK", 1);
    } else {
        ui_button(ok_rect(d), "OK", 1);
        ui_button(cancel_rect(d), "Esc", 1);
    }
}
