// Written by [@saphhic](https://github.com/saphhic)
// Date: November 3rd 2026
// Description: Implements a compact, code-focused desktop editor.

#include "desktop/include/desktop.h"
#include "user/filesystem.h"
#include "user/syscall.h"

#define CODE_MAX_FILE (4u * 1024u * 1024u)
#define CODE_SLACK 16384u
#define CODE_PATH_CAP 192u
#define CODE_TOOLBAR_H 32
#define CODE_TAB_H 24
#define CODE_STATUS_H 20
#define CODE_SIDEBAR_W 176
#define CODE_TREE_ROW_H 20
#define CODE_GUTTER_W 52

#define CODE_BG UINT32_C(0xff1e1e1e)
#define CODE_SIDEBAR UINT32_C(0xff252526)
#define CODE_TAB_BG UINT32_C(0xff2d2d30)
#define CODE_EDITOR UINT32_C(0xff1e1e1e)
#define CODE_LINE_HI UINT32_C(0xff252526)
#define CODE_GUTTER UINT32_C(0xff1e1e1e)
#define CODE_LINE_NUMBER UINT32_C(0xff858585)
#define CODE_TEXT UINT32_C(0xffd4d4d4)
#define CODE_KEYWORD UINT32_C(0xff569cd6)
#define CODE_STRING UINT32_C(0xffce9178)
#define CODE_COMMENT UINT32_C(0xff6a9955)
#define CODE_NUMBER UINT32_C(0xffb5cea8)
#define CODE_PREPROCESSOR UINT32_C(0xffc586c0)
#define CODE_STATUS UINT32_C(0xff007acc)
#define CODE_CARET UINT32_C(0xffaeafad)
#define CODE_TREE_SELECTED UINT32_C(0xff37373d)

struct code_state {
    char path[CODE_PATH_CAP];
    char folder[CODE_PATH_CAP];
    char pending_path[CODE_PATH_CAP];
    char *buffer;
    uint32_t capacity;
    uint32_t length;
    uint32_t cursor;
    uint32_t top_line;
    uint32_t left_column;
    struct dir_item entries[WELOVEC_USER_FS_MAX_ENTRIES];
    uint32_t entry_count;
    uint32_t tree_top;
    uint32_t tree_selection;
    uint8_t dirty;
    uint8_t binary;
    char status[64];
};
APP_STATE_CHECK(struct code_state);

enum { CODE_OPEN, CODE_SAVE, CODE_SAVE_AS, CODE_NEW, CODE_BUTTON_COUNT };
static const char *const k_code_buttons[CODE_BUTTON_COUNT] = {
    "Open", "Save", "Save As", "New"
};

static uint32_t text_length(const char *text) {
    uint32_t length = 0;
    while (text && text[length]) ++length;
    return length;
}

static int identifier_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int identifier_char(char c) {
    return identifier_start(c) || (c >= '0' && c <= '9');
}

static int keyword_equal(const char *text, uint32_t length,
                         const char *keyword) {
    uint32_t i = 0;
    while (i < length && keyword[i] && text[i] == keyword[i]) ++i;
    return i == length && !keyword[i];
}

static int is_keyword(const char *text, uint32_t length) {
    static const char *const words[] = {
        "alignas", "auto", "bool", "break", "case", "catch", "char",
        "class", "const", "constexpr", "continue", "default", "delete",
        "do", "double", "else", "enum", "extern", "false", "float",
        "for", "friend", "if", "include", "inline", "int", "long",
        "namespace", "new", "nullptr", "private", "protected", "public",
        "register", "return", "short", "signed", "sizeof", "static",
        "struct", "switch", "template", "this", "throw", "true", "try",
        "typedef", "typename", "union", "unsigned", "using", "virtual",
        "void", "volatile", "while", "def", "elif", "from", "import",
        "lambda", "pass", "self", "class", "echo", "fi", "function",
        "then", "local"
    };
    for (uint32_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        if (keyword_equal(text, length, words[i])) return 1;
    return 0;
}

static uint32_t line_start(const struct code_state *state, uint32_t offset) {
    while (offset && state->buffer[offset - 1] != '\n') --offset;
    return offset;
}

static uint32_t line_end(const struct code_state *state, uint32_t offset) {
    while (offset < state->length && state->buffer[offset] != '\n') ++offset;
    if (offset > 0 && state->buffer[offset - 1] == '\r') --offset;
    return offset;
}

static uint32_t next_line(const struct code_state *state, uint32_t offset) {
    while (offset < state->length && state->buffer[offset] != '\n') ++offset;
    return offset < state->length ? offset + 1 : state->length;
}

static uint32_t line_of(const struct code_state *state, uint32_t offset) {
    uint32_t lines = 0;
    for (uint32_t i = 0; i < offset && i < state->length; ++i)
        if (state->buffer[i] == '\n') ++lines;
    return lines;
}

static uint32_t offset_of_line(const struct code_state *state, uint32_t line) {
    uint32_t offset = 0;
    while (line-- && offset < state->length)
        offset = next_line(state, offset);
    return offset;
}

static int grow(struct code_state *state, uint32_t needed) {
    if (needed <= state->capacity) return 1;
    if (needed > CODE_MAX_FILE + CODE_SLACK) return 0;
    uint32_t capacity = state->capacity ? state->capacity : 4096;
    while (capacity < needed) {
        if (capacity > (CODE_MAX_FILE + CODE_SLACK) / 2) {
            capacity = CODE_MAX_FILE + CODE_SLACK;
            break;
        }
        capacity *= 2;
    }
    char *replacement = (char *)welovec_mmap(capacity);
    if (!replacement) return 0;
    if (state->buffer) {
        if (state->length) memcpy(replacement, state->buffer, state->length);
        (void)welovec_munmap(state->buffer, state->capacity);
    }
    state->buffer = replacement;
    state->capacity = capacity;
    return 1;
}

static void set_title(struct window *window) {
    struct code_state *state = window->state;
    char title[32];
    uint32_t start = text_length(state->path);
    while (start && state->path[start - 1] != '/') --start;
    d_strcpy(title, sizeof(title), state->path[0] ? state->path + start : "Welcome");
    if (state->dirty) d_strcat(title, sizeof(title), "*");
    wm_set_title(window, title);
}

static void refresh_tree(struct code_state *state) {
    uint32_t count = 0;
    const int result = fsu_list(state->folder, state->entries,
                                WELOVEC_USER_FS_MAX_ENTRIES, &count);
    state->entry_count = result < 0 ? 0 : count;
    if (state->tree_selection > state->entry_count)
        state->tree_selection = state->entry_count;
    if (state->tree_top > state->entry_count)
        state->tree_top = state->entry_count;
    if (result < 0) d_strcpy(state->status, sizeof(state->status), "Folder unavailable");
}

static void set_folder_for_path(struct code_state *state, const char *path) {
    d_strcpy(state->folder, sizeof(state->folder), path);
    d_parent_path(state->folder);
    refresh_tree(state);
}

static int open_file(struct window *window, const char *path) {
    struct code_state *state = window->state;
    struct welovec_user_file_info info;
    if (!path || !path[0] || text_length(path) >= sizeof(state->path)) {
        d_strcpy(state->status, sizeof(state->status), "Invalid file path");
        return 0;
    }
    if (welovec_fs_stat(path, &info) < 0 || info.type != WELOVEC_USER_FS_FILE) {
        d_strcpy(state->status, sizeof(state->status), "Cannot open file");
        return 0;
    }
    if (info.size > CODE_MAX_FILE) {
        d_strcpy(state->status, sizeof(state->status), "File exceeds 4 MB limit");
        return 0;
    }
    if (!grow(state, (uint32_t)info.size + CODE_SLACK)) {
        d_strcpy(state->status, sizeof(state->status), "Not enough memory");
        return 0;
    }
    const int64_t read = welovec_fs_read(path, state->buffer, state->capacity);
    if (read < 0) {
        d_strcpy(state->status, sizeof(state->status), "Read failed");
        return 0;
    }
    state->length = (uint32_t)read;
    state->cursor = 0;
    state->top_line = 0;
    state->left_column = 0;
    state->dirty = 0;
    state->binary = 0;
    d_strcpy(state->path, sizeof(state->path), path);
    for (uint32_t i = 0; i < state->length && i < 4096; ++i)
        if (!state->buffer[i]) state->binary = 1;
    if (state->binary)
        d_strcpy(state->status, sizeof(state->status), "Binary file: read-only");
    else
        state->status[0] = '\0';
    set_folder_for_path(state, path);
    set_title(window);
    return 1;
}

static void new_document(struct window *window) {
    struct code_state *state = window->state;
    state->length = 0;
    state->cursor = 0;
    state->top_line = 0;
    state->left_column = 0;
    state->path[0] = '\0';
    state->dirty = 0;
    state->binary = 0;
    state->status[0] = '\0';
    set_title(window);
    wm_invalidate(window);
    wm_taskbar_repaint();
}

static void confirm_open(void *context, const char *text) {
    (void)text;
    struct window *window = context;
    struct code_state *state = window->state;
    if (state->pending_path[0]) (void)open_file(window, state->pending_path);
    state->pending_path[0] = '\0';
    wm_invalidate(window);
    wm_taskbar_repaint();
}

static void request_open(struct window *window, const char *path) {
    struct code_state *state = window->state;
    if (!path || !path[0]) return;
    if (!state->dirty) {
        (void)open_file(window, path);
        wm_invalidate(window);
        wm_taskbar_repaint();
        return;
    }
    d_strcpy(state->pending_path, sizeof(state->pending_path), path);
    dlg_confirm("Discard unsaved changes and open another file?",
                confirm_open, window);
}

static void open_prompt_done(void *context, const char *text) {
    struct window *window = context;
    request_open(window, text);
}

static void ask_open(struct window *window) {
    struct code_state *state = window->state;
    dlg_prompt("Open file (absolute path):", state->path[0] ? state->path : state->folder,
               open_prompt_done, window);
}

static void save_to(struct window *window, const char *path) {
    struct code_state *state = window->state;
    if (state->binary) {
        d_strcpy(state->status, sizeof(state->status), "Binary files are read-only");
        wm_invalidate(window);
        return;
    }
    const int64_t written = welovec_fs_write(path, state->buffer ? state->buffer : "",
                                              state->length);
    if (written < 0) {
        d_strcpy(state->status, sizeof(state->status), "Save failed (permissions?)");
    } else {
        d_strcpy(state->path, sizeof(state->path), path);
        state->dirty = 0;
        state->status[0] = '\0';
        set_folder_for_path(state, path);
        d_strcpy(state->status, sizeof(state->status), "Saved");
    }
    set_title(window);
    wm_invalidate(window);
    wm_taskbar_repaint();
}

static void save_as_done(void *context, const char *text) {
    struct window *window = context;
    if (text[0]) save_to(window, text);
}

static void ask_save_as(struct window *window) {
    struct code_state *state = window->state;
    char suggestion[CODE_PATH_CAP];
    if (state->path[0]) {
        d_strcpy(suggestion, sizeof(suggestion), state->path);
    } else {
        d_strcpy(suggestion, sizeof(suggestion), "/home/");
        d_strcat(suggestion, sizeof(suggestion), g_desk.user);
        d_strcat(suggestion, sizeof(suggestion), "/untitled.c");
    }
    dlg_prompt("Save as (absolute path):", suggestion, save_as_done, window);
}

static void save_document(struct window *window) {
    struct code_state *state = window->state;
    if (!state->path[0]) ask_save_as(window);
    else save_to(window, state->path);
}

static void new_confirmed(void *context, const char *text) {
    (void)text;
    new_document(context);
}

static void button_action(struct window *window, int button) {
    struct code_state *state = window->state;
    if (button == CODE_OPEN) {
        ask_open(window);
    } else if (button == CODE_SAVE) {
        save_document(window);
    } else if (button == CODE_SAVE_AS) {
        ask_save_as(window);
    } else if (state->dirty) {
        dlg_confirm("Discard unsaved changes and create a new file?",
                    new_confirmed, window);
    } else {
        new_document(window);
    }
}

static int open_app(struct window *window, const char *argument) {
    struct code_state *state = window->state;
    d_strcpy(state->folder, sizeof(state->folder), "/home/");
    d_strcat(state->folder, sizeof(state->folder), g_desk.user);
    if (welovec_fs_exists(state->folder) != 1)
        d_strcpy(state->folder, sizeof(state->folder), "/");
    refresh_tree(state);
    state->status[0] = '\0';
    if (argument && argument[0]) (void)open_file(window, argument);
    set_title(window);
    return 0;
}

static void close_app(struct window *window) {
    struct code_state *state = window->state;
    if (state->buffer) (void)welovec_munmap(state->buffer, state->capacity);
    state->buffer = 0;
}

static void discard_close(void *context, const char *text) {
    (void)text;
    struct window *window = context;
    ((struct code_state *)window->state)->dirty = 0;
    wm_close(window);
}

static int query_close(struct window *window) {
    struct code_state *state = window->state;
    if (!state->dirty) return 1;
    dlg_confirm("Discard unsaved changes?", discard_close, window);
    return 0;
}

static rect_t editor_pane(rect_t client) {
    return rect_make(client.x + CODE_SIDEBAR_W, client.y + CODE_TOOLBAR_H,
                     client.w - CODE_SIDEBAR_W,
                     client.h - CODE_TOOLBAR_H - CODE_STATUS_H);
}

static rect_t text_area(rect_t pane) {
    return rect_make(pane.x, pane.y + CODE_TAB_H, pane.w,
                     pane.h - CODE_TAB_H);
}

static rect_t sidebar_area(rect_t client) {
    return rect_make(client.x, client.y + CODE_TOOLBAR_H, CODE_SIDEBAR_W,
                     client.h - CODE_TOOLBAR_H - CODE_STATUS_H);
}

static rect_t button_rect(rect_t client, int button) {
    int32_t x = client.x + 5;
    for (int i = 0; i < button; ++i) x += ui_button_w(k_code_buttons[i]) + 4;
    return rect_make(x, client.y + 4, ui_button_w(k_code_buttons[button]), 24);
}

static uint32_t token_color(const struct code_state *state, uint32_t end,
                            uint32_t position,
                            uint32_t first_nonspace, uint32_t *token_end) {
    const char *text = state->buffer;
    const char c = text[position];
    if (position == first_nonspace && c == '#') {
        *token_end = end;
        return CODE_PREPROCESSOR;
    }
    if (c == '/' && position + 1 < end && text[position + 1] == '/') {
        *token_end = end;
        return CODE_COMMENT;
    }
    if (c == '/' && position + 1 < end && text[position + 1] == '*') {
        uint32_t finish = position + 2;
        while (finish + 1 < end &&
               !(text[finish] == '*' && text[finish + 1] == '/')) ++finish;
        *token_end = finish + 1 < end ? finish + 2 : end;
        return CODE_COMMENT;
    }
    if (c == '"' || c == '\'') {
        const char quote = c;
        uint32_t finish = position + 1;
        while (finish < end) {
            if (text[finish] == '\\' && finish + 1 < end) {
                finish += 2;
                continue;
            }
            if (text[finish++] == quote) break;
        }
        *token_end = finish;
        return CODE_STRING;
    }
    if (identifier_start(c)) {
        uint32_t finish = position + 1;
        while (finish < end && identifier_char(text[finish])) ++finish;
        *token_end = finish;
        return is_keyword(text + position, finish - position)
            ? CODE_KEYWORD : CODE_TEXT;
    }
    if (c >= '0' && c <= '9') {
        uint32_t finish = position + 1;
        while (finish < end &&
               (identifier_char(text[finish]) || text[finish] == '.')) ++finish;
        *token_end = finish;
        return CODE_NUMBER;
    }
    *token_end = position + 1;
    return CODE_TEXT;
}

static void draw_source_line(struct code_state *state, rect_t area,
                             int32_t row, uint32_t line_number,
                             uint32_t start, uint32_t end,
                             uint32_t first_nonspace, uint32_t visible_columns,
                             int active) {
    const int32_t y = area.y + row * FONT_H;
    if (active) gfx_fill(area.x, y, area.w, FONT_H, CODE_LINE_HI);

    char number[16];
    const uint32_t digits = d_utoa((uint64_t)line_number + 1, number);
    const int32_t number_x = area.x + 4 + CODE_GUTTER_W -
                             (int32_t)digits * FONT_W - 6;
    gfx_text_n(number_x, y, number, digits, CODE_LINE_NUMBER);

    const uint32_t visible_start = state->left_column;
    const uint32_t visible_end = visible_start + visible_columns;
    uint32_t position = start;
    while (position < end) {
        uint32_t token_end = position + 1;
        const uint32_t color = token_color(state, end, position,
                                           first_nonspace, &token_end);
        const uint32_t column = position - start;
        const uint32_t token_column_end = token_end - start;
        const uint32_t draw_column = column > visible_start ? column : visible_start;
        const uint32_t draw_end = token_column_end < visible_end
            ? token_column_end : visible_end;
        if (draw_column < draw_end) {
            gfx_text_n(area.x + 4 + CODE_GUTTER_W +
                           (int32_t)(draw_column - visible_start) * FONT_W,
                       y, state->buffer + start + draw_column,
                       draw_end - draw_column, color);
        }
        position = token_end;
    }
}

static void draw_tree(struct code_state *state, rect_t side) {
    gfx_fill(side.x, side.y, side.w, side.h, CODE_SIDEBAR);
    gfx_text(side.x + 10, side.y + 8, "EXPLORER", C_WHITE);
    gfx_fill(side.x + 8, side.y + 28, side.w - 16, 1, CODE_TAB_BG);
    const uint32_t path_length = text_length(state->folder);
    uint32_t path_start = path_length > 9 ? path_length - 9 : 0;
    gfx_text_n(side.x + 8, side.y + 34, state->folder + path_start,
               path_length - path_start, C_TEXT_DIM);
    const int32_t list_y = side.y + 58;
    gfx_fill(side.x + 8, list_y - 5, side.w - 16, 1, CODE_TAB_BG);
    const int32_t rows = (side.h - 64) / CODE_TREE_ROW_H;
    if (rows <= 0) return;
    const uint32_t visible = (uint32_t)rows;
    if (state->tree_top > state->entry_count)
        state->tree_top = state->entry_count;
    const uint32_t total = state->entry_count - state->tree_top + 1;
    const uint32_t count = total < visible ? total : visible;
    for (uint32_t row = 0; row < count; ++row) {
        const int32_t y = list_y + (int32_t)row * CODE_TREE_ROW_H;
        const uint32_t selected = state->tree_top + row;
        if (selected == state->tree_selection)
            gfx_fill(side.x + 2, y, side.w - 4, CODE_TREE_ROW_H,
                     CODE_TREE_SELECTED);
        if (!selected) {
            gfx_text(side.x + 10, y + 2, "..  Parent folder", C_TEXT_DIM);
            continue;
        }
        const struct dir_item *entry = &state->entries[selected - 1];
        const char *prefix = entry->type == WELOVEC_USER_FS_DIRECTORY ? "> " : "  ";
        const uint32_t prefix_length = text_length(prefix);
        const uint32_t max_name = (uint32_t)((side.w - 16) / FONT_W) - prefix_length;
        gfx_text(side.x + 8, y + 2, prefix,
                 entry->type == WELOVEC_USER_FS_DIRECTORY
                     ? UINT32_C(0xffdcdcaa) : C_TEXT_DIM);
        gfx_text_n(side.x + 8 + (int32_t)prefix_length * FONT_W, y + 2,
                   entry->name,
                   text_length(entry->name) < max_name
                       ? text_length(entry->name) : max_name,
                   entry->type == WELOVEC_USER_FS_DIRECTORY
                       ? UINT32_C(0xffdcdcaa) : C_TEXT);
    }
}

static void draw_editor(struct code_state *state, rect_t pane) {
    const rect_t tab = rect_make(pane.x, pane.y, pane.w, CODE_TAB_H);
    gfx_fill(tab.x, tab.y, tab.w, tab.h, CODE_TAB_BG);
    gfx_fill(tab.x, tab.y + tab.h - 1, tab.w, 1, CODE_STATUS);
    char name[64];
    if (state->path[0]) {
        uint32_t start = text_length(state->path);
        while (start && state->path[start - 1] != '/') --start;
        d_strcpy(name, sizeof(name), state->path + start);
    } else {
        d_strcpy(name, sizeof(name), "Welcome");
    }
    if (state->dirty) d_strcat(name, sizeof(name), " *");
    gfx_text(tab.x + 12, tab.y + 4, name, C_TEXT);

    const rect_t area = text_area(pane);
    gfx_fill(area.x, area.y, area.w, area.h, CODE_EDITOR);
    gfx_fill(area.x, area.y, CODE_GUTTER_W + 8, area.h, CODE_GUTTER);
    const int32_t rows = area.h / FONT_H;
    const int32_t text_x = area.x + 4 + CODE_GUTTER_W;
    const int32_t columns = (area.w - CODE_GUTTER_W - 12) / FONT_W;
    const uint32_t visible_columns = columns > 0 ? (uint32_t)columns : 1;
    uint32_t offset = state->buffer ? offset_of_line(state, state->top_line) : 0;
    uint32_t line_number = state->top_line;
    const uint32_t cursor_line = line_of(state, state->cursor);
    const uint32_t cursor_column = state->cursor - line_start(state, state->cursor);
    int cursor_drawn = 0;

    for (int32_t row = 0; row < rows; ++row) {
        const uint32_t start = offset;
        const uint32_t end = state->buffer ? line_end(state, start) : 0;
        uint32_t first_nonspace = start;
        while (first_nonspace < end &&
               (state->buffer[first_nonspace] == ' ' ||
                state->buffer[first_nonspace] == '\t')) ++first_nonspace;
        draw_source_line(state, area, row, line_number, start, end,
                         first_nonspace, visible_columns,
                         line_number == cursor_line);
        if (line_number == cursor_line &&
            cursor_column >= state->left_column &&
            cursor_column < state->left_column + visible_columns) {
            gfx_fill(text_x + (int32_t)(cursor_column - state->left_column) * FONT_W,
                     area.y + row * FONT_H, 2, FONT_H, CODE_CARET);
            cursor_drawn = 1;
        }
        if (!state->buffer || offset >= state->length) {
            if (!cursor_drawn && line_number == cursor_line) {
                gfx_fill(text_x, area.y + row * FONT_H, 2, FONT_H, CODE_CARET);
                cursor_drawn = 1;
            }
            if (!state->buffer || offset == state->length) break;
        }
        offset = next_line(state, offset);
        ++line_number;
    }
    if (!state->length && state->cursor == 0)
        gfx_text(text_x + 8, area.y + 2 * FONT_H,
                 "Open a file or start typing", C_TEXT_DIM);
}

static void draw(struct window *window, rect_t client) {
    struct code_state *state = window->state;
    gfx_fill(client.x, client.y, client.w, client.h, CODE_BG);
    for (int button = 0; button < CODE_BUTTON_COUNT; ++button)
        ui_button(button_rect(client, button), k_code_buttons[button], 1);

    const rect_t side = sidebar_area(client);
    draw_tree(state, side);
    const rect_t pane = editor_pane(client);
    draw_editor(state, pane);

    const int32_t status_y = client.y + client.h - CODE_STATUS_H;
    gfx_fill(client.x, status_y, client.w, CODE_STATUS_H, CODE_STATUS);
    const uint32_t line = line_of(state, state->cursor);
    const uint32_t column = state->cursor - line_start(state, state->cursor);
    char status[64], number[16];
    d_strcpy(status, sizeof(status), "Ln ");
    d_utoa(line + 1, number);
    d_strcat(status, sizeof(status), number);
    d_strcat(status, sizeof(status), "  Col ");
    d_utoa(column + 1, number);
    d_strcat(status, sizeof(status), number);
    d_strcat(status, sizeof(status), "  C");
    gfx_text(client.x + 8, status_y + 2, status, C_WHITE);
    if (state->status[0])
        ui_text_right(client.x + client.w - 6, status_y + 2,
                      state->status, C_WHITE);
}

static void scroll_cursor_into_view(struct window *window) {
    struct code_state *state = window->state;
    const rect_t area = text_area(editor_pane(wm_client(window)));
    uint32_t rows = area.h > 0 ? (uint32_t)(area.h / FONT_H) : 1;
    int32_t columns = (area.w - CODE_GUTTER_W - 12) / FONT_W;
    if (!rows) rows = 1;
    if (columns <= 0) columns = 1;
    const uint32_t line = line_of(state, state->cursor);
    const uint32_t column = state->cursor - line_start(state, state->cursor);
    if (line < state->top_line) state->top_line = line;
    if (line >= state->top_line + rows) state->top_line = line - rows + 1;
    if (column < state->left_column) state->left_column = column;
    if (column >= state->left_column + (uint32_t)columns)
        state->left_column = column - (uint32_t)columns + 1;
}

static int insert_text(struct code_state *state, const char *text,
                       uint32_t length) {
    if (state->binary || !grow(state, state->length + length + 1)) return 0;
    memmove(state->buffer + state->cursor + length,
            state->buffer + state->cursor, state->length - state->cursor);
    memcpy(state->buffer + state->cursor, text, length);
    state->length += length;
    state->cursor += length;
    state->dirty = 1;
    return 1;
}

static void erase_before(struct code_state *state) {
    if (state->binary || !state->cursor) return;
    memmove(state->buffer + state->cursor - 1,
            state->buffer + state->cursor,
            state->length - state->cursor);
    --state->cursor;
    --state->length;
    state->dirty = 1;
}

static void mouse(struct window *window, const struct mouse_event *event) {
    struct code_state *state = window->state;
    const rect_t client = wm_client(window);
    const rect_t side = sidebar_area(client);
    if (event->type == MOUSE_WHEEL) {
        if (event->x < CODE_SIDEBAR_W) {
            int32_t top = (int32_t)state->tree_top - event->wheel * 3;
            if (top < 0) top = 0;
            if (top > (int32_t)state->entry_count) top = (int32_t)state->entry_count;
            state->tree_top = (uint32_t)top;
        } else {
            int32_t top = (int32_t)state->top_line - event->wheel * 3;
            if (top < 0) top = 0;
            state->top_line = (uint32_t)top;
        }
        wm_invalidate(window);
        return;
    }
    if (event->type != MOUSE_DOWN) return;
    for (int button = 0; button < CODE_BUTTON_COUNT; ++button) {
        if (rect_hit(button_rect(client, button), event->x, event->y)) {
            button_action(window, button);
            return;
        }
    }
    if (rect_hit(side, event->x, event->y)) {
        const int32_t list_y = side.y + 58;
        if (event->y < list_y) return;
        const uint32_t row = (uint32_t)(event->y - list_y) / CODE_TREE_ROW_H;
        const uint32_t index = state->tree_top + row;
        state->tree_selection = index;
        if (!index) {
            d_parent_path(state->folder);
            refresh_tree(state);
            state->tree_top = 0;
            wm_invalidate(window);
            return;
        }
        if (index > state->entry_count) return;
        const struct dir_item *entry = &state->entries[index - 1];
        char path[CODE_PATH_CAP];
        if (!d_join_path(path, sizeof(path), state->folder, entry->name)) return;
        if (entry->type == WELOVEC_USER_FS_DIRECTORY) {
            d_strcpy(state->folder, sizeof(state->folder), path);
            refresh_tree(state);
            state->tree_top = 0;
            state->tree_selection = 0;
            wm_invalidate(window);
        } else {
            request_open(window, path);
        }
        return;
    }

    const rect_t pane = editor_pane(client);
    const rect_t area = text_area(pane);
    const int32_t text_x = area.x + 4 + CODE_GUTTER_W;
    if (!rect_hit(area, event->x, event->y) || !state->buffer ||
        event->x < text_x) return;
    const uint32_t line = state->top_line +
        (uint32_t)((event->y - area.y) / FONT_H);
    const uint32_t start = offset_of_line(state, line);
    const uint32_t end = line_end(state, start);
    uint32_t column = state->left_column +
        (uint32_t)((event->x - text_x) / FONT_W);
    const uint32_t line_length = end - start;
    if (column > line_length) column = line_length;
    state->cursor = start + column;
    scroll_cursor_into_view(window);
    wm_invalidate(window);
}

static void move_vertical(struct code_state *state, int direction) {
    const uint32_t start = line_start(state, state->cursor);
    const uint32_t column = state->cursor - start;
    uint32_t target = start;
    if (direction < 0) {
        if (!target) return;
        target = line_start(state, target - 1);
    } else {
        const uint32_t end = line_end(state, target);
        if (end >= state->length) return;
        target = end + 1;
    }
    const uint32_t end = line_end(state, target);
    state->cursor = target + (column < end - target ? column : end - target);
}

static int key(struct window *window, int key_code) {
    struct code_state *state = window->state;
    if (key_code == KEY_LEFT) {
        if (state->cursor) --state->cursor;
    } else if (key_code == KEY_RIGHT) {
        if (state->cursor < state->length) ++state->cursor;
    } else if (key_code == KEY_UP || key_code == KEY_CTRL_UP) {
        const int steps = key_code == KEY_CTRL_UP ? 10 : 1;
        for (int i = 0; i < steps; ++i) move_vertical(state, -1);
    } else if (key_code == KEY_DOWN || key_code == KEY_CTRL_DOWN) {
        const int steps = key_code == KEY_CTRL_DOWN ? 10 : 1;
        for (int i = 0; i < steps; ++i) move_vertical(state, 1);
    } else if (key_code == '\b' || key_code == 0x7f) {
        erase_before(state);
    } else if (key_code == '\n') {
        (void)insert_text(state, "\n", 1);
    } else if (key_code == '\t') {
        (void)insert_text(state, "    ", 4);
    } else if (key_code >= 32 && key_code < 127) {
        const char c = (char)key_code;
        (void)insert_text(state, &c, 1);
    } else {
        return 0;
    }
    state->status[0] = '\0';
    set_title(window);
    scroll_cursor_into_view(window);
    wm_invalidate(window);
    wm_taskbar_repaint();
    return 1;
}

const struct app_ops app_code = {
    "Code", 800, 560, 480, 320, 1,
    open_app, close_app, query_close, draw, mouse, key, 0, 0, 0, 0,
};
