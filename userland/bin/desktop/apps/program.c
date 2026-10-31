// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Lists installed programs and lets users launch them.

#include "desktop/include/desktop.h"
#include "user/framebuffer.h"
#include "user/input.h"
#include "user/process.h"
#include "user/security.h"
#include "user/syscall.h"

#define ORPHAN_MAX 12

struct program_state {
    uint64_t pid;
    uint64_t generation;
    uint64_t last_draw_tick;
    int64_t exit_status;
    uint8_t exited;
    uint8_t pending_draw;
    uint8_t argument_count;
    char path[192];
    char arguments[6][64];
};
APP_STATE_CHECK(struct program_state);

static uint64_t g_orphan_pids[ORPHAN_MAX];

static int open_program(struct window *w, const char *path) {
    struct program_state *s = w->state;
    if (!path || !path[0]) return -1;
    char launch[512];
    d_strcpy(launch, sizeof(launch), path);
    uint32_t start = 0;
    while (launch[start] && launch[start] != '\n') ++start;
    if (launch[start]) launch[start++] = '\0';
    if (d_strlen(launch) >= sizeof(s->path)) return -1;
    d_strcpy(s->path, sizeof(s->path), launch);
    while (launch[start] && s->argument_count < 6) {
        uint32_t end = start;
        while (launch[end] && launch[end] != '\n') ++end;
        const char delimiter = launch[end];
        launch[end] = '\0';
        d_strcpy(s->arguments[s->argument_count],
                 sizeof(s->arguments[s->argument_count]), launch + start);
        ++s->argument_count;
        if (!delimiter) break;
        start = end + 1;
    }

    const rect_t client = wm_client(w);
    const char *argv[7] = {s->path, 0, 0, 0, 0, 0, 0};
    for (uint32_t i = 0; i < s->argument_count; ++i)
        argv[i + 1] = s->arguments[i];
    const int64_t pid = welovec_exec_windowed(
        s->path, s->argument_count + 1, argv, WELOVEC_PRIORITY_NORMAL,
        (uint32_t)client.w, (uint32_t)client.h);
    if (pid < 0) {
        dlg_message("Could not start this program (memory or process resources unavailable).");
        return -1;
    }
    s->pid = (uint64_t)pid;
    s->generation = welovec_fb_surface_generation(s->pid);
    s->last_draw_tick = g_desk.now;

    uint32_t name_start = d_strlen(s->path);
    while (name_start && s->path[name_start - 1] != '/') --name_start;
    char title[32];
    d_strcpy(title, sizeof(title), s->path + name_start);
    if (d_ends_with(title, ".elf")) title[d_strlen(title) - 4] = '\0';
    wm_set_title(w, title[0] ? title : "Program");
    return 0;
}

static void save_orphan(uint64_t pid) {
    if (!pid) return;
    for (uint32_t i = 0; i < ORPHAN_MAX; ++i) {
        if (g_orphan_pids[i] == pid) return;
        if (!g_orphan_pids[i]) {
            g_orphan_pids[i] = pid;
            return;
        }
    }
}

static void close_program(struct window *w) {
    struct program_state *s = w->state;
    if (!s->pid) return;
    (void)welovec_terminate_windowed_tree(
        s->pid, -(int64_t)WELOVEC_SIGNAL_TERMINATE);

    int64_t status = 0;
    const int64_t waited = welovec_waitpid(s->pid, &status, WELOVEC_WAIT_NOHANG);
    if (waited == 0) save_orphan(s->pid);
    s->pid = 0;
}

static void draw(struct window *w, rect_t client) {
    struct program_state *s = w->state;
    const rect_t clip = rect_intersect(gfx_clip_get(), client);
    if (rect_empty(clip)) return;
    const int32_t source_x = clip.x - client.x;
    const int32_t source_y = clip.y - client.y;
    (void)welovec_fb_surface_blit(
        s->pid, clip.x, clip.y, source_x, source_y,
        (uint32_t)clip.w, (uint32_t)clip.h);
}

static void mouse(struct window *w, const struct mouse_event *e) {
    struct program_state *s = w->state;
    if (!s->pid) return;
    const rect_t client = wm_client(w);
    int32_t x = e->x;
    int32_t y = e->y;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= client.w) x = client.w - 1;
    if (y >= client.h) y = client.h - 1;
    welovec_pointer_event event = {0};
    event.absolute_x = client.w > 1
        ? (int32_t)((int64_t)x * 65535 / (client.w - 1)) : 0;
    event.absolute_y = client.h > 1
        ? (int32_t)((int64_t)y * 65535 / (client.h - 1)) : 0;
    event.buttons = e->buttons;
    event.flags = WELOVEC_POINTER_EVENT_ABSOLUTE;
    event.source = WELOVEC_POINTER_SOURCE_VIRTIO;
    event.wheel = e->type == MOUSE_WHEEL ? e->wheel : 0;
    (void)welovec_input_forward(s->pid, 0, 0, &event, 1);
}

static int key(struct window *w, int key_code) {
    struct program_state *s = w->state;
    if (!s->pid) return 1;
    char sequence[6];
    uint32_t count = 0;
    char final = 0;
    if (key_code == KEY_UP) final = 'A';
    else if (key_code == KEY_DOWN) final = 'B';
    else if (key_code == KEY_RIGHT) final = 'C';
    else if (key_code == KEY_LEFT) final = 'D';
    if (final) {
        sequence[0] = '\x1b'; sequence[1] = '[';
        sequence[2] = final; count = 3;
    } else if (key_code == KEY_CTRL_UP || key_code == KEY_CTRL_DOWN) {
        sequence[0] = '\x1b'; sequence[1] = '['; sequence[2] = '1';
        sequence[3] = ';'; sequence[4] = '5';
        sequence[5] = key_code == KEY_CTRL_UP ? 'A' : 'B'; count = 6;
    } else if (key_code >= 0 && key_code <= 0x7f) {
        sequence[0] = (char)key_code; count = 1;
    }
    if (count) (void)welovec_input_forward(s->pid, sequence, count, 0, 0);
    return 1;
}

static void tick(struct window *w, uint64_t ticks) {
    (void)ticks;
    struct program_state *s = w->state;
    if (!s->pid || s->exited) return;
    struct welovec_user_process_info info;
    if (welovec_process_query(s->pid, &info) < 0 ||
        info.state != WELOVEC_USER_PROCESS_ZOMBIE) return;
    (void)welovec_terminate_windowed_tree(
        s->pid, -(int64_t)WELOVEC_SIGNAL_TERMINATE);
    s->exited = 1;
    s->exit_status = info.exit_status;
    s->generation = welovec_fb_surface_generation(s->pid);
    s->pending_draw = 0;
    char title[32], num[24];
    d_strcpy(title, sizeof(title), w->title);
    d_strcat(title, sizeof(title), " [exit ");
    if (s->exit_status < 0) {
        d_strcat(title, sizeof(title), "-");
        d_utoa((uint64_t)0 - (uint64_t)s->exit_status, num);
    } else {
        d_utoa((uint64_t)s->exit_status, num);
    }
    d_strcat(title, sizeof(title), num);
    d_strcat(title, sizeof(title), "]");
    wm_set_title(w, title);
    wm_invalidate(w);
    wm_taskbar_repaint();
}

static int dirty(struct window *w) {
    struct program_state *s = w->state;
    if (!s->pid || s->exited) return 0;
    const uint64_t generation = welovec_fb_surface_generation(s->pid);
    if (generation != s->generation) {
        s->generation = generation;
        s->pending_draw = 1;
    }
    /* Coalesce rapid terminal writes before copying the full window surface.
       At 100 Hz this caps redraws near 20 fps and keeps help/list output from
       monopolizing the desktop compositor. */
    if (!s->pending_draw || g_desk.now - s->last_draw_tick < 5) return 0;
    s->pending_draw = 0;
    s->last_draw_tick = g_desk.now;
    return 1;
}

static int resize(struct window *w, int32_t width, int32_t height) {
    struct program_state *s = w->state;
    if (!s->pid) return 1;
    if (welovec_fb_surface_resize(s->pid, (uint32_t)width,
                                  (uint32_t)height) != 0) return 0;
    {
        s->generation = welovec_fb_surface_generation(s->pid);
        s->last_draw_tick = g_desk.now;
        s->pending_draw = 0;
    }
    return 1;
}

void app_program_reap(void) {
    for (uint32_t i = 0; i < ORPHAN_MAX; ++i) {
        if (!g_orphan_pids[i]) continue;
        int64_t status = 0;
        const int64_t result = welovec_waitpid(
            g_orphan_pids[i], &status, WELOVEC_WAIT_NOHANG);
        if (result != 0) g_orphan_pids[i] = 0;
    }
}

void app_program_wait_all(void) {
    for (uint32_t i = 0; i < ORPHAN_MAX; ++i) {
        if (!g_orphan_pids[i]) continue;
        int64_t status = 0;
        (void)welovec_waitpid(g_orphan_pids[i], &status, 0);
        g_orphan_pids[i] = 0;
    }
}

const struct app_ops app_program = {
    "Program", 640, 420, 320, 200, 0,
    open_program, close_program, 0, draw, mouse, key, tick, dirty, resize, 1,
};
