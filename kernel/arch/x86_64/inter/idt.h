// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for idt in the inter module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

//   Interrupt Descriptor Table. Maps each of the 256 possible interrupt
//   vectors to a handler stub. Vectors 0-31 are CPU exceptions (fixed by
//   the architecture, e.g. 14 = #PF), 32-47 are the legacy PIC IRQs after
//   remapping (see pic.h — they MUST be remapped off 0-31 or a spurious
//   IRQ0 during boot looks exactly like a #DE exception), the rest are
//   free for software interrupts / a future syscall vector (0x80 is the
//   traditional choice).

#ifndef ARCH_X86_64_IDT_H
#define ARCH_X86_64_IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

#define IDT_GATE_INTERRUPT 0x8E
#define IDT_GATE_TRAP 0x8F
#define IDT_GATE_USER_INT 0xEE

struct __attribute__((packed)) idt_entry_t {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
};

struct __attribute__((packed)) idt_ptr_t {
    uint16_t limit;
    uint64_t base;
};

#ifdef __cplusplus
extern "C" {
#endif

void idt_init(void);

void idt_set_gate(uint8_t vector, uint64_t handler, uint16_t selector, uint8_t ist, uint8_t type_attr);


#ifdef __cplusplus
}
#endif

#endif // ARCH_X86_64_IDT_H
