// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Finds the startup configuration and launches the first user program.

#include "startupd.h"
#include "../arch/x86_64/io/serial.h"

#include "../arch/x86_64/exec/elf_loader.h"
#include "../filesystem/vfs.h"

#define STARTUP_CONFIG_MAX_SIZE 4096u

/* fs_list has no iterator, so keep its bounded result outside the boot stack. */
static VfsListEntry startup_entries[MAX_LIST_ENTRIES];

static char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static int text_equal(const char *left, const char *right) {
    uint32_t i = 0;
    if (!left || !right) return 0;
    while (left[i] && right[i] && ascii_lower(left[i]) == ascii_lower(right[i])) ++i;
    return left[i] == '\0' && right[i] == '\0';
}

static int ends_with_conf(const char *name) {
    uint32_t length = 0;
    if (!name) return 0;
    while (name[length]) ++length;
    return length >= 5 && name[length - 5] == '.' &&
           ascii_lower(name[length - 4]) == 'c' &&
           ascii_lower(name[length - 3]) == 'o' &&
           ascii_lower(name[length - 2]) == 'n' &&
           ascii_lower(name[length - 1]) == 'f';
}

static int text_less(const char *left, const char *right) {
    uint32_t i = 0;
    while (left[i] && right[i]) {
        const char a = ascii_lower(left[i]);
        const char b = ascii_lower(right[i]);
        if (a != b) return a < b;
        ++i;
    }
    return left[i] == '\0' && right[i] != '\0';
}

static void copy_trimmed(char *out, uint32_t capacity, const char *input) {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t length = 0;
    if (!out || !capacity) return;
    out[0] = '\0';
    if (!input) return;
    while (input[start] == ' ' || input[start] == '\t') ++start;
    while (input[start + end]) ++end;
    while (end && (input[start + end - 1] == ' ' || input[start + end - 1] == '\t')) --end;
    while (length < end && length + 1 < capacity) {
        out[length] = input[start + length];
        ++length;
    }
    out[length] = '\0';
}

static int join_config_path(const char *directory, const char *name,
                            char *out, uint32_t capacity) {
    uint32_t position = 0;
    if (!directory || !name || !out || !capacity) return 0;
    while (directory[position] && position + 1 < capacity) {
        out[position] = directory[position];
        ++position;
    }
    if (directory[position] || position + 1 >= capacity) return 0;
    if (position && out[position - 1] != '/') out[position++] = '/';
    for (uint32_t i = 0; name[i]; ++i) {
        if (position + 1 >= capacity) return 0;
        out[position++] = name[i];
    }
    out[position] = '\0';
    return 1;
}

static int find_config_in_directory(const char *directory, char *out_path) {
    int selected = -1;
    char candidate[MAX_FILENAME];
    const int count = fs_list(directory, startup_entries, MAX_LIST_ENTRIES);
    for (int i = 0; i < count; ++i) {
        if (startup_entries[i].is_directory || !ends_with_conf(startup_entries[i].name)) continue;
        if (selected < 0 || text_less(startup_entries[i].name, startup_entries[selected].name))
            selected = i;
    }
    if (selected < 0 || !join_config_path(directory, startup_entries[selected].name,
                                           candidate, sizeof(candidate))) return 0;
    vfs_copy_text(out_path, MAX_FILENAME, candidate);
    return 1;
}

static int find_startup_config(char *out_path) {
    static const char *const directories[] = {"/bin", "/userspace/bin"};
    char standard_path[MAX_FILENAME];

    /* A named startup.conf always takes precedence over other config files. */
    for (uint32_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        if (join_config_path(directories[i], "startup.conf", standard_path,
                             sizeof(standard_path)) &&
            file_exists(standard_path) && !fs_is_directory(standard_path)) {
            vfs_copy_text(out_path, MAX_FILENAME, standard_path);
            return 1;
        }
    }
    for (uint32_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i)
        if (find_config_in_directory(directories[i], out_path)) return 1;
    return 0;
}

static int parse_config(const uint8_t *data, uint32_t size, char *out_binary) {
    char line[MAX_FILENAME];
    uint32_t line_length = 0;
    int found_binary = 0;

    if (!data || !out_binary || size > STARTUP_CONFIG_MAX_SIZE) return 0;
    out_binary[0] = '\0';

    for (uint32_t i = 0; i <= size; ++i) {
        const char value = i == size ? '\n' : (char)data[i];
        if (value == '\0') return 0;
        if (value != '\n' && value != '\r') {
            if (line_length + 1 >= sizeof(line)) return 0;
            line[line_length++] = value;
            continue;
        }

        line[line_length] = '\0';
        line_length = 0;
        char *comment = line;
        while (*comment && *comment != '#') ++comment;
        *comment = '\0';

        char cleaned[MAX_FILENAME];
        copy_trimmed(cleaned, sizeof(cleaned), line);
        if (!cleaned[0]) continue;

        char *equals = cleaned;
        while (*equals && *equals != '=') ++equals;
        if (!*equals) continue; /* Ignore unrelated settings in a shared .conf. */
        *equals = '\0';

        char key[MAX_FILENAME];
        char binary[MAX_FILENAME];
        copy_trimmed(key, sizeof(key), cleaned);
        copy_trimmed(binary, sizeof(binary), equals + 1);
        if (!text_equal(key, "exec") && !text_equal(key, "binary")) continue;
        if (found_binary || !binary[0] || binary[0] != '/') return 0;
        vfs_copy_text(out_binary, MAX_FILENAME, binary);
        if (!text_equal(out_binary, binary)) return 0;
        found_binary = 1;
    }
    return found_binary;
}

static int load_config_binary(const char *config_path, char *out_binary) {
    uint8_t *data = 0;
    uint32_t size = 0;
    if (!vfs_read_file(config_path, &data, &size) || !data) return 0;
    const int valid = size <= STARTUP_CONFIG_MAX_SIZE && parse_config(data, size, out_binary);
    vfs_free_file(data);
    return valid;
}

static int launch_binary(const char *path) {
    const struct welovec_exec_options options = {0, 0, 1, 0, 0};
    struct welovec_exec_result result = {0, 0, 0};
    const enum welovec_exec_error error = welovec_exec_launch_elf64(path, &options, &result);
    if (error != WELOVEC_EXEC_SUCCESS) {
        serial_write("[startupd] could not launch ");
        serial_write(path);
        serial_write(": ");
        serial_write(welovec_exec_error_string(error));
        serial_write("\n");
        return 0;
    }

    serial_write("[startupd] launched as guest: ");
    serial_write(path);
    serial_write("\n");
    return 1;
}

int welovec_startupd_run(void) {
    char config_path[MAX_FILENAME];
    char binary_path[MAX_FILENAME];
    static const char *const default_binaries[] = {
        "/bin/init.elf",
        "/userspace/bin/init.elf"
    };

    if (find_startup_config(config_path)) {
        serial_write("[startupd] reading ");
        serial_write(config_path);
        serial_write("\n");
        if (load_config_binary(config_path, binary_path)) {
            if (launch_binary(binary_path)) return 1;
        } else {
            serial_write("[startupd] invalid config; expected exec=/absolute/path/to/program.elf\n");
        }
    } else {
        serial_write("[startupd] no .conf found; trying init.elf\n");
    }

    for (uint32_t i = 0; i < sizeof(default_binaries) / sizeof(default_binaries[0]); ++i)
        if (launch_binary(default_binaries[i])) return 1;

    serial_write("[startupd] no user program started; kernel will remain idle\n");
    return 0;
}
