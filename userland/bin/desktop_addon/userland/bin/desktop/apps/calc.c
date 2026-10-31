// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the desktop calculator.

#include "desktop/include/desktop.h"

/* Four-function integer calculator (64-bit, wraps on overflow). Works with the
 * mouse and with the keyboard: digits, + - * / %, Enter or '=' to evaluate,
 * Backspace to delete a digit, 'c' to clear. */

struct calc_state {
    int64_t acc;
    int64_t cur;
    char op;
    uint8_t fresh;       /* next digit starts a new number */
    uint8_t error;
};
APP_STATE_CHECK(struct calc_state);

static const char *const k_keys[5][4] = {
    {"C", "<", "%", "/"},
    {"7", "8", "9", "*"},
    {"4", "5", "6", "-"},
    {"1", "2", "3", "+"},
    {"0", "+/-", "=", "="},
};

static int64_t apply(struct calc_state *s, int64_t a, char op, int64_t b) {
    const uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    switch (op) {
    case '+': return (int64_t)(ua + ub);
    case '-': return (int64_t)(ua - ub);
    case '*': return (int64_t)(ua * ub);
    case '/':
    case '%':
        if (b == 0 || (a == INT64_MIN && b == -1)) {
            s->error = 1;
            return 0;
        }
        return op == '/' ? a / b : a % b;
    default: return b;
    }
}

static void press(struct calc_state *s, char k) {
    if (s->error && k != 'C') return;
    if (k >= '0' && k <= '9') {
        if (s->fresh) { s->cur = 0; s->fresh = 0; }
        const uint64_t next = (uint64_t)s->cur * 10 + (uint64_t)(k - '0');
        if (s->cur < 100000000000000000LL && s->cur > -100000000000000000LL)
            s->cur = s->cur < 0 ? -(int64_t)next : (int64_t)next;
    } else if (k == 'C') {
        s->acc = s->cur = 0;
        s->op = 0;
        s->fresh = 1;
        s->error = 0;
    } else if (k == '<') {
        if (!s->fresh) s->cur /= 10;
    } else if (k == 'N') {
        s->cur = -s->cur;
    } else if (k == '=' || k == '+' || k == '-' || k == '*' || k == '/' || k == '%') {
        if (s->op && !s->fresh) s->cur = apply(s, s->acc, s->op, s->cur);
        s->acc = s->cur;
        s->op = (k == '=') ? 0 : k;
        s->fresh = 1;
    }
}

static int open(struct window *w, const char *arg) {
    struct calc_state *s = w->state;
    (void)arg;
    s->fresh = 1;
    return 0;
}

static rect_t key_rect(rect_t c, int row, int col) {
    const int32_t top = 64;
    const int32_t kw = (c.w - 10) / 4;
    const int32_t kh = (c.h - top - 6) / 5;
    int32_t span = 1;
    if (row == 4 && col == 2) span = 2;
    return rect_make(c.x + 4 + col * kw, c.y + top + row * kh, kw * span - 4, kh - 4);
}

static void draw(struct window *w, rect_t c) {
    struct calc_state *s = w->state;
    ui_sunken(rect_make(c.x + 4, c.y + 6, c.w - 8, 48), C_WHITE);
    char text[32];
    if (s->error) d_strcpy(text, sizeof(text), "Error");
    else if (s->cur < 0) {
        text[0] = '-';
        d_utoa((uint64_t)0 - (uint64_t)s->cur, text + 1);
    } else {
        d_utoa((uint64_t)s->cur, text);
    }
    ui_text_right(c.x + c.w - 14, c.y + 22, text, C_TEXT);
    if (s->op) {
        const char o[2] = {s->op, 0};
        gfx_text(c.x + 12, c.y + 10, o, C_TEXT_DIM);
    }
    for (int r = 0; r < 5; ++r)
        for (int k = 0; k < 4; ++k) {
            if (r == 4 && k == 3) continue;
            ui_button(key_rect(c, r, k), k_keys[r][k], 1);
        }
}

static char key_char(const char *label) {
    if (label[1]) return 'N';         /* "+/-" */
    return label[0];
}

static void mouse(struct window *w, const struct mouse_event *e) {
    struct calc_state *s = w->state;
    if (e->type != MOUSE_DOWN) return;
    const rect_t full = wm_client(w);
    const rect_t c = rect_make(0, 0, full.w, full.h);
    for (int r = 0; r < 5; ++r)
        for (int k = 0; k < 4; ++k) {
            if (r == 4 && k == 3) continue;
            if (rect_hit(key_rect(c, r, k), e->x, e->y)) {
                press(s, key_char(k_keys[r][k]));
                wm_invalidate(w);
                return;
            }
        }
}

static int key(struct window *w, int k) {
    struct calc_state *s = w->state;
    char c = 0;
    if ((k >= '0' && k <= '9') || k == '+' || k == '-' || k == '*' || k == '/' || k == '%')
        c = (char)k;
    else if (k == '=' || k == '\n') c = '=';
    else if (k == 'c' || k == 'C') c = 'C';
    else if (k == '\b' || k == 0x7f) c = '<';
    else return 0;
    press(s, c);
    wm_invalidate(w);
    return 1;
}

const struct app_ops app_calc = {
    "Calculator", 336, 380, 336, 300, 1, open, 0, 0, draw, mouse, key, 0, 0, 0, 0,
};
