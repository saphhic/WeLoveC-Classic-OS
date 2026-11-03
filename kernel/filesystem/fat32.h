// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for fat32 in the filesystem module.

#ifndef WELOVEC_FILESYSTEM_FAT32_H
#define WELOVEC_FILESYSTEM_FAT32_H

#include <stdint.h>
#include "vfs.h"
#include "arch/common/Block/block_device.hpp"

// The memory image must remain mapped and writable for the life of the mount.
bool fat32_mount(void *image, uint64_t image_size);
// first_block/block_count are expressed in the registered device's block units.
bool fat32_mount_device(welovec::block::Device *device, uint64_t first_block,
                       uint64_t block_count);
bool fat32_mount_auto();
bool fat32_is_mounted();
bool fat32_path_is_mounted(const char *path, char *relative_path, uint32_t capacity);

bool fat32_file_exists(const char *path);
bool fat32_is_directory(const char *path);
uint32_t fat32_file_size(const char *path);
bool fat32_read_file(const char *path, uint8_t **out_data, uint32_t *out_size);
bool fat32_write_file(const char *path, const uint8_t *data, uint32_t size);
bool fat32_make_directory(const char *path);
bool fat32_delete(const char *path);
bool fat32_move(const char *old_path, const char *new_path);
int fat32_list(const char *path, VfsListEntry *out_entries, int max_entries);
int fat32_list_range(const char *path, VfsListEntry *out_entries,
                     int max_entries, uint64_t offset);

#endif
