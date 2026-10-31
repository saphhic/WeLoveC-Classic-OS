// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for ports in the io module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

// no functioms, only declared constants
// so its easier and safer to use I/O in progrmas and more understandable 

#ifndef ARCH_X86_64_PORTS_H
#define ARCH_X86_64_PORTS_H

#define PIC1_COMMAND   0x20
#define PIC1_DATA      0x21

#define PIC2_COMMAND   0xA0
#define PIC2_DATA      0xA1

#define PIT_COMMAND    0x43
#define PIT_CHANNEL0   0x40

#define CMOS_ADDRESS   0x70
#define CMOS_DATA      0x71

#define PS2_DATA       0x60
#define PS2_STATUS     0x64
#define PS2_COMMAND    0x64

#endif // ARCH_X86_64_PORTS_H