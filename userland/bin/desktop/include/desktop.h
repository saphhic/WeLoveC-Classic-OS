// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for desktop in the desktop module.

#ifndef WELOVEC_DESKTOP_H
#define WELOVEC_DESKTOP_H

/*
 * WeLoveC lightweight desktop - shared types and module interfaces.
 *
 * Design rules (they follow from what the kernel offers today):
 *  - The desktop keeps a retained model (window list and app state), repaints
 *    damaged rectangles back-to-front, and publishes each repaint as one
 *    buffered framebuffer update. Windowed child programs draw into private
 *    surfaces that the desktop composites into those updates.
 *  - Everything is static or short-lived (`mmap` scratch that is unmapped
 *    right away), so the resident footprint stays near the 64 KiB user stack
 *    plus a few tens of KiB of code and .bss.
 *  - Only syscall wrappers from kernel/include/user/ are used; there is no libc.
 */

#include <stdint.h>

#define DESKTOP_VERSION "1.0"

/* ------------------------------------------------------------------ geometry */

typedef struct rect {
    int32_t x, y, w, h;
} rect_t;

static inline rect_t rect_make(int32_t x, int32_t y, int32_t w, int32_t h) {
    rect_t r = {x, y, w, h};
    return r;
}

static inline int rect_empty(rect_t r) { return r.w <= 0 || r.h <= 0; }

static inline int rect_hit(rect_t r, int32_t px, int32_t py) {
    return px >= r.x && py >= r.y && px < r.x + r.w && py < r.y + r.h;
}

static inline rect_t rect_intersect(rect_t a, rect_t b) {
    const int32_t x0 = a.x > b.x ? a.x : b.x;
    const int32_t y0 = a.y > b.y ? a.y : b.y;
    const int32_t x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    const int32_t y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    rect_t r = {x0, y0, x1 - x0, y1 - y0};
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

static inline int rect_overlap(rect_t a, rect_t b) {
    return !rect_empty(rect_intersect(a, b));
}

static inline rect_t rect_union(rect_t a, rect_t b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    const int32_t x0 = a.x < b.x ? a.x : b.x;
    const int32_t y0 = a.y < b.y ? a.y : b.y;
    const int32_t x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    const int32_t y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    rect_t r = {x0, y0, x1 - x0, y1 - y0};
    return r;
}

/* ------------------------------------------------------------------- layout */

enum {
    FONT_W = 16,        /* kernel text cells are 16x16 */
    FONT_H = 16,
    TITLE_H = 24,
    BORDER = 2,
    TASKBAR_H = 32,
    ROW_H = 20,
    WIN_MAX = 6,
    APP_STATE_MAX = 3072
};

/* ------------------------------------------------------------------ palette */

#define C_DESKTOP     UINT32_C(0xff24566b)
#define C_PANEL       UINT32_C(0xff2b2f36)
#define C_PANEL_HI    UINT32_C(0xff474e59)
#define C_ACCENT      UINT32_C(0xffc65d13)
#define C_ACCENT_DK   UINT32_C(0xff98420d)
#define C_WIN_BG      UINT32_C(0xffeceff1)
#define C_TEXT        UINT32_C(0xff1b1e23)
#define C_TEXT_DIM    UINT32_C(0xff626a74)
#define C_WHITE       UINT32_C(0xffffffff)
#define C_BLACK       UINT32_C(0xff000000)
#define C_BORDER      UINT32_C(0xff14171b)
#define C_TITLE_ON    UINT32_C(0xffb9500c)
#define C_TITLE_OFF   UINT32_C(0xff6b7380)
#define C_BTN         UINT32_C(0xffd7dce2)
#define C_SHADOW      UINT32_C(0xff8a929c)
#define C_EDIT_BG     UINT32_C(0xfffbfcfd)
#define C_SEL         UINT32_C(0xffc65d13)
#define C_OK          UINT32_C(0xff3aa55d)
#define C_WARN        UINT32_C(0xffd9a21b)
#define C_ERR         UINT32_C(0xffc9433f)
#define C_FOLDER      UINT32_C(0xffe0a92d)
#define C_ICON_SEL    UINT32_C(0xff3b7fa0)
#define C_ELF_ICON    UINT32_C(0xff94c99e)
#define C_ICON_GRAY   UINT32_C(0xff7b838d)

/* -------------------------------------------------------------------- input */

enum {
    KEY_UP = 0x100,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_CTRL_UP,
    KEY_CTRL_DOWN
};

enum { MOUSE_DOWN = 1, MOUSE_UP, MOUSE_MOVE, MOUSE_WHEEL };

struct mouse_event {
    int32_t type;
    int32_t x, y;      /* window-client relative when handed to an app */
    uint32_t buttons;
    int32_t wheel;     /* > 0 scrolls up */
    int32_t clicks;    /* 1 or 2 on MOUSE_DOWN */
};

/* ------------------------------------------------------------- global state */

struct desk_state {
    int32_t width, height;
    rect_t work;        /* desktop area above the taskbar */
    rect_t taskbar;
    char user[32];
    uint32_t uid, gid, admin;
    uint32_t quit;
    uint32_t vm;        /* running under a hypervisor (selects power path) */
    int32_t ptr_x, ptr_y;
    uint32_t ptr_buttons;
    uint64_t now;       /* scheduler ticks, 100 Hz */
};
extern struct desk_state g_desk;

/* -------------------------------------------------------------------- utils */

uint32_t d_strlen(const char *s);
int d_streq(const char *a, const char *b);
void d_strcpy(char *dst, uint32_t cap, const char *src);
void d_strcat(char *dst, uint32_t cap, const char *src);
uint32_t d_utoa(uint64_t value, char *out);          /* returns length */
void d_pad2(char *out, int32_t value);               /* "07" */
void d_fmt_size(char *out, uint64_t bytes);          /* "512 B", "3 KB" */
int d_join_path(char *out, uint32_t cap, const char *dir, const char *name);
void d_parent_path(char *path);
int d_ends_with(const char *s, const char *suffix);  /* ASCII case-insensitive */
void *memset(void *dst, int value, __SIZE_TYPE__ size);
void *memcpy(void *dst, const void *src, __SIZE_TYPE__ size);
void *memmove(void *dst, const void *src, __SIZE_TYPE__ size);

/* ----------------------------------------------------------------- graphics */

void gfx_init(void);
void gfx_clip(rect_t r);
void gfx_clip_screen(void);
rect_t gfx_clip_get(void);
void gfx_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void gfx_frame(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void gfx_text(int32_t x, int32_t y, const char *s, uint32_t color);
void gfx_text_n(int32_t x, int32_t y, const char *s, uint32_t n, uint32_t color);

/* Small widget helpers (ui.c) */
int32_t ui_button_w(const char *label);
void ui_button(rect_t r, const char *label, int enabled);
void ui_sunken(rect_t r, uint32_t background);
void ui_scrollbar(rect_t track, int32_t top, int32_t visible, int32_t total);
void ui_text_right(int32_t right, int32_t y, const char *s, uint32_t color);

/* ------------------------------------------------------------ file helpers */

struct dir_item {
    char name[60];
    uint32_t size;
    uint8_t type;       /* WELOVEC_USER_FS_FILE / _DIRECTORY */
};
/* Lists up to `cap` (<=32) entries. Returns 0 or a negative errno. */
int fsu_list(const char *path, struct dir_item *out, uint32_t cap,
             uint32_t *count);
int64_t fsu_copy_file(const char *source, const char *destination);

/* ------------------------------------------------------------------ windows */

struct window;

struct app_ops {
    const char *name;
    int16_t def_w, def_h;
    int16_t min_w, min_h;
    uint8_t single;                       /* only one instance at a time */
    int (*open)(struct window *w, const char *arg);   /* 0 = ok */
    void (*close)(struct window *w);
    int (*query_close)(struct window *w);              /* 1 = may close now */
    void (*draw)(struct window *w, rect_t client);
    void (*mouse)(struct window *w, const struct mouse_event *e);
    int (*key)(struct window *w, int key);             /* 1 = consumed */
    void (*tick)(struct window *w, uint64_t ticks);    /* once per second */
    int (*dirty)(struct window *w);                    /* output changed */
    int (*resize)(struct window *w, int32_t width, int32_t height);
    uint8_t surface_backed;                /* draw blits a complete app surface */
};

struct window {
    rect_t r, restore;
    uint8_t used, minimized, maximized;
    char title[32];
    const struct app_ops *ops;
    void *state;
};

void wm_init(void);
struct window *wm_open(const struct app_ops *ops, const char *arg);
void wm_close(struct window *w);
void wm_request_close(struct window *w);
void wm_raise(struct window *w);
void wm_set_title(struct window *w, const char *title);
rect_t wm_client(const struct window *w);
void wm_invalidate(struct window *w);
void wm_invalidate_rect(struct window *w, rect_t client_relative);
void wm_repaint(rect_t area);
void wm_full_repaint(void);
void wm_mouse(const struct mouse_event *e);
void wm_key(int key);
void wm_tick(uint64_t ticks);
void wm_refresh_windows(void);
void wm_shutdown(void);
int wm_cursor_resize(void);
uint32_t wm_window_count(void);
void wm_taskbar_repaint(void);

#define APP_STATE_CHECK(type) \
    _Static_assert(sizeof(type) <= APP_STATE_MAX, #type " exceeds APP_STATE_MAX")

extern const struct app_ops app_files;
extern const struct app_ops app_editor;
extern const struct app_ops app_tasks;
extern const struct app_ops app_sysinfo;
extern const struct app_ops app_calc;
extern const struct app_ops app_geometry;
extern const struct app_ops app_program;
void app_program_reap(void);
void app_program_wait_all(void);

/* ------------------------------------------------------------------- dialogs */

typedef void (*dlg_callback)(void *context, const char *text);

void dlg_prompt(const char *title, const char *initial, dlg_callback done,
                void *context);
void dlg_confirm(const char *text, dlg_callback done, void *context);
void dlg_message(const char *text);
int dlg_active(void);
void dlg_key(int key);
void dlg_mouse(const struct mouse_event *e);
void dlg_draw(rect_t area);

/* ------------------------------------------------------- menu, icons, apps */

enum {
    ACT_TERMINAL = 1,
    ACT_FILES,
    ACT_EDITOR,
    ACT_TASKS,
    ACT_SYSINFO,
    ACT_CALC,
    ACT_GEOMETRY,
    ACT_RUN,
    ACT_EXIT,
    ACT_REBOOT,
    ACT_SHUTDOWN,
    ACT_PROGRAM_BASE = 100
};

void menu_init(void);
int menu_is_open(void);
void menu_toggle(void);
void menu_close(void);
void menu_draw(rect_t area);
int menu_mouse(const struct mouse_event *e);   /* 1 = consumed */
int menu_key(int key);                         /* 1 = consumed */
void icons_draw(rect_t area);
int icons_mouse(const struct mouse_event *e);  /* 1 = consumed */
int icons_drag_active(void);
void app_launch(int action);

/* ------------------------------------------------------------------ session */

/* Opens an ELF inside a managed window. */
int desk_run(const char *path, uint32_t argc, const char *const argv[],
             int pause_after);
void desk_run_line(const char *line);
void desk_power(int reboot);

/* ------------------------------------------------------------------- cursor */

void input_init(void);
int input_pump(void);                  /* returns non-zero if anything happened */
void input_flush(void);
void cursor_draw(void);
rect_t cursor_rect(void);

#endif
