// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the desktop text editor.

#include "desktop/include/desktop.h"
#include "user/filesystem.h"
#include "user/syscall.h"

/*
 * Plain-text editor. The document lives in an anonymous mapping sized to the
 * file (+ slack) and grows by doubling, so an idle editor costs nothing and a
 * closed one returns all its memory. The keyboard layer delivers no Ctrl+letter
 * and no Home/End/Delete, so saving is done with the toolbar buttons.
 */

#define TOOLBAR_H 32
#define STATUS_H 20
#define SLACK 16384u
#define MAX_FILE (4u * 1024u * 1024u)

struct editor_state {
    char path[192];
    char *buf;
    uint32_t cap, len, cur;
    uint32_t top_line, left_col;
    uint8_t dirty, binary;
    char status[48];
};
APP_STATE_CHECK(struct editor_state);

enum { B_SAVE, B_SAVEAS, B_NEW, B_COUNT };
static const char *const k_btn[B_COUNT] = {"Save", "Save as", "New"};

static void set_title(struct window *w) {
    struct editor_state *s = w->state;
    char t[32];
    uint32_t i = d_strlen(s->path);
    while (i && s->path[i - 1] != '/') --i;
    d_strcpy(t, sizeof(t), s->path[0] ? s->path + i : "untitled");
    if (s->dirty) d_strcat(t, sizeof(t), "*");
    wm_set_title(w, t);
}

static int grow(struct editor_state *s, uint32_t need) {
    if (need <= s->cap) return 1;
    uint32_t cap = s->cap ? s->cap : 4096;
    while (cap < need) cap *= 2;
    if (cap > MAX_FILE + SLACK) return 0;
    char *nb = welovec_mmap(cap);
    if (!nb) return 0;
    if (s->buf) {
        memcpy(nb, s->buf, s->len);
        (void)welovec_munmap(s->buf, s->cap);
    }
    s->buf = nb;
    s->cap = cap;
    return 1;
}

static void load(struct window *w, const char *path) {
    struct editor_state *s = w->state;
    struct welovec_user_file_info info;
    if (welovec_fs_stat(path, &info) < 0 || info.type != WELOVEC_USER_FS_FILE) {
        d_strcpy(s->status, sizeof(s->status), "Cannot open file");
        return;
    }
    if (info.size > MAX_FILE) {
        d_strcpy(s->status, sizeof(s->status), "File too large (4 MB max)");
        return;
    }
    if (!grow(s, (uint32_t)info.size + SLACK)) {
        d_strcpy(s->status, sizeof(s->status), "Out of memory");
        return;
    }
    const int64_t got = welovec_fs_read(path, s->buf, s->cap);
    if (got < 0) {
        d_strcpy(s->status, sizeof(s->status), "Read failed");
        return;
    }
    s->len = (uint32_t)got;
    d_strcpy(s->path, sizeof(s->path), path);
    for (uint32_t i = 0; i < s->len && i < 4096; ++i)
        if (!s->buf[i]) s->binary = 1;
    if (s->binary) d_strcpy(s->status, sizeof(s->status), "Binary file: read-only");
}

static int open(struct window *w, const char *arg) {
    if (arg && arg[0]) load(w, arg);
    set_title(w);
    return 0;
}

static void close_app(struct window *w) {
    struct editor_state *s = w->state;
    if (s->buf) (void)welovec_munmap(s->buf, s->cap);
    s->buf = 0;
}

static void force_close(void *ctx, const char *t) {
    (void)t;
    struct window *w = ctx;
    ((struct editor_state *)w->state)->dirty = 0;
    wm_close(w);
}

static int query_close(struct window *w) {
    struct editor_state *s = w->state;
    if (!s->dirty) return 1;
    dlg_confirm("Discard unsaved changes?", force_close, w);
    return 0;
}

/* --- text geometry ------------------------------------------------------------ */

static uint32_t line_start(const struct editor_state *s, uint32_t off) {
    while (off && s->buf[off - 1] != '\n') --off;
    return off;
}

static uint32_t next_line(const struct editor_state *s, uint32_t off) {
    while (off < s->len && s->buf[off] != '\n') ++off;
    return off < s->len ? off + 1 : s->len;
}

static uint32_t line_of(const struct editor_state *s, uint32_t off) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < off && i < s->len; ++i) n += s->buf[i] == '\n';
    return n;
}

static uint32_t offset_of_line(const struct editor_state *s, uint32_t line) {
    uint32_t off = 0;
    while (line-- && off < s->len) off = next_line(s, off);
    return off;
}

static rect_t text_rect(rect_t c) {
    return rect_make(c.x, c.y + TOOLBAR_H, c.w, c.h - TOOLBAR_H - STATUS_H);
}

static void scroll_to_cursor(struct window *w) {
    struct editor_state *s = w->state;
    const rect_t c = wm_client(w);
    const rect_t tr = text_rect(c);
    const uint32_t rows = (uint32_t)(tr.h / FONT_H > 0 ? tr.h / FONT_H : 1);
    const uint32_t cols = (uint32_t)((tr.w - 8) / FONT_W > 0 ? (tr.w - 8) / FONT_W : 1);
    const uint32_t line = line_of(s, s->cur);
    const uint32_t col = s->cur - line_start(s, s->cur);
    if (line < s->top_line) s->top_line = line;
    if (line >= s->top_line + rows) s->top_line = line - rows + 1;
    if (col < s->left_col) s->left_col = col;
    if (col >= s->left_col + cols) s->left_col = col - cols + 1;
}

/* --- editing -------------------------------------------------------------------- */

static int insert(struct editor_state *s, const char *text, uint32_t n) {
    if (s->binary || !grow(s, s->len + n + 1)) return 0;
    memmove(s->buf + s->cur + n, s->buf + s->cur, s->len - s->cur);
    memcpy(s->buf + s->cur, text, n);
    s->len += n;
    s->cur += n;
    s->dirty = 1;
    return 1;
}

static void erase_before(struct editor_state *s) {
    if (s->binary || !s->cur) return;
    memmove(s->buf + s->cur - 1, s->buf + s->cur, s->len - s->cur);
    --s->len;
    --s->cur;
    s->dirty = 1;
}

static void save_to(struct window *w, const char *path) {
    struct editor_state *s = w->state;
    if (s->binary) {
        d_strcpy(s->status, sizeof(s->status), "Binary file: not saved");
        return;
    }
    const int64_t r = welovec_fs_write(path, s->buf ? s->buf : "", s->len);
    if (r < 0) {
        d_strcpy(s->status, sizeof(s->status), "Save failed (permissions?)");
    } else {
        d_strcpy(s->path, sizeof(s->path), path);
        s->dirty = 0;
        d_strcpy(s->status, sizeof(s->status), "Saved");
    }
    set_title(w);
}

static void save_as_done(void *ctx, const char *text) {
    struct window *w = ctx;
    if (text[0]) save_to(w, text);
    wm_invalidate(w);
    wm_taskbar_repaint();
}

static void ask_save_as(struct window *w) {
    struct editor_state *s = w->state;
    char suggest[192];
    if (s->path[0]) d_strcpy(suggest, sizeof(suggest), s->path);
    else {
        d_strcpy(suggest, sizeof(suggest), "/home/");
        d_strcat(suggest, sizeof(suggest), g_desk.user);
        d_strcat(suggest, sizeof(suggest), "/untitled.txt");
    }
    dlg_prompt("Save as (absolute path):", suggest, save_as_done, w);
}

static void new_done(void *ctx, const char *t) {
    (void)t;
    struct window *w = ctx;
    struct editor_state *s = w->state;
    s->len = s->cur = s->top_line = s->left_col = 0;
    s->path[0] = '\0';
    s->dirty = s->binary = 0;
    s->status[0] = '\0';
    set_title(w);
    wm_invalidate(w);
    wm_taskbar_repaint();
}

static void button(struct window *w, int b) {
    struct editor_state *s = w->state;
    if (b == B_SAVE) {
        if (s->path[0]) save_to(w, s->path);
        else ask_save_as(w);
        wm_invalidate(w);
        wm_taskbar_repaint();
    } else if (b == B_SAVEAS) {
        ask_save_as(w);
    } else if (s->dirty) {
        dlg_confirm("Discard unsaved changes?", new_done, w);
    } else {
        new_done(w, "");
    }
}

/* --- drawing ---------------------------------------------------------------------- */

static rect_t button_rect(rect_t c, int b) {
    int32_t x = c.x + 4;
    for (int i = 0; i < b; ++i) x += ui_button_w(k_btn[i]) + 4;
    return rect_make(x, c.y + 4, ui_button_w(k_btn[b]), 24);
}

static void draw(struct window *w, rect_t c) {
    struct editor_state *s = w->state;
    for (int b = 0; b < B_COUNT; ++b) ui_button(button_rect(c, b), k_btn[b], 1);

    const rect_t tr = text_rect(c);
    gfx_fill(tr.x, tr.y, tr.w, tr.h, C_EDIT_BG);
    const int32_t rows = tr.h / FONT_H;
    const int32_t cols = (tr.w - 8) / FONT_W;
    uint32_t off = s->buf ? offset_of_line(s, s->top_line) : 0;
    for (int32_t r = 0; r < rows && s->buf; ++r) {
        uint32_t end = off;
        while (end < s->len && s->buf[end] != '\n') ++end;
        uint32_t from = off + s->left_col;
        if (from < end) {
            uint32_t n = end - from;
            if (n > (uint32_t)cols) n = (uint32_t)cols;
            gfx_text_n(tr.x + 4, tr.y + r * FONT_H, s->buf + from, n, C_TEXT);
        }
        if (end >= s->len) break;
        off = end + 1;
    }
    const uint32_t line = line_of(s, s->cur);
    const uint32_t col = s->cur - line_start(s, s->cur);
    if (line >= s->top_line && col >= s->left_col) {
        gfx_fill(tr.x + 4 + (int32_t)(col - s->left_col) * FONT_W,
                 tr.y + (int32_t)(line - s->top_line) * FONT_H, 2, FONT_H, C_ACCENT);
    }

    const int32_t sy = c.y + c.h - STATUS_H;
    gfx_fill(c.x, sy, c.w, STATUS_H, C_BTN);
    char info[64], num[24];
    d_strcpy(info, sizeof(info), "Ln ");
    d_utoa(line + 1, num);
    d_strcat(info, sizeof(info), num);
    d_strcat(info, sizeof(info), " Col ");
    d_utoa(col + 1, num);
    d_strcat(info, sizeof(info), num);
    d_strcat(info, sizeof(info), s->dirty ? "  [modified]" : "");
    gfx_text(c.x + 6, sy + 2, info, C_TEXT);
    if (s->status[0])
        ui_text_right(c.x + c.w - 6, sy + 2, s->status, C_TEXT_DIM);
}

static void mouse(struct window *w, const struct mouse_event *e) {
    struct editor_state *s = w->state;
    const rect_t full = wm_client(w);
    const rect_t c = rect_make(0, 0, full.w, full.h);
    const rect_t tr = text_rect(c);
    if (e->type == MOUSE_WHEEL) {
        const int32_t t = (int32_t)s->top_line - e->wheel * 3;
        s->top_line = t < 0 ? 0 : (uint32_t)t;
        wm_invalidate(w);
        return;
    }
    if (e->type != MOUSE_DOWN) return;
    for (int b = 0; b < B_COUNT; ++b)
        if (rect_hit(button_rect(c, b), e->x, e->y)) {
            button(w, b);
            return;
        }
    if (!rect_hit(tr, e->x, e->y) || !s->buf) return;
    uint32_t off = offset_of_line(s, s->top_line + (uint32_t)((e->y - tr.y) / FONT_H));
    const uint32_t end = next_line(s, off);
    uint32_t col = s->left_col + (uint32_t)(e->x > tr.x + 4 ? (e->x - tr.x - 4 + FONT_W / 2) / FONT_W : 0);
    uint32_t span = end - off;
    if (span && s->buf[end - 1] == '\n') --span;
    if (col > span) col = span;
    s->cur = off + col;
    wm_invalidate(w);
}

static int key(struct window *w, int k) {
    struct editor_state *s = w->state;
    int whole = 0;                         /* needs a full text-area repaint */
    s->status[0] = '\0';
    if (k == '\t') {
        (void)insert(s, "    ", 4);
    } else if (k >= 32 && k < 127) {
        const char ch = (char)k;
        (void)insert(s, &ch, 1);
    } else if (k == '\n') {
        (void)insert(s, "\n", 1);
        whole = 1;
    } else if (k == '\b' || k == 0x7f) {
        if (s->cur && s->buf[s->cur - 1] == '\n') whole = 1;
        erase_before(s);
    } else if (k == KEY_LEFT) {
        if (s->cur) --s->cur;
    } else if (k == KEY_RIGHT) {
        if (s->cur < s->len) ++s->cur;
    } else if (k == KEY_UP || k == KEY_DOWN || k == KEY_CTRL_UP || k == KEY_CTRL_DOWN) {
        const uint32_t ls = line_start(s, s->cur);
        const uint32_t col = s->cur - ls;
        const int up = (k == KEY_UP || k == KEY_CTRL_UP);
        const int steps = (k == KEY_CTRL_UP || k == KEY_CTRL_DOWN) ? 10 : 1;
        uint32_t target = ls;
        for (int i = 0; i < steps; ++i) {
            if (up) {
                if (!target) break;
                target = line_start(s, target - 1);
            } else {
                uint32_t e = target;
                while (e < s->len && s->buf[e] != '\n') ++e;
                if (e >= s->len) break;
                target = e + 1;
            }
        }
        uint32_t span = 0;
        while (target + span < s->len && s->buf[target + span] != '\n') ++span;
        whole = 1;
        s->cur = target + (col < span ? col : span);
        whole = 1;
    } else {
        return 0;
    }
    set_title(w);
    const uint32_t before_top = s->top_line, before_left = s->left_col;
    scroll_to_cursor(w);
    if (whole || before_top != s->top_line || before_left != s->left_col)
        wm_invalidate_rect(w, rect_make(0, TOOLBAR_H, wm_client(w).w, wm_client(w).h - TOOLBAR_H));
    else {
        const uint32_t line = line_of(s, s->cur);
        wm_invalidate_rect(w, rect_make(0, TOOLBAR_H + (int32_t)(line - s->top_line) * FONT_H,
                                        wm_client(w).w, FONT_H));
        wm_invalidate_rect(w, rect_make(0, wm_client(w).h - STATUS_H, wm_client(w).w, STATUS_H));
    }
    wm_taskbar_repaint();
    return 1;
}

const struct app_ops app_editor = {
    "Editor", 640, 440, 400, 200, 0, open, close_app, query_close, draw, mouse, key, 0, 0, 0, 0,
};
