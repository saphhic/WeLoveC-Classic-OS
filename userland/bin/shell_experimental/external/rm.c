// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the rm component in the shell experimental module.

#include "cmd_common.h"

void userland_main(uint64_t argc, char **argv) {
    const int directory_only = argc > 0 && cmd_equal(argv[0], "rmdir");
    if (argc < 2) {
        cmd_print(directory_only ? "usage: rmdir <directory> [directory ...]\n"
                                 : "usage: rm <path> [path ...]\n",
                  CMD_YELLOW);
        return;
    }
    for (uint64_t index = 1; index < argc; ++index) {
        if (directory_only) {
            struct welovec_user_file_info info;
            const int64_t stat = welovec_fs_stat(argv[index], &info);
            if (stat < 0 || info.type != WELOVEC_USER_FS_DIRECTORY) {
                cmd_print("rmdir: not a directory or access denied\n", CMD_RED);
                continue;
            }
        }
        const int64_t result = welovec_fs_unlink(argv[index]);
        if (result < 0) cmd_print_error("rm", result);
        else {
            cmd_print("removed ", CMD_GREEN);
            cmd_print(argv[index], CMD_WHITE);
            cmd_print("\n", CMD_WHITE);
        }
    }
}
