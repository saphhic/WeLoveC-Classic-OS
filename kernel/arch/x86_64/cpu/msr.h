// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for msr in the cpu module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

//   this is the MSR (Model Specific Registers)
//   similar to CR registers but it isnt the same
//   MSR registers are defined like this:
//     example: "IA32_EFER" this activates long mode
//
//   a other example is: "IA32_APIC_BASE" wich idicates where APIC is.
//   
//   you read whit:     rdmsr();
//   you write whit:    wrmsr();


#ifndef ARCH_X86_64_MSR_H
#define ARCH_X86_64_MSR_H

#include <stdint.h>

static  inline uint64_t rdmsr(uint32_t reg)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr"
                      : "=a"(lo), "=d"(hi)
                      : "c"(reg));
    return ((uint64_t)hi << 32) | lo;
}

static  inline void wrmsr(uint32_t reg, uint64_t val)
{
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    
    __asm__ volatile ("wrmsr"
                      :
                      : "c"(reg), "a"(lo), "d"(hi));
}

#endif // ARCH_X86_64_MSR_H
