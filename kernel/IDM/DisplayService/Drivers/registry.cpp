// Static driver registry. Add new PCI GPU drivers here as they are implemented.

#include "gpu_driver.hpp"

namespace welovec::display::gpu {

namespace {
const Driver *const kDrivers[] = {
    &kRembrandtDriver
};
}

const Driver *const *registered_drivers(uint32_t *out_count) {
    if (out_count) {
        *out_count = static_cast<uint32_t>(
            sizeof(kDrivers) / sizeof(kDrivers[0]));
    }
    return kDrivers;
}

}
