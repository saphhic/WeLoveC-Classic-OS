// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for cpuid in the cpu module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

//   this script asks the CPU ehat it can do.
//   for example:
//     when we do cpuid() and do an instruction depending on what instruction we asked for
//     well know if the CPU supports SSE, AVX, x2APIC etc.
//     and well also know its instructions type, if its Intel, AMD, VMware, etc.

#ifndef ARCH_X86_64_CPUID_H
#define ARCH_X86_64_CPUID_H

#include <stdint.h>

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid" : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx) : "a" (leaf), "c" (subleaf));
}

#endif // ARCH_X86_64_CPUID_H