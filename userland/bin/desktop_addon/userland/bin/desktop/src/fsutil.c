// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Provides file operations shared by desktop applications.

#include "desktop/include/desktop.h"
#include "user/filesystem.h"
#include "user/syscall.h"

/*
 * FS_LIST fills `struct welovec_user_dir_entry`, which carries a 1 KiB name, so
 * a full 32-entry listing is ~33 KB. That is too much for the 64 KiB stack and
 * wasteful as permanent .bss, so the scratch buffer is mapped only for the
 * duration of the call and released immediately.
 *
 * The kernel call has no offset argument: a directory with more than 32
 * entries can only be listed up to its first 32.
 */
int fsu_list(const char *path, struct dir_item *out, uint32_t cap,
             uint32_t *count) {
    *count = 0;
    if (cap > WELOVEC_USER_FS_MAX_ENTRIES) cap = WELOVEC_USER_FS_MAX_ENTRIES;
    const uint64_t bytes =
        (uint64_t)WELOVEC_USER_FS_MAX_ENTRIES * sizeof(struct welovec_user_dir_entry);
    struct welovec_user_dir_entry *scratch = welovec_mmap(bytes);
    if (!scratch) return -12;

    const int64_t listed =
        welovec_fs_list(path, scratch, WELOVEC_USER_FS_MAX_ENTRIES);
    int result = 0;
    if (listed < 0) {
        result = (int)listed;
    } else {
        uint32_t used = 0;
        for (int64_t i = 0; i < listed && used < cap; ++i) {
            const char *name = scratch[i].name;
            if (d_strlen(name) >= sizeof(out[0].name)) continue; /* too long to show */
            d_strcpy(out[used].name, sizeof(out[used].name), name);
            out[used].size = (uint32_t)scratch[i].size;
            out[used].type = (uint8_t)scratch[i].type;
            ++used;
        }
        *count = used;
    }
    (void)welovec_munmap(scratch, bytes);
    return result;
}

/* Streams a file in 1 KiB blocks so big files never need a big buffer. */
int64_t fsu_copy_file(const char *source, const char *destination) {
    if (d_streq(source, destination)) return -22;
    if (welovec_fs_exists(destination) == 1) return -17;
    const int64_t input = welovec_open(source, WELOVEC_OPEN_READ);
    if (input < 0) return input;
    const int64_t output = welovec_open(
        destination,
        WELOVEC_OPEN_WRITE | WELOVEC_OPEN_CREATE | WELOVEC_OPEN_TRUNCATE);
    if (output < 0) {
        (void)welovec_close((int)input);
        return output;
    }
    char block[1024];
    int64_t result = 0;
    for (;;) {
        const int64_t got = welovec_read((int)input, block, sizeof(block));
        if (got < 0) {
            result = got;
            break;
        }
        if (!got) break;
        const int64_t put = welovec_write_fd((int)output, block, (uint64_t)got);
        if (put != got) {
            result = put < 0 ? put : -5;
            break;
        }
    }
    (void)welovec_close((int)input);
    (void)welovec_close((int)output);
    if (result < 0) (void)welovec_fs_unlink(destination);
    return result;
}
