// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Validates and loads ELF64 executables into user processes.

// Written by [@saphhic](https://github.com/saphhic)  
// Date: October 30th 2026

#include "elf_loader.h"

#include "arch/common/MM/Heap/heap.hpp"
#include "arch/common/Process/manager.hpp"
#include "arch/x86_64/mm/paging/page_table.hpp"
#include "arch/x86_64/scheduler/scheduler.hpp"
#include "filesystem/vfs.h"
#include "security/privileges.hpp"

namespace {
constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kMinimumLoadAddress = 0x11000; // 0x10000 contains the thread-return stub.
constexpr uint32_t kMaximumFileSize = 64 * 1024 * 1024;
constexpr uint64_t kMaximumMappedPages = 32768; // 128 MiB of virtual image memory.
constexpr uint16_t kMaximumProgramHeaders = 64;
constexpr uint32_t kMaximumArguments = 64;

constexpr uint8_t kElfClass64 = 2;
constexpr uint8_t kElfLittleEndian = 1;
constexpr uint8_t kElfCurrentVersion = 1;
constexpr uint16_t kElfTypeExecutable = 2;
constexpr uint16_t kElfMachineX86_64 = 62;
constexpr uint32_t kProgramLoad = 1;
constexpr uint32_t kProgramDynamic = 2;
constexpr uint32_t kProgramInterpreter = 3;
constexpr uint32_t kProgramTls = 7;
constexpr uint32_t kProgramGnuStack = 0x6474e551;
constexpr uint32_t kFlagExecute = 1;
constexpr uint32_t kFlagWrite = 2;
constexpr uint32_t kFlagRead = 4;

#pragma pack(push, 1)
struct Elf64Header {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_header_offset;
    uint64_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_entry_size;
    uint16_t program_header_count;
    uint16_t section_header_entry_size;
    uint16_t section_header_count;
    uint16_t section_name_index;
};

struct Elf64ProgramHeader {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
};
#pragma pack(pop)

static_assert(sizeof(Elf64Header) == 64, "ELF64 header layout must match the ABI");
static_assert(sizeof(Elf64ProgramHeader) == 56, "ELF64 program header layout must match the ABI");

struct LoadSegment {
    Elf64ProgramHeader header;
    uint64_t page_start;
    uint64_t page_end;
};

struct LoadSegmentStorage {
    LoadSegment *items;
    ~LoadSegmentStorage() {
        if (items) welovec::memory::heap::deallocate(items);
    }
};

void copy_bytes(void *destination, const void *source, uint64_t size) {
    auto *out = static_cast<uint8_t *>(destination);
    const auto *in = static_cast<const uint8_t *>(source);
    for (uint64_t i = 0; i < size; ++i) out[i] = in[i];
}

uint64_t user_limit() {
    return welovec::x86_64::mm::paging::active_levels() == 5
        ? UINT64_C(0x0100000000000000) : UINT64_C(0x0000800000000000);
}

bool ranges_overlap(uint64_t a_begin, uint64_t a_end,
                    uint64_t b_begin, uint64_t b_end) {
    return a_begin < b_end && b_begin < a_end;
}

welovec_exec_error validate_image(const uint8_t *image, uint32_t image_size,
                                  Elf64Header *out_header, LoadSegment *segments,
                                  uint32_t *out_segment_count, uint64_t *out_program_headers,
                                  uint64_t *out_total_pages, uint64_t *out_image_end) {
    if (!image || image_size < sizeof(Elf64Header) || !out_header || !segments ||
        !out_segment_count || !out_program_headers || !out_total_pages || !out_image_end)
        return WELOVEC_EXEC_INVALID_ELF;
    if (!welovec::x86_64::mm::paging::nx_enabled())
        return WELOVEC_EXEC_UNSUPPORTED_ELF;

    Elf64Header header{};
    copy_bytes(&header, image, sizeof(header));
    if (header.ident[0] != 0x7f || header.ident[1] != 'E' ||
        header.ident[2] != 'L' || header.ident[3] != 'F' ||
        header.ident[4] != kElfClass64 || header.ident[5] != kElfLittleEndian ||
        header.ident[6] != kElfCurrentVersion || header.version != kElfCurrentVersion ||
        header.header_size != sizeof(Elf64Header)) return WELOVEC_EXEC_INVALID_ELF;
    if (header.type != kElfTypeExecutable || header.machine != kElfMachineX86_64 ||
        header.program_header_entry_size != sizeof(Elf64ProgramHeader) ||
        !header.program_header_count || header.program_header_count > kMaximumProgramHeaders)
        return WELOVEC_EXEC_UNSUPPORTED_ELF;

    const uint64_t program_header_bytes =
        static_cast<uint64_t>(header.program_header_count) * sizeof(Elf64ProgramHeader);
    if (header.program_header_offset > image_size ||
        program_header_bytes > image_size - header.program_header_offset)
        return WELOVEC_EXEC_INVALID_ELF;

    const uint64_t limit = user_limit();
    uint32_t segment_count = 0;
    uint64_t total_pages = 0;
    uint64_t image_end = 0;
    bool entry_is_executable = false;
    uint64_t program_headers_address = 0;

    for (uint32_t i = 0; i < header.program_header_count; ++i) {
        Elf64ProgramHeader ph{};
        copy_bytes(&ph, image + header.program_header_offset +
                         static_cast<uint64_t>(i) * sizeof(ph), sizeof(ph));
        if (ph.type == kProgramInterpreter || ph.type == kProgramDynamic || ph.type == kProgramTls)
            return WELOVEC_EXEC_UNSUPPORTED_ELF;
        if (ph.type == kProgramGnuStack && (ph.flags & kFlagExecute))
            return WELOVEC_EXEC_UNSUPPORTED_ELF;
        if (ph.type != kProgramLoad || !ph.memory_size) continue;

        if (ph.flags & ~(kFlagRead | kFlagWrite | kFlagExecute))
            return WELOVEC_EXEC_INVALID_SEGMENT;
        if ((ph.flags & (kFlagWrite | kFlagExecute)) == (kFlagWrite | kFlagExecute))
            return WELOVEC_EXEC_INVALID_SEGMENT;
        if (ph.file_size > ph.memory_size || ph.offset > image_size ||
            ph.file_size > image_size - ph.offset ||
            ph.virtual_address > UINT64_MAX - ph.memory_size)
            return WELOVEC_EXEC_INVALID_SEGMENT;
        if (ph.alignment > 1 &&
            ((ph.alignment & (ph.alignment - 1)) ||
             (ph.virtual_address & (ph.alignment - 1)) != (ph.offset & (ph.alignment - 1))))
            return WELOVEC_EXEC_INVALID_SEGMENT;

        const uint64_t memory_end = ph.virtual_address + ph.memory_size;
        if (ph.virtual_address < kMinimumLoadAddress || memory_end > limit ||
            memory_end > UINT64_MAX - (kPageSize - 1))
            return WELOVEC_EXEC_INVALID_SEGMENT;
        const uint64_t page_start = ph.virtual_address & ~(kPageSize - 1);
        const uint64_t page_end = (memory_end + kPageSize - 1) & ~(kPageSize - 1);
        const uint64_t page_count = (page_end - page_start) / kPageSize;
        if (memory_end > image_end) image_end = memory_end;
        if (!page_count || page_count > kMaximumMappedPages - total_pages)
            return WELOVEC_EXEC_INVALID_SEGMENT;
        if (segment_count >= kMaximumProgramHeaders)
            return WELOVEC_EXEC_INVALID_SEGMENT;

        for (uint32_t previous = 0; previous < segment_count; ++previous) {
            if (ranges_overlap(page_start, page_end,
                              segments[previous].page_start, segments[previous].page_end))
                return WELOVEC_EXEC_OVERLAPPING_SEGMENTS;
        }

        segments[segment_count++] = LoadSegment{ph, page_start, page_end};
        total_pages += page_count;

        if ((ph.flags & kFlagExecute) && header.entry >= ph.virtual_address &&
            header.entry < memory_end) entry_is_executable = true;

        if (header.program_header_offset >= ph.offset &&
            program_header_bytes <= ph.file_size &&
            header.program_header_offset - ph.offset <= ph.file_size - program_header_bytes) {
            program_headers_address = ph.virtual_address +
                                      (header.program_header_offset - ph.offset);
        }
    }

    if (!segment_count || !entry_is_executable || header.entry < kMinimumLoadAddress ||
        header.entry >= limit) return WELOVEC_EXEC_INVALID_ELF;
    *out_header = header;
    *out_segment_count = segment_count;
    *out_program_headers = program_headers_address;
    *out_total_pages = total_pages;
    *out_image_end = image_end;
    return WELOVEC_EXEC_SUCCESS;
}

} // namespace

static enum welovec_exec_error launch_elf64(
    const char *path, const struct welovec_exec_options *options,
    struct welovec_exec_result *out_result,
    const welovec::security::Credentials &credentials) {
    using welovec::process::ProgramStartupInfo;
    using welovec::x86_64::scheduler::Priority;

    if (out_result) *out_result = {};
    if (!path || !path[0] || !out_result) return WELOVEC_EXEC_INVALID_ARGUMENT;

    uint32_t argument_count = options ? options->argument_count : 0;
    const char *const *arguments = options ? options->arguments : nullptr;
    const uint8_t priority = options ? options->priority :
        static_cast<uint8_t>(Priority::Normal);
    const uint32_t framebuffer_width = options ? options->framebuffer_width : 0;
    const uint32_t framebuffer_height = options ? options->framebuffer_height : 0;
    const char *default_arguments[1] = {path};
    if (priority > static_cast<uint8_t>(Priority::Low) ||
        ((framebuffer_width == 0) != (framebuffer_height == 0)) ||
        argument_count > kMaximumArguments || (argument_count && !arguments))
        return WELOVEC_EXEC_INVALID_ARGUMENT;
    if (!argument_count) {
        arguments = default_arguments;
        argument_count = 1;
    }
    for (uint32_t i = 0; i < argument_count; ++i)
        if (!arguments[i]) return WELOVEC_EXEC_INVALID_ARGUMENT;

    if (!file_exists(path)) return WELOVEC_EXEC_FILE_NOT_FOUND;
    if (fs_is_directory(path)) return WELOVEC_EXEC_NOT_REGULAR_FILE;
    const uint32_t reported_size = fs_file_size(path);
    if (reported_size > kMaximumFileSize) return WELOVEC_EXEC_FILE_TOO_LARGE;
    if (reported_size < sizeof(Elf64Header)) return WELOVEC_EXEC_INVALID_ELF;

    uint8_t *image = nullptr;
    uint32_t image_size = 0;
    if (!vfs_read_file(path, &image, &image_size) || !image)
        return WELOVEC_EXEC_READ_FAILED;
    if (image_size > kMaximumFileSize) {
        vfs_free_file(image);
        return WELOVEC_EXEC_FILE_TOO_LARGE;
    }

    Elf64Header header{};
    LoadSegmentStorage segment_storage{
        static_cast<LoadSegment *>(welovec::memory::heap::allocate_zeroed(
            sizeof(LoadSegment) * kMaximumProgramHeaders))
    };
    if (!segment_storage.items) {
        vfs_free_file(image);
        return WELOVEC_EXEC_OUT_OF_MEMORY;
    }
    uint32_t segment_count = 0;
    uint64_t program_headers_address = 0;
    uint64_t total_pages = 0;
    uint64_t image_end = 0;
    welovec_exec_error error = validate_image(image, image_size, &header,
                                               segment_storage.items,
                                               &segment_count, &program_headers_address,
                                               &total_pages, &image_end);
    (void)total_pages;
    if (error != WELOVEC_EXEC_SUCCESS) {
        vfs_free_file(image);
        return error;
    }

    const uint64_t process_id = welovec::process::create_with_credentials(path, credentials);
    if (!process_id) {
        vfs_free_file(image);
        return WELOVEC_EXEC_PROCESS_FAILED;
    }

    bool loaded = true;
    for (uint32_t i = 0; loaded && i < segment_count; ++i) {
        const LoadSegment &segment = segment_storage.items[i];
        const uint64_t page_count = (segment.page_end - segment.page_start) / kPageSize;
        loaded = welovec::process::allocate_user_pages(
            process_id, segment.page_start, page_count, true, false);
        if (loaded && segment.header.file_size)
            loaded = welovec::process::write_user_memory(
                process_id, segment.header.virtual_address,
                image + segment.header.offset, segment.header.file_size);
        if (loaded)
            loaded = welovec::process::protect_user_pages(
                process_id, segment.page_start, page_count,
                (segment.header.flags & kFlagWrite) != 0,
                (segment.header.flags & kFlagExecute) != 0);
    }

    uint64_t thread_id = 0;
    welovec_exec_error launch_error = WELOVEC_EXEC_OUT_OF_MEMORY;
    if (loaded && framebuffer_width &&
        !welovec::process::initialize_windowed_process(
            process_id, framebuffer_width, framebuffer_height))
        loaded = false;
    if (loaded) {
        const ProgramStartupInfo startup{
            program_headers_address,
            header.program_header_entry_size,
            header.program_header_count,
            header.entry,
            image_end
        };
        thread_id = welovec::process::create_user_program_thread(
            process_id, path, header.entry, argument_count, arguments, &startup, priority);
        loaded = thread_id != 0;
        if (!loaded) launch_error = WELOVEC_EXEC_THREAD_FAILED;
    }
    vfs_free_file(image);

    if (!loaded) {
        (void)welovec::process::destroy(process_id);
        return launch_error;
    }

    out_result->process_id = process_id;
    out_result->thread_id = thread_id;
    out_result->entry_point = header.entry;
    return WELOVEC_EXEC_SUCCESS;
}

extern "C" enum welovec_exec_error welovec_exec_launch_elf64(
    const char *path, const struct welovec_exec_options *options,
    struct welovec_exec_result *out_result) {
    welovec::security::Credentials credentials{};
    const bool has_user_process = welovec::process::current_id() != 0;
    const bool credentials_ready = has_user_process
        ? welovec::process::current_credentials(&credentials)
        : welovec::security::default_credentials(&credentials);
    if (!credentials_ready) return WELOVEC_EXEC_PROCESS_FAILED;
    return launch_elf64(path, options, out_result, credentials);
}

extern "C" enum welovec_exec_error welovec_exec_launch_elf64_as_current(
    const char *path, const struct welovec_exec_options *options,
    struct welovec_exec_result *out_result) {
    welovec::security::Credentials credentials{};
    if (!welovec::process::current_id() ||
        !welovec::process::current_credentials(&credentials))
        return WELOVEC_EXEC_PROCESS_FAILED;
    return launch_elf64(path, options, out_result, credentials);
}

extern "C" enum welovec_exec_error welovec_exec_launch_elf64_as_admin(
    const char *path, const struct welovec_exec_options *options,
    struct welovec_exec_result *out_result) {
    welovec::security::Credentials credentials{};
    if (!welovec::security::root_credentials(&credentials))
        return WELOVEC_EXEC_PROCESS_FAILED;
    return launch_elf64(path, options, out_result, credentials);
}

extern "C" enum welovec_exec_error welovec_exec_launch_elf64_as_uid(
    const char *path, const struct welovec_exec_options *options,
    struct welovec_exec_result *out_result, uint32_t uid) {
    welovec::security::Credentials credentials{};
    if (!welovec::security::credentials_for_uid(uid, &credentials))
        return WELOVEC_EXEC_INVALID_ARGUMENT;
    return launch_elf64(path, options, out_result, credentials);
}

extern "C" const char *welovec_exec_error_string(enum welovec_exec_error error) {
    switch (error) {
        case WELOVEC_EXEC_SUCCESS: return "success";
        case WELOVEC_EXEC_INVALID_ARGUMENT: return "invalid argument";
        case WELOVEC_EXEC_FILE_NOT_FOUND: return "file not found";
        case WELOVEC_EXEC_NOT_REGULAR_FILE: return "path is a directory";
        case WELOVEC_EXEC_FILE_TOO_LARGE: return "executable exceeds the loader limit";
        case WELOVEC_EXEC_READ_FAILED: return "VFS read failed";
        case WELOVEC_EXEC_INVALID_ELF: return "invalid ELF image";
        case WELOVEC_EXEC_UNSUPPORTED_ELF: return "unsupported ELF feature";
        case WELOVEC_EXEC_INVALID_SEGMENT: return "invalid load segment";
        case WELOVEC_EXEC_OVERLAPPING_SEGMENTS: return "load segments share a page";
        case WELOVEC_EXEC_OUT_OF_MEMORY: return "memory allocation failed";
        case WELOVEC_EXEC_PROCESS_FAILED: return "process creation failed";
        case WELOVEC_EXEC_THREAD_FAILED: return "initial thread creation failed";
        default: return "unknown execution error";
    }
}
