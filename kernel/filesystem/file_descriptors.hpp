// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for file descriptors in the filesystem module.

#ifndef WELOVEC_FILESYSTEM_FILE_DESCRIPTORS_HPP
#define WELOVEC_FILESYSTEM_FILE_DESCRIPTORS_HPP

#include <stdint.h>

namespace welovec::filesystem {

constexpr uint32_t kOpenRead = 1u << 0;
constexpr uint32_t kOpenWrite = 1u << 1;
constexpr uint32_t kOpenCreate = 1u << 2;
constexpr uint32_t kOpenTruncate = 1u << 3;
constexpr uint32_t kOpenAppend = 1u << 4;

struct DescriptorInfo {
    char path[1024];
    uint64_t offset;
    uint32_t flags;
};

int32_t open_descriptor(uint64_t process_id, const char *path, uint32_t flags);
bool close_descriptor(uint64_t process_id, int32_t descriptor);
bool get_descriptor(uint64_t process_id, int32_t descriptor, DescriptorInfo *out);
bool set_descriptor_offset(uint64_t process_id, int32_t descriptor, uint64_t offset);
void close_process_descriptors(uint64_t process_id);

}

#endif
