// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Configures the processor interrupt descriptor table.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

// program for idt.h

#include "idt.h"
#include "gdt.h"
#include "isr.h"

static idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t idtr;

extern "C" void idt_flush(uint64_t idtr_addr);

void idt_set_gate(uint8_t vector, uint64_t handler, uint16_t selector, uint8_t ist, uint8_t type_attr) {
    idt[vector].offset_low = (uint16_t)(handler & 0xFFFF);
    idt[vector].offset_mid = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)(handler >> 32) & 0xFFFFFFFF;
    idt[vector].selector = selector;
    idt[vector].ist = ist & 0x7;
    idt[vector].type_attr = type_attr;
    idt[vector].reserved = 0;
}

void idt_init(void) {
    idtr.limit = (sizeof(idt_entry_t) * IDT_ENTRIES) - 1;
    idtr.base = (uint64_t)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(i, 0, 0, 0, 0);

    idt_set_gate(0,  (uint64_t)isr0,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(1,  (uint64_t)isr1,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(2,  (uint64_t)isr2,  GDT_SEL_KCODE, 1, IDT_GATE_INTERRUPT);
    idt_set_gate(3,  (uint64_t)isr3,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(4,  (uint64_t)isr4,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(5,  (uint64_t)isr5,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(6,  (uint64_t)isr6,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(7,  (uint64_t)isr7,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(8,  (uint64_t)isr8,  GDT_SEL_KCODE, 2, IDT_GATE_INTERRUPT);
    idt_set_gate(9,  (uint64_t)isr9,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(10, (uint64_t)isr10, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(11, (uint64_t)isr11, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(12, (uint64_t)isr12, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(13, (uint64_t)isr13, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(14, (uint64_t)isr14, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(15, (uint64_t)isr15, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(16, (uint64_t)isr16, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(17, (uint64_t)isr17, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(18, (uint64_t)isr18, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(19, (uint64_t)isr19, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(20, (uint64_t)isr20, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(21, (uint64_t)isr21, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(22, (uint64_t)isr22, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(23, (uint64_t)isr23, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(24, (uint64_t)isr24, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(25, (uint64_t)isr25, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(26, (uint64_t)isr26, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(27, (uint64_t)isr27, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(28, (uint64_t)isr28, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(29, (uint64_t)isr29, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(30, (uint64_t)isr30, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(31, (uint64_t)isr31, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);

    idt_set_gate(32, (uint64_t)irq0,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(33, (uint64_t)irq1,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(34, (uint64_t)irq2,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(35, (uint64_t)irq3,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(36, (uint64_t)irq4,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(37, (uint64_t)irq5,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(38, (uint64_t)irq6,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(39, (uint64_t)irq7,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(40, (uint64_t)irq8,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(41, (uint64_t)irq9,  GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(42, (uint64_t)irq10, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(43, (uint64_t)irq11, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(44, (uint64_t)irq12, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(45, (uint64_t)irq13, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(46, (uint64_t)irq14, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(47, (uint64_t)irq15, GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    
    idt_flush((uint64_t)&idtr);
}
