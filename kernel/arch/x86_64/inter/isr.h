// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for isr in the inter module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

//   Shared contract between isr.asm and the C++ dispatchers in
//   idt.cpp / exceptions.cpp. Field order below MUST match the push
//   order in isr_common_stub/irq_common_stub in isr.asm exactly, or the
//   handler reads garbage into rax/rbx/etc. (If you ever change one, you
//   have to change the other.)

#ifndef ARCH_X86_64_ISR_H
#define ARCH_X86_64_ISR_H

#include <stdint.h>

struct registers_t {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    
    uint64_t int_no;
    uint64_t err_code;
    
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t user_rsp;
    uint64_t ss;
};

typedef void (*isr_handler_t)(registers_t *regs);

#ifdef __cplusplus
extern "C" {
#endif

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

void isr_handler(registers_t *regs);
registers_t *irq_handler(registers_t *regs);

void irq_install_handler(int irq_num, isr_handler_t handler);
void irq_uninstall_handler(int irq_num);

#ifdef __cplusplus
}
#endif

#endif // ARCH_X86_64_ISR_H
