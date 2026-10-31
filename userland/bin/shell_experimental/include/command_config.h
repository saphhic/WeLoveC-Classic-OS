// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Parses and describes command configuration for the experimental shell.

#ifndef WELOVEC_SHELL_COMMAND_CONFIG_H
#define WELOVEC_SHELL_COMMAND_CONFIG_H

#include <stdint.h>
#include "user/filesystem.h"

#define SHELL_COMMAND_DIR "/userspace/bin/shell/commands"
#define SHELL_COMMAND_NAME_CAPACITY 48u
#define SHELL_COMMAND_DESCRIPTION_CAPACITY 192u
#define SHELL_COMMAND_PATH_CAPACITY 256u
#define SHELL_COMMAND_ACTION_CAPACITY 24u
#define SHELL_COMMAND_PATH_ARGS_CAPACITY 16u
#define SHELL_CONFIG_CAPACITY 2048u

struct shell_command_config {
    char name[SHELL_COMMAND_NAME_CAPACITY];
    char description[SHELL_COMMAND_DESCRIPTION_CAPACITY];
    char executable[SHELL_COMMAND_PATH_CAPACITY];
    char action[SHELL_COMMAND_ACTION_CAPACITY];
    char path_args[SHELL_COMMAND_PATH_ARGS_CAPACITY];
};

static inline uint32_t shell_cfg_length(const char *text) {
    uint32_t length = 0;
    while (text && text[length]) ++length;
    return length;
}

static inline int shell_cfg_equal_n(const char *left, uint32_t left_length,
                                    const char *right) {
    uint32_t index = 0;
    while (right[index] && index < left_length && left[index] == right[index]) ++index;
    return index == left_length && right[index] == '\0';
}

static inline void shell_cfg_copy(char *destination, uint32_t capacity,
                                  const char *source, uint32_t length) {
    if (!capacity) return;
    if (length >= capacity) length = capacity - 1;
    for (uint32_t index = 0; index < length; ++index)
        destination[index] = source[index];
    destination[length] = '\0';
}

static inline int shell_cfg_name_valid(const char *name) {
    if (!name || !name[0]) return 0;
    for (uint32_t index = 0; name[index]; ++index) {
        const char c = name[index];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
        if (index + 1 >= SHELL_COMMAND_NAME_CAPACITY) return 0;
    }
    return 1;
}

static inline int shell_cfg_parse(const char *data, uint32_t size,
                                  struct shell_command_config *config) {
    if (!data || !config || size >= SHELL_CONFIG_CAPACITY) return 0;
    config->name[0] = '\0';
    config->description[0] = '\0';
    config->executable[0] = '\0';
    config->action[0] = '\0';
    config->path_args[0] = '\0';

    uint32_t start = 0;
    for (uint32_t cursor = 0; cursor <= size; ++cursor) {
        if (cursor < size && data[cursor] == '\0') return 0;
        if (cursor < size && data[cursor] != '\n' && data[cursor] != '\r') continue;
        uint32_t end = cursor;
        while (start < end && (data[start] == ' ' || data[start] == '\t')) ++start;
        while (end > start && (data[end - 1] == ' ' || data[end - 1] == '\t')) --end;
        if (start < end && data[start] != '#') {
            uint32_t equals = start;
            while (equals < end && data[equals] != '=') ++equals;
            if (equals < end) {
                uint32_t key_end = equals;
                while (key_end > start && (data[key_end - 1] == ' ' ||
                                            data[key_end - 1] == '\t')) --key_end;
                uint32_t value_start = equals + 1;
                while (value_start < end && (data[value_start] == ' ' ||
                                              data[value_start] == '\t')) ++value_start;
                uint32_t value_end = end;
                while (value_end > value_start &&
                       (data[value_end - 1] == ' ' || data[value_end - 1] == '\t'))
                    --value_end;
                const uint32_t value_length = value_end - value_start;
                if (shell_cfg_equal_n(data + start, key_end - start, "name"))
                    shell_cfg_copy(config->name, sizeof(config->name), data + value_start, value_length);
                else if (shell_cfg_equal_n(data + start, key_end - start, "description"))
                    shell_cfg_copy(config->description, sizeof(config->description), data + value_start, value_length);
                else if (shell_cfg_equal_n(data + start, key_end - start, "exec"))
                    shell_cfg_copy(config->executable, sizeof(config->executable), data + value_start, value_length);
                else if (shell_cfg_equal_n(data + start, key_end - start, "action"))
                    shell_cfg_copy(config->action, sizeof(config->action), data + value_start, value_length);
                else if (shell_cfg_equal_n(data + start, key_end - start, "path_args"))
                    shell_cfg_copy(config->path_args, sizeof(config->path_args), data + value_start, value_length);
            }
        }
        start = cursor + 1;
        if (cursor < size && data[cursor] == '\r' && cursor + 1 < size &&
            data[cursor + 1] == '\n') {
            ++cursor;
            start = cursor + 1;
        }
    }
    return shell_cfg_name_valid(config->name) &&
           (config->executable[0] || config->action[0]);
}

static inline int shell_cfg_load_path(const char *path,
                                      struct shell_command_config *config) {
    char contents[SHELL_CONFIG_CAPACITY];
    const int64_t size = welovec_fs_read(path, contents, sizeof(contents));
    return size >= 0 && (uint64_t)size < sizeof(contents) &&
           shell_cfg_parse(contents, (uint32_t)size, config);
}

static inline int shell_cfg_find(const char *name,
                                 struct shell_command_config *config) {
    if (!shell_cfg_name_valid(name)) return 0;
    char path[sizeof(SHELL_COMMAND_DIR) + SHELL_COMMAND_NAME_CAPACITY + 8];
    uint32_t cursor = 0;
    while (SHELL_COMMAND_DIR[cursor]) {
        path[cursor] = SHELL_COMMAND_DIR[cursor];
        ++cursor;
    }
    path[cursor++] = '/';
    for (uint32_t index = 0; name[index]; ++index) path[cursor++] = name[index];
    path[cursor++] = '.';
    path[cursor++] = 'c';
    path[cursor++] = 'o';
    path[cursor++] = 'n';
    path[cursor++] = 'f';
    path[cursor] = '\0';
    return shell_cfg_load_path(path, config) && shell_cfg_equal_n(
        config->name, shell_cfg_length(config->name), name);
}

static inline int shell_cfg_resolve_path(const char *cwd, const char *path,
                                         char output[SHELL_COMMAND_PATH_CAPACITY]) {
    if (!cwd || !cwd[0] || !path || !output) return 0;
    char combined[SHELL_COMMAND_PATH_CAPACITY * 2];
    uint32_t used = 0;
    if (path[0] == '/') {
        while (path[used] && used + 1 < sizeof(combined)) {
            combined[used] = path[used];
            ++used;
        }
        if (path[used]) return 0;
    } else {
        uint32_t cwd_length = 0;
        while (cwd[cwd_length]) ++cwd_length;
        if (cwd_length + 1 >= sizeof(combined)) return 0;
        for (uint32_t index = 0; index < cwd_length; ++index)
            combined[used++] = cwd[index];
        if (used > 1 || combined[0] != '/') combined[used++] = '/';
        for (uint32_t index = 0; path[index]; ++index) {
            if (used + 1 >= sizeof(combined)) return 0;
            combined[used++] = path[index];
        }
    }
    combined[used] = '\0';

    uint32_t output_length = 1;
    uint32_t segment_starts[SHELL_COMMAND_PATH_CAPACITY];
    uint32_t segment_count = 0;
    output[0] = '/';
    output[1] = '\0';
    for (uint32_t cursor = 0; cursor <= used;) {
        while (cursor < used && combined[cursor] == '/') ++cursor;
        const uint32_t start = cursor;
        while (cursor < used && combined[cursor] != '/') ++cursor;
        const uint32_t length = cursor - start;
        if (!length) break;
        if (length == 1 && combined[start] == '.') continue;
        if (length == 2 && combined[start] == '.' && combined[start + 1] == '.') {
            if (segment_count) {
                output_length = segment_starts[--segment_count];
                output[output_length] = '\0';
            }
            continue;
        }
        const uint32_t extra = length + (output_length > 1 ? 1u : 0u);
        if (output_length + extra >= SHELL_COMMAND_PATH_CAPACITY ||
            segment_count >= SHELL_COMMAND_PATH_CAPACITY) return 0;
        segment_starts[segment_count++] = output_length;
        if (output_length > 1) output[output_length++] = '/';
        for (uint32_t index = 0; index < length; ++index)
            output[output_length++] = combined[start + index];
        output[output_length] = '\0';
    }
    return 1;
}

#endif
