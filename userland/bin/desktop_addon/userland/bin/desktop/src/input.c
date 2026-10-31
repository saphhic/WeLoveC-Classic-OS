// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Translates keyboard and mouse events for the desktop interface.

#include "desktop/include/desktop.h"
#include "user/input.h"
#include "user/syscall.h"

/*
 * Input pump. Pointer events are polled in batches (relative devices send many
 * small deltas), reduced to one cursor update per batch, and turned into
 * window-system events. Keyboard bytes come from the global character queue;
 * arrow keys arrive as ESC [ A..D sequences and are decoded here.
 */

#define CURSOR_W 12
#define CURSOR_H 16
#define DOUBLE_CLICK_TICKS 40u
#define DOUBLE_CLICK_SLOP 5

static const char *const k_cursor[CURSOR_H] = {
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#.....#####",
    "#..#..#     ",
    "#.# #..#    ",
    "##  #..#    ",
    "#    #..#   ",
    "     ###    ",
};

static int32_t g_new_x, g_new_y;      /* position while a batch is processed */
static uint32_t g_buttons;
static uint64_t g_last_down;
static int32_t g_last_x, g_last_y;

static int g_esc_state;               /* 0 none, 1 saw ESC, 2 inside ESC [ */
static int32_t g_esc_param[2];
static int g_esc_count;

rect_t cursor_rect(void) {
    return rect_make(g_desk.ptr_x, g_desk.ptr_y, CURSOR_W, CURSOR_H);
}

void cursor_draw(void) {
    if (wm_cursor_resize()) {
        for (int32_t i = 0; i < 9; ++i) {
            gfx_fill(g_desk.ptr_x + 2 + i, g_desk.ptr_y + 13 - i,
                     2, 2, C_BLACK);
            if (i > 0 && i < 8)
                gfx_fill(g_desk.ptr_x + 3 + i, g_desk.ptr_y + 13 - i,
                         1, 1, C_WHITE);
        }
        return;
    }
    for (int32_t row = 0; row < CURSOR_H; ++row) {
        const char *line = k_cursor[row];
        int32_t col = 0;
        while (col < CURSOR_W) {
            const char c = line[col];
            if (c != '#' && c != '.') {
                ++col;
                continue;
            }
            const int32_t start = col;
            while (col < CURSOR_W && line[col] == c) ++col;
            gfx_fill(g_desk.ptr_x + start, g_desk.ptr_y + row, col - start, 1,
                     c == '#' ? C_BLACK : C_WHITE);
        }
    }
}

void input_init(void) {
    g_new_x = g_desk.ptr_x = g_desk.width / 2;
    g_new_y = g_desk.ptr_y = g_desk.height / 2;
    g_buttons = 0;
    g_last_down = 0;
    g_esc_state = 0;
    input_flush();
}

void input_flush(void) {
    welovec_pointer_event events[16];
    char keys[64];
    while (welovec_pointer_poll(events, 16) > 0) {
    }
    while (welovec_input_poll(keys, sizeof(keys)) > 0) {
    }
    g_esc_state = 0;
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

/* Repaint the cursor damage as one clipped region, then dispatch motion. */
static int flush_move(void) {
    if (g_new_x == g_desk.ptr_x && g_new_y == g_desk.ptr_y) return 0;
    const rect_t old = cursor_rect();
    g_desk.ptr_x = g_new_x;
    g_desk.ptr_y = g_new_y;
    const rect_t current = cursor_rect();
    if (rect_overlap(old, current)) {
        wm_repaint(rect_union(old, current));
    } else {
        wm_repaint(old);
        wm_repaint(current);
    }
    struct mouse_event e = {MOUSE_MOVE, g_desk.ptr_x, g_desk.ptr_y,
                            g_buttons, 0, 0};
    wm_mouse(&e);
    return 1;
}

static void emit_button(int down) {
    struct mouse_event e = {down ? MOUSE_DOWN : MOUSE_UP, g_desk.ptr_x,
                            g_desk.ptr_y, g_buttons, 0, 1};
    if (down) {
        if (g_desk.now - g_last_down <= DOUBLE_CLICK_TICKS &&
            iabs(e.x - g_last_x) <= DOUBLE_CLICK_SLOP &&
            iabs(e.y - g_last_y) <= DOUBLE_CLICK_SLOP) {
            e.clicks = 2;
            g_last_down = 0;
        } else {
            g_last_down = g_desk.now ? g_desk.now : 1;
            g_last_x = e.x;
            g_last_y = e.y;
        }
    }
    wm_mouse(&e);
}

static int pump_pointer(void) {
    int activity = 0;
    welovec_pointer_event events[16];
    for (;;) {
        const int64_t n = welovec_pointer_poll(events, 16);
        if (n <= 0) break;
        activity = 1;
        for (int64_t i = 0; i < n; ++i) {
            const welovec_pointer_event *ev = &events[i];
            if (ev->flags & WELOVEC_POINTER_EVENT_ABSOLUTE) {
                g_new_x = (int32_t)((int64_t)ev->absolute_x *
                                    (g_desk.width - 1) / 65535);
                g_new_y = (int32_t)((int64_t)ev->absolute_y *
                                    (g_desk.height - 1) / 65535);
            }
            if (ev->flags & WELOVEC_POINTER_EVENT_RELATIVE) {
                g_new_x += ev->delta_x;
                g_new_y += ev->delta_y;
            }
            g_new_x = clamp(g_new_x, 0, g_desk.width - 1);
            g_new_y = clamp(g_new_y, 0, g_desk.height - 1);

            const uint32_t was = g_buttons & WELOVEC_POINTER_BUTTON_LEFT;
            const uint32_t now = ev->buttons & WELOVEC_POINTER_BUTTON_LEFT;
            g_buttons = ev->buttons;
            if (ev->wheel) {
                /* PS/2 reports "toward the user" as positive; virtio and HID
                 * report "away". Normalise to: positive scrolls up. */
                int32_t wheel = ev->wheel;
                if (ev->source == WELOVEC_POINTER_SOURCE_PS2) wheel = -wheel;
                (void)flush_move();
                struct mouse_event e = {MOUSE_WHEEL, g_desk.ptr_x, g_desk.ptr_y,
                                        g_buttons, wheel, 0};
                wm_mouse(&e);
            }
            if (was != now) {
                (void)flush_move();
                emit_button(now != 0);
            }
        }
        if (n < 16) break;
    }
    (void)flush_move();
    g_desk.ptr_buttons = g_buttons;
    return activity;
}

static void send_key(int key) { wm_key(key); }

static void feed_key_byte(unsigned char c) {
    if (g_esc_state == 0) {
        if (c == 0x1b) g_esc_state = 1;
        else send_key(c);
        return;
    }
    if (g_esc_state == 1) {
        if (c == '[') {
            g_esc_state = 2;
            g_esc_param[0] = g_esc_param[1] = 0;
            g_esc_count = 0;
        } else {
            g_esc_state = 0;
            send_key(0x1b);
            feed_key_byte(c);
        }
        return;
    }
    if (c >= '0' && c <= '9') {
        g_esc_param[g_esc_count] = g_esc_param[g_esc_count] * 10 + (c - '0');
    } else if (c == ';') {
        if (g_esc_count < 1) ++g_esc_count;
    } else {
        const int ctrl = g_esc_count >= 1 && g_esc_param[1] == 5;
        g_esc_state = 0;
        switch (c) {
        case 'A': send_key(ctrl ? KEY_CTRL_UP : KEY_UP); break;
        case 'B': send_key(ctrl ? KEY_CTRL_DOWN : KEY_DOWN); break;
        case 'C': send_key(KEY_RIGHT); break;
        case 'D': send_key(KEY_LEFT); break;
        default: break;
        }
    }
}

static int pump_keyboard(void) {
    int activity = 0;
    char keys[64];
    for (;;) {
        const int64_t n = welovec_input_poll(keys, sizeof(keys));
        if (n <= 0) break;
        activity = 1;
        for (int64_t i = 0; i < n; ++i) feed_key_byte((unsigned char)keys[i]);
    }
    /* Arrow sequences are queued atomically, so a lone ESC at the end of a
     * poll really is the Escape key. */
    if (g_esc_state == 1) {
        g_esc_state = 0;
        send_key(0x1b);
        activity = 1;
    }
    return activity;
}

int input_pump(void) {
    int activity = pump_pointer();
    activity |= pump_keyboard();
    return activity;
}
