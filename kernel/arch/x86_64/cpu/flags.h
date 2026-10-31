// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for flags in the cpu module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

// function that reads register of RFLAGS
// wich contain: Carry, Zero, Overflow, Interrupt Flag, Direction, etc.

#ifndef ARCH_X86_64_FLAGS_H
#define ARCH_X86_64_FLAGS_H

#include <stdint.h>

static inline uint64_t read_rflags(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq\n\t"
                      "popq %0"
                      : "=r"(flags));
    return flags;
}

#endif // ARCH_X86_64_FLAGS_H