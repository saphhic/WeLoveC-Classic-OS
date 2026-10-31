// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for mouse in the input module.

#ifndef WELOVEC_X86_64_MOUSE_H
#define WELOVEC_X86_64_MOUSE_H

#include <stdint.h>
#include "include/user/input_event.h"

#ifdef __cplusplus
extern "C" {
#endif

int64_t welovec_pointer_read(welovec_pointer_event *events, uint64_t capacity);
uint32_t welovec_pointer_sources(void);
int welovec_ps2_synaptics_detected(void);
const char *welovec_i2c_touchpad_status(void);

#ifdef __cplusplus
}
#endif

#endif
