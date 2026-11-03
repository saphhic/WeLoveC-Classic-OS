// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements reading and writing of FAT32 volumes.

#include "fat32.h"

#include "arch/common/MM/Heap/heap.hpp"
#include "arch/x86_64/scheduler/task_lock.hpp"

namespace {
using welovec::block::Device;
using welovec::x86_64::scheduler::TaskLock;

constexpr uint32_t kMaximumSectorSize = 4096;
constexpr uint32_t kMaximumName = 255;
constexpr uint32_t kMaximumLfnEntries = 20;
constexpr uint32_t kEoc = 0x0fffffff;
constexpr uint32_t kBadCluster = 0x0ffffff7;
constexpr uint64_t kNoSlot = UINT64_MAX;
struct MountAlias {
    const char *path;
    const char *volume_path;
};

constexpr MountAlias kMountAliases[] = {
    {"/userspace", "/userspace"},
    {"/bootloader", "/bootloader"},
    {"/kernel", "/kernel"},
    {"/userland", "/userland"},
};

struct Volume {
    Device *device;
    uint64_t first_block;
    uint64_t partition_blocks;
    uint64_t total_sectors;
    uint64_t fat_start_sector;
    uint64_t data_start_sector;
    uint64_t fat_size_sectors;
    uint64_t cluster_count;
    uint32_t bytes_per_sector;
    uint32_t device_blocks_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t reserved_sectors;
    uint32_t fat_count;
    uint32_t root_cluster;
    uint32_t fsinfo_sector;
    uint32_t backup_boot_sector;
    uint32_t active_fat;
    uint32_t free_clusters;
    uint32_t next_free;
    bool fat_mirroring;
    bool fsinfo_valid;
    bool read_only;
    bool mounted;
};

struct FatNode {
    char name[1024];
    bool found;
    bool directory;
    uint32_t first_cluster;
    uint32_t size;
    uint32_t parent_cluster;
    uint32_t lfn_slots;
    uint64_t short_slot;
    uint8_t short_entry[32];
};

struct LfnState {
    uint16_t characters[260];
    uint8_t checksum;
    uint8_t expected_order;
    uint8_t total_entries;
    bool active;
};

struct RawMemoryImage {
    uint8_t *bytes;
    uint64_t size;
};

TaskLock gFatLock;
Volume gVolume{};
Device gRawImageDevice{};
RawMemoryImage gRawImage{};

uint16_t rd16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t rd32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t rd64(const uint8_t *p) {
    return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32);
}

void wr16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void wr32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

void copy_bytes(void *destination, const void *source, uint64_t length) {
    auto *out = static_cast<uint8_t *>(destination);
    const auto *in = static_cast<const uint8_t *>(source);
    for (uint64_t i = 0; i < length; ++i) out[i] = in[i];
}

void zero_bytes(void *destination, uint64_t length) {
    auto *out = static_cast<uint8_t *>(destination);
    for (uint64_t i = 0; i < length; ++i) out[i] = 0;
}

uint32_t string_length(const char *s) {
    uint32_t length = 0;
    if (s) while (s[length]) ++length;
    return length;
}

bool utf8_to_utf16_name(const char *name, uint16_t output[kMaximumName], uint32_t *out_length) {
    if (!name || !out_length) return false;
    const uint32_t byte_length = string_length(name);
    if (!byte_length || byte_length > 1020) return false;
    uint32_t input = 0;
    uint32_t count = 0;
    uint32_t last_codepoint = 0;
    while (input < byte_length) {
        const uint8_t first = static_cast<uint8_t>(name[input++]);
        uint32_t codepoint;
        uint32_t continuation;
        if (first < 0x80) { codepoint = first; continuation = 0; }
        else if ((first & 0xe0) == 0xc0) { codepoint = first & 0x1f; continuation = 1; }
        else if ((first & 0xf0) == 0xe0) { codepoint = first & 0x0f; continuation = 2; }
        else if ((first & 0xf8) == 0xf0) { codepoint = first & 0x07; continuation = 3; }
        else return false;
        if (continuation > byte_length - input) return false;
        for (uint32_t i = 0; i < continuation; ++i) {
            const uint8_t next = static_cast<uint8_t>(name[input++]);
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((continuation == 1 && codepoint < 0x80) ||
            (continuation == 2 && codepoint < 0x800) ||
            (continuation == 3 && codepoint < 0x10000) ||
        codepoint > 0x10ffff || codepoint == 0xffff || codepoint == 0xfffe ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
        if (codepoint < 0x20 || codepoint == 0x7f || codepoint == '"' || codepoint == '*' ||
            codepoint == '/' || codepoint == ':' || codepoint == '<' || codepoint == '>' ||
            codepoint == '?' || codepoint == '\\' || codepoint == '|') return false;
        const uint32_t needed = codepoint > 0xffff ? 2 : 1;
        if (count + needed > kMaximumName) return false;
        if (needed == 1) output[count++] = static_cast<uint16_t>(codepoint);
        else {
            codepoint -= 0x10000;
            output[count++] = static_cast<uint16_t>(0xd800 | (codepoint >> 10));
            output[count++] = static_cast<uint16_t>(0xdc00 | (codepoint & 0x3ff));
        }
        last_codepoint = (first < 0x80) ? first : 0;
    }
    if (!count || last_codepoint == ' ' || last_codepoint == '.') return false;
    *out_length = count;
    return true;
}

bool append_utf8(uint32_t codepoint, char *out, uint32_t capacity, uint32_t *position) {
    uint8_t encoded[4];
    uint32_t length;
    if (codepoint < 0x80) { encoded[0] = codepoint; length = 1; }
    else if (codepoint < 0x800) {
        encoded[0] = 0xc0 | (codepoint >> 6);
        encoded[1] = 0x80 | (codepoint & 0x3f);
        length = 2;
    } else if (codepoint < 0x10000) {
        encoded[0] = 0xe0 | (codepoint >> 12);
        encoded[1] = 0x80 | ((codepoint >> 6) & 0x3f);
        encoded[2] = 0x80 | (codepoint & 0x3f);
        length = 3;
    } else {
        encoded[0] = 0xf0 | (codepoint >> 18);
        encoded[1] = 0x80 | ((codepoint >> 12) & 0x3f);
        encoded[2] = 0x80 | ((codepoint >> 6) & 0x3f);
        encoded[3] = 0x80 | (codepoint & 0x3f);
        length = 4;
    }
    if (!out || !position || length >= capacity - *position) return false;
    for (uint32_t i = 0; i < length; ++i) out[(*position)++] = static_cast<char>(encoded[i]);
    return true;
}

char upper_ascii(char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

char lower_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equal_ci(const char *a, const char *b) {
    uint32_t i = 0;
    if (!a || !b) return false;
    while (a[i] && b[i] && upper_ascii(a[i]) == upper_ascii(b[i])) ++i;
    return a[i] == '\0' && b[i] == '\0';
}

void copy_text(char *out, uint32_t capacity, const char *in) {
    if (!out || !capacity) return;
    uint32_t i = 0;
    if (in) while (in[i] && i + 1 < capacity) { out[i] = in[i]; ++i; }
    out[i] = '\0';
}

bool device_read(Device *device, uint64_t lba, uint32_t count, void *buffer) {
    return welovec::block::read(device, lba, count, buffer);
}

bool volume_read_bytes(uint64_t byte_offset, void *buffer, uint64_t length);
bool volume_write_bytes(uint64_t byte_offset, const void *buffer, uint64_t length);

bool mount_candidate(Device *device, uint64_t first_block, uint64_t block_count, Volume *out) {
    if (!device || !out || !device->read || !device->block_size ||
        device->block_size > kMaximumSectorSize || first_block > device->block_count ||
        block_count > device->block_count - first_block || !block_count) return false;

    uint8_t boot[kMaximumSectorSize];
    if (!device_read(device, first_block, 1, boot)) return false;
    if (device->block_size < 512 || boot[510] != 0x55 || boot[511] != 0xaa) return false;

    const uint32_t bytes_per_sector = rd16(boot + 11);
    const uint32_t sectors_per_cluster = boot[13];
    const uint32_t reserved_sectors = rd16(boot + 14);
    const uint32_t fat_count = boot[16];
    const uint32_t root_entries = rd16(boot + 17);
    const uint64_t total_sectors = rd16(boot + 19) ? rd16(boot + 19) : rd32(boot + 32);
    const uint32_t fat16_size = rd16(boot + 22);
    const uint64_t fat_size = rd32(boot + 36);
    const uint16_t ext_flags = rd16(boot + 40);
    const uint16_t fs_version = rd16(boot + 42);
    const uint32_t root_cluster = rd32(boot + 44) & 0x0fffffff;
    const uint32_t fsinfo_sector = rd16(boot + 48);
    const uint32_t backup_boot = rd16(boot + 50);

    if (bytes_per_sector < 512 ||
        bytes_per_sector > kMaximumSectorSize ||
        (bytes_per_sector & (bytes_per_sector - 1)) ||
        device->block_size > bytes_per_sector || bytes_per_sector % device->block_size ||
        !sectors_per_cluster || sectors_per_cluster > 128 ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) || !reserved_sectors ||
        !fat_count || fat_count > 4 || root_entries || fat16_size || !fat_size ||
        fs_version != 0 || !total_sectors ||
        total_sectors > block_count / (bytes_per_sector / device->block_size) ||
        total_sectors > UINT32_MAX) return false;

    const uint64_t fat_area = static_cast<uint64_t>(fat_count) * fat_size;
    if (fat_area > total_sectors || reserved_sectors > total_sectors - fat_area) return false;
    const uint64_t data_start = static_cast<uint64_t>(reserved_sectors) + fat_area;
    if (data_start >= total_sectors || fat_size > UINT32_MAX) return false;
    const uint64_t data_sectors = total_sectors - data_start;
    const uint64_t clusters = data_sectors / sectors_per_cluster;
    const uint64_t fat_entries = fat_size * bytes_per_sector / 4;
    if (static_cast<uint64_t>(bytes_per_sector) * sectors_per_cluster > 32768 ||
        clusters < 65525 || clusters >= 0x0ffffff5ULL || clusters + 2 > fat_entries ||
        root_cluster < 2 || root_cluster >= clusters + 2) return false;

    Volume candidate{};
    candidate.device = device;
    candidate.first_block = first_block;
    candidate.partition_blocks = block_count;
    candidate.total_sectors = total_sectors;
    candidate.fat_start_sector = reserved_sectors;
    candidate.data_start_sector = data_start;
    candidate.fat_size_sectors = fat_size;
    candidate.cluster_count = clusters;
    candidate.bytes_per_sector = bytes_per_sector;
    candidate.device_blocks_per_sector = bytes_per_sector / device->block_size;
    candidate.sectors_per_cluster = sectors_per_cluster;
    candidate.cluster_size = bytes_per_sector * sectors_per_cluster;
    candidate.reserved_sectors = reserved_sectors;
    candidate.fat_count = fat_count;
    candidate.root_cluster = root_cluster;
    candidate.fsinfo_sector = fsinfo_sector;
    candidate.backup_boot_sector = backup_boot;
    candidate.fat_mirroring = (ext_flags & 0x80) == 0;
    candidate.active_fat = candidate.fat_mirroring ? 0 : (ext_flags & 0x0f);
    candidate.read_only = device->read_only || !device->write;
    candidate.mounted = true;
    if (candidate.active_fat >= fat_count) return false;

    gVolume = candidate;
    uint8_t fsinfo[kMaximumSectorSize];
    if (fsinfo_sector && fsinfo_sector < reserved_sectors &&
        volume_read_bytes(static_cast<uint64_t>(fsinfo_sector) * bytes_per_sector,
                          fsinfo, bytes_per_sector) &&
        rd32(fsinfo) == 0x41615252 && rd32(fsinfo + 484) == 0x61417272 &&
        rd32(fsinfo + 508) == 0xaa550000) {
        gVolume.fsinfo_valid = true;
        gVolume.free_clusters = rd32(fsinfo + 488);
        gVolume.next_free = rd32(fsinfo + 492);
        if (gVolume.free_clusters > gVolume.cluster_count) gVolume.free_clusters = UINT32_MAX;
        if (gVolume.next_free < 2 || gVolume.next_free >= gVolume.cluster_count + 2)
            gVolume.next_free = 2;
    } else {
        gVolume.free_clusters = UINT32_MAX;
        gVolume.next_free = 2;
    }
    *out = gVolume;
    return true;
}

uint64_t cluster_sector(uint32_t cluster) {
    return gVolume.data_start_sector + static_cast<uint64_t>(cluster - 2) *
           gVolume.sectors_per_cluster;
}

bool cluster_valid(uint32_t cluster) {
    return gVolume.mounted && cluster >= 2 &&
           static_cast<uint64_t>(cluster) < gVolume.cluster_count + 2 &&
           cluster < 0x0ffffff0;
}

bool volume_read_sectors(uint64_t sector, uint32_t count, void *buffer) {
    if (!gVolume.mounted || !count || sector >= gVolume.total_sectors ||
        count > gVolume.total_sectors - sector ||
        count > UINT32_MAX / gVolume.device_blocks_per_sector) return false;
    const uint64_t first_block = gVolume.first_block + sector * gVolume.device_blocks_per_sector;
    return device_read(gVolume.device, first_block,
                       count * gVolume.device_blocks_per_sector, buffer);
}

bool volume_read_bytes(uint64_t offset, void *buffer, uint64_t length) {
    if (!gVolume.mounted || !buffer || offset > gVolume.total_sectors * gVolume.bytes_per_sector ||
        length > gVolume.total_sectors * gVolume.bytes_per_sector - offset) return false;
    auto *out = static_cast<uint8_t *>(buffer);
    uint8_t *sector = nullptr;
    bool success = true;
    while (length) {
        const uint64_t lba = offset / gVolume.bytes_per_sector;
        const uint32_t within = static_cast<uint32_t>(offset % gVolume.bytes_per_sector);
        if (!within && length >= gVolume.bytes_per_sector) {
            uint64_t count = length / gVolume.bytes_per_sector;
            if (count > UINT32_MAX) count = UINT32_MAX;
            if (!volume_read_sectors(lba, static_cast<uint32_t>(count), out)) {
                success = false;
                break;
            }
            const uint64_t bytes = count * gVolume.bytes_per_sector;
            offset += bytes; out += bytes; length -= bytes;
        } else {
            if (!sector) {
                sector = static_cast<uint8_t *>(
                    welovec::memory::heap::allocate(kMaximumSectorSize));
                if (!sector) {
                    success = false;
                    break;
                }
            }
            if (!volume_read_sectors(lba, 1, sector)) {
                success = false;
                break;
            }
            uint32_t chunk = gVolume.bytes_per_sector - within;
            if (chunk > length) chunk = static_cast<uint32_t>(length);
            copy_bytes(out, sector + within, chunk);
            offset += chunk; out += chunk; length -= chunk;
        }
    }
    if (sector) welovec::memory::heap::deallocate(sector);
    return success;
}

bool volume_write_sectors(uint64_t sector, uint32_t count, const void *buffer) {
    if (!gVolume.mounted || gVolume.read_only || !count || sector >= gVolume.total_sectors ||
        count > gVolume.total_sectors - sector) return false;
    return welovec::block::write(gVolume.device, gVolume.first_block + sector, count, buffer);
}

bool volume_write_bytes(uint64_t offset, const void *buffer, uint64_t length) {
    if (!gVolume.mounted || gVolume.read_only || !buffer ||
        offset > gVolume.total_sectors * gVolume.bytes_per_sector ||
        length > gVolume.total_sectors * gVolume.bytes_per_sector - offset) return false;
    const auto *in = static_cast<const uint8_t *>(buffer);
    uint8_t sector[kMaximumSectorSize];
    while (length) {
        const uint64_t lba = offset / gVolume.bytes_per_sector;
        const uint32_t within = static_cast<uint32_t>(offset % gVolume.bytes_per_sector);
        if (!within && length >= gVolume.bytes_per_sector) {
            uint64_t count = length / gVolume.bytes_per_sector;
            if (count > UINT32_MAX) count = UINT32_MAX;
            if (!volume_write_sectors(lba, static_cast<uint32_t>(count), in)) return false;
            const uint64_t bytes = count * gVolume.bytes_per_sector;
            offset += bytes; in += bytes; length -= bytes;
        } else {
            if (!volume_read_sectors(lba, 1, sector)) return false;
            uint32_t chunk = gVolume.bytes_per_sector - within;
            if (chunk > length) chunk = static_cast<uint32_t>(length);
            copy_bytes(sector + within, in, chunk);
            if (!volume_write_sectors(lba, 1, sector)) return false;
            offset += chunk; in += chunk; length -= chunk;
        }
    }
    return true;
}

bool fat_get(uint32_t cluster, uint32_t *value) {
    if (!value || cluster >= gVolume.cluster_count + 2) return false;
    const uint64_t offset = (static_cast<uint64_t>(gVolume.active_fat) * gVolume.fat_size_sectors *
                             gVolume.bytes_per_sector) + static_cast<uint64_t>(cluster) * 4;
    uint8_t raw[4];
    if (!volume_read_bytes(static_cast<uint64_t>(gVolume.fat_start_sector) *
                           gVolume.bytes_per_sector + offset, raw, sizeof(raw))) return false;
    *value = rd32(raw) & 0x0fffffff;
    return true;
}

bool fat_set(uint32_t cluster, uint32_t value) {
    if (gVolume.read_only || cluster >= gVolume.cluster_count + 2) return false;
    const uint32_t first = gVolume.fat_mirroring ? 0 : gVolume.active_fat;
    const uint32_t count = gVolume.fat_mirroring ? gVolume.fat_count : 1;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t fat_index = gVolume.fat_mirroring ? i : first;
        const uint64_t offset = (static_cast<uint64_t>(gVolume.fat_start_sector) +
            static_cast<uint64_t>(fat_index) * gVolume.fat_size_sectors) *
            gVolume.bytes_per_sector + static_cast<uint64_t>(cluster) * 4;
        uint8_t raw[4];
        if (!volume_read_bytes(offset, raw, sizeof(raw))) return false;
        wr32(raw, (rd32(raw) & 0xf0000000) | (value & 0x0fffffff));
        if (!volume_write_bytes(offset, raw, sizeof(raw))) return false;
    }
    return true;
}

bool is_eoc(uint32_t cluster) { return cluster >= 0x0ffffff8; }
bool is_link(uint32_t cluster) { return cluster_valid(cluster); }

bool update_fsinfo() {
    if (!gVolume.fsinfo_valid || gVolume.read_only) return true;
    uint8_t info[kMaximumSectorSize];
    const uint64_t offset = static_cast<uint64_t>(gVolume.fsinfo_sector) * gVolume.bytes_per_sector;
    if (!volume_read_bytes(offset, info, gVolume.bytes_per_sector)) return false;
    wr32(info + 488, gVolume.free_clusters);
    wr32(info + 492, gVolume.next_free);
    if (!volume_write_bytes(offset, info, gVolume.bytes_per_sector)) return false;
    if (gVolume.backup_boot_sector && gVolume.backup_boot_sector < gVolume.reserved_sectors &&
        gVolume.backup_boot_sector + gVolume.fsinfo_sector < gVolume.reserved_sectors) {
        const uint64_t backup_offset = static_cast<uint64_t>(gVolume.backup_boot_sector +
            gVolume.fsinfo_sector) * gVolume.bytes_per_sector;
        if (volume_read_bytes(backup_offset, info, gVolume.bytes_per_sector) &&
            rd32(info) == 0x41615252 && rd32(info + 484) == 0x61417272 &&
            rd32(info + 508) == 0xaa550000) {
            wr32(info + 488, gVolume.free_clusters);
            wr32(info + 492, gVolume.next_free);
            if (!volume_write_bytes(backup_offset, info, gVolume.bytes_per_sector)) return false;
        }
    }
    return true;
}

bool allocate_cluster(uint32_t *out_cluster) {
    if (!out_cluster || gVolume.read_only || !gVolume.mounted) return false;
    uint32_t candidate = gVolume.next_free;
    if (candidate < 2 || static_cast<uint64_t>(candidate) >= gVolume.cluster_count + 2) candidate = 2;
    for (uint64_t scanned = 0; scanned < gVolume.cluster_count; ++scanned) {
        uint32_t value;
        if (!fat_get(candidate, &value)) return false;
        if (value == 0) {
            if (!fat_set(candidate, kEoc)) return false;
            if (gVolume.free_clusters != UINT32_MAX && gVolume.free_clusters) --gVolume.free_clusters;
            gVolume.next_free = candidate + 1;
            if (static_cast<uint64_t>(gVolume.next_free) >= gVolume.cluster_count + 2) gVolume.next_free = 2;
            update_fsinfo();
            *out_cluster = candidate;
            return true;
        }
        ++candidate;
        if (static_cast<uint64_t>(candidate) >= gVolume.cluster_count + 2) candidate = 2;
    }
    return false;
}

bool free_chain(uint32_t first) {
    if (!first) return true;
    uint32_t current = first;
    for (uint64_t n = 0; n < gVolume.cluster_count; ++n) {
        if (!cluster_valid(current)) return false;
        uint32_t next;
        if (!fat_get(current, &next) || !fat_set(current, 0)) return false;
        if (gVolume.free_clusters != UINT32_MAX) ++gVolume.free_clusters;
        if (next == 0 || is_eoc(next)) {
            gVolume.next_free = current;
            update_fsinfo();
            return true;
        }
        if (!is_link(next) || next == kBadCluster) return false;
        current = next;
    }
    return false;
}

bool allocate_chain(uint64_t count, uint32_t *out_first, uint32_t *out_last) {
    if (!out_first || !out_last) return false;
    *out_first = *out_last = 0;
    uint32_t previous = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint32_t cluster;
        if (!allocate_cluster(&cluster)) {
            if (*out_first) free_chain(*out_first);
            return false;
        }
        if (!*out_first) *out_first = cluster;
        if (previous && !fat_set(previous, cluster)) {
            fat_set(cluster, 0);
            if (*out_first != cluster) free_chain(*out_first);
            return false;
        }
        previous = cluster;
    }
    *out_last = previous;
    return true;
}

uint64_t cluster_byte_offset(uint32_t cluster) {
    return cluster_sector(cluster) * gVolume.bytes_per_sector;
}

bool zero_cluster(uint32_t cluster) {
    auto *buffer = static_cast<uint8_t *>(welovec::memory::heap::allocate(gVolume.cluster_size));
    if (!buffer) return false;
    zero_bytes(buffer, gVolume.cluster_size);
    const bool ok = volume_write_bytes(cluster_byte_offset(cluster), buffer, gVolume.cluster_size);
    welovec::memory::heap::deallocate(buffer);
    return ok;
}

bool write_file_data_chain(uint32_t first, const uint8_t *data, uint32_t size) {
    if (!size) return true;
    auto *buffer = static_cast<uint8_t *>(welovec::memory::heap::allocate(gVolume.cluster_size));
    if (!buffer) return false;
    uint32_t remaining = size;
    uint32_t cluster = first;
    bool ok = true;
    uint64_t guard = 0;
    while (remaining && guard++ < gVolume.cluster_count) {
        if (!cluster_valid(cluster)) { ok = false; break; }
        const uint32_t chunk = remaining < gVolume.cluster_size ? remaining : gVolume.cluster_size;
        zero_bytes(buffer, gVolume.cluster_size);
        copy_bytes(buffer, data + (size - remaining), chunk);
        if (!volume_write_bytes(cluster_byte_offset(cluster), buffer, gVolume.cluster_size)) {
            ok = false; break;
        }
        remaining -= chunk;
        if (remaining && (!fat_get(cluster, &cluster) || !cluster_valid(cluster))) {
            ok = false; break;
        }
    }
    if (remaining) ok = false;
    welovec::memory::heap::deallocate(buffer);
    return ok;
}

bool read_file_data(const FatNode &node, uint8_t *destination) {
    if (!node.size) return true;
    if (!node.first_cluster || !cluster_valid(node.first_cluster)) return false;
    uint32_t remaining = node.size;
    uint32_t cluster = node.first_cluster;
    uint64_t guard = 0;
    while (remaining && guard++ < gVolume.cluster_count) {
        if (!cluster_valid(cluster)) return false;
        const uint32_t chunk = remaining < gVolume.cluster_size ? remaining : gVolume.cluster_size;
        if (!volume_read_bytes(cluster_byte_offset(cluster), destination + (node.size - remaining), chunk)) return false;
        remaining -= chunk;
        if (remaining) {
            uint32_t next;
            if (!fat_get(cluster, &next) || !cluster_valid(next) || is_eoc(next) || next == kBadCluster) return false;
            cluster = next;
        }
    }
    return remaining == 0;
}

bool chain_cluster_at(uint32_t first, uint64_t index, uint32_t *out) {
    if (!out || !cluster_valid(first) || index >= gVolume.cluster_count) return false;
    uint32_t current = first;
    for (uint64_t i = 0; i < index; ++i) {
        uint32_t next;
        if (!fat_get(current, &next) || !cluster_valid(next) || is_eoc(next) || next == kBadCluster) return false;
        current = next;
    }
    *out = current;
    return true;
}

void lfn_reset(LfnState *state) {
    if (!state) return;
    for (uint32_t i = 0; i < 260; ++i) state->characters[i] = 0xffff;
    state->checksum = 0;
    state->expected_order = 0;
    state->total_entries = 0;
    state->active = false;
}

bool lfn_is_entry(const uint8_t *entry) { return entry[11] == 0x0f; }

uint8_t lfn_checksum(const uint8_t short_name[11]) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 11; ++i)
        sum = static_cast<uint8_t>(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    return sum;
}

void lfn_add(LfnState *state, const uint8_t *entry) {
    const uint8_t raw_order = entry[0];
    const uint8_t order = raw_order & 0x1f;
    if (!order || order > kMaximumLfnEntries || (raw_order & 0xa0)) {
        lfn_reset(state); return;
    }
    if (raw_order & 0x40) {
        lfn_reset(state);
        state->active = true;
        state->expected_order = order;
        state->total_entries = order;
        state->checksum = entry[13];
    }
    if (!state->active || state->expected_order != order || state->checksum != entry[13] ||
        entry[12] != 0 || rd16(entry + 26) != 0) {
        lfn_reset(state); return;
    }
    const uint8_t offsets[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    const uint32_t base = static_cast<uint32_t>(order - 1) * 13;
    for (uint32_t i = 0; i < 13; ++i)
        state->characters[base + i] = rd16(entry + offsets[i]);
    --state->expected_order;
}

bool lfn_decode(const LfnState *state, const uint8_t short_name[11], char *out, uint32_t capacity) {
    if (!state || !state->active || state->expected_order ||
        state->checksum != lfn_checksum(short_name) || !out || capacity < 2) return false;
    uint32_t pos = 0;
    uint32_t last_codepoint = 0;
    for (uint32_t i = 0; i < 260; ++i) {
        uint32_t codepoint = state->characters[i];
        if (codepoint == 0 || codepoint == 0xffff) break;
        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
            if (i + 1 >= 260) return false;
            const uint32_t low = state->characters[++i];
            if (low < 0xdc00 || low > 0xdfff) return false;
            codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
        } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) return false;
        if (codepoint < 0x20 || codepoint == 0x7f || codepoint == '"' || codepoint == '*' ||
            codepoint == '/' || codepoint == ':' || codepoint == '<' || codepoint == '>' ||
            codepoint == '?' || codepoint == '\\' || codepoint == '|') return false;
        if (!append_utf8(codepoint, out, capacity, &pos)) return false;
        last_codepoint = codepoint;
    }
    if (last_codepoint == ' ' || last_codepoint == '.') return false;
    out[pos] = '\0';
    return pos != 0;
}

void short_name_decode(const uint8_t *entry, char *out, uint32_t capacity) {
    uint32_t pos = 0;
    const uint8_t nt = entry[12];
    for (uint32_t i = 0; i < 8 && entry[i] != ' ' && pos + 1 < capacity; ++i) {
        char ch = static_cast<char>(entry[i]);
        if (nt & 0x08) ch = lower_ascii(ch);
        out[pos++] = ch;
    }
    bool extension = false;
    for (uint32_t i = 8; i < 11; ++i) if (entry[i] != ' ') extension = true;
    if (extension && pos + 1 < capacity) {
        out[pos++] = '.';
        for (uint32_t i = 8; i < 11 && entry[i] != ' ' && pos + 1 < capacity; ++i) {
            char ch = static_cast<char>(entry[i]);
            if (nt & 0x10) ch = lower_ascii(ch);
            out[pos++] = ch;
        }
    }
    out[pos] = '\0';
}

bool directory_next(uint32_t cluster, uint32_t *next) {
    uint32_t value;
    if (!fat_get(cluster, &value)) return false;
    if (is_eoc(value) || value == 0) { *next = 0; return true; }
    if (!cluster_valid(value) || value == kBadCluster) return false;
    *next = value;
    return true;
}

bool lookup_child(uint32_t directory_cluster, uint32_t parent_cluster,
                  const char *component, FatNode *out, bool *out_found) {
    if (!cluster_valid(directory_cluster) || !component || !out || !out_found) return false;
    *out_found = false;
    auto *buffer = static_cast<uint8_t *>(welovec::memory::heap::allocate(gVolume.cluster_size));
    if (!buffer) return false;
    uint32_t cluster = directory_cluster;
    uint64_t slot_base = 0;
    bool found = false;
    for (uint64_t chain = 0; cluster_valid(cluster) && chain < gVolume.cluster_count; ++chain) {
        if (!volume_read_bytes(cluster_byte_offset(cluster), buffer, gVolume.cluster_size)) {
            welovec::memory::heap::deallocate(buffer);
            return false;
        }
        const uint32_t entries = gVolume.cluster_size / 32;
        LfnState lfn{};
        lfn_reset(&lfn);
        for (uint32_t i = 0; i < entries; ++i) {
            const uint8_t *entry = buffer + static_cast<uint64_t>(i) * 32;
            if (entry[0] == 0) { cluster = 0; break; }
            if (entry[0] == 0xe5) { lfn_reset(&lfn); continue; }
            if (lfn_is_entry(entry)) { lfn_add(&lfn, entry); continue; }
            if (entry[11] & 0x08) { lfn_reset(&lfn); continue; }
            char long_name[1024];
            char short_name[16];
            const bool has_lfn = lfn_decode(&lfn, entry, long_name, sizeof(long_name));
            short_name_decode(entry, short_name, sizeof(short_name));
            if (equal_ci(component, has_lfn ? long_name : short_name) ||
                (has_lfn && equal_ci(component, short_name))) {
                zero_bytes(out, sizeof(*out));
                copy_text(out->name, sizeof(out->name), has_lfn ? long_name : short_name);
                out->found = true;
                out->directory = (entry[11] & 0x10) != 0;
                out->first_cluster = ((static_cast<uint32_t>(rd16(entry + 20)) << 16) |
                                      rd16(entry + 26)) & 0x0fffffff;
                out->size = rd32(entry + 28);
                out->parent_cluster = parent_cluster;
                out->lfn_slots = has_lfn ? lfn.total_entries : 0;
                out->short_slot = slot_base + i;
                copy_bytes(out->short_entry, entry, 32);
                found = true;
                break;
            }
            lfn_reset(&lfn);
        }
        if (found) break;
        if (!cluster) break;
        slot_base += entries;
        uint32_t next;
        if (!directory_next(cluster, &next)) {
            welovec::memory::heap::deallocate(buffer);
            return false;
        }
        if (!next) { cluster = 0; break; }
        cluster = next;
    }
    if (!found && cluster_valid(cluster)) {
        welovec::memory::heap::deallocate(buffer);
        return false; // A directory chain that never terminates is corrupt.
    }
    welovec::memory::heap::deallocate(buffer);
    *out_found = found;
    return true;
}

bool find_node(const char *path, FatNode *out) {
    if (!gVolume.mounted || !path || !out) return false;
    uint32_t current = gVolume.root_cluster;
    uint32_t parents[128];
    uint32_t depth = 0;
    uint32_t pos = 0;
    const uint32_t path_len = string_length(path);
    if (path_len > 1023) return false;
    while (path[pos] == '/' || path[pos] == '\\') ++pos;
    if (!path[pos]) {
        zero_bytes(out, sizeof(*out));
        out->found = true; out->directory = true;
        out->first_cluster = gVolume.root_cluster;
        out->parent_cluster = gVolume.root_cluster;
        out->short_slot = kNoSlot;
        copy_text(out->name, sizeof(out->name), "/");
        return true;
    }

    bool trailing_slash = path_len && (path[path_len - 1] == '/' || path[path_len - 1] == '\\');
    FatNode last{};
    while (path[pos]) {
        char component[1024];
        uint32_t n = 0;
        while (path[pos] && path[pos] != '/' && path[pos] != '\\') {
            if (n + 1 >= sizeof(component)) return false;
            component[n++] = path[pos++];
        }
        component[n] = '\0';
        while (path[pos] == '/' || path[pos] == '\\') ++pos;
        if (!n || (n == 1 && component[0] == '.')) continue;
        if (n == 2 && component[0] == '.' && component[1] == '.') {
            if (depth) current = parents[--depth];
            zero_bytes(&last, sizeof(last));
            continue;
        }
        uint16_t component_utf16[kMaximumName];
        uint32_t component_units = 0;
        if (!utf8_to_utf16_name(component, component_utf16, &component_units) || !component_units)
            return false;
        FatNode node{};
        bool found = false;
        if (!lookup_child(current, current, component, &node, &found) || !found) return false;
        if (path[pos]) {
            if (!node.directory || depth >= 128 || !cluster_valid(node.first_cluster)) return false;
            parents[depth++] = current;
            current = node.first_cluster;
        } else {
            last = node;
        }
    }
    if (!last.found) {
        zero_bytes(out, sizeof(*out));
        out->found = true; out->directory = true;
        out->first_cluster = current;
        out->parent_cluster = depth ? parents[depth - 1] : current;
        out->short_slot = kNoSlot;
        copy_text(out->name, sizeof(out->name), "/");
        return true;
    }
    if (trailing_slash && !last.directory) return false;
    *out = last;
    return true;
}

bool directory_has_child(uint32_t dir_cluster, uint32_t parent_cluster) {
    auto *buffer = static_cast<uint8_t *>(welovec::memory::heap::allocate(gVolume.cluster_size));
    if (!buffer) return true;
    uint32_t cluster = dir_cluster;
    bool has_child = false;
    for (uint64_t chain = 0; cluster_valid(cluster) && chain < gVolume.cluster_count; ++chain) {
        if (!volume_read_bytes(cluster_byte_offset(cluster), buffer, gVolume.cluster_size)) { has_child = true; break; }
        const uint32_t entries = gVolume.cluster_size / 32;
        for (uint32_t i = 0; i < entries; ++i) {
            const uint8_t *entry = buffer + static_cast<uint64_t>(i) * 32;
            if (!entry[0]) { cluster = 0; break; }
            if (entry[0] == 0xe5 || lfn_is_entry(entry) || (entry[11] & 0x08)) continue;
            if (entry[0] == '.' && (entry[1] == ' ' || entry[1] == '.')) continue;
            has_child = true; break;
        }
        if (has_child || !cluster) break;
        uint32_t next;
        if (!directory_next(cluster, &next)) { has_child = true; break; }
        if (!next) { cluster = 0; break; }
        cluster = next;
    }
    if (cluster_valid(cluster)) has_child = true;
    (void)parent_cluster;
    welovec::memory::heap::deallocate(buffer);
    return has_child;
}

int find_free_slots(uint32_t directory_cluster, uint32_t needed, uint64_t *start_slot) {
    auto *buffer = static_cast<uint8_t *>(welovec::memory::heap::allocate(gVolume.cluster_size));
    if (!buffer) return -1;
    uint32_t cluster = directory_cluster;
    uint64_t slot_base = 0;
    uint64_t run_start = 0;
    uint32_t run = 0;
    bool end_seen = false;
    bool found = false;
    bool failed = false;
    for (uint64_t chain = 0; cluster_valid(cluster) && chain < gVolume.cluster_count; ++chain) {
        if (!volume_read_bytes(cluster_byte_offset(cluster), buffer, gVolume.cluster_size)) {
            failed = true;
            break;
        }
        const uint32_t entries = gVolume.cluster_size / 32;
        for (uint32_t i = 0; i < entries; ++i) {
            const uint8_t first = buffer[static_cast<uint64_t>(i) * 32];
            if (first == 0) end_seen = true;
            if (end_seen || first == 0xe5) {
                if (!run) run_start = slot_base + i;
                if (++run >= needed) { found = true; break; }
            } else run = 0;
        }
        if (found) break;
        slot_base += entries;
        uint32_t next;
        if (!directory_next(cluster, &next)) { failed = true; break; }
        if (!next) { cluster = 0; break; }
        cluster = next;
    }
    if (!found && cluster_valid(cluster)) failed = true;
    welovec::memory::heap::deallocate(buffer);
    if (failed) return -1;
    if (found) *start_slot = run_start;
    return found ? 1 : 0;
}

bool append_directory_cluster(uint32_t directory_cluster) {
    uint32_t last = directory_cluster;
    for (uint64_t i = 0; i < gVolume.cluster_count; ++i) {
        uint32_t next;
        if (!directory_next(last, &next)) return false;
        if (!next) {
            uint32_t added;
            if (!allocate_cluster(&added)) return false;
            if (!zero_cluster(added) || !fat_set(last, added)) {
                fat_set(added, 0);
                return false;
            }
            return true;
        }
        last = next;
    }
    return false;
}

bool reserve_directory_slots(uint32_t directory_cluster, uint32_t needed, uint64_t *start_slot) {
    for (uint64_t attempt = 0; attempt < gVolume.cluster_count; ++attempt) {
        const int status = find_free_slots(directory_cluster, needed, start_slot);
        if (status > 0) return true;
        if (status < 0) return false;
        if (!append_directory_cluster(directory_cluster)) return false;
    }
    return false;
}

bool write_directory_slot(uint32_t directory_cluster, uint64_t slot,
                          const uint8_t entry[32]) {
    const uint64_t entries_per_cluster = gVolume.cluster_size / 32;
    uint32_t cluster;
    if (!chain_cluster_at(directory_cluster, slot / entries_per_cluster, &cluster)) return false;
    const uint64_t offset = cluster_byte_offset(cluster) + (slot % entries_per_cluster) * 32;
    return volume_write_bytes(offset, entry, 32);
}

bool alias_exists(uint32_t directory_cluster, const uint8_t short_name[11]) {
    auto *buffer = static_cast<uint8_t *>(welovec::memory::heap::allocate(gVolume.cluster_size));
    if (!buffer) return true;
    uint32_t cluster = directory_cluster;
    bool exists = false;
    for (uint64_t chain = 0; cluster_valid(cluster) && chain < gVolume.cluster_count; ++chain) {
        if (!volume_read_bytes(cluster_byte_offset(cluster), buffer, gVolume.cluster_size)) { exists = true; break; }
        const uint32_t entries = gVolume.cluster_size / 32;
        for (uint32_t i = 0; i < entries; ++i) {
            const uint8_t *entry = buffer + static_cast<uint64_t>(i) * 32;
            if (!entry[0]) { cluster = 0; break; }
            if (entry[0] == 0xe5 || lfn_is_entry(entry)) continue;
            bool equal = true;
            for (uint32_t j = 0; j < 11; ++j)
                if (entry[j] != short_name[j]) { equal = false; break; }
            if (equal) { exists = true; break; }
        }
        if (exists || !cluster) break;
        uint32_t next;
        if (!directory_next(cluster, &next)) { exists = true; break; }
        if (!next) { cluster = 0; break; }
        cluster = next;
    }
    if (cluster_valid(cluster)) exists = true;
    welovec::memory::heap::deallocate(buffer);
    return exists;
}

bool valid_short_char(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    const char allowed[] = "$%'-_@~`!(){}^#&";
    for (uint32_t i = 0; allowed[i]; ++i) if (c == allowed[i]) return true;
    return false;
}

void fill_short_component(const char *input, uint32_t length, char *output,
                          uint32_t capacity, bool *all_lower, bool *all_upper) {
    uint32_t written = 0;
    *all_lower = true; *all_upper = true;
    for (uint32_t i = 0; i < length && written < capacity; ++i) {
        const char c = input[i];
        if (!valid_short_char(upper_ascii(c))) continue;
        if (c >= 'A' && c <= 'Z') *all_lower = false;
        if (c >= 'a' && c <= 'z') *all_upper = false;
        output[written++] = upper_ascii(c);
    }
    if (!written && length) { output[0] = '_'; written = 1; }
    output[written] = '\0';
}

bool direct_short_name(const char *name, uint8_t out[11], uint8_t *ntres,
                       bool *needs_lfn) {
    const uint32_t length = string_length(name);
    int32_t dot = -1;
    for (uint32_t i = 0; i < length; ++i) {
        if (name[i] == '.') {
            if (dot >= 0) return false;
            dot = static_cast<int32_t>(i);
        }
    }
    const uint32_t base_len = dot < 0 ? length : static_cast<uint32_t>(dot);
    const uint32_t ext_start = dot < 0 ? length : static_cast<uint32_t>(dot + 1);
    const uint32_t ext_len = dot < 0 ? 0 : length - ext_start;
    if (!base_len || base_len > 8 || ext_len > 3 || (dot >= 0 && !ext_len)) return false;
    bool base_lower, base_upper, ext_lower, ext_upper;
    char base[9], ext[4];
    fill_short_component(name, base_len, base, 8, &base_lower, &base_upper);
    fill_short_component(name + ext_start, ext_len, ext, 3, &ext_lower, &ext_upper);
    if (string_length(base) != base_len || string_length(ext) != ext_len) return false;
    for (uint32_t i = 0; i < 11; ++i) out[i] = ' ';
    for (uint32_t i = 0; i < base_len; ++i) out[i] = static_cast<uint8_t>(base[i]);
    for (uint32_t i = 0; i < ext_len; ++i) out[8 + i] = static_cast<uint8_t>(ext[i]);
    *ntres = 0;
    if (base_lower && !base_upper) *ntres |= 0x08;
    if (ext_lower && !ext_upper) *ntres |= 0x10;

    char represented[16];
    uint32_t pos = 0;
    for (uint32_t i = 0; i < base_len; ++i) represented[pos++] =
        (*ntres & 0x08) ? lower_ascii(base[i]) : base[i];
    if (ext_len) {
        represented[pos++] = '.';
        for (uint32_t i = 0; i < ext_len; ++i) represented[pos++] =
            (*ntres & 0x10) ? lower_ascii(ext[i]) : ext[i];
    }
    represented[pos] = '\0';
    *needs_lfn = false;
    for (uint32_t i = 0; i <= length; ++i) {
        if (name[i] != represented[i]) { *needs_lfn = true; break; }
    }
    return true;
}

bool make_short_alias(uint32_t directory_cluster, const char *name,
                      uint8_t out[11], uint8_t *ntres, bool *needs_lfn) {
    const uint32_t length = string_length(name);
    uint16_t utf16_name[kMaximumName];
    uint32_t utf16_length = 0;
    if (!utf8_to_utf16_name(name, utf16_name, &utf16_length) || !utf16_length ||
        (length == 1 && name[0] == '.') ||
        (length == 2 && name[0] == '.' && name[1] == '.')) return false;
    for (uint32_t i = 0; i < length; ++i) {
        const uint8_t c = static_cast<uint8_t>(name[i]);
        if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
            c == '>' || c == '?' || c == '\\' || c == '|' || c == 0x7f) return false;
    }
    if (direct_short_name(name, out, ntres, needs_lfn) &&
        !alias_exists(directory_cluster, out)) return true;

    int32_t last_dot = -1;
    for (uint32_t i = 0; i < length; ++i) if (name[i] == '.') last_dot = i;
    const uint32_t base_length = last_dot >= 0 ? static_cast<uint32_t>(last_dot) : length;
    const uint32_t ext_start = last_dot >= 0 ? static_cast<uint32_t>(last_dot + 1) : length;
    char base[9], ext[4];
    bool bl, bu, el, eu;
    fill_short_component(name, base_length, base, 6, &bl, &bu);
    fill_short_component(name + ext_start, length - ext_start, ext, 3, &el, &eu);
    const uint32_t ext_len = (last_dot >= 0 && ext_start < length) ? string_length(ext) : 0;
    for (uint32_t suffix = 1; suffix < 1000000; ++suffix) {
        char digits[7];
        uint32_t digit_count = 0, value = suffix;
        do { digits[digit_count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value && digit_count < 6);
        if (value) continue;
        for (uint32_t i = 0; i < 11; ++i) out[i] = ' ';
        const uint32_t prefix_count = 8 - digit_count - 1;
        for (uint32_t i = 0; i < prefix_count && base[i]; ++i) out[i] = static_cast<uint8_t>(base[i]);
        out[prefix_count] = '~';
        for (uint32_t i = 0; i < digit_count; ++i)
            out[prefix_count + 1 + i] = static_cast<uint8_t>(digits[digit_count - i - 1]);
        for (uint32_t i = 0; i < ext_len; ++i) out[8 + i] = static_cast<uint8_t>(ext[i]);
        if (!alias_exists(directory_cluster, out)) {
            *ntres = 0;
            *needs_lfn = true;
            return true;
        }
    }
    return false;
}

bool parent_and_leaf(const char *path, char *parent, uint32_t parent_capacity,
                     char *leaf, uint32_t leaf_capacity) {
    if (!path || !parent || !leaf) return false;
    uint32_t len = string_length(path);
    while (len && (path[len - 1] == '/' || path[len - 1] == '\\')) --len;
    if (!len) return false;
    uint32_t slash = UINT32_MAX;
    for (uint32_t i = 0; i < len; ++i) if (path[i] == '/' || path[i] == '\\') slash = i;
    const uint32_t leaf_start = slash == UINT32_MAX ? 0 : slash + 1;
    if (leaf_start >= len || len - leaf_start >= leaf_capacity) return false;
    for (uint32_t i = 0; i < len - leaf_start; ++i) leaf[i] = path[leaf_start + i];
    leaf[len - leaf_start] = '\0';
    if (slash == UINT32_MAX || slash == 0) copy_text(parent, parent_capacity, "/");
    else {
        const uint32_t parent_len = slash;
        if (parent_len + 1 >= parent_capacity) return false;
        for (uint32_t i = 0; i < parent_len; ++i) parent[i] = path[i];
        parent[parent_len] = '\0';
    }
    return true;
}

bool create_directory_entry(uint32_t parent_cluster, const char *name,
                            bool directory, uint32_t first_cluster, uint32_t size,
                            FatNode *created) {
    uint8_t short_name[11];
    uint8_t ntres;
    bool needs_lfn;
    if (!make_short_alias(parent_cluster, name, short_name, &ntres, &needs_lfn)) return false;
    uint16_t utf16_name[kMaximumName];
    uint32_t name_length = 0;
    if (!utf8_to_utf16_name(name, utf16_name, &name_length)) return false;
    const uint32_t lfn_count = needs_lfn ? (name_length + 1 + 12) / 13 : 0;
    if (lfn_count > kMaximumLfnEntries) return false;
    const uint32_t needed = lfn_count + 1;
    uint64_t start_slot;
    if (!reserve_directory_slots(parent_cluster, needed, &start_slot)) return false;

    uint8_t checksum = lfn_checksum(short_name);
    for (uint32_t disk_index = 0; disk_index < lfn_count; ++disk_index) {
        const uint32_t order = lfn_count - disk_index;
        uint8_t entry[32];
        for (uint32_t i = 0; i < 32; ++i) entry[i] = 0xff;
        entry[0] = static_cast<uint8_t>(order | (order == lfn_count ? 0x40 : 0));
        entry[11] = 0x0f; entry[12] = 0; entry[13] = checksum; wr16(entry + 26, 0);
        const uint8_t offsets[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
        const uint32_t base = (order - 1) * 13;
        for (uint32_t i = 0; i < 13; ++i) {
            uint16_t ch = 0xffff;
            const uint32_t index = base + i;
            if (index < name_length) ch = utf16_name[index];
            else if (index == name_length) ch = 0;
            wr16(entry + offsets[i], ch);
        }
        if (!write_directory_slot(parent_cluster, start_slot + disk_index, entry)) return false;
    }

    uint8_t short_entry[32]{};
    copy_bytes(short_entry, short_name, 11);
    short_entry[11] = directory ? 0x10 : 0x20;
    short_entry[12] = ntres;
    wr16(short_entry + 16, 0x0021); // 1980-01-01, valid FAT date until an RTC is available.
    wr16(short_entry + 18, 0x0021);
    wr16(short_entry + 24, 0x0021);
    wr16(short_entry + 20, static_cast<uint16_t>(first_cluster >> 16));
    wr16(short_entry + 26, static_cast<uint16_t>(first_cluster));
    wr32(short_entry + 28, size);
    if (!write_directory_slot(parent_cluster, start_slot + lfn_count, short_entry)) return false;
    if (created) {
        zero_bytes(created, sizeof(*created));
        copy_text(created->name, sizeof(created->name), name);
        created->found = true; created->directory = directory;
        created->first_cluster = first_cluster; created->size = size;
        created->parent_cluster = parent_cluster; created->lfn_slots = lfn_count;
        created->short_slot = start_slot + lfn_count;
        copy_bytes(created->short_entry, short_entry, 32);
    }
    return true;
}

bool update_node_entry(const FatNode &node, uint32_t first_cluster, uint32_t size) {
    if (node.short_slot == kNoSlot) return false;
    uint8_t entry[32];
    copy_bytes(entry, node.short_entry, 32);
    wr16(entry + 20, static_cast<uint16_t>(first_cluster >> 16));
    wr16(entry + 26, static_cast<uint16_t>(first_cluster));
    wr32(entry + 28, size);
    if (!write_directory_slot(node.parent_cluster, node.short_slot, entry)) return false;
    return true;
}

bool mark_node_deleted(const FatNode &node) {
    if (node.short_slot == kNoSlot) return false;
    const uint64_t first = node.short_slot - node.lfn_slots;
    uint8_t deleted[32];
    for (uint32_t i = 0; i <= node.lfn_slots; ++i) {
        uint32_t cluster;
        const uint64_t entries_per_cluster = gVolume.cluster_size / 32;
        if (!chain_cluster_at(node.parent_cluster, (first + i) / entries_per_cluster, &cluster)) return false;
        const uint64_t offset = cluster_byte_offset(cluster) + ((first + i) % entries_per_cluster) * 32;
        if (!volume_read_bytes(offset, deleted, sizeof(deleted))) return false;
        deleted[0] = 0xe5;
        if (!volume_write_bytes(offset, deleted, sizeof(deleted))) return false;
    }
    return true;
}

bool create_directory_locked(const char *path) {
    char parent_path[1024], leaf[1024];
    if (!parent_and_leaf(path, parent_path, sizeof(parent_path), leaf, sizeof(leaf))) return false;
    FatNode parent{}, exists{};
    bool found = false;
    if (!find_node(parent_path, &parent) || !parent.directory ||
        !lookup_child(parent.first_cluster, parent.first_cluster, leaf, &exists, &found) || found)
        return false;
    uint32_t directory_cluster;
    if (!allocate_cluster(&directory_cluster)) return false;
    if (!zero_cluster(directory_cluster)) {
        free_chain(directory_cluster);
        return false;
    }

    uint8_t dot_entries[64]{};
    for (uint32_t i = 0; i < 11; ++i) dot_entries[i] = ' ';
    dot_entries[0] = '.'; dot_entries[11] = 0x10;
    wr16(dot_entries + 16, 0x0021); wr16(dot_entries + 18, 0x0021); wr16(dot_entries + 24, 0x0021);
    wr16(dot_entries + 20, static_cast<uint16_t>(directory_cluster >> 16));
    wr16(dot_entries + 26, static_cast<uint16_t>(directory_cluster));
    for (uint32_t i = 32; i < 43; ++i) dot_entries[i] = ' ';
    dot_entries[32] = '.'; dot_entries[33] = '.'; dot_entries[43] = 0x10;
    wr16(dot_entries + 48, 0x0021); wr16(dot_entries + 50, 0x0021); wr16(dot_entries + 56, 0x0021);
    const uint32_t parent_cluster = parent.first_cluster == gVolume.root_cluster ? 0 : parent.first_cluster;
    wr16(dot_entries + 52, static_cast<uint16_t>(parent_cluster >> 16));
    wr16(dot_entries + 58, static_cast<uint16_t>(parent_cluster));
    if (!volume_write_bytes(cluster_byte_offset(directory_cluster), dot_entries, sizeof(dot_entries)) ||
        !welovec::block::flush(gVolume.device)) {
        free_chain(directory_cluster);
        welovec::block::flush(gVolume.device);
        return false;
    }
    if (!create_directory_entry(parent.first_cluster, leaf, true, directory_cluster, 0, nullptr)) {
        // The directory entry write may have partially reached the device.
        // Keep its target cluster allocated if the result is uncertain.
        welovec::block::flush(gVolume.device);
        return false;
    }
    return welovec::block::flush(gVolume.device);
}

bool write_file_locked(const char *path, const uint8_t *data, uint32_t size) {
    if (size && !data) return false;
    const uint32_t path_length = string_length(path);
    if (!path_length || path[path_length - 1] == '/' || path[path_length - 1] == '\\') return false;
    char parent_path[1024], leaf[1024];
    if (!parent_and_leaf(path, parent_path, sizeof(parent_path), leaf, sizeof(leaf))) return false;
    FatNode parent{}, node{};
    if (!find_node(parent_path, &parent) || !parent.directory) return false;
    bool exists = false;
    if (!lookup_child(parent.first_cluster, parent.first_cluster, leaf, &node, &exists)) return false;
    if (exists && (node.directory || (node.short_entry[11] & 0x01))) return false;
    if (!exists && size == 0) {
        if (!create_directory_entry(parent.first_cluster, leaf, false, 0, 0, nullptr)) return false;
        return welovec::block::flush(gVolume.device);
    }

    const uint64_t clusters_needed = size ? (static_cast<uint64_t>(size) + gVolume.cluster_size - 1) /
                                           gVolume.cluster_size : 0;
    uint32_t new_first = 0, new_last = 0;
    if (clusters_needed && !allocate_chain(clusters_needed, &new_first, &new_last)) return false;
    if (size && !write_file_data_chain(new_first, data, size)) {
        if (new_first) free_chain(new_first);
        welovec::block::flush(gVolume.device);
        return false;
    }
    if (size && !welovec::block::flush(gVolume.device)) {
        // Leave the new chain allocated if the data flush failed. The device
        // may have persisted part of it, so reclaiming it could corrupt a
        // directory entry if a later metadata write reached the device.
        return false;
    }

    if (exists) {
        const uint32_t old_first = node.first_cluster;
        if (!update_node_entry(node, new_first, size)) {
            // The directory entry may have been partially written, so do not
            // release the replacement chain on an ambiguous device error.
            welovec::block::flush(gVolume.device);
            return false;
        }
        if (!welovec::block::flush(gVolume.device)) return false;
        if (old_first && !free_chain(old_first)) {
            welovec::block::flush(gVolume.device);
            return false;
        }
        return welovec::block::flush(gVolume.device);
    }
    if (!create_directory_entry(parent.first_cluster, leaf, false, new_first, size, nullptr)) {
        // The short directory entry may have reached the device even if the
        // write reported an error; keep the referenced data allocated.
        welovec::block::flush(gVolume.device);
        return false;
    }
    return welovec::block::flush(gVolume.device);
}

bool delete_node_locked(const FatNode &node) {
    if (!node.found || node.short_slot == kNoSlot ||
        (node.short_entry[11] & 0x01) ||
        (node.directory && directory_has_child(node.first_cluster, node.first_cluster))) return false;
    if (!mark_node_deleted(node)) return false;
    if (!welovec::block::flush(gVolume.device)) return false;
    if (node.first_cluster && !free_chain(node.first_cluster)) {
        welovec::block::flush(gVolume.device);
        return false;
    }
    return welovec::block::flush(gVolume.device);
}

uint32_t crc32(const uint8_t *data, uint64_t length) {
    uint32_t crc = 0xffffffff;
    for (uint64_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320 & (0u - (crc & 1)));
    }
    return ~crc;
}

bool probe_partition(Device *device, uint64_t first, uint64_t count) {
    Volume mounted{};
    if (!mount_candidate(device, first, count, &mounted)) return false;
    gVolume = mounted;
    return true;
}

bool scan_gpt(Device *device) {
    if (device->block_count < 3 || device->block_size < 512 ||
        device->block_size > kMaximumSectorSize) return false;
    uint8_t header[kMaximumSectorSize];
    if (!device_read(device, 1, 1, header) ||
        header[0] != 'E' || header[1] != 'F' || header[2] != 'I' || header[3] != ' ' ||
        header[4] != 'P' || header[5] != 'A' || header[6] != 'R' || header[7] != 'T') return false;
    const uint32_t header_size = rd32(header + 12);
    const uint32_t header_crc = rd32(header + 16);
    if (header_size < 92 || header_size > device->block_size || rd64(header + 24) != 1) return false;
    wr32(header + 16, 0);
    if (crc32(header, header_size) != header_crc) return false;
    const uint64_t entry_lba = rd64(header + 72);
    const uint32_t entry_count = rd32(header + 80);
    const uint32_t entry_size = rd32(header + 84);
    const uint32_t entries_crc = rd32(header + 88);
    if (!entry_count || entry_count > 4096 || entry_size < 128 || entry_size > 4096 ||
        (entry_size & 7)) return false;
    const uint64_t bytes = static_cast<uint64_t>(entry_count) * entry_size;
    if (bytes > 16 * 1024 * 1024 || entry_lba >= device->block_count ||
        (bytes + device->block_size - 1) / device->block_size > device->block_count - entry_lba)
        return false;
    auto *entries = static_cast<uint8_t *>(welovec::memory::heap::allocate(bytes));
    if (!entries) return false;
    const uint64_t sectors = (bytes + device->block_size - 1) / device->block_size;
    auto *raw = static_cast<uint8_t *>(welovec::memory::heap::allocate(sectors * device->block_size));
    if (!raw) { welovec::memory::heap::deallocate(entries); return false; }
    bool ok = device_read(device, entry_lba, static_cast<uint32_t>(sectors), raw);
    if (ok) {
        copy_bytes(entries, raw, bytes);
        ok = crc32(entries, bytes) == entries_crc;
    }
    bool found = false;
    if (ok) {
        for (uint32_t i = 0; i < entry_count && !found; ++i) {
            const uint8_t *entry = entries + static_cast<uint64_t>(i) * entry_size;
            bool empty = true;
            for (uint32_t j = 0; j < 16; ++j) if (entry[j]) { empty = false; break; }
            if (empty) continue;
            const uint64_t start = rd64(entry + 32);
            const uint64_t end = rd64(entry + 40);
            if (start > end || end >= device->block_count) continue;
            found = probe_partition(device, start, end - start + 1);
        }
    }
    welovec::memory::heap::deallocate(raw);
    welovec::memory::heap::deallocate(entries);
    return found;
}

bool scan_device(Device *device) {
    if (!device || device->block_count == 0 || device->block_size > kMaximumSectorSize) return false;
    if (probe_partition(device, 0, device->block_count)) return true;
    uint8_t sector[kMaximumSectorSize];
    if (!device_read(device, 0, 1, sector) || device->block_size < 512 ||
        sector[510] != 0x55 || sector[511] != 0xaa) return scan_gpt(device);
    if (sector[450] == 0xee || sector[466] == 0xee ||
        sector[482] == 0xee || sector[498] == 0xee)
        if (scan_gpt(device)) return true;

    for (uint32_t i = 0; i < 4; ++i) {
        const uint8_t *partition = sector + 446 + i * 16;
        const uint8_t type = partition[4];
        const uint32_t first = rd32(partition + 8);
        const uint32_t count = rd32(partition + 12);
        if (!type || !first || !count || first >= device->block_count ||
            count > device->block_count - first) continue;
        if (probe_partition(device, first, count)) return true;
        if (type == 0x05 || type == 0x0f || type == 0x85) {
            uint64_t ebr = first;
            for (uint32_t logical = 0; logical < 128 && ebr < device->block_count; ++logical) {
                uint8_t ebr_sector[kMaximumSectorSize];
                if (!device_read(device, ebr, 1, ebr_sector) || ebr_sector[510] != 0x55 || ebr_sector[511] != 0xaa) break;
                const uint8_t *logical_part = ebr_sector + 446;
                const uint32_t rel = rd32(logical_part + 8);
                const uint32_t size = rd32(logical_part + 12);
                if (logical_part[4] && rel && size && ebr + rel < device->block_count &&
                    size <= device->block_count - (ebr + rel) &&
                    probe_partition(device, ebr + rel, size)) return true;
                const uint8_t *next = ebr_sector + 462;
                const uint32_t next_rel = rd32(next + 8);
                if (!next[4] || !next_rel || ebr + next_rel >= device->block_count) break;
                ebr = first + next_rel;
            }
        }
    }
    return false;
}

bool raw_read(Device *device, uint64_t lba, uint32_t count, void *buffer) {
    auto *image = static_cast<RawMemoryImage *>(device->context);
    if (!image || lba > device->block_count || count > device->block_count - lba) return false;
    copy_bytes(buffer, image->bytes + lba * device->block_size,
               static_cast<uint64_t>(count) * device->block_size);
    return true;
}

bool raw_write(Device *device, uint64_t lba, uint32_t count, const void *buffer) {
    auto *image = static_cast<RawMemoryImage *>(device->context);
    if (!image || lba > device->block_count || count > device->block_count - lba) return false;
    copy_bytes(image->bytes + lba * device->block_size, buffer,
               static_cast<uint64_t>(count) * device->block_size);
    return true;
}

}

bool fat32_mount(void *image, uint64_t image_size) {
    if (!image || image_size < 512) return false;
    const uint64_t irq = gFatLock.lock();
    gRawImage.bytes = static_cast<uint8_t *>(image);
    gRawImage.size = image_size;
    gRawImageDevice = {};
    copy_text(gRawImageDevice.name, sizeof(gRawImageDevice.name), "fat32-image");
    gRawImageDevice.context = &gRawImage;
    gRawImageDevice.block_size = 512;
    gRawImageDevice.block_count = image_size / 512;
    gRawImageDevice.read_only = false;
    gRawImageDevice.read = raw_read;
    gRawImageDevice.write = raw_write;
    gRawImageDevice.flush = nullptr;
    Volume mounted{};
    bool ok = mount_candidate(&gRawImageDevice, 0, gRawImageDevice.block_count, &mounted);
    if (!ok) ok = scan_device(&gRawImageDevice);
    gFatLock.unlock(irq);
    return ok;
}

bool fat32_mount_device(Device *device, uint64_t first_block, uint64_t block_count) {
    const uint64_t irq = gFatLock.lock();
    Volume mounted{};
    const bool ok = mount_candidate(device, first_block, block_count, &mounted);
    if (ok) gVolume = mounted;
    gFatLock.unlock(irq);
    return ok;
}

bool fat32_mount_auto() {
    if (fat32_is_mounted()) return true;
    const uint64_t irq = gFatLock.lock();
    bool found = false;
    const uint32_t count = welovec::block::device_count();
    for (uint32_t i = 0; i < count && !found; ++i)
        found = scan_device(welovec::block::device_at(i));
    gFatLock.unlock(irq);
    return found;
}

bool fat32_is_mounted() {
    const uint64_t irq = gFatLock.lock();
    const bool result = gVolume.mounted;
    gFatLock.unlock(irq);
    return result;
}

bool fat32_path_is_mounted(const char *path, char *relative_path, uint32_t capacity) {
    if (!path || !relative_path || capacity < 2) return false;
    const uint64_t irq = gFatLock.lock();
    if (!gVolume.mounted) { gFatLock.unlock(irq); return false; }
    for (const MountAlias &alias : kMountAliases) {
        const uint32_t mount_length = string_length(alias.path);
        uint32_t index = 0;
        while (index < mount_length && path[index] == alias.path[index]) ++index;
        if (index != mount_length ||
            (path[index] && path[index] != '/' && path[index] != '\\')) continue;

        const char *suffix = path + mount_length;
        const uint32_t root_length = string_length(alias.volume_path);
        const uint32_t suffix_length = string_length(suffix);
        if (root_length + suffix_length >= capacity) {
            gFatLock.unlock(irq);
            return false;
        }
        uint32_t output = 0;
        while (alias.volume_path[output]) {
            relative_path[output] = alias.volume_path[output];
            ++output;
        }
        for (uint32_t part = 0; suffix[part]; ++part)
            relative_path[output++] = suffix[part];
        relative_path[output] = '\0';
        gFatLock.unlock(irq);
        return true;
    }
    gFatLock.unlock(irq);
    return false;
}

bool fat32_file_exists(const char *path) {
    const uint64_t irq = gFatLock.lock();
    FatNode node{};
    const bool result = find_node(path, &node) && node.found;
    gFatLock.unlock(irq);
    return result;
}

bool fat32_is_directory(const char *path) {
    const uint64_t irq = gFatLock.lock();
    FatNode node{};
    const bool result = find_node(path, &node) && node.found && node.directory;
    gFatLock.unlock(irq);
    return result;
}

uint32_t fat32_file_size(const char *path) {
    const uint64_t irq = gFatLock.lock();
    FatNode node{};
    const bool ok = find_node(path, &node) && node.found && !node.directory;
    const uint32_t size = ok ? node.size : 0;
    gFatLock.unlock(irq);
    return size;
}

bool fat32_read_file(const char *path, uint8_t **out_data, uint32_t *out_size) {
    if (!out_data || !out_size) return false;
    *out_data = nullptr; *out_size = 0;
    const uint64_t irq = gFatLock.lock();
    FatNode node{};
    if (!find_node(path, &node) || !node.found || node.directory || node.size == UINT32_MAX) {
        gFatLock.unlock(irq); return false;
    }
    auto *data = static_cast<uint8_t *>(welovec::memory::heap::allocate(static_cast<size_t>(node.size) + 1));
    if (!data) { gFatLock.unlock(irq); return false; }
    if (!read_file_data(node, data)) {
        welovec::memory::heap::deallocate(data);
        gFatLock.unlock(irq); return false;
    }
    data[node.size] = 0;
    *out_data = data; *out_size = node.size;
    gFatLock.unlock(irq);
    return true;
}

bool fat32_write_file(const char *path, const uint8_t *data, uint32_t size) {
    const uint64_t irq = gFatLock.lock();
    bool result = gVolume.mounted && write_file_locked(path, data, size);
    if (!result && gVolume.mounted) welovec::block::flush(gVolume.device);
    gFatLock.unlock(irq);
    return result;
}

bool fat32_make_directory(const char *path) {
    const uint64_t irq = gFatLock.lock();
    FatNode existing{};
    bool result = false;
    if (gVolume.mounted && find_node(path, &existing) && existing.found)
        result = existing.directory;
    else if (gVolume.mounted) result = create_directory_locked(path);
    gFatLock.unlock(irq);
    return result;
}

bool fat32_delete(const char *path) {
    const uint64_t irq = gFatLock.lock();
    FatNode node{};
    bool result = false;
    if (gVolume.mounted && find_node(path, &node) && node.found &&
        !(node.name[0] == '.' && (!node.name[1] || node.name[1] == '.')))
        result = delete_node_locked(node);
    if (!result && gVolume.mounted) welovec::block::flush(gVolume.device);
    gFatLock.unlock(irq);
    return result;
}

bool fat32_move(const char *old_path, const char *new_path) {
    const uint64_t irq = gFatLock.lock();
    FatNode source{};
    bool result = false;
    if (gVolume.mounted && find_node(old_path, &source) && source.found && !source.directory &&
        !(source.short_entry[11] & 0x01)) {
        char parent_path[1024], leaf[1024];
        FatNode parent{}, target{};
        bool target_exists = false;
        if (!parent_and_leaf(new_path, parent_path, sizeof(parent_path), leaf, sizeof(leaf)) ||
            !find_node(parent_path, &parent) || !parent.directory ||
            !lookup_child(parent.first_cluster, parent.first_cluster, leaf, &target, &target_exists) ||
            target_exists) {
            gFatLock.unlock(irq);
            return false;
        }
        auto *buffer = static_cast<uint8_t *>(welovec::memory::heap::allocate(static_cast<size_t>(source.size) + 1));
        if (buffer && read_file_data(source, buffer) && write_file_locked(new_path, buffer, source.size)) {
            result = mark_node_deleted(source);
            if (result) result = welovec::block::flush(gVolume.device);
            if (result && source.first_cluster) result = free_chain(source.first_cluster);
            if (result) result = welovec::block::flush(gVolume.device);
        }
        if (buffer) welovec::memory::heap::deallocate(buffer);
    }
    gFatLock.unlock(irq);
    return result;
}

int fat32_list_range(const char *path, VfsListEntry *out_entries,
                     int max_entries, uint64_t offset) {
    if (!out_entries || max_entries <= 0) return 0;
    const uint64_t irq = gFatLock.lock();
    FatNode directory{};
    if (!gVolume.mounted || !find_node(path, &directory) || !directory.found || !directory.directory) {
        gFatLock.unlock(irq); return 0;
    }
    auto *buffer = static_cast<uint8_t *>(welovec::memory::heap::allocate(gVolume.cluster_size));
    if (!buffer) { gFatLock.unlock(irq); return 0; }
    uint32_t cluster = directory.first_cluster;
    int count = 0;
    uint64_t skipped = 0;
    for (uint64_t chain = 0; cluster_valid(cluster) && chain < gVolume.cluster_count && count < max_entries; ++chain) {
        if (!volume_read_bytes(cluster_byte_offset(cluster), buffer, gVolume.cluster_size)) break;
        const uint32_t entries = gVolume.cluster_size / 32;
        LfnState lfn{}; lfn_reset(&lfn);
        for (uint32_t i = 0; i < entries && count < max_entries; ++i) {
            const uint8_t *entry = buffer + static_cast<uint64_t>(i) * 32;
            if (!entry[0]) { cluster = 0; break; }
            if (entry[0] == 0xe5) { lfn_reset(&lfn); continue; }
            if (lfn_is_entry(entry)) { lfn_add(&lfn, entry); continue; }
            if (entry[11] & 0x08) { lfn_reset(&lfn); continue; }
            char long_name[1024], short_name[16];
            const bool has_lfn = lfn_decode(&lfn, entry, long_name, sizeof(long_name));
            short_name_decode(entry, short_name, sizeof(short_name));
            const char *name = has_lfn ? long_name : short_name;
            if (equal_ci(name, ".") || equal_ci(name, "..")) { lfn_reset(&lfn); continue; }
            if (skipped < offset) {
                ++skipped;
                lfn_reset(&lfn);
                continue;
            }
            copy_text(out_entries[count].name, sizeof(out_entries[count].name), name);
            out_entries[count].size = rd32(entry + 28);
            out_entries[count].is_directory = (entry[11] & 0x10) != 0;
            ++count;
            lfn_reset(&lfn);
        }
        if (!cluster || count >= max_entries) break;
        uint32_t next;
        if (!directory_next(cluster, &next) || !next) break;
        cluster = next;
    }
    welovec::memory::heap::deallocate(buffer);
    gFatLock.unlock(irq);
    return count;
}

int fat32_list(const char *path, VfsListEntry *out_entries, int max_entries) {
    return fat32_list_range(path, out_entries, max_entries, 0);
}

