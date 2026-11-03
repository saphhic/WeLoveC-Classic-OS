// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the main loop and command dispatch for the experimental shell.

#include <stdint.h>

#include "include/shell.h"
#include "include/command_config.h"
#include "user/filesystem.h"
#include "user/framebuffer.h"
#include "user/input.h"
#include "user/power.h"
#include "user/process.h"
#include "user/security.h"
#include "user/stdio.h"
#include "user/syscall.h"

#define COLOR_WHITE UINT32_C(0xffffffff)
#define COLOR_BLACK UINT32_C(0xff000000)
#define COLOR_GREEN UINT32_C(0xff70e080)
#define COLOR_RED UINT32_C(0xffff6666)
#define COLOR_YELLOW UINT32_C(0xffffd166)
#define SHELL_HELP_OUTPUT_CAPACITY 8192u
#define SHELL_HELP_SYSCALL_CHUNK 128u
#define SHELL_HELP_MAX_SPANS (WELOVEC_USER_FS_MAX_ENTRIES * 4u + 4u)
#define SHELL_APPS_PATH_CAPACITY WELOVEC_USER_PATH_MAX

struct shell_help_span {
    uint32_t offset;
    uint32_t length;
    uint32_t color;
};

static struct welovec_user_dir_entry g_command_entries[WELOVEC_USER_FS_MAX_ENTRIES];
static char g_help_output[SHELL_HELP_OUTPUT_CAPACITY];
static struct shell_help_span g_help_spans[SHELL_HELP_MAX_SPANS];

static int help_append(const char *text, uint32_t color, uint32_t *length,
                      uint32_t *span_count) {
    if (!text || !length || !span_count ||
        *length >= SHELL_HELP_OUTPUT_CAPACITY) return 0;
    const uint32_t start = *length;
    while (*text) {
        if (*length + 1 >= SHELL_HELP_OUTPUT_CAPACITY) {
            g_help_output[*length] = '\0';
            return 0;
        }
        g_help_output[(*length)++] = *text++;
    }
    g_help_output[*length] = '\0';
    if (*length != start) {
        if (*span_count &&
            g_help_spans[*span_count - 1].color == color &&
            g_help_spans[*span_count - 1].offset +
                g_help_spans[*span_count - 1].length == start) {
            g_help_spans[*span_count - 1].length += *length - start;
        } else if (*span_count < SHELL_HELP_MAX_SPANS) {
            g_help_spans[*span_count] = (struct shell_help_span){
                start, *length - start, color
            };
            ++*span_count;
        } else {
            return 0;
        }
    }
    return 1;
}

static void print_help_output(uint32_t span_count) {
    for (uint32_t span_index = 0; span_index < span_count; ++span_index) {
        const struct shell_help_span span = g_help_spans[span_index];
        uint32_t offset = span.offset;
        const uint32_t limit = span.offset + span.length;
        while (offset < limit) {
            uint32_t end = offset;
            while (end < limit && end - offset < SHELL_HELP_SYSCALL_CHUNK &&
               g_help_output[end] != '\n') ++end;
            if (end < limit && g_help_output[end] == '\n') ++end;
            if (end == offset) break;
            const char saved = g_help_output[end];
            g_help_output[end] = '\0';
            shell_print(g_help_output + offset, span.color);
            g_help_output[end] = saved;
            offset = end;
        }
    }
}

#define SHELL_HISTORY_CAPACITY 32u

struct shell_line_editor {
    char text[SHELL_LINE_CAPACITY];
    uint32_t length;
    uint32_t cursor;
    uint32_t rendered_length;
    uint32_t maximum_length;
    struct welovec_fb_text_cursor origin;
    struct welovec_fb_text_cursor caret;
    uint32_t cursor_visible;
    uint32_t scroll_view;
};

static char g_shell_history[SHELL_HISTORY_CAPACITY][SHELL_LINE_CAPACITY];
static char g_shell_history_draft[SHELL_LINE_CAPACITY];
static uint32_t g_shell_history_count;
static uint32_t g_shell_history_next;
static uint32_t g_shell_history_offset;

static void print_signed_status(int64_t value) {
    if (value < 0) {
        shell_print("-", COLOR_RED);
        shell_print_unsigned(UINT64_C(0) - (uint64_t)value, COLOR_RED);
    } else {
        shell_print_unsigned((uint64_t)value, COLOR_RED);
    }
}

static int text_equal(const char *left, const char *right) {
    uint32_t index = 0;
    while (left[index] && right[index] && left[index] == right[index]) ++index;
    return left[index] == '\0' && right[index] == '\0';
}

static void copy_text(char *destination, uint32_t capacity, const char *source) {
    if (!capacity) return;
    uint32_t index = 0;
    while (source[index] && index + 1 < capacity) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static int resolve_shell_path(const struct shell_state *state, const char *path,
                              char output[SHELL_PATH_CAPACITY]) {
    if (path[0] != '~' || (path[1] && path[1] != '/'))
        return shell_resolve_path(state->cwd, path, output);
    char expanded[SHELL_PATH_CAPACITY];
    uint32_t cursor = 0;
    const char *prefix = "/userland/tmp";
    while (prefix[cursor] && cursor + 1 < sizeof(expanded)) {
        expanded[cursor] = prefix[cursor];
        ++cursor;
    }
    uint32_t tail = path[1] == '/' ? 2 : 1;
    if (path[tail]) {
        if (cursor + 1 >= sizeof(expanded)) return 0;
        expanded[cursor++] = '/';
        while (path[tail] && cursor + 1 < sizeof(expanded))
            expanded[cursor++] = path[tail++];
        if (path[tail]) return 0;
    }
    expanded[cursor] = '\0';
    return shell_resolve_path("/", expanded, output);
}

static int config_file_name(const char *name) {
    uint32_t length = 0;
    while (name[length]) ++length;
    return length > 5 && name[length - 5] == '.' && name[length - 4] == 'c' &&
           name[length - 3] == 'o' && name[length - 2] == 'n' &&
           name[length - 1] == 'f';
}

static int elf_file_name(const char *name) {
    uint32_t length = 0;
    while (name[length]) ++length;
    return length > 4 && name[length - 4] == '.' && name[length - 3] == 'e' &&
           name[length - 2] == 'l' && name[length - 1] == 'f';
}

static void show_help(void) {
    uint32_t output_length = 0;
    uint32_t span_count = 0;
    g_help_output[0] = '\0';
    const int64_t count = welovec_fs_list(
        SHELL_COMMAND_DIR, g_command_entries, WELOVEC_USER_FS_MAX_ENTRIES);
    if (count < 0) {
        shell_print("help: cannot read command descriptions\n", COLOR_RED);
        return;
    }
    for (int64_t index = 0; index < count; ++index) {
        if (g_command_entries[index].type != WELOVEC_USER_FS_FILE ||
            !config_file_name(g_command_entries[index].name)) continue;
        char path[sizeof(SHELL_COMMAND_DIR) + WELOVEC_USER_PATH_MAX + 2];
        uint32_t cursor = 0;
        while (SHELL_COMMAND_DIR[cursor]) {
            path[cursor] = SHELL_COMMAND_DIR[cursor];
            ++cursor;
        }
        path[cursor++] = '/';
        uint32_t name_index = 0;
        while (g_command_entries[index].name[name_index] &&
               cursor + 1 < sizeof(path))
            path[cursor++] = g_command_entries[index].name[name_index++];
        path[cursor] = '\0';

        struct shell_command_config config;
        if (!shell_cfg_load_path(path, &config)) continue;
        if (!help_append("  ", COLOR_WHITE, &output_length, &span_count) ||
            !help_append(config.name, COLOR_YELLOW, &output_length, &span_count) ||
            !help_append(" - ", COLOR_WHITE, &output_length, &span_count) ||
            !help_append(config.description[0] ? config.description :
                         "No description.", COLOR_WHITE, &output_length,
                         &span_count) ||
            !help_append("\n", COLOR_WHITE, &output_length, &span_count)) {
            (void)help_append("  ... output truncated\n", COLOR_YELLOW,
                              &output_length, &span_count);
            break;
        }
    }
    (void)help_append("  Installed programs are under /userspace/bin and /userspace/bin/shell/bin.\n",
                      COLOR_YELLOW, &output_length, &span_count);
    (void)help_append("  apps recursively lists every .elf file in the filesystem.\n",
                      COLOR_YELLOW, &output_length, &span_count);
    shell_print("Available commands\n", COLOR_GREEN);
    print_help_output(span_count);
}

static int has_elf_extension(const char *name) {
    uint32_t length = 0;
    while (name && name[length]) ++length;
    if (length <= 4 || name[length - 4] != '.') return 0;
    char e = name[length - 3], l = name[length - 2], f = name[length - 1];
    if (e >= 'A' && e <= 'Z') e = (char)(e + ('a' - 'A'));
    if (l >= 'A' && l <= 'Z') l = (char)(l + ('a' - 'A'));
    if (f >= 'A' && f <= 'Z') f = (char)(f + ('a' - 'A'));
    return e == 'e' && l == 'l' && f == 'f';
}

static void show_apps(void) {
    uint32_t capacity = 16;
    uint32_t queued = 0;
    uint32_t next = 0;
    uint32_t found = 0;
    int incomplete = 0;
    char (*directories)[SHELL_APPS_PATH_CAPACITY] =
        (char (*)[SHELL_APPS_PATH_CAPACITY])welovec_mmap(
            (uint64_t)capacity * SHELL_APPS_PATH_CAPACITY);
    if (!directories) {
        shell_print("apps: out of memory while scanning the filesystem\n",
                    COLOR_RED);
        return;
    }
    copy_text(directories[queued++], SHELL_APPS_PATH_CAPACITY, "/");
    shell_print("ELF files found in the filesystem\n", COLOR_GREEN);

    while (next < queued) {
        char current_directory[SHELL_APPS_PATH_CAPACITY];
        copy_text(current_directory, sizeof(current_directory),
                  directories[next++]);
        const char *directory = current_directory;
        uint64_t offset = 0;
        for (;;) {
            const int64_t count = welovec_fs_list_range(
                directory, g_command_entries, WELOVEC_USER_FS_MAX_ENTRIES,
                offset);
            if (count < 0) {
                incomplete = 1;
                break;
            }
            if (!count) break;
            for (int64_t index = 0; index < count; ++index) {
                const struct welovec_user_dir_entry *entry =
                    &g_command_entries[index];
                char path[SHELL_APPS_PATH_CAPACITY];
                uint32_t used = 0;
                while (directory[used] && used + 1 < sizeof(path)) {
                    path[used] = directory[used];
                    ++used;
                }
                if (directory[used]) {
                    incomplete = 1;
                    continue;
                }
                if (used > 1 && path[used - 1] == '/') --used;
                if (used && path[used - 1] != '/') {
                    if (used + 1 >= sizeof(path)) {
                        incomplete = 1;
                        continue;
                    }
                    path[used++] = '/';
                }
                uint32_t name_index = 0;
                while (entry->name[name_index] && used + 1 < sizeof(path))
                    path[used++] = entry->name[name_index++];
                if (entry->name[name_index]) {
                    incomplete = 1;
                    continue;
                }
                path[used] = '\0';

                if (entry->type == WELOVEC_USER_FS_DIRECTORY) {
                    if (queued == capacity) {
                        if (capacity > UINT32_MAX / 2u) {
                            incomplete = 1;
                            continue;
                        }
                        const uint32_t new_capacity = capacity * 2u;
                        char (*grown)[SHELL_APPS_PATH_CAPACITY] =
                            (char (*)[SHELL_APPS_PATH_CAPACITY])welovec_mmap(
                                (uint64_t)new_capacity *
                                SHELL_APPS_PATH_CAPACITY);
                        if (!grown) {
                            incomplete = 1;
                            continue;
                        }
                        for (uint32_t row = 0; row < queued; ++row)
                            copy_text(grown[row], SHELL_APPS_PATH_CAPACITY,
                                      directories[row]);
                        (void)welovec_munmap(
                            directories,
                            (uint64_t)capacity * SHELL_APPS_PATH_CAPACITY);
                        directories = grown;
                        capacity = new_capacity;
                    }
                    copy_text(directories[queued++],
                              SHELL_APPS_PATH_CAPACITY, path);
                } else if (has_elf_extension(entry->name)) {
                    shell_print("  ", COLOR_WHITE);
                    shell_print(path, COLOR_YELLOW);
                    shell_print("\n", COLOR_WHITE);
                    found = 1;
                }
            }
            offset += (uint64_t)count;
            if ((uint64_t)count < WELOVEC_USER_FS_MAX_ENTRIES) break;
        }
    }
    if (!found) shell_print("  No ELF files were found.\n", COLOR_RED);
    if (incomplete)
        shell_print("  Some paths could not be scanned.\n", COLOR_YELLOW);
    (void)welovec_munmap(
        directories, (uint64_t)capacity * SHELL_APPS_PATH_CAPACITY);
}

static void reboot_virtual_machine(void) {
    const int64_t result = welovec_system_reboot(1);
    if (result < 0) shell_print("reboot: VM reset request failed\n", COLOR_RED);
    else shell_print("reboot: VM reset request sent\n", COLOR_YELLOW);
}

static void reboot_hardware(void) {
    const int64_t result = welovec_system_reboot(0);
    if (result < 0) shell_print("reboot: hardware reset request failed\n", COLOR_RED);
    else shell_print("reboot: hardware reset request sent\n", COLOR_YELLOW);
}

static void shutdown_virtual_machine(void) {
    const int64_t result = welovec_system_shutdown(1);
    if (result < 0) shell_print("shutdown: VM power-off request failed\n", COLOR_RED);
    else shell_print("shutdown: VM power-off request sent\n", COLOR_YELLOW);
}

static void shutdown_hardware(void) {
    const int64_t result = welovec_system_shutdown(0);
    if (result < 0) shell_print("shutdown: ACPI power-off is unavailable\n", COLOR_RED);
    else shell_print("shutdown: ACPI power-off request sent\n", COLOR_YELLOW);
}

static void change_directory(struct shell_state *state,
                             char arguments[SHELL_ARGUMENT_CAPACITY][SHELL_ARGUMENT_LENGTH],
                             uint32_t argument_count) {
    char requested[SHELL_PATH_CAPACITY];
    if (argument_count > 2) {
        shell_print("cd: expected at most one path\n", COLOR_RED);
        return;
    }
    if (argument_count == 1 || text_equal(arguments[1], "~")) {
        copy_text(requested, sizeof(requested), "/userland/tmp");
    } else {
        copy_text(requested, sizeof(requested), arguments[1]);
    }

    char resolved[SHELL_PATH_CAPACITY];
    if (!resolve_shell_path(state, requested, resolved)) {
        shell_print("cd: path is too long or invalid\n", COLOR_RED);
        return;
    }
    struct welovec_user_file_info info;
    const int64_t result = welovec_fs_stat(resolved, &info);
    if (result < 0 || info.type != WELOVEC_USER_FS_DIRECTORY) {
        shell_print("cd: directory not found or access denied\n", COLOR_RED);
        return;
    }
    copy_text(state->cwd, sizeof(state->cwd), resolved);
}

static void run_external(const struct shell_state *state,
                         const struct shell_command_config *config,
                         char arguments[SHELL_ARGUMENT_CAPACITY][SHELL_ARGUMENT_LENGTH],
                         uint32_t argument_count) {
    if (!config->executable[0] || config->executable[0] != '/') {
        shell_print("shell: command config has no absolute executable path\n", COLOR_RED);
        return;
    }
    const int pass_cwd = text_equal(config->action, "admin-run") ||
                         text_equal(config->action, "exec-run") ||
                         text_equal(config->action, "cwd-first");
    if (argument_count + (pass_cwd ? 1u : 0u) > SHELL_ARGUMENT_CAPACITY) {
        shell_print("shell: too many arguments\n", COLOR_RED);
        return;
    }

    char resolved[SHELL_ARGUMENT_CAPACITY][SHELL_PATH_CAPACITY];
    const char *exec_arguments[SHELL_ARGUMENT_CAPACITY];
    copy_text(resolved[0], sizeof(resolved[0]), arguments[0]);
    exec_arguments[0] = resolved[0];
    uint32_t exec_count = argument_count;
    if (pass_cwd) {
        copy_text(resolved[1], sizeof(resolved[1]), state->cwd);
        exec_arguments[1] = resolved[1];
        exec_count++;
    }
    for (uint32_t index = 1; index < argument_count; ++index) {
        const uint32_t target = index + (pass_cwd ? 1u : 0u);
        copy_text(resolved[target], sizeof(resolved[target]), arguments[index]);
        exec_arguments[target] = resolved[target];
    }

    const uint32_t first_parameter = pass_cwd ? 2u : 1u;
    const uint32_t parameter_count = exec_count - first_parameter;
    const int install_all_mode = text_equal(config->name, "install") &&
                                 parameter_count >= 2 &&
                                 text_equal(exec_arguments[first_parameter], "-all");
    const uint32_t last_optional_minimum = install_all_mode ? 3u : 2u;
    for (uint32_t index = first_parameter; index < exec_count; ++index) {
        const int is_all = text_equal(config->path_args, "all");
        const int is_first = text_equal(config->path_args, "first") &&
                             index == first_parameter;
        const int is_last = text_equal(config->path_args, "last") &&
                            index == exec_count - 1;
        const int is_last_optional = text_equal(config->path_args, "last2") &&
                                     parameter_count >= last_optional_minimum &&
                                     index == exec_count - 1;
        if (!(is_all || is_first || is_last || is_last_optional)) continue;
        const uint32_t source_index = index - (pass_cwd ? 1u : 0u);
        if (!resolve_shell_path(state, arguments[source_index], resolved[index])) {
            shell_print("shell: invalid or overlong path argument\n", COLOR_RED);
            return;
        }
    }

    const int64_t child = welovec_exec(config->executable, exec_count,
                                       exec_arguments, WELOVEC_PRIORITY_NORMAL);
    if (child < 0) {
        shell_print("shell: could not start command (error ", COLOR_RED);
        shell_print_unsigned((uint64_t)(-child), COLOR_RED);
        shell_print(")\n", COLOR_RED);
        return;
    }
    int64_t status = 0;
    const int64_t waited = welovec_waitpid((uint64_t)child, &status, 0);
    if (waited < 0) {
        shell_print("shell: waitpid failed\n", COLOR_RED);
    } else if (status != 0) {
        shell_print("shell: command exited with status ", COLOR_RED);
        print_signed_status(status);
        shell_print("\n", COLOR_RED);
    }
}

static void run_command(struct shell_state *state,
                        char arguments[SHELL_ARGUMENT_CAPACITY][SHELL_ARGUMENT_LENGTH],
                        uint32_t argument_count) {
    if (!argument_count) return;
    if (text_equal(arguments[0], "help")) {
        show_help();
        return;
    }
    if (text_equal(arguments[0], "apps")) {
        if (argument_count != 1) {
            shell_print("apps: takes no arguments\n", COLOR_RED);
            return;
        }
        show_apps();
        return;
    }
    if (text_equal(arguments[0], "reboot")) {
        if (welovec_running_in_virtual_machine()) reboot_virtual_machine();
        else reboot_hardware();
        return;
    }
    if (text_equal(arguments[0], "shutdown")) {
        if (welovec_running_in_virtual_machine()) shutdown_virtual_machine();
        else shutdown_hardware();
        return;
    }

    if (elf_file_name(arguments[0])) {
        struct shell_command_config direct_config;
        direct_config.executable[0] = '\0';
        direct_config.action[0] = '\0';
        direct_config.path_args[0] = '\0';
        if (!resolve_shell_path(state, arguments[0], direct_config.executable)) {
            shell_print("shell: invalid ELF path\n", COLOR_RED);
            return;
        }
        run_external(state, &direct_config, arguments, argument_count);
        return;
    }

    struct shell_command_config config;
    if (!shell_cfg_find(arguments[0], &config)) {
        shell_print("shell: command not found; type help\n", COLOR_RED);
        return;
    }
    if (text_equal(config.action, "exit")) {
        welovec_exit(0);
    } else if (text_equal(config.action, "pwd")) {
        shell_print(state->cwd, COLOR_WHITE);
        shell_print("\n", COLOR_WHITE);
    } else if (text_equal(config.action, "cd")) {
        change_directory(state, arguments, argument_count);
    } else if (config.executable[0]) {
        run_external(state, &config, arguments, argument_count);
    } else {
        shell_print("shell: command config has no action or executable\n", COLOR_RED);
    }
}

static void initialize_shell_state(struct shell_state *state) {
    copy_text(state->cwd, sizeof(state->cwd), "/userland/tmp");
    copy_text(state->username, sizeof(state->username), "guest");
    state->authenticated = 0;
    struct welovec_user_status status;
    if (welovec_get_user_status(&status) == 0 && status.name[0]) {
        copy_text(state->username, sizeof(state->username), status.name);
        if (status.authenticated) state->authenticated = 1;
    }
}

static void editor_capture_origin(struct shell_line_editor *editor) {
    struct welovec_fb_info info;
    if (welovec_print_cursor_get(&editor->origin) < 0) {
        editor->origin.x = 8;
        editor->origin.y = 8;
    }
    if (welovec_fb_get_info(&info) < 0) {
        /* A framebuffer-query failure must not disable shell input. Keep the line
           editor usable even when a framebuffer query is unavailable. */
        editor->maximum_length = SHELL_LINE_CAPACITY - 1;
        return;
    }
    const int32_t right = (int32_t)info.width - 8;
    uint32_t cells = editor->origin.x < right
        ? (uint32_t)(right - editor->origin.x) / 8u : 0;
    if (cells) --cells;
    if (!cells) cells = 1;
    if (cells >= SHELL_LINE_CAPACITY) cells = SHELL_LINE_CAPACITY - 1;
    editor->maximum_length = cells;
}

static void editor_erase_cursor(struct shell_line_editor *editor,
                                int restore_character) {
    if (!editor->cursor_visible) return;
    (void)welovec_fb_rect(editor->caret.x, editor->caret.y, 8, 8,
                          COLOR_BLACK);
    const uint32_t old_cursor = editor->caret.x >= editor->origin.x
        ? (uint32_t)(editor->caret.x - editor->origin.x) / 8u : UINT32_MAX;
    if (restore_character && old_cursor < editor->length) {
        const char character[2] = {editor->text[old_cursor], '\0'};
        (void)welovec_print_cursor_set(editor->caret.x, editor->caret.y);
        shell_print(character, COLOR_WHITE);
    }
    (void)welovec_print_cursor_set(editor->caret.x, editor->caret.y);
    editor->cursor_visible = 0;
}

static void editor_render_from(struct shell_line_editor *editor, uint32_t start) {
    editor_erase_cursor(editor, 0);

    uint32_t clear_columns = editor->rendered_length;
    if (editor->length > clear_columns) clear_columns = editor->length;
    if (start > clear_columns) start = clear_columns;
    /* Appending text only needs the old caret cell cleared. Redrawing the
       suffix is necessary only when existing glyphs shift or a tail is removed. */
    if (clear_columns > start &&
        (start < editor->rendered_length ||
         editor->length < editor->rendered_length)) {
        (void)welovec_fb_rect(
            editor->origin.x + (int32_t)(start * 8u), editor->origin.y,
            (int32_t)((clear_columns - start) * 8u), 8, COLOR_BLACK);
    }

    if (editor->length > start) {
        (void)welovec_print_cursor_set(
            editor->origin.x + (int32_t)(start * 8u), editor->origin.y);
        shell_print(editor->text + start, COLOR_WHITE);
    }
    editor->rendered_length = editor->length;
    editor->caret.x = editor->origin.x + (int32_t)(editor->cursor * 8u);
    editor->caret.y = editor->origin.y;
    (void)welovec_print_cursor_set(editor->caret.x, editor->caret.y);
}

static void editor_render(struct shell_line_editor *editor) {
    editor_render_from(editor, 0);
}

static void editor_show_cursor(struct shell_line_editor *editor) {
    if (editor->scroll_view) return;
    (void)welovec_fb_rect(editor->caret.x, editor->caret.y, 2, 8,
                          COLOR_WHITE);
    editor->cursor_visible = 1;
}

static void editor_hide_cursor(struct shell_line_editor *editor) {
    editor_erase_cursor(editor, 1);
}

static void editor_changed_from(struct shell_line_editor *editor, uint32_t start) {
    editor_render_from(editor, start);
    editor_show_cursor(editor);
}

static void editor_move_cursor(struct shell_line_editor *editor) {
    editor_erase_cursor(editor, 1);
    editor->caret.x = editor->origin.x + (int32_t)(editor->cursor * 8u);
    editor->caret.y = editor->origin.y;
    (void)welovec_print_cursor_set(editor->caret.x, editor->caret.y);
    editor_show_cursor(editor);
}

static void editor_insert(struct shell_line_editor *editor, char character) {
    if (editor->length >= editor->maximum_length ||
        editor->length + 1 >= sizeof(editor->text)) return;
    const uint32_t changed_from = editor->cursor;
    for (uint32_t index = editor->length; index > editor->cursor; --index)
        editor->text[index] = editor->text[index - 1];
    editor->text[editor->cursor++] = character;
    ++editor->length;
    editor->text[editor->length] = '\0';
    editor_changed_from(editor, changed_from);
}

static void editor_backspace(struct shell_line_editor *editor) {
    if (!editor->cursor) return;
    for (uint32_t index = editor->cursor - 1; index + 1 < editor->length; ++index)
        editor->text[index] = editor->text[index + 1];
    --editor->cursor;
    --editor->length;
    editor->text[editor->length] = '\0';
    editor_changed_from(editor, editor->cursor);
}

static void editor_delete(struct shell_line_editor *editor) {
    if (editor->cursor >= editor->length) return;
    for (uint32_t index = editor->cursor; index + 1 < editor->length; ++index)
        editor->text[index] = editor->text[index + 1];
    --editor->length;
    editor->text[editor->length] = '\0';
    editor_changed_from(editor, editor->cursor);
}

static void editor_history_add(const char *text) {
    if (!text[0]) return;
    uint32_t index = 0;
    while (text[index] && index + 1 < SHELL_LINE_CAPACITY) {
        g_shell_history[g_shell_history_next][index] = text[index];
        ++index;
    }
    g_shell_history[g_shell_history_next][index] = '\0';
    g_shell_history_next = (g_shell_history_next + 1) % SHELL_HISTORY_CAPACITY;
    if (g_shell_history_count < SHELL_HISTORY_CAPACITY) ++g_shell_history_count;
    g_shell_history_offset = 0;
}

static void editor_load_text(struct shell_line_editor *editor, const char *text) {
    uint32_t length = 0;
    while (text[length] && length < editor->maximum_length &&
           length + 1 < sizeof(editor->text)) {
        editor->text[length] = text[length];
        ++length;
    }
    editor->text[length] = '\0';
    editor->length = length;
    editor->cursor = length;
    editor_changed_from(editor, 0);
}

static void editor_history_move(struct shell_line_editor *editor, int direction) {
    if (!g_shell_history_count) return;
    if (!g_shell_history_offset) {
        uint32_t index = 0;
        while (editor->text[index] && index + 1 < sizeof(g_shell_history_draft)) {
            g_shell_history_draft[index] = editor->text[index];
            ++index;
        }
        g_shell_history_draft[index] = '\0';
    }
    if (direction < 0) {
        if (g_shell_history_offset < g_shell_history_count)
            ++g_shell_history_offset;
    } else if (g_shell_history_offset) {
        --g_shell_history_offset;
    }
    if (!g_shell_history_offset) {
        editor_load_text(editor, g_shell_history_draft);
        return;
    }
    const uint32_t history_index =
        (g_shell_history_next + SHELL_HISTORY_CAPACITY - g_shell_history_offset) %
        SHELL_HISTORY_CAPACITY;
    editor_load_text(editor, g_shell_history[history_index]);
}

static void editor_scroll_up(struct shell_line_editor *editor) {
    editor_hide_cursor(editor);
    if (welovec_print_scrollback(1) > 0) {
        editor->scroll_view = 1;
    } else {
        editor_show_cursor(editor);
    }
}

static void editor_return_live(struct shell_line_editor *editor) {
    if (!editor->scroll_view) return;
    (void)welovec_print_scrollback(0);
    editor->scroll_view = 0;
    editor_render(editor);
    editor_show_cursor(editor);
}

static int editor_has_modifier(const char *parameters, const char *modifier) {
    uint32_t index = 0;
    while (parameters[index] && modifier[index] &&
           parameters[index] == modifier[index]) ++index;
    return !parameters[index] && !modifier[index];
}

static void editor_handle_escape(struct shell_line_editor *editor,
                                const char *parameters, char final) {
    const int control = editor_has_modifier(parameters, "1;5") ||
                        editor_has_modifier(parameters, "5");
    if (final == 'A') {
        if (control) {
            editor_return_live(editor);
            editor_history_move(editor, -1);
        } else {
            editor_scroll_up(editor);
        }
    } else if (final == 'B') {
        if (control) {
            editor_return_live(editor);
            editor_history_move(editor, 1);
        } else {
            editor_return_live(editor);
        }
    } else {
        editor_return_live(editor);
        if (final == 'C') {
            if (editor->cursor < editor->length) ++editor->cursor;
            editor_move_cursor(editor);
        } else if (final == 'D') {
            if (editor->cursor) --editor->cursor;
            editor_move_cursor(editor);
        } else if (final == 'H' || editor_has_modifier(parameters, "1")) {
            editor->cursor = 0;
            editor_move_cursor(editor);
        } else if (final == 'F' || editor_has_modifier(parameters, "4")) {
            editor->cursor = editor->length;
            editor_move_cursor(editor);
        } else if (final == '~' && editor_has_modifier(parameters, "3")) {
            editor_delete(editor);
        } else if (final == '~' && editor_has_modifier(parameters, "5")) {
            editor_scroll_up(editor);
        }
    }
}

static void editor_commit(struct shell_line_editor *editor) {
    editor_return_live(editor);
    editor_hide_cursor(editor);
    editor->text[editor->length] = '\0';
    editor_history_add(editor->text);
    (void)welovec_print_cursor_set(editor->caret.x, editor->caret.y);
    shell_print("\n", COLOR_WHITE);
}

static void editor_begin(struct shell_line_editor *editor,
                         const struct shell_state *state) {
    editor->length = 0;
    editor->cursor = 0;
    editor->rendered_length = 0;
    editor->text[0] = '\0';
    editor->cursor_visible = 0;
    editor->scroll_view = 0;
    shell_print_prompt(state);
    editor_capture_origin(editor);
    if (!editor->maximum_length) {
        shell_print("\n", COLOR_WHITE);
        shell_print_prompt(state);
        editor_capture_origin(editor);
    }
    editor_render(editor);
    editor_show_cursor(editor);
}

static void shell_main(void) {
    struct shell_state state;
    initialize_shell_state(&state);
    (void)clear_screen();
    (void)welovec_print_reset_cursor();
    shell_print("WeLoveC shell\n", COLOR_GREEN);

    struct shell_line_editor editor;
    editor_begin(&editor, &state);
    uint32_t escape_state = 0;
    char escape_parameters[8];
    uint32_t escape_length = 0;
    for (;;) {
        char input[32];
        const int64_t count = welovec_input_poll(input, sizeof(input));
        if (count < 0) {
            (void)welovec_sleep(1);
            continue;
        }
        if (!count) {
            (void)welovec_sleep(1);
            continue;
        }

        for (int64_t index = 0; index < count; ++index) {
            const unsigned char character = (unsigned char)input[index];
            if (escape_state == 1) {
                if (character == '[') {
                    escape_state = 2;
                    escape_length = 0;
                } else {
                    escape_state = 0;
                }
                continue;
            }
            if (escape_state == 2) {
                if (character >= 0x40 && character <= 0x7e) {
                    escape_parameters[escape_length] = '\0';
                    editor_handle_escape(&editor, escape_parameters,
                                         (char)character);
                    escape_state = 0;
                } else if (escape_length + 1 < sizeof(escape_parameters)) {
                    escape_parameters[escape_length++] = (char)character;
                } else {
                    escape_state = 0;
                }
                continue;
            }
            if (character == 0x1b) {
                escape_state = 1;
                continue;
            }

            editor_return_live(&editor);
            if (character == '\r' || character == '\n') {
                editor_commit(&editor);
                char arguments[SHELL_ARGUMENT_CAPACITY][SHELL_ARGUMENT_LENGTH];
                uint32_t argument_count = 0;
                if (shell_parse_line(editor.text, arguments, &argument_count))
                    run_command(&state, arguments, argument_count);
                else
                    shell_print("shell: invalid quoting or too many arguments\n", COLOR_RED);
                g_shell_history_offset = 0;
                editor_begin(&editor, &state);
            } else if (character == '\b' || character == 0x7f) {
                editor_backspace(&editor);
            } else if (character >= 0x20 && character <= 0x7e) {
                editor_insert(&editor, (char)character);
            }
            if (!editor.scroll_view) editor_show_cursor(&editor);
        }
    }
}

void userland_main(uint64_t argc, char **argv) {
    (void)argc;
    (void)argv;
    shell_main();
}
