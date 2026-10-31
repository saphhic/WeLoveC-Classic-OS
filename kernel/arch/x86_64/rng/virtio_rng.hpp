// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for virtio rng in the rng module.

#ifndef WELOVEC_X86_64_VIRTIO_RNG_HPP
#define WELOVEC_X86_64_VIRTIO_RNG_HPP

#include <stdint.h>

namespace welovec::x86_64::rng {

/* Read bytes from an attached modern virtio-rng PCI device. */
bool fill_from_virtio(uint8_t *output, uint64_t size);

}

#endif
