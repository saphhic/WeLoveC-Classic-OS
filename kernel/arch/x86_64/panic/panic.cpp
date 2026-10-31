// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Displays fatal error state and stops the kernel.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

// program for [panic.h](<DyntKernel/arch/x86_64/panic.h>)

#include "panic.h"
#include "../inter/isr.h"
#include "../io/serial.h"
#include "../cpu/cpu.h"

static void print_hex64(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char buf[19];

    buf[0] = '0';
    buf[1] = 'x';
    buf[18] = '\0';

    for (int i = 0; i < 16; i++) {
        buf[17 - i] = digits[value & 0xF];
        value >>= 4;
    }

    serial_write(buf);
}

[[noreturn]] void panic(const char *message, registers_t *regs) {
    cli();

    serial_write("\nKERNEL PANIC!: \n");
    serial_write("Oh noo, seems like WeLoveC Kernel experienced a crash.\n");
    serial_write("restart is needed. ERROR:\n");
    serial_write(message);
    serial_write("\n");
    
    if (regs) {
        serial_write("vector="); print_hex64(regs->int_no);
        serial_write("  err="); print_hex64(regs->err_code);
        serial_write("\n");

        serial_write("rip="); print_hex64(regs->rip);
        serial_write("  cs="); print_hex64(regs->cs);
        serial_write("  rflags="); print_hex64(regs->rflags);
        serial_write("\n");

        serial_write("rax="); print_hex64(regs->rax);
        serial_write("  rbx="); print_hex64(regs->rbx);
        serial_write("  rcx="); print_hex64(regs->rcx);
        serial_write("  rdx="); print_hex64(regs->rdx);
        serial_write("\n");

        serial_write("rsi="); print_hex64(regs->rsi);
        serial_write("  rdi="); print_hex64(regs->rdi);
        serial_write("  rbp="); print_hex64(regs->rbp);
        serial_write("  rsp="); print_hex64(regs->user_rsp);
        serial_write("\n");
    }

    serial_write("system halted. \n");

    for (;;) {
        hlt();
    }
}
