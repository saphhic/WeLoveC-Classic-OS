/*
 * CircuitLab is a small, standalone circuit editor and DC simulator for
 * WeLoveC. It is built as /userspace/bin/circuit_lab/circuit_lab.elf and can
 * run in its own desktop window.
 */

#include <stdint.h>

#include "user/framebuffer.h"
#include "user/input.h"
#include "user/syscall.h"

enum {
    MAX_PARTS = 32,
    MAX_WIRES = 64,
    MAX_TERMINALS = MAX_PARTS * 2,
    MAX_UNKNOWNS = 96,
    HEADER_H = 42,
    SIDEBAR_W = 172,
    FOOTER_H = 38,
    PART_BATTERY = 0,
    PART_RESISTOR,
    PART_LAMP,
    PART_SWITCH,
    TOOL_SELECT,
    TOOL_WIRE,
    STATUS_IDLE,
    STATUS_RUNNING,
    STATUS_SOLVED,
    STATUS_ERROR
};

struct part {
    int32_t x, y;
    uint8_t type;
    uint8_t closed;
};

struct wire { uint8_t a, b; };

static struct part parts[MAX_PARTS];
static struct wire wires[MAX_WIRES];
static uint32_t part_count, wire_count;
/* Default dimensions; the window's actual surface size replaces these at boot. */
static uint32_t screen_w = 636, screen_h = 393;
static uint32_t tool = TOOL_SELECT;
static int32_t selected = -1;
static int32_t wire_start = -1;
static int32_t drag_part = -1, drag_dx, drag_dy;
static uint8_t running;
static uint8_t solve_status = STATUS_IDLE;
static uint8_t left_was_down;
static uint64_t last_size_check;
static float terminal_voltage[MAX_TERMINALS];
static float part_current[MAX_PARTS];
static float matrix[MAX_UNKNOWNS][MAX_UNKNOWNS + 1];

static const uint32_t C_BG       = UINT32_C(0xff111923);
static const uint32_t C_HEADER   = UINT32_C(0xff1a2633);
static const uint32_t C_SIDE     = UINT32_C(0xff18222d);
static const uint32_t C_CANVAS   = UINT32_C(0xff101820);
static const uint32_t C_GRID     = UINT32_C(0xff1e2d39);
static const uint32_t C_TEXT     = UINT32_C(0xffeaf2f8);
static const uint32_t C_DIM      = UINT32_C(0xff94a8b7);
static const uint32_t C_ACCENT   = UINT32_C(0xff37c8a0);
static const uint32_t C_YELLOW   = UINT32_C(0xffffcc66);
static const uint32_t C_RED      = UINT32_C(0xffff7373);
static const uint32_t C_PART     = UINT32_C(0xffe1edf4);
static const uint32_t C_WIRE     = UINT32_C(0xff90a4b2);
static const uint32_t C_POWER    = UINT32_C(0xff43e39a);
static const uint32_t C_BUTTON   = UINT32_C(0xff283746);
static const uint32_t C_GROUND   = UINT32_C(0xff596b78);

static int32_t min_i32(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t max_i32(int32_t a, int32_t b) { return a > b ? a : b; }
static float abs_f(float value) { return value < 0.0f ? -value : value; }

static void text(int32_t x, int32_t y, const char *s, uint32_t color) {
    (void)welovec_fb_text(x, y, s, color);
}

static void line(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                 uint32_t color) {
    (void)welovec_fb_line(x0, y0, x1, y1, color);
}

static void rect(int32_t x, int32_t y, int32_t w, int32_t h,
                 uint32_t color) {
    if (w > 0 && h > 0) (void)welovec_fb_rect(x, y, w, h, color);
}

static void circle(int32_t x, int32_t y, int32_t radius, uint32_t color) {
    (void)welovec_fb_fill_circle(x, y, radius, color);
}

static int hit(int32_t x, int32_t y, int32_t rx, int32_t ry,
               int32_t rw, int32_t rh) {
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static int compact_ui(void) { return screen_w < 560; }
static int tiny_ui(void) { return screen_w < 400; }
static int32_t sidebar_width(void) {
    return screen_w < 400 ? 104 : (screen_w < 560 ? 128 : SIDEBAR_W);
}
static int32_t footer_height(void) { return screen_h < 220 ? 24 : FOOTER_H; }
static int32_t canvas_x(void) {
    return (int32_t)min_i32(sidebar_width() + 8, (int32_t)screen_w - 20);
}
static int32_t canvas_y(void) { return HEADER_H + 8; }
static int32_t canvas_right(void) { return (int32_t)screen_w - 8; }
static int32_t canvas_bottom(void) { return (int32_t)screen_h - footer_height() - 8; }

static void tool_rect(uint32_t index, int32_t *x, int32_t *y,
                      int32_t *w, int32_t *h) {
    const int32_t side = sidebar_width();
    if (screen_h < 220) {
        const int32_t cell_w = side / 2;
        *x = 6 + (int32_t)(index % 2) * cell_w;
        *y = 56 + (int32_t)(index / 2) * 27;
        *w = cell_w - 8;
        *h = 23;
    } else if (screen_h < 300) {
        *x = 8;
        *y = 50 + (int32_t)index * 27;
        *w = side - 16;
        *h = 23;
    } else {
        *x = 8;
        *y = 76 + (int32_t)index * 40;
        *w = side - 16;
        *h = 34;
    }
}

static uint32_t tool_kind(uint32_t index) {
    return index < 4 ? index : (index == 4 ? TOOL_WIRE : TOOL_SELECT);
}

static void control_rect(uint32_t index, int32_t *x, int32_t *w) {
    if (compact_ui()) {
        static const int32_t offsets[] = {200, 136, 72};
        static const int32_t widths[] = {58, 58, 64};
        *x = (int32_t)screen_w - offsets[index];
        *w = widths[index];
    } else {
        static const int32_t offsets[] = {368, 256, 128};
        static const int32_t widths[] = {104, 120, 120};
        *x = (int32_t)screen_w - offsets[index];
        *w = widths[index];
    }
}

static void part_bounds(int32_t *min_x, int32_t *max_x,
                        int32_t *min_y, int32_t *max_y) {
    const int32_t cx = canvas_x(), cy = canvas_y();
    const int32_t cw = canvas_right() - cx;
    const int32_t ch = canvas_bottom() - cy;
    *min_x = cx + (cw < 128 ? cw / 3 : 48);
    *max_x = canvas_right() - (cw < 128 ? cw / 3 : 48);
    *min_y = cy + (ch < 130 ? 30 : 38);
    *max_y = canvas_bottom() - (ch < 130 ? 38 : 54);
    if (*max_x < *min_x) *max_x = *min_x;
    if (*max_y < *min_y) *max_y = *min_y;
}

static int refresh_dimensions(void) {
    struct welovec_fb_info info;
    if (welovec_fb_get_info(&info) < 0 || !info.width || !info.height ||
        (info.width == screen_w && info.height == screen_h)) return 0;

    int32_t old_min_x, old_max_x, old_min_y, old_max_y;
    part_bounds(&old_min_x, &old_max_x, &old_min_y, &old_max_y);
    screen_w = info.width;
    screen_h = info.height;
    int32_t new_min_x, new_max_x, new_min_y, new_max_y;
    part_bounds(&new_min_x, &new_max_x, &new_min_y, &new_max_y);
    const int32_t old_x_span = max_i32(1, old_max_x - old_min_x);
    const int32_t old_y_span = max_i32(1, old_max_y - old_min_y);
    const int32_t new_x_span = new_max_x - new_min_x;
    const int32_t new_y_span = new_max_y - new_min_y;
    for (uint32_t i = 0; i < part_count; ++i) {
        const int64_t x_offset = (int64_t)(parts[i].x - old_min_x) * new_x_span;
        const int64_t y_offset = (int64_t)(parts[i].y - old_min_y) * new_y_span;
        parts[i].x = new_min_x + (int32_t)(x_offset / old_x_span);
        parts[i].y = new_min_y + (int32_t)(y_offset / old_y_span);
    }
    return 1;
}

static void draw_button(int32_t x, int32_t y, int32_t w, int32_t h,
                        const char *label, int active) {
    rect(x, y, w, h, active ? UINT32_C(0xff176b5a) : C_BUTTON);
    (void)welovec_fb_outline(x, y, w, h,
                             active ? C_ACCENT : UINT32_C(0xff405363));
    text(x + 8, y + (h - 16) / 2, label, C_TEXT);
}

static int32_t terminal_x(uint32_t part, uint32_t pin) {
    return parts[part].x + (pin ? 40 : -40);
}

static int32_t terminal_y(uint32_t part) { return parts[part].y; }

static void draw_wire_path(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                           uint32_t color) {
    int32_t mid = x0 + (x1 - x0) / 2;
    line(x0, y0, mid, y0, color);
    line(mid, y0, mid, y1, color);
    line(mid, y1, x1, y1, color);
}

static void draw_part(uint32_t index) {
    const struct part *p = &parts[index];
    const int32_t x = p->x, y = p->y;
    const uint32_t color = (selected == (int32_t)index) ? C_YELLOW : C_PART;
    line(x - 40, y, x - 18, y, color);
    line(x + 18, y, x + 40, y, color);
    circle(x - 40, y, 3, (wire_start == (int32_t)(index * 2)) ? C_YELLOW : color);
    circle(x + 40, y, 3, (wire_start == (int32_t)(index * 2 + 1)) ? C_YELLOW : color);

    if (p->type == PART_BATTERY) {
        line(x - 15, y - 16, x - 15, y + 16, color);
        line(x - 6, y - 9, x - 6, y + 9, color);
        line(x + 6, y - 16, x + 6, y + 16, color);
        line(x + 15, y - 9, x + 15, y + 9, color);
        text(x - 6, y - 34, "9V", C_YELLOW);
    } else if (p->type == PART_RESISTOR) {
        line(x - 18, y, x - 14, y, color);
        line(x + 14, y, x + 18, y, color);
        line(x - 14, y, x - 9, y - 9, color);
        line(x - 9, y - 9, x - 3, y + 9, color);
        line(x - 3, y + 9, x + 3, y - 9, color);
        line(x + 3, y - 9, x + 9, y + 9, color);
        line(x + 9, y + 9, x + 14, y, color);
        text(x - 14, y + 18, "100R", C_DIM);
    } else if (p->type == PART_LAMP) {
        const uint32_t lamp = running && solve_status == STATUS_SOLVED &&
            part_current[index] > 0.002f ? C_YELLOW : color;
        (void)welovec_fb_circle(x, y, 15, lamp);
        line(x - 8, y - 8, x + 8, y + 8, lamp);
        line(x - 8, y + 8, x + 8, y - 8, lamp);
        text(x - 15, y + 20,
             running && part_current[index] > 0.002f ? "ON" : "220R",
             running && part_current[index] > 0.002f ? C_YELLOW : C_DIM);
    } else {
        circle(x - 15, y, 3, color);
        circle(x + 15, y, 3, color);
        line(x - 15, y, p->closed ? x + 15 : x + 9,
             p->closed ? y : y - 14, color);
        text(x - 17, y + 20, p->closed ? "ON" : "OFF",
             p->closed ? C_POWER : C_DIM);
    }

    if (running && solve_status == STATUS_SOLVED && p->type != PART_LAMP) {
        char value[16];
        const int milliamps = (int)(part_current[index] * 1000.0f + 0.5f);
        value[0] = (char)('0' + (milliamps / 100) % 10);
        value[1] = (char)('0' + (milliamps / 10) % 10);
        value[2] = (char)('0' + milliamps % 10);
        value[3] = 'm'; value[4] = 'A'; value[5] = '\0';
        text(x - 20, y + 22, value, C_DIM);
    }
}

static void draw_tool_icon(uint32_t type, int32_t x, int32_t y) {
    const uint32_t c = C_TEXT;
    if (type == PART_BATTERY) {
        line(x, y - 10, x, y + 10, c);
        line(x + 8, y - 6, x + 8, y + 6, c);
        line(x - 12, y, x, y, c);
        line(x + 8, y, x + 19, y, c);
    } else if (type == PART_RESISTOR) {
        line(x - 16, y, x - 10, y, c);
        line(x + 10, y, x + 16, y, c);
        line(x - 10, y, x - 5, y - 6, c);
        line(x - 5, y - 6, x, y + 6, c);
        line(x, y + 6, x + 5, y - 6, c);
        line(x + 5, y - 6, x + 10, y, c);
    } else if (type == PART_LAMP) {
        (void)welovec_fb_circle(x, y, 10, c);
        line(x - 5, y - 5, x + 5, y + 5, c);
        line(x - 5, y + 5, x + 5, y - 5, c);
    } else if (type == PART_SWITCH) {
        circle(x - 9, y, 2, c);
        circle(x + 9, y, 2, c);
        line(x - 9, y, x + 7, y - 6, c);
    } else if (type == TOOL_WIRE) {
        line(x - 15, y + 7, x - 4, y + 7, c);
        line(x - 4, y + 7, x - 4, y - 7, c);
        line(x - 4, y - 7, x + 15, y - 7, c);
    } else {
        text(x - 7, y - 8, "A", c);
    }
}

static void draw(void) {
    const int32_t cx = canvas_x(), cy = canvas_y();
    const int32_t cr = canvas_right(), cb = canvas_bottom();
    /* This ELF runs on a private window surface. The desktop composites that
       surface, so drawing directly avoids claiming the kernel's global frame. */
    rect(0, 0, (int32_t)screen_w, (int32_t)screen_h, C_BG);
    rect(0, 0, (int32_t)screen_w, HEADER_H, C_HEADER);
    text(14, 12, "CIRCUIT LAB", C_ACCENT);
    if (!compact_ui()) text(210, 12, "Editor de circuitos DC", C_TEXT);

    const int32_t side = sidebar_width();
    const int32_t footer = footer_height();
    rect(0, HEADER_H, side, max_i32(0, (int32_t)screen_h - HEADER_H - footer), C_SIDE);
    if (screen_h >= 300) text(12, 54, "COMPONENTES", C_DIM);
    static const char *const wide_labels[] = {
        "Bateria", "Resistor", "Bombilla", "Switch", "Cable", "Elegir"
    };
    static const char *const compact_labels[] = {
        "BAT", "RES", "LMP", "SW", "WIRE", "SEL"
    };
    static const char *const tiny_labels[] = {"B", "R", "L", "S", "W", "A"};
    for (uint32_t i = 0; i < 6; ++i) {
        int32_t x, y, w, h;
        tool_rect(i, &x, &y, &w, &h);
        const uint32_t kind = tool_kind(i);
        const int active = tool == kind;
        rect(x, y, w, h,
             active ? UINT32_C(0xff176b5a) : C_BUTTON);
        (void)welovec_fb_outline(x, y, w, h,
                                 active ? C_ACCENT : UINT32_C(0xff405363));
        if (screen_h < 220) {
            text(x + (w - 16) / 2, y + (h - 16) / 2,
                 tiny_labels[i], active ? C_TEXT : C_DIM);
        } else {
            draw_tool_icon(kind, x + (side < 140 ? 16 : 24), y + h / 2);
            text(x + (side < 140 ? 34 : 49), y + (h - 16) / 2,
                 side < SIDEBAR_W ? compact_labels[i] : wide_labels[i],
                 active ? C_TEXT : C_DIM);
        }
    }

    rect(cx, cy, cr - cx, cb - cy, C_CANVAS);
    (void)welovec_fb_outline(cx, cy, cr - cx, cb - cy, UINT32_C(0xff304452));
    for (int32_t x = cx + 10; x < cr - 4; x += 20)
        line(x, cy + 1, x, cb - 2, C_GRID);
    for (int32_t y = cy + 10; y < cb - 4; y += 20)
        line(cx + 1, y, cr - 2, y, C_GRID);

    for (uint32_t i = 0; i < wire_count; ++i) {
        const uint32_t a = wires[i].a, b = wires[i].b;
        uint32_t color = C_WIRE;
        if (running && solve_status == STATUS_SOLVED) {
            const float v = terminal_voltage[a];
            color = abs_f(v) > 0.2f ? C_POWER : C_GROUND;
        }
        draw_wire_path(terminal_x(a / 2, a % 2), terminal_y(a / 2),
                       terminal_x(b / 2, b % 2), terminal_y(b / 2), color);
    }
    for (uint32_t i = 0; i < part_count; ++i) draw_part(i);

    if (wire_start >= 0) {
        text(cx + 12, cy + 10, "Elige el segundo terminal", C_YELLOW);
    } else if (part_count == 0) {
        text(cx + 20, cy + 24, "Pulsa EJEMPLO o elige un componente.", C_DIM);
        text(cx + 20, cy + 48, "Conecta los terminales con la herramienta Cable.", C_DIM);
    }

    const int32_t controls_y = 50;
    int32_t demo_x, demo_w, clear_x, clear_w, run_x, run_w;
    control_rect(0, &demo_x, &demo_w);
    control_rect(1, &clear_x, &clear_w);
    control_rect(2, &run_x, &run_w);
    draw_button(demo_x, controls_y, demo_w, 30,
                compact_ui() ? "DEM" : "DEMO", 0);
    draw_button(clear_x, controls_y, clear_w, 30,
                compact_ui() ? "CLR" : "BORRAR", 0);
    draw_button(run_x, controls_y, run_w, 30,
                compact_ui() ? (running ? "OFF" : "RUN")
                             : (running ? "PARAR" : "SIMULAR"), running);

    rect(0, (int32_t)screen_h - footer, (int32_t)screen_w, footer, C_HEADER);
    if (solve_status == STATUS_SOLVED && running)
        text(10, (int32_t)screen_h - (footer < 30 ? 19 : 27),
             compact_ui() ? "RUN: verde = tension" :
                            "Simulacion activa: verde = nodo con tension", C_POWER);
    else if (solve_status == STATUS_ERROR)
        text(10, (int32_t)screen_h - (footer < 30 ? 19 : 27),
             compact_ui() ? "Error de circuito" :
                            "Error de circuito: revisa fuente y conexiones", C_RED);
    else if (footer < 30)
        text(10, (int32_t)screen_h - 19,
             tiny_ui() ? "1-4  W cable  Espacio" :
                            "1-4 piezas  W cable  espacio simula", C_DIM);
    else if (compact_ui()) {
        text(10, (int32_t)screen_h - 33,
             tiny_ui() ? "1-4 piezas  W cable" : "1-4 pieza  W cable", C_DIM);
        text(10, (int32_t)screen_h - 17,
             tiny_ui() ? "Space run  C clear" : "Espacio simula  C limpia", C_DIM);
    }
    else {
        text(10, (int32_t)screen_h - 33, "1-4 pieza  W cable  Espacio simula", C_DIM);
        text(10, (int32_t)screen_h - 17, "C limpia  Supr borra  Cable: pin a pin", C_DIM);
    }
}

static int find_terminal(int32_t x, int32_t y) {
    int best = -1;
    int32_t best_distance = 12 * 12;
    for (uint32_t i = 0; i < part_count; ++i) {
        for (uint32_t pin = 0; pin < 2; ++pin) {
            const int32_t dx = x - terminal_x(i, pin);
            const int32_t dy = y - terminal_y(i);
            const int32_t d = dx * dx + dy * dy;
            if (d <= best_distance) {
                best_distance = d;
                best = (int)(i * 2 + pin);
            }
        }
    }
    return best;
}

static int find_part(int32_t x, int32_t y) {
    for (uint32_t i = part_count; i > 0; --i) {
        const struct part *p = &parts[i - 1];
        if (hit(x, y, p->x - 22, p->y - 25, 44, 54)) return (int)(i - 1);
    }
    return -1;
}

static int32_t snap(int32_t value, int32_t origin) {
    const int32_t delta = value - origin;
    return origin + ((delta + 10) / 20) * 20;
}

static void stop_simulation(void) {
    running = 0;
    solve_status = STATUS_IDLE;
    for (uint32_t i = 0; i < MAX_TERMINALS; ++i) terminal_voltage[i] = 0.0f;
    for (uint32_t i = 0; i < MAX_PARTS; ++i) part_current[i] = 0.0f;
}

static uint8_t root_parent[MAX_TERMINALS];

static uint8_t root(uint8_t item) {
    while (root_parent[item] != item) {
        root_parent[item] = root_parent[root_parent[item]];
        item = root_parent[item];
    }
    return item;
}

static void join(uint8_t a, uint8_t b) {
    const uint8_t ra = root(a), rb = root(b);
    if (ra != rb) root_parent[rb] = ra;
}

static void stamp_conductance(int32_t a, int32_t b, float conductance,
                              uint32_t nodes) {
    if (a >= 0) matrix[a][a] += conductance;
    if (b >= 0) matrix[b][b] += conductance;
    if (a >= 0 && b >= 0) {
        matrix[a][b] -= conductance;
        matrix[b][a] -= conductance;
    }
    (void)nodes;
}

static int solve_matrix(uint32_t unknowns) {
    for (uint32_t col = 0; col < unknowns; ++col) {
        uint32_t pivot = col;
        float largest = abs_f(matrix[col][col]);
        for (uint32_t row = col + 1; row < unknowns; ++row) {
            const float candidate = abs_f(matrix[row][col]);
            if (candidate > largest) { largest = candidate; pivot = row; }
        }
        if (largest < 0.000001f) return 0;
        if (pivot != col) {
            for (uint32_t k = col; k <= unknowns; ++k) {
                const float t = matrix[col][k];
                matrix[col][k] = matrix[pivot][k];
                matrix[pivot][k] = t;
            }
        }
        const float divisor = matrix[col][col];
        for (uint32_t k = col; k <= unknowns; ++k) matrix[col][k] /= divisor;
        for (uint32_t row = 0; row < unknowns; ++row) {
            if (row == col) continue;
            const float factor = matrix[row][col];
            if (abs_f(factor) < 0.0000001f) continue;
            for (uint32_t k = col; k <= unknowns; ++k)
                matrix[row][k] -= factor * matrix[col][k];
        }
    }
    return 1;
}

static void simulate(void) {
    uint8_t node_for_root[MAX_TERMINALS];
    int32_t node_for_terminal[MAX_TERMINALS];
    float node_voltage[MAX_TERMINALS];
    uint32_t source_part[MAX_PARTS];
    uint32_t source_count = 0, node_count = 0;
    int32_t ground_root = -1;

    stop_simulation();
    for (uint32_t i = 0; i < MAX_TERMINALS; ++i) root_parent[i] = (uint8_t)i;
    for (uint32_t i = 0; i < wire_count; ++i) join(wires[i].a, wires[i].b);
    for (uint32_t i = 0; i < part_count; ++i) {
        if (parts[i].type == PART_BATTERY) {
            if (ground_root < 0) ground_root = root((uint8_t)(i * 2));
            source_part[source_count++] = i;
        }
    }
    if (!source_count) { solve_status = STATUS_ERROR; return; }

    for (uint32_t i = 0; i < MAX_TERMINALS; ++i) node_for_root[i] = 0xff;
    for (uint32_t i = 0; i < part_count * 2; ++i) {
        const uint8_t r = root((uint8_t)i);
        if ((int32_t)r == ground_root) node_for_terminal[i] = -1;
        else {
            if (node_for_root[r] == 0xff) node_for_root[r] = (uint8_t)node_count++;
            node_for_terminal[i] = node_for_root[r];
        }
    }
    const uint32_t unknowns = node_count + source_count;
    if (!unknowns || unknowns > MAX_UNKNOWNS) { solve_status = STATUS_ERROR; return; }
    for (uint32_t row = 0; row < unknowns; ++row)
        for (uint32_t col = 0; col <= unknowns; ++col) matrix[row][col] = 0.0f;

    for (uint32_t i = 0; i < part_count; ++i) {
        const int32_t a = node_for_terminal[i * 2];
        const int32_t b = node_for_terminal[i * 2 + 1];
        float resistance = 0.0f;
        if (parts[i].type == PART_RESISTOR) resistance = 100.0f;
        else if (parts[i].type == PART_LAMP) resistance = 220.0f;
        else if (parts[i].type == PART_SWITCH && parts[i].closed) resistance = 0.02f;
        if (resistance > 0.0f) stamp_conductance(a, b, 1.0f / resistance, node_count);
    }
    for (uint32_t i = 0; i < source_count; ++i) {
        const uint32_t p = source_part[i];
        const uint32_t equation = node_count + i;
        const int32_t negative = node_for_terminal[p * 2];
        const int32_t positive = node_for_terminal[p * 2 + 1];
        if (positive >= 0) {
            matrix[positive][equation] += 1.0f;
            matrix[equation][positive] += 1.0f;
        }
        if (negative >= 0) {
            matrix[negative][equation] -= 1.0f;
            matrix[equation][negative] -= 1.0f;
        }
        matrix[equation][unknowns] = 9.0f;
    }
    if (!solve_matrix(unknowns)) { solve_status = STATUS_ERROR; return; }

    for (uint32_t i = 0; i < node_count; ++i) node_voltage[i] = matrix[i][unknowns];
    for (uint32_t i = 0; i < part_count * 2; ++i) {
        const int32_t node = node_for_terminal[i];
        terminal_voltage[i] = node < 0 ? 0.0f : node_voltage[node];
    }
    for (uint32_t i = 0; i < part_count; ++i) {
        const float delta = terminal_voltage[i * 2 + 1] - terminal_voltage[i * 2];
        float resistance = 0.0f;
        if (parts[i].type == PART_RESISTOR) resistance = 100.0f;
        else if (parts[i].type == PART_LAMP) resistance = 220.0f;
        else if (parts[i].type == PART_SWITCH && parts[i].closed) resistance = 0.02f;
        if (resistance > 0.0f) part_current[i] = abs_f(delta) / resistance;
        else if (parts[i].type == PART_BATTERY) {
            for (uint32_t s = 0; s < source_count; ++s)
                if (source_part[s] == i) part_current[i] = abs_f(matrix[node_count + s][unknowns]);
        }
    }
    solve_status = STATUS_SOLVED;
    running = 1;
}

static void clear_circuit(void) {
    part_count = wire_count = 0;
    selected = wire_start = -1;
    stop_simulation();
}

static void add_part(uint32_t type, int32_t x, int32_t y) {
    if (part_count >= MAX_PARTS) return;
    int32_t min_x, max_x, min_y, max_y;
    part_bounds(&min_x, &max_x, &min_y, &max_y);
    x = max_i32(min_x, min_i32(max_x, snap(x, canvas_x() + 10)));
    y = max_i32(min_y, min_i32(max_y, snap(y, canvas_y() + 10)));
    for (uint32_t i = 0; i < part_count; ++i) {
        if (abs_f((float)(parts[i].x - x)) < 42.0f &&
            abs_f((float)(parts[i].y - y)) < 34.0f) return;
    }
    parts[part_count].x = x;
    parts[part_count].y = y;
    parts[part_count].type = (uint8_t)type;
    parts[part_count].closed = 0;
    ++part_count;
    selected = (int32_t)part_count - 1;
    stop_simulation();
}

static void move_part(uint32_t index, int32_t x, int32_t y) {
    int32_t min_x, max_x, min_y, max_y;
    part_bounds(&min_x, &max_x, &min_y, &max_y);
    x = max_i32(min_x, min_i32(max_x, snap(x, canvas_x() + 10)));
    y = max_i32(min_y, min_i32(max_y, snap(y, canvas_y() + 10)));
    for (uint32_t i = 0; i < part_count; ++i) {
        if (i == index) continue;
        if (abs_f((float)(parts[i].x - x)) < 42.0f &&
            abs_f((float)(parts[i].y - y)) < 34.0f) return;
    }
    if (parts[index].x == x && parts[index].y == y) return;
    parts[index].x = x;
    parts[index].y = y;
    stop_simulation();
}

static void add_wire(int32_t a, int32_t b) {
    if (a < 0 || b < 0 || a == b || wire_count >= MAX_WIRES) return;
    for (uint32_t i = 0; i < wire_count; ++i)
        if ((wires[i].a == a && wires[i].b == b) ||
            (wires[i].a == b && wires[i].b == a)) return;
    wires[wire_count].a = (uint8_t)a;
    wires[wire_count].b = (uint8_t)b;
    ++wire_count;
    stop_simulation();
}

static void load_example(void) {
    clear_circuit();
    const int32_t cx = canvas_x(), cy = canvas_y();
    const int32_t cw = canvas_right() - cx;
    const int32_t ch = canvas_bottom() - cy;
    if (cw < 280 || ch < 130) {
        int32_t min_x, max_x, min_y, max_y;
        part_bounds(&min_x, &max_x, &min_y, &max_y);
        const int32_t mid_x = min_x + (max_x - min_x) / 2;
        const int32_t top_y = min_y + (max_y - min_y) / 3;
        add_part(PART_BATTERY, min_x, top_y);
        add_part(PART_SWITCH, mid_x, top_y);
        add_part(PART_LAMP, max_x, top_y);
        /* Small windows use a compact battery-switch-lamp loop. */
        add_wire(1, 2);
        add_wire(3, 4);
        add_wire(5, 0);
    } else {
        add_part(PART_BATTERY, cx + cw / 5, cy + ch / 3);
        add_part(PART_SWITCH, cx + cw * 2 / 5, cy + ch / 3);
        add_part(PART_LAMP, cx + cw * 3 / 5, cy + ch / 3);
        add_part(PART_RESISTOR, cx + cw * 3 / 5, cy + ch * 2 / 3);
        /* Battery positive -> switch -> lamp -> resistor -> battery negative. */
        add_wire(1, 2);
        add_wire(3, 4);
        add_wire(5, 7);
        add_wire(6, 0);
    }
    selected = -1;
    tool = TOOL_SELECT;
}

static void handle_click(int32_t x, int32_t y) {
    const int32_t cx = canvas_x(), cy = canvas_y();
    const int32_t cr = canvas_right(), cb = canvas_bottom();
    const int32_t controls_y = 50;
    for (uint32_t i = 0; i < 3; ++i) {
        int32_t control_x, control_w;
        control_rect(i, &control_x, &control_w);
        if (!hit(x, y, control_x, controls_y, control_w, 30)) continue;
        if (i == 0) load_example();
        else if (i == 1) clear_circuit();
        else if (running) stop_simulation();
        else simulate();
        return;
    }
    if (x < sidebar_width()) {
        for (uint32_t item = 0; item < 6; ++item) {
            int32_t bx, by, bw, bh;
            tool_rect(item, &bx, &by, &bw, &bh);
            if (hit(x, y, bx, by, bw, bh)) {
                tool = tool_kind(item);
                wire_start = -1;
                return;
            }
        }
        if (screen_h < 220) {
            /* Tiny window mode uses keyboard shortcuts to keep the palette
               usable even when there is room for buttons only. */
            const int32_t row = (y - 56) / 27;
            const int32_t col = x < sidebar_width() / 2 ? 0 : 1;
            const int32_t item = row * 2 + col;
            if (item >= 0 && item < 6) {
                tool = tool_kind((uint32_t)item);
                wire_start = -1;
            }
        }
        return;
    }
    if (!hit(x, y, cx, cy, cr - cx, cb - cy)) return;
    if (tool <= PART_SWITCH) {
        add_part(tool, x, y);
    } else if (tool == TOOL_WIRE) {
        const int terminal = find_terminal(x, y);
        if (terminal < 0) { wire_start = -1; return; }
        if (wire_start < 0) wire_start = terminal;
        else {
            add_wire(wire_start, terminal);
            wire_start = -1;
        }
    } else {
        const int part = find_part(x, y);
        selected = part;
        if (part >= 0 && parts[part].type == PART_SWITCH) {
            parts[part].closed = !parts[part].closed;
            if (running) simulate();
        }
    }
}

static void remove_selected(void) {
    if (selected < 0 || (uint32_t)selected >= part_count) return;
    const uint32_t removed_terminal = (uint32_t)selected * 2;
    uint32_t write = 0;
    for (uint32_t i = 0; i < wire_count; ++i) {
        if (wires[i].a == removed_terminal || wires[i].a == removed_terminal + 1 ||
            wires[i].b == removed_terminal || wires[i].b == removed_terminal + 1) continue;
        wires[write++] = wires[i];
    }
    wire_count = write;
    for (uint32_t i = 0; i < wire_count; ++i) {
        if (wires[i].a > removed_terminal + 1) wires[i].a -= 2;
        if (wires[i].b > removed_terminal + 1) wires[i].b -= 2;
    }
    for (uint32_t i = (uint32_t)selected; i + 1 < part_count; ++i) parts[i] = parts[i + 1];
    --part_count;
    selected = -1;
    stop_simulation();
}

static void handle_key(char key) {
    if (key >= '1' && key <= '4') {
        tool = (uint32_t)(key - '1'); wire_start = -1;
    } else if (key == 'w' || key == 'W') {
        tool = TOOL_WIRE; wire_start = -1;
    } else if (key == 's' || key == 'S') {
        tool = TOOL_SELECT; wire_start = -1;
    } else if (key == ' ' || key == '\n') {
        if (running) stop_simulation(); else simulate();
    } else if (key == 'e' || key == 'E') load_example();
    else if (key == 'c' || key == 'C') clear_circuit();
    else if (key == 0x1b) wire_start = -1;
    else if (key == 0x7f || key == '\b') remove_selected();
}

static int pump_input(void) {
    int activity = 0;
    welovec_pointer_event events[16];
    for (;;) {
        const int64_t count = welovec_pointer_poll(events, 16);
        if (count <= 0) break;
        activity = 1;
        for (int64_t i = 0; i < count; ++i) {
            const welovec_pointer_event *e = &events[i];
            int32_t x = 0, y = 0;
            if (e->flags & WELOVEC_POINTER_EVENT_ABSOLUTE) {
                x = (int32_t)((int64_t)e->absolute_x * (screen_w - 1) / 65535);
                y = (int32_t)((int64_t)e->absolute_y * (screen_h - 1) / 65535);
            }
            if (e->flags & WELOVEC_POINTER_EVENT_RELATIVE) {
                /* The desktop forwards absolute coordinates for windowed apps. */
                x += e->delta_x; y += e->delta_y;
            }
            const uint8_t down = (e->buttons & WELOVEC_POINTER_BUTTON_LEFT) != 0;
            if (down && !left_was_down) {
                handle_click(x, y);
                drag_part = -1;
                if (tool == TOOL_SELECT && hit(x, y, canvas_x(), canvas_y(),
                                                canvas_right() - canvas_x(),
                                                canvas_bottom() - canvas_y())) {
                    const int part = find_part(x, y);
                    if (part >= 0 && parts[part].type != PART_SWITCH) {
                        drag_part = part;
                        drag_dx = parts[part].x - x;
                        drag_dy = parts[part].y - y;
                    }
                }
            } else if (down && left_was_down && drag_part >= 0) {
                move_part((uint32_t)drag_part, x + drag_dx, y + drag_dy);
            } else if (!down && left_was_down) {
                drag_part = -1;
            }
            left_was_down = down;
        }
        if (count < 16) break;
    }
    char keys[64];
    for (;;) {
        const int64_t count = welovec_input_poll(keys, sizeof(keys));
        if (count <= 0) break;
        activity = 1;
        for (int64_t i = 0; i < count; ++i) handle_key(keys[i]);
    }
    return activity;
}

void userland_main(uint64_t argc, char **argv) {
    (void)argc; (void)argv;
    (void)refresh_dimensions();
    load_example();
    draw();
    last_size_check = welovec_getticks();
    for (;;) {
        int activity = pump_input();
        const uint64_t now = welovec_getticks();
        if (now - last_size_check >= 10) {
            last_size_check = now;
            if (refresh_dimensions()) activity = 1;
        }
        if (activity) draw();
        if (!activity) (void)welovec_sleep(1);
    }
}
