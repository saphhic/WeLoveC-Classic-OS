// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the Start menu and its actions.

#include "desktop/include/desktop.h"
#include "user/filesystem.h"
#include "user/input.h"

/*
 * Start menu + desktop icons. "Programs" scans /userspace/bin for up to six
 * entries with the <name>/<name>.elf layout produced by the root Makefile.
 * Built-in tools are listed separately and launch their own ELF processes.
 */

#define ITEM_H 24
#define MENU_W 272
#define PROG_MAX 6
#define SEP 0

struct item {
    const char *label;
    int action;
};

static const struct item k_items[] = {
    {"Terminal", ACT_TERMINAL}, {"Files", ACT_FILES},
    {"Text editor", ACT_EDITOR}, {"Code", ACT_CODE},
    {"Task manager", ACT_TASKS},
    {"System info", ACT_SYSINFO}, {"Calculator", ACT_CALC},
    {"Run...", ACT_RUN},
};
#define N_ITEMS ((int)(sizeof(k_items) / sizeof(k_items[0])))

static const struct item k_power[] = {
    {"Exit desktop", ACT_EXIT}, {"Reboot", ACT_REBOOT},
    {"Shut down", ACT_SHUTDOWN},
};
#define N_POWER ((int)(sizeof(k_power) / sizeof(k_power[0])))

static int g_open;
static int g_sel = -1;
static char g_prog[PROG_MAX][32];
static int g_prog_n;

/* Row model: items, [sep + programs], sep, power items. */
static int row_count(void) {
    return N_ITEMS + (g_prog_n ? g_prog_n + 1 : 0) + 1 + N_POWER;
}

static int row_action(int row, const char **label) {
    if (row < N_ITEMS) {
        *label = k_items[row].label;
        return k_items[row].action;
    }
    row -= N_ITEMS;
    if (g_prog_n) {
        if (row == 0) { *label = 0; return SEP; }
        --row;
        if (row < g_prog_n) {
            *label = g_prog[row];
            return ACT_PROGRAM_BASE + row;
        }
        row -= g_prog_n;
    }
    if (row == 0) { *label = 0; return SEP; }
    --row;
    *label = k_power[row].label;
    return k_power[row].action;
}

static void launch_builtin(const char *name) {
    char path[112];
    d_strcpy(path, sizeof(path), "/userspace/bin/");
    d_strcat(path, sizeof(path), name);
    d_strcat(path, sizeof(path), "/");
    d_strcat(path, sizeof(path), name);
    d_strcat(path, sizeof(path), ".elf");
    (void)desk_run(path, 0, 0, 1);
}

static rect_t menu_rect(void) {
    const int32_t h = row_count() * ITEM_H + 8;
    return rect_make(0, g_desk.taskbar.y - h, MENU_W, h);
}

static rect_t row_rect(int row) {
    const rect_t m = menu_rect();
    return rect_make(m.x + 4, m.y + 4 + row * ITEM_H, m.w - 8, ITEM_H);
}

static void scan_programs(void) {
    struct dir_item items[32];
    uint32_t count = 0;
    g_prog_n = 0;
    if (fsu_list("/userspace/bin", items, 32, &count) < 0) return;
    for (uint32_t i = 0; i < count && g_prog_n < PROG_MAX; ++i) {
        if (items[i].type != WELOVEC_USER_FS_DIRECTORY) continue;
        if (d_streq(items[i].name, "desktop") ||
            d_streq(items[i].name, "desktop_addon"))
            continue;
        if (d_strlen(items[i].name) >= sizeof(g_prog[0])) continue;
        char path[160];
        d_strcpy(path, sizeof(path), "/userspace/bin/");
        d_strcat(path, sizeof(path), items[i].name);
        d_strcat(path, sizeof(path), "/");
        d_strcat(path, sizeof(path), items[i].name);
        d_strcat(path, sizeof(path), ".elf");
        if (welovec_fs_exists(path) != 1) continue;
        d_strcpy(g_prog[g_prog_n++], sizeof(g_prog[0]), items[i].name);
    }
}

void menu_init(void) {
    g_open = 0;
    g_sel = -1;
    g_prog_n = 0;
}

int menu_is_open(void) { return g_open; }

void menu_close(void) {
    if (!g_open) return;
    const rect_t m = menu_rect();
    g_open = 0;
    wm_repaint(m);
    wm_taskbar_repaint();
}

void menu_toggle(void) {
    if (g_open) {
        menu_close();
        return;
    }
    scan_programs();
    g_open = 1;
    g_sel = -1;
    wm_repaint(menu_rect());
    wm_taskbar_repaint();
}

static void draw_menu_icon(int action, int32_t x, int32_t y) {
    if (action >= ACT_PROGRAM_BASE) {
        gfx_fill(x, y, 64, 16, C_ELF_ICON);
        gfx_text(x, y, ".elf", C_WHITE);
        return;
    }
    switch (action) {
    case ACT_TERMINAL:
        gfx_fill(x, y, 28, 16, C_BLACK);
        gfx_frame(x, y, 28, 16, C_WHITE);
        gfx_text(x + 4, y, ">", UINT32_C(0xff70e080));
        break;
    case ACT_FILES:
        gfx_fill(x, y + 2, 10, 4, C_FOLDER);
        gfx_fill(x, y + 5, 24, 11, UINT32_C(0xfff2c14e));
        gfx_frame(x, y + 5, 24, 11, UINT32_C(0xff9a7415));
        break;
    case ACT_EDITOR:
        gfx_fill(x + 5, y, 18, 16, C_WHITE);
        gfx_frame(x + 5, y, 18, 16, C_SHADOW);
        gfx_fill(x + 8, y + 4, 12, 1, C_SHADOW);
        gfx_fill(x + 8, y + 8, 12, 1, C_SHADOW);
        gfx_fill(x + 8, y + 12, 9, 1, C_SHADOW);
        break;
    case ACT_CODE:
        gfx_fill(x + 3, y, 22, 16, UINT32_C(0xff182331));
        gfx_frame(x + 3, y, 22, 16, UINT32_C(0xff70d6dc));
        gfx_text(x + 5, y, "{}", C_WHITE);
        break;
    case ACT_TASKS:
        gfx_fill(x + 3, y + 9, 5, 7, C_OK);
        gfx_fill(x + 11, y + 5, 5, 11, C_WARN);
        gfx_fill(x + 19, y + 1, 5, 15, C_ERR);
        break;
    case ACT_SYSINFO:
        gfx_fill(x + 4, y, 18, 16, C_ACCENT);
        gfx_frame(x + 4, y, 18, 16, C_WHITE);
        gfx_text(x + 8, y, "i", C_WHITE);
        break;
    case ACT_CALC:
        gfx_fill(x + 3, y, 22, 16, C_PANEL_HI);
        gfx_frame(x + 3, y, 22, 16, C_WHITE);
        gfx_fill(x + 7, y + 3, 14, 2, C_WHITE);
        gfx_fill(x + 7, y + 8, 3, 3, C_BTN);
        gfx_fill(x + 13, y + 8, 3, 3, C_BTN);
        gfx_fill(x + 19, y + 8, 2, 3, C_BTN);
        break;
    case ACT_RUN:
        gfx_fill(x + 3, y, 22, 16, C_PANEL_HI);
        gfx_text(x + 7, y, ">", C_WHITE);
        break;
    default:
        gfx_fill(x + 3, y, 22, 16, C_ICON_GRAY);
        gfx_text(x + 7, y, "?", C_WHITE);
        break;
    }
}

void menu_draw(rect_t area) {
    if (!g_open) return;
    const rect_t m = menu_rect();
    const rect_t clip = rect_intersect(area, m);
    if (rect_empty(clip)) return;
    gfx_clip(clip);
    gfx_fill(m.x, m.y, m.w, m.h, C_PANEL);
    gfx_frame(m.x, m.y, m.w, m.h, C_PANEL_HI);
    for (int row = 0; row < row_count(); ++row) {
        const char *label = 0;
        const int act = row_action(row, &label);
        const rect_t r = row_rect(row);
        if (act == SEP) {
            gfx_fill(r.x + 4, r.y + ITEM_H / 2, r.w - 8, 1, C_PANEL_HI);
            continue;
        }
        if (row == g_sel) gfx_fill(r.x, r.y, r.w, r.h, C_ACCENT);
        draw_menu_icon(act, r.x + 8, r.y + (ITEM_H - 16) / 2);
        gfx_text(r.x + (act >= ACT_PROGRAM_BASE ? 80 : 44),
                 r.y + (ITEM_H - FONT_H) / 2, label, C_WHITE);
    }
}

static int row_at(int32_t x, int32_t y) {
    for (int row = 0; row < row_count(); ++row)
        if (rect_hit(row_rect(row), x, y)) return row;
    return -1;
}

static void set_sel(int sel) {
    if (sel == g_sel) return;
    const int old = g_sel;
    g_sel = sel;
    if (old >= 0) wm_repaint(row_rect(old));
    if (sel >= 0) wm_repaint(row_rect(sel));
}

static void activate(int row) {
    const char *label = 0;
    const int act = row_action(row, &label);
    if (act == SEP) return;
    (void)label;
    menu_close();
    app_launch(act);
}

int menu_mouse(const struct mouse_event *e) {
    if (e->type == MOUSE_MOVE) {
        set_sel(row_at(e->x, e->y));
        return 1;
    }
    if (e->type != MOUSE_DOWN) return 0;
    const int row = row_at(e->x, e->y);
    if (row >= 0) {
        activate(row);
        return 1;
    }
    /* Clicking the Start button again toggles; anywhere else just dismisses. */
    menu_close();
    return 1;
}

int menu_key(int key) {
    const int n = row_count();
    if (key == 0x1b) {
        menu_close();
        return 1;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        int s = g_sel;
        for (int tries = 0; tries < n; ++tries) {
            s = key == KEY_UP ? (s <= 0 ? n - 1 : s - 1) : (s + 1) % n;
            const char *l = 0;
            if (row_action(s, &l) != SEP) break;
        }
        set_sel(s);
        return 1;
    }
    if (key == '\n' && g_sel >= 0) {
        activate(g_sel);
        return 1;
    }
    return 1;
}

/* ---------------------------------------------------------------- app launch */

static void confirm_reboot(void *ctx, const char *t) {
    (void)ctx; (void)t;
    desk_power(1);
}

static void confirm_shutdown(void *ctx, const char *t) {
    (void)ctx; (void)t;
    desk_power(0);
}

static void run_done(void *ctx, const char *text) {
    (void)ctx;
    desk_run_line(text);
}

void app_launch(int action) {
    static const char *const shell_argv[1] = {"/userspace/bin/shell/shell.elf"};
    switch (action) {
    case ACT_TERMINAL:
        (void)desk_run(shell_argv[0], 1, shell_argv, 0);
        break;
    case ACT_FILES: launch_builtin("files"); break;
    case ACT_EDITOR: launch_builtin("editor"); break;
    case ACT_CODE: launch_builtin("code"); break;
    case ACT_TASKS: launch_builtin("taskmgr"); break;
    case ACT_SYSINFO: launch_builtin("sysinfo"); break;
    case ACT_CALC: launch_builtin("calculator"); break;
    case ACT_GEOMETRY: launch_builtin("geometry"); break;
    case ACT_RUN:
        dlg_prompt("Run program (name or /path.elf, then arguments):", "",
                   run_done, 0);
        break;
    case ACT_EXIT: g_desk.quit = 1; break;
    case ACT_REBOOT: dlg_confirm("Reboot the machine?", confirm_reboot, 0); break;
    case ACT_SHUTDOWN:
        dlg_confirm("Shut down the machine?", confirm_shutdown, 0);
        break;
    default:
        if (action >= ACT_PROGRAM_BASE && action - ACT_PROGRAM_BASE < g_prog_n) {
            char path[96];
            const char *name = g_prog[action - ACT_PROGRAM_BASE];
            d_strcpy(path, sizeof(path), "/userspace/bin/");
            d_strcat(path, sizeof(path), name);
            d_strcat(path, sizeof(path), "/");
            d_strcat(path, sizeof(path), name);
            d_strcat(path, sizeof(path), ".elf");
            (void)desk_run(path, 0, 0, 1);
        }
        break;
    }
}

/* ------------------------------------------------------------------- icons */

#define ICON_CELL_W 96
#define ICON_CELL_H 84
#define N_ICONS 7

static const struct {
    const char *label;
    int action;
} k_icons[N_ICONS] = {
    {"Term", ACT_TERMINAL}, {"Files", ACT_FILES}, {"Edit", ACT_EDITOR},
    {"Tasks", ACT_TASKS},   {"Info", ACT_SYSINFO}, {"Calc", ACT_CALC},
    {"Geometry", ACT_GEOMETRY},
};

static rect_t g_icon_cells[N_ICONS];
static int g_icon_layout_ready;
static int g_icon_sel = -1;
static int g_icon_drag = -1;
static int g_icon_drag_moved;
static int32_t g_icon_drag_x, g_icon_drag_y;
static int32_t g_icon_drag_offset_x, g_icon_drag_offset_y;

static void icons_ensure_layout(void) {
    if (g_icon_layout_ready) return;
    int rows = (g_desk.work.h - 12) / ICON_CELL_H;
    if (rows < 1) rows = 1;
    for (int i = 0; i < N_ICONS; ++i)
        g_icon_cells[i] = rect_make(8 + (i / rows) * ICON_CELL_W,
                                    12 + (i % rows) * ICON_CELL_H,
                                    ICON_CELL_W, ICON_CELL_H);
    g_icon_layout_ready = 1;
}

static rect_t icon_cell(int i) {
    icons_ensure_layout();
    return g_icon_cells[i];
}

static void draw_icon_art(int i, int32_t x, int32_t y) {
    switch (i) {
    case 0:
        gfx_fill(x, y, 48, 48, UINT32_C(0xff1b1f24));
        gfx_frame(x, y, 48, 48, C_WHITE);
        gfx_text(x + 8, y + 16, ">_", UINT32_C(0xff70e080));
        break;
    case 1:
        gfx_fill(x + 2, y + 8, 20, 8, C_FOLDER);
        gfx_fill(x + 2, y + 14, 44, 28, UINT32_C(0xfff2c14e));
        gfx_frame(x + 2, y + 14, 44, 28, UINT32_C(0xff9a7415));
        break;
    case 2:
        gfx_fill(x + 8, y + 2, 32, 44, C_WHITE);
        gfx_frame(x + 8, y + 2, 32, 44, C_SHADOW);
        for (int32_t l = 0; l < 5; ++l) gfx_fill(x + 13, y + 10 + l * 7, 22, 3, C_SHADOW);
        break;
    case 3:
        gfx_fill(x, y, 48, 48, UINT32_C(0xff1b1f24));
        gfx_fill(x + 6, y + 30, 8, 12, C_OK);
        gfx_fill(x + 18, y + 18, 8, 24, C_WARN);
        gfx_fill(x + 30, y + 8, 8, 34, C_ERR);
        break;
    case 4:
        gfx_fill(x, y, 48, 48, C_ACCENT);
        gfx_frame(x, y, 48, 48, C_WHITE);
        gfx_text(x + 16, y + 16, "i", C_WHITE);
        break;
    case 5:
        gfx_fill(x, y, 48, 48, UINT32_C(0xff303640));
        gfx_frame(x, y, 48, 48, C_WHITE);
        for (int32_t r = 0; r < 3; ++r)
            for (int32_t c = 0; c < 3; ++c)
                gfx_fill(x + 6 + c * 14, y + 6 + r * 14, 10, 10,
                         c == 2 ? C_WARN : C_BTN);
        break;
    default:
        gfx_fill(x, y, 48, 48, UINT32_C(0xff182331));
        gfx_frame(x, y, 48, 48, C_WHITE);
        gfx_frame(x + 9, y + 11, 27, 27, UINT32_C(0xff70d6dc));
        gfx_frame(x + 15, y + 5, 27, 27, C_WARN);
        gfx_fill(x + 7, y + 9, 6, 6, C_WHITE);
        gfx_fill(x + 33, y + 3, 6, 6, C_WHITE);
        gfx_fill(x + 34, y + 31, 6, 6, C_WHITE);
        gfx_fill(x + 13, y + 37, 6, 6, C_WHITE);
        break;
    }
}

void icons_draw(rect_t area) {
    for (int i = 0; i < N_ICONS; ++i) {
        const rect_t cell = icon_cell(i);
        if (!rect_overlap(cell, area)) continue;
        if (i == g_icon_sel) gfx_fill(cell.x, cell.y, cell.w, cell.h, C_ICON_SEL);
        draw_icon_art(i, cell.x + 24, cell.y + 4);
        const int32_t tw = (int32_t)d_strlen(k_icons[i].label) * FONT_W;
        gfx_text(cell.x + (cell.w - tw) / 2, cell.y + 58, k_icons[i].label, C_WHITE);
    }
}

int icons_mouse(const struct mouse_event *e) {
    if (e->type == MOUSE_UP && g_icon_drag >= 0) {
        g_icon_drag = -1;
        g_icon_drag_moved = 0;
        return 1;
    }

    if (e->type == MOUSE_MOVE && g_icon_drag >= 0) {
        if (!(e->buttons & WELOVEC_POINTER_BUTTON_LEFT)) {
            g_icon_drag = -1;
            g_icon_drag_moved = 0;
            return 1;
        }
        const int32_t dx = e->x - g_icon_drag_x;
        const int32_t dy = e->y - g_icon_drag_y;
        if (!g_icon_drag_moved && (dx > 4 || dx < -4 || dy > 4 || dy < -4))
            g_icon_drag_moved = 1;
        if (!g_icon_drag_moved) return 1;

        rect_t old = g_icon_cells[g_icon_drag];
        rect_t moved = old;
        moved.x = e->x - g_icon_drag_offset_x;
        moved.y = e->y - g_icon_drag_offset_y;
        if (moved.x < 0) moved.x = 0;
        if (moved.y < 0) moved.y = 0;
        if (moved.x + moved.w > g_desk.width)
            moved.x = g_desk.width - moved.w;
        if (moved.y + moved.h > g_desk.work.h)
            moved.y = g_desk.work.h - moved.h;
        if (moved.x != old.x || moved.y != old.y) {
            g_icon_cells[g_icon_drag] = moved;
            wm_repaint(rect_union(old, moved));
        }
        return 1;
    }

    if (e->type != MOUSE_DOWN && e->type != MOUSE_MOVE) return 0;
    int hit = -1;
    for (int i = 0; i < N_ICONS; ++i)
        if (rect_hit(icon_cell(i), e->x, e->y)) hit = i;
    if (hit != g_icon_sel) {
        const int old = g_icon_sel;
        g_icon_sel = hit;
        if (old >= 0) wm_repaint(icon_cell(old));
        if (hit >= 0) wm_repaint(icon_cell(hit));
    }
    if (hit >= 0 && e->clicks == 2) {
        app_launch(k_icons[hit].action);
        return 1;
    }
    if (e->type == MOUSE_DOWN && hit >= 0) {
        g_icon_drag = hit;
        g_icon_drag_moved = 0;
        g_icon_drag_x = e->x;
        g_icon_drag_y = e->y;
        g_icon_drag_offset_x = e->x - g_icon_cells[hit].x;
        g_icon_drag_offset_y = e->y - g_icon_cells[hit].y;
    }
    return hit >= 0;
}

int icons_drag_active(void) { return g_icon_drag >= 0; }
