// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the stat component in the shell experimental module.

#include "cmd_common.h"

void userland_main(uint64_t argc, char **argv) {
    if (argc < 2) {
        cmd_print("usage: stat <path> [path ...]\n", CMD_YELLOW);
        return;
    }
    for (uint64_t index = 1; index < argc; ++index) {
        struct welovec_user_file_info info;
        const int64_t result = welovec_fs_stat(argv[index], &info);
        if (result < 0) {
            cmd_print_error("stat", result);
            continue;
        }
        cmd_print(info.type == WELOVEC_USER_FS_DIRECTORY ? "directory  " : "file       ",
                  CMD_GREEN);
        cmd_print(argv[index], CMD_WHITE);
        cmd_print("  size=", CMD_WHITE);
        cmd_print_number(info.size, CMD_YELLOW);
        cmd_print(" bytes\n", CMD_WHITE);
    }
}
