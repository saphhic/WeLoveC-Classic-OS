// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for scheduler in the scheduler module.

#ifndef WELOVEC_X86_64_SCHEDULER_HPP
#define WELOVEC_X86_64_SCHEDULER_HPP

#include <stdint.h>
#include "arch/x86_64/mm/physical/physical_address.hpp"

namespace welovec::x86_64::scheduler {

using TaskId = uint64_t;
using TaskEntry = void (*)(void *argument);
using ExitCallback = void (*)(TaskId thread_id, uint64_t process_id, int64_t status, void *context);

enum class Priority : uint8_t {
    High = 0,
    Normal = 1,
    Background = 2,
    Low = 3
};

struct Statistics {
    uint64_t ticks;
    uint64_t context_switches;
    uint64_t preemptions;
    uint64_t task_count;
    uint64_t ready_count;
    uint64_t sleeping_count;
    uint64_t blocked_count;
};

bool initialize();
bool ready();
bool running();
void start();
TaskId create_kernel_thread(const char *name, TaskEntry entry, void *argument,
                            Priority priority = Priority::Normal);
TaskId create_user_thread(const char *name, uint64_t entry, uint64_t user_stack,
                          uint64_t argument, uint64_t process_id,
                          welovec::x86_64::mm::PhysicalAddress address_space,
                          ExitCallback on_exit, void *exit_context,
                          Priority priority = Priority::Normal);
bool terminate_group(uint64_t process_id, int64_t status);
void yield();
bool sleep(uint64_t ticks);
bool block_current();
bool wake(TaskId id);
[[noreturn]] void exit_current();
[[noreturn]] void exit_current(int64_t status);
TaskId current_id();
uint64_t current_process_id();
welovec::x86_64::mm::PhysicalAddress current_address_space();
uint64_t tick_count();
Statistics statistics();

}

#endif
