// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the mkdir component in the shell experimental module.

#include "cmd_common.h"

void userland_main(uint64_t argc, char **argv) {
    if (argc < 2) {
        cmd_print("usage: mkdir <directory> [directory ...]\n", CMD_YELLOW);
        return;
    }
    for (uint64_t index = 1; index < argc; ++index) {
        const int64_t result = welovec_fs_mkdir(argv[index]);
        if (result < 0) cmd_print_error("mkdir", result);
        else {
            cmd_print("created ", CMD_GREEN);
            cmd_print(argv[index], CMD_WHITE);
            cmd_print("\n", CMD_WHITE);
        }
    }
}
