// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Resolves host names through DNS.

#include "cmd_common.h"
#include "user/network.h"

void userland_main(uint64_t argc, char **argv) {
    if (argc != 2) {
        cmd_print("usage: dns <hostname>\n", CMD_YELLOW);
        return;
    }
    cmd_print("Querying DNS for ", CMD_WHITE);
    cmd_print(argv[1], CMD_YELLOW);
    cmd_print("...\n", CMD_WHITE);
    int found = 0;
    uint8_t address4[4];
    const int64_t result4 = welovec_dns_lookup_ipv4(argv[1], address4);
    if (result4 == 0) {
        found = 1;
        cmd_print(argv[1], CMD_GREEN);
        cmd_print(" A -> ", CMD_WHITE);
        for (uint32_t index = 0; index < 4; ++index) {
            if (index) cmd_print(".", CMD_WHITE);
            cmd_print_number(address4[index], CMD_YELLOW);
        }
        cmd_print("\n", CMD_WHITE);
    }
    uint8_t address6[16];
    const int64_t result6 = welovec_dns_lookup_ipv6(argv[1], address6);
    if (result6 == 0) {
        found = 1;
        cmd_print(argv[1], CMD_GREEN);
        cmd_print(" AAAA -> ", CMD_WHITE);
        for (uint32_t index = 0; index < 16; index += 2) {
            if (index) cmd_print(":", CMD_WHITE);
            cmd_print_hex_byte(address6[index], CMD_YELLOW);
            cmd_print_hex_byte(address6[index + 1], CMD_YELLOW);
        }
        cmd_print("\n", CMD_WHITE);
    }
    if (!found) cmd_print_error("dns", result4 < 0 ? result4 : result6);
}
