// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for xhci in the input module.

#ifndef WELOVEC_X86_64_XHCI_HPP
#define WELOVEC_X86_64_XHCI_HPP

namespace welovec::x86_64::input::usb {

bool initialize();
bool start_probe();
const char *probe_status();

}

extern "C" const char *welovec_xhci_probe_status(void);

#endif
