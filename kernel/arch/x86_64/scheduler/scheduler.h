// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for scheduler in the scheduler module.

#ifndef WELOVEC_X86_64_SCHEDULER_C_API_H
#define WELOVEC_X86_64_SCHEDULER_C_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t scheduler_task_id_t;
typedef void (*scheduler_task_entry_t)(void *argument);

enum scheduler_priority {
    SCHEDULER_PRIORITY_HIGH = 0,
    SCHEDULER_PRIORITY_NORMAL = 1,
    SCHEDULER_PRIORITY_BACKGROUND = 2,
    SCHEDULER_PRIORITY_LOW = 3
};

int scheduler_initialize(void);
void scheduler_start(void);
scheduler_task_id_t scheduler_create_kernel_thread(const char *name,
                                                    scheduler_task_entry_t entry,
                                                    void *argument,
                                                    uint8_t priority);
void scheduler_yield(void);
int scheduler_sleep(uint64_t ticks);
int scheduler_block_current(void);
int scheduler_wake(scheduler_task_id_t id);
void scheduler_exit_current(void);
scheduler_task_id_t scheduler_current_id(void);
uint64_t scheduler_tick_count(void);

#ifdef __cplusplus
}
#endif

#endif
