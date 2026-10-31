// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for panic in the panic module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

//   A panic that doesn't print WHY is worthless — this is meant to be
//   the one thing that still works even if everything else (paging,
//   the heap, a display driver) is what just broke.

#ifndef ARCH_X86_64_PANIC_H
#define ARCH_X86_64_PANIC_H

struct registers_t;

#ifdef __cplusplus
extern "C" {
#endif

[[noreturn]] void panic(const char *message, registers_t *regs);

#ifdef __cplusplus
}
#endif

#endif // ARCH_X86_64_PANIC_H