// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for pic in the inter module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

//   By default the 8259 PICs fire IRQ0-7 on interrupt vectors 0x08-0x0F
//   and IRQ8-15 on 0x70-0x77 — vectors 0x08-0x0F COLLIDE with CPU
//   exceptions #DF, #TS, #NP, #SS, #GP, #PF, etc. If you enable
//   interrupts without remapping first, the very first timer tick
//   (IRQ0) looks EXACTLY like a #DE (divide error) to this IDT. This is
//   one of the most common "why does my kernel immediately triple
//   fault" bugs, so it's not optional and not really simplifiable.
//
//   Long-term this should be replaced/complemented by the local APIC +
//   IOAPIC (required for SMP anyway), but the PIC is what's guaranteed
//   present and simplest to bring a single core up with first.


#ifndef ARCH_X86_64_PIC_H
#define ARCH_X86_64_PIC_H

#include <stdint.h>

#define PIC_EOI 0x20
#define ICW4_8086 0x01

#ifdef __cplusplus
extern "C" {
#endif

void pic_remap(uint8_t offset1, uint8_t offset2);
void pic_send_eoi(uint8_t irq);

void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);

#ifdef __cplusplus
}
#endif

#endif // ARCH_X86_64_PIC_H