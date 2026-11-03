<!-- Written by [@saphhic](https://github.com/saphhic) -->
<!-- Date: November 3rd 2026 -->
<!-- Description: Documents the system call API available to user-space programs. -->

# Userland system-call API

User programs include headers from `kernel/include/user/`. The syscall number
is passed in `RAX`; up to six arguments use `RDI`, `RSI`, `RDX`, `R10`, `R8`,
and `R9`, in that order. The wrappers issue the `syscall` instruction and
return either a result or a negative errno value. `RCX` and `R11` are clobbered.
Pointer arguments are copied and checked by the kernel; user programs do not
receive kernel addresses.

## Framebuffer and desktop drawing

Include `user/framebuffer.h`. Regular processes draw to the shared framebuffer,
so their drawing calls can overwrite pixels another program has drawn. A child
started with `EXEC_WINDOWED` instead draws into a process-owned surface that
the desktop composites into its window. Calls are clipped to the active surface
and drawing requests are bounded to 16 Mi pixels per call; text is limited to
4096 bytes. Colors use `0xAARRGGBB` (`welovec_fb_rgb()` creates opaque colors).
The desktop batches each damaged region with
`welovec_fb_frame_begin()` and `welovec_fb_frame_end()`, so its drawing commands
are published together instead of appearing one primitive at a time. Other
programs can use the same pair; if the kernel cannot allocate its screen-sized
back buffer, `frame_begin` returns an error and callers can keep drawing
directly.

```c
#include "user/framebuffer.h"

static void draw_desktop(void) {
    struct welovec_fb_info screen;
    if (welovec_fb_get_info(&screen) < 0) return;

    const uint32_t background = welovec_fb_rgb(24, 32, 48);
    const uint32_t panel = welovec_fb_rgb(40, 56, 80);
    (void)welovec_fb_rect(0, 0, (int32_t)screen.width,
                          (int32_t)screen.height, background);
    (void)welovec_fb_rect(0, 0, (int32_t)screen.width, 40, panel);
    (void)welovec_fb_text(16, 12, "WeLoveC desktop", 0xffffffff);
}
```

Available wrappers are `welovec_fb_get_info`, `welovec_fb_pixel`,
`welovec_fb_rect`, `welovec_fb_rect_alpha`, `welovec_fb_gradient`,
`welovec_fb_outline`, `welovec_fb_line`, `welovec_fb_circle`,
`welovec_fb_fill_circle`, `welovec_fb_char`, `welovec_fb_text`, and
`welovec_fb_scroll_up`.
`welovec_fb_char` accepts scales 1–4. Text uses the current 16×16 font at scale
1, starts at the supplied coordinates, and treats newline as a new line.
`welovec_fb_scroll_up` shifts a checked rectangle upward and fills the exposed
rows with the supplied color. Each drawing syscall is limited to 16 Mi pixels.

The API does not expose the framebuffer address or `framebuffer_init`; apps
draw through checked operations rather than writing physical memory. The
`printf_fb` convenience library keeps its own cursor. The `welovec_print*()`
and `welovec_print_cursor_*()` syscalls use the kernel framebuffer text cursor.
`welovec_fb_frame_begin(x, y, width, height)` starts a buffered update for a
screen region, and `welovec_fb_frame_end()` publishes it. Always end a frame
after drawing; process termination cancels an unfinished frame.

### Text output and clearing

Include `user/stdio.h`. The `printf_fb` helpers keep a cursor in the userland
library, use the 16×16 font, handle newlines, tabs, backspace and line wrapping,
and scroll at the bottom. They do not parse format strings. The single-color
form uses a transparent character background; `printf_fb_colors` accepts both
foreground and background colors. Colors use `0xAARRGGBB`. Compile and link
`userland/lib/fbstdio.c` into each executable that uses these functions.

```c

#include "user/stdio.h"
#include "user/framebuffer.h"

static void show_status(void) {
    (void)clear_screen();
    (void)printf_fb("WeLoveC userland\n");
    (void)printf_fb_color("[OK] ", welovec_fb_rgb(80, 220, 120));
    (void)printf_fb_colors("network ready\n",
                           welovec_fb_rgb(255, 255, 255),
                           welovec_fb_rgb(24, 48, 32));
}
```

Print helpers return the number of text bytes or a negative errno. Clear helpers
return zero on success or a negative errno. The `welovec_`-prefixed variants
expose the same API. `clear_screen_color(color)` fills the whole framebuffer in
bounded rectangles and resets this library's cursor.
For ordinary processes, clearing and drawing affect the shared framebuffer, so
a clear can erase the kernel console or another program's drawing. A child
started with `EXEC_WINDOWED` draws on its private surface, which the desktop
composites into the window. `welovec_fb_print_set_cursor(x, y)` sets
the library cursor in pixel coordinates; `welovec_fb_print_reset_cursor()`
returns it to the default margin on the next print.

## Keyboard input

Input uses the existing `SYS_READ` syscall on file descriptor 0. Include
`user/input.h` and call `welovec_input_read(buffer, capacity)`. The call blocks
until at least one character is queued, returns that character plus any other
characters already queued, or returns a negative errno. Key-down events are
translated to ASCII using the current US QWERTY map; key-up events and raw HID
events are not exposed. Enter, tab, backspace, and Escape map to `\n`, `\t`,
`\b`, and byte `0x1b`.

```c
#include "user/input.h"
#include "user/syscall.h"

static void terminal_read_once(void) {
    char input[64];
    const int64_t count = welovec_input_read(input, sizeof(input));
    if (count > 0) (void)welovec_write(input, (uint64_t)count);
}
```

Ordinary processes read from the shared keyboard queue. Windowed children read
from private input queues populated by their desktop parent. For nonblocking
input, `welovec_input_poll()` returns up to 256 available characters;
`welovec_pointer_poll()` returns up to 32 pointer events. Both return zero when
their queue is empty. `welovec_pointer_sources()` reports the detected pointer
source mask, and `welovec_input_forward()` lets the parent desktop send
characters and pointer events to a windowed child it owns.

## Network access

Include `user/network.h`. Userland gets IPv4/IPv6 configuration, ICMP ping,
DNS A/AAAA lookups, and a small process-owned TCP interface. No raw Ethernet
frames, DMA buffers, PCI/MMIO addresses, or driver controls are exposed. DNS
requests use the configured IPv4 DNS server. TCP has at most eight kernel
connections system-wide, allows up to 1400 bytes per send or receive call, and
receive is nonblocking (zero means no data queued yet). Each receive call,
including one with zero capacity, polls the network stack once and then drains
queued bytes; it must not skip the poll or it can leave newly arrived TCP data
unread.

```c
#include "user/network.h"

static void request_http_page(void) {
    uint8_t address[4];
    if (welovec_dns_lookup_ipv4("example.com", address) < 0) return;
    const int64_t socket = welovec_tcp_connect_ipv4(address, 80);
    if (socket < 0) return;

    static const char request[] =
        "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    (void)welovec_tcp_send((int)socket, request, sizeof(request) - 1);
    char response[512];
    for (;;) {
        const int64_t count = welovec_tcp_receive(
            (int)socket, response, sizeof(response));
        if (count < 0) break;
        if (count > 0) {
            /* Consume `count` response bytes here. */
            break;
        }
        (void)welovec_sleep(1);
    }
    (void)welovec_tcp_close((int)socket);
}
```

The TCP transport retries SYN and unacknowledged data segments, tracks peer ACKs
and receive windows, rejects data it cannot retain, and uses unpredictable
initial sequence numbers and ephemeral ports from the kernel random generator.
Connect failures preserve their cause: `-EAGAIN` means no secure entropy source
is available, `-EHOSTUNREACH` means ARP/NDP could not resolve the next hop,
`-EIO` means sending the SYN failed, `-ECONNREFUSED` means the peer sent a reset,
and `-ETIMEDOUT` means no handshake response arrived.
It still processes data in order with a stop-and-wait sender; congestion
control, window scaling, out-of-order buffering, complete FIN handling, UDP
sockets, and raw sockets are not implemented. IPv4 fragments and IPv6 extension
headers are unsupported. It is not yet a production TCP stack.

TLS is available through the userland Mbed TLS adapter in `userland/lib/tls/`,
not through a kernel syscall. The adapter requires TLS 1.2 or newer, validates
the certificate chain and hostname against the installed CA bundle, and uses
the kernel random and UTC clock interfaces. The build links that adapter into
both `install` and the text browser. The shared HTTP client in
`userland/lib/web/http.c` provides bounded GET requests, `Content-Length` and
chunked response handling, and limited redirect support. The browser keeps
certificate errors visible and never silently falls back from HTTPS to HTTP.
Its HTML-to-text converter is a small presentation helper, not a standards
compliant browser engine.

Ping/DNS/TCP operations and the kernel polling worker are serialized because
the current network stack has shared packet-processing state. Browser work
still needs real network-loss and interoperability checks before this transport
can be treated as production-ready.

## Syscall number list

The table covers the current syscall ABI. IDs 13 and 14 are reserved and return
`-ENOSYS`; the other listed IDs are implemented. Unless specified otherwise,
failures return `-errno`.

| ID | Name | Purpose / arguments |
|---:|---|---|
| 0 | `EXIT` | Exit process; `RDI=status`. |
| 1 | `WRITE` | Write to kernel console; `RDI=buffer`, `RSI=size`. |
| 2 | `YIELD` | Yield the current thread. |
| 3 | `SLEEP` | Sleep scheduler ticks; `RDI=ticks`. |
| 4 | `THREAD_EXIT` | Exit only the current thread; `RDI=status`. |
| 5 | `GETPID` | Return current process ID. |
| 6 | `THREAD_CREATE` | Create a thread; `RDI=entry`, `RSI=argument`. |
| 7 | `GETUID` | Return current user ID. |
| 8 | `GETGID` | Return current group ID. |
| 9 | `GETCAPS` | Return current capability mask. |
| 10 | `HAS_CAPABILITY` | `RDI=capability`; return 1 or 0. |
| 11 | `EXEC` | Execute ELF as caller; path, argv, argc, priority in `RDI`,`RSI`,`RDX`,`R10`. |
| 12 | `EXEC_ADMIN` | Execute ELF as administrator; requires `EXEC_AS_ADMIN`. |
| 13 | `RESERVED_13` | Reserved; returns `-ENOSYS`. |
| 14 | `RESERVED_14` | Reserved; returns `-ENOSYS`. |
| 15 | `EXEC_AS_USER` | Execute as an existing UID; requires `MANAGE_USERS` and any added capabilities. |
| 16 | `FS_EXISTS` | `RDI=path`; return 1 if present, else 0. |
| 17 | `FS_STAT` | `RDI=path`, `RSI=file info output`. |
| 18 | `FS_READ` | Read whole file; `RDI=path`, `RSI=buffer`, `RDX=capacity`. |
| 19 | `FS_WRITE` | Replace whole file; `RDI=path`, `RSI=buffer`, `RDX=size`; guest write paths are restricted. |
| 20 | `FS_LIST` | `RDI=directory`, `RSI=entries`, `RDX=capacity` (maximum 32; returns the first entries). |
| 21 | `FS_MKDIR` | Create directory; `RDI=path`. |
| 22 | `FS_UNLINK` | Remove file; `RDI=path`. |
| 23 | `GETTICKS` | Return scheduler tick count. |
| 24 | `READ` | Read descriptor; `RDI=fd`, `RSI=buffer`, `RDX=size`; fd 0 blocks for keyboard input or a windowed child's private input. |
| 25 | `FD_WRITE` | Write descriptor; `RDI=fd`, `RSI=buffer`, `RDX=size`. |
| 26 | `OPEN` | Open VFS path; `RDI=path`, `RSI=flags`; returns descriptor. |
| 27 | `CLOSE` | Close descriptor; `RDI=fd`. |
| 28 | `SEEK` | Seek descriptor; `RDI=fd`, `RSI=offset`, `RDX=SET/CUR/END`. |
| 29 | `MMAP` | Map anonymous user memory; `RDI=size`, limited by boot-time free-page capacity and current available memory (500 MiB fallback if capacity is unavailable). |
| 30 | `MUNMAP` | Unmap user range; `RDI=address`, `RSI=size`. |
| 31 | `WAITPID` | Wait for child; `RDI=pid`, `RSI=status pointer`, `RDX=options`. |
| 32 | `PROCESS_QUERY` | Query process; `RDI=pid`, `RSI=process info output`. |
| 33 | `KILL` | Send supported signal; `RDI=pid`, `RSI=signal`; permission checked. |
| 34 | `SIGACTION` | Set signal handler; `RDI=signal`, `RSI=handler`, `RDX=old handler output`. |
| 35 | `SIGRETURN` | Restore context; used by the kernel signal trampoline. |
| 36 | `FB_INFO` | `RDI=framebuffer info output`; no framebuffer pointer is returned. |
| 37 | `FB_PIXEL` | `RDI=x`, `RSI=y`, `RDX=ARGB color`. |
| 38 | `FB_RECT` | `RDI=x`, `RSI=y`, `RDX=w`, `R10=h`, `R8=color`. |
| 39 | `FB_RECT_ALPHA` | Rect arguments plus `R8=color`, `R9=alpha`. |
| 40 | `FB_GRADIENT` | Rect arguments plus `R8=top color`, `R9=bottom color`. |
| 41 | `FB_OUTLINE` | Rect arguments plus `R8=color`. |
| 42 | `FB_LINE` | `RDI=x0`, `RSI=y0`, `RDX=x1`, `R10=y1`, `R8=color`. |
| 43 | `FB_CIRCLE` | `RDI=cx`, `RSI=cy`, `RDX=radius`, `R10=color`. |
| 44 | `FB_FILL_CIRCLE` | Same arguments as `FB_CIRCLE`, filled. |
| 45 | `FB_CHAR` | `RDI=x`, `RSI=y`, `RDX=ASCII`, `R10=fg`, `R8=bg`, `R9=scale`. |
| 46 | `FB_TEXT` | `RDI=x`, `RSI=y`, `RDX=text`, `R10=color`; maximum 4096 bytes. |
| 47 | `NET_INFO` | `RDI=IPv4 network info output`. |
| 48 | `NET_PING` | `RDI=4-byte IPv4 address`; return 0 on reply, `-ETIMEDOUT` otherwise. |
| 49 | `NET_DNS` | `RDI=hostname`, `RSI=4-byte IPv4 output`. |
| 50 | `NET_TCP_CONNECT` | `RDI=IPv4 address`, `RSI=port`; returns a process-owned socket handle or a negative errno identifying route, entropy, send, refusal, or timeout failures. |
| 51 | `NET_TCP_SEND` | `RDI=socket`, `RSI=data`, `RDX=size` (maximum 1400). |
| 52 | `NET_TCP_RECV` | `RDI=socket`, `RSI=output`, `RDX=capacity` (maximum 1400, nonblocking; polls once). |
| 53 | `NET_TCP_CLOSE` | Close process-owned TCP handle; `RDI=socket`. |
| 54 | `FB_SCROLL_UP` | `RDI=x`, `RSI=y`, `RDX=w`, `R10=h`, `R8=rows`, `R9=fill color`; region limited to 16 Mi pixels. |
| 55 | `LOGIN` | Authenticate the current process; `RDI=username`, `RSI=password`. |
| 56 | `LOGOUT` | End the current login session and restore guest credentials. |
| 57 | `USER_STATUS` | Copy current login status to the structure at `RDI`. |
| 58 | `USER_CREATE` | Create an account; `RDI=username`, `RSI=password`, `RDX=flags`; requires account-management permission. |
| 59 | `USER_LIST` | Copy account records to `RDI`; `RSI=capacity`. A null pointer and zero capacity query the count. |
| 60 | `PRINT` | Print a NUL-terminated string using the kernel framebuffer cursor; `RDI=text`, `RSI=optional font name`. |
| 61 | `PRINT_COLOR` | Print a NUL-terminated string with a color; `RDI=text`, `RSI=ARGB color`, `RDX=optional font name`. |
| 62 | `PRINT_RESET_CURSOR` | Reset the kernel framebuffer text cursor. |
| 63 | `RTC_UTC` | Copy UTC date and time to the structure at `RDI`. |
| 64 | `SYSTEM_REBOOT` | Reboot; `RDI=1` selects the virtual-machine method, `0` the hardware method. |
| 65 | `SYSTEM_SHUTDOWN` | Shut down; `RDI=1` selects the virtual-machine method, `0` the hardware method. |
| 66 | `FS_MOVE` | Move a file; `RDI=source path`, `RSI=destination path`. |
| 67 | `SECURE_RANDOM` | Fill `RDI=buffer` with `RSI=size` secure random bytes (maximum 256); returns `-ENODEV` if no secure source is available. |
| 68 | `NET_DNS_IPV6` | Resolve a hostname to an IPv6 address; `RDI=hostname`, `RSI=16-byte output`. |
| 69 | `NET_TCP_CONNECT_IPV6` | Connect to IPv6; `RDI=16-byte address`, `RSI=port`; returns a process-owned socket handle. |
| 70 | `NET_IPV6_INFO` | Copy IPv6 configuration to the structure at `RDI`. |
| 71 | `PRINT_CURSOR_GET` | Copy the kernel framebuffer text cursor coordinates to `RDI`. |
| 72 | `PRINT_CURSOR_SET` | Set text cursor coordinates; `RDI=x`, `RSI=y`. |
| 73 | `PRINT_SCROLLBACK` | `RDI=direction`: 1 older output, -1 newer, 0 live output. Returns 1 if the view changed. |
| 74 | `INPUT_POLL` | Nonblocking keyboard read; `RDI=buffer`, `RSI=capacity` (maximum 256). Uses the window's private queue when available. |
| 75 | `POINTER_POLL` | Nonblocking pointer-event read; `RDI=events`, `RSI=capacity` (maximum 32). Uses the window's private queue when available. |
| 76 | `POINTER_SOURCES` | Return the detected pointer-source bitmask. |
| 77 | `EXEC_WINDOWED` | Start a child with a private framebuffer and input queue; `RDI=path`, `RSI=argv`, `RDX=argc`, `R10=priority`, `R8=width`, `R9=height`. |
| 78 | `FB_SURFACE_BLIT` | Copy a child surface into the caller's framebuffer; `RDI=pid`, `RSI=dest x`, `RDX=dest y`, `R10=source x`, `R8=source y`, `R9=(width<<32)|height`. |
| 79 | `FB_SURFACE_RESIZE` | Resize an owned child's surface; `RDI=pid`, `RSI=width`, `RDX=height`. |
| 80 | `INPUT_FORWARD` | Forward child input; `RDI=pid`, `RSI=characters`, `RDX=character count` (max 255), `R10=pointer events`, `R8=event count` (max 15). |
| 81 | `FB_SURFACE_GENERATION` | Return the generation number for an owned child surface; `RDI=pid`. |
| 82 | `TERMINATE_WINDOWED_TREE` | Terminate an owned windowed child and its descendants; `RDI=pid`, `RSI=status`. |
| 83 | `FB_FRAME_BEGIN` | Buffer a clipped screen region; `RDI=x`, `RSI=y`, `RDX=width`, `R10=height`. |
| 84 | `FB_FRAME_END` | Publish the current buffered region; no arguments. |
| 85 | `FS_LIST_RANGE` | `RDI=directory`, `RSI=entries`, `RDX=capacity` (maximum 32), `R10=entry offset`. |

`FS_LIST_RANGE` pages through a directory without changing the existing
`FS_LIST` behavior. Its capacity is at most 32 entries. Advance the offset by
the number of entries returned until the call returns fewer entries than the
requested capacity. The `apps` shell command uses this interface to recursively
find and list ELF files across the VFS.

Descriptors 0, 1, and 2 are standard input, output, and error. Each process can
open up to 29 additional files, with a kernel-wide limit of 128 open files.
Whole-file `FS_READ` and `FS_WRITE` operations are limited to 16 MiB per call.
Filesystem access checks use the caller's identity and capabilities. All
sessions may write under shared `/tmp`; authenticated users may also write in
their own `/home/<name>`. The system-configuration capability permits other
write paths, except that account database files remain kernel-only.

## Return-value convention

Successful operations return zero, a byte count, an ID/handle, or the documented
boolean value. Errors are negative errno values such as `-EFAULT`, `-EINVAL`,
`-ENODEV`, `-EBADF`, and `-ETIMEDOUT`. `SIGRETURN` is internal and ordinary
applications should not call it directly.
