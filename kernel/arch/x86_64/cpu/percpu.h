// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for percpu in the cpu module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

//   SYSCALL does NOT switch stacks the way an interrupt/TSS.rsp0 does —
//   it leaves RSP exactly as the user program had it. Running kernel code
//   on a user-controlled stack is not something we want to do even for
//   one instruction, so well: keep a small per-CPU
//   struct, point GS_BASE/KERNEL_GS_BASE MSRs at it, and use `swapgs` +
//   %gs-relative loads in the syscall entry stub to get onto a
//   kernel stack.

#ifndef ARCH_X86_64_PERCPU_H
#define ARCH_X86_64_PERCPU_H

#include <stdint.h>

struct __attribute__((packed)) percpu_t {
    uint64_t self;
    uint64_t kernel_rsp;
    uint64_t user_rsp;
};

#ifdef __cplusplus
extern "C" {
#endif

extern percpu_t g_percpu0;

void percpu_init(uint64_t kernel_stack_top);

#ifdef __cplusplus
}
#endif

#endif // ARCH_X86_64_PERCPU_H
