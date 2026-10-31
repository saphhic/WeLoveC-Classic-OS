// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Reads a simple configuration file and launches the specified executable.

#include "user/exec_conf.h"

#include "user/filesystem.h"
#include "user/security.h"

#define EXEC_CONF_MAX_SIZE 4096u
#define EXEC_CONF_PATH_CAPACITY 256u

static char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static int key_is_exec(const char *key, uint32_t length) {
    static const char exec_key[] = "exec";
    static const char binary_key[] = "binary";
    const char *candidate = 0;
    uint32_t candidate_length = 0;

    if (length == sizeof(exec_key) - 1) {
        candidate = exec_key;
        candidate_length = sizeof(exec_key) - 1;
    } else if (length == sizeof(binary_key) - 1) {
        candidate = binary_key;
        candidate_length = sizeof(binary_key) - 1;
    } else {
        return 0;
    }

    for (uint32_t i = 0; i < candidate_length; ++i)
        if (ascii_lower(key[i]) != candidate[i]) return 0;
    return 1;
}

static void trim_bounds(const char *text, uint32_t *start, uint32_t *end) {
    while (*start < *end && (text[*start] == ' ' || text[*start] == '\t'))
        ++*start;
    while (*end > *start && (text[*end - 1] == ' ' || text[*end - 1] == '\t'))
        --*end;
}

static int parse_config(const char *data, uint32_t size,
                        char binary[EXEC_CONF_PATH_CAPACITY]) {
    uint32_t line_start = 0;
    int found_binary = 0;
    binary[0] = '\0';

    for (uint32_t i = 0; i <= size; ++i) {
        if (i < size && data[i] == '\0') return 0;
        if (i != size && data[i] != '\n' && data[i] != '\r') continue;

        uint32_t line_end = i;
        for (uint32_t comment = line_start; comment < line_end; ++comment) {
            if (data[comment] == '#') {
                line_end = comment;
                break;
            }
        }

        uint32_t key_start = line_start;
        uint32_t key_end = line_end;
        trim_bounds(data, &key_start, &key_end);
        if (key_start < key_end) {
            uint32_t equals = key_start;
            while (equals < key_end && data[equals] != '=') ++equals;
            if (equals < key_end) {
                uint32_t name_start = key_start;
                uint32_t name_end = equals;
                uint32_t value_start = equals + 1;
                uint32_t value_end = key_end;
                trim_bounds(data, &name_start, &name_end);
                trim_bounds(data, &value_start, &value_end);

                if (key_is_exec(data + name_start, name_end - name_start)) {
                    const uint32_t path_length = value_end - value_start;
                    if (found_binary || !path_length || path_length >= EXEC_CONF_PATH_CAPACITY ||
                        data[value_start] != '/')
                        return 0;
                    for (uint32_t path_index = 0; path_index < path_length; ++path_index)
                        binary[path_index] = data[value_start + path_index];
                    binary[path_length] = '\0';
                    found_binary = 1;
                }
            }
        }

        line_start = i + 1;
        if (i < size && data[i] == '\r' && i + 1 < size && data[i + 1] == '\n') {
            ++i;
            line_start = i + 1;
        }
    }
    return found_binary;
}

int64_t welovec_exec_conf(const char *config_path) {
    if (!config_path) return -22;

    char config[EXEC_CONF_MAX_SIZE + 1];
    const int64_t size = welovec_fs_read(config_path, config, sizeof(config));
    if (size < 0) return size;
    if ((uint64_t)size > EXEC_CONF_MAX_SIZE) return -7;
    config[size] = '\0';

    char binary[EXEC_CONF_PATH_CAPACITY];
    if (!parse_config(config, (uint32_t)size, binary)) return -22;

    return welovec_exec(binary, 0, 0, WELOVEC_PRIORITY_NORMAL);
}

int64_t exec_conf(const char *config_path) {
    return welovec_exec_conf(config_path);
}
