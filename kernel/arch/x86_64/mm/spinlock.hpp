// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for spinlock in the memory management module.

#ifndef WELOVEC_X86_64_MM_SPINLOCK_HPP
#define WELOVEC_X86_64_MM_SPINLOCK_HPP

#include <stdint.h>

namespace welovec::x86_64::mm {

class IrqSpinLock {
public:
    constexpr IrqSpinLock() : state_(0) {}
    IrqSpinLock(const IrqSpinLock &) = delete;
    IrqSpinLock &operator=(const IrqSpinLock &) = delete;

    uint64_t lock_irqsave() {
        uint64_t flags;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
        while (__atomic_test_and_set(&state_, __ATOMIC_ACQUIRE)) {
            while (__atomic_load_n(&state_, __ATOMIC_RELAXED)) __asm__ volatile("pause");
        }
        return flags;
    }

    void unlock_irqrestore(uint64_t flags) {
        __atomic_clear(&state_, __ATOMIC_RELEASE);
        if (flags & (UINT64_C(1) << 9)) __asm__ volatile("sti" ::: "memory");
    }

private:
    uint8_t state_;
};

class LockGuard {
public:
    explicit LockGuard(IrqSpinLock &lock) : lock_(lock), flags_(lock.lock_irqsave()) {}
    ~LockGuard() { lock_.unlock_irqrestore(flags_); }
    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;
private:
    IrqSpinLock &lock_;
    uint64_t flags_;
};

}

#endif
