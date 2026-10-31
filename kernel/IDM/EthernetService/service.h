// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for service in the ethernet service module.

#ifndef WELOVEC_ETHERNET_SERVICE_C_API_H
#define WELOVEC_ETHERNET_SERVICE_C_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int welovec_ethernet_initialize(void);
int welovec_network_initialize(void);
int welovec_network_start_polling(void);
/* Kernel-only serialization shared by the network poller and syscall clients. */
/* These guards serialize stack access while preserving the caller's IF state.
   Interrupts are enabled while held so bounded network waits stay preemptible. */
uint64_t welovec_network_api_lock(void);
void welovec_network_api_unlock(uint64_t saved_flags);

#ifdef __cplusplus
}
#endif

#endif
