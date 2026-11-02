// Written by [@saphhic](https://github.com/saphhic)
// Date: November 2nd 2026
// Description: Displays and rotates projections of basic shapes in arbitrary dimensions.

#include "desktop/include/desktop.h"
#include "user/framebuffer.h"
#include "user/input.h"

#define GEO_DIMENSION_MAX UINT32_MAX
#define GEO_TAU 6.28318530718f
#define GEO_PI 3.14159265359f

enum { GEO_CUBE, GEO_SPHERE, GEO_SIMPLEX, GEO_SHAPE_COUNT };

struct geometry_state {
    uint32_t dimension;
    uint8_t shape;
    uint8_t editing;
    uint8_t fresh_input;
    uint8_t dragging;
    int32_t drag_x, drag_y;
    float yaw, pitch;
    char dimension_input[11];
};
APP_STATE_CHECK(struct geometry_state);

static const uint32_t k_edge_back = UINT32_C(0xff536476);
static const uint32_t k_edge_front = UINT32_C(0xff70d6dc);
static const uint32_t k_node_color = UINT32_C(0xfff1cf73);
static const char *const k_shape_names[GEO_SHAPE_COUNT] = {
    "Cubo", "Circulo", "Triangulo"
};

static float wrap_angle(float angle) {
    while (angle > GEO_PI) angle -= GEO_TAU;
    while (angle < -GEO_PI) angle += GEO_TAU;
    return angle;
}

/* Small local polynomial keeps this freestanding desktop app independent of libm. */
static float geo_sin(float angle) {
    float x = wrap_angle(angle);
    if (x > GEO_PI * 0.5f) x = GEO_PI - x;
    else if (x < -GEO_PI * 0.5f) x = -GEO_PI - x;
    const float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.00833333f - x2 * 0.00019841f)));
}

static float geo_cos(float angle) { return geo_sin(angle + GEO_PI * 0.5f); }

static void sync_dimension_text(struct geometry_state *s) {
    d_utoa(s->dimension, s->dimension_input);
}

static rect_t shape_button(rect_t c, int index) {
    const int32_t gap = 5;
    const int32_t width = (c.w - 20 - gap * (GEO_SHAPE_COUNT - 1)) / GEO_SHAPE_COUNT;
    return rect_make(c.x + 10 + index * (width + gap), c.y + 8, width, 32);
}

static rect_t canvas_rect(rect_t c) {
    return rect_make(c.x + 10, c.y + 50, c.w - 20, c.h - 100);
}

static rect_t reset_button(rect_t c) {
    const rect_t canvas = canvas_rect(c);
    return rect_make(canvas.x + canvas.w - 94, canvas.y + 8, 84, 26);
}

static rect_t dimension_minus(rect_t c) {
    return rect_make(c.x + c.w - 262, c.y + c.h - 35, 32, 26);
}

static rect_t dimension_field(rect_t c) {
    return rect_make(c.x + c.w - 222, c.y + c.h - 35, 178, 26);
}

static rect_t dimension_plus(rect_t c) {
    return rect_make(c.x + c.w - 42, c.y + c.h - 35, 32, 26);
}

static int open(struct window *w, const char *arg) {
    struct geometry_state *s = w->state;
    (void)arg;
    s->dimension = 3;
    s->shape = GEO_CUBE;
    sync_dimension_text(s);
    return 0;
}

static int out_code(float x, float y, rect_t clip) {
    int code = 0;
    if (x < (float)clip.x) code |= 1;
    else if (x > (float)(clip.x + clip.w - 1)) code |= 2;
    if (y < (float)clip.y) code |= 4;
    else if (y > (float)(clip.y + clip.h - 1)) code |= 8;
    return code;
}

static void draw_line(rect_t clip, float x0, float y0, float x1, float y1,
                      uint32_t color) {
    clip = rect_intersect(clip, gfx_clip_get());
    if (rect_empty(clip)) return;
    int c0 = out_code(x0, y0, clip);
    int c1 = out_code(x1, y1, clip);
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (!(c0 | c1)) {
            (void)welovec_fb_line((int32_t)(x0 + 0.5f), (int32_t)(y0 + 0.5f),
                                  (int32_t)(x1 + 0.5f), (int32_t)(y1 + 0.5f),
                                  color);
            return;
        }
        if (c0 & c1) return;
        const int outside = c0 ? c0 : c1;
        float x = 0.0f, y = 0.0f;
        if (outside & 8) {
            if (y1 == y0) return;
            y = (float)(clip.y + clip.h - 1);
            x = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        } else if (outside & 4) {
            if (y1 == y0) return;
            y = (float)clip.y;
            x = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        } else if (outside & 2) {
            if (x1 == x0) return;
            x = (float)(clip.x + clip.w - 1);
            y = y0 + (y1 - y0) * (x - x0) / (x1 - x0);
        } else {
            if (x1 == x0) return;
            x = (float)clip.x;
            y = y0 + (y1 - y0) * (x - x0) / (x1 - x0);
        }
        if (outside == c0) {
            x0 = x; y0 = y; c0 = out_code(x0, y0, clip);
        } else {
            x1 = x; y1 = y; c1 = out_code(x1, y1, clip);
        }
    }
}

static void rotate_point(const struct geometry_state *s, float x, float y, float z,
                         float *rx, float *ry, float *rz) {
    const float cy = geo_cos(s->yaw), sy = geo_sin(s->yaw);
    const float cp = geo_cos(s->pitch), sp = geo_sin(s->pitch);
    const float yaw_x = x * cy + z * sy;
    const float yaw_z = -x * sy + z * cy;
    *rx = yaw_x;
    *ry = y * cp - yaw_z * sp;
    *rz = y * sp + yaw_z * cp;
}

static void screen_point(const struct geometry_state *s, rect_t canvas,
                         float x, float y, float z, float radius,
                         float *sx, float *sy, float *depth) {
    float rx, ry, rz;
    rotate_point(s, x, y, z, &rx, &ry, &rz);
    float perspective = 1.0f / (1.0f - rz * 0.16f);
    if (perspective < 0.72f) perspective = 0.72f;
    if (perspective > 1.42f) perspective = 1.42f;
    *sx = (float)(canvas.x + canvas.w / 2) + rx * radius * perspective;
    *sy = (float)(canvas.y + canvas.h / 2 + 10) - ry * radius * perspective;
    *depth = rz;
}

static void draw_vertex(const struct geometry_state *s, rect_t canvas,
                        float x, float y, float z, float radius) {
    float sx, sy, depth;
    screen_point(s, canvas, x, y, z, radius, &sx, &sy, &depth);
    if (out_code(sx, sy, canvas)) return;
    gfx_fill((int32_t)(sx + 0.5f) - 2, (int32_t)(sy + 0.5f) - 2,
             5, 5, k_node_color);
}

static void draw_projected_edge(const struct geometry_state *s, rect_t canvas,
                                float x0, float y0, float z0,
                                float x1, float y1, float z1, float radius) {
    float sx0, sy0, d0, sx1, sy1, d1;
    screen_point(s, canvas, x0, y0, z0, radius, &sx0, &sy0, &d0);
    screen_point(s, canvas, x1, y1, z1, radius, &sx1, &sy1, &d1);
    draw_line(canvas, sx0, sy0, sx1, sy1,
              (d0 + d1) < 0.0f ? k_edge_back : k_edge_front);
}

static void axis_weights(uint32_t axis, float *x, float *y, float *z) {
    static const float extra[4][3] = {
        { 0.58f,  0.58f,  0.58f},
        {-0.58f,  0.58f, -0.58f},
        { 0.58f, -0.58f, -0.58f},
        {-0.58f, -0.58f,  0.58f}
    };
    if (axis == 0) { *x = 1.0f; *y = 0.0f; *z = 0.0f; }
    else if (axis == 1) { *x = 0.0f; *y = 1.0f; *z = 0.0f; }
    else if (axis == 2) { *x = 0.0f; *y = 0.0f; *z = 1.0f; }
    else {
        const float *weights = extra[(axis - 3u) % 4u];
        *x = weights[0]; *y = weights[1]; *z = weights[2];
    }
}

static void draw_cube(const struct geometry_state *s, rect_t canvas, float radius) {
    uint64_t multiplicity[7] = {0};
    uint32_t group_count = s->dimension < 3u ? s->dimension : 3u;
    for (uint32_t axis = 0; axis < group_count; ++axis)
        multiplicity[axis] = 1;
    if (s->dimension > 3u) {
        const uint64_t extra = (uint64_t)s->dimension - 3u;
        const uint64_t each = extra / 4u;
        const uint64_t remainder = extra % 4u;
        const uint32_t extra_groups = extra < 4u ? (uint32_t)extra : 4u;
        group_count = 3u + extra_groups;
        for (uint32_t group = 0; group < extra_groups; ++group)
            multiplicity[3u + group] = each + (group < remainder ? 1u : 0u);
    }
    /* Repeated projection directions combine into one segment of the zonotope. */
    const uint32_t count = 1u << group_count;
    const float scale = s->dimension > 3u ? 3.0f / (float)s->dimension : 1.0f;
    float vertices[128][3];
    for (uint32_t mask = 0; mask < count; ++mask) {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        for (uint32_t axis = 0; axis < group_count; ++axis) {
            float wx, wy, wz;
            axis_weights(axis, &wx, &wy, &wz);
            const float sign = (mask & (1u << axis)) ? 1.0f : -1.0f;
            const float amount = sign * (float)multiplicity[axis];
            x += amount * wx; y += amount * wy; z += amount * wz;
        }
        vertices[mask][0] = x * scale;
        vertices[mask][1] = y * scale;
        vertices[mask][2] = z * scale;
    }
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t mask = 0; mask < count; ++mask) {
            for (uint32_t axis = 0; axis < group_count; ++axis) {
                if (mask & (1u << axis)) continue;
                const uint32_t other = mask | (1u << axis);
                float ax, ay, az, bx, by, bz;
                screen_point(s, canvas, vertices[mask][0], vertices[mask][1],
                             vertices[mask][2], radius, &ax, &ay, &az);
                screen_point(s, canvas, vertices[other][0], vertices[other][1],
                             vertices[other][2], radius, &bx, &by, &bz);
                const int back = (az + bz) < 0.0f;
                if (back == (pass == 0))
                    draw_line(canvas, ax, ay, bx, by,
                              back ? k_edge_back : k_edge_front);
            }
        }
    }
    for (uint32_t i = 0; i < count; ++i)
        draw_vertex(s, canvas, vertices[i][0], vertices[i][1], vertices[i][2], radius);
}

static void draw_sphere_ring(const struct geometry_state *s, rect_t canvas,
                             float radius, float ring_scale, uint32_t color) {
    const uint32_t segments = 48;
    for (uint32_t i = 0; i < segments; ++i) {
        const float a0 = GEO_TAU * (float)i / (float)segments;
        const float a1 = GEO_TAU * (float)(i + 1u) / (float)segments;
        float x0 = geo_cos(a0) * ring_scale;
        float y0 = geo_sin(a0) * ring_scale;
        float x1 = geo_cos(a1) * ring_scale;
        float y1 = geo_sin(a1) * ring_scale;
        float sx0, sy0, d0, sx1, sy1, d1;
        screen_point(s, canvas, x0, y0, 0.0f, radius, &sx0, &sy0, &d0);
        screen_point(s, canvas, x1, y1, 0.0f, radius, &sx1, &sy1, &d1);
        draw_line(canvas, sx0, sy0, sx1, sy1, color);
    }
}

static void draw_sphere(const struct geometry_state *s, rect_t canvas, float radius) {
    if (s->dimension == 1) {
        draw_vertex(s, canvas, -0.76f, 0.0f, 0.0f, radius);
        draw_vertex(s, canvas, 0.76f, 0.0f, 0.0f, radius);
        return;
    }
    if (s->dimension == 2) {
        draw_sphere_ring(s, canvas, radius, 1.0f, k_edge_front);
        return;
    }

    enum { LATITUDES = 8, LONGITUDES = 16 };
    for (int lat = 0; lat <= LATITUDES; ++lat) {
        const float phi = -GEO_PI * 0.5f + GEO_PI * (float)lat / (float)LATITUDES;
        const float ring = geo_cos(phi), height = geo_sin(phi);
        for (int lon = 0; lon < LONGITUDES; ++lon) {
            const float a0 = GEO_TAU * (float)lon / (float)LONGITUDES;
            const float a1 = GEO_TAU * (float)(lon + 1) / (float)LONGITUDES;
            draw_projected_edge(s, canvas,
                ring * geo_cos(a0), height, ring * geo_sin(a0),
                ring * geo_cos(a1), height, ring * geo_sin(a1), radius);
            if (lat < LATITUDES) {
                const float next_phi = -GEO_PI * 0.5f + GEO_PI * (float)(lat + 1) /
                                       (float)LATITUDES;
                const float next_ring = geo_cos(next_phi);
                const float next_height = geo_sin(next_phi);
                draw_projected_edge(s, canvas,
                    ring * geo_cos(a0), height, ring * geo_sin(a0),
                    next_ring * geo_cos(a0), next_height,
                    next_ring * geo_sin(a0), radius);
            }
        }
    }
    if (s->dimension > 3u) {
        /* Projecting an n-sphere (n > 3) onto 3D fills the sphere's interior. */
        int32_t slices = (canvas.w < canvas.h ? canvas.w : canvas.h) / 20;
        if (slices > 12) slices = 12;
        if (slices < 2) slices = 2;
        for (int32_t i = 1; i < slices; ++i) {
            const float ring_scale = (float)i / (float)slices;
            draw_sphere_ring(s, canvas, radius, ring_scale, k_edge_back);
        }
    }
}

static void simplex_vertex(uint32_t dimensions, uint32_t vertex,
                           float *x, float *y, float *z) {
    if (dimensions == 1) {
        *x = vertex ? 0.76f : -0.76f; *y = 0.0f; *z = 0.0f;
        return;
    }
    if (dimensions == 2) {
        const float angle = -GEO_PI * 0.5f + GEO_TAU * (float)vertex / 3.0f;
        *x = geo_cos(angle) * 0.78f; *y = geo_sin(angle) * 0.78f; *z = 0.0f;
        return;
    }
    if (dimensions == 3) {
        static const float tetra[4][3] = {
            { 0.58f,  0.58f,  0.58f},
            { 0.58f, -0.58f, -0.58f},
            {-0.58f,  0.58f, -0.58f},
            {-0.58f, -0.58f,  0.58f}
        };
        *x = tetra[vertex][0]; *y = tetra[vertex][1]; *z = tetra[vertex][2];
        return;
    }
    const uint64_t total = (uint64_t)dimensions + 1u;
    const uint64_t extra = total - 3u;
    const uint64_t each = extra / 4u;
    const uint64_t remainder = extra % 4u;
    float avg_x = 0.0f, avg_y = 0.0f, avg_z = 0.0f;
    for (uint32_t axis = 0; axis < 3; ++axis) {
        float wx, wy, wz;
        axis_weights(axis, &wx, &wy, &wz);
        avg_x += wx; avg_y += wy; avg_z += wz;
    }
    for (uint32_t group = 0; group < 4; ++group) {
        const uint64_t count = each + (group < remainder ? 1u : 0u);
        float wx, wy, wz;
        axis_weights(3u + group, &wx, &wy, &wz);
        avg_x += (float)count * wx;
        avg_y += (float)count * wy;
        avg_z += (float)count * wz;
    }
    avg_x /= (float)total; avg_y /= (float)total; avg_z /= (float)total;
    float wx, wy, wz;
    axis_weights(vertex < 3u ? vertex : 3u + ((vertex - 3u) % 4u),
                 &wx, &wy, &wz);
    *x = wx - avg_x; *y = wy - avg_y; *z = wz - avg_z;
}

static void draw_simplex(const struct geometry_state *s, rect_t canvas, float radius) {
    const uint32_t dimensions = s->dimension;
    uint32_t count;
    if (dimensions <= 3u) count = dimensions + 1u;
    else {
        const uint64_t extra = (uint64_t)dimensions - 2u;
        count = 3u + (extra < 4u ? (uint32_t)extra : 4u);
    }
    float vertices[7][3];
    for (uint32_t i = 0; i < count; ++i)
        simplex_vertex(dimensions, i, &vertices[i][0], &vertices[i][1], &vertices[i][2]);
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t j = i + 1; j < count; ++j)
            draw_projected_edge(s, canvas,
                vertices[i][0], vertices[i][1], vertices[i][2],
                vertices[j][0], vertices[j][1], vertices[j][2], radius);
    }
    for (uint32_t i = 0; i < count; ++i)
        draw_vertex(s, canvas, vertices[i][0], vertices[i][1], vertices[i][2], radius);
}

static void draw_model(const struct geometry_state *s, rect_t canvas) {
    int32_t diameter = canvas.w < canvas.h ? canvas.w : canvas.h;
    float radius = (float)diameter * 0.32f;
    if (radius < 22.0f) radius = 22.0f;
    if (s->shape == GEO_CUBE) draw_cube(s, canvas, radius);
    else if (s->shape == GEO_SPHERE) draw_sphere(s, canvas, radius);
    else draw_simplex(s, canvas, radius);
}

static void draw(struct window *w, rect_t c) {
    struct geometry_state *s = w->state;
    const rect_t canvas = canvas_rect(c);
    gfx_fill(c.x + 10, c.y + c.h - 42, c.w - 20, 42, C_PANEL);
    for (int i = 0; i < GEO_SHAPE_COUNT; ++i) {
        const rect_t button = shape_button(c, i);
        const int selected = s->shape == i;
        gfx_fill(button.x, button.y, button.w, button.h,
                 selected ? C_ACCENT : C_BTN);
        gfx_frame(button.x, button.y, button.w, button.h,
                 selected ? C_ACCENT_DK : C_SHADOW);
        const int32_t text_w = (int32_t)d_strlen(k_shape_names[i]) * FONT_W;
        gfx_text(button.x + (button.w - text_w) / 2,
                 button.y + (button.h - FONT_H) / 2,
                 k_shape_names[i], selected ? C_WHITE : C_TEXT);
    }

    gfx_fill(canvas.x, canvas.y, canvas.w, canvas.h, UINT32_C(0xff121a25));
    gfx_frame(canvas.x, canvas.y, canvas.w, canvas.h, C_SHADOW);
    char dimension[16];
    char caption[96];
    d_utoa(s->dimension, dimension);
    const char *object_name = "Simplex ";
    if (s->shape == GEO_CUBE) object_name = "Cubo ";
    else if (s->shape == GEO_SPHERE)
        object_name = s->dimension <= 2 ? "Circulo " : "Esfera ";
    else if (s->dimension == 1) object_name = "Segmento ";
    else if (s->dimension == 2) object_name = "Triangulo ";
    d_strcpy(caption, sizeof(caption), object_name);
    d_strcat(caption, sizeof(caption), "  ");
    d_strcat(caption, sizeof(caption), dimension);
    d_strcat(caption, sizeof(caption), "D");
    gfx_text(canvas.x + 12, canvas.y + 10, caption, C_WHITE);
    gfx_text(canvas.x + 12, canvas.y + 30,
             "Arrastra para girar (vista 3D)", UINT32_C(0xffaebdca));

    const rect_t reset = reset_button(c);
    gfx_fill(reset.x, reset.y, reset.w, reset.h, C_PANEL_HI);
    gfx_frame(reset.x, reset.y, reset.w, reset.h, C_SHADOW);
    gfx_text(reset.x + 10, reset.y + 5, "Reiniciar", C_WHITE);
    draw_model(s, canvas);

    const rect_t minus = dimension_minus(c);
    const rect_t field = dimension_field(c);
    const rect_t plus = dimension_plus(c);
    gfx_text(c.x + c.w - 328, c.y + c.h - 31, "Dim.", C_WHITE);
    ui_button(minus, "-", s->dimension > 1);
    gfx_fill(field.x, field.y, field.w, field.h,
             s->editing ? C_WHITE : C_EDIT_BG);
    gfx_frame(field.x, field.y, field.w, field.h,
              s->editing ? C_ACCENT : C_SHADOW);
    gfx_text(field.x + 7, field.y + 5, s->dimension_input,
             s->editing ? C_TEXT : C_TEXT_DIM);
    ui_button(plus, "+", s->dimension < GEO_DIMENSION_MAX);
}

static void change_dimension(struct geometry_state *s, int delta) {
    if (delta < 0 && s->dimension > 1) --s->dimension;
    if (delta > 0 && s->dimension < GEO_DIMENSION_MAX) ++s->dimension;
    s->editing = s->fresh_input = 0;
    sync_dimension_text(s);
}

static void apply_dimension(struct geometry_state *s) {
    uint64_t value = 0;
    for (uint32_t i = 0; s->dimension_input[i]; ++i)
        value = value * 10u + (uint32_t)(s->dimension_input[i] - '0');
    if (value >= 1 && value <= GEO_DIMENSION_MAX) s->dimension = (uint32_t)value;
    else sync_dimension_text(s);
    s->editing = s->fresh_input = 0;
}

static void mouse(struct window *w, const struct mouse_event *e) {
    struct geometry_state *s = w->state;
    const rect_t full = wm_client(w);
    const rect_t local = rect_make(0, 0, full.w, full.h);
    if (e->type == MOUSE_UP) {
        s->dragging = 0;
        return;
    }
    if (e->type == MOUSE_MOVE && s->dragging) {
        if (!(e->buttons & WELOVEC_POINTER_BUTTON_LEFT)) {
            s->dragging = 0;
            return;
        }
        const int32_t dx = e->x - s->drag_x;
        const int32_t dy = e->y - s->drag_y;
        s->yaw = wrap_angle(s->yaw + (float)dx * 0.012f);
        s->pitch = wrap_angle(s->pitch + (float)dy * 0.012f);
        s->drag_x = e->x;
        s->drag_y = e->y;
        wm_invalidate(w);
        return;
    }
    if (e->type != MOUSE_DOWN) return;

    for (int i = 0; i < GEO_SHAPE_COUNT; ++i) {
        if (rect_hit(shape_button(local, i), e->x, e->y)) {
            s->shape = (uint8_t)i;
            s->editing = s->fresh_input = 0;
            wm_invalidate(w);
            return;
        }
    }
    if (rect_hit(dimension_minus(local), e->x, e->y)) {
        change_dimension(s, -1);
        wm_invalidate(w);
        return;
    }
    if (rect_hit(dimension_plus(local), e->x, e->y)) {
        change_dimension(s, 1);
        wm_invalidate(w);
        return;
    }
    if (rect_hit(dimension_field(local), e->x, e->y)) {
        s->editing = 1;
        s->fresh_input = 1;
        sync_dimension_text(s);
        wm_invalidate(w);
        return;
    }
    if (rect_hit(reset_button(local), e->x, e->y)) {
        s->yaw = s->pitch = 0.0f;
        s->editing = s->fresh_input = 0;
        wm_invalidate(w);
        return;
    }
    if (rect_hit(canvas_rect(local), e->x, e->y)) {
        s->editing = s->fresh_input = 0;
        s->dragging = 1;
        s->drag_x = e->x;
        s->drag_y = e->y;
    }
}

static int key(struct window *w, int value) {
    struct geometry_state *s = w->state;
    if (s->editing) {
        uint32_t length = d_strlen(s->dimension_input);
        if (value >= '0' && value <= '9') {
            if (s->fresh_input) {
                s->dimension_input[0] = '\0';
                length = 0;
                s->fresh_input = 0;
            }
            if (length < 10) {
                s->dimension_input[length++] = (char)value;
                s->dimension_input[length] = '\0';
            }
            wm_invalidate(w);
            return 1;
        }
        if (value == '\b' || value == 0x7f) {
            if (s->fresh_input) {
                s->dimension_input[0] = '\0';
                s->fresh_input = 0;
            } else if (length) {
                s->dimension_input[length - 1] = '\0';
            }
            wm_invalidate(w);
            return 1;
        }
        if (value == '\n') {
            apply_dimension(s);
            wm_invalidate(w);
            return 1;
        }
        if (value == 0x1b) {
            sync_dimension_text(s);
            s->editing = s->fresh_input = 0;
            wm_invalidate(w);
            return 1;
        }
        return 1;
    }
    if (value == KEY_LEFT || value == KEY_RIGHT || value == KEY_UP || value == KEY_DOWN) {
        if (value == KEY_LEFT) s->yaw -= 0.12f;
        if (value == KEY_RIGHT) s->yaw += 0.12f;
        if (value == KEY_UP) s->pitch -= 0.12f;
        if (value == KEY_DOWN) s->pitch += 0.12f;
        s->yaw = wrap_angle(s->yaw);
        s->pitch = wrap_angle(s->pitch);
        wm_invalidate(w);
        return 1;
    }
    if (value == '+' || value == '=') {
        change_dimension(s, 1);
        wm_invalidate(w);
        return 1;
    }
    if (value == '-') {
        change_dimension(s, -1);
        wm_invalidate(w);
        return 1;
    }
    return 0;
}

const struct app_ops app_geometry = {
    "Objetos", 720, 520, 520, 360, 1,
    open, 0, 0, draw, mouse, key, 0, 0, 0, 0,
};
