// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for vfs in the filesystem module.

//written by saphhic.

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif
#include "boot/boot_protocol.h"

#define MAX_FILENAME 1024
#define MAX_FILES 256
#define MAX_LIST_ENTRIES 32

typedef struct {
    char name[MAX_FILENAME];
    char* content;
    uint32_t size;
    bool is_directory;
} FileNode;

typedef struct {
    char name[MAX_FILENAME];
    uint32_t size;
    bool is_directory;
} VfsListEntry;

#ifdef __cplusplus
extern "C" {
#endif

extern FileNode file_system[MAX_FILES];
extern int file_count;

void fs_init();
int welovec_filesystem_initialize(const WelovecBootInfo* boot_info);

void vfs_resolve_path(const char* current, const char* in, char* out);
void vfs_get_parent(const char* path, char* out);
void vfs_join_path(const char* base, const char* child, bool is_dir, char* out);
void vfs_copy_text(char* dst, int cap, const char* src);

bool dir(const char* path);
bool make_dir(const char* path);
bool save_file(const char* path, const char* content);
bool append_file(const char* path, const char* content);
bool delete_file(const char* path);
bool move_file(const char* old_path, const char* new_path);

bool file_exists(const char* path);
bool fs_is_directory(const char* path);
uint32_t fs_file_size(const char* path);
char* read_file(const char* path);
bool vfs_read_file(const char* path, uint8_t** out_data, uint32_t* out_size);
bool vfs_write_file(const char* path, const uint8_t* data, uint32_t size);
bool vfs_write_file_range(const char* path, uint32_t offset,
                          const uint8_t* data, uint32_t size);
void vfs_free_file(void* data);
const FileNode* fs_get_node(const char* path);
int fs_list(const char* path, VfsListEntry* out_entries, int max_entries);

#ifdef __cplusplus
}
#endif

#endif // VFS_H
