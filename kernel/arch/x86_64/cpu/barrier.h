// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for barrier in the cpu module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

// CPU sometimes change prioritys if there are multiple clusters
// this script is the barrier to prevent random priority cnages in clusters.
// these barriers obligate the CPU to finish a task before continuing.

#ifndef ARCH_X86_64_BARRIER_H
#define ARCH_X86_64_BARRIER_H

static inline void mfence(void) {
    __asm__ volatile("mfence" : : : "memory");

}

static inline void lfence(void) {
    __asm__ volatile("lfence" : : : "memory");

}

static inline void sfence(void) {
    __asm__ volatile("sfence" : : : "memory");

}

#endif // ARCH_X86_64_BARRIER_H