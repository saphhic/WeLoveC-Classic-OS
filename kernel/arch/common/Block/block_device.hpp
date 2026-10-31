// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for block device in the block module.

#ifndef WELOVEC_BLOCK_DEVICE_HPP
#define WELOVEC_BLOCK_DEVICE_HPP

#include <stdint.h>
#include "boot/boot_protocol.h"

namespace welovec::block {

constexpr uint32_t kMaximumDevices = 16;

struct Device;
using ReadBlocks = bool (*)(Device *, uint64_t first_block, uint32_t count, void *buffer);
using WriteBlocks = bool (*)(Device *, uint64_t first_block, uint32_t count, const void *buffer);
using Flush = bool (*)(Device *);

struct Device {
    char name[32];
    void *context;
    uint64_t block_count;
    uint32_t block_size;
    bool read_only;
    ReadBlocks read;
    WriteBlocks write;
    Flush flush;
};

bool initialize(const WelovecBootInfo *boot_info);
bool register_device(Device *device);
uint32_t device_count();
Device *device_at(uint32_t index);
bool read(Device *device, uint64_t first_block, uint32_t count, void *buffer);
bool write(Device *device, uint64_t first_block, uint32_t count, const void *buffer);
bool flush(Device *device);

namespace ata_pio {
uint32_t initialize();
}

}

#endif
