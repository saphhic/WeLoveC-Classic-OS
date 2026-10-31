// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Requests administrator credentials and runs an authorized command.

#include "cmd_common.h"
#include "../include/command_config.h"
#include "user/input.h"
#include "user/process.h"
#include "user/security.h"

#define ARUN_ARG_CAPACITY SHELL_COMMAND_NAME_CAPACITY
#define ARUN_ARG_LENGTH SHELL_COMMAND_PATH_CAPACITY

static int resolve_user_path(const char *cwd, const char *requested,
                             char output[SHELL_COMMAND_PATH_CAPACITY]) {
    if (!requested || !requested[0]) return 0;
    if (requested[0] != '~' ||
        (requested[1] && requested[1] != '/'))
        return shell_cfg_resolve_path(cwd, requested, output);

    static const char home[] = "/userland/tmp";
    char expanded[SHELL_COMMAND_PATH_CAPACITY];
    uint32_t length = 0;
    while (home[length] && length + 1 < sizeof(expanded)) {
        expanded[length] = home[length];
        ++length;
    }
    const char *suffix = requested[1] == '/' ? requested + 2 : requested + 1;
    if (suffix[0]) {
        if (length + 1 >= sizeof(expanded)) return 0;
        expanded[length++] = '/';
        while (*suffix && length + 1 < sizeof(expanded))
            expanded[length++] = *suffix++;
        if (*suffix) return 0;
    }
    expanded[length] = '\0';
    return shell_cfg_resolve_path("/", expanded, output);
}

static void clear_secret(char *secret, uint32_t capacity) {
    volatile char *bytes = secret;
    while (capacity--) *bytes++ = '\0';
}

static void copy_string(char *destination, uint32_t capacity,
                        const char *source) {
    if (!capacity) return;
    uint32_t index = 0;
    while (source[index] && index + 1 < capacity) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static void print_signed_status(int64_t value) {
    if (value < 0) {
        cmd_print("-", CMD_RED);
        cmd_print_number(UINT64_C(0) - (uint64_t)value, CMD_RED);
    } else {
        cmd_print_number((uint64_t)value, CMD_RED);
    }
}

static int has_elf_suffix(const char *path) {
    return cmd_has_extension(path, ".elf");
}

static int is_installed_elf(const char *path) {
    struct welovec_user_file_info info;
    return has_elf_suffix(path) && welovec_fs_stat(path, &info) == 0 &&
           info.type == WELOVEC_USER_FS_FILE;
}

static int copy_path_argument(char *destination, uint32_t capacity,
                              const char *source) {
    uint32_t length = cmd_length(source);
    if (length >= 2 && source[0] == '(' && source[length - 1] == ')') {
        --length;
        if (length <= 1 || length - 1 >= capacity) return 0;
        for (uint32_t index = 1; index < length; ++index)
            destination[index - 1] = source[index];
        destination[length - 1] = '\0';
        return 1;
    }
    if (length >= capacity) return 0;
    copy_string(destination, capacity, source);
    return 1;
}

static int find_admin_name(char name[WELOVEC_USER_NAME_MAX]) {
    struct welovec_user_status current;
    if (welovec_get_user_status(&current) == 0 && current.authenticated &&
        (current.capabilities & WELOVEC_CAP_EXEC_AS_ADMIN)) {
        copy_string(name, WELOVEC_USER_NAME_MAX, current.name);
        return 1;
    }
    const int64_t count = welovec_list_users(0, 0);
    if (count <= 0 || count > WELOVEC_USER_MAX_ACCOUNTS) return 0;
    struct welovec_user_record users[WELOVEC_USER_MAX_ACCOUNTS];
    const int64_t listed = welovec_list_users(users, WELOVEC_USER_MAX_ACCOUNTS);
    if (listed < 0) return 0;
    for (int64_t index = 0; index < listed; ++index) {
        if (users[index].capabilities & WELOVEC_CAP_EXEC_AS_ADMIN) {
            copy_string(name, WELOVEC_USER_NAME_MAX, users[index].name);
            return 1;
        }
    }
    return 0;
}

static int read_password(char password[WELOVEC_USER_PASSWORD_MAX + 1]) {
    uint32_t length = 0;
    password[0] = '\0';
    cmd_print("Administrator password: ", CMD_YELLOW);
    for (;;) {
        char input[32];
        const int64_t count = welovec_input_read(input, sizeof(input));
        if (count <= 0) continue;
        int done = 0;
        for (int64_t index = 0; index < count; ++index) {
            const unsigned char character = (unsigned char)input[index];
            if (character == '\r' || character == '\n') {
                cmd_print("\n", CMD_WHITE);
                done = 1;
                break;
            }
            if ((character == '\b' || character == 0x7f) && length) {
                password[--length] = '\0';
                cmd_print("\b", CMD_WHITE);
            } else if (character >= 0x20 && character <= 0x7e &&
                       length < WELOVEC_USER_PASSWORD_MAX) {
                password[length++] = (char)character;
                password[length] = '\0';
                cmd_print("*", CMD_WHITE);
            }
        }
        if (done) return length != 0;
    }
}

static int resolve_target(const char *target, const char *cwd,
                          char executable[SHELL_COMMAND_PATH_CAPACITY],
                          struct shell_command_config *config,
                          int *has_config) {
    *has_config = 0;
    if (has_elf_suffix(target)) {
        if (!resolve_user_path(cwd, target, executable)) return 0;
        return executable[0] == '/' && is_installed_elf(executable);
    }
    if (!shell_cfg_find(target, config) || !config->executable[0]) return 0;
    copy_string(executable, SHELL_COMMAND_PATH_CAPACITY, config->executable);
    if (!is_installed_elf(executable)) return 0;
    *has_config = 1;
    return 1;
}

static int normalize_arguments(
    const char *cwd, const struct shell_command_config *config,
    int has_config, char arguments[ARUN_ARG_CAPACITY][ARUN_ARG_LENGTH],
    uint32_t count) {
    if (!has_config) return 1;
    const uint32_t first_parameter = cmd_equal(config->action, "cwd-first") ? 2u : 1u;
    if (count < first_parameter) return 1;
    const uint32_t parameter_count = count - first_parameter;
    const int install_all_mode = cmd_equal(config->name, "install") &&
                                 parameter_count >= 2 &&
                                 cmd_equal(arguments[first_parameter], "-all");
    const uint32_t last_optional_minimum = install_all_mode ? 3u : 2u;
    for (uint32_t index = first_parameter; index < count; ++index) {
        const int all = cmd_equal(config->path_args, "all");
        const int first = cmd_equal(config->path_args, "first") &&
                          index == first_parameter;
        const int last = cmd_equal(config->path_args, "last") && index == count - 1;
        const int last_optional = cmd_equal(config->path_args, "last2") &&
                                  parameter_count >= last_optional_minimum &&
                                  index == count - 1;
        if (!(all || first || last || last_optional)) continue;
        char path[ARUN_ARG_LENGTH];
        if (!resolve_user_path(cwd, arguments[index], path)) return 0;
        copy_string(arguments[index], ARUN_ARG_LENGTH, path);
    }
    return 1;
}

void userland_main(uint64_t argc, char **argv) {
    if (argc < 3) {
        cmd_print("usage: arun <command> [arguments ...] | arun exec <path.elf> [arguments ...]\n",
                  CMD_YELLOW);
        return;
    }
    const char *cwd = argv[1];
    char target_storage[SHELL_COMMAND_PATH_CAPACITY];
    if (!copy_path_argument(target_storage, sizeof(target_storage), argv[2])) {
        cmd_print("arun: invalid command or ELF path\n", CMD_RED);
        return;
    }
    const char *target = target_storage;
    if (cmd_equal(target, "cd")) {
        cmd_print("arun: cd changes the current shell and cannot run as a child.\n",
                  CMD_RED);
        cmd_print("Run cd <path> first, then use arun with a relative path.\n",
                  CMD_YELLOW);
        return;
    }
    struct shell_command_config config;
    char executable[SHELL_COMMAND_PATH_CAPACITY];
    int has_config = 0;
    const int exec_path_target = cmd_equal(target, "exec");
    if (exec_path_target) {
        char path_argument[SHELL_COMMAND_PATH_CAPACITY];
        if (argc < 4) {
            cmd_print("usage: arun exec <path.elf> [arguments ...]\n", CMD_YELLOW);
            return;
        }
        if (!copy_path_argument(path_argument, sizeof(path_argument), argv[3]) ||
            !resolve_user_path(cwd, path_argument, executable)) {
            cmd_print("arun exec: invalid or overlong path\n", CMD_RED);
            return;
        }
        if (!has_elf_suffix(executable)) {
            cmd_print("arun exec: target must have a .elf suffix: ", CMD_RED);
            cmd_print(executable, CMD_RED);
            cmd_print("\n", CMD_RED);
            return;
        }
        if (!is_installed_elf(executable)) {
            cmd_print("arun exec: ELF file does not exist: ", CMD_RED);
            cmd_print(executable, CMD_RED);
            cmd_print("\n", CMD_RED);
            return;
        }
    } else if (!resolve_target(target, cwd, executable, &config, &has_config)) {
        cmd_print("arun: target is not an installed command or valid .elf path: ",
                  CMD_RED);
        cmd_print(target, CMD_RED);
        cmd_print("\nUse 'arun <command>' or 'arun exec <path.elf>'.\n", CMD_YELLOW);
        return;
    }

    const int target_needs_cwd = has_config &&
                                 cmd_equal(config.action, "cwd-first");
    const uint32_t child_count =
        (uint32_t)(argc - (exec_path_target ? 3 : 2)) +
        (target_needs_cwd ? 1u : 0u);
    if (!child_count || child_count > ARUN_ARG_CAPACITY) {
        cmd_print("arun: too many command arguments\n", CMD_RED);
        return;
    }

    char admin_name[WELOVEC_USER_NAME_MAX];
    if (!find_admin_name(admin_name)) {
        cmd_print("arun: no administrator account is available\n", CMD_RED);
        return;
    }
    char password[WELOVEC_USER_PASSWORD_MAX + 1];
    if (!read_password(password)) {
        clear_secret(password, sizeof(password));
        cmd_print("arun: password is required\n", CMD_RED);
        return;
    }
    const int64_t login = welovec_login(admin_name, password);
    clear_secret(password, sizeof(password));
    if (login < 0) {
        clear_secret(admin_name, sizeof(admin_name));
        cmd_print("arun: administrator authentication failed\n", CMD_RED);
        return;
    }

    char child_storage[ARUN_ARG_CAPACITY][ARUN_ARG_LENGTH];
    const char *child_arguments[ARUN_ARG_CAPACITY];
    if (exec_path_target) {
        copy_string(child_storage[0], sizeof(child_storage[0]), executable);
        child_arguments[0] = child_storage[0];
        for (uint32_t index = 1; index < child_count; ++index) {
            copy_string(child_storage[index], sizeof(child_storage[index]),
                        argv[index + 3]);
            child_arguments[index] = child_storage[index];
        }
    } else {
        copy_string(child_storage[0], sizeof(child_storage[0]),
                    has_config ? config.name : target);
        child_arguments[0] = child_storage[0];
        uint32_t destination = 1;
        if (target_needs_cwd) {
            copy_string(child_storage[destination], sizeof(child_storage[destination]), cwd);
            child_arguments[destination] = child_storage[destination];
            ++destination;
        }
        for (uint32_t source = 3; source < argc; ++source) {
            copy_string(child_storage[destination], sizeof(child_storage[destination]),
                        argv[source]);
            child_arguments[destination] = child_storage[destination];
            ++destination;
        }
    }
    if (!normalize_arguments(cwd, &config, has_config && !exec_path_target,
                             child_storage, child_count)) {
        (void)welovec_logout();
        clear_secret(admin_name, sizeof(admin_name));
        cmd_print("arun: invalid or overlong path argument\n", CMD_RED);
        return;
    }

    const int64_t child = welovec_exec_admin(
        executable, child_count, child_arguments, WELOVEC_PRIORITY_NORMAL);
    int64_t child_status = 0;
    int64_t result = child;
    if (child >= 0) result = welovec_waitpid((uint64_t)child, &child_status, 0);
    (void)welovec_logout();
    clear_secret(admin_name, sizeof(admin_name));
    if (result < 0) cmd_print_error("arun", result);
    else if (child_status != 0) {
        cmd_print("administrator command exited with status ", CMD_RED);
        print_signed_status(child_status);
        cmd_print("\n", CMD_RED);
    } else cmd_print("administrator command finished\n", CMD_GREEN);
}
