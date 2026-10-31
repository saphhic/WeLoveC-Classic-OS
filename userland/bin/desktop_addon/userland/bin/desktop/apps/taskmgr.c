// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Displays and manages system tasks.

#include "desktop/include/desktop.h"
#include "user/process.h"
#include "user/syscall.h"

/* Task manager: scans PIDs with PROCESS_QUERY once per second (there is no
 * "list processes" syscall) and can send SIGTERM / SIGKILL to the selection. */

#define MAX_PID 96
#define MAX_ROWS 40
#define TOOLBAR_H 32
#define HEADER_H 20
#define STATUS_H 20

struct row {
    uint32_t pid, ppid, uid, threads;
    uint8_t state;
    char name[20];
};

struct tasks_state {
    struct row rows[MAX_ROWS];
    uint32_t count;
    int32_t sel, top;
    char status[48];
};
APP_STATE_CHECK(struct tasks_state);

enum { B_REFRESH, B_END, B_KILL, B_COUNT };
static const char *const k_btn[B_COUNT] = {"Refresh", "End task", "Kill"};

static void refresh(struct tasks_state *s) {
    uint32_t keep = 0;
    if (s->sel >= 0 && s->sel < (int32_t)s->count) keep = s->rows[s->sel].pid;
    s->count = 0;
    for (uint32_t pid = 1; pid <= MAX_PID && s->count < MAX_ROWS; ++pid) {
        struct welovec_user_process_info info;
        if (welovec_process_query(pid, &info) < 0) continue;
        struct row *r = &s->rows[s->count++];
        r->pid = (uint32_t)info.id;
        r->ppid = (uint32_t)info.parent_id;
        r->uid = info.uid;
        r->threads = (uint32_t)info.live_threads;
        r->state = (uint8_t)info.state;
        d_strcpy(r->name, sizeof(r->name), info.name);
    }
    s->sel = -1;
    for (uint32_t i = 0; i < s->count; ++i)
        if (s->rows[i].pid == keep) s->sel = (int32_t)i;
}

static int open(struct window *w, const char *arg) {
    (void)arg;
    refresh(w->state);
    return 0;
}

static rect_t button_rect(rect_t c, int b) {
    int32_t x = c.x + 4;
    for (int i = 0; i < b; ++i) x += ui_button_w(k_btn[i]) + 4;
    return rect_make(x, c.y + 4, ui_button_w(k_btn[b]), 24);
}

static rect_t list_rect(rect_t c) {
    return rect_make(c.x, c.y + TOOLBAR_H + HEADER_H, c.w,
                     c.h - TOOLBAR_H - HEADER_H - STATUS_H);
}

static void put_num(char *line, uint32_t cap, uint32_t value, uint32_t width) {
    char num[24];
    const uint32_t n = d_utoa(value, num);
    for (uint32_t i = n; i < width; ++i) d_strcat(line, cap, " ");
    d_strcat(line, cap, num);
}

static void draw(struct window *w, rect_t c) {
    struct tasks_state *s = w->state;
    for (int b = 0; b < B_COUNT; ++b) ui_button(button_rect(c, b), k_btn[b], 1);

    gfx_fill(c.x, c.y + TOOLBAR_H, c.w, HEADER_H, C_BTN);
    gfx_text(c.x + 6, c.y + TOOLBAR_H + 2, "  PID PPID  UID STA THR NAME", C_TEXT);

    const rect_t lr = list_rect(c);
    ui_sunken(lr, C_EDIT_BG);
    const int32_t rows = lr.h / ROW_H;
    for (int32_t i = 0; i < rows; ++i) {
        const int32_t idx = s->top + i;
        if (idx >= (int32_t)s->count) break;
        const struct row *r = &s->rows[idx];
        const int32_t y = lr.y + 1 + i * ROW_H;
        const int on = idx == s->sel;
        if (on) gfx_fill(lr.x + 1, y, lr.w - 14, ROW_H, C_SEL);
        char line[80];
        line[0] = '\0';
        put_num(line, sizeof(line), r->pid, 5);
        put_num(line, sizeof(line), r->ppid, 5);
        put_num(line, sizeof(line), r->uid, 5);
        d_strcat(line, sizeof(line),
                 r->state == WELOVEC_USER_PROCESS_ACTIVE ? " run"
                 : r->state == WELOVEC_USER_PROCESS_ZOMBIE ? " zmb" : " end");
        put_num(line, sizeof(line), r->threads, 4);
        d_strcat(line, sizeof(line), " ");
        d_strcat(line, sizeof(line), r->name);
        gfx_text(lr.x + 6, y + 2, line, on ? C_WHITE : C_TEXT);
    }
    ui_scrollbar(rect_make(lr.x + lr.w - 13, lr.y + 1, 12, lr.h - 2), s->top, rows,
                 (int32_t)s->count);

    const int32_t sy = c.y + c.h - STATUS_H;
    gfx_fill(c.x, sy, c.w, STATUS_H, C_BTN);
    char line[80], num[24];
    d_utoa(s->count, num);
    d_strcpy(line, sizeof(line), num);
    d_strcat(line, sizeof(line), " processes");
    gfx_text(c.x + 6, sy + 2, line, C_TEXT);
    if (s->status[0]) ui_text_right(c.x + c.w - 6, sy + 2, s->status, C_TEXT_DIM);
}

static void send_signal(struct window *w, uint32_t sig) {
    struct tasks_state *s = w->state;
    if (s->sel < 0 || s->sel >= (int32_t)s->count) return;
    const uint32_t pid = s->rows[s->sel].pid;
    if (pid == (uint32_t)welovec_getpid()) {
        dlg_message("This is the desktop itself. Use Start > Exit desktop.");
        return;
    }
    const int64_t r = welovec_kill(pid, sig);
    d_strcpy(s->status, sizeof(s->status), r < 0 ? "Not permitted" : "Signal sent");
    refresh(s);
}

static void mouse(struct window *w, const struct mouse_event *e) {
    struct tasks_state *s = w->state;
    const rect_t full = wm_client(w);
    const rect_t c = rect_make(0, 0, full.w, full.h);
    const rect_t lr = list_rect(c);
    if (e->type == MOUSE_WHEEL) {
        s->top -= e->wheel * 3;
        const int32_t max = (int32_t)s->count - lr.h / ROW_H;
        if (s->top > max) s->top = max;
        if (s->top < 0) s->top = 0;
        wm_invalidate(w);
        return;
    }
    if (e->type != MOUSE_DOWN) return;
    s->status[0] = '\0';
    for (int b = 0; b < B_COUNT; ++b) {
        if (!rect_hit(button_rect(c, b), e->x, e->y)) continue;
        if (b == B_REFRESH) refresh(s);
        else send_signal(w, b == B_END ? WELOVEC_SIGNAL_TERMINATE : WELOVEC_SIGNAL_KILL);
        wm_invalidate(w);
        return;
    }
    if (rect_hit(lr, e->x, e->y)) {
        const int32_t idx = s->top + (e->y - lr.y - 1) / ROW_H;
        if (idx >= 0 && idx < (int32_t)s->count) s->sel = idx;
        wm_invalidate(w);
    }
}

static int key(struct window *w, int k) {
    struct tasks_state *s = w->state;
    if (k == KEY_UP && s->sel > 0) --s->sel;
    else if (k == KEY_DOWN && s->sel + 1 < (int32_t)s->count) ++s->sel;
    else return 0;
    const int32_t rows = list_rect(rect_make(0, 0, wm_client(w).w, wm_client(w).h)).h / ROW_H;
    if (s->sel < s->top) s->top = s->sel;
    if (rows > 0 && s->sel >= s->top + rows) s->top = s->sel - rows + 1;
    wm_invalidate(w);
    return 1;
}

static void tick(struct window *w, uint64_t ticks) {
    (void)ticks;
    refresh(w->state);
    wm_invalidate(w);
}

const struct app_ops app_tasks = {
    "Task manager", 560, 380, 420, 220, 1, open, 0, 0, draw, mouse, key, tick, 0, 0, 0,
};
