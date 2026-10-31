// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for ps2 controller in the input module.

#ifndef WELOVEC_X86_64_PS2_CONTROLLER_HPP
#define WELOVEC_X86_64_PS2_CONTROLLER_HPP

#include <stdint.h>

namespace welovec::x86_64::input::ps2 {

using AuxiliaryByteHandler = void (*)(uint8_t byte);

bool initialize_controller();
bool enable_auxiliary_port();
bool configure_auxiliary(bool enabled);
bool send_auxiliary_byte(uint8_t value);
bool read_auxiliary_response(uint8_t *value);
void install_auxiliary_handler(AuxiliaryByteHandler handler);
bool start_polling_worker();

}

#endif
