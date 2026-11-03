// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Defines system call numbers and wrappers for user programs.

#ifndef WELOVEC_USER_SYSCALL_H
#define WELOVEC_USER_SYSCALL_H

#include <stdint.h>

enum welovec_user_syscall_number {
    WELOVEC_SYS_EXIT = 0,
    WELOVEC_SYS_WRITE = 1,
    WELOVEC_SYS_YIELD = 2,
    WELOVEC_SYS_SLEEP = 3,
    WELOVEC_SYS_THREAD_EXIT = 4,
    WELOVEC_SYS_GETPID = 5,
    WELOVEC_SYS_THREAD_CREATE = 6,
    WELOVEC_SYS_GETUID = 7,
    WELOVEC_SYS_GETGID = 8,
    WELOVEC_SYS_GETCAPS = 9,
    WELOVEC_SYS_HAS_CAPABILITY = 10,
    WELOVEC_SYS_EXEC = 11,
    WELOVEC_SYS_EXEC_ADMIN = 12,
    WELOVEC_SYS_RESERVED_13 = 13,
    WELOVEC_SYS_RESERVED_14 = 14,
    WELOVEC_SYS_EXEC_AS_USER = 15,
    WELOVEC_SYS_FS_EXISTS = 16,
    WELOVEC_SYS_FS_STAT = 17,
    WELOVEC_SYS_FS_READ = 18,
    WELOVEC_SYS_FS_WRITE = 19,
    WELOVEC_SYS_FS_LIST = 20,
    WELOVEC_SYS_FS_MKDIR = 21,
    WELOVEC_SYS_FS_UNLINK = 22,
    WELOVEC_SYS_GETTICKS = 23,
    WELOVEC_SYS_READ = 24,
    WELOVEC_SYS_FD_WRITE = 25,
    WELOVEC_SYS_OPEN = 26,
    WELOVEC_SYS_CLOSE = 27,
    WELOVEC_SYS_SEEK = 28,
    WELOVEC_SYS_MMAP = 29,
    WELOVEC_SYS_MUNMAP = 30,
    WELOVEC_SYS_WAITPID = 31,
    WELOVEC_SYS_PROCESS_QUERY = 32,
    WELOVEC_SYS_KILL = 33,
    WELOVEC_SYS_SIGACTION = 34,
    WELOVEC_SYS_SIGRETURN = 35,
    WELOVEC_SYS_FB_INFO = 36,
    WELOVEC_SYS_FB_PIXEL = 37,
    WELOVEC_SYS_FB_RECT = 38,
    WELOVEC_SYS_FB_RECT_ALPHA = 39,
    WELOVEC_SYS_FB_GRADIENT = 40,
    WELOVEC_SYS_FB_OUTLINE = 41,
    WELOVEC_SYS_FB_LINE = 42,
    WELOVEC_SYS_FB_CIRCLE = 43,
    WELOVEC_SYS_FB_FILL_CIRCLE = 44,
    WELOVEC_SYS_FB_CHAR = 45,
    WELOVEC_SYS_FB_TEXT = 46,
    WELOVEC_SYS_NET_INFO = 47,
    WELOVEC_SYS_NET_PING = 48,
    WELOVEC_SYS_NET_DNS = 49,
    WELOVEC_SYS_NET_TCP_CONNECT = 50,
    WELOVEC_SYS_NET_TCP_SEND = 51,
    WELOVEC_SYS_NET_TCP_RECV = 52,
    WELOVEC_SYS_NET_TCP_CLOSE = 53,
    WELOVEC_SYS_FB_SCROLL_UP = 54,
    WELOVEC_SYS_LOGIN = 55,
    WELOVEC_SYS_LOGOUT = 56,
    WELOVEC_SYS_USER_STATUS = 57,
    WELOVEC_SYS_USER_CREATE = 58,
    WELOVEC_SYS_USER_LIST = 59,
    WELOVEC_SYS_PRINT = 60,
    WELOVEC_SYS_PRINT_COLOR = 61,
    WELOVEC_SYS_PRINT_RESET_CURSOR = 62,
    WELOVEC_SYS_RTC_UTC = 63,
    WELOVEC_SYS_SYSTEM_REBOOT = 64,
    WELOVEC_SYS_SYSTEM_SHUTDOWN = 65,
    WELOVEC_SYS_FS_MOVE = 66,
    WELOVEC_SYS_SECURE_RANDOM = 67,
    WELOVEC_SYS_NET_DNS_IPV6 = 68,
    WELOVEC_SYS_NET_TCP_CONNECT_IPV6 = 69,
    WELOVEC_SYS_NET_IPV6_INFO = 70,
    WELOVEC_SYS_PRINT_CURSOR_GET = 71,
    WELOVEC_SYS_PRINT_CURSOR_SET = 72,
    WELOVEC_SYS_PRINT_SCROLLBACK = 73,
    WELOVEC_SYS_INPUT_POLL = 74,
    WELOVEC_SYS_POINTER_POLL = 75,
    WELOVEC_SYS_POINTER_SOURCES = 76,
    WELOVEC_SYS_EXEC_WINDOWED = 77,
    WELOVEC_SYS_FB_SURFACE_BLIT = 78,
    WELOVEC_SYS_FB_SURFACE_RESIZE = 79,
    WELOVEC_SYS_INPUT_FORWARD = 80,
    WELOVEC_SYS_FB_SURFACE_GENERATION = 81,
    WELOVEC_SYS_TERMINATE_WINDOWED_TREE = 82,
    WELOVEC_SYS_FB_FRAME_BEGIN = 83,
    WELOVEC_SYS_FB_FRAME_END = 84,
    WELOVEC_SYS_FS_LIST_RANGE = 85
};

static inline int64_t welovec_user_syscall0(uint64_t number) {
    uint64_t result = number;
    __asm__ volatile("syscall" : "+a"(result) : : "rcx", "r11", "memory");
    return (int64_t)result;
}

static inline int64_t welovec_user_syscall1(uint64_t number, uint64_t argument1) {
    uint64_t result = number;
    __asm__ volatile("syscall" : "+a"(result) : "D"(argument1) : "rcx", "r11", "memory");
    return (int64_t)result;
}

static inline int64_t welovec_user_syscall2(uint64_t number,
                                            uint64_t argument1,
                                            uint64_t argument2) {
    uint64_t result = number;
    __asm__ volatile("syscall" : "+a"(result)
                     : "D"(argument1), "S"(argument2)
                     : "rcx", "r11", "memory");
    return (int64_t)result;
}

static inline int64_t welovec_user_syscall3(uint64_t number,
                                            uint64_t argument1,
                                            uint64_t argument2,
                                            uint64_t argument3) {
    uint64_t result = number;
    __asm__ volatile("syscall" : "+a"(result)
                     : "D"(argument1), "S"(argument2), "d"(argument3)
                     : "rcx", "r11", "memory");
    return (int64_t)result;
}

static inline int64_t welovec_user_syscall4(uint64_t number,
                                            uint64_t argument1,
                                            uint64_t argument2,
                                            uint64_t argument3,
                                            uint64_t argument4) {
    uint64_t result = number;
    register uint64_t r10 __asm__("r10") = argument4;
    __asm__ volatile("syscall" : "+a"(result)
                     : "D"(argument1), "S"(argument2), "d"(argument3), "r"(r10)
                     : "rcx", "r11", "memory");
    return (int64_t)result;
}

static inline int64_t welovec_user_syscall5(uint64_t number,
                                            uint64_t argument1,
                                            uint64_t argument2,
                                            uint64_t argument3,
                                            uint64_t argument4,
                                            uint64_t argument5) {
    uint64_t result = number;
    register uint64_t r10 __asm__("r10") = argument4;
    register uint64_t r8 __asm__("r8") = argument5;
    __asm__ volatile("syscall" : "+a"(result)
                     : "D"(argument1), "S"(argument2), "d"(argument3),
                       "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return (int64_t)result;
}

static inline int64_t welovec_user_syscall6(uint64_t number,
                                            uint64_t argument1,
                                            uint64_t argument2,
                                            uint64_t argument3,
                                            uint64_t argument4,
                                            uint64_t argument5,
                                            uint64_t argument6) {
    uint64_t result = number;
    register uint64_t r10 __asm__("r10") = argument4;
    register uint64_t r8 __asm__("r8") = argument5;
    register uint64_t r9 __asm__("r9") = argument6;
    __asm__ volatile("syscall" : "+a"(result)
                     : "D"(argument1), "S"(argument2), "d"(argument3),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return (int64_t)result;
}

static inline int64_t welovec_write(const void *data, uint64_t size) {
    return welovec_user_syscall2(WELOVEC_SYS_WRITE, (uint64_t)(uintptr_t)data, size);
}

static inline int64_t welovec_read(int fd, void *data, uint64_t size) {
    return welovec_user_syscall3(WELOVEC_SYS_READ, (uint64_t)fd,
                                 (uint64_t)(uintptr_t)data, size);
}

static inline int64_t welovec_write_fd(int fd, const void *data, uint64_t size) {
    return welovec_user_syscall3(WELOVEC_SYS_FD_WRITE, (uint64_t)fd,
                                 (uint64_t)(uintptr_t)data, size);
}

static inline int64_t welovec_open(const char *path, uint32_t flags) {
    return welovec_user_syscall2(WELOVEC_SYS_OPEN,
                                 (uint64_t)(uintptr_t)path, flags);
}

static inline int64_t welovec_close(int fd) {
    return welovec_user_syscall1(WELOVEC_SYS_CLOSE, (uint64_t)fd);
}

static inline int64_t welovec_seek(int fd, int64_t offset, uint32_t whence) {
    return welovec_user_syscall3(WELOVEC_SYS_SEEK, (uint64_t)fd,
                                 (uint64_t)offset, whence);
}

static inline void *welovec_mmap(uint64_t size) {
    int64_t result = welovec_user_syscall1(WELOVEC_SYS_MMAP, size);
    return result < 0 ? (void *)0 : (void *)(uintptr_t)result;
}

static inline int64_t welovec_munmap(void *address, uint64_t size) {
    return welovec_user_syscall2(WELOVEC_SYS_MUNMAP,
                                 (uint64_t)(uintptr_t)address, size);
}

static inline int64_t welovec_waitpid(uint64_t pid, int64_t *status, uint32_t options) {
    return welovec_user_syscall3(WELOVEC_SYS_WAITPID, pid,
                                 (uint64_t)(uintptr_t)status, options);
}

static inline int64_t welovec_kill(uint64_t pid, uint32_t signal_number) {
    return welovec_user_syscall2(WELOVEC_SYS_KILL, pid, signal_number);
}

static inline int64_t welovec_yield(void) {
    return welovec_user_syscall0(WELOVEC_SYS_YIELD);
}

static inline int64_t welovec_sleep(uint64_t ticks) {
    return welovec_user_syscall1(WELOVEC_SYS_SLEEP, ticks);
}

static inline uint64_t welovec_getpid(void) {
    return (uint64_t)welovec_user_syscall0(WELOVEC_SYS_GETPID);
}

static inline uint64_t welovec_getticks(void) {
    return (uint64_t)welovec_user_syscall0(WELOVEC_SYS_GETTICKS);
}

static inline int64_t welovec_thread_create(void (*entry)(void *), void *argument) {
    return welovec_user_syscall2(WELOVEC_SYS_THREAD_CREATE,
                                 (uint64_t)(uintptr_t)entry,
                                 (uint64_t)(uintptr_t)argument);
}

static inline __attribute__((noreturn)) void welovec_exit(int64_t status) {
    (void)welovec_user_syscall1(WELOVEC_SYS_EXIT, (uint64_t)status);
    for (;;) __asm__ volatile("pause");
}

static inline __attribute__((noreturn)) void welovec_thread_exit(int64_t status) {
    (void)welovec_user_syscall1(WELOVEC_SYS_THREAD_EXIT, (uint64_t)status);
    for (;;) __asm__ volatile("pause");
}

#endif
