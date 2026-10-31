// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Coordinates initialization of physical memory, virtual memory, and the heap.

#include "memory_manager.h"
#include "PMM/pmm.hpp"
#include "VMM/vmm.hpp"
#include "Heap/heap.hpp"
#include "arch/x86_64/mm/paging/page_table.hpp"
#include "arch/x86_64/io/console.h"

extern "C" int welovec_memory_manager_initialize(const WelovecBootInfo *boot_info) {
    kernel_console_write("  MM: checking active page tables...\n");
    if (!welovec::x86_64::mm::paging::initialize(boot_info)) {
        kernel_console_write("  MM: page-table handoff rejected.\n");
        return 0;
    }
    kernel_console_write("  MM: initializing physical frame allocator...\n");
    if (!welovec::memory::pmm::initialize(boot_info)) {
        kernel_console_write("  MM: physical frame allocator rejected the UEFI map.\n");
        return 0;
    }
    kernel_console_write("  MM: initializing virtual address allocator...\n");
    if (!welovec::memory::vmm::initialize()) {
        kernel_console_write("  MM: virtual address allocator initialization failed.\n");
        return 0;
    }
    kernel_console_write("  MM: initializing kernel heap...\n");
    if (!welovec::memory::heap::initialize()) {
        kernel_console_write("  MM: kernel heap initialization failed.\n");
        return 0;
    }
    kernel_console_write("  MM: all memory subsystems ready.\n");
    return 1;
}
