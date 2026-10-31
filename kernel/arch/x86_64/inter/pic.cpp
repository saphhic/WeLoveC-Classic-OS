// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the pic component in the inter module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

// program for [pic.h](<DyntKernel/arch/x86_64/pic.h>)

#include "pic.h"
#include "../io/io.h"
#include "../io/ports.h"

#define ICW1_ICW4 0x01
#define ICW1_INIT 0x10
#define ICW1_8086 0x01

static uint8_t pic1_mask = 0xFF;
static uint8_t pic2_mask = 0xFF;

void pic_remap(uint8_t offset1, uint8_t offset2) {
    pic1_mask = inb(PIC1_DATA);
    pic2_mask = inb(PIC2_DATA);

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC1_DATA, offset1);
    io_wait();
    outb(PIC2_DATA, offset2);
    io_wait();
    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();
    outb(PIC1_DATA, pic1_mask);
    outb(PIC2_DATA, pic2_mask);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = inb(port) | (uint8_t)(1 << irq);
    outb(port, value);
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = inb(port) & ~(uint8_t)(1 << irq);
    outb(port, value);
}
