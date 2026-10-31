// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Provides kernel console output over the serial port.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

// program for [serial.h](<DyntKernel/arch/x86_64/serial.h>)

#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static int transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_write_char(char c) {
    while (!transmit_empty());
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *str) {
    while (*str) {
        if (*str == '\n')
            serial_write_char('\r');
        serial_write_char(*str);
        str++;
    }
}