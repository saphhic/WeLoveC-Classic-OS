// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Sends ICMP requests to check IPv4 connectivity.

#include "cmd_common.h"
#include "user/network.h"

void userland_main(uint64_t argc, char **argv) {
    if (argc != 2) {
        cmd_print("usage: ping <IPv4-or-hostname>\n", CMD_YELLOW);
        return;
    }
    uint8_t address[4];
    int64_t result;
    if (!cmd_valid_ipv4(argv[1], address)) {
        cmd_print("Resolving ", CMD_WHITE);
        cmd_print(argv[1], CMD_YELLOW);
        cmd_print("...\n", CMD_WHITE);
        result = welovec_dns_lookup_ipv4(argv[1], address);
        if (result < 0) {
            cmd_print_error("ping: DNS lookup", result);
            return;
        }
    }
    cmd_print("PING ", CMD_GREEN);
    cmd_print(argv[1], CMD_WHITE);
    cmd_print("\n", CMD_WHITE);
    result = welovec_ping_ipv4(address);
    if (result < 0) cmd_print_error("ping", result);
    else cmd_print("reply received\n", CMD_GREEN);
}
