// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the user-space init process, which checks the system and starts the session.

#include <stdint.h>
#include "user/filesystem.h"
#include "user/input.h"
#include "user/security.h"
#include "user/stdio.h"
#include "user/exec_conf.h"

#define LOGIN_COLOR_WHITE UINT32_C(0xffffffff)
#define LOGIN_COLOR_GREEN UINT32_C(0xff70e080)
#define LOGIN_COLOR_RED UINT32_C(0xffff6666)
#define LOGIN_COLOR_YELLOW UINT32_C(0xffffd166)

static void write_text(const char *text) {
    uint64_t length = 0;
    while (text[length]) ++length;
    (void)welovec_write(text, length);
}

static void write_unsigned(uint64_t value) {
    char digits[21];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));
    while (count) (void)welovec_write(&digits[--count], 1);
}

static void write_signed(int64_t value) {
    if (value < 0) {
        write_text("-");
        write_unsigned(UINT64_C(0) - (uint64_t)value);
    } else {
        write_unsigned((uint64_t)value);
    }
}

static void clear_secret(char *secret, uint32_t capacity) {
    volatile char *cursor = secret;
    while (capacity--) *cursor++ = '\0';
}

/* Returns 1 for a complete line, 0 for overflow, and -1 for input failure. */
static int read_login_field(const char *prompt, char *output, uint32_t capacity,
                            int masked) {
    uint32_t length = 0;
    uint32_t discarded = 0;
    if (!output || capacity < 2) return -1;
    output[0] = '\0';
    (void)print_color(prompt, LOGIN_COLOR_YELLOW, WELOVEC_FONT_SYSTEM8);

    for (;;) {
        char input[32];
        const int64_t count = welovec_input_read(input, sizeof(input));
        if (count < 0) return -1;
        if (count == 0) continue;
        int complete = 0;
        for (int64_t index = 0; index < count; ++index) {
            const unsigned char character = (unsigned char)input[index];
            if (character == '\r' || character == '\n') {
                complete = 1;
                break;
            }
            if (character == '\b' || character == 0x7f) {
                if (discarded) {
                    --discarded;
                } else if (length) {
                    output[--length] = '\0';
                    (void)print_color("\b", LOGIN_COLOR_WHITE,
                                      WELOVEC_FONT_SYSTEM8);
                }
                continue;
            }
            if (character < 0x20 || character > 0x7e) continue;
            if (length + 1 < capacity) {
                output[length++] = (char)character;
                output[length] = '\0';
                char visible[2] = {(char)(masked ? '*' : character), '\0'};
                (void)print_color(visible, LOGIN_COLOR_WHITE,
                                  WELOVEC_FONT_SYSTEM8);
            } else if (discarded != UINT32_MAX) {
                ++discarded;
            }
        }
        if (complete) {
            (void)print_color("\n", LOGIN_COLOR_WHITE, WELOVEC_FONT_SYSTEM8);
            return discarded ? 0 : 1;
        }
    }
}

static int authenticate_for_shell(void) {
    struct welovec_user_status status;
    if (welovec_get_user_status(&status) == 0 && status.authenticated) return 1;

    char username[WELOVEC_USER_NAME_MAX];
    char password[WELOVEC_USER_PASSWORD_MAX + 1];
    (void)clear_screen();
    (void)welovec_print_reset_cursor();
    (void)print_color("WeLoveC login\n", LOGIN_COLOR_GREEN,
                      WELOVEC_FONT_SYSTEM16);

    for (;;) {
        clear_secret(username, sizeof(username));
        clear_secret(password, sizeof(password));
        const int username_result = read_login_field(
            "Username: ", username, sizeof(username), 0);
        if (username_result < 0) {
            (void)print_color("Login unavailable: keyboard input failed.\n",
                              LOGIN_COLOR_RED, WELOVEC_FONT_SYSTEM8);
            return 0;
        }
        if (!username_result || !username[0]) {
            (void)print_color("Username is too long or empty. Try again.\n",
                              LOGIN_COLOR_RED, WELOVEC_FONT_SYSTEM8);
            continue;
        }

        const int password_result = read_login_field(
            "Password: ", password, sizeof(password), 1);
        if (password_result < 0) {
            clear_secret(username, sizeof(username));
            clear_secret(password, sizeof(password));
            (void)print_color("Login unavailable: keyboard input failed.\n",
                              LOGIN_COLOR_RED, WELOVEC_FONT_SYSTEM8);
            return 0;
        }
        if (!password_result || !password[0]) {
            clear_secret(username, sizeof(username));
            clear_secret(password, sizeof(password));
            (void)print_color("Password is too long or empty. Try again.\n",
                              LOGIN_COLOR_RED, WELOVEC_FONT_SYSTEM8);
            continue;
        }

        (void)print_color("Verifying credentials; please wait...\n",
                          LOGIN_COLOR_YELLOW, WELOVEC_FONT_SYSTEM8);
        const int64_t result = welovec_login(username, password);
        clear_secret(username, sizeof(username));
        clear_secret(password, sizeof(password));
        if (result == 0) return 1;
        (void)print_color("Login failed. Check your username and password.\n",
                          LOGIN_COLOR_RED, WELOVEC_FONT_SYSTEM8);
    }
}

void userland_main(uint64_t argc, char **argv) {
    (void)argc;
    (void)argv;

    static const char sample[] = "filesystem syscalls are working\n";
    char readback[sizeof(sample)];
    const int64_t bytes_written = welovec_fs_write(
        "/tmp/userland.txt", sample, sizeof(sample) - 1);
    const int64_t bytes_read = welovec_fs_read(
        "/tmp/userland.txt", readback, sizeof(readback));
    if (bytes_written == (int64_t)(sizeof(sample) - 1) &&
        bytes_read == bytes_written) {
        /* VFS startup probe passed; keep the framebuffer for the shell. */
    } else {
        write_text("[userland] VFS syscall failed: write=");
        write_signed(bytes_written);
        write_text(" read=");
        write_signed(bytes_read);
        write_text("\n");
    }

    if (!authenticate_for_shell()) {
        write_text("[userland] no authenticated session; shell was not started\n");
        welovec_exit(1);
        return;
    }

    static const char program_config[] =
        "/userspace/bin/shell/shell.conf";
    for (;;) {
        const int64_t config_exists = welovec_fs_exists(program_config);
        if (config_exists == 1) {
            const int64_t child_pid = exec_conf(program_config);
            if (child_pid >= 0) {
                int64_t exit_status = 0;
                const int64_t waited = welovec_waitpid(
                    (uint64_t)child_pid, &exit_status, 0);
                if (waited < 0) {
                    write_text("[userland] waitpid failed, error=");
                    write_signed(waited);
                    write_text("\n");
                    welovec_exit(1);
                    return;
                }
                if (exit_status != 0) {
                    write_text("[userland] shell exited with status=");
                    write_signed(exit_status);
                    write_text("; check the kernel serial fault log\n");
                    welovec_exit(1);
                    return;
                }
            } else {
                write_text("[userland] exec_conf failed, error=");
                write_signed(child_pid);
                write_text("\n");
            }
        } else if (config_exists < 0) {
            write_text("[userland] config lookup failed, error=");
            write_signed(config_exists);
            write_text("\n");
        } else {
            write_text("[userland] no shell program config found\n");
            welovec_exit(0);
        }
        (void)welovec_sleep(100);
    }
}
