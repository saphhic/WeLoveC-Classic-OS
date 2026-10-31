// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the virtual filesystem layer and its operations.

#include "vfs.h"

#include "fat32.h"
#include "arch/common/Block/block_device.hpp"
#include "arch/common/MM/Heap/heap.hpp"
#include "arch/x86_64/io/serial.h"

FileNode file_system[MAX_FILES];
int file_count = 0;

namespace {
bool gFsReady;

uint32_t text_length(const char *text) {
    uint32_t length = 0;
    if (text) while (text[length]) ++length;
    return length;
}

bool starts_with(const char *text, const char *prefix) {
    if (!text || !prefix) return false;
    while (*prefix) if (*text++ != *prefix++) return false;
    return true;
}

bool is_root(const char *path) {
    return path && path[0] == '/' && path[1] == '\0';
}

bool normalize_path(const char *path, char *out, uint32_t capacity, bool directory) {
    if (!path || !out || capacity < 2) return false;
    struct Component { uint32_t start; uint32_t length; };
    Component components[MAX_FILENAME / 2];
    uint32_t component_count = 0;
    uint32_t i = 0;
    while (path[i] == '/' || path[i] == '\\') ++i;

    while (path[i]) {
        const uint32_t start = i;
        while (path[i] && path[i] != '/' && path[i] != '\\') ++i;
        const uint32_t length = i - start;
        if (length == 1 && path[start] == '.') {
            // Ignore current-directory components.
        } else if (length == 2 && path[start] == '.' && path[start + 1] == '.') {
            if (component_count) --component_count;
        } else if (length) {
            if (component_count >= sizeof(components) / sizeof(components[0])) return false;
            components[component_count++] = {start, length};
        }
        while (path[i] == '/' || path[i] == '\\') ++i;
    }

    uint32_t position = 0;
    out[position++] = '/';
    for (uint32_t c = 0; c < component_count; ++c) {
        const uint32_t start = components[c].start;
        const uint32_t length = components[c].length;
        if (position > 1) {
            if (position + 1 >= capacity) return false;
            out[position++] = '/';
        }
        if (length >= capacity - position) return false;
        for (uint32_t j = 0; j < length; ++j) out[position++] = path[start + j];
    }
    if (directory && component_count && position + 1 < capacity) out[position++] = '/';
    out[position] = '\0';
    return true;
}

int find_exact(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < file_count; ++i) {
        uint32_t j = 0;
        while (name[j] && file_system[i].name[j] && name[j] == file_system[i].name[j]) ++j;
        if (!name[j] && !file_system[i].name[j]) return i;
    }
    return -1;
}

int find_node(const char *path) {
    char normalized[MAX_FILENAME];
    if (!normalize_path(path, normalized, sizeof(normalized), false)) return -1;
    int index = find_exact(normalized);
    if (index >= 0 || is_root(normalized)) return index;
    const uint32_t length = text_length(normalized);
    if (length + 1 >= sizeof(normalized)) return -1;
    normalized[length] = '/';
    normalized[length + 1] = '\0';
    return find_exact(normalized);
}

bool fat_path(const char *path, char *relative) {
    return fat32_path_is_mounted(path, relative, MAX_FILENAME);
}

bool path_is_directory_form(const char *path) {
    const uint32_t length = text_length(path);
    return length && (path[length - 1] == '/' || path[length - 1] == '\\');
}

bool alloc_content(FileNode *node, const char *content) {
    if (!node) return false;
    const uint32_t length = text_length(content);
    char *copy = static_cast<char *>(welovec::memory::heap::allocate(length + 1));
    if (!copy) return false;
    for (uint32_t i = 0; i < length; ++i) copy[i] = content[i];
    copy[length] = '\0';
    if (node->content) welovec::memory::heap::deallocate(node->content);
    node->content = copy;
    node->size = length;
    return true;
}

bool create_node(const char *normalized, bool directory, const char *content) {
    if (!normalized || file_count >= MAX_FILES) return false;
    FileNode &node = file_system[file_count];
    vfs_copy_text(node.name, MAX_FILENAME, normalized);
    node.content = nullptr;
    node.size = 0;
    node.is_directory = directory;
    if (!directory && !alloc_content(&node, content ? content : "")) return false;
    ++file_count;
    return true;
}

bool ensure_parent_dirs(const char *normalized) {
    const uint32_t length = text_length(normalized);
    char parent[MAX_FILENAME];
    for (uint32_t i = 1; i < length; ++i) {
        if (normalized[i] != '/') continue;
        uint32_t count = i + 1;
        for (uint32_t j = 0; j < count; ++j) parent[j] = normalized[j];
        parent[count] = '\0';
        const int index = find_exact(parent);
        if (index >= 0) {
            if (!file_system[index].is_directory) return false;
        } else if (!create_node(parent, true, nullptr)) return false;
    }
    return true;
}

void seed_ram_filesystem() {
    file_count = 0;
    for (uint32_t i = 0; i < MAX_FILES; ++i) {
        file_system[i].name[0] = '\0';
        file_system[i].content = nullptr;
        file_system[i].size = 0;
        file_system[i].is_directory = false;
    }
    create_node("/", true, nullptr);
    make_dir("/home/");
    make_dir("/home/user/");
    make_dir("/home/user/Desktop/");
    make_dir("/home/user/Documents/");
    make_dir("/system/");
    make_dir("/bin/");
    make_dir("/tmp/");
    // FAT volume mounts are exposed as sibling paths to keep the project's
    // source tree separate from the executable userspace tree.
    create_node("/userspace/", true, nullptr);
    create_node("/bootloader/", true, nullptr);
    create_node("/kernel/", true, nullptr);
    create_node("/userland/", true, nullptr);
    save_file("/home/user/readme.txt",
              "Welcome to KryptonOS VFS.\nThis filesystem lives in RAM for this boot session.\n");
    save_file("/home/user/Documents/notes.txt",
              "Mouse, framebuffer desktop and VFS are online.\n");
    save_file("/home/user/demo.html",
              "<!doctype html><html><head><style>"
              "body{background:#f8fafc;color:#172033;font-size:16px;margin:8px;}"
              "h1{color:#0757c8;margin-bottom:8px;}"
              ".panel{background:#fff;border:1px solid #c8d1df;padding:10px;margin:8px 0;}"
              ".ok{color:#15803d;font-weight:bold;}"
              "a{color:#0757c8;text-decoration:underline;}"
              "</style></head><body><h1>KryptonOS HTML/CSS</h1>"
              "<div class=\"panel\">Este archivo vive en el VFS y prueba estilos CSS.</div>"
              "<p class=\"ok\">Renderer activo.</p>"
              "<script>document.write('<p class=\"ok\">JS basico activo.</p>');</script>"
              "<p><a href=\"http://example.com/\">Enlace de ejemplo</a></p>"
              "</body></html>");
    save_file("/system/about.txt", "KryptonOS framebuffer desktop\nVFS: in-memory hierarchical filesystem\n");
}
}

extern "C" int welovec_filesystem_initialize(const WelovecBootInfo *boot_info) {
    if (!welovec::block::initialize(boot_info)) return 0;
    (void)fat32_mount_auto();
    fs_init();
    return 1;
}

void vfs_copy_text(char *destination, int capacity, const char *source) {
    if (!destination || capacity <= 0) return;
    uint32_t i = 0;
    if (source) while (source[i] && i + 1 < static_cast<uint32_t>(capacity)) {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

void vfs_resolve_path(const char *current, const char *input, char *out) {
    if (!out) return;
    char joined[MAX_FILENAME * 2];
    uint32_t position = 0;
    if (input && (input[0] == '/' || input[0] == '\\')) {
        vfs_copy_text(joined, sizeof(joined), input);
    } else {
        const char *base = current && current[0] ? current : "/";
        const uint32_t base_length = text_length(base);
        for (uint32_t i = 0; i < base_length && position + 1 < sizeof(joined); ++i)
            joined[position++] = base[i];
        if (position && joined[position - 1] != '/' && position + 1 < sizeof(joined))
            joined[position++] = '/';
        for (uint32_t i = 0; input && input[i] && position + 1 < sizeof(joined); ++i)
            joined[position++] = input[i];
        joined[position] = '\0';
    }
    if (!normalize_path(joined, out, MAX_FILENAME, false)) vfs_copy_text(out, MAX_FILENAME, "/");
}

void vfs_get_parent(const char *path, char *out) {
    if (!out) return;
    char normalized[MAX_FILENAME];
    if (!normalize_path(path, normalized, sizeof(normalized), false) || is_root(normalized)) {
        vfs_copy_text(out, MAX_FILENAME, "/");
        return;
    }
    uint32_t length = text_length(normalized);
    while (length && normalized[length - 1] != '/') --length;
    if (length <= 1) vfs_copy_text(out, MAX_FILENAME, "/");
    else {
        for (uint32_t i = 0; i < length - 1; ++i) out[i] = normalized[i];
        out[length - 1] = '\0';
    }
}

void vfs_join_path(const char *base, const char *child, bool is_dir, char *out) {
    if (!out) return;
    char joined[MAX_FILENAME * 2];
    uint32_t position = 0;
    const char *root = base && base[0] ? base : "/";
    for (uint32_t i = 0; root[i] && position + 1 < sizeof(joined); ++i)
        joined[position++] = root[i];
    if (position && joined[position - 1] != '/' && position + 1 < sizeof(joined))
        joined[position++] = '/';
    for (uint32_t i = 0; child && child[i] && position + 1 < sizeof(joined); ++i)
        joined[position++] = child[i];
    joined[position] = '\0';
    if (!normalize_path(joined, out, MAX_FILENAME, is_dir)) vfs_copy_text(out, MAX_FILENAME, "/");
}

void fs_init() {
    if (gFsReady) return;
    seed_ram_filesystem();
    gFsReady = true;
}

bool make_dir(const char *path) {
    char relative[MAX_FILENAME];
    if (fat_path(path, relative)) return fat32_make_directory(relative);
    char normalized[MAX_FILENAME];
    if (!normalize_path(path, normalized, sizeof(normalized), true)) return false;
    const int existing = find_exact(normalized);
    if (existing >= 0) return file_system[existing].is_directory;
    return ensure_parent_dirs(normalized) && create_node(normalized, true, nullptr);
}

bool file_exists(const char *path) {
    char relative[MAX_FILENAME];
    if (fat_path(path, relative)) return fat32_file_exists(relative);
    return find_node(path) >= 0;
}

bool fs_is_directory(const char *path) {
    char relative[MAX_FILENAME];
    if (fat_path(path, relative)) return fat32_is_directory(relative);
    const int index = find_node(path);
    return index >= 0 && file_system[index].is_directory;
}

uint32_t fs_file_size(const char *path) {
    char relative[MAX_FILENAME];
    if (fat_path(path, relative)) return fat32_file_size(relative);
    const int index = find_node(path);
    return index >= 0 && !file_system[index].is_directory ? file_system[index].size : 0;
}

const FileNode *fs_get_node(const char *path) {
    char relative[MAX_FILENAME];
    if (fat_path(path, relative)) return nullptr;
    const int index = find_node(path);
    return index >= 0 ? &file_system[index] : nullptr;
}

bool save_file(const char *path, const char *content) {
    if (!path) return false;
    char relative[MAX_FILENAME];
    if (fat_path(path, relative)) {
        if (path_is_directory_form(path)) return fat32_make_directory(relative);
        const uint32_t length = text_length(content);
        return fat32_write_file(relative, reinterpret_cast<const uint8_t *>(content), length);
    }
    if (path_is_directory_form(path)) return make_dir(path);
    char normalized[MAX_FILENAME];
    if (!normalize_path(path, normalized, sizeof(normalized), false) || is_root(normalized) ||
        !ensure_parent_dirs(normalized)) return false;
    const int index = find_exact(normalized);
    if (index < 0) return create_node(normalized, false, content ? content : "");
    if (file_system[index].is_directory) return false;
    return alloc_content(&file_system[index], content ? content : "");
}

bool append_file(const char *path, const char *content) {
    if (!path || !content) return false;
    char relative[MAX_FILENAME];
    if (fat_path(path, relative)) {
        uint8_t *old_data = nullptr;
        uint32_t old_size = 0;
        if (!fat32_read_file(relative, &old_data, &old_size))
            return save_file(path, content);
        const uint32_t addition = text_length(content);
        if (old_size == UINT32_MAX || addition > UINT32_MAX - old_size - 1) {
            welovec::memory::heap::deallocate(old_data);
            return false;
        }
        auto *joined = static_cast<uint8_t *>(welovec::memory::heap::allocate(old_size + addition + 1));
        if (!joined) {
            welovec::memory::heap::deallocate(old_data);
            return false;
        }
        for (uint32_t i = 0; i < old_size; ++i) joined[i] = old_data[i];
        for (uint32_t i = 0; i < addition; ++i) joined[old_size + i] = content[i];
        joined[old_size + addition] = 0;
        const bool result = fat32_write_file(relative, joined, old_size + addition);
        welovec::memory::heap::deallocate(joined);
        welovec::memory::heap::deallocate(old_data);
        return result;
    }
    const int index = find_node(path);
    if (index < 0) return save_file(path, content);
    if (file_system[index].is_directory) return false;
    const uint32_t old_size = file_system[index].size;
    const uint32_t addition = text_length(content);
    if (old_size == UINT32_MAX || addition > UINT32_MAX - old_size - 1) return false;
    auto *joined = static_cast<char *>(welovec::memory::heap::allocate(old_size + addition + 1));
    if (!joined) return false;
    for (uint32_t i = 0; i < old_size; ++i) joined[i] = file_system[index].content[i];
    for (uint32_t i = 0; i < addition; ++i) joined[old_size + i] = content[i];
    joined[old_size + addition] = '\0';
    welovec::memory::heap::deallocate(file_system[index].content);
    file_system[index].content = joined;
    file_system[index].size = old_size + addition;
    return true;
}

bool delete_file(const char *path) {
    char relative[MAX_FILENAME];
    if (fat_path(path, relative)) return fat32_delete(relative);
    const int index = find_node(path);
    if (index < 0 || is_root(file_system[index].name)) return false;
    char prefix[MAX_FILENAME];
    vfs_copy_text(prefix, sizeof(prefix), file_system[index].name);
    const bool directory = file_system[index].is_directory;
    int output = 0;
    for (int input = 0; input < file_count; ++input) {
        const bool remove = input == index ||
            (directory && input != index && starts_with(file_system[input].name, prefix));
        if (remove) {
            if (file_system[input].content)
                welovec::memory::heap::deallocate(file_system[input].content);
            continue;
        }
        if (output != input) file_system[output] = file_system[input];
        ++output;
    }
    file_count = output;
    return true;
}

bool move_file(const char *old_path, const char *new_path) {
    if (!old_path || !new_path) return false;
    char old_relative[MAX_FILENAME], new_relative[MAX_FILENAME];
    const bool old_fat = fat_path(old_path, old_relative);
    const bool new_fat = fat_path(new_path, new_relative);
    if (old_fat || new_fat) return old_fat && new_fat && fat32_move(old_relative, new_relative);
    const int index = find_node(old_path);
    if (index < 0 || is_root(file_system[index].name)) return false;
    const bool directory = file_system[index].is_directory;
    char old_name[MAX_FILENAME], new_name[MAX_FILENAME];
    vfs_copy_text(old_name, sizeof(old_name), file_system[index].name);
    if (!normalize_path(new_path, new_name, sizeof(new_name), directory) ||
        find_exact(new_name) >= 0 || !ensure_parent_dirs(new_name)) return false;
    const uint32_t old_length = text_length(old_name), new_length = text_length(new_name);
    if (directory && starts_with(new_name, old_name)) return false;
    for (int i = 0; directory && i < file_count; ++i) {
        if (i == index || !starts_with(file_system[i].name, old_name)) continue;
        if (new_length + text_length(file_system[i].name + old_length) >= MAX_FILENAME) return false;
    }
    vfs_copy_text(file_system[index].name, MAX_FILENAME, new_name);
    if (directory) for (int i = 0; i < file_count; ++i) {
        if (i == index || !starts_with(file_system[i].name, old_name)) continue;
        char updated[MAX_FILENAME];
        vfs_copy_text(updated, sizeof(updated), new_name);
        const uint32_t length = text_length(updated);
        vfs_copy_text(updated + length, MAX_FILENAME - length,
                      file_system[i].name + old_length);
        vfs_copy_text(file_system[i].name, MAX_FILENAME, updated);
    }
    return true;
}

char *read_file(const char *path) {
    char relative[MAX_FILENAME];
    if (fat_path(path, relative)) {
        uint8_t *data = nullptr;
        uint32_t size = 0;
        if (!fat32_read_file(relative, &data, &size)) return nullptr;
        (void)size;
        return reinterpret_cast<char *>(data);
    }
    const int index = find_node(path);
    return index >= 0 && !file_system[index].is_directory ? file_system[index].content : nullptr;
}

bool vfs_read_file(const char *path, uint8_t **out_data, uint32_t *out_size) {
    if (!out_data || !out_size) return false;
    *out_data = nullptr;
    *out_size = 0;
    char relative[MAX_FILENAME];
    if (fat_path(path, relative)) return fat32_read_file(relative, out_data, out_size);
    const int index = find_node(path);
    if (index < 0 || file_system[index].is_directory) return false;
    if (file_system[index].size == UINT32_MAX) return false;
    auto *copy = static_cast<uint8_t *>(welovec::memory::heap::allocate(file_system[index].size + 1));
    if (!copy) return false;
    for (uint32_t i = 0; i < file_system[index].size; ++i)
        copy[i] = static_cast<uint8_t>(file_system[index].content[i]);
    copy[file_system[index].size] = 0;
    *out_data = copy;
    *out_size = file_system[index].size;
    return true;
}

bool vfs_write_file(const char *path, const uint8_t *data, uint32_t size) {
    if (!path || (!data && size) || size == UINT32_MAX || path_is_directory_form(path))
        return false;

    char relative[MAX_FILENAME];
    if (fat_path(path, relative)) return fat32_write_file(relative, data, size);

    char normalized[MAX_FILENAME];
    if (!normalize_path(path, normalized, sizeof(normalized), false) || is_root(normalized) ||
        !ensure_parent_dirs(normalized)) return false;
    const int index = find_exact(normalized);
    if (index >= 0 && file_system[index].is_directory) return false;

    auto *copy = static_cast<char *>(welovec::memory::heap::allocate(size + 1));
    if (!copy) return false;
    for (uint32_t i = 0; i < size; ++i) copy[i] = static_cast<char>(data[i]);
    copy[size] = '\0';

    if (index >= 0) {
        if (file_system[index].content)
            welovec::memory::heap::deallocate(file_system[index].content);
        file_system[index].content = copy;
        file_system[index].size = size;
        return true;
    }
    if (file_count >= MAX_FILES) {
        welovec::memory::heap::deallocate(copy);
        return false;
    }
    FileNode &node = file_system[file_count++];
    vfs_copy_text(node.name, MAX_FILENAME, normalized);
    node.content = copy;
    node.size = size;
    node.is_directory = false;
    return true;
}

bool vfs_write_file_range(const char *path, uint32_t offset,
                          const uint8_t *data, uint32_t size) {
    if (!path || (!data && size) || offset > UINT32_MAX - size) return false;
    if (!size) return true;
    const bool exists = file_exists(path);
    if (exists && fs_is_directory(path)) return false;

    uint8_t *old_data = nullptr;
    uint32_t old_size = 0;
    if (exists && !vfs_read_file(path, &old_data, &old_size)) return false;
    const uint32_t end = offset + size;
    const uint32_t new_size = old_size > end ? old_size : end;
    auto *joined = static_cast<uint8_t *>(welovec::memory::heap::allocate(new_size));
    if (!joined) {
        if (old_data) vfs_free_file(old_data);
        return false;
    }
    for (uint32_t i = 0; i < old_size; ++i) joined[i] = old_data[i];
    for (uint32_t i = old_size; i < new_size; ++i) joined[i] = 0;
    for (uint32_t i = 0; i < size; ++i) joined[offset + i] = data[i];
    const bool result = vfs_write_file(path, joined, new_size);
    welovec::memory::heap::deallocate(joined);
    if (old_data) vfs_free_file(old_data);
    return result;
}

void vfs_free_file(void *data) {
    if (data) welovec::memory::heap::deallocate(data);
}

int fs_list(const char *path, VfsListEntry *entries, int max_entries) {
    if (!entries || max_entries <= 0) return 0;
    char relative[MAX_FILENAME];
    if (fat_path(path, relative)) return fat32_list(relative, entries, max_entries);
    char normalized[MAX_FILENAME];
    if (!normalize_path(path && path[0] ? path : "/", normalized, sizeof(normalized), true)) return 0;
    const int directory = find_exact(normalized);
    if (directory < 0 || !file_system[directory].is_directory) return 0;
    const uint32_t prefix_length = text_length(normalized);
    int count = 0;
    for (int i = 0; i < file_count && count < max_entries; ++i) {
        if (i == directory || !starts_with(file_system[i].name, normalized)) continue;
        const char *rest = file_system[i].name + prefix_length;
        if (!rest[0]) continue;
        uint32_t length = text_length(rest);
        if (rest[length - 1] == '/') --length;
        bool direct_child = true;
        for (uint32_t j = 0; j < length; ++j) if (rest[j] == '/') direct_child = false;
        if (!direct_child || !length) continue;
        vfs_copy_text(entries[count].name, sizeof(entries[count].name), rest);
        entries[count].name[length < sizeof(entries[count].name) ? length : sizeof(entries[count].name) - 1] = '\0';
        entries[count].size = file_system[i].size;
        entries[count].is_directory = file_system[i].is_directory;
        ++count;
    }
    return count;
}

bool dir(const char *path) {
    auto *entries = static_cast<VfsListEntry *>(welovec::memory::heap::allocate(
        sizeof(VfsListEntry) * MAX_LIST_ENTRIES));
    if (!entries) return false;
    const int count = fs_list(path, entries, MAX_LIST_ENTRIES);
    for (int i = 0; i < count; ++i) {
        serial_write(entries[i].name);
        serial_write(entries[i].is_directory ? "/ [DIR]\n" : " [FILE]\n");
    }
    welovec::memory::heap::deallocate(entries);
    return count > 0;
}
