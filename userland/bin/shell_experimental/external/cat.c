// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the cat component in the shell experimental module.

#include "cmd_common.h"

void userland_main(uint64_t argc, char **argv) {
    if (argc < 2) {
        cmd_print("usage: cat <file> [file ...]\n", CMD_YELLOW);
        return;
    }
    char buffer[513];
    for (uint64_t file = 1; file < argc; ++file) {
        const int64_t descriptor = welovec_open(argv[file], WELOVEC_OPEN_READ);
        if (descriptor < 0) {
            cmd_print_error("cat", descriptor);
            continue;
        }
        for (;;) {
            const int64_t count = welovec_read((int)descriptor, buffer, sizeof(buffer) - 1);
            if (count < 0) {
                cmd_print_error("cat", count);
                break;
            }
            if (!count) break;
            uint32_t printable = (uint32_t)count;
            for (uint32_t index = 0; index < printable; ++index) {
                if (buffer[index] == '\0') printable = index;
            }
            buffer[printable] = '\0';
            cmd_print(buffer, CMD_WHITE);
        }
        (void)welovec_close((int)descriptor);
    }
}
