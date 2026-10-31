// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the write component in the shell experimental module.

#include "cmd_common.h"

void userland_main(uint64_t argc, char **argv) {
    if (argc < 3) {
        cmd_print("usage: write <file> <text ...>\n", CMD_YELLOW);
        return;
    }
    char content[4096];
    uint32_t used = 0;
    for (uint64_t argument = 2; argument < argc; ++argument) {
        if (argument > 2 && used + 1 < sizeof(content)) content[used++] = ' ';
        for (uint32_t index = 0; argv[argument][index]; ++index) {
            if (used + 1 >= sizeof(content)) {
                cmd_print("write: text exceeds 4095 bytes\n", CMD_RED);
                return;
            }
            content[used++] = argv[argument][index];
        }
    }
    const int64_t descriptor = welovec_open(
        argv[1], WELOVEC_OPEN_WRITE | WELOVEC_OPEN_CREATE | WELOVEC_OPEN_TRUNCATE);
    if (descriptor < 0) {
        cmd_print_error("write", descriptor);
        return;
    }
    const int64_t result = welovec_write_fd((int)descriptor, content, used);
    (void)welovec_close((int)descriptor);
    if (result < 0) cmd_print_error("write", result);
    else cmd_print("file written\n", CMD_GREEN);
}
