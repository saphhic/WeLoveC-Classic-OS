// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for gdt in the inter module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026


// Global Descriptor Table plus Task State Segment.



#ifndef ARCH_X86_64_GDT_H
#define ARCH_X86_64_GDT_H

#include <stdint.h>

#define GDT_SEL_NULL  0x00
#define GDT_SEL_KCODE 0x08
#define GDT_SEL_KDATA 0x10
#define GDT_SEL_UDATA 0x18
#define GDT_SEL_UCODE 0x20
#define GDT_SEL_TSS   0x28

struct __attribute__((packed)) gdt_entry_t {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
};

struct __attribute__((packed)) gdt_tss_entry_t {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
};

struct __attribute__((packed)) gdt_ptr_t {
    uint16_t limit;
    uint64_t base;
};

struct __attribute__((packed)) tss_t {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};

#ifdef __cplusplus
extern "C" {
#endif

void gdt_init(void);

void tss_set_kernel_stack(uint64_t rsp0);

#ifdef __cplusplus
}
#endif

#endif // ARCH_X86_64_GDT_H
