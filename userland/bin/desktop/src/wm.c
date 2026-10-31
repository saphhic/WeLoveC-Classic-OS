// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Manages desktop windows, focus, movement, and resizing.

#include "desktop/include/desktop.h"
#include "user/time.h"

/*
 * Window manager. Windows are plain rectangles in a z-ordered list; their
 * content is produced on demand by the owning app's draw() callback, so no
 * pixels are ever stored. Moving a window drags an outline and only repaints
 * the strips it leaves behind; the window itself is repainted once on release.
 */

#define BTN_W 22
#define BTN_H 18
#define RESIZE_GRIP 16
#define START_W 96
#define CLOCK_CHARS 9

static struct window g_win[WIN_MAX];
static uint8_t g_state[WIN_MAX][APP_STATE_MAX] __attribute__((aligned(16)));
static uint8_t g_z[WIN_MAX];          /* z-order of slot indices, [0] = bottom */
static uint32_t g_zn;
static struct window *g_focus;
static struct window *g_capture;

static struct {
    int active;
    int resize;
    struct window *win;
    int32_t off_x, off_y;
    rect_t ghost;
} g_drag;

static char g_clock[CLOCK_CHARS + 1] = "--:-- UTC";

/* ------------------------------------------------------------------ helpers */

static rect_t screen_rect(void) {
    return rect_make(0, 0, g_desk.width, g_desk.height);
}

static int z_index(const struct window *w) {
    for (uint32_t i = 0; i < g_zn; ++i)
        if (&g_win[g_z[i]] == w) return (int)i;
    return -1;
}

static void z_remove(const struct window *w) {
    const int at = z_index(w);
    if (at < 0) return;
    for (uint32_t i = (uint32_t)at; i + 1 < g_zn; ++i) g_z[i] = g_z[i + 1];
    --g_zn;
}

static void z_push(struct window *w) {
    z_remove(w);
    g_z[g_zn++] = (uint8_t)(w - g_win);
}

rect_t wm_client(const struct window *w) {
    return rect_make(w->r.x + BORDER, w->r.y + 1 + TITLE_H, w->r.w - 2 * BORDER,
                     w->r.h - (1 + TITLE_H) - BORDER);
}

static rect_t title_rect(const struct window *w) {
    return rect_make(w->r.x, w->r.y, w->r.w, 1 + TITLE_H);
}

/* which: 0 close, 1 maximise, 2 minimise */
static rect_t button_rect(const struct window *w, int which) {
    return rect_make(w->r.x + w->r.w - 1 - 4 - BTN_W * (which + 1) - 2 * which,
                     w->r.y + 1 + (TITLE_H - BTN_H) / 2, BTN_W, BTN_H);
}

static rect_t resize_handle_rect(const struct window *w) {
    return rect_make(w->r.x + w->r.w - RESIZE_GRIP,
                     w->r.y + w->r.h - RESIZE_GRIP,
                     RESIZE_GRIP, RESIZE_GRIP);
}

static void paint_resize_grip(const struct window *w, rect_t clip) {
    if (w->maximized) return;
    const rect_t handle = rect_intersect(resize_handle_rect(w), clip);
    if (rect_empty(handle)) return;
    gfx_clip(clip);
    for (int32_t line = 0; line < 3; ++line) {
        const int32_t inset = 5 + line * 4;
        gfx_fill(w->r.x + w->r.w - inset - 1,
                 w->r.y + w->r.h - 2, inset - 3, 1, C_SHADOW);
        gfx_fill(w->r.x + w->r.w - 2,
                 w->r.y + w->r.h - inset - 1, 1, inset - 3, C_SHADOW);
    }
}

static struct window *top_visible(void) {
    for (int i = (int)g_zn - 1; i >= 0; --i)
        if (!g_win[g_z[i]].minimized) return &g_win[g_z[i]];
    return 0;
}

static struct window *window_at(int32_t x, int32_t y) {
    for (int i = (int)g_zn - 1; i >= 0; --i) {
        struct window *w = &g_win[g_z[i]];
        if (!w->minimized && rect_hit(w->r, x, y)) return w;
    }
    return 0;
}

uint32_t wm_window_count(void) { return g_zn; }

void wm_set_title(struct window *w, const char *title) {
    d_strcpy(w->title, sizeof(w->title), title);
}

/* ------------------------------------------------------------------ painting */

static void paint_window(const struct window *w, rect_t area) {
    const rect_t clip = rect_intersect(area, w->r);
    if (rect_empty(clip)) return;
    gfx_clip(clip);
    const int active = (w == g_focus);
    const uint32_t tc = active ? C_TITLE_ON : C_TITLE_OFF;

    gfx_fill(w->r.x, w->r.y, w->r.w, w->r.h, tc);
    gfx_frame(w->r.x, w->r.y, w->r.w, w->r.h, C_BORDER);

    const int32_t room = w->r.w - 12 - 3 * (BTN_W + 2) - 8;
    const int32_t chars = room > 0 ? room / FONT_W : 0;
    gfx_text_n(w->r.x + 8, w->r.y + 1 + (TITLE_H - FONT_H) / 2, w->title,
               (uint32_t)chars, C_WHITE);

    for (int b = 0; b < 3; ++b) {
        const rect_t br = button_rect(w, b);
        const uint32_t bg = active ? C_ACCENT : C_PANEL_HI;
        gfx_fill(br.x, br.y, br.w, br.h, b == 0 ? (active ? C_ERR : C_PANEL_HI) : bg);
        const int32_t gx = br.x + 6, gy = br.y + 4;
        if (b == 0) {
            for (int32_t i = 0; i < 10; ++i) {
                gfx_fill(gx + i, gy + i, 2, 1, C_WHITE);
                gfx_fill(gx + 9 - i, gy + i, 2, 1, C_WHITE);
            }
        } else if (b == 1) {
            gfx_frame(gx, gy, 10, 10, C_WHITE);
            gfx_fill(gx, gy, 10, 2, C_WHITE);
        } else {
            gfx_fill(gx, gy + 7, 10, 2, C_WHITE);
        }
    }

    const rect_t client = wm_client(w);
    if (!w->ops->surface_backed)
        gfx_fill(client.x, client.y, client.w, client.h, C_WIN_BG);
    gfx_clip(rect_intersect(clip, client));
    if (w->ops->draw) w->ops->draw((struct window *)w, client);
    paint_resize_grip(w, clip);
}

static void ghost_strips(rect_t g, rect_t out[4]) {
    out[0] = rect_make(g.x, g.y, g.w, 2);
    out[1] = rect_make(g.x, g.y + g.h - 2, g.w, 2);
    out[2] = rect_make(g.x, g.y + 2, 2, g.h - 4);
    out[3] = rect_make(g.x + g.w - 2, g.y + 2, 2, g.h - 4);
}

static void paint_ghost(rect_t area) {
    if (!g_drag.active) return;
    rect_t strips[4];
    ghost_strips(g_drag.ghost, strips);
    for (int i = 0; i < 4; ++i) {
        gfx_clip(rect_intersect(area, strips[i]));
        gfx_fill(strips[i].x, strips[i].y, strips[i].w, strips[i].h, C_WHITE);
    }
}

static rect_t clock_rect(void) {
    const int32_t w = CLOCK_CHARS * FONT_W + 16;
    return rect_make(g_desk.width - w, g_desk.taskbar.y, w, TASKBAR_H);
}

static rect_t task_button_rect(uint32_t nth, uint32_t total) {
    const int32_t left = START_W + 12;
    const int32_t avail = clock_rect().x - left - 8;
    int32_t bw = total ? avail / (int32_t)total - 4 : 0;
    if (bw > 176) bw = 176;
    if (bw < 32) bw = 32;
    return rect_make(left + (int32_t)nth * (bw + 4), g_desk.taskbar.y + 3, bw,
                     TASKBAR_H - 6);
}

static void paint_taskbar(rect_t area) {
    const rect_t tb = g_desk.taskbar;
    const rect_t clip = rect_intersect(area, tb);
    if (rect_empty(clip)) return;
    gfx_clip(clip);
    gfx_fill(tb.x, tb.y, tb.w, tb.h, C_PANEL);
    gfx_fill(tb.x, tb.y, tb.w, 1, C_PANEL_HI);

    const int open = menu_is_open();
    gfx_fill(4, tb.y + 3, START_W, TASKBAR_H - 6, open ? C_ACCENT_DK : C_ACCENT);
    gfx_text(4 + (START_W - 5 * FONT_W) / 2, tb.y + 8, "Start", C_WHITE);

    uint32_t total = 0;
    for (uint32_t i = 0; i < WIN_MAX; ++i) total += g_win[i].used;
    uint32_t nth = 0;
    for (uint32_t i = 0; i < WIN_MAX; ++i) {
        if (!g_win[i].used) continue;
        const rect_t br = task_button_rect(nth++, total);
        const int on = (&g_win[i] == g_focus && !g_win[i].minimized);
        gfx_fill(br.x, br.y, br.w, br.h, on ? C_ACCENT_DK : C_PANEL_HI);
        const int32_t chars = (br.w - 12) / FONT_W;
        gfx_text_n(br.x + 6, br.y + (br.h - FONT_H) / 2, g_win[i].title,
                   chars > 0 ? (uint32_t)chars : 0,
                   g_win[i].minimized ? C_SHADOW : C_WHITE);
    }
    const rect_t ck = clock_rect();
    gfx_text(ck.x + 8, tb.y + 8, g_clock, C_WHITE);
}

void wm_taskbar_repaint(void) { wm_repaint(g_desk.taskbar); }

void wm_repaint(rect_t area) {
    area = rect_intersect(area, screen_rect());
    if (rect_empty(area)) return;
    gfx_clip(area);
    gfx_fill(area.x, area.y, area.w, area.h, C_DESKTOP);
    icons_draw(area);
    for (uint32_t i = 0; i < g_zn; ++i) {
        const struct window *w = &g_win[g_z[i]];
        const int hidden_dragged_window = g_drag.active && !g_drag.resize &&
            w == g_drag.win;
        if (!hidden_dragged_window && !w->minimized && rect_overlap(w->r, area))
            paint_window(w, area);
    }
    paint_taskbar(area);
    menu_draw(area);
    paint_ghost(area);
    dlg_draw(area);
    const rect_t cr = cursor_rect();
    if (rect_overlap(cr, area)) {
        gfx_clip(area);
        cursor_draw();
    }
    gfx_clip_screen();
}

void wm_full_repaint(void) { wm_repaint(screen_rect()); }

void wm_invalidate_rect(struct window *w, rect_t rel) {
    const rect_t c = wm_client(w);
    rect_t r = rect_make(c.x + rel.x, c.y + rel.y, rel.w, rel.h);
    r = rect_intersect(r, c);
    if (w->minimized) return;
    wm_repaint(r);
}

void wm_invalidate(struct window *w) {
    if (w->minimized) return;
    wm_repaint(wm_client(w));
}

/* ----------------------------------------------------------- focus and stack */

static void set_focus(struct window *w) {
    if (g_focus == w) return;
    struct window *old = g_focus;
    g_focus = w;
    if (old && old->used && !old->minimized) wm_repaint(title_rect(old));
    if (w && !w->minimized) wm_repaint(title_rect(w));
    wm_taskbar_repaint();
}

void wm_raise(struct window *w) {
    const int was_min = w->minimized;
    const int top = (z_index(w) == (int)g_zn - 1);
    w->minimized = 0;
    if (!top) z_push(w);
    if (!top || was_min) {
        struct window *old = g_focus;
        g_focus = w;
        (void)old;
        wm_repaint(w->r);
        wm_taskbar_repaint();
        if (old && old != w && old->used && !old->minimized)
            wm_repaint(title_rect(old));
    } else {
        set_focus(w);
    }
}

static void minimise(struct window *w) {
    w->minimized = 1;
    if (g_focus == w) g_focus = top_visible();
    wm_repaint(w->r);
    wm_taskbar_repaint();
}

static void toggle_maximise(struct window *w) {
    const rect_t old = w->r;
    const rect_t old_restore = w->restore;
    const uint8_t old_maximized = w->maximized;
    if (w->maximized) {
        w->r = w->restore;
        w->maximized = 0;
    } else {
        w->restore = w->r;
        w->r = g_desk.work;
        w->maximized = 1;
    }
    if (w->ops->resize) {
        const rect_t client = wm_client(w);
        if (!w->ops->resize(w, client.w, client.h)) {
            w->r = old;
            w->restore = old_restore;
            w->maximized = old_maximized;
            dlg_message("Not enough memory to resize this window.");
        }
    }
    wm_repaint(rect_union(old, w->r));
}

/* ------------------------------------------------------------ open and close */

struct window *wm_open(const struct app_ops *ops, const char *arg) {
    if (ops->single) {
        for (uint32_t i = 0; i < WIN_MAX; ++i) {
            if (g_win[i].used && g_win[i].ops == ops) {
                wm_raise(&g_win[i]);
                return &g_win[i];
            }
        }
    }
    int slot = -1;
    for (int i = 0; i < WIN_MAX; ++i) {
        if (!g_win[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        dlg_message("Too many windows open. Close one first.");
        return 0;
    }
    struct window *w = &g_win[slot];
    memset(w, 0, sizeof(*w));
    memset(g_state[slot], 0, APP_STATE_MAX);
    w->used = 1;
    w->ops = ops;
    w->state = g_state[slot];
    d_strcpy(w->title, sizeof(w->title), ops->name);

    const rect_t wk = g_desk.work;
    int32_t cw = ops->def_w < wk.w ? ops->def_w : wk.w;
    int32_t ch = ops->def_h < wk.h ? ops->def_h : wk.h;
    const int32_t step = (int32_t)(g_zn % 6);
    int32_t x = 120 + step * 28;
    int32_t y = 16 + step * 26;
    if (x + cw > wk.w) x = wk.w - cw;
    if (y + ch > wk.h) y = wk.h - ch;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    w->r = rect_make(x, y, cw, ch);

    if (ops->open && ops->open(w, arg) != 0) {
        w->used = 0;
        return 0;
    }
    z_push(w);
    g_focus = w;
    wm_repaint(w->r);
    wm_taskbar_repaint();
    return w;
}

void wm_close(struct window *w) {
    if (!w->used) return;
    const rect_t area = w->r;
    if (w->ops->close) w->ops->close(w);
    if (g_capture == w) g_capture = 0;
    if (g_drag.win == w) g_drag.active = 0;
    z_remove(w);
    w->used = 0;
    if (g_focus == w) g_focus = top_visible();
    wm_repaint(area);
    wm_taskbar_repaint();
    if (g_focus) wm_repaint(title_rect(g_focus));
}

void wm_request_close(struct window *w) {
    if (w->ops->query_close && !w->ops->query_close(w)) return;
    wm_close(w);
}

/* --------------------------------------------------------------------- input */

static void forward_mouse(struct window *w, const struct mouse_event *e) {
    if (!w->ops->mouse) return;
    const rect_t c = wm_client(w);
    struct mouse_event local = *e;
    local.x = e->x - c.x;
    local.y = e->y - c.y;
    w->ops->mouse(w, &local);
}

static void drag_begin(struct window *w, const struct mouse_event *e, int resize) {
    g_drag.active = 1;
    g_drag.resize = resize;
    g_drag.win = w;
    g_drag.off_x = e->x - w->r.x;
    g_drag.off_y = e->y - w->r.y;
    g_drag.ghost = w->r;
    rect_t strips[4];
    ghost_strips(g_drag.ghost, strips);
    for (int i = 0; i < 4; ++i) wm_repaint(strips[i]);
    if (!resize) wm_repaint(w->r);
}

static void drag_move(const struct mouse_event *e) {
    rect_t ng = g_drag.ghost;
    if (g_drag.resize) {
        const struct window *w = g_drag.win;
        const int32_t origin_x = ng.x > 0 ? ng.x : 0;
        const int32_t origin_y = ng.y > 0 ? ng.y : 0;
        int32_t max_w = g_desk.width - origin_x;
        int32_t max_h = g_desk.work.h - origin_y;
        int32_t min_w = w->ops->min_w > 0 ? w->ops->min_w : 160;
        int32_t min_h = w->ops->min_h > 0 ? w->ops->min_h : 100;
        if (min_w > max_w) min_w = max_w;
        if (min_h > max_h) min_h = max_h;
        ng.w = e->x - ng.x;
        ng.h = e->y - ng.y;
        if (ng.w < min_w) ng.w = min_w;
        if (ng.h < min_h) ng.h = min_h;
        if (ng.w > max_w) ng.w = max_w;
        if (ng.h > max_h) ng.h = max_h;
    } else {
        ng.x = e->x - g_drag.off_x;
        ng.y = e->y - g_drag.off_y;
        if (ng.y < 0) ng.y = 0;
        if (ng.y > g_desk.work.h - TITLE_H) ng.y = g_desk.work.h - TITLE_H;
        if (ng.x < 80 - ng.w) ng.x = 80 - ng.w;
        if (ng.x > g_desk.width - 80) ng.x = g_desk.width - 80;
    }
    if (ng.x == g_drag.ghost.x && ng.y == g_drag.ghost.y &&
        ng.w == g_drag.ghost.w && ng.h == g_drag.ghost.h) return;
    rect_t olds[4], news[4];
    ghost_strips(g_drag.ghost, olds);
    g_drag.ghost = ng;
    ghost_strips(ng, news);
    for (int i = 0; i < 4; ++i) wm_repaint(olds[i]);
    for (int i = 0; i < 4; ++i) wm_repaint(news[i]);
}

static void drag_end(void) {
    struct window *w = g_drag.win;
    const rect_t requested = g_drag.ghost;
    g_drag.active = 0;
    if (!w || !w->used) return;
    const rect_t old = w->r;
    w->r = requested;
    const int resized = old.w != w->r.w || old.h != w->r.h;
    if (resized && w->ops->resize) {
        const rect_t client = wm_client(w);
        if (!w->ops->resize(w, client.w, client.h)) {
            w->r = old;
            dlg_message("Not enough memory to resize this window.");
        }
    }
    if (rect_overlap(old, requested)) {
        wm_repaint(rect_union(old, requested));
    } else {
        wm_repaint(old);
        wm_repaint(requested);
    }
}

static void taskbar_click(const struct mouse_event *e) {
    if (e->x >= 4 && e->x < 4 + START_W) {
        menu_toggle();
        return;
    }
    uint32_t total = 0;
    for (uint32_t i = 0; i < WIN_MAX; ++i) total += g_win[i].used;
    uint32_t nth = 0;
    for (uint32_t i = 0; i < WIN_MAX; ++i) {
        if (!g_win[i].used) continue;
        const rect_t br = task_button_rect(nth++, total);
        if (!rect_hit(br, e->x, e->y)) continue;
        struct window *w = &g_win[i];
        if (w->minimized || w != g_focus) wm_raise(w);
        else minimise(w);
        return;
    }
}

static void title_click(struct window *w, const struct mouse_event *e) {
    if (rect_hit(button_rect(w, 0), e->x, e->y)) {
        wm_request_close(w);
    } else if (rect_hit(button_rect(w, 1), e->x, e->y)) {
        toggle_maximise(w);
    } else if (rect_hit(button_rect(w, 2), e->x, e->y)) {
        minimise(w);
    } else if (e->clicks == 2) {
        toggle_maximise(w);
    } else if (!w->maximized) {
        drag_begin(w, e, 0);
    }
}

void wm_mouse(const struct mouse_event *e) {
    if (dlg_active()) {
        dlg_mouse(e);
        return;
    }
    if (g_drag.active) {
        if (e->type == MOUSE_MOVE) drag_move(e);
        else if (e->type == MOUSE_UP) drag_end();
        return;
    }
    if (icons_drag_active() && e->type == MOUSE_UP) {
        (void)icons_mouse(e);
        return;
    }
    if (g_capture) {
        struct window *w = g_capture;
        if (e->type == MOUSE_UP) g_capture = 0;
        if (w->used) forward_mouse(w, e);
        return;
    }
    if (e->type == MOUSE_MOVE) {
        if (menu_is_open()) (void)menu_mouse(e);
        if (icons_drag_active()) {
            (void)icons_mouse(e);
            return;
        }
        struct window *w = window_at(e->x, e->y);
        if (w && rect_hit(wm_client(w), e->x, e->y)) forward_mouse(w, e);
        else if (!w) (void)icons_mouse(e);
        return;
    }
    if (e->type == MOUSE_WHEEL) {
        struct window *w = window_at(e->x, e->y);
        if (w && rect_hit(wm_client(w), e->x, e->y)) forward_mouse(w, e);
        return;
    }
    if (e->type != MOUSE_DOWN) return;

    if (menu_is_open() && menu_mouse(e)) return;
    if (rect_hit(g_desk.taskbar, e->x, e->y)) {
        taskbar_click(e);
        return;
    }
    struct window *w = window_at(e->x, e->y);
    if (!w) {
        (void)icons_mouse(e);
        return;
    }
    if (w != g_focus || z_index(w) != (int)g_zn - 1) wm_raise(w);
    if (rect_hit(title_rect(w), e->x, e->y)) {
        title_click(w, e);
    } else if (!w->maximized &&
               rect_hit(resize_handle_rect(w), e->x, e->y)) {
        drag_begin(w, e, 1);
    } else if (rect_hit(wm_client(w), e->x, e->y)) {
        g_capture = w;
        forward_mouse(w, e);
    }
}

void wm_key(int key) {
    if (dlg_active()) {
        dlg_key(key);
        return;
    }
    if (menu_is_open()) {
        if (menu_key(key)) return;
    }
    if (g_focus && g_focus->ops->key && g_focus->ops->key(g_focus, key)) return;
    if (key == '\t' && g_zn > 1) {
        /* Cycle: send the top window to the back and focus the next one. */
        uint8_t first = g_z[0];
        for (uint32_t i = 0; i + 1 < g_zn; ++i) g_z[i] = g_z[i + 1];
        g_z[g_zn - 1] = first;
        wm_raise(&g_win[g_z[g_zn - 1]]);
        return;
    }
    if (key == 0x1b) menu_toggle();
}

/* --------------------------------------------------------------------- clock */

static void update_clock(void) {
    struct welovec_user_datetime t;
    char text[CLOCK_CHARS + 1];
    if (welovec_rtc_get_utc(&t) < 0) return;
    d_pad2(text, t.hour);
    text[2] = ':';
    d_pad2(text + 3, t.minute);
    text[5] = ' ';
    text[6] = 'U';
    text[7] = 'T';
    text[8] = 'C';
    text[9] = '\0';
    if (d_streq(text, g_clock)) return;
    d_strcpy(g_clock, sizeof(g_clock), text);
    wm_repaint(clock_rect());
}

void wm_tick(uint64_t ticks) {
    update_clock();
    for (uint32_t i = 0; i < WIN_MAX; ++i) {
        struct window *w = &g_win[i];
        if (w->used && !w->minimized && w->ops->tick) w->ops->tick(w, ticks);
    }
    app_program_reap();
}

void wm_refresh_windows(void) {
    for (uint32_t i = 0; i < WIN_MAX; ++i) {
        struct window *w = &g_win[i];
        if (w->used && !w->minimized && w->ops->dirty && w->ops->dirty(w))
            wm_invalidate(w);
    }
}

void wm_shutdown(void) {
    for (uint32_t i = 0; i < WIN_MAX; ++i) {
        struct window *w = &g_win[i];
        if (w->used && w->ops->close) w->ops->close(w);
        w->used = 0;
    }
    g_zn = 0;
    g_focus = 0;
    g_capture = 0;
    g_drag.active = 0;
    app_program_wait_all();
}

int wm_cursor_resize(void) {
    if (g_drag.active) return g_drag.resize;
    struct window *w = window_at(g_desk.ptr_x, g_desk.ptr_y);
    return w && !w->maximized &&
        rect_hit(resize_handle_rect(w), g_desk.ptr_x, g_desk.ptr_y);
}

void wm_init(void) {
    memset(g_win, 0, sizeof(g_win));
    g_zn = 0;
    g_focus = 0;
    g_capture = 0;
    g_drag.active = 0;
    update_clock();
}
