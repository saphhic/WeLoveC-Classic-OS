// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for io in the io module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

//   this script is the input output script,
//   defines functions like: outb, inb, outw, inw
//   its job is to communicate  whit the I/O series ports.

#ifndef ARCH_X86_64_IO_H
#define ARCH_X86_64_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1"
                      :
                      : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0"
                      : "=a"(value)
                      : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1"
                      :
                      : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0"
                      : "=a"(value)
                      : "Nd"(port));
    return value;
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1"
                      :
                      : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0"
                      : "=a"(value)
                      : "Nd"(port));
    return value;
}

static inline void io_wait(void)
{
    __asm__ volatile ("outb %%al,$0x80"
                      :
                      : "a"(0));
}

#endif