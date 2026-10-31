// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the service component in the ethernet service module.

#include "service.h"

#include "ethernet.h"
#include "netstack.h"
#include "arch/x86_64/scheduler/scheduler.h"

namespace {

scheduler_task_id_t g_network_polling_task = 0;
uint32_t g_network_api_lock = 0;

void network_polling_task(void *) {
    for (;;) {
        const uint64_t network_flags = welovec_network_api_lock();
        netstack_poll();
        welovec_network_api_unlock(network_flags);
        scheduler_sleep(1);
    }
}

}

extern "C" uint64_t welovec_network_api_lock(void) {
    uint64_t saved_flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(saved_flags) :: "memory");

    while (__atomic_exchange_n(&g_network_api_lock, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("sti" ::: "memory");
        if (!scheduler_sleep(1)) scheduler_yield();
    }

    /* Syscalls enter with IF masked. Keep the lock owner preemptible while it
       polls for ARP, ICMP, DNS or TCP responses. */
    __asm__ volatile("sti" ::: "memory");
    return saved_flags;
}

extern "C" void welovec_network_api_unlock(uint64_t saved_flags) {
    __asm__ volatile("cli" ::: "memory");
    __atomic_store_n(&g_network_api_lock, 0, __ATOMIC_RELEASE);
    if (saved_flags & (UINT64_C(1) << 9))
        __asm__ volatile("sti" ::: "memory");
}

extern "C" int welovec_ethernet_initialize(void) {
    return ethernet_init() ? 1 : 0;
}

extern "C" int welovec_network_initialize(void) {
    return netstack_init() ? 1 : 0;
}

extern "C" int welovec_network_start_polling(void) {
    if (g_network_polling_task != 0) return 1;

    netstack_info info = {};
    netstack_get_info(&info);
    if (!info.initialized) return 0;

    g_network_polling_task = scheduler_create_kernel_thread(
        "network-poll", network_polling_task, nullptr, SCHEDULER_PRIORITY_BACKGROUND);
    return g_network_polling_task != 0 ? 1 : 0;
}
