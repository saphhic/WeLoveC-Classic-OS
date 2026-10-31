// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the ls component in the shell experimental module.

#include "cmd_common.h"
#include "../include/command_config.h"

void userland_main(uint64_t argc, char **argv) {
    if (argc < 2 || argc > 3) {
        cmd_print("usage: ls [path]\n", CMD_YELLOW);
        return;
    }
    struct welovec_user_dir_entry entries[WELOVEC_USER_FS_MAX_ENTRIES];
    char resolved[SHELL_COMMAND_PATH_CAPACITY];
    const char *path = argv[1];
    if (argc == 3) {
        if (!shell_cfg_resolve_path(argv[1], argv[2], resolved)) {
            cmd_print("ls: invalid or overlong path\n", CMD_RED);
            return;
        }
        path = resolved;
    }
    const int64_t count = welovec_fs_list(path, entries,
                                          WELOVEC_USER_FS_MAX_ENTRIES);
    if (count < 0) {
        cmd_print_error("ls", count);
        return;
    }
    for (int64_t index = 0; index < count; ++index) {
        cmd_print(entries[index].type == WELOVEC_USER_FS_DIRECTORY ? "d " : "- ",
                  entries[index].type == WELOVEC_USER_FS_DIRECTORY ? CMD_GREEN : CMD_WHITE);
        cmd_print(entries[index].name, CMD_WHITE);
        if (entries[index].type == WELOVEC_USER_FS_FILE) {
            cmd_print("  ", CMD_WHITE);
            cmd_print_number(entries[index].size, CMD_YELLOW);
            cmd_print(" B", CMD_WHITE);
        }
        cmd_print("\n", CMD_WHITE);
    }
}
