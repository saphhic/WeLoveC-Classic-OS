// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for serial in the io module.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: 26 July 2026

//   <<NOT>> a real driver (no IRQ-driven RX, no buffering yet) — just enough
//   polled TX on COM1 to get panic messages and early boot logs out
//   before a framebuffer console. QEMU/VirtualBox both wire
//   COM1 to a file or the host terminal by default, which makes this
//   the cheapest reliable debug channel we'll have for a long time.

//   When a Frambuffer terminal is added in DyntOS this will have to be partially changed so
//   debug errors print in the terminal and not only in vm COM1.

#ifndef ARCH_X86_64_SERIAL_H
#define ARCH_X86_64_SERIAL_H

#ifdef __cplusplus
extern "C" {
#endif

void serial_init(void);
void serial_write_char(char c);
void serial_write(const char *str);

#ifdef __cplusplus
}
#endif

#endif // ARCH_X86_64_SERIAL_H
