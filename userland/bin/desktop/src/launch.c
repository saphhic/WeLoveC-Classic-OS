// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Finds and launches programs from the desktop.

#include "desktop/include/desktop.h"
#include "user/filesystem.h"
#include "user/input.h"
#include "user/power.h"
#include "user/security.h"
#include "user/stdio.h"
#include "user/syscall.h"

int desk_run(const char *path, uint32_t argc, const char *const argv[],
             int pause_after) {
    (void)pause_after;
    char launch[512];
    if (!path || !path[0] || d_strlen(path) >= sizeof(launch)) return -1;
    d_strcpy(launch, sizeof(launch), path);
    for (uint32_t i = 1; argv && i < argc && i < 7; ++i) {
        const uint32_t used = d_strlen(launch);
        const uint32_t length = d_strlen(argv[i]);
        if (used + length + 2 > sizeof(launch)) return -1;
        launch[used] = '\n';
        launch[used + 1] = '\0';
        d_strcat(launch, sizeof(launch), argv[i]);
    }
    return wm_open(&app_program, launch) ? 0 : -1;
}

/* Name -> path: absolute paths as-is, otherwise look in the known bin dirs. */
static int resolve(const char *name, char *out, uint32_t cap) {
    if (name[0] == '/') {
        d_strcpy(out, cap, name);
        return welovec_fs_exists(out) == 1;
    }
    char base[64];
    d_strcpy(base, sizeof(base), name);
    if (d_ends_with(base, ".elf")) base[d_strlen(base) - 4] = '\0';
    d_strcpy(out, cap, "/userspace/bin/shell/bin/");
    d_strcat(out, cap, base);
    d_strcat(out, cap, ".elf");
    if (welovec_fs_exists(out) == 1) return 1;
    d_strcpy(out, cap, "/userspace/bin/");
    d_strcat(out, cap, base);
    d_strcat(out, cap, "/");
    d_strcat(out, cap, base);
    d_strcat(out, cap, ".elf");
    return welovec_fs_exists(out) == 1;
}

void desk_run_line(const char *line) {
    char buffer[128];
    char args[6][64];
    const char *argv[6];
    uint32_t argc = 0;
    d_strcpy(buffer, sizeof(buffer), line);
    uint32_t i = 0;
    while (buffer[i] && argc < 6) {
        while (buffer[i] == ' ') ++i;
        if (!buffer[i]) break;
        uint32_t n = 0;
        while (buffer[i] && buffer[i] != ' ') {
            if (n + 1 < sizeof(args[0])) args[argc][n++] = buffer[i];
            ++i;
        }
        args[argc][n] = '\0';
        ++argc;
    }
    if (!argc) return;
    char path[128];
    if (!resolve(args[0], path, sizeof(path))) {
        dlg_message("Program not found. Use a name or an absolute .elf path.");
        return;
    }
    d_strcpy(args[0], sizeof(args[0]), path);
    for (uint32_t k = 0; k < argc; ++k) argv[k] = args[k];
    (void)desk_run(path, argc, argv, 1);
}

void desk_power(int reboot) {
    const uint32_t vm = welovec_running_in_virtual_machine();
    if (reboot) (void)welovec_system_reboot(vm);
    else (void)welovec_system_shutdown(vm);
}
