// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for elf loader in the exec module.

#ifndef WELOVEC_X86_64_EXEC_ELF_LOADER_H
#define WELOVEC_X86_64_EXEC_ELF_LOADER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum welovec_exec_error {
    WELOVEC_EXEC_SUCCESS = 0,
    WELOVEC_EXEC_INVALID_ARGUMENT,
    WELOVEC_EXEC_FILE_NOT_FOUND,
    WELOVEC_EXEC_NOT_REGULAR_FILE,
    WELOVEC_EXEC_FILE_TOO_LARGE,
    WELOVEC_EXEC_READ_FAILED,
    WELOVEC_EXEC_INVALID_ELF,
    WELOVEC_EXEC_UNSUPPORTED_ELF,
    WELOVEC_EXEC_INVALID_SEGMENT,
    WELOVEC_EXEC_OVERLAPPING_SEGMENTS,
    WELOVEC_EXEC_OUT_OF_MEMORY,
    WELOVEC_EXEC_PROCESS_FAILED,
    WELOVEC_EXEC_THREAD_FAILED
};

struct welovec_exec_options {
    uint32_t argument_count;
    const char *const *arguments;
    uint8_t priority; /* 0 high, 1 normal, 2 background, 3 low */
    uint32_t framebuffer_width;  /* both dimensions zero for a normal process */
    uint32_t framebuffer_height;
};

struct welovec_exec_result {
    uint64_t process_id;
    uint64_t thread_id;
    uint64_t entry_point;
};

/* Loads a static ELF64 x86-64 ET_EXEC image from the VFS and starts it.
   With no arguments, argv[0] defaults to path. */
enum welovec_exec_error welovec_exec_launch_elf64(
    const char *path, const struct welovec_exec_options *options,
    struct welovec_exec_result *out_result);

/* Kernel-only launch paths. User programs must use include/user/security.h. */
enum welovec_exec_error welovec_exec_launch_elf64_as_current(
    const char *path, const struct welovec_exec_options *options,
    struct welovec_exec_result *out_result);
enum welovec_exec_error welovec_exec_launch_elf64_as_admin(
    const char *path, const struct welovec_exec_options *options,
    struct welovec_exec_result *out_result);
enum welovec_exec_error welovec_exec_launch_elf64_as_uid(
    const char *path, const struct welovec_exec_options *options,
    struct welovec_exec_result *out_result, uint32_t uid);

const char *welovec_exec_error_string(enum welovec_exec_error error);

#ifdef __cplusplus
}
#endif

#endif
