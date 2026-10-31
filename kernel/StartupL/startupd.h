// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for startupd in the startup module.

#ifndef WELOVEC_STARTUPD_H
#define WELOVEC_STARTUPD_H

/* Finds a startup .conf in /bin or /userspace/bin and launches its ELF.
   Falls back to init.elf when there is no usable configuration. */
int welovec_startupd_run(void);

#endif
