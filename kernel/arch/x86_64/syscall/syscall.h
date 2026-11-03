// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Defines system call numbers and types used by the x86-64 kernel.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

//   The real (SYSCALL/SYSRET) mechanism, not an int 0x80 gate. Field
//   order in syscall_regs_t MUST match the push order in syscall_entry.asm
//   exactly — same contract as registers_t/isr.asm.

#ifndef ARCH_X86_64_SYSCALL_H
#define ARCH_X86_64_SYSCALL_H

#include <stdint.h>

struct __attribute__((packed)) syscall_regs_t {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t user_rsp;
};

#define SYS_EXIT 0
#define SYS_WRITE 1
#define SYS_YIELD 2
#define SYS_SLEEP 3
#define SYS_THREAD_EXIT 4
#define SYS_GETPID 5
#define SYS_THREAD_CREATE 6
#define SYS_GETUID 7
#define SYS_GETGID 8
#define SYS_GETCAPS 9
#define SYS_HAS_CAPABILITY 10
#define SYS_EXEC 11
#define SYS_EXEC_ADMIN 12
#define SYS_USER_QUERY_RESERVED 13
#define SYS_USER_ADD_RESERVED 14
#define SYS_EXEC_AS_USER 15
#define SYS_FS_EXISTS 16
#define SYS_FS_STAT 17
#define SYS_FS_READ 18
#define SYS_FS_WRITE 19
#define SYS_FS_LIST 20
#define SYS_FS_LIST_RANGE 85
#define SYS_FS_MKDIR 21
#define SYS_FS_UNLINK 22
#define SYS_GETTICKS 23
#define SYS_READ 24
#define SYS_FD_WRITE 25
#define SYS_OPEN 26
#define SYS_CLOSE 27
#define SYS_SEEK 28
#define SYS_MMAP 29
#define SYS_MUNMAP 30
#define SYS_WAITPID 31
#define SYS_PROCESS_QUERY 32
#define SYS_KILL 33
#define SYS_SIGACTION 34
#define SYS_SIGRETURN 35
#define SYS_FB_INFO 36
#define SYS_FB_PIXEL 37
#define SYS_FB_RECT 38
#define SYS_FB_RECT_ALPHA 39
#define SYS_FB_GRADIENT 40
#define SYS_FB_OUTLINE 41
#define SYS_FB_LINE 42
#define SYS_FB_CIRCLE 43
#define SYS_FB_FILL_CIRCLE 44
#define SYS_FB_CHAR 45
#define SYS_FB_TEXT 46
#define SYS_NET_INFO 47
#define SYS_NET_PING 48
#define SYS_NET_DNS 49
#define SYS_NET_TCP_CONNECT 50
#define SYS_NET_TCP_SEND 51
#define SYS_NET_TCP_RECV 52
#define SYS_NET_TCP_CLOSE 53
#define SYS_FB_SCROLL_UP 54
#define SYS_LOGIN 55
#define SYS_LOGOUT 56
#define SYS_USER_STATUS 57
#define SYS_USER_CREATE 58
#define SYS_USER_LIST 59
#define SYS_PRINT 60
#define SYS_PRINT_COLOR 61
#define SYS_PRINT_RESET_CURSOR 62
#define SYS_RTC_UTC 63
#define SYS_SYSTEM_REBOOT 64
#define SYS_SYSTEM_SHUTDOWN 65
#define SYS_FS_MOVE 66
#define SYS_SECURE_RANDOM 67
#define SYS_NET_DNS_IPV6 68
#define SYS_NET_TCP_CONNECT_IPV6 69
#define SYS_NET_IPV6_INFO 70
#define SYS_PRINT_CURSOR_GET 71
#define SYS_PRINT_CURSOR_SET 72
#define SYS_PRINT_SCROLLBACK 73
#define SYS_INPUT_POLL 74
#define SYS_POINTER_POLL 75
#define SYS_POINTER_SOURCES 76
#define SYS_EXEC_WINDOWED 77
#define SYS_FB_SURFACE_BLIT 78
#define SYS_FB_SURFACE_RESIZE 79
#define SYS_INPUT_FORWARD 80
#define SYS_FB_SURFACE_GENERATION 81
#define SYS_TERMINATE_WINDOWED_TREE 82
#define SYS_FB_FRAME_BEGIN 83
#define SYS_FB_FRAME_END 84

/* Returns use negative errno values on failure unless the wrapper says otherwise. */
/* SYS_WRITE: RDI=user buffer, RSI=byte count; returns bytes written. */
/* SYS_READ/SYS_FD_WRITE: RDI=fd, RSI=buffer, RDX=count. */
/* SYS_OPEN: RDI=path, RSI=flags; SYS_CLOSE: RDI=fd. */
/* SYS_SEEK: RDI=fd, RSI=signed offset, RDX=SEEK_SET/CUR/END. */
/* SYS_MMAP: RDI=size; SYS_MUNMAP: RDI=address, RSI=size. */
/* SYS_WAITPID: RDI=pid, RSI=status pointer, RDX=options. */
/* SYS_PROCESS_QUERY: RDI=pid, RSI=information pointer. */
/* SYS_KILL: RDI=pid, RSI=signal. */
/* SYS_SIGACTION: RDI=signal, RSI=handler, RDX=old-handler pointer (optional). */
/* SYS_SIGRETURN is entered only by the kernel's user signal-return trampoline. */
/* SYS_THREAD_CREATE: RDI=user entry, RSI=argument; returns the new thread ID. */
/* FS calls use absolute VFS paths and return either a result or -errno. */
/* SYS_FS_MOVE: RDI=source path, RSI=destination path. */
/* FB draw calls receive signed coordinates and colors in 0xAARRGGBB format. */
/* FB scroll-up is bounded to 16 Mi pixels and receives x,y,w,h,rows,color. */
/* SYS_PRINT: RDI=text, RSI=font name (NULL selects system16). */
/* SYS_PRINT_COLOR: RDI=text, RSI=ARGB foreground, RDX=font name. */
/* SYS_PRINT_RESET_CURSOR: reset the shared print cursor to the screen margin. */
/* PRINT_CURSOR_GET: RDI=welovec_fb_text_cursor output buffer. */
/* PRINT_CURSOR_SET: RDI=x, RSI=y. PRINT_SCROLLBACK: RDI=1 older, -1 newer, 0 live. */
/* INPUT_POLL: RDI=buffer, RSI=capacity; returns queued keyboard bytes without blocking. */
/* SYS_RTC_UTC: RDI=welovec_user_datetime output buffer. */
/* SYS_SYSTEM_REBOOT/SHUTDOWN: RDI=1 for a virtual machine, 0 for hardware. */
/* Print calls accept system16/system8/ubuntu and return bytes written. */
/* Network calls expose IPv4/ICMP/DNS and owned TCP handles, never raw Ethernet. */

#ifdef __cplusplus
extern "C" {
#endif

void syscall_init(void);
extern void syscall_entry(void);

void syscall_dispatch(syscall_regs_t *regs);

#ifdef __cplusplus
}
#endif

#endif // ARCH_X86_64_SYSCALL_H
