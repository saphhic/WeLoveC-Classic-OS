// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for control regs in the cpu module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

//   this script defines the CPU CR registers,
//    example: CR0, CR2.

#ifndef ARCH_X86_64_CONTROL_REGS_H
#define ARCH_X86_64_CONTROL_REGS_H

#include <stdint.h>

static inline uint64_t read_cr0(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr0, %0"
                      : "=r"(val));
    return val;
}

static inline void write_cr0(uint64_t val)
{
    __asm__ volatile ("mov %0, %%cr0"
                      :
                      : "r"(val));
}

static inline uint64_t read_cr2(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr2, %0"
                      : "=r"(val));
    return val;
}

static inline void write_cr2(uint64_t val)
{
    __asm__ volatile ("mov %0, %%cr2"
                      :
                      : "r"(val));
}

static inline uint64_t read_cr3(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr3, %0"
                      : "=r"(val));
    return val;
}

static inline void write_cr3(uint64_t val)
{
    __asm__ volatile ("mov %0, %%cr3"
                      :
                      : "r"(val));
}

static inline uint64_t read_cr4(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr4, %0"
                      : "=r"(val));
    return val;
}

static inline void write_cr4(uint64_t val)
{
    __asm__ volatile ("mov %0, %%cr4"
                      :
                      : "r"(val));
}

#endif // ARCH_X86_64_CONTROL_REGS_H