// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for pci in the pci module.

#ifndef WELOVEC_X86_64_PCI_HPP
#define WELOVEC_X86_64_PCI_HPP

#include <stdint.h>

namespace welovec::x86_64::pci {

struct Device {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
};

enum class BarType : uint8_t {
    Absent,
    Io,
    MemoryBelow1MiB,
    Memory32,
    Memory64,
    Continuation,
    Reserved
};

struct Bar {
    uint8_t index;
    BarType type;
    bool assigned;
    bool prefetchable;
    uint64_t address;
};

constexpr uint8_t kMaximumBars = 6;

using Visitor = bool (*)(const Device &, void *);

bool enumerate(Visitor visitor, void *context);
uint8_t read8(const Device &device, uint8_t offset);
uint16_t read16(const Device &device, uint8_t offset);
uint32_t read32(const Device &device, uint8_t offset);
void write8(const Device &device, uint8_t offset, uint8_t value);
void write16(const Device &device, uint8_t offset, uint16_t value);
void write32(const Device &device, uint8_t offset, uint32_t value);
uint64_t memory_bar_address(const Device &device, uint8_t index);
/* Reads BAR layout without changing configuration space. A 64-bit BAR uses
   the following entry as a Continuation slot. No BAR sizing probe is done. */
bool read_bars(const Device &device, Bar out[kMaximumBars], uint8_t *out_count);
bool enable_memory_and_bus_master(const Device &device);

}

#endif
