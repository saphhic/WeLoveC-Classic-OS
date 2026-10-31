// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for task lock in the scheduler module.

#ifndef WELOVEC_X86_64_SCHEDULER_TASK_LOCK_HPP
#define WELOVEC_X86_64_SCHEDULER_TASK_LOCK_HPP

#include "arch/x86_64/scheduler/scheduler.hpp"

namespace welovec::x86_64::scheduler {

// Serializes task-context operations that may take a while. Interrupts are
// restored after acquisition so device I/O and other long critical sections
// do not stop the system timer. Do not acquire this lock from an interrupt.
class TaskLock {
public:
    constexpr TaskLock() : state_(0) {}
    TaskLock(const TaskLock &) = delete;
    TaskLock &operator=(const TaskLock &) = delete;

    uint64_t lock() {
        uint64_t flags;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
        for (;;) {
            if (!__atomic_test_and_set(&state_, __ATOMIC_ACQUIRE)) {
                if ((flags & (UINT64_C(1) << 9)) ||
                    welovec::x86_64::scheduler::running())
                    __asm__ volatile("sti" ::: "memory");
                return flags;
            }

            if ((flags & (UINT64_C(1) << 9)) ||
                welovec::x86_64::scheduler::running()) {
                __asm__ volatile("sti" ::: "memory");
                welovec::x86_64::scheduler::yield();
                __asm__ volatile("cli" ::: "memory");
            } else {
                __asm__ volatile("pause" ::: "memory");
            }
        }
    }

    void unlock(uint64_t flags) {
        __asm__ volatile("cli" ::: "memory");
        __atomic_clear(&state_, __ATOMIC_RELEASE);
        if (flags & (UINT64_C(1) << 9)) __asm__ volatile("sti" ::: "memory");
    }

private:
    uint8_t state_;
};

}

#endif
