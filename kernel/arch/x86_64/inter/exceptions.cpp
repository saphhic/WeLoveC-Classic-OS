// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Handles CPU exceptions and their diagnostics.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

//   The C-side counterpart of isr.asm: isr_handler() runs for every CPU
//   exception (vectors 0-31), irq_handler() runs for every remapped
//   hardware IRQ (32-47). Keeping these here (rather than inline in
//   idt.cpp) means idt.cpp only ever deals with "how do I register a
//   vector", not "what do I do when it fires".

#include "isr.h"
#include "pic.h"
#include "../panic/panic.h"
#include "../io/serial.h"
#include "arch/common/Process/manager.hpp"

static const char *exception_name(uint8_t vector) {
    static const char *names[32] = {
        "Divide-by-zero",              "Debug",
        "Non-Maskable Interrupt",      "Breakpoint",
        "Overflow",                    "Bound Range Exceeded",
        "Invalid Opcode",              "Device Not Available",
        "Double Fault",                "Coprocessor Segment Overrun",
        "Invalid TSS",                 "Segment Not Present",
        "Stack-Segment Fault",         "General Protection Fault",
        "Page Fault",                  "Reserved",
        "x87 Floating-Point Error",    "Alignment Check",
        "Machine Check",               "SIMD Floating-Point Exception",
        "Virtualization Exception",    "Control Protection Exception",
        "Reserved",                    "Reserved",
        "Reserved",                    "Reserved",
        "Reserved",                    "Reserved",
        "Hypervisor Injection",        "VMM Communication Exception",
        "Security Exception",          "Reserved",
    };

    if (vector < 32)
        return names[vector];
    
    return "Unknown exception";
}

extern "C" void isr_handler(registers_t *regs) {
    if (regs && (regs->cs & 3) == 3 && welovec::process::current_id()) {
        auto write_hex = [](uint64_t value) {
            static const char digits[] = "0123456789abcdef";
            char output[16];
            for (int index = 15; index >= 0; --index) {
                output[index] = digits[value & 0xf];
                value >>= 4;
            }
            for (int index = 0; index < 16; ++index)
                serial_write_char(output[index]);
        };
        serial_write("[user fault] pid=");
        write_hex(welovec::process::current_id());
        serial_write(" vector=");
        write_hex(regs->int_no);
        serial_write(" error=");
        write_hex(regs->err_code);
        serial_write(" rip=");
        write_hex(regs->rip);
        serial_write("\n");
        welovec::process::exit_current(-static_cast<int64_t>(regs->int_no) - 1);
    }
    panic(exception_name(regs->int_no), regs);
}

static isr_handler_t irq_handlers[16] = { 0 };

void irq_install_handler(int irq_num, isr_handler_t handler) {
    if (irq_num < 0 || irq_num >= 16)
        return;

    irq_handlers[irq_num] = handler;
}

void irq_uninstall_handler(int irq_num) {
    if (irq_num < 0 || irq_num >= 16)
        return;

    irq_handlers[irq_num] = 0;
}

extern "C" registers_t *scheduler_timer_dispatch(registers_t *regs);

extern "C" registers_t *irq_handler(registers_t *regs) {
    if (!regs || regs->int_no < 32 || regs->int_no > 47) return regs;
    const int irq_num = static_cast<int>(regs->int_no - 32);

    if (irq_handlers[irq_num]) irq_handlers[irq_num](regs);
    pic_send_eoi(static_cast<uint8_t>(irq_num));
    if (irq_num == 0) return scheduler_timer_dispatch(regs);
    return regs;
}
