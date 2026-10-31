// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Displays the real-time clock and system uptime.

#include "cmd_common.h"
#include "user/time.h"

static void print_two_digits(uint32_t value) {
    char output[3] = {
        (char)('0' + (value / 10) % 10), (char)('0' + value % 10), '\0'
    };
    cmd_print(output, CMD_WHITE);
}

void userland_main(uint64_t argc, char **argv) {
    (void)argv;
    if (argc != 1) {
        cmd_print("usage: time\n", CMD_YELLOW);
        return;
    }
    struct welovec_user_datetime now;
    const int64_t result = welovec_rtc_get_utc(&now);
    if (result < 0) {
        cmd_print_error("time: UTC clock", result);
    } else {
        cmd_print("UTC ", CMD_GREEN);
        cmd_print_number((uint64_t)now.year, CMD_WHITE);
        cmd_print("-", CMD_WHITE);
        print_two_digits((uint32_t)now.month);
        cmd_print("-", CMD_WHITE);
        print_two_digits((uint32_t)now.day);
        cmd_print(" ", CMD_WHITE);
        print_two_digits((uint32_t)now.hour);
        cmd_print(":", CMD_WHITE);
        print_two_digits((uint32_t)now.minute);
        cmd_print(":", CMD_WHITE);
        print_two_digits((uint32_t)now.second);
        cmd_print("\n", CMD_WHITE);
    }
    const uint64_t uptime_seconds = welovec_getticks() / 100;
    cmd_print("Uptime: ", CMD_GREEN);
    cmd_print_number(uptime_seconds / 86400, CMD_WHITE);
    cmd_print("d ", CMD_WHITE);
    cmd_print_number((uptime_seconds / 3600) % 24, CMD_WHITE);
    cmd_print("h ", CMD_WHITE);
    cmd_print_number((uptime_seconds / 60) % 60, CMD_WHITE);
    cmd_print("m ", CMD_WHITE);
    cmd_print_number(uptime_seconds % 60, CMD_WHITE);
    cmd_print("s\n", CMD_WHITE);
}
