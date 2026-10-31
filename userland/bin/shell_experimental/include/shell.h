// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for shell in the shell experimental module.

#ifndef WELOVEC_SHELL_H
#define WELOVEC_SHELL_H

#include <stdint.h>
#include "command_config.h"

#define SHELL_LINE_CAPACITY 512u
#define SHELL_ARGUMENT_CAPACITY 24u
#define SHELL_ARGUMENT_LENGTH 256u
#define SHELL_PATH_CAPACITY 256u

struct shell_state {
    char cwd[SHELL_PATH_CAPACITY];
    char username[48];
    uint32_t authenticated;
};

int shell_parse_line(const char *line,
                     char arguments[SHELL_ARGUMENT_CAPACITY][SHELL_ARGUMENT_LENGTH],
                     uint32_t *out_count);
int shell_resolve_path(const char *cwd, const char *path,
                       char output[SHELL_PATH_CAPACITY]);
void shell_print_prompt(const struct shell_state *state);
void shell_print(const char *text, uint32_t color);
void shell_print_unsigned(uint64_t value, uint32_t color);

#endif
