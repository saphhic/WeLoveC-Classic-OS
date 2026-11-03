// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares file and directory operations available to user programs.

#ifndef WELOVEC_USER_FILESYSTEM_H
#define WELOVEC_USER_FILESYSTEM_H

#include <stdint.h>
#include "user/syscall.h"

#define WELOVEC_USER_PATH_MAX 1024u
#define WELOVEC_USER_FS_MAX_ENTRIES 32u
#define WELOVEC_STDIN_FILENO 0
#define WELOVEC_STDOUT_FILENO 1
#define WELOVEC_STDERR_FILENO 2

enum welovec_user_fs_type {
    WELOVEC_USER_FS_FILE = 1,
    WELOVEC_USER_FS_DIRECTORY = 2
};

enum welovec_user_open_flags {
    WELOVEC_OPEN_READ = 1u,
    WELOVEC_OPEN_WRITE = 2u,
    WELOVEC_OPEN_CREATE = 4u,
    WELOVEC_OPEN_TRUNCATE = 8u,
    WELOVEC_OPEN_APPEND = 16u
};

enum welovec_user_seek_whence {
    WELOVEC_SEEK_SET = 0,
    WELOVEC_SEEK_CUR = 1,
    WELOVEC_SEEK_END = 2
};

struct welovec_user_file_info {
    uint64_t size;
    uint32_t type;
    uint32_t reserved;
};

struct welovec_user_dir_entry {
    char name[WELOVEC_USER_PATH_MAX];
    uint64_t size;
    uint32_t type;
    uint32_t reserved;
};

static inline int64_t welovec_fs_exists(const char *path) {
    return welovec_user_syscall1(WELOVEC_SYS_FS_EXISTS, (uint64_t)(uintptr_t)path);
}

static inline int64_t welovec_fs_stat(const char *path,
                                      struct welovec_user_file_info *out_info) {
    return welovec_user_syscall2(WELOVEC_SYS_FS_STAT,
                                 (uint64_t)(uintptr_t)path,
                                 (uint64_t)(uintptr_t)out_info);
}

/* Reads an entire file. Returns its byte count, or a negative errno value. */
static inline int64_t welovec_fs_read(const char *path, void *buffer, uint64_t capacity) {
    return welovec_user_syscall3(WELOVEC_SYS_FS_READ,
                                 (uint64_t)(uintptr_t)path,
                                 (uint64_t)(uintptr_t)buffer, capacity);
}

/* Replaces a file. All users may write under shared, temporary /tmp. */
static inline int64_t welovec_fs_write(const char *path,
                                       const void *buffer, uint64_t size) {
    return welovec_user_syscall3(WELOVEC_SYS_FS_WRITE,
                                 (uint64_t)(uintptr_t)path,
                                 (uint64_t)(uintptr_t)buffer, size);
}

/* Lists up to capacity entries; capacity is limited to 32. */
static inline int64_t welovec_fs_list(const char *path,
                                      struct welovec_user_dir_entry *entries,
                                      uint64_t capacity) {
    return welovec_user_syscall3(WELOVEC_SYS_FS_LIST,
                                 (uint64_t)(uintptr_t)path,
                                 (uint64_t)(uintptr_t)entries, capacity);
}

/* Lists one page of a directory, starting after `offset` entries. */
static inline int64_t welovec_fs_list_range(
        const char *path, struct welovec_user_dir_entry *entries,
        uint64_t capacity, uint64_t offset) {
    return welovec_user_syscall4(WELOVEC_SYS_FS_LIST_RANGE,
                                 (uint64_t)(uintptr_t)path,
                                 (uint64_t)(uintptr_t)entries, capacity, offset);
}

static inline int64_t welovec_fs_mkdir(const char *path) {
    return welovec_user_syscall1(WELOVEC_SYS_FS_MKDIR, (uint64_t)(uintptr_t)path);
}

static inline int64_t welovec_fs_unlink(const char *path) {
    return welovec_user_syscall1(WELOVEC_SYS_FS_UNLINK, (uint64_t)(uintptr_t)path);
}

static inline int64_t welovec_fs_move(const char *source, const char *destination) {
    return welovec_user_syscall2(WELOVEC_SYS_FS_MOVE,
                                 (uint64_t)(uintptr_t)source,
                                 (uint64_t)(uintptr_t)destination);
}

#endif
