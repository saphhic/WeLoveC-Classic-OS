// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares keyboard and pointer event input functions for user programs.

#ifndef WELOVEC_USER_INPUT_H
#define WELOVEC_USER_INPUT_H

#include <stdint.h>
#include "user/syscall.h"
#include "user/input_event.h"

#define WELOVEC_STDIN_FILENO 0

/* Blocks for one key, then drains any additional queued characters. */
static inline int64_t welovec_input_read(char *buffer, uint64_t capacity) {
    return welovec_read(WELOVEC_STDIN_FILENO, buffer, capacity);
}

/* Returns available input immediately, or zero when the keyboard queue is empty. */
static inline int64_t welovec_input_poll(char *buffer, uint64_t capacity) {
    return welovec_user_syscall2(WELOVEC_SYS_INPUT_POLL,
                                 (uint64_t)(uintptr_t)buffer, capacity);
}

/* Returns pointer events without blocking. Each event identifies its source. */
static inline int64_t welovec_pointer_poll(welovec_pointer_event *events,
                                           uint64_t capacity) {
    return welovec_user_syscall2(WELOVEC_SYS_POINTER_POLL,
                                 (uint64_t)(uintptr_t)events, capacity);
}

static inline uint32_t welovec_pointer_sources(void) {
    return (uint32_t)welovec_user_syscall0(WELOVEC_SYS_POINTER_SOURCES);
}

/* Forward desktop input to one of its windowed child processes. */
static inline int64_t welovec_input_forward(
    uint64_t process_id, const char *characters, uint32_t character_count,
    const welovec_pointer_event *events, uint32_t event_count) {
    return welovec_user_syscall5(
        WELOVEC_SYS_INPUT_FORWARD, process_id,
        (uint64_t)(uintptr_t)characters, character_count,
        (uint64_t)(uintptr_t)events, event_count);
}

#endif
