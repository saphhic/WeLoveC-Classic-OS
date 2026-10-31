// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Parses commands and arguments for the experimental shell.

#include "../include/shell.h"

static int copy_argument_char(char *destination, uint32_t *length, char value) {
    if (*length + 1 >= SHELL_ARGUMENT_LENGTH) return 0;
    destination[(*length)++] = value;
    destination[*length] = '\0';
    return 1;
}

int shell_parse_line(const char *line,
                     char arguments[SHELL_ARGUMENT_CAPACITY][SHELL_ARGUMENT_LENGTH],
                     uint32_t *out_count) {
    if (!line || !out_count) return 0;
    *out_count = 0;
    uint32_t cursor = 0;
    char quote = '\0';
    int escaped = 0;
    int token_open = 0;

    while (line[cursor]) {
        const char value = line[cursor++];
        if (escaped) {
            if (!token_open) {
                if (*out_count >= SHELL_ARGUMENT_CAPACITY) return 0;
                arguments[*out_count][0] = '\0';
                token_open = 1;
            }
            uint32_t length = 0;
            while (arguments[*out_count][length]) ++length;
            if (!copy_argument_char(arguments[*out_count], &length, value)) return 0;
            escaped = 0;
            continue;
        }
        if (value == '\\' && quote != '\'') {
            escaped = 1;
            continue;
        }
        if (quote) {
            if (value == quote) {
                quote = '\0';
                continue;
            }
        } else if (value == '\'' || value == '"') {
            if (!token_open) {
                if (*out_count >= SHELL_ARGUMENT_CAPACITY) return 0;
                arguments[*out_count][0] = '\0';
                token_open = 1;
            }
            quote = value;
            continue;
        } else if (value == ' ' || value == '\t') {
            if (token_open) {
                ++*out_count;
                token_open = 0;
            }
            continue;
        }

        if (!token_open) {
            if (*out_count >= SHELL_ARGUMENT_CAPACITY) return 0;
            arguments[*out_count][0] = '\0';
            token_open = 1;
        }
        uint32_t length = 0;
        while (arguments[*out_count][length]) ++length;
        if (!copy_argument_char(arguments[*out_count], &length, value)) return 0;
    }
    if (escaped || quote) return 0;
    if (token_open) ++*out_count;
    return 1;
}

int shell_resolve_path(const char *cwd, const char *path,
                       char output[SHELL_PATH_CAPACITY]) {
    return shell_cfg_resolve_path(cwd, path, output);
}
