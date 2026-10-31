// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Is the entry point for the desktop component.

#include <stdint.h>

#include "desktop/include/desktop.h"
#include "user/framebuffer.h"
#include "user/power.h"
#include "user/security.h"
#include "user/stdio.h"
#include "user/syscall.h"

/*
 * WeLoveC lightweight desktop (userland/bin/desktop).
 *
 *   main.c        startup and the event loop
 *   src/gfx.c     clipped drawing primitives (+ src/font.c clip fallback glyphs)
 *   src/ui.c      buttons / scrollbar helpers
 *   src/wm.c      windows, focus, drag outline, taskbar, repaint
 *   src/input.c   pointer + keyboard decoding, software cursor
 *   src/menu.c    Start menu, desktop icons, app launcher
 *   src/dialog.c  modal prompt / confirm / message
 *   src/launch.c  windowed ELF runs (terminal, programs), power
 *   src/fsutil.c  directory listing and file copy helpers
 *   apps/ *.c     Files, Editor, Task manager, System info, Calculator
 */

struct desk_state g_desk;

static void fatal(const char *text) {
    (void)welovec_write(text, d_strlen(text));
    welovec_exit(1);
}

static int main_init(void) {
    struct welovec_fb_info fb;
    if (welovec_fb_get_info(&fb) < 0 || fb.width < 480 || fb.height < 320) return -1;
    g_desk.width = (int32_t)fb.width;
    g_desk.height = (int32_t)fb.height;
    g_desk.taskbar = rect_make(0, g_desk.height - TASKBAR_H, g_desk.width, TASKBAR_H);
    g_desk.work = rect_make(0, 0, g_desk.width, g_desk.height - TASKBAR_H);
    g_desk.vm = welovec_running_in_virtual_machine();
    g_desk.now = welovec_getticks();

    struct welovec_user_status st;
    d_strcpy(g_desk.user, sizeof(g_desk.user), "guest");
    if (welovec_get_user_status(&st) == 0) {
        d_strcpy(g_desk.user, sizeof(g_desk.user), st.name);
        g_desk.uid = st.uid;
        g_desk.gid = st.gid;
    }
    g_desk.admin = welovec_is_admin();
    return 0;
}

void userland_main(uint64_t argc, char **argv) {
    (void)argc;
    (void)argv;
    if (main_init() < 0) fatal("desktop: framebuffer unavailable or too small\n");

    gfx_init();
    wm_init();
    menu_init();
    input_init();
    wm_full_repaint();

    uint64_t last_second = g_desk.now;
    while (!g_desk.quit) {
        g_desk.now = welovec_getticks();
        if (g_desk.now - last_second >= 100) {
            last_second = g_desk.now;
            wm_tick(g_desk.now);
        }
        wm_refresh_windows();
        /* Repaint the pointer after app surfaces so it stays above their pixels. */
        const int busy = input_pump();
        if (!busy) (void)welovec_sleep(1);   /* idle: ~100 wakeups/s, no spinning */
    }

    wm_shutdown();
    (void)clear_screen_color(C_BLACK);
    (void)welovec_print_reset_cursor();
    welovec_exit(0);
}
