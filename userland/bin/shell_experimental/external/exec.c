// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the exec component in the shell experimental module.

#include "cmd_common.h"
#include "../include/command_config.h"
#include "user/security.h"

#define EXEC_ARGUMENT_CAPACITY 24u

static int has_elf_suffix(const char *path) {
    return cmd_has_extension(path, ".elf");
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

static int copy_path_argument(char *destination, uint32_t capacity,
                              const char *source) {
    const uint32_t length = cmd_length(source);
    if (length >= 2 && source[0] == '(' && source[length - 1] == ')') {
        if (length - 2 >= capacity) return 0;
        for (uint32_t index = 0; index < length - 2; ++index)
            destination[index] = source[index + 1];
        destination[length - 2] = '\0';
        return length > 2;
    }
    if (length >= capacity) return 0;
    copy_text(destination, capacity, source);
    return 1;
}

void userland_main(uint64_t argc, char **argv) {
    if (argc < 3) {
        cmd_print("usage: exec <path.elf> [arguments ...]\n", CMD_YELLOW);
        return;
    }
    if (argc - 2 > EXEC_ARGUMENT_CAPACITY) {
        cmd_print("exec: too many arguments\n", CMD_RED);
        return;
    }

    char path_argument[SHELL_COMMAND_PATH_CAPACITY];
    char executable[SHELL_COMMAND_PATH_CAPACITY];
    if (!copy_path_argument(path_argument, sizeof(path_argument), argv[2]) ||
        !shell_cfg_resolve_path(argv[1], path_argument, executable) ||
        !has_elf_suffix(executable)) {
        cmd_print("exec: invalid ELF path\n", CMD_RED);
        return;
    }

    const uint32_t child_count = (uint32_t)(argc - 2);
    char first_argument[SHELL_COMMAND_PATH_CAPACITY];
    copy_text(first_argument, sizeof(first_argument), executable);
    const char *child_arguments[EXEC_ARGUMENT_CAPACITY];
    child_arguments[0] = first_argument;
    for (uint32_t index = 1; index < child_count; ++index)
        child_arguments[index] = argv[index + 2];

    const int64_t child = welovec_exec(executable, child_count, child_arguments,
                                       WELOVEC_PRIORITY_NORMAL);
    if (child < 0) {
        cmd_print_error("exec", child);
        return;
    }
    int64_t status = 0;
    const int64_t waited = welovec_waitpid((uint64_t)child, &status, 0);
    if (waited < 0)
        cmd_print("exec: could not wait for the program\n", CMD_RED);
    else if (status != 0) {
        cmd_print("exec: program exited with status ", CMD_RED);
        if (status < 0) {
            cmd_print("-", CMD_RED);
            cmd_print_number(UINT64_C(0) - (uint64_t)status, CMD_RED);
        } else {
            cmd_print_number((uint64_t)status, CMD_RED);
        }
        cmd_print("\n", CMD_RED);
    }
}
