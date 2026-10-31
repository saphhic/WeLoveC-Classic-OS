// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Validates user requests and dispatches them to the appropriate kernel operation.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

#include "syscall.h"
#include "../io/serial.h"
#include "../scheduler/scheduler.hpp"
#include "arch/common/Process/manager.hpp"
#include "arch/common/MM/Heap/heap.hpp"
#include "arch/x86_64/exec/elf_loader.h"
#include "filesystem/vfs.h"
#include "filesystem/file_descriptors.hpp"
#include "IDM/DisplayService/framebuffer.hpp"
#include "IDM/EthernetService/netstack.h"
#include "IDM/EthernetService/netstack_ipv6.h"
#include "IDM/EthernetService/netstack_tcp.h"
#include "IDM/EthernetService/service.h"
#include "arch/x86_64/input/keyboard.h"
#include "arch/x86_64/input/mouse.hpp"
#include "arch/x86_64/power/power.h"
#include "arch/common/Time/rtc.h"
#include "arch/x86_64/mm/spinlock.hpp"
#include "user/filesystem.h"
#include "user/framebuffer.h"
#include "user/network.h"
#include "user/process.h"
#include "user/time.h"
#include "security/privileges.hpp"
#include "security/privileges.h"

namespace {
constexpr uint64_t kWriteLimit = 1024 * 1024;
constexpr uint64_t kWriteChunk = 256;
constexpr uint64_t kFileIoLimit = 16 * 1024 * 1024;
constexpr uint32_t kExecPathCapacity = 256;
constexpr uint32_t kMaximumExecArguments = 64;
constexpr uint32_t kExecArgumentBytes = 16 * 1024;
constexpr uint64_t kSignalReturnAddress = UINT64_C(0x10020);
constexpr uint64_t kErrPermission = 13;
constexpr uint64_t kErrFault = 14;
constexpr uint64_t kErrInvalid = 22;
constexpr uint64_t kErrNoMemory = 12;
constexpr uint64_t kErrNotFound = 2;
constexpr uint64_t kErrTooManyArguments = 7;
constexpr uint64_t kErrNoProcess = 3;
constexpr uint64_t kErrIo = 5;
constexpr uint64_t kErrIsDirectory = 21;
constexpr uint64_t kErrNotDirectory = 20;
constexpr uint64_t kErrExist = 17;
constexpr uint64_t kErrDirectoryNotEmpty = 39;
constexpr uint64_t kErrNoSpace = 28;
constexpr uint64_t kErrFileTooLarge = 27;
constexpr uint64_t kErrBadDescriptor = 9;
constexpr uint64_t kErrChild = 10;
constexpr uint64_t kErrNoDevice = 19;
constexpr uint64_t kErrNotSupported = 95;
constexpr uint64_t kErrTooBig = 7;
constexpr uint64_t kErrNoFileDescriptor = 24;
constexpr uint64_t kErrNetworkUnreachable = 101;
constexpr uint64_t kErrTimedOut = 110;
constexpr uint64_t kFramebufferMaxPixelsPerCall = UINT64_C(16) * 1024 * 1024;
constexpr uint32_t kFramebufferMaxTextBytes = 4096;
constexpr uint32_t kTcpUserSocketCount = WELOVEC_TCP_MAX_CONNECTIONS;
constexpr uint32_t kTcpUserIoLimit = WELOVEC_TCP_MAX_IO;
constexpr uint32_t kKnownOpenFlags = welovec::filesystem::kOpenRead |
    welovec::filesystem::kOpenWrite | welovec::filesystem::kOpenCreate |
    welovec::filesystem::kOpenTruncate | welovec::filesystem::kOpenAppend;

struct UserTcpSocket {
    uint64_t owner_process;
    int32_t transport_id;
};

welovec::x86_64::mm::IrqSpinLock gUserTcpSocketLock;
UserTcpSocket gUserTcpSockets[kTcpUserSocketCount]{};

bool copy_user_string(uint64_t source, char *destination, uint64_t capacity,
                      uint64_t *out_length, bool allow_empty = false);

bool decode_i32(uint64_t value, int32_t *out) {
    if (!out) return false;
    const int64_t signed_value = static_cast<int64_t>(value);
    if (signed_value < INT32_MIN || signed_value > INT32_MAX) return false;
    *out = static_cast<int32_t>(signed_value);
    return true;
}

bool framebuffer_ready() {
    return framebuffer_available();
}

bool framebuffer_region_valid(int32_t width, int32_t height) {
    return width > 0 && height > 0;
}

int64_t framebuffer_info_syscall(const syscall_regs_t *regs) {
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);
    uint32_t width = 0, height = 0;
    if (!framebuffer_get_resolution(&width, &height))
        return -static_cast<int64_t>(kErrNoDevice);
    const welovec_fb_info info{width, height, pitch, 4};
    return welovec::process::copy_to_current_user(
        regs->rdi, &info, sizeof(info))
        ? 0 : -static_cast<int64_t>(kErrFault);
}

int64_t framebuffer_pixel_syscall(const syscall_regs_t *regs) {
    int32_t x = 0, y = 0;
    if (!decode_i32(regs->rdi, &x) || !decode_i32(regs->rsi, &y) ||
        regs->rdx > UINT32_MAX)
        return -static_cast<int64_t>(kErrInvalid);
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);
    put_pixel(x, y, static_cast<uint32_t>(regs->rdx));
    return 0;
}

int64_t framebuffer_rect_syscall(const syscall_regs_t *regs) {
    int32_t x = 0, y = 0, width = 0, height = 0;
    if (!decode_i32(regs->rdi, &x) || !decode_i32(regs->rsi, &y) ||
        !decode_i32(regs->rdx, &width) || !decode_i32(regs->r10, &height) ||
        regs->r8 > UINT32_MAX || !framebuffer_region_valid(width, height))
        return -static_cast<int64_t>(kErrInvalid);
    if (static_cast<uint64_t>(width) * height > kFramebufferMaxPixelsPerCall)
        return -static_cast<int64_t>(kErrTooBig);
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);
    draw_rect(x, y, width, height, static_cast<uint32_t>(regs->r8));
    return 0;
}

int64_t framebuffer_alpha_rect_syscall(const syscall_regs_t *regs) {
    int32_t x = 0, y = 0, width = 0, height = 0;
    if (!decode_i32(regs->rdi, &x) || !decode_i32(regs->rsi, &y) ||
        !decode_i32(regs->rdx, &width) || !decode_i32(regs->r10, &height) ||
        regs->r8 > UINT32_MAX || regs->r9 > UINT8_MAX ||
        !framebuffer_region_valid(width, height))
        return -static_cast<int64_t>(kErrInvalid);
    if (static_cast<uint64_t>(width) * height > kFramebufferMaxPixelsPerCall)
        return -static_cast<int64_t>(kErrTooBig);
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);
    draw_rect_alpha(x, y, width, height, static_cast<uint32_t>(regs->r8),
                    static_cast<uint8_t>(regs->r9));
    return 0;
}

int64_t framebuffer_gradient_syscall(const syscall_regs_t *regs) {
    int32_t x = 0, y = 0, width = 0, height = 0;
    if (!decode_i32(regs->rdi, &x) || !decode_i32(regs->rsi, &y) ||
        !decode_i32(regs->rdx, &width) || !decode_i32(regs->r10, &height) ||
        regs->r8 > UINT32_MAX || regs->r9 > UINT32_MAX ||
        !framebuffer_region_valid(width, height))
        return -static_cast<int64_t>(kErrInvalid);
    if (static_cast<uint64_t>(width) * height > kFramebufferMaxPixelsPerCall)
        return -static_cast<int64_t>(kErrTooBig);
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);
    draw_rect_vgradient(x, y, width, height, static_cast<uint32_t>(regs->r8),
                        static_cast<uint32_t>(regs->r9));
    return 0;
}

int64_t framebuffer_outline_syscall(const syscall_regs_t *regs) {
    int32_t x = 0, y = 0, width = 0, height = 0;
    if (!decode_i32(regs->rdi, &x) || !decode_i32(regs->rsi, &y) ||
        !decode_i32(regs->rdx, &width) || !decode_i32(regs->r10, &height) ||
        regs->r8 > UINT32_MAX || !framebuffer_region_valid(width, height))
        return -static_cast<int64_t>(kErrInvalid);
    if (static_cast<uint64_t>(width) * height > kFramebufferMaxPixelsPerCall)
        return -static_cast<int64_t>(kErrTooBig);
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);
    draw_rect_outline(x, y, width, height, static_cast<uint32_t>(regs->r8));
    return 0;
}

int64_t framebuffer_line_syscall(const syscall_regs_t *regs) {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!decode_i32(regs->rdi, &x0) || !decode_i32(regs->rsi, &y0) ||
        !decode_i32(regs->rdx, &x1) || !decode_i32(regs->r10, &y1) ||
        regs->r8 > UINT32_MAX)
        return -static_cast<int64_t>(kErrInvalid);
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);
    draw_line(x0, y0, x1, y1, static_cast<uint32_t>(regs->r8));
    return 0;
}

int64_t framebuffer_circle_syscall(const syscall_regs_t *regs, bool filled) {
    int32_t center_x = 0, center_y = 0, radius = 0;
    if (!decode_i32(regs->rdi, &center_x) || !decode_i32(regs->rsi, &center_y) ||
        !decode_i32(regs->rdx, &radius) || radius <= 0 ||
        regs->r10 > UINT32_MAX)
        return -static_cast<int64_t>(kErrInvalid);
    const uint64_t diameter = static_cast<uint64_t>(radius) * 2 + 1;
    if (diameter > kFramebufferMaxPixelsPerCall / diameter)
        return -static_cast<int64_t>(kErrTooBig);
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);
    if (filled) draw_filled_circle(center_x, center_y, radius,
                                   static_cast<uint32_t>(regs->r10));
    else draw_circule(center_x, center_y, radius,
                      static_cast<uint32_t>(regs->r10));
    return 0;
}

int64_t framebuffer_char_syscall(const syscall_regs_t *regs) {
    int32_t x = 0, y = 0;
    if (!decode_i32(regs->rdi, &x) || !decode_i32(regs->rsi, &y) ||
        regs->rdx > 0x7f || regs->r10 > UINT32_MAX || regs->r8 > UINT32_MAX ||
        regs->r9 < 1 || regs->r9 > 4)
        return -static_cast<int64_t>(kErrInvalid);
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);
    draw_char(x, y, static_cast<char>(regs->rdx),
              static_cast<uint32_t>(regs->r10),
              static_cast<uint32_t>(regs->r8),
              static_cast<int>(regs->r9));
    return 0;
}

int64_t framebuffer_text_syscall(const syscall_regs_t *regs) {
    int32_t x = 0, y = 0;
    if (!decode_i32(regs->rdi, &x) || !decode_i32(regs->rsi, &y) ||
        regs->r10 > UINT32_MAX)
        return -static_cast<int64_t>(kErrInvalid);
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);
    char text[kFramebufferMaxTextBytes + 1];
    uint64_t length = 0;
    if (!copy_user_string(regs->rdx, text, sizeof(text), &length, true))
        return -static_cast<int64_t>(kErrFault);
    int64_t cursor_x = x;
    int64_t cursor_y = y;
    for (uint64_t i = 0; i + 1 < length; ++i) {
        const char character = text[i];
        if (character == '\n') {
            cursor_x = x;
            cursor_y += 16;
            continue;
        }
        if (character == '\r') {
            cursor_x = x;
            continue;
        }
        if (cursor_x >= INT32_MIN && cursor_x <= INT32_MAX &&
            cursor_y >= INT32_MIN && cursor_y <= INT32_MAX)
            draw_char(static_cast<int>(cursor_x), static_cast<int>(cursor_y),
                      character, static_cast<uint32_t>(regs->r10), 0, 1);
        cursor_x += 16;
    }
    return static_cast<int64_t>(length - 1);
}

bool framebuffer_font_from_name(const char *name, uint32_t *out_font) {
    if (!name || !out_font || !name[0]) return false;

    const char *candidates[] = {"system16", "system8", "ubuntu"};
    const uint32_t fonts[] = {
        WELOVEC_FB_FONT_SYSTEM16,
        WELOVEC_FB_FONT_SYSTEM8,
        WELOVEC_FB_FONT_UBUNTU
    };
    for (uint32_t candidate = 0; candidate < 3; ++candidate) {
        uint32_t index = 0;
        while (name[index] && candidates[candidate][index] == name[index])
            ++index;
        if (!name[index] && !candidates[candidate][index]) {
            *out_font = fonts[candidate];
            return true;
        }
    }
    return false;
}

int64_t framebuffer_print_syscall(const syscall_regs_t *regs,
                                  bool with_color) {
    if (with_color && regs->rsi > UINT32_MAX)
        return -static_cast<int64_t>(kErrInvalid);

    uint32_t font = WELOVEC_FB_FONT_SYSTEM16;
    const uint64_t font_name_pointer = with_color ? regs->rdx : regs->rsi;
    if (font_name_pointer) {
        char font_name[16];
        uint64_t font_name_length = 0;
        if (!copy_user_string(font_name_pointer, font_name, sizeof(font_name),
                              &font_name_length))
            return -static_cast<int64_t>(kErrFault);
        if (!framebuffer_font_from_name(font_name, &font))
            return -static_cast<int64_t>(kErrInvalid);
    }

    char text[kFramebufferMaxTextBytes + 1];
    uint64_t text_length = 0;
    if (!copy_user_string(regs->rdi, text, sizeof(text), &text_length, true))
        return -static_cast<int64_t>(kErrFault);
    if (!framebuffer_font_valid(font))
        return -static_cast<int64_t>(kErrInvalid);
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);

    const uint32_t foreground = with_color
        ? static_cast<uint32_t>(regs->rsi) : UINT32_C(0xffffffff);
    print_fb_font(text, foreground, font);
    return static_cast<int64_t>(text_length - 1);
}

int64_t framebuffer_scroll_up_syscall(const syscall_regs_t *regs) {
    int32_t x = 0, y = 0, width = 0, height = 0;
    if (!decode_i32(regs->rdi, &x) || !decode_i32(regs->rsi, &y) ||
        !decode_i32(regs->rdx, &width) || !decode_i32(regs->r10, &height) ||
        !framebuffer_region_valid(width, height) || regs->r8 == 0 ||
        regs->r8 > static_cast<uint32_t>(height) || regs->r9 > UINT32_MAX)
        return -static_cast<int64_t>(kErrInvalid);
    if (static_cast<uint64_t>(width) * static_cast<uint64_t>(height) >
        kFramebufferMaxPixelsPerCall)
        return -static_cast<int64_t>(kErrTooBig);
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);
    fb_scroll_up_region(x, y, width, height, static_cast<int32_t>(regs->r8),
                        static_cast<uint32_t>(regs->r9));
    return 0;
}

int64_t framebuffer_print_cursor_get_syscall(const syscall_regs_t *regs) {
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);
    const welovec_fb_text_cursor cursor{fb_get_cursor_x(), fb_get_cursor_y()};
    return welovec::process::copy_to_current_user(
        regs->rdi, &cursor, sizeof(cursor))
        ? 0 : -static_cast<int64_t>(kErrFault);
}

int64_t framebuffer_print_cursor_set_syscall(const syscall_regs_t *regs) {
    int32_t x = 0, y = 0;
    if (!decode_i32(regs->rdi, &x) || !decode_i32(regs->rsi, &y))
        return -static_cast<int64_t>(kErrInvalid);
    uint32_t screen_width = 0, screen_height = 0;
    if (!framebuffer_ready() ||
        !framebuffer_get_resolution(&screen_width, &screen_height))
        return -static_cast<int64_t>(kErrNoDevice);
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= screen_width ||
        static_cast<uint32_t>(y) >= screen_height)
        return -static_cast<int64_t>(kErrInvalid);
    fb_set_cursor(x, y);
    return 0;
}

int64_t framebuffer_print_scrollback_syscall(const syscall_regs_t *regs) {
    int32_t direction = 0;
    if (!decode_i32(regs->rdi, &direction) || direction < -1 || direction > 1)
        return -static_cast<int64_t>(kErrInvalid);
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);
    return fb_scrollback_page(direction) ? 1 : 0;
}

int64_t input_poll_syscall(const syscall_regs_t *regs) {
    constexpr uint64_t maximum_capacity = 256;
    if (!regs->rdi || !regs->rsi || regs->rsi > maximum_capacity)
        return -static_cast<int64_t>(kErrInvalid);
    char buffer[maximum_capacity];
    const int64_t count = welovec::process::current_has_window_input()
        ? welovec::process::read_current_window_input(buffer, regs->rsi, false)
        : welovec_keyboard_read(buffer, regs->rsi, 0);
    if (count < 0) return -static_cast<int64_t>(kErrIo);
    if (count && !welovec::process::copy_to_current_user(
                     regs->rdi, buffer, static_cast<uint64_t>(count)))
        return -static_cast<int64_t>(kErrFault);
    return count;
}

int64_t pointer_poll_syscall(const syscall_regs_t *regs) {
    constexpr uint64_t maximum_capacity = 32;
    if (!regs->rdi || !regs->rsi || regs->rsi > maximum_capacity)
        return -static_cast<int64_t>(kErrInvalid);
    welovec_pointer_event events[maximum_capacity]{};
    const int64_t count = welovec::process::current_has_window_input()
        ? welovec::process::poll_current_window_pointer(events, regs->rsi)
        : welovec::x86_64::input::pointer::read_events(events, regs->rsi);
    if (count < 0) return -static_cast<int64_t>(kErrIo);
    const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(events[0]);
    if (bytes && !welovec::process::copy_to_current_user(regs->rdi, events, bytes))
        return -static_cast<int64_t>(kErrFault);
    return count;
}

int64_t framebuffer_surface_blit_syscall(const syscall_regs_t *regs) {
    int32_t dest_x = 0, dest_y = 0, source_x = 0, source_y = 0;
    if (!regs->rdi || !decode_i32(regs->rsi, &dest_x) ||
        !decode_i32(regs->rdx, &dest_y) ||
        !decode_i32(regs->r10, &source_x) ||
        !decode_i32(regs->r8, &source_y))
        return -static_cast<int64_t>(kErrInvalid);
    const uint32_t blit_width = static_cast<uint32_t>(regs->r9 >> 32);
    const uint32_t blit_height = static_cast<uint32_t>(regs->r9);
    if (!blit_width || !blit_height || blit_width > INT32_MAX ||
        blit_height > INT32_MAX)
        return -static_cast<int64_t>(kErrInvalid);
    if (!framebuffer_ready()) return -static_cast<int64_t>(kErrNoDevice);
    WelovecFramebufferSurface *surface = nullptr;
    if (!welovec::process::child_framebuffer_surface(
            welovec::process::current_id(), regs->rdi, &surface))
        return -static_cast<int64_t>(kErrPermission);
    const bool copied = framebuffer_surface_blit(
        surface, dest_x, dest_y, source_x, source_y,
        static_cast<int32_t>(blit_width), static_cast<int32_t>(blit_height));
    welovec::process::release_child_framebuffer_surface(regs->rdi);
    return copied ? 0 : -static_cast<int64_t>(kErrInvalid);
}

int64_t framebuffer_surface_resize_syscall(const syscall_regs_t *regs) {
    if (!regs->rdi || regs->rsi > UINT32_MAX || regs->rdx > UINT32_MAX ||
        !regs->rsi || !regs->rdx)
        return -static_cast<int64_t>(kErrInvalid);
    if (!welovec::process::resize_child_framebuffer_surface(
            welovec::process::current_id(), regs->rdi,
            static_cast<uint32_t>(regs->rsi), static_cast<uint32_t>(regs->rdx)))
        return -static_cast<int64_t>(kErrNoMemory);
    return 0;
}

int64_t framebuffer_surface_generation_syscall(const syscall_regs_t *regs) {
    if (!regs->rdi) return -static_cast<int64_t>(kErrInvalid);
    WelovecFramebufferSurface *surface = nullptr;
    if (!welovec::process::child_framebuffer_surface(
            welovec::process::current_id(), regs->rdi, &surface))
        return -static_cast<int64_t>(kErrPermission);
    const uint64_t generation = framebuffer_surface_generation(surface);
    welovec::process::release_child_framebuffer_surface(regs->rdi);
    return static_cast<int64_t>(generation);
}

int64_t input_forward_syscall(const syscall_regs_t *regs) {
    constexpr uint64_t maximum_characters = 255;
    constexpr uint64_t maximum_events = 15;
    if (!regs->rdi || regs->rdx > maximum_characters || regs->r8 > maximum_events ||
        (regs->rdx && !regs->rsi) || (regs->r8 && !regs->r10))
        return -static_cast<int64_t>(kErrInvalid);
    char characters[maximum_characters];
    welovec_pointer_event events[maximum_events]{};
    if ((regs->rdx && !welovec::process::copy_from_current_user(
             regs->rsi, characters, regs->rdx)) ||
        (regs->r8 && !welovec::process::copy_from_current_user(
             regs->r10, events, regs->r8 * sizeof(events[0]))))
        return -static_cast<int64_t>(kErrFault);
    return welovec::process::forward_window_input(
            welovec::process::current_id(), regs->rdi, characters,
            static_cast<uint32_t>(regs->rdx), events,
            static_cast<uint32_t>(regs->r8))
        ? 0 : -static_cast<int64_t>(kErrPermission);
}

bool valid_dns_hostname(const char *hostname, uint64_t length) {
    if (!hostname || length < 2 || length > 254) return false;
    uint32_t label_length = 0;
    bool previous_hyphen = false;
    for (uint64_t i = 0; i + 1 < length; ++i) {
        const char c = hostname[i];
        if (c == '.') {
            if (!label_length || previous_hyphen) return false;
            label_length = 0;
            previous_hyphen = false;
            continue;
        }
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool digit = c >= '0' && c <= '9';
        if (!alpha && !digit && c != '-') return false;
        if (!label_length && c == '-') return false;
        previous_hyphen = c == '-';
        if (++label_length > 63) return false;
    }
    return label_length && !previous_hyphen;
}

void reset_user_tcp_socket(uint32_t index) {
    gUserTcpSockets[index] = {};
}

bool user_tcp_socket_lookup(uint64_t owner, uint64_t handle, int32_t *out_id) {
    if (!owner || !out_id || handle == 0 || handle > kTcpUserSocketCount)
        return false;
    const uint64_t irq = gUserTcpSocketLock.lock_irqsave();
    const UserTcpSocket socket = gUserTcpSockets[handle - 1];
    gUserTcpSocketLock.unlock_irqrestore(irq);
    if (socket.owner_process != owner || socket.transport_id < 0) return false;
    *out_id = socket.transport_id;
    return true;
}

void reap_dead_user_tcp_sockets() {
    for (uint32_t index = 0; index < kTcpUserSocketCount; ++index) {
        const uint64_t irq = gUserTcpSocketLock.lock_irqsave();
        const UserTcpSocket candidate = gUserTcpSockets[index];
        gUserTcpSocketLock.unlock_irqrestore(irq);
        if (!candidate.owner_process) continue;

        welovec::process::Information owner{};
        if (welovec::process::query(candidate.owner_process, &owner) &&
            owner.state == welovec::process::State::Active) continue;

        int32_t transport_id = -1;
        const uint64_t clear_irq = gUserTcpSocketLock.lock_irqsave();
        if (gUserTcpSockets[index].owner_process == candidate.owner_process &&
            gUserTcpSockets[index].transport_id == candidate.transport_id) {
            transport_id = candidate.transport_id;
            reset_user_tcp_socket(index);
        }
        gUserTcpSocketLock.unlock_irqrestore(clear_irq);
        if (transport_id >= 0) tcp_close(transport_id);
    }
}

int64_t network_info_syscall(const syscall_regs_t *regs) {
    netstack_info source{};
    const uint64_t network_flags = welovec_network_api_lock();
    netstack_get_info(&source);
    welovec_network_api_unlock(network_flags);
    if (!source.initialized) return -static_cast<int64_t>(kErrNoDevice);

    welovec_network_info info{};
    info.flags = WELOVEC_NET_INFO_INITIALIZED;
    if (source.ipv4_configured) info.flags |= WELOVEC_NET_INFO_IPV4_READY;
    if (source.using_dhcp) info.flags |= WELOVEC_NET_INFO_DHCP;
    info.dhcp_phase = source.dhcp_phase;
    for (uint32_t i = 0; i < 6; ++i) info.mac[i] = source.mac[i];
    for (uint32_t i = 0; i < 4; ++i) {
        info.ipv4[i] = source.ip[i];
        info.subnet_mask[i] = source.subnet_mask[i];
        info.gateway[i] = source.gateway[i];
        info.dns_server[i] = source.dns_server[i];
    }
    info.arp_packets_received = source.arp_packets_received;
    info.ipv4_packets_received = source.ipv4_packets_received;
    info.ipv4_packets_sent = source.ipv4_packets_sent;
    info.icmp_echo_requests_sent = source.icmp_echo_requests_sent;
    info.icmp_echo_replies_received = source.icmp_echo_replies_received;
    return welovec::process::copy_to_current_user(
        regs->rdi, &info, sizeof(info))
        ? 0 : -static_cast<int64_t>(kErrFault);
}

int64_t network_ping_syscall(const syscall_regs_t *regs) {
    uint8_t target[4];
    if (!welovec::process::copy_from_current_user(regs->rdi, target, sizeof(target)))
        return -static_cast<int64_t>(kErrFault);
    const uint64_t network_flags = welovec_network_api_lock();
    netstack_info info{};
    netstack_get_info(&info);
    const bool configured = info.initialized && info.ipv4_configured;
    const bool replied = configured && netstack_ping(target);
    welovec_network_api_unlock(network_flags);
    if (!configured) return -static_cast<int64_t>(kErrNetworkUnreachable);
    return replied ? 0 : -static_cast<int64_t>(kErrTimedOut);
}

int64_t network_dns_syscall(const syscall_regs_t *regs) {
    char hostname[254];
    uint64_t length = 0;
    if (!copy_user_string(regs->rdi, hostname, sizeof(hostname), &length))
        return -static_cast<int64_t>(kErrFault);
    if (!valid_dns_hostname(hostname, length))
        return -static_cast<int64_t>(kErrInvalid);
    uint8_t address[4];
    const uint64_t network_flags = welovec_network_api_lock();
    netstack_info info{};
    netstack_get_info(&info);
    const bool configured = info.initialized && info.ipv4_configured;
    const bool resolved = configured && netstack_dns_resolve(hostname, address);
    welovec_network_api_unlock(network_flags);
    if (!configured) return -static_cast<int64_t>(kErrNetworkUnreachable);
    if (!resolved) return -static_cast<int64_t>(kErrNotFound);
    return welovec::process::copy_to_current_user(
        regs->rsi, address, sizeof(address))
        ? 0 : -static_cast<int64_t>(kErrFault);
}

int64_t network_ipv6_info_syscall(const syscall_regs_t *regs) {
    netstack_ipv6_info source{};
    const uint64_t network_flags = welovec_network_api_lock();
    netstack_ipv6_get_info(&source);
    welovec_network_api_unlock(network_flags);
    if (!source.configured) return -static_cast<int64_t>(kErrNoDevice);
    welovec_network_ipv6_info info{};
    info.flags = WELOVEC_NET_IPV6_CONFIGURED;
    if (source.has_global_address) info.flags |= WELOVEC_NET_IPV6_GLOBAL_READY;
    if (source.has_default_router) info.flags |= WELOVEC_NET_IPV6_ROUTER_READY;
    for (uint32_t i = 0; i < 16; ++i) {
        info.link_local[i] = source.link_local[i];
        info.global_address[i] = source.global_address[i];
        info.prefix[i] = source.prefix[i];
        info.default_router[i] = source.default_router[i];
    }
    return welovec::process::copy_to_current_user(regs->rdi, &info, sizeof(info))
        ? 0 : -static_cast<int64_t>(kErrFault);
}

int64_t network_dns_ipv6_syscall(const syscall_regs_t *regs) {
    char hostname[254];
    uint64_t length = 0;
    if (!copy_user_string(regs->rdi, hostname, sizeof(hostname), &length))
        return -static_cast<int64_t>(kErrFault);
    if (!valid_dns_hostname(hostname, length))
        return -static_cast<int64_t>(kErrInvalid);
    uint8_t address[16];
    const uint64_t network_flags = welovec_network_api_lock();
    netstack_info info{};
    netstack_get_info(&info);
    const bool configured = info.initialized && info.ipv4_configured;
    const bool resolved = configured && netstack_dns_resolve_ipv6(hostname, address);
    welovec_network_api_unlock(network_flags);
    if (!configured) return -static_cast<int64_t>(kErrNetworkUnreachable);
    if (!resolved) return -static_cast<int64_t>(kErrNotFound);
    return welovec::process::copy_to_current_user(regs->rsi, address, sizeof(address))
        ? 0 : -static_cast<int64_t>(kErrFault);
}

int64_t network_tcp_connect_syscall(const syscall_regs_t *regs) {
    if (regs->rsi == 0 || regs->rsi > UINT16_MAX)
        return -static_cast<int64_t>(kErrInvalid);
    const uint64_t owner = welovec::process::current_id();
    if (!owner) return -static_cast<int64_t>(kErrPermission);
    uint8_t address[4];
    if (!welovec::process::copy_from_current_user(regs->rdi, address, sizeof(address)))
        return -static_cast<int64_t>(kErrFault);

    const uint64_t network_flags = welovec_network_api_lock();
    reap_dead_user_tcp_sockets();
    int slot = -1;
    const uint64_t irq = gUserTcpSocketLock.lock_irqsave();
    for (uint32_t i = 0; i < kTcpUserSocketCount; ++i) {
        if (!gUserTcpSockets[i].owner_process) {
            slot = static_cast<int>(i);
            gUserTcpSockets[i] = {owner, -2}; // Reserved during connect.
            break;
        }
    }
    gUserTcpSocketLock.unlock_irqrestore(irq);
    if (slot < 0) {
        welovec_network_api_unlock(network_flags);
        return -static_cast<int64_t>(kErrNoFileDescriptor);
    }

    netstack_info info{};
    netstack_get_info(&info);
    const int transport_id = info.initialized && info.ipv4_configured
        ? tcp_connect(address, static_cast<uint16_t>(regs->rsi)) : -1;
    const uint64_t finish_irq = gUserTcpSocketLock.lock_irqsave();
    if (transport_id >= 0) gUserTcpSockets[slot].transport_id = transport_id;
    else reset_user_tcp_socket(static_cast<uint32_t>(slot));
    gUserTcpSocketLock.unlock_irqrestore(finish_irq);
    welovec_network_api_unlock(network_flags);
    if (transport_id < 0)
        return info.initialized && info.ipv4_configured
            ? -static_cast<int64_t>(kErrTimedOut)
            : -static_cast<int64_t>(kErrNetworkUnreachable);
    return slot + 1;
}

int64_t network_tcp_connect_ipv6_syscall(const syscall_regs_t *regs) {
    if (regs->rsi == 0 || regs->rsi > UINT16_MAX)
        return -static_cast<int64_t>(kErrInvalid);
    const uint64_t owner = welovec::process::current_id();
    if (!owner) return -static_cast<int64_t>(kErrPermission);
    uint8_t address[16];
    if (!welovec::process::copy_from_current_user(regs->rdi, address, sizeof(address)))
        return -static_cast<int64_t>(kErrFault);

    const uint64_t network_flags = welovec_network_api_lock();
    reap_dead_user_tcp_sockets();
    int slot = -1;
    const uint64_t irq = gUserTcpSocketLock.lock_irqsave();
    for (uint32_t i = 0; i < kTcpUserSocketCount; ++i) {
        if (!gUserTcpSockets[i].owner_process) {
            slot = static_cast<int>(i);
            gUserTcpSockets[i] = {owner, -2};
            break;
        }
    }
    gUserTcpSocketLock.unlock_irqrestore(irq);
    if (slot < 0) {
        welovec_network_api_unlock(network_flags);
        return -static_cast<int64_t>(kErrNoFileDescriptor);
    }

    netstack_ipv6_info info{};
    netstack_ipv6_get_info(&info);
    const int transport_id = info.configured
        ? tcp_connect_ipv6(address, static_cast<uint16_t>(regs->rsi)) : -1;
    const uint64_t finish_irq = gUserTcpSocketLock.lock_irqsave();
    if (transport_id >= 0) gUserTcpSockets[slot].transport_id = transport_id;
    else reset_user_tcp_socket(static_cast<uint32_t>(slot));
    gUserTcpSocketLock.unlock_irqrestore(finish_irq);
    welovec_network_api_unlock(network_flags);
    if (transport_id < 0)
        return info.configured ? -static_cast<int64_t>(kErrTimedOut)
                               : -static_cast<int64_t>(kErrNetworkUnreachable);
    return slot + 1;
}

int64_t network_tcp_send_syscall(const syscall_regs_t *regs) {
    if (regs->rdx > kTcpUserIoLimit)
        return -static_cast<int64_t>(kErrTooBig);
    const uint64_t owner = welovec::process::current_id();
    if (!owner) return -static_cast<int64_t>(kErrPermission);
    int32_t transport_id = -1;
    uint8_t data[kTcpUserIoLimit];
    if (regs->rdx && !welovec::process::copy_from_current_user(
            regs->rsi, data, regs->rdx))
        return -static_cast<int64_t>(kErrFault);

    const uint64_t network_flags = welovec_network_api_lock();
    reap_dead_user_tcp_sockets();
    if (!user_tcp_socket_lookup(owner, regs->rdi, &transport_id)) {
        welovec_network_api_unlock(network_flags);
        return -static_cast<int64_t>(kErrBadDescriptor);
    }
    const bool sent = tcp_send(transport_id, data, static_cast<uint16_t>(regs->rdx));
    welovec_network_api_unlock(network_flags);
    return sent ? static_cast<int64_t>(regs->rdx)
                : -static_cast<int64_t>(kErrIo);
}

int64_t network_tcp_receive_syscall(const syscall_regs_t *regs) {
    if (regs->rdx > kTcpUserIoLimit)
        return -static_cast<int64_t>(kErrTooBig);
    const uint64_t owner = welovec::process::current_id();
    if (!owner) return -static_cast<int64_t>(kErrPermission);
    int32_t transport_id = -1;
    uint8_t data[kTcpUserIoLimit];
    const uint64_t network_flags = welovec_network_api_lock();
    reap_dead_user_tcp_sockets();
    if (!user_tcp_socket_lookup(owner, regs->rdi, &transport_id)) {
        welovec_network_api_unlock(network_flags);
        return -static_cast<int64_t>(kErrBadDescriptor);
    }
    const int received = regs->rdx
        ? tcp_recv(transport_id, data, static_cast<int>(regs->rdx), 0) : 0;
    welovec_network_api_unlock(network_flags);
    if (received > 0 && !welovec::process::copy_to_current_user(
            regs->rsi, data, static_cast<uint64_t>(received)))
        return -static_cast<int64_t>(kErrFault);
    return received;
}

int64_t network_tcp_close_syscall(const syscall_regs_t *regs) {
    const uint64_t owner = welovec::process::current_id();
    if (!owner) return -static_cast<int64_t>(kErrPermission);
    const uint64_t network_flags = welovec_network_api_lock();
    reap_dead_user_tcp_sockets();
    int32_t transport_id = -1;
    if (!user_tcp_socket_lookup(owner, regs->rdi, &transport_id)) {
        welovec_network_api_unlock(network_flags);
        return -static_cast<int64_t>(kErrBadDescriptor);
    }
    const uint64_t irq = gUserTcpSocketLock.lock_irqsave();
    if (regs->rdi <= kTcpUserSocketCount &&
        gUserTcpSockets[regs->rdi - 1].owner_process == owner &&
        gUserTcpSockets[regs->rdi - 1].transport_id == transport_id)
        reset_user_tcp_socket(static_cast<uint32_t>(regs->rdi - 1));
    gUserTcpSocketLock.unlock_irqrestore(irq);
    tcp_close(transport_id);
    welovec_network_api_unlock(network_flags);
    return 0;
}

bool copy_user_string(uint64_t source, char *destination, uint64_t capacity,
                      uint64_t *out_length, bool allow_empty) {
    if (!source || !destination || !capacity || !out_length) return false;
    for (uint64_t index = 0; index < capacity; ++index) {
        if (source > UINT64_MAX - index ||
            !welovec::process::copy_from_current_user(source + index,
                                                       &destination[index], 1)) return false;
        if (!destination[index]) {
            if (!index && !allow_empty) return false;
            *out_length = index + 1;
            return true;
        }
    }
    return false;
}

void clear_sensitive_buffer(char *buffer, uint64_t capacity) {
    volatile char *bytes = buffer;
    while (capacity--) *bytes++ = 0;
}

enum class ExecMode { Inherit, Admin, User, Windowed };

int64_t execute_user_program(const syscall_regs_t *regs, ExecMode mode) {
    if (mode == ExecMode::Admin && !welovec::security::current_has_capability(
                                       WELOVEC_CAP_EXEC_AS_ADMIN))
        return -static_cast<int64_t>(kErrPermission);
    if (mode == ExecMode::User && !welovec::security::current_has_capability(
                                      WELOVEC_CAP_MANAGE_USERS))
        return -static_cast<int64_t>(kErrPermission);
    if (!welovec::process::current_id()) return -static_cast<int64_t>(kErrPermission);
    if (mode == ExecMode::User && regs->r8 > UINT32_MAX)
        return -static_cast<int64_t>(kErrInvalid);
    if (mode == ExecMode::Windowed &&
        (!regs->r8 || !regs->r9 || regs->r8 > 4096 || regs->r9 > 2160 ||
         regs->r8 * regs->r9 > UINT64_C(2) * 1024 * 1024))
        return -static_cast<int64_t>(kErrInvalid);
    if (mode == ExecMode::User) {
        welovec::security::Credentials caller{};
        welovec::security::Credentials target{};
        if (!welovec::process::current_credentials(&caller) ||
            !welovec::security::credentials_for_uid(static_cast<uint32_t>(regs->r8), &target))
            return -static_cast<int64_t>(kErrNotFound);
        const uint64_t added_capabilities = target.capabilities & ~caller.capabilities;
        if (added_capabilities &&
            !welovec::security::has_capability(caller, WELOVEC_CAP_EXEC_AS_ADMIN))
            return -static_cast<int64_t>(kErrPermission);
    }

    const uint32_t argument_count = static_cast<uint32_t>(regs->rdx);
    if (regs->rdx > kMaximumExecArguments || regs->r10 > 3 ||
        (argument_count && !regs->rsi))
        return -static_cast<int64_t>(argument_count > kMaximumExecArguments
                                         ? kErrTooManyArguments : kErrInvalid);
    const uint8_t priority = static_cast<uint8_t>(regs->r10);

    char path[kExecPathCapacity];
    uint64_t path_length = 0;
    if (!copy_user_string(regs->rdi, path, sizeof(path), &path_length))
        return -static_cast<int64_t>(kErrFault);

    char *argument_storage = nullptr;
    const char *arguments[kMaximumExecArguments]{};
    uint32_t used_argument_bytes = 0;
    if (argument_count) {
        argument_storage = static_cast<char *>(
            welovec::memory::heap::allocate(kExecArgumentBytes));
        if (!argument_storage) return -static_cast<int64_t>(kErrNoMemory);

        for (uint32_t index = 0; index < argument_count; ++index) {
            if (regs->rsi > UINT64_MAX - static_cast<uint64_t>(index) * sizeof(uint64_t)) {
                welovec::memory::heap::deallocate(argument_storage);
                return -static_cast<int64_t>(kErrFault);
            }
            uint64_t user_argument = 0;
            if (!welovec::process::copy_from_current_user(
                    regs->rsi + static_cast<uint64_t>(index) * sizeof(uint64_t),
                    &user_argument, sizeof(user_argument)) ||
                used_argument_bytes >= kExecArgumentBytes) {
                welovec::memory::heap::deallocate(argument_storage);
                return -static_cast<int64_t>(kErrFault);
            }

            uint64_t copied = 0;
            char *argument = argument_storage + used_argument_bytes;
            if (!copy_user_string(user_argument, argument,
                                  kExecArgumentBytes - used_argument_bytes, &copied, true)) {
                welovec::memory::heap::deallocate(argument_storage);
                return -static_cast<int64_t>(kErrFault);
            }
            arguments[index] = argument;
            used_argument_bytes += static_cast<uint32_t>(copied);
        }
    }

    (void)path_length;
    const welovec_exec_options options{
        argument_count,
        argument_count ? arguments : nullptr,
        priority,
        mode == ExecMode::Windowed ? static_cast<uint32_t>(regs->r8) : 0,
        mode == ExecMode::Windowed ? static_cast<uint32_t>(regs->r9) : 0
    };
    welovec_exec_result result{};
    welovec_exec_error error;
    if (mode == ExecMode::Admin) {
        error = welovec_exec_launch_elf64_as_admin(path, &options, &result);
    } else if (mode == ExecMode::User) {
        error = welovec_exec_launch_elf64_as_uid(
            path, &options, &result, static_cast<uint32_t>(regs->r8));
    } else {
        error = welovec_exec_launch_elf64_as_current(path, &options, &result);
    }
    if (argument_storage) welovec::memory::heap::deallocate(argument_storage);
    if (error != WELOVEC_EXEC_SUCCESS)
        return -100 - static_cast<int64_t>(error);
    return static_cast<int64_t>(result.process_id);
}

bool copy_user_path(uint64_t source, char *path) {
    char input[MAX_FILENAME];
    uint64_t length = 0;
    if (!copy_user_string(source, input, sizeof(input), &length)) return false;
    (void)length;
    vfs_resolve_path("/", input, path);
    return path[0] == '/';
}

bool path_is_within(const char *path, const char *directory) {
    uint32_t i = 0;
    while (directory[i] && path[i] == directory[i]) ++i;
    return directory[i] == '\0' && (path[i] == '\0' || path[i] == '/');
}

bool is_account_database_path(const char *path) {
    return path_is_within(path, "/userspace/system/security0.db") ||
           path_is_within(path, "/userspace/system/security1.db");
}

bool user_can_write_path(const char *path) {
    if (is_account_database_path(path)) return false;
    /* The kernel temporary directory is shared and writable by guest. */
    if (path_is_within(path, "/tmp")) return true;
    if (welovec::security::current_has_capability(WELOVEC_CAP_SYSTEM_CONFIG)) return true;
    welovec_user_status status{};
    if (!welovec::security::current_user_status(&status) || !status.authenticated)
        return false;
    char home[WELOVEC_USER_NAME_MAX + 7] = "/home/";
    uint32_t index = 6;
    while (index + 1 < sizeof(home) && status.name[index - 6]) {
        home[index] = status.name[index - 6];
        ++index;
    }
    home[index] = '\0';
    return path_is_within(path, home);
}

bool user_can_read_path(const char *path) {
    if (is_account_database_path(path)) return false;
    if (path_is_within(path, "/userspace/system") &&
        !welovec::security::current_has_capability(WELOVEC_CAP_SYSTEM_CONFIG))
        return false;
    if (path_is_within(path, "/home")) {
        if (welovec::security::current_has_capability(WELOVEC_CAP_SYSTEM_CONFIG))
            return true;
        welovec_user_status status{};
        if (!welovec::security::current_user_status(&status) || !status.authenticated)
            return false;
        char home[WELOVEC_USER_NAME_MAX + 7] = "/home/";
        uint32_t index = 6;
        while (index + 1 < sizeof(home) && status.name[index - 6]) {
            home[index] = status.name[index - 6];
            ++index;
        }
        home[index] = '\0';
        return path_is_within(path, home);
    }
    return true;
}

int64_t fs_exists_syscall(const syscall_regs_t *regs) {
    char path[MAX_FILENAME];
    if (!copy_user_path(regs->rdi, path)) return -static_cast<int64_t>(kErrFault);
    if (!user_can_read_path(path)) return -static_cast<int64_t>(kErrPermission);
    return file_exists(path) ? 1 : 0;
}

int64_t fs_stat_syscall(const syscall_regs_t *regs) {
    char path[MAX_FILENAME];
    if (!copy_user_path(regs->rdi, path)) return -static_cast<int64_t>(kErrFault);
    if (!user_can_read_path(path)) return -static_cast<int64_t>(kErrPermission);
    if (!file_exists(path)) return -static_cast<int64_t>(kErrNotFound);

    welovec_user_file_info info{};
    info.type = fs_is_directory(path) ? WELOVEC_USER_FS_DIRECTORY : WELOVEC_USER_FS_FILE;
    info.size = info.type == WELOVEC_USER_FS_FILE ? fs_file_size(path) : 0;
    if (!welovec::process::copy_to_current_user(regs->rsi, &info, sizeof(info)))
        return -static_cast<int64_t>(kErrFault);
    return 0;
}

int64_t fs_read_syscall(const syscall_regs_t *regs) {
    char path[MAX_FILENAME];
    if (!copy_user_path(regs->rdi, path)) return -static_cast<int64_t>(kErrFault);
    if (!user_can_read_path(path)) return -static_cast<int64_t>(kErrPermission);
    if (!file_exists(path)) return -static_cast<int64_t>(kErrNotFound);
    if (fs_is_directory(path)) return -static_cast<int64_t>(kErrIsDirectory);

    uint8_t *data = nullptr;
    uint32_t size = 0;
    if (!vfs_read_file(path, &data, &size) || !data)
        return -static_cast<int64_t>(kErrIo);
    if (size > kFileIoLimit) {
        vfs_free_file(data);
        return -static_cast<int64_t>(kErrFileTooLarge);
    }
    if (size > regs->rdx) {
        vfs_free_file(data);
        return -static_cast<int64_t>(kErrNoSpace);
    }
    if (size && !welovec::process::copy_to_current_user(regs->rsi, data, size)) {
        vfs_free_file(data);
        return -static_cast<int64_t>(kErrFault);
    }
    vfs_free_file(data);
    return size;
}

int64_t fs_write_syscall(const syscall_regs_t *regs) {
    char path[MAX_FILENAME];
    if (!copy_user_path(regs->rdi, path)) return -static_cast<int64_t>(kErrFault);
    if (!user_can_read_path(path)) return -static_cast<int64_t>(kErrPermission);
    if (!user_can_write_path(path)) return -static_cast<int64_t>(kErrPermission);
    if (regs->rdx > kFileIoLimit || regs->rdx > UINT32_MAX)
        return -static_cast<int64_t>(kErrFileTooLarge);
    if (fs_is_directory(path)) return -static_cast<int64_t>(kErrIsDirectory);

    uint8_t *data = nullptr;
    if (regs->rdx) {
        data = static_cast<uint8_t *>(welovec::memory::heap::allocate(regs->rdx));
        if (!data) return -static_cast<int64_t>(kErrNoMemory);
        if (!welovec::process::copy_from_current_user(regs->rsi, data, regs->rdx)) {
            welovec::memory::heap::deallocate(data);
            return -static_cast<int64_t>(kErrFault);
        }
    }
    const bool written = vfs_write_file(path, data, static_cast<uint32_t>(regs->rdx));
    if (data) welovec::memory::heap::deallocate(data);
    return written ? static_cast<int64_t>(regs->rdx) : -static_cast<int64_t>(kErrIo);
}

int64_t fs_list_syscall(const syscall_regs_t *regs) {
    char path[MAX_FILENAME];
    if (!copy_user_path(regs->rdi, path)) return -static_cast<int64_t>(kErrFault);
    if (!user_can_read_path(path)) return -static_cast<int64_t>(kErrPermission);
    if (!file_exists(path)) return -static_cast<int64_t>(kErrNotFound);
    if (!fs_is_directory(path)) return -static_cast<int64_t>(kErrNotDirectory);
    if (regs->rdx > WELOVEC_USER_FS_MAX_ENTRIES)
        return -static_cast<int64_t>(kErrInvalid);
    if (!regs->rdx) return 0;
    if (!regs->rsi) return -static_cast<int64_t>(kErrFault);

    const uint64_t maximum = regs->rdx;
    auto *entries = static_cast<VfsListEntry *>(welovec::memory::heap::allocate(
        maximum * sizeof(VfsListEntry)));
    auto *user_entries = static_cast<welovec_user_dir_entry *>(welovec::memory::heap::allocate(
        maximum * sizeof(welovec_user_dir_entry)));
    if (!entries || !user_entries) {
        if (entries) welovec::memory::heap::deallocate(entries);
        if (user_entries) welovec::memory::heap::deallocate(user_entries);
        return -static_cast<int64_t>(kErrNoMemory);
    }

    const int count = fs_list(path, entries, static_cast<int>(maximum));
    if (count < 0 || static_cast<uint64_t>(count) > maximum) {
        welovec::memory::heap::deallocate(user_entries);
        welovec::memory::heap::deallocate(entries);
        return -static_cast<int64_t>(kErrIo);
    }
    for (int i = 0; i < count; ++i) {
        uint32_t j = 0;
        while (j + 1 < sizeof(user_entries[i].name) && entries[i].name[j]) {
            user_entries[i].name[j] = entries[i].name[j];
            ++j;
        }
        user_entries[i].name[j] = '\0';
        user_entries[i].size = entries[i].size;
        user_entries[i].type = entries[i].is_directory
            ? WELOVEC_USER_FS_DIRECTORY : WELOVEC_USER_FS_FILE;
        user_entries[i].reserved = 0;
    }
    const bool copied = !count || welovec::process::copy_to_current_user(
        regs->rsi, user_entries, static_cast<uint64_t>(count) * sizeof(*user_entries));
    welovec::memory::heap::deallocate(user_entries);
    welovec::memory::heap::deallocate(entries);
    return copied ? count : -static_cast<int64_t>(kErrFault);
}

int64_t fs_mkdir_syscall(const syscall_regs_t *regs) {
    char path[MAX_FILENAME];
    if (!copy_user_path(regs->rdi, path)) return -static_cast<int64_t>(kErrFault);
    if (!user_can_write_path(path)) return -static_cast<int64_t>(kErrPermission);
    if (make_dir(path)) return 0;
    return file_exists(path) ? -static_cast<int64_t>(kErrIo)
                             : -static_cast<int64_t>(kErrNoSpace);
}

int64_t fs_unlink_syscall(const syscall_regs_t *regs) {
    char path[MAX_FILENAME];
    if (!copy_user_path(regs->rdi, path)) return -static_cast<int64_t>(kErrFault);
    if (!user_can_write_path(path)) return -static_cast<int64_t>(kErrPermission);
    if (!file_exists(path)) return -static_cast<int64_t>(kErrNotFound);
    if (fs_is_directory(path)) {
        VfsListEntry child{};
        if (fs_list(path, &child, 1) > 0)
            return -static_cast<int64_t>(kErrDirectoryNotEmpty);
    }
    return delete_file(path) ? 0 : -static_cast<int64_t>(kErrIo);
}

int64_t fs_move_syscall(const syscall_regs_t *regs) {
    char source[MAX_FILENAME], destination[MAX_FILENAME];
    if (!copy_user_path(regs->rdi, source) ||
        !copy_user_path(regs->rsi, destination))
        return -static_cast<int64_t>(kErrFault);
    if (!user_can_write_path(source) || !user_can_write_path(destination))
        return -static_cast<int64_t>(kErrPermission);
    if (!file_exists(source)) return -static_cast<int64_t>(kErrNotFound);
    if (file_exists(destination)) return -static_cast<int64_t>(kErrExist);
    return move_file(source, destination)
        ? 0 : -static_cast<int64_t>(kErrIo);
}

int64_t console_write_user(uint64_t user_buffer, uint64_t size) {
    if (size > kWriteLimit) return -static_cast<int64_t>(kErrInvalid);
    char buffer[kWriteChunk];
    uint64_t written = 0;
    while (written < size) {
        uint64_t count = size - written;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        if (user_buffer > UINT64_MAX - written ||
            !welovec::process::copy_from_current_user(user_buffer + written, buffer, count))
            return written ? static_cast<int64_t>(written)
                           : -static_cast<int64_t>(kErrFault);
        const bool display_ready = framebuffer_available();
        for (uint64_t i = 0; i < count; ++i) {
            if (buffer[i] == '\n') serial_write_char('\r');
            serial_write_char(buffer[i]);
            if (display_ready) putchar_fb(buffer[i]);
        }
        written += count;
    }
    return static_cast<int64_t>(written);
}

int64_t fd_open_syscall(const syscall_regs_t *regs) {
    const uint64_t process_id = welovec::process::current_id();
    if (!process_id) return -static_cast<int64_t>(kErrPermission);
    char path[MAX_FILENAME];
    if (!copy_user_path(regs->rdi, path)) return -static_cast<int64_t>(kErrFault);
    const uint32_t flags = static_cast<uint32_t>(regs->rsi);
    if (regs->rsi > UINT32_MAX || (flags & ~kKnownOpenFlags) ||
        !(flags & (welovec::filesystem::kOpenRead | welovec::filesystem::kOpenWrite)) ||
        ((flags & welovec::filesystem::kOpenTruncate) &&
         !(flags & welovec::filesystem::kOpenWrite)) ||
        ((flags & welovec::filesystem::kOpenAppend) &&
         !(flags & welovec::filesystem::kOpenWrite)))
        return -static_cast<int64_t>(kErrInvalid);
    if ((flags & welovec::filesystem::kOpenWrite) && !user_can_write_path(path))
        return -static_cast<int64_t>(kErrPermission);
    if ((flags & welovec::filesystem::kOpenRead) && !user_can_read_path(path))
        return -static_cast<int64_t>(kErrPermission);

    bool exists = file_exists(path);
    if (!exists && (flags & welovec::filesystem::kOpenCreate)) {
        if (!vfs_write_file(path, nullptr, 0)) return -static_cast<int64_t>(kErrIo);
        exists = true;
    }
    if (!exists) return -static_cast<int64_t>(kErrNotFound);
    if (fs_is_directory(path)) return -static_cast<int64_t>(kErrIsDirectory);
    if ((flags & welovec::filesystem::kOpenTruncate) &&
        !vfs_write_file(path, nullptr, 0)) return -static_cast<int64_t>(kErrIo);

    const int32_t descriptor = welovec::filesystem::open_descriptor(process_id, path, flags);
    if (descriptor < 0) return -static_cast<int64_t>(kErrNoMemory);
    if (flags & welovec::filesystem::kOpenAppend)
        (void)welovec::filesystem::set_descriptor_offset(
            process_id, descriptor, fs_file_size(path));
    return descriptor;
}

int64_t fd_close_syscall(const syscall_regs_t *regs) {
    if (regs->rdi > INT32_MAX) return -static_cast<int64_t>(kErrBadDescriptor);
    return welovec::filesystem::close_descriptor(
        welovec::process::current_id(), static_cast<int32_t>(regs->rdi))
        ? 0 : -static_cast<int64_t>(kErrBadDescriptor);
}

int64_t fd_read_syscall(const syscall_regs_t *regs) {
    if (regs->rdi > INT32_MAX || regs->rdx > kFileIoLimit)
        return -static_cast<int64_t>(kErrInvalid);
    const int32_t descriptor = static_cast<int32_t>(regs->rdi);
    if (descriptor == 0) {
        if (!welovec_keyboard_available()) return -static_cast<int64_t>(kErrNoDevice);
        if (!regs->rdx) return 0;
        if (regs->rdx > UINT32_MAX) return -static_cast<int64_t>(kErrInvalid);
        auto *buffer = static_cast<char *>(welovec::memory::heap::allocate(regs->rdx));
        if (!buffer && regs->rdx) return -static_cast<int64_t>(kErrNoMemory);
        const int64_t read = welovec::process::current_has_window_input()
            ? welovec::process::read_current_window_input(
                buffer, regs->rdx, regs->rdx ? true : false)
            : welovec_keyboard_read(buffer, regs->rdx, regs->rdx ? 1 : 0);
        if (read > 0 && !welovec::process::copy_to_current_user(
                regs->rsi, buffer, static_cast<uint64_t>(read))) {
            welovec::memory::heap::deallocate(buffer);
            return -static_cast<int64_t>(kErrFault);
        }
        if (buffer) welovec::memory::heap::deallocate(buffer);
        return read < 0 ? -static_cast<int64_t>(kErrIo) : read;
    }
    if (descriptor < 3) return -static_cast<int64_t>(kErrBadDescriptor);

    welovec::filesystem::DescriptorInfo info{};
    const uint64_t process_id = welovec::process::current_id();
    if (!welovec::filesystem::get_descriptor(process_id, descriptor, &info) ||
        !(info.flags & welovec::filesystem::kOpenRead))
        return -static_cast<int64_t>(kErrBadDescriptor);
    if (!user_can_read_path(info.path)) return -static_cast<int64_t>(kErrPermission);
    if (!regs->rdx) return 0;

    uint8_t *data = nullptr;
    uint32_t size = 0;
    if (!vfs_read_file(info.path, &data, &size) || !data)
        return -static_cast<int64_t>(kErrIo);
    if (size > kFileIoLimit) {
        vfs_free_file(data);
        return -static_cast<int64_t>(kErrFileTooLarge);
    }
    const uint64_t count = info.offset < size
        ? ((size - info.offset) < regs->rdx ? size - info.offset : regs->rdx) : 0;
    if (count && !welovec::process::copy_to_current_user(regs->rsi, data + info.offset, count)) {
        vfs_free_file(data);
        return -static_cast<int64_t>(kErrFault);
    }
    vfs_free_file(data);
    if (count) (void)welovec::filesystem::set_descriptor_offset(
        process_id, descriptor, info.offset + count);
    return static_cast<int64_t>(count);
}

int64_t fd_write_syscall(const syscall_regs_t *regs) {
    if (regs->rdi > INT32_MAX || regs->rdx > kFileIoLimit)
        return -static_cast<int64_t>(kErrInvalid);
    const int32_t descriptor = static_cast<int32_t>(regs->rdi);
    if (descriptor == 1 || descriptor == 2)
        return console_write_user(regs->rsi, regs->rdx);
    if (descriptor < 3) return -static_cast<int64_t>(kErrBadDescriptor);

    const uint64_t process_id = welovec::process::current_id();
    welovec::filesystem::DescriptorInfo info{};
    if (!welovec::filesystem::get_descriptor(process_id, descriptor, &info) ||
        !(info.flags & welovec::filesystem::kOpenWrite))
        return -static_cast<int64_t>(kErrBadDescriptor);
    if (!user_can_write_path(info.path)) return -static_cast<int64_t>(kErrPermission);
    if (!regs->rdx) return 0;
    auto *data = static_cast<uint8_t *>(welovec::memory::heap::allocate(regs->rdx));
    if (!data) return -static_cast<int64_t>(kErrNoMemory);
    if (!welovec::process::copy_from_current_user(regs->rsi, data, regs->rdx)) {
        welovec::memory::heap::deallocate(data);
        return -static_cast<int64_t>(kErrFault);
    }

    uint64_t offset = info.offset;
    if (info.flags & welovec::filesystem::kOpenAppend)
        offset = fs_file_size(info.path);
    if (regs->rdx > UINT32_MAX || offset > UINT32_MAX - regs->rdx) {
        welovec::memory::heap::deallocate(data);
        return -static_cast<int64_t>(kErrFileTooLarge);
    }
    const bool written = vfs_write_file_range(
        info.path, static_cast<uint32_t>(offset), data, static_cast<uint32_t>(regs->rdx));
    welovec::memory::heap::deallocate(data);
    if (!written) return -static_cast<int64_t>(kErrIo);
    (void)welovec::filesystem::set_descriptor_offset(
        process_id, descriptor, offset + regs->rdx);
    return static_cast<int64_t>(regs->rdx);
}

int64_t fd_seek_syscall(const syscall_regs_t *regs) {
    if (regs->rdi > INT32_MAX || regs->rdx > 2)
        return -static_cast<int64_t>(kErrInvalid);
    const uint64_t process_id = welovec::process::current_id();
    welovec::filesystem::DescriptorInfo info{};
    if (!welovec::filesystem::get_descriptor(
            process_id, static_cast<int32_t>(regs->rdi), &info))
        return -static_cast<int64_t>(kErrBadDescriptor);
    int64_t base = 0;
    if (regs->rdx == 1) base = static_cast<int64_t>(info.offset);
    if (regs->rdx == 2) base = static_cast<int64_t>(fs_file_size(info.path));
    const int64_t delta = static_cast<int64_t>(regs->rsi);
    if ((delta > 0 && base > INT64_MAX - delta) ||
        (delta < 0 && delta < -base)) return -static_cast<int64_t>(kErrInvalid);
    const int64_t target = base + delta;
    if (target < 0 || static_cast<uint64_t>(target) > kFileIoLimit)
        return -static_cast<int64_t>(kErrInvalid);
    if (!welovec::filesystem::set_descriptor_offset(
            process_id, static_cast<int32_t>(regs->rdi), static_cast<uint64_t>(target)))
        return -static_cast<int64_t>(kErrBadDescriptor);
    return target;
}

bool process_access_allowed(uint64_t caller, const welovec::process::Information &target) {
    if (welovec::security::current_has_capability(WELOVEC_CAP_MANAGE_PROCESSES)) return true;
    return target.id == caller || target.parent_id == caller;
}

int64_t process_query_syscall(const syscall_regs_t *regs) {
    if (!regs->rdi) return -static_cast<int64_t>(kErrInvalid);
    welovec::process::Information info{};
    if (!welovec::process::query(regs->rdi, &info))
        return -static_cast<int64_t>(kErrNotFound);
    if (!process_access_allowed(welovec::process::current_id(), info))
        return -static_cast<int64_t>(kErrPermission);
    welovec_user_process_info user{};
    user.id = info.id;
    user.parent_id = info.parent_id;
    user.live_threads = info.live_threads;
    user.exit_status = info.exit_status;
    user.state = static_cast<uint32_t>(info.state);
    user.uid = info.credentials.uid;
    user.gid = info.credentials.gid;
    user.capabilities = info.credentials.capabilities;
    for (uint32_t i = 0; i < sizeof(user.name); ++i) user.name[i] = info.name[i];
    if (!welovec::process::copy_to_current_user(regs->rsi, &user, sizeof(user)))
        return -static_cast<int64_t>(kErrFault);
    return 0;
}

int64_t waitpid_syscall(const syscall_regs_t *regs) {
    if (!regs->rdi || (regs->rdx & ~static_cast<uint64_t>(WELOVEC_WAIT_NOHANG)))
        return -static_cast<int64_t>(kErrInvalid);
    const uint64_t caller = welovec::process::current_id();
    for (;;) {
        welovec::process::Information info{};
        if (!welovec::process::query(regs->rdi, &info))
            return -static_cast<int64_t>(kErrChild);
        if (!process_access_allowed(caller, info))
            return -static_cast<int64_t>(kErrChild);
        if (info.state == welovec::process::State::Zombie) {
            if (welovec::process::windowed_tree_terminating(regs->rdi) &&
                !welovec::process::reap_windowed_descendants(regs->rdi)) {
                if (regs->rdx & WELOVEC_WAIT_NOHANG) return 0;
                if (!welovec::x86_64::scheduler::sleep(1))
                    return -static_cast<int64_t>(kErrIo);
                continue;
            }
            if (regs->rsi && !welovec::process::copy_to_current_user(
                    regs->rsi, &info.exit_status, sizeof(info.exit_status)))
                return -static_cast<int64_t>(kErrFault);
            if (!welovec::process::destroy(regs->rdi))
                return -static_cast<int64_t>(kErrChild);
            return static_cast<int64_t>(regs->rdi);
        }
        if (regs->rdx & WELOVEC_WAIT_NOHANG) return 0;
        if (!welovec::x86_64::scheduler::sleep(1))
            return -static_cast<int64_t>(kErrIo);
    }
}

int64_t kill_syscall(const syscall_regs_t *regs) {
    if (!regs->rdi || regs->rsi > UINT32_MAX)
        return -static_cast<int64_t>(kErrInvalid);
    const uint32_t signal_number = static_cast<uint32_t>(regs->rsi);
    if (signal_number != 0 && signal_number != WELOVEC_SIGNAL_INTERRUPT &&
        signal_number != WELOVEC_SIGNAL_KILL && signal_number != WELOVEC_SIGNAL_TERMINATE)
        return -static_cast<int64_t>(kErrNotSupported);
    welovec::process::Information target{};
    if (!welovec::process::query(regs->rdi, &target))
        return -static_cast<int64_t>(kErrNotFound);
    welovec::security::Credentials caller{};
    if (!welovec::process::current_credentials(&caller))
        return -static_cast<int64_t>(kErrPermission);
    if (!welovec::security::current_has_capability(WELOVEC_CAP_MANAGE_PROCESSES) &&
        caller.uid != target.credentials.uid)
        return -static_cast<int64_t>(kErrPermission);
    if (signal_number == 0) return 0;
    if (signal_number == WELOVEC_SIGNAL_KILL)
        return welovec::process::terminate(
            regs->rdi, -static_cast<int64_t>(signal_number))
            ? 0 : -static_cast<int64_t>(kErrInvalid);

    const auto disposition = welovec::process::send_signal(regs->rdi, signal_number);
    if (disposition == welovec::process::SignalDisposition::Invalid)
        return -static_cast<int64_t>(kErrInvalid);
    if (disposition == welovec::process::SignalDisposition::Default)
        return welovec::process::terminate(
            regs->rdi, -static_cast<int64_t>(signal_number))
            ? 0 : -static_cast<int64_t>(kErrInvalid);
    return 0;
}

int64_t terminate_windowed_tree_syscall(const syscall_regs_t *regs) {
    if (!regs->rdi) return -static_cast<int64_t>(kErrInvalid);
    if (!welovec::process::terminate_windowed_tree(
            welovec::process::current_id(), regs->rdi,
            static_cast<int64_t>(regs->rsi)))
        return -static_cast<int64_t>(kErrPermission);
    return 0;
}

int64_t sigaction_syscall(const syscall_regs_t *regs) {
    if (regs->rdi > UINT32_MAX)
        return -static_cast<int64_t>(kErrInvalid);
    uint64_t old_handler = 0;
    if (!welovec::process::set_current_signal_handler(
            static_cast<uint32_t>(regs->rdi), regs->rsi, &old_handler))
        return -static_cast<int64_t>(kErrInvalid);
    if (regs->rdx && !welovec::process::copy_to_current_user(
            regs->rdx, &old_handler, sizeof(old_handler))) {
        (void)welovec::process::set_current_signal_handler(
            static_cast<uint32_t>(regs->rdi), old_handler, nullptr);
        return -static_cast<int64_t>(kErrFault);
    }
    return 0;
}
}

void syscall_dispatch(syscall_regs_t *regs) {
    if (!regs) return;
    const uint64_t syscall_number = regs->rax;
    const bool framebuffer_operation =
        (syscall_number >= SYS_FB_INFO && syscall_number <= SYS_FB_TEXT) ||
        syscall_number == SYS_FB_SCROLL_UP || syscall_number == SYS_PRINT ||
        syscall_number == SYS_PRINT_COLOR ||
        syscall_number == SYS_PRINT_RESET_CURSOR ||
        syscall_number == SYS_PRINT_CURSOR_GET ||
        syscall_number == SYS_PRINT_CURSOR_SET ||
        syscall_number == SYS_PRINT_SCROLLBACK;
    WelovecFramebufferTargetState framebuffer_target{};
    bool framebuffer_target_active = false;
    if (framebuffer_operation) {
        WelovecFramebufferSurface *surface = nullptr;
        if (welovec::process::current_framebuffer_surface(&surface))
            framebuffer_target_active =
                framebuffer_surface_begin(surface, &framebuffer_target);
    }
    switch (regs->rax) {
        case SYS_WRITE: {
            regs->rax = static_cast<uint64_t>(console_write_user(regs->rdi, regs->rsi));
            break;
        }
        case SYS_EXIT:
            welovec::process::exit_current(static_cast<int64_t>(regs->rdi));
        case SYS_THREAD_EXIT:
            welovec::x86_64::scheduler::exit_current(static_cast<int64_t>(regs->rdi));
        case SYS_YIELD:
            welovec::x86_64::scheduler::yield();
            regs->rax = 0;
            break;
        case SYS_SLEEP:
            regs->rax = welovec::x86_64::scheduler::sleep(regs->rdi)
                ? 0 : static_cast<uint64_t>(-22);
            break;
        case SYS_GETPID:
            regs->rax = welovec::process::current_id();
            break;
        case SYS_THREAD_CREATE: {
            const uint64_t thread_id = welovec::process::create_user_thread(
                welovec::process::current_id(), "user-thread", regs->rdi, regs->rsi,
                static_cast<uint8_t>(welovec::x86_64::scheduler::Priority::Normal));
            regs->rax = thread_id ? thread_id : static_cast<uint64_t>(-22); // EINVAL
            break;
        }
        case SYS_GETUID:
        case SYS_GETGID:
        case SYS_GETCAPS: {
            welovec::security::Credentials credentials{};
            if (!welovec::process::current_credentials(&credentials)) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrNoProcess));
            } else if (regs->rax == SYS_GETUID) {
                regs->rax = credentials.uid;
            } else if (regs->rax == SYS_GETGID) {
                regs->rax = credentials.gid;
            } else {
                regs->rax = welovec::process::current_session_authenticated()
                    ? credentials.capabilities : 0;
            }
            break;
        }
        case SYS_HAS_CAPABILITY:
            regs->rax = welovec::security::current_has_capability(regs->rdi) ? 1 : 0;
            break;
        case SYS_LOGIN: {
            char username[WELOVEC_USER_NAME_MAX];
            char password[WELOVEC_USER_PASSWORD_MAX + 1];
            uint64_t username_length = 0, password_length = 0;
            if (!copy_user_string(regs->rdi, username, sizeof(username), &username_length) ||
                !copy_user_string(regs->rsi, password, sizeof(password), &password_length)) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrFault));
                clear_sensitive_buffer(password, sizeof(password));
                break;
            }
            (void)username_length;
            (void)password_length;
            const int64_t result = welovec::security::login(username, password);
            clear_sensitive_buffer(username, sizeof(username));
            clear_sensitive_buffer(password, sizeof(password));
            regs->rax = static_cast<uint64_t>(result);
            break;
        }
        case SYS_LOGOUT:
            regs->rax = static_cast<uint64_t>(welovec::security::logout());
            break;
        case SYS_USER_STATUS: {
            welovec_user_status status{};
            if (!welovec::security::current_user_status(&status)) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrNoProcess));
            } else if (!welovec::process::copy_to_current_user(
                           regs->rdi, &status, sizeof(status))) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrFault));
            } else {
                regs->rax = 0;
            }
            break;
        }
        case SYS_USER_CREATE: {
            if (regs->rdx > UINT32_MAX) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrInvalid));
                break;
            }
            char username[WELOVEC_USER_NAME_MAX];
            char password[WELOVEC_USER_PASSWORD_MAX + 1];
            uint64_t username_length = 0, password_length = 0;
            if (!copy_user_string(regs->rdi, username, sizeof(username), &username_length) ||
                !copy_user_string(regs->rsi, password, sizeof(password), &password_length)) {
                clear_sensitive_buffer(username, sizeof(username));
                clear_sensitive_buffer(password, sizeof(password));
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrFault));
                break;
            }
            (void)username_length;
            (void)password_length;
            const int64_t result = welovec::security::create_user(
                username, password, static_cast<uint32_t>(regs->rdx));
            clear_sensitive_buffer(username, sizeof(username));
            clear_sensitive_buffer(password, sizeof(password));
            regs->rax = static_cast<uint64_t>(result);
            break;
        }
        case SYS_USER_LIST: {
            if (regs->rsi > WELOVEC_USER_MAX_ACCOUNTS) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrInvalid));
                break;
            }
            if (!regs->rsi) {
                regs->rax = static_cast<uint64_t>(welovec::security::list_users(nullptr, 0));
                break;
            }
            if (!regs->rdi) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrFault));
                break;
            }
            welovec_user_record users[WELOVEC_USER_MAX_ACCOUNTS]{};
            const int64_t count = welovec::security::list_users(users, regs->rsi);
            if (count < 0) {
                regs->rax = static_cast<uint64_t>(count);
            } else if (count == 0) {
                regs->rax = 0;
            } else if (!welovec::process::copy_to_current_user(
                           regs->rdi, users,
                           static_cast<uint64_t>(count) * sizeof(users[0]))) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrFault));
            } else {
                regs->rax = static_cast<uint64_t>(count);
            }
            break;
        }
        case SYS_EXEC:
            regs->rax = static_cast<uint64_t>(execute_user_program(regs, ExecMode::Inherit));
            break;
        case SYS_EXEC_WINDOWED:
            regs->rax = static_cast<uint64_t>(execute_user_program(regs, ExecMode::Windowed));
            break;
        case SYS_EXEC_ADMIN:
            regs->rax = static_cast<uint64_t>(execute_user_program(regs, ExecMode::Admin));
            break;
        case SYS_EXEC_AS_USER:
            regs->rax = static_cast<uint64_t>(execute_user_program(regs, ExecMode::User));
            break;
        case SYS_FS_EXISTS:
            regs->rax = static_cast<uint64_t>(fs_exists_syscall(regs));
            break;
        case SYS_FS_STAT:
            regs->rax = static_cast<uint64_t>(fs_stat_syscall(regs));
            break;
        case SYS_FS_READ:
            regs->rax = static_cast<uint64_t>(fs_read_syscall(regs));
            break;
        case SYS_FS_WRITE:
            regs->rax = static_cast<uint64_t>(fs_write_syscall(regs));
            break;
        case SYS_FS_LIST:
            regs->rax = static_cast<uint64_t>(fs_list_syscall(regs));
            break;
        case SYS_FS_MKDIR:
            regs->rax = static_cast<uint64_t>(fs_mkdir_syscall(regs));
            break;
        case SYS_FS_UNLINK:
            regs->rax = static_cast<uint64_t>(fs_unlink_syscall(regs));
            break;
        case SYS_FS_MOVE:
            regs->rax = static_cast<uint64_t>(fs_move_syscall(regs));
            break;
        case SYS_GETTICKS:
            regs->rax = welovec::x86_64::scheduler::tick_count();
            break;
        case SYS_SECURE_RANDOM: {
            constexpr uint64_t kSecureRandomMaximum = 256;
            if (!regs->rsi || regs->rsi > kSecureRandomMaximum) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrInvalid));
                break;
            }
            uint8_t random_bytes[kSecureRandomMaximum];
            if (!welovec::security::secure_random_bytes(random_bytes, regs->rsi)) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrNoDevice));
                break;
            }
            if (!welovec::process::copy_to_current_user(
                    regs->rdi, random_bytes, regs->rsi)) {
                for (volatile uint8_t &byte : random_bytes) byte = 0;
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrFault));
                break;
            }
            for (volatile uint8_t &byte : random_bytes) byte = 0;
            regs->rax = regs->rsi;
            break;
        }
        case SYS_RTC_UTC: {
            RtcDateTime time{};
            if (!rtc_get_utc_time(&time)) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrNoDevice));
                break;
            }
            const welovec_user_datetime result{
                time.year, time.month, time.day, time.hour, time.minute, time.second
            };
            regs->rax = welovec::process::copy_to_current_user(
                regs->rdi, &result, sizeof(result))
                ? 0 : static_cast<uint64_t>(-static_cast<int64_t>(kErrFault));
            break;
        }
        case SYS_SYSTEM_REBOOT:
        case SYS_SYSTEM_SHUTDOWN: {
            if (regs->rdi > 1) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrInvalid));
            } else {
                const bool virtual_machine = regs->rdi != 0;
                const bool accepted = regs->rax == SYS_SYSTEM_REBOOT
                    ? welovec_system_reboot(virtual_machine)
                    : welovec_system_shutdown(virtual_machine);
                regs->rax = accepted ? 0 :
                    static_cast<uint64_t>(-static_cast<int64_t>(kErrNoDevice));
            }
            break;
        }
        case SYS_READ:
            regs->rax = static_cast<uint64_t>(fd_read_syscall(regs));
            break;
        case SYS_FD_WRITE:
            regs->rax = static_cast<uint64_t>(fd_write_syscall(regs));
            break;
        case SYS_OPEN:
            regs->rax = static_cast<uint64_t>(fd_open_syscall(regs));
            break;
        case SYS_CLOSE:
            regs->rax = static_cast<uint64_t>(fd_close_syscall(regs));
            break;
        case SYS_SEEK:
            regs->rax = static_cast<uint64_t>(fd_seek_syscall(regs));
            break;
        case SYS_MMAP: {
            if (!regs->rdi) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrInvalid));
                break;
            }
            if (regs->rdi > welovec::process::maximum_user_mmap_size()) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrNoMemory));
                break;
            }
            const uint64_t address = welovec::process::map_current_user_memory(regs->rdi);
            regs->rax = address ? address : static_cast<uint64_t>(-static_cast<int64_t>(kErrNoMemory));
            break;
        }
        case SYS_MUNMAP:
            regs->rax = welovec::process::unmap_current_user_memory(regs->rdi, regs->rsi)
                ? 0 : static_cast<uint64_t>(-static_cast<int64_t>(kErrInvalid));
            break;
        case SYS_WAITPID:
            regs->rax = static_cast<uint64_t>(waitpid_syscall(regs));
            break;
        case SYS_PROCESS_QUERY:
            regs->rax = static_cast<uint64_t>(process_query_syscall(regs));
            break;
        case SYS_KILL:
            regs->rax = static_cast<uint64_t>(kill_syscall(regs));
            break;
        case SYS_SIGACTION:
            regs->rax = static_cast<uint64_t>(sigaction_syscall(regs));
            break;
        case SYS_SIGRETURN:
            if (!welovec::process::restore_signal_context(regs))
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrInvalid));
            break;
        case SYS_PRINT:
            regs->rax = static_cast<uint64_t>(framebuffer_print_syscall(regs, false));
            break;
        case SYS_PRINT_COLOR:
            regs->rax = static_cast<uint64_t>(framebuffer_print_syscall(regs, true));
            break;
        case SYS_PRINT_RESET_CURSOR:
            if (!framebuffer_ready()) {
                regs->rax = static_cast<uint64_t>(-static_cast<int64_t>(kErrNoDevice));
            } else {
                fb_reset_print_cursor();
                regs->rax = 0;
            }
            break;
        case SYS_PRINT_CURSOR_GET:
            regs->rax = static_cast<uint64_t>(framebuffer_print_cursor_get_syscall(regs));
            break;
        case SYS_PRINT_CURSOR_SET:
            regs->rax = static_cast<uint64_t>(framebuffer_print_cursor_set_syscall(regs));
            break;
        case SYS_PRINT_SCROLLBACK:
            regs->rax = static_cast<uint64_t>(framebuffer_print_scrollback_syscall(regs));
            break;
        case SYS_INPUT_POLL:
            regs->rax = static_cast<uint64_t>(input_poll_syscall(regs));
            break;
        case SYS_POINTER_POLL:
            regs->rax = static_cast<uint64_t>(pointer_poll_syscall(regs));
            break;
        case SYS_POINTER_SOURCES:
            regs->rax = welovec::x86_64::input::pointer::detected_sources();
            break;
        case SYS_FB_SURFACE_BLIT:
            regs->rax = static_cast<uint64_t>(framebuffer_surface_blit_syscall(regs));
            break;
        case SYS_FB_SURFACE_RESIZE:
            regs->rax = static_cast<uint64_t>(framebuffer_surface_resize_syscall(regs));
            break;
        case SYS_INPUT_FORWARD:
            regs->rax = static_cast<uint64_t>(input_forward_syscall(regs));
            break;
        case SYS_FB_SURFACE_GENERATION:
            regs->rax = static_cast<uint64_t>(framebuffer_surface_generation_syscall(regs));
            break;
        case SYS_TERMINATE_WINDOWED_TREE:
            regs->rax = static_cast<uint64_t>(terminate_windowed_tree_syscall(regs));
            break;
        case SYS_FB_INFO:
            regs->rax = static_cast<uint64_t>(framebuffer_info_syscall(regs));
            break;
        case SYS_FB_PIXEL:
            regs->rax = static_cast<uint64_t>(framebuffer_pixel_syscall(regs));
            break;
        case SYS_FB_RECT:
            regs->rax = static_cast<uint64_t>(framebuffer_rect_syscall(regs));
            break;
        case SYS_FB_RECT_ALPHA:
            regs->rax = static_cast<uint64_t>(framebuffer_alpha_rect_syscall(regs));
            break;
        case SYS_FB_GRADIENT:
            regs->rax = static_cast<uint64_t>(framebuffer_gradient_syscall(regs));
            break;
        case SYS_FB_OUTLINE:
            regs->rax = static_cast<uint64_t>(framebuffer_outline_syscall(regs));
            break;
        case SYS_FB_LINE:
            regs->rax = static_cast<uint64_t>(framebuffer_line_syscall(regs));
            break;
        case SYS_FB_CIRCLE:
            regs->rax = static_cast<uint64_t>(framebuffer_circle_syscall(regs, false));
            break;
        case SYS_FB_FILL_CIRCLE:
            regs->rax = static_cast<uint64_t>(framebuffer_circle_syscall(regs, true));
            break;
        case SYS_FB_CHAR:
            regs->rax = static_cast<uint64_t>(framebuffer_char_syscall(regs));
            break;
        case SYS_FB_TEXT:
            regs->rax = static_cast<uint64_t>(framebuffer_text_syscall(regs));
            break;
        case SYS_FB_SCROLL_UP:
            regs->rax = static_cast<uint64_t>(framebuffer_scroll_up_syscall(regs));
            break;
        case SYS_NET_INFO:
            regs->rax = static_cast<uint64_t>(network_info_syscall(regs));
            break;
        case SYS_NET_PING:
            regs->rax = static_cast<uint64_t>(network_ping_syscall(regs));
            break;
        case SYS_NET_DNS:
            regs->rax = static_cast<uint64_t>(network_dns_syscall(regs));
            break;
        case SYS_NET_DNS_IPV6:
            regs->rax = static_cast<uint64_t>(network_dns_ipv6_syscall(regs));
            break;
        case SYS_NET_IPV6_INFO:
            regs->rax = static_cast<uint64_t>(network_ipv6_info_syscall(regs));
            break;
        case SYS_NET_TCP_CONNECT:
            regs->rax = static_cast<uint64_t>(network_tcp_connect_syscall(regs));
            break;
        case SYS_NET_TCP_CONNECT_IPV6:
            regs->rax = static_cast<uint64_t>(network_tcp_connect_ipv6_syscall(regs));
            break;
        case SYS_NET_TCP_SEND:
            regs->rax = static_cast<uint64_t>(network_tcp_send_syscall(regs));
            break;
        case SYS_NET_TCP_RECV:
            regs->rax = static_cast<uint64_t>(network_tcp_receive_syscall(regs));
            break;
        case SYS_NET_TCP_CLOSE:
            regs->rax = static_cast<uint64_t>(network_tcp_close_syscall(regs));
            break;
        default:
            regs->rax = static_cast<uint64_t>(-38); // ENOSYS
            break;
    }

    if (framebuffer_target_active)
        framebuffer_surface_end(&framebuffer_target);

    uint32_t delivered_signal = 0;
    const int signal_result = welovec::process::prepare_pending_signal(
        regs, kSignalReturnAddress, &delivered_signal);
    if (signal_result < 0) {
        const int64_t status = delivered_signal
            ? -static_cast<int64_t>(delivered_signal)
            : -static_cast<int64_t>(kErrFault);
        const uint64_t process_id = welovec::process::current_id();
        if (!welovec::process::terminate(process_id, status))
            welovec::process::exit_current(status);
    }
}
