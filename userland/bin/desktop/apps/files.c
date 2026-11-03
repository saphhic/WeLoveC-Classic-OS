// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the desktop file browser.

#include "desktop/include/desktop.h"
#include "user/filesystem.h"
#include "user/security.h"

/* File manager: browse, open (ELF runs, anything else opens in the editor),
 * create, move/rename, delete and copy/paste. Names are limited to 59 bytes
 * and a directory shows at most its first 32 entries (kernel FS_LIST limit). */

#define TOOLBAR_H 32
#define PATH_H 22
#define STATUS_H 20

struct files_state {
    char path[192];
    struct dir_item ent[32];
    uint32_t count;
    int32_t sel, top;
    int32_t err;
    uint32_t full;       /* listing hit the 32 entry kernel limit */
    char status[64];
};
APP_STATE_CHECK(struct files_state);

static char g_clip_path[256];        /* last "Copy" target, shared by windows */

enum { B_UP, B_NEWDIR, B_NEWFILE, B_MOVE, B_DEL, B_COPY, B_PASTE, B_COUNT };
static const char *const k_btn[B_COUNT] = {"Up",   "+Dir", "+File", "Move",
                                           "Del", "Copy", "Paste"};

static int cmp_items(const struct dir_item *a, const struct dir_item *b) {
    if (a->type != b->type) return a->type == WELOVEC_USER_FS_DIRECTORY ? -1 : 1;
    uint32_t i = 0;
    for (;; ++i) {
        char x = a->name[i], y = b->name[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}

static void refresh(struct files_state *s) {
    uint32_t n = 0;
    s->err = fsu_list(s->path, s->ent, 32, &n);
    s->count = s->err < 0 ? 0 : n;
    s->full = (s->err >= 0 && n >= 32);
    for (uint32_t i = 1; i < s->count; ++i) {       /* insertion sort, n <= 32 */
        struct dir_item key = s->ent[i];
        uint32_t j = i;
        while (j > 0 && cmp_items(&s->ent[j - 1], &key) > 0) {
            s->ent[j] = s->ent[j - 1];
            --j;
        }
        s->ent[j] = key;
    }
    if (s->sel >= (int32_t)s->count) s->sel = (int32_t)s->count - 1;
    if (s->sel < 0 && s->count) s->sel = 0;
    s->top = 0;
}

static void say(struct window *w, const char *text) {
    struct files_state *s = w->state;
    d_strcpy(s->status, sizeof(s->status), text);
    wm_invalidate(w);
}

static void fail(const char *what, int64_t err) {
    char msg[96], num[24];
    d_strcpy(msg, sizeof(msg), what);
    d_strcat(msg, sizeof(msg), " failed (error ");
    d_utoa(err < 0 ? (uint64_t)0 - (uint64_t)err : (uint64_t)err, num);
    d_strcat(msg, sizeof(msg), num);
    d_strcat(msg, sizeof(msg), ").");
    dlg_message(msg);
}

static int open(struct window *w, const char *arg) {
    struct files_state *s = w->state;
    (void)arg;
    d_strcpy(s->path, sizeof(s->path), "/");
    char home[64];
    d_strcpy(home, sizeof(home), "/home/");
    d_strcat(home, sizeof(home), g_desk.user);
    if (welovec_fs_exists(home) == 1) d_strcpy(s->path, sizeof(s->path), home);
    else if (welovec_fs_exists("/tmp") == 1) d_strcpy(s->path, sizeof(s->path), "/tmp");
    refresh(s);
    wm_set_title(w, "Files");
    return 0;
}

static void enter(struct window *w, const char *name) {
    struct files_state *s = w->state;
    char next[192];
    if (!d_join_path(next, sizeof(next), s->path, name)) {
        dlg_message("Path is too long.");
        return;
    }
    d_strcpy(s->path, sizeof(s->path), next);
    refresh(s);
    s->status[0] = '\0';
    wm_invalidate(w);
}

static void open_entry(struct window *w) {
    struct files_state *s = w->state;
    if (s->sel < 0 || s->sel >= (int32_t)s->count) return;
    const struct dir_item *it = &s->ent[s->sel];
    if (it->type == WELOVEC_USER_FS_DIRECTORY) {
        enter(w, it->name);
        return;
    }
    char path[256];
    if (!d_join_path(path, sizeof(path), s->path, it->name)) return;
    if (d_ends_with(it->name, ".elf")) {
        (void)desk_run(path, 0, 0, 1);
        refresh(s);
        return;
    }
    static const char *const editor_argv[] = {
        "/userspace/bin/editor/editor.elf", 0
    };
    const char *arguments[] = {editor_argv[0], path};
    (void)desk_run(editor_argv[0], 2, arguments, 1);
}

static void go_up(struct window *w) {
    struct files_state *s = w->state;
    d_parent_path(s->path);
    refresh(s);
    wm_invalidate(w);
}

/* --- prompts / confirms ---------------------------------------------------- */

static void do_newdir(void *ctx, const char *text) {
    struct window *w = ctx;
    struct files_state *s = w->state;
    char p[256];
    if (!text[0] || !d_join_path(p, sizeof(p), s->path, text)) return;
    const int64_t r = welovec_fs_mkdir(p);
    if (r < 0) fail("Create folder", r);
    refresh(s);
    wm_invalidate(w);
}

static void do_newfile(void *ctx, const char *text) {
    struct window *w = ctx;
    struct files_state *s = w->state;
    char p[256];
    const uint32_t length = d_strlen(text);
    if (!length || length >= 120 || text[0] == '.') {
        dlg_message("Enter a file name; new text files use .txt or .conf.");
        return;
    }
    uint32_t dot = length;
    for (uint32_t i = 1; i < length; ++i)
        if (text[i] == '.') dot = i;
    char filename[128];
    d_strcpy(filename, sizeof(filename), text);
    if (dot == length) {
        d_strcat(filename, sizeof(filename), ".txt");
    } else if (!d_ends_with(text, ".txt") && !d_ends_with(text, ".conf")) {
        dlg_message("New text files can only use .txt or .conf extensions.");
        return;
    }
    if (!d_join_path(p, sizeof(p), s->path, filename)) return;
    if (welovec_fs_exists(p) == 1) {
        dlg_message("That name already exists.");
        return;
    }
    const int64_t r = welovec_fs_write(p, "", 0);
    if (r < 0) fail("Create file", r);
    refresh(s);
    wm_invalidate(w);
}

static void do_move(void *ctx, const char *text) {
    struct window *w = ctx;
    struct files_state *s = w->state;
    if (s->sel < 0 || !text[0]) return;
    char from[256], to[256];
    if (!d_join_path(from, sizeof(from), s->path, s->ent[s->sel].name)) return;
    if (text[0] == '/') d_strcpy(to, sizeof(to), text);
    else if (!d_join_path(to, sizeof(to), s->path, text)) return;
    const int64_t r = welovec_fs_move(from, to);
    if (r < 0) fail("Move", r);
    refresh(s);
    wm_invalidate(w);
}

static void do_delete(void *ctx, const char *text) {
    (void)text;
    struct window *w = ctx;
    struct files_state *s = w->state;
    char p[256];
    if (s->sel < 0 || !d_join_path(p, sizeof(p), s->path, s->ent[s->sel].name))
        return;
    const int64_t r = welovec_fs_unlink(p);
    if (r < 0) fail("Delete (folders must be empty)", r);
    refresh(s);
    wm_invalidate(w);
}

static void do_paste(struct window *w) {
    struct files_state *s = w->state;
    if (!g_clip_path[0]) {
        say(w, "Nothing copied yet.");
        return;
    }
    struct welovec_user_file_info info;
    if (welovec_fs_stat(g_clip_path, &info) < 0 ||
        info.type != WELOVEC_USER_FS_FILE) {
        dlg_message("Only files can be copied.");
        return;
    }
    uint32_t i = d_strlen(g_clip_path);
    while (i && g_clip_path[i - 1] != '/') --i;
    char to[256];
    if (!d_join_path(to, sizeof(to), s->path, g_clip_path + i)) return;
    const int64_t r = fsu_copy_file(g_clip_path, to);
    if (r == -17) dlg_message("A file with that name already exists here.");
    else if (r < 0) fail("Copy", r);
    else say(w, "Copied.");
    refresh(s);
    wm_invalidate(w);
}

static void button_action(struct window *w, int b) {
    struct files_state *s = w->state;
    const int have = s->sel >= 0 && s->sel < (int32_t)s->count;
    switch (b) {
    case B_UP: go_up(w); break;
    case B_NEWDIR: dlg_prompt("New folder name:", "", do_newdir, w); break;
    case B_NEWFILE:
        dlg_prompt("New text file name (.txt or .conf):", "", do_newfile, w);
        break;
    case B_MOVE:
        if (have) dlg_prompt("Rename (name) or move (/full/path):",
                             s->ent[s->sel].name, do_move, w);
        break;
    case B_DEL:
        if (have) dlg_confirm("Delete the selected item?", do_delete, w);
        break;
    case B_COPY:
        if (have && s->ent[s->sel].type == WELOVEC_USER_FS_FILE) {
            d_join_path(g_clip_path, sizeof(g_clip_path), s->path,
                        s->ent[s->sel].name);
            say(w, "Copied to clipboard.");
        }
        break;
    case B_PASTE: do_paste(w); break;
    default: break;
    }
}

/* --- layout ------------------------------------------------------------------ */

static rect_t button_rect(rect_t c, int b) {
    int32_t x = c.x + 4;
    for (int i = 0; i < b; ++i) x += ui_button_w(k_btn[i]) + 4;
    return rect_make(x, c.y + 4, ui_button_w(k_btn[b]), 24);
}

static rect_t list_rect(rect_t c) {
    return rect_make(c.x, c.y + TOOLBAR_H + PATH_H, c.w,
                     c.h - TOOLBAR_H - PATH_H - STATUS_H);
}

static int32_t rows_visible(rect_t c) { return list_rect(c).h / ROW_H; }

static void draw_file_icon(int32_t x, int32_t y, const struct dir_item *item) {
    if (item->type == WELOVEC_USER_FS_DIRECTORY) {
        gfx_fill(x, y + 2, 12, 4, C_FOLDER);
        gfx_fill(x, y + 5, 22, 11, UINT32_C(0xfff2c14e));
        gfx_frame(x, y + 5, 22, 11, UINT32_C(0xff9a7415));
    } else if (d_ends_with(item->name, ".elf")) {
        gfx_fill(x, y, 66, 16, C_ELF_ICON);
        gfx_text(x + 1, y, ".elf", C_WHITE);
    } else if (d_ends_with(item->name, ".conf")) {
        gfx_fill(x, y, 20, 16, C_ICON_GRAY);
        for (int32_t i = 0; i < 4; ++i) {
            gfx_fill(x + 8 + (i == 0 ? -1 : i == 1 ? 9 : i == 2 ? 2 : 8),
                     y + (i == 0 ? 1 : i == 1 ? 8 : i == 2 ? 13 : 5),
                     3, 3, C_WHITE);
        }
        gfx_fill(x + 5, y + 4, 10, 8, C_WHITE);
        gfx_fill(x + 8, y + 6, 4, 4, C_ICON_GRAY);
        gfx_fill(x + 9, y + 7, 2, 2, C_PANEL_HI);
    } else {
        gfx_fill(x, y, 20, 16, C_ICON_GRAY);
        gfx_text(x + 2, y, "?", C_WHITE);
    }
}

static void keep_visible(struct window *w) {
    struct files_state *s = w->state;
    const int32_t rows = rows_visible(wm_client(w));
    if (s->sel < s->top) s->top = s->sel;
    if (rows > 0 && s->sel >= s->top + rows) s->top = s->sel - rows + 1;
    if (s->top < 0) s->top = 0;
}

static void draw(struct window *w, rect_t c) {
    struct files_state *s = w->state;
    for (int b = 0; b < B_COUNT; ++b) {
        const rect_t r = button_rect(c, b);
        if (r.x + r.w > c.x + c.w) break;
        ui_button(r, k_btn[b], 1);
    }
    const int32_t py = c.y + TOOLBAR_H;
    gfx_fill(c.x, py, c.w, PATH_H, C_BTN);
    const int32_t pchars = (c.w - 12) / FONT_W;
    const int32_t plen = (int32_t)d_strlen(s->path);
    const int32_t from = plen > pchars ? plen - pchars : 0;
    gfx_text(c.x + 6, py + 3, s->path + from, C_TEXT);

    const rect_t lr = list_rect(c);
    ui_sunken(lr, C_EDIT_BG);
    const int32_t rows = rows_visible(c);
    const int32_t name_chars = (lr.w - 78 - 8 * FONT_W - 16) / FONT_W;
    for (int32_t i = 0; i < rows; ++i) {
        const int32_t idx = s->top + i;
        if (idx >= (int32_t)s->count) break;
        const struct dir_item *it = &s->ent[idx];
        const int32_t y = lr.y + 1 + i * ROW_H;
        const int on = idx == s->sel;
        if (on) gfx_fill(lr.x + 1, y, lr.w - 14, ROW_H, C_SEL);
        const uint32_t fg = on ? C_WHITE : C_TEXT;
        const int dir = it->type == WELOVEC_USER_FS_DIRECTORY;
        draw_file_icon(lr.x + 5, y + 2, it);
        gfx_text_n(lr.x + 76, y + 2, it->name,
                   name_chars > 0 ? (uint32_t)name_chars : 0, fg);
        if (!dir) {
            char sz[24];
            d_fmt_size(sz, it->size);
            ui_text_right(lr.x + lr.w - 18, y + 2, sz, on ? C_WHITE : C_TEXT_DIM);
        }
    }
    ui_scrollbar(rect_make(lr.x + lr.w - 13, lr.y + 1, 12, lr.h - 2), s->top, rows,
                 (int32_t)s->count);

    const int32_t sy = c.y + c.h - STATUS_H;
    gfx_fill(c.x, sy, c.w, STATUS_H, C_BTN);
    char line[96], num[24];
    if (s->err < 0) {
        d_strcpy(line, sizeof(line), "Cannot read this folder (error ");
        d_utoa((uint64_t)0 - (uint64_t)(int64_t)s->err, num);
        d_strcat(line, sizeof(line), num);
        d_strcat(line, sizeof(line), ")");
    } else if (s->status[0]) {
        d_strcpy(line, sizeof(line), s->status);
    } else {
        d_utoa(s->count, num);
        d_strcpy(line, sizeof(line), num);
        d_strcat(line, sizeof(line), s->full ? " items (first 32 only)" : " items");
    }
    gfx_text_n(c.x + 6, sy + 2, line, (uint32_t)((c.w - 12) / FONT_W), C_TEXT);
}

static void mouse(struct window *w, const struct mouse_event *e) {
    struct files_state *s = w->state;
    const rect_t c = rect_make(0, 0, wm_client(w).w, wm_client(w).h);
    if (e->type == MOUSE_WHEEL) {
        s->top -= e->wheel * 3;
        const int32_t max = (int32_t)s->count - rows_visible(c);
        if (s->top > max) s->top = max;
        if (s->top < 0) s->top = 0;
        wm_invalidate(w);
        return;
    }
    if (e->type != MOUSE_DOWN) return;
    s->status[0] = '\0';
    for (int b = 0; b < B_COUNT; ++b) {
        if (rect_hit(button_rect(c, b), e->x, e->y)) {
            button_action(w, b);
            return;
        }
    }
    const rect_t lr = list_rect(c);
    if (!rect_hit(lr, e->x, e->y)) return;
    if (e->x >= lr.x + lr.w - 13) {                  /* scrollbar: page jump */
        const int32_t rows = rows_visible(c);
        s->top += (e->y < lr.y + lr.h / 2) ? -rows : rows;
        const int32_t max = (int32_t)s->count - rows;
        if (s->top > max) s->top = max;
        if (s->top < 0) s->top = 0;
        wm_invalidate(w);
        return;
    }
    const int32_t idx = s->top + (e->y - lr.y - 1) / ROW_H;
    if (idx < 0 || idx >= (int32_t)s->count) return;
    s->sel = idx;
    if (e->clicks == 2) open_entry(w);
    wm_invalidate(w);
}

static int key(struct window *w, int k) {
    struct files_state *s = w->state;
    if (k == KEY_UP && s->sel > 0) --s->sel;
    else if (k == KEY_DOWN && s->sel + 1 < (int32_t)s->count) ++s->sel;
    else if (k == KEY_CTRL_UP) s->sel = s->sel > 8 ? s->sel - 8 : 0;
    else if (k == KEY_CTRL_DOWN) {
        s->sel += 8;
        if (s->sel >= (int32_t)s->count) s->sel = (int32_t)s->count - 1;
    } else if (k == '\n') { open_entry(w); wm_invalidate(w); return 1; }
    else if (k == '\b' || k == 0x7f) { go_up(w); return 1; }
    else return 0;
    keep_visible(w);
    wm_invalidate(w);
    return 1;
}

static void tick(struct window *w, uint64_t ticks) {
    (void)ticks;
    (void)w;
}

const struct app_ops app_files = {
    "Files", 640, 420, 480, 240, 0, open, 0, 0, draw, mouse, key, tick, 0, 0, 0,
};
