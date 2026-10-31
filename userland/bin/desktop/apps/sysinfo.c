// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Displays system information on the desktop.

#include "desktop/include/desktop.h"
#include "user/framebuffer.h"
#include "user/input.h"
#include "user/network.h"
#include "user/time.h"

/* Read-only status window; the two time-dependent lines refresh every second. */

struct sysinfo_state {
    struct welovec_network_info net;
    uint8_t net_ok;
    uint32_t pointers;
};
APP_STATE_CHECK(struct sysinfo_state);

static int open(struct window *w, const char *arg) {
    struct sysinfo_state *s = w->state;
    (void)arg;
    s->net_ok = welovec_network_get_info(&s->net) == 0;
    s->pointers = welovec_pointer_sources();
    return 0;
}

static void ip(char *out, uint32_t cap, const uint8_t a[4]) {
    char num[24];
    out[0] = '\0';
    for (int i = 0; i < 4; ++i) {
        d_utoa(a[i], num);
        d_strcat(out, cap, num);
        if (i < 3) d_strcat(out, cap, ".");
    }
}

static void row(rect_t c, int line, const char *label, const char *value) {
    const int32_t y = c.y + 10 + line * 22;
    gfx_text(c.x + 10, y, label, C_TEXT_DIM);
    gfx_text(c.x + 10 + 11 * FONT_W, y, value, C_TEXT);
}

static void draw(struct window *w, rect_t c) {
    struct sysinfo_state *s = w->state;
    char v[96], num[24];
    int n = 0;

    row(c, n++, "Desktop", "WeLoveC desktop " DESKTOP_VERSION);

    d_strcpy(v, sizeof(v), g_desk.user);
    d_strcat(v, sizeof(v), " (uid ");
    d_utoa(g_desk.uid, num);
    d_strcat(v, sizeof(v), num);
    d_strcat(v, sizeof(v), g_desk.admin ? ", admin)" : ")");
    row(c, n++, "User", v);

    d_utoa((uint64_t)g_desk.width, v);
    d_strcat(v, sizeof(v), " x ");
    d_utoa((uint64_t)g_desk.height, num);
    d_strcat(v, sizeof(v), num);
    row(c, n++, "Screen", v);

    const uint64_t secs = g_desk.now / 100;
    d_utoa(secs / 3600, v);
    d_strcat(v, sizeof(v), "h ");
    d_utoa((secs / 60) % 60, num);
    d_strcat(v, sizeof(v), num);
    d_strcat(v, sizeof(v), "m ");
    d_utoa(secs % 60, num);
    d_strcat(v, sizeof(v), num);
    d_strcat(v, sizeof(v), "s");
    row(c, n++, "Uptime", v);

    struct welovec_user_datetime t;
    if (welovec_rtc_get_utc(&t) == 0) {
        d_utoa((uint64_t)t.year, v);
        d_strcat(v, sizeof(v), "-");
        d_pad2(num, t.month);
        d_strcat(v, sizeof(v), num);
        d_strcat(v, sizeof(v), "-");
        d_pad2(num, t.day);
        d_strcat(v, sizeof(v), num);
        d_strcat(v, sizeof(v), " ");
        d_pad2(num, t.hour);
        d_strcat(v, sizeof(v), num);
        d_strcat(v, sizeof(v), ":");
        d_pad2(num, t.minute);
        d_strcat(v, sizeof(v), num);
        d_strcat(v, sizeof(v), ":");
        d_pad2(num, t.second);
        d_strcat(v, sizeof(v), num);
        d_strcat(v, sizeof(v), " UTC");
        row(c, n++, "Clock", v);
    }

    v[0] = '\0';
    if (s->pointers & WELOVEC_POINTER_SOURCE_PS2) d_strcat(v, sizeof(v), "PS/2 ");
    if (s->pointers & WELOVEC_POINTER_SOURCE_USB_HID) d_strcat(v, sizeof(v), "USB ");
    if (s->pointers & WELOVEC_POINTER_SOURCE_VIRTIO) d_strcat(v, sizeof(v), "virtio ");
    if (s->pointers & WELOVEC_POINTER_SOURCE_I2C_HID) d_strcat(v, sizeof(v), "I2C ");
    if (!v[0]) d_strcpy(v, sizeof(v), "none detected");
    row(c, n++, "Pointers", v);

    d_utoa(wm_window_count(), v);
    d_strcat(v, sizeof(v), " of ");
    d_utoa(WIN_MAX, num);
    d_strcat(v, sizeof(v), num);
    row(c, n++, "Windows", v);

    if (s->net_ok && (s->net.flags & WELOVEC_NET_INFO_IPV4_READY)) {
        ip(v, sizeof(v), s->net.ipv4);
        row(c, n++, "IPv4", v);
        ip(v, sizeof(v), s->net.gateway);
        row(c, n++, "Gateway", v);
        ip(v, sizeof(v), s->net.dns_server);
        row(c, n++, "DNS", v);
    } else {
        row(c, n++, "Network", "not configured");
    }
}

static void tick(struct window *w, uint64_t ticks) {
    (void)ticks;
    wm_invalidate(w);
}

const struct app_ops app_sysinfo = {
    "System info", 600, 300, 440, 260, 1, open, 0, 0, draw, 0, 0, tick, 0, 0, 0,
};
