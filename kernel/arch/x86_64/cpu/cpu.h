// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for cpu in the cpu module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026
 
//   this script is the instruction definer for the cpu
//   it serves so functions like, cli, sti, hlt can be used directly.

#ifndef ARCH_X86_64_CPU_H
#define ARCH_X86_64_CPU_H

static inline void cli(void)
{
    __asm__ volatile("cli");
}

static inline void sti(void)
{
    __asm__ volatile("sti");
}

static inline void hlt(void)
{
    __asm__ volatile("hlt");
}

static inline void pause_cpu(void)
{
    __asm__ volatile("pause");
}

static inline void nop(void)
{
    __asm__ volatile("nop");
}

static inline void load_idt(const void* descriptor)
{
    __asm__ volatile("lidt (%0)" : : "r"(descriptor) : "memory");
}

static inline void breakpoint(void)
{
    __asm__ volatile("int3");
}

#endif // ARCH_X86_CPU_H

