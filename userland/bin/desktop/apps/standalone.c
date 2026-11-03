// Written by [@saphhic](https://github.com/saphhic)
// Description: Runs one desktop application as a windowed user process.

#include "desktop/include/desktop.h"
#include "user/framebuffer.h"
#include "user/input.h"
#include "user/security.h"
#include "user/stdio.h"
#include "user/syscall.h"
#include "user/time.h"

extern const struct app_ops *desktop_app_target(void);

struct desk_state g_desk;

static void fail(const char *message) {
    (void)welovec_write(message, d_strlen(message));
    welovec_exit(1);
}

static int initialize_desktop_surface(void) {
    struct welovec_fb_info fb;
    if (welovec_fb_get_info(&fb) < 0 || !fb.width || !fb.height ||
        fb.width > INT32_MAX || fb.height > INT32_MAX) return 0;
    g_desk.width = (int32_t)fb.width;
    g_desk.height = (int32_t)fb.height;
    g_desk.work = rect_make(0, 0, g_desk.width, g_desk.height);
    g_desk.taskbar = rect_make(0, g_desk.height, g_desk.width, 0);
    g_desk.now = welovec_getticks();
    d_strcpy(g_desk.user, sizeof(g_desk.user), "guest");

    struct welovec_user_status status;
    if (welovec_get_user_status(&status) == 0) {
        d_strcpy(g_desk.user, sizeof(g_desk.user), status.name);
        g_desk.uid = status.uid;
        g_desk.gid = status.gid;
    }
    g_desk.admin = welovec_is_admin();
    return 1;
}

void userland_main(uint64_t argc, char **argv) {
    const struct app_ops *app = desktop_app_target();
    if (!app || !initialize_desktop_surface())
        fail("desktop application: framebuffer unavailable\n");

    gfx_init();
    wm_init_embedded();
    input_init();
    if (!wm_open(app, argc > 1 ? argv[1] : 0))
        fail("desktop application: could not open\n");
    wm_full_repaint();

    uint64_t last_second = g_desk.now;
    for (;;) {
        g_desk.now = welovec_getticks();
        if (g_desk.now - last_second >= 100) {
            last_second = g_desk.now;
            struct welovec_fb_info fb;
            if (welovec_fb_get_info(&fb) == 0 && fb.width <= INT32_MAX &&
                fb.height <= INT32_MAX &&
                ((int32_t)fb.width != g_desk.width ||
                 (int32_t)fb.height != g_desk.height))
                (void)wm_embedded_resize((int32_t)fb.width,
                                         (int32_t)fb.height);
            wm_tick(g_desk.now);
        }
        wm_refresh_windows();
        const int busy = input_pump();
        if (!busy) (void)welovec_sleep(1);
    }
}
