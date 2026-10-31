// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the cp component in the shell experimental module.

#include "cmd_common.h"

void userland_main(uint64_t argc, char **argv) {
    if (argc != 3) {
        cmd_print("usage: cp <source> <destination>\n", CMD_YELLOW);
        return;
    }
    const int64_t result = cmd_copy_file(argv[1], argv[2]);
    if (result < 0) cmd_print_error("cp", result);
    else cmd_print("file copied\n", CMD_GREEN);
}
