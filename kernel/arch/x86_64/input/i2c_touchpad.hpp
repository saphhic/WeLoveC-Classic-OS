// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for i2c touchpad in the input module.

#ifndef WELOVEC_X86_64_INPUT_I2C_TOUCHPAD_HPP
#define WELOVEC_X86_64_INPUT_I2C_TOUCHPAD_HPP

struct WelovecBootInfo;

namespace welovec::x86_64::input::i2c_touchpad {

bool initialize(const WelovecBootInfo *boot_info);

}

#endif
