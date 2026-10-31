// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the mv component in the shell experimental module.

#include "cmd_common.h"

void userland_main(uint64_t argc, char **argv) {
    if (argc != 3) {
        cmd_print("usage: mv <source> <destination>\n", CMD_YELLOW);
        return;
    }
    const int64_t result = welovec_fs_move(argv[1], argv[2]);
    if (result < 0) cmd_print_error("mv", result);
    else cmd_print("path moved\n", CMD_GREEN);
}
