// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements process and thread creation, inspection, and termination.

#include "manager.hpp"
#include "manager.h"
#include "security/privileges.hpp"

#include "arch/common/MM/PMM/pmm.hpp"
#include "arch/common/MM/Heap/heap.hpp"
#include "arch/x86_64/mm/paging/page_table.hpp"
#include "arch/x86_64/mm/physical/physical_memory.hpp"
#include "arch/x86_64/mm/spinlock.hpp"
#include "arch/x86_64/scheduler/scheduler.hpp"
#include "arch/x86_64/syscall/syscall.h"
#include "filesystem/file_descriptors.hpp"

namespace welovec::process {

namespace {
using welovec::x86_64::mm::IrqSpinLock;
using welovec::x86_64::mm::PhysicalAddress;
using welovec::x86_64::mm::paging::Mapping;
using welovec::x86_64::mm::paging::PageFlags;

constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kMinimumUserAddress = 0x10000;
constexpr uint64_t kThreadReturnAddress = kMinimumUserAddress;
constexpr uint64_t kSignalReturnAddress = kThreadReturnAddress + 0x20;
constexpr uint64_t kUserStackPages = 16;
constexpr uint64_t kUserStackSize = kUserStackPages * kPageSize;
constexpr uint64_t kDefaultMmapBase = UINT64_C(0x10000000);
constexpr uint64_t kFallbackMmapSize = UINT64_C(500) * 1024 * 1024;
constexpr uint32_t kMaximumProgramArguments = 64;
constexpr uint64_t kMaximumProgramArgumentBytes = 16 * 1024;
constexpr uint32_t kNameCapacity = 32;
constexpr uint32_t kMaximumMappedRegions = 64;
constexpr uint32_t kSignalLimit = 32;
constexpr uint32_t kSignalInterrupt = 2;
constexpr uint32_t kSignalKill = 9;
constexpr uint32_t kSignalTerminate = 15;
constexpr uint64_t kSignalIgnore = 1;
constexpr uint32_t kWindowCharacterQueueSize = 256;
constexpr uint32_t kWindowPointerQueueSize = 16;

struct WindowInput {
    char characters[kWindowCharacterQueueSize];
    uint32_t character_read;
    uint32_t character_write;
    welovec_pointer_event pointer_events[kWindowPointerQueueSize];
    uint32_t pointer_read;
    uint32_t pointer_write;
    uint32_t references;
};

struct MappedRegion {
    uint64_t base;
    uint64_t size;
    bool used;
};

struct Process {
    uint64_t id;
    uint64_t parent_id;
    PhysicalAddress root;
    State state;
    uint64_t live_threads;
    uint64_t creating_threads;
    uint64_t active_operations;
    bool ever_had_thread;
    uint64_t stack_cursor;
    uint64_t mmap_base;
    MappedRegion mapped_regions[kMaximumMappedRegions];
    uint64_t signal_handlers[kSignalLimit];
    uint64_t pending_signals;
    bool signal_active;
    uint64_t signal_task_id;
    syscall_regs_t saved_signal_context;
    int64_t exit_status;
    int64_t last_thread_status;
    welovec::security::Credentials credentials;
    bool session_authenticated;
    bool owns_window_surface;
    bool window_tree_terminating;
    bool termination_requested;
    bool close_descriptors_pending;
    uint64_t window_root_id;
    WelovecFramebufferSurface *framebuffer_surface;
    WindowInput *window_input;
    char name[kNameCapacity];
    Process *next;
};

struct Manager {
    constexpr Manager() : lock{}, head(nullptr), next_id(1), initialized(false) {}
    IrqSpinLock lock;
    Process *head;
    uint64_t next_id;
    bool initialized;
};

constinit Manager gManager{};

void zero_bytes(void *destination, uint64_t length) {
    auto *bytes = static_cast<uint8_t *>(destination);
    for (uint64_t i = 0; i < length; ++i) bytes[i] = 0;
}

void copy_bytes(void *destination, const void *source, uint64_t length) {
    auto *out = static_cast<uint8_t *>(destination);
    const auto *in = static_cast<const uint8_t *>(source);
    for (uint64_t i = 0; i < length; ++i) out[i] = in[i];
}

void copy_name(char *destination, const char *source) {
    uint32_t i = 0;
    if (source) {
        while (i + 1 < kNameCapacity && source[i]) {
            destination[i] = source[i];
            ++i;
        }
    }
    destination[i] = '\0';
}

uint64_t user_limit() {
    return welovec::x86_64::mm::paging::active_levels() == 5
        ? UINT64_C(0x0100000000000000) : UINT64_C(0x0000800000000000);
}

Process *find_locked(uint64_t process_id) {
    for (Process *process = gManager.head; process; process = process->next)
        if (process->id == process_id) return process;
    return nullptr;
}

bool valid_user_range(uint64_t address, uint64_t size) {
    if (!size) return address >= kMinimumUserAddress && address < user_limit();
    return address >= kMinimumUserAddress && address < user_limit() &&
           size <= user_limit() - address;
}

bool allocate_pages(PhysicalAddress root, uint64_t base, uint64_t page_count,
                    PageFlags flags) {
    if (!page_count || (base & (kPageSize - 1)) || base < kMinimumUserAddress ||
        base >= user_limit() ||
        page_count > (user_limit() - base) / kPageSize) return false;

    if (!welovec::x86_64::mm::paging::nx_enabled())
        flags = static_cast<PageFlags>(static_cast<uint64_t>(flags) &
                                      ~static_cast<uint64_t>(PageFlags::NoExecute));

    uint64_t mapped = 0;
    for (; mapped < page_count; ++mapped) {
        PhysicalAddress frame;
        if (!welovec::memory::pmm::allocate_frame(&frame)) break;
        auto *memory = static_cast<uint8_t *>(
            welovec::x86_64::mm::physical::direct_map(frame));
        if (!memory) {
            welovec::memory::pmm::free_frame(frame);
            break;
        }
        zero_bytes(memory, kPageSize);
        if (!welovec::x86_64::mm::paging::map_page_in(
                root, base + mapped * kPageSize, frame, flags, true)) {
            welovec::memory::pmm::free_frame(frame);
            break;
        }
    }

    if (mapped == page_count) return true;
    while (mapped) {
        --mapped;
        welovec::x86_64::mm::paging::unmap_page_in(
            root, base + mapped * kPageSize, nullptr, true);
    }
    return false;
}

bool copy_to_process(PhysicalAddress root, uint64_t destination,
                     const void *source, uint64_t size, bool require_writable = false) {
    if ((!source && size) || !valid_user_range(destination, size)) return false;
    const auto *input = static_cast<const uint8_t *>(source);
    uint64_t copied = 0;
    while (copied < size) {
        Mapping mapping{};
        const uint64_t address = destination + copied;
        if (!welovec::x86_64::mm::paging::translate_in(root, address, &mapping) ||
            !(mapping.raw_flags & (UINT64_C(1) << 2)) ||
            (require_writable && !(mapping.raw_flags & (UINT64_C(1) << 1)))) return false;
        const uint64_t offset = address & (kPageSize - 1);
        uint64_t chunk = kPageSize - offset;
        if (chunk > size - copied) chunk = size - copied;
        auto *target = static_cast<uint8_t *>(
            welovec::x86_64::mm::physical::direct_map(mapping.physical));
        if (!target) return false;
        copy_bytes(target, input + copied, chunk);
        copied += chunk;
    }
    return true;
}

bool install_thread_return_stub(PhysicalAddress root) {
    // mov rdi, rax; mov eax, SYS_THREAD_EXIT; syscall; ud2
    const uint8_t stub[] = {
        0x48, 0x89, 0xc7,
        0xb8, static_cast<uint8_t>(SYS_THREAD_EXIT), 0x00, 0x00, 0x00,
        0x0f, 0x05, 0x0f, 0x0b
    };
    // ret from a user signal handler enters this stub to restore its saved context.
    const uint8_t signal_return_stub[] = {
        0xb8, static_cast<uint8_t>(SYS_SIGRETURN), 0x00, 0x00, 0x00,
        0x0f, 0x05, 0x0f, 0x0b
    };
    return allocate_pages(root, kThreadReturnAddress, 1, PageFlags::User) &&
           copy_to_process(root, kThreadReturnAddress, stub, sizeof(stub)) &&
           copy_to_process(root, kSignalReturnAddress, signal_return_stub,
                           sizeof(signal_return_stub));
}

bool copy_from_process(PhysicalAddress root, uint64_t source,
                       void *destination, uint64_t size) {
    if ((!destination && size) || !valid_user_range(source, size)) return false;
    auto *output = static_cast<uint8_t *>(destination);
    uint64_t copied = 0;
    while (copied < size) {
        Mapping mapping{};
        const uint64_t address = source + copied;
        if (!welovec::x86_64::mm::paging::translate_in(root, address, &mapping) ||
            !(mapping.raw_flags & (UINT64_C(1) << 2))) return false;
        const uint64_t offset = address & (kPageSize - 1);
        uint64_t chunk = kPageSize - offset;
        if (chunk > size - copied) chunk = size - copied;
        const auto *input = static_cast<const uint8_t *>(
            welovec::x86_64::mm::physical::direct_map(mapping.physical));
        if (!input) return false;
        copy_bytes(output + copied, input, chunk);
        copied += chunk;
    }
    return true;
}

bool build_program_stack(PhysicalAddress root, uint64_t stack_base, uint64_t stack_top,
                         uint32_t argument_count, const char *const *arguments,
                         const ProgramStartupInfo *startup, uint64_t *user_stack) {
    if (!user_stack || !startup || !arguments || !argument_count ||
        argument_count > kMaximumProgramArguments) return false;

    uint64_t argument_addresses[kMaximumProgramArguments]{};
    uint64_t stack_pointer = stack_top;
    uint64_t argument_bytes = 0;
    for (uint32_t i = argument_count; i > 0; --i) {
        const char *argument = arguments[i - 1];
        if (!argument) return false;
        uint64_t length = 0;
        while (length < kMaximumProgramArgumentBytes - argument_bytes && argument[length])
            ++length;
        if (length == kMaximumProgramArgumentBytes - argument_bytes) return false;
        ++length; // Include the terminating NUL.
        if (length > stack_pointer - stack_base) return false;
        stack_pointer -= length;
        if (!copy_to_process(root, stack_pointer, argument, length, true)) return false;
        argument_addresses[i - 1] = stack_pointer;
        argument_bytes += length;
    }

    // SysV AMD64 initial stack: argc, argv[], NULL, empty envp, then auxv.
    uint64_t words[1 + kMaximumProgramArguments + 1 + 1 + 12]{};
    uint32_t count = 0;
    words[count++] = argument_count;
    for (uint32_t i = 0; i < argument_count; ++i)
        words[count++] = argument_addresses[i];
    words[count++] = 0; // argv terminator
    words[count++] = 0; // envp terminator

    if (startup->program_headers && startup->program_header_entry_size &&
        startup->program_header_count) {
        words[count++] = 3; // AT_PHDR
        words[count++] = startup->program_headers;
        words[count++] = 4; // AT_PHENT
        words[count++] = startup->program_header_entry_size;
        words[count++] = 5; // AT_PHNUM
        words[count++] = startup->program_header_count;
    }
    words[count++] = 6; // AT_PAGESZ
    words[count++] = kPageSize;
    words[count++] = 9; // AT_ENTRY
    words[count++] = startup->entry;
    words[count++] = 0; // AT_NULL
    words[count++] = 0;

    const uint64_t table_size = static_cast<uint64_t>(count) * sizeof(uint64_t);
    if (table_size > stack_pointer - stack_base) return false;
    stack_pointer = (stack_pointer - table_size) & ~UINT64_C(0xf);
    if (stack_pointer < stack_base || table_size > stack_top - stack_pointer ||
        !copy_to_process(root, stack_pointer, words, table_size, true)) return false;
    *user_stack = stack_pointer;
    return true;
}

void thread_exited(welovec::x86_64::scheduler::TaskId, uint64_t process_id,
                   int64_t status, void *) {
    bool close_descriptors = false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (process) {
        process->ever_had_thread = true;
        process->last_thread_status = status;
        if (process->live_threads) --process->live_threads;
        if (process->state == State::Active && !process->live_threads &&
            !process->creating_threads) {
            process->state = State::Zombie;
            process->exit_status = status;
            close_descriptors = true;
        } else if (process->state == State::Terminating && !process->live_threads &&
                   !process->creating_threads) {
            process->state = State::Zombie;
            close_descriptors = true;
        }
    }
    gManager.lock.unlock_irqrestore(irq);
    if (close_descriptors) welovec::filesystem::close_process_descriptors(process_id);
}

void finish_creation_locked(Process *process) {
    if (process->state == State::Terminating && !process->live_threads &&
        !process->creating_threads)
        process->state = State::Zombie;
    else if (process->state == State::Active && process->ever_had_thread &&
             !process->live_threads && !process->creating_threads) {
        process->state = State::Zombie;
        process->exit_status = process->last_thread_status;
    }
}

void finish_operation(uint64_t process_id) {
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (process && process->active_operations) --process->active_operations;
    gManager.lock.unlock_irqrestore(irq);
}

} // namespace

bool initialize() {
    if (gManager.initialized) return true;
    if (!welovec::memory::heap::ready() ||
        !welovec::x86_64::mm::paging::ready() ||
        !welovec::x86_64::scheduler::ready()) return false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    gManager.initialized = true;
    gManager.lock.unlock_irqrestore(irq);
    return true;
}

uint64_t create(const char *name) {
    security::Credentials credentials{};
    if (!security::default_credentials(&credentials)) return 0;
    return create_with_credentials(name, credentials);
}

uint64_t create_with_credentials(const char *name,
                                 const security::Credentials &credentials) {
    if (!gManager.initialized) return 0;
    if (!security::credentials_are_valid(credentials)) return 0;
    auto *process = static_cast<Process *>(
        welovec::memory::heap::allocate_zeroed(sizeof(Process)));
    if (!process) return 0;
    if (!welovec::x86_64::mm::paging::create_address_space(&process->root)) {
        welovec::memory::heap::deallocate(process);
        return 0;
    }
    if (!install_thread_return_stub(process->root)) {
        welovec::x86_64::mm::paging::destroy_address_space(process->root);
        welovec::memory::heap::deallocate(process);
        return 0;
    }
    process->state = State::Active;
    process->stack_cursor = user_limit() - kPageSize;
    process->mmap_base = kDefaultMmapBase;
    process->credentials = credentials;
    process->parent_id = welovec::x86_64::scheduler::current_process_id();
    process->session_authenticated = current_session_authenticated();
    copy_name(process->name, name);

    const uint64_t irq = gManager.lock.lock_irqsave();
    if (gManager.next_id == 0 || gManager.next_id == UINT64_MAX) {
        gManager.lock.unlock_irqrestore(irq);
        welovec::x86_64::mm::paging::destroy_address_space(process->root);
        welovec::memory::heap::deallocate(process);
        return 0;
    }
    process->id = gManager.next_id++;
    Process *parent = find_locked(process->parent_id);
    if (process->parent_id && (!parent || parent->state != State::Active)) {
        gManager.lock.unlock_irqrestore(irq);
        welovec::x86_64::mm::paging::destroy_address_space(process->root);
        welovec::memory::heap::deallocate(process);
        return 0;
    }
    if (parent && parent->state == State::Active &&
        parent->framebuffer_surface && parent->window_input) {
        process->framebuffer_surface = parent->framebuffer_surface;
        process->window_input = parent->window_input;
        process->window_root_id = parent->window_root_id;
        ++process->framebuffer_surface->references;
        ++process->window_input->references;
    }
    process->next = gManager.head;
    gManager.head = process;
    gManager.lock.unlock_irqrestore(irq);
    return process->id;
}

bool allocate_user_pages(uint64_t process_id, uint64_t base, uint64_t page_count,
                         bool writable, bool executable) {
    if (!gManager.initialized || !page_count || (writable && executable) ||
        base < kMinimumUserAddress ||
        base >= user_limit() ||
        (base & (kPageSize - 1)) || page_count > (user_limit() - base) / kPageSize)
        return false;

    PhysicalAddress root;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state != State::Active) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    root = process->root;
    ++process->active_operations;
    gManager.lock.unlock_irqrestore(irq);

    PageFlags flags = PageFlags::User;
    if (writable) flags = flags | PageFlags::Writable;
    if (!executable) flags = flags | PageFlags::NoExecute;
    const bool result = allocate_pages(root, base, page_count, flags);
    finish_operation(process_id);
    return result;
}

bool protect_user_pages(uint64_t process_id, uint64_t base, uint64_t page_count,
                        bool writable, bool executable) {
    if (!gManager.initialized || !page_count || (writable && executable) ||
        base < kMinimumUserAddress || base >= user_limit() ||
        (base & (kPageSize - 1)) || page_count > (user_limit() - base) / kPageSize)
        return false;

    PhysicalAddress root;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state != State::Active) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    root = process->root;
    ++process->active_operations;
    gManager.lock.unlock_irqrestore(irq);

    bool result = true;
    for (uint64_t page = 0; page < page_count; ++page) {
        Mapping mapping{};
        if (!welovec::x86_64::mm::paging::translate_in(
                root, base + page * kPageSize, &mapping) || mapping.page_size != kPageSize ||
            !(mapping.raw_flags & (UINT64_C(1) << 2))) {
            result = false;
            break;
        }
    }

    PageFlags flags = PageFlags::User;
    if (writable) flags = flags | PageFlags::Writable;
    if (!executable && welovec::x86_64::mm::paging::nx_enabled())
        flags = flags | PageFlags::NoExecute;
    for (uint64_t page = 0; result && page < page_count; ++page)
        result = welovec::x86_64::mm::paging::protect_page_in(
            root, base + page * kPageSize, flags);

    finish_operation(process_id);
    return result;
}

bool write_user_memory(uint64_t process_id, uint64_t destination,
                       const void *source, uint64_t size) {
    if (!gManager.initialized) return false;
    PhysicalAddress root;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state != State::Active) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    root = process->root;
    ++process->active_operations;
    gManager.lock.unlock_irqrestore(irq);
    const bool result = copy_to_process(root, destination, source, size, true);
    finish_operation(process_id);
    return result;
}

static uint64_t create_user_thread_common(uint64_t process_id, const char *name,
                                         uint64_t entry, uint64_t argument,
                                         uint32_t argument_count,
                                         const char *const *arguments,
                                         const ProgramStartupInfo *startup,
                                         uint8_t priority, bool program_entry) {
    if (!gManager.initialized || priority > 3 || entry < kMinimumUserAddress ||
        entry == kThreadReturnAddress || entry >= user_limit() ||
        (program_entry && (!startup || !arguments || !argument_count ||
                           argument_count > kMaximumProgramArguments))) return 0;

    PhysicalAddress root;
    uint64_t stack_top = 0;
    Mapping code_mapping{};
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state != State::Active ||
        process->stack_cursor < kMinimumUserAddress + kUserStackSize + kPageSize) {
        gManager.lock.unlock_irqrestore(irq);
        return 0;
    }
    if (program_entry && startup && startup->image_end >= process->mmap_base) {
        if (startup->image_end > UINT64_MAX - (2 * kPageSize - 1)) {
            gManager.lock.unlock_irqrestore(irq);
            return 0;
        }
        process->mmap_base = (startup->image_end + kPageSize - 1) & ~(kPageSize - 1);
        process->mmap_base += kPageSize;
    }
    root = process->root;
    if (!welovec::x86_64::mm::paging::translate_in(root, entry, &code_mapping) ||
        !(code_mapping.raw_flags & (UINT64_C(1) << 2)) ||
        (welovec::x86_64::mm::paging::nx_enabled() &&
         (code_mapping.raw_flags & (UINT64_C(1) << 63)))) {
        gManager.lock.unlock_irqrestore(irq);
        return 0;
    }
    stack_top = process->stack_cursor & ~UINT64_C(0xf);
    process->stack_cursor = stack_top - kUserStackSize - kPageSize;
    ++process->creating_threads;
    ++process->live_threads;
    gManager.lock.unlock_irqrestore(irq);

    bool ok = true;
    const uint64_t stack_base = stack_top - kUserStackSize;
    ok = allocate_pages(root, stack_base, kUserStackPages,
                        PageFlags::User | PageFlags::Writable | PageFlags::NoExecute);
    uint64_t initial_stack = stack_top - sizeof(uint64_t);
    if (ok && program_entry) {
        ok = build_program_stack(root, stack_base, stack_top, argument_count,
                                 arguments, startup, &initial_stack);
    } else if (ok) {
        const uint64_t return_address = kThreadReturnAddress;
        ok = copy_to_process(root, initial_stack, &return_address,
                             sizeof(return_address), true);
    }

    uint64_t thread_id = 0;
    if (ok) {
        thread_id = welovec::x86_64::scheduler::create_user_thread(
            name, entry, initial_stack, program_entry ? 0 : argument,
            process_id, root, thread_exited, nullptr,
            static_cast<welovec::x86_64::scheduler::Priority>(priority));
        ok = thread_id != 0;
    }
    if (!ok && stack_top) {
        for (uint64_t page = 0; page < kUserStackPages; ++page)
            welovec::x86_64::mm::paging::unmap_page_in(
                root, stack_base + page * kPageSize, nullptr, true);
    }

    bool terminate_group = false;
    int64_t group_status = 0;
    const uint64_t finish_irq = gManager.lock.lock_irqsave();
    process = find_locked(process_id);
    if (process) {
        if (process->creating_threads) --process->creating_threads;
        if (ok) process->ever_had_thread = true;
        if (!ok && process->live_threads) --process->live_threads;
        terminate_group = process->state == State::Terminating &&
                          process->creating_threads == 0;
        group_status = process->exit_status;
        finish_creation_locked(process);
    }
    gManager.lock.unlock_irqrestore(finish_irq);
    if (terminate_group)
        welovec::x86_64::scheduler::terminate_group(process_id, group_status);
    return thread_id;
}

uint64_t create_user_thread(uint64_t process_id, const char *name, uint64_t entry,
                            uint64_t argument, uint8_t priority) {
    return create_user_thread_common(process_id, name, entry, argument, 0,
                                     nullptr, nullptr, priority, false);
}

uint64_t create_user_program_thread(uint64_t process_id, const char *name,
                                    uint64_t entry, uint32_t argument_count,
                                    const char *const *arguments,
                                    const ProgramStartupInfo *startup,
                                    uint8_t priority) {
    return create_user_thread_common(process_id, name, entry, 0, argument_count,
                                     arguments, startup, priority, true);
}

bool terminate(uint64_t process_id, int64_t status) {
    if (!gManager.initialized || !process_id) return false;
    bool terminate_now = false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state != State::Active) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    if (process->state == State::Active) {
        process->state = State::Terminating;
        process->exit_status = status;
    }
    terminate_now = process->creating_threads == 0;
    if (!process->live_threads && !process->creating_threads)
        process->state = State::Zombie;
    const bool close_descriptors = process->state == State::Zombie;
    gManager.lock.unlock_irqrestore(irq);
    if (close_descriptors) welovec::filesystem::close_process_descriptors(process_id);
    if (terminate_now) welovec::x86_64::scheduler::terminate_group(process_id, status);
    return true;
}

bool terminate_windowed_tree(uint64_t parent_id, uint64_t process_id,
                             int64_t status) {
    if (!gManager.initialized || !parent_id || !process_id) return false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *root = find_locked(process_id);
    if (!root || root->parent_id != parent_id ||
        root->window_root_id != process_id || !root->framebuffer_surface ||
        !root->window_input) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    root->window_tree_terminating = true;
    for (Process *process = gManager.head; process; process = process->next) {
        if (process->window_root_id != process_id ||
            process->state == State::Zombie) continue;
        process->state = State::Terminating;
        process->exit_status = status;
        if (!process->live_threads && !process->creating_threads) {
            process->state = State::Zombie;
            process->close_descriptors_pending = true;
        }
    }
    gManager.lock.unlock_irqrestore(irq);

    for (;;) {
        uint64_t terminate_id = 0;
        uint64_t close_id = 0;
        const uint64_t scan_irq = gManager.lock.lock_irqsave();
        for (Process *process = gManager.head; process; process = process->next) {
            if (process->window_root_id != process_id) continue;
            if (process->close_descriptors_pending) {
                process->close_descriptors_pending = false;
                close_id = process->id;
                break;
            }
            if (process->state == State::Terminating &&
                !process->creating_threads && !process->termination_requested) {
                process->termination_requested = true;
                terminate_id = process->id;
                break;
            }
        }
        gManager.lock.unlock_irqrestore(scan_irq);
        if (close_id) {
            welovec::filesystem::close_process_descriptors(close_id);
            continue;
        }
        if (terminate_id) {
            welovec::x86_64::scheduler::terminate_group(terminate_id, status);
            continue;
        }
        break;
    }
    return true;
}

bool windowed_tree_terminating(uint64_t process_id) {
    if (!gManager.initialized || !process_id) return false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    const bool result = process && process->window_tree_terminating;
    gManager.lock.unlock_irqrestore(irq);
    return result;
}

bool reap_windowed_descendants(uint64_t process_id) {
    if (!gManager.initialized || !process_id) return false;
    for (;;) {
        uint64_t reap_id = 0;
        bool has_descendants = false;
        const uint64_t irq = gManager.lock.lock_irqsave();
        Process *root = find_locked(process_id);
        if (!root || !root->window_tree_terminating ||
            root->state != State::Zombie) {
            gManager.lock.unlock_irqrestore(irq);
            return false;
        }
        for (Process *process = gManager.head; process; process = process->next) {
            if (process->id == process_id ||
                process->window_root_id != process_id) continue;
            has_descendants = true;
            if (process->state == State::Zombie && !process->live_threads &&
                !process->creating_threads && !process->active_operations) {
                reap_id = process->id;
                break;
            }
        }
        gManager.lock.unlock_irqrestore(irq);
        if (!has_descendants) return true;
        if (!reap_id) return false;
        if (!destroy(reap_id)) return false;
    }
}

bool query(uint64_t process_id, Information *out) {
    if (!gManager.initialized || !out) return false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    out->id = process->id;
    out->parent_id = process->parent_id;
    out->live_threads = process->live_threads;
    out->exit_status = process->exit_status;
    out->state = process->state;
    out->credentials = process->credentials;
    copy_bytes(out->name, process->name, sizeof(out->name));
    gManager.lock.unlock_irqrestore(irq);
    return true;
}

bool destroy(uint64_t process_id) {
    if (!gManager.initialized || !process_id) return false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process **link = &gManager.head;
    while (*link && (*link)->id != process_id) link = &(*link)->next;
    Process *process = *link;
    if (!process || process->live_threads || process->creating_threads ||
        process->active_operations ||
        (process->state != State::Zombie && process->state != State::Active) ||
        process->root == welovec::x86_64::mm::paging::active_root()) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    *link = process->next;
    gManager.lock.unlock_irqrestore(irq);

    welovec::filesystem::close_process_descriptors(process_id);

    if (!welovec::x86_64::mm::paging::destroy_address_space(process->root)) {
        const uint64_t restore_irq = gManager.lock.lock_irqsave();
        process->next = gManager.head;
        gManager.head = process;
        gManager.lock.unlock_irqrestore(restore_irq);
        return false;
    }
    bool destroy_surface = false;
    bool destroy_window_input = false;
    const uint64_t resource_irq = gManager.lock.lock_irqsave();
    if (process->framebuffer_surface && process->framebuffer_surface->references) {
        destroy_surface = --process->framebuffer_surface->references == 0;
    }
    if (process->window_input && process->window_input->references) {
        destroy_window_input = --process->window_input->references == 0;
    }
    gManager.lock.unlock_irqrestore(resource_irq);
    if (destroy_surface) {
        framebuffer_surface_destroy(process->framebuffer_surface);
        welovec::memory::heap::deallocate(process->framebuffer_surface);
    }
    if (destroy_window_input)
        welovec::memory::heap::deallocate(process->window_input);
    welovec::memory::heap::deallocate(process);
    return true;
}

uint64_t maximum_user_mmap_size() {
    const auto stats = welovec::memory::pmm::statistics();
    if (!stats.maximum_physical_address) return kFallbackMmapSize;
    if (stats.boot_available_frames > UINT64_MAX / kPageSize) return UINT64_MAX;
    return stats.boot_available_frames * kPageSize;
}

uint64_t map_current_user_memory(uint64_t size) {
    if (!gManager.initialized || !size || size > maximum_user_mmap_size() ||
        size > UINT64_MAX - (kPageSize - 1)) return 0;
    const uint64_t rounded_size = (size + kPageSize - 1) & ~(kPageSize - 1);
    const uint64_t page_count = rounded_size / kPageSize;
    const uint64_t process_id = current_id();
    if (!process_id) return 0;

    PhysicalAddress root;
    uint64_t base = 0;
    uint32_t slot = kMaximumMappedRegions;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state != State::Active) {
        gManager.lock.unlock_irqrestore(irq);
        return 0;
    }
    for (uint32_t i = 0; i < kMaximumMappedRegions; ++i) {
        if (!process->mapped_regions[i].used) { slot = i; break; }
    }
    if (slot == kMaximumMappedRegions) {
        gManager.lock.unlock_irqrestore(irq);
        return 0;
    }
    root = process->root;
    uint64_t candidate = process->mmap_base;
    while (candidate < process->stack_cursor &&
           rounded_size <= process->stack_cursor - candidate) {
        bool overlap = false;
        for (const MappedRegion &region : process->mapped_regions) {
            if (!region.used) continue;
            const uint64_t region_size =
                (region.size + kPageSize - 1) & ~(kPageSize - 1);
            const uint64_t region_end = region.base + region_size;
            if (candidate < region_end && region.base < candidate + rounded_size) {
                candidate = region_end;
                overlap = true;
                break;
            }
        }
        if (overlap) continue;

        for (uint64_t page = 0; page < page_count; ++page) {
            Mapping mapping{};
            const uint64_t address = candidate + page * kPageSize;
            if (welovec::x86_64::mm::paging::translate_in(root, address, &mapping)) {
                candidate = address + kPageSize;
                overlap = true;
                break;
            }
        }
        if (overlap) continue;
        base = candidate;
        break;
    }
    if (!base) {
        gManager.lock.unlock_irqrestore(irq);
        return 0;
    }
    process->mapped_regions[slot] = {base, size, true};
    ++process->active_operations;
    gManager.lock.unlock_irqrestore(irq);

    const bool mapped = allocate_pages(root, base, page_count,
        PageFlags::User | PageFlags::Writable | PageFlags::NoExecute);
    if (!mapped) {
        const uint64_t rollback_irq = gManager.lock.lock_irqsave();
        process = find_locked(process_id);
        if (process) {
            process->mapped_regions[slot] = {};
            if (process->active_operations) --process->active_operations;
        }
        gManager.lock.unlock_irqrestore(rollback_irq);
        return 0;
    }
    finish_operation(process_id);
    return base;
}

bool unmap_current_user_memory(uint64_t address, uint64_t size) {
    if (!gManager.initialized || !address || !size ||
        size > UINT64_MAX - (kPageSize - 1)) return false;
    const uint64_t process_id = current_id();
    if (!process_id) return false;

    PhysicalAddress root;
    uint64_t page_count = 0;
    uint32_t slot = kMaximumMappedRegions;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state != State::Active) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    for (uint32_t i = 0; i < kMaximumMappedRegions; ++i) {
        if (process->mapped_regions[i].used && process->mapped_regions[i].base == address &&
            process->mapped_regions[i].size == size) { slot = i; break; }
    }
    if (slot == kMaximumMappedRegions) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    root = process->root;
    page_count = ((size + kPageSize - 1) & ~(kPageSize - 1)) / kPageSize;
    ++process->active_operations;
    gManager.lock.unlock_irqrestore(irq);

    bool unmapped = true;
    for (uint64_t page = 0; page < page_count; ++page) {
        if (!welovec::x86_64::mm::paging::unmap_page_in(
                root, address + page * kPageSize, nullptr, true)) {
            unmapped = false;
            break;
        }
    }
    const uint64_t finish_irq = gManager.lock.lock_irqsave();
    process = find_locked(process_id);
    if (process) {
        if (unmapped) process->mapped_regions[slot] = {};
        if (process->active_operations) --process->active_operations;
    }
    gManager.lock.unlock_irqrestore(finish_irq);
    return unmapped;
}

bool current_user_address_executable(uint64_t address) {
    if (!gManager.initialized || address < kMinimumUserAddress ||
        address >= user_limit() ||
        (address >= kThreadReturnAddress &&
         address < kThreadReturnAddress + kPageSize)) return false;
    const uint64_t process_id = current_id();
    if (!process_id) return false;
    PhysicalAddress root;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state != State::Active) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    root = process->root;
    gManager.lock.unlock_irqrestore(irq);

    Mapping mapping{};
    return welovec::x86_64::mm::paging::translate_in(root, address, &mapping) &&
        (mapping.raw_flags & (UINT64_C(1) << 2)) &&
        (!welovec::x86_64::mm::paging::nx_enabled() ||
         !(mapping.raw_flags & (UINT64_C(1) << 63)));
}

bool set_current_signal_handler(uint32_t signal, uint64_t handler,
                                uint64_t *old_handler) {
    if (!gManager.initialized ||
        (signal != kSignalInterrupt && signal != kSignalTerminate) ||
        (handler > kSignalIgnore && !current_user_address_executable(handler)))
        return false;
    const uint64_t process_id = current_id();
    if (!process_id) return false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state != State::Active) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    if (old_handler) *old_handler = process->signal_handlers[signal];
    process->signal_handlers[signal] = handler;
    gManager.lock.unlock_irqrestore(irq);
    return true;
}

SignalDisposition send_signal(uint64_t process_id, uint32_t signal) {
    if (!gManager.initialized || !process_id || signal >= kSignalLimit || !signal ||
        signal == kSignalKill ||
        (signal != kSignalInterrupt && signal != kSignalTerminate))
        return SignalDisposition::Invalid;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state != State::Active) {
        gManager.lock.unlock_irqrestore(irq);
        return SignalDisposition::Invalid;
    }
    const uint64_t handler = process->signal_handlers[signal];
    if (handler == kSignalIgnore) {
        gManager.lock.unlock_irqrestore(irq);
        return SignalDisposition::Ignored;
    }
    if (!handler) {
        gManager.lock.unlock_irqrestore(irq);
        return SignalDisposition::Default;
    }
    process->pending_signals |= UINT64_C(1) << signal;
    gManager.lock.unlock_irqrestore(irq);
    return SignalDisposition::Queued;
}

int prepare_pending_signal(syscall_regs_t *regs, uint64_t return_trampoline,
                           uint32_t *out_signal) {
    if (!gManager.initialized || !regs || !return_trampoline) return -1;
    const uint64_t process_id = current_id();
    const uint64_t task_id = welovec::x86_64::scheduler::current_id();
    if (!process_id || !task_id) return 0;

    PhysicalAddress root;
    uint64_t handler = 0;
    uint32_t signal = 0;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state != State::Active || process->signal_active) {
        gManager.lock.unlock_irqrestore(irq);
        return 0;
    }
    for (uint32_t candidate = 1; candidate < kSignalLimit; ++candidate) {
        if (!(process->pending_signals & (UINT64_C(1) << candidate))) continue;
        process->pending_signals &= ~(UINT64_C(1) << candidate);
        handler = process->signal_handlers[candidate];
        if (handler == kSignalIgnore) continue;
        signal = candidate;
        break;
    }
    if (!signal) {
        gManager.lock.unlock_irqrestore(irq);
        return 0;
    }
    if (!handler) {
        if (out_signal) *out_signal = signal;
        gManager.lock.unlock_irqrestore(irq);
        return -1; // The default action terminates this process.
    }
    process->signal_active = true;
    process->signal_task_id = task_id;
    process->saved_signal_context = *regs;
    root = process->root;
    gManager.lock.unlock_irqrestore(irq);

    if (out_signal) *out_signal = signal;
    if (regs->user_rsp < kMinimumUserAddress + 32) return -1;
    const uint64_t user_stack = ((regs->user_rsp - 16) & ~UINT64_C(0xf)) + 8;
    if (user_stack >= regs->user_rsp ||
        !copy_to_process(root, user_stack, &return_trampoline,
                         sizeof(return_trampoline), true)) return -1;
    regs->user_rsp = user_stack;
    regs->rcx = handler;
    regs->rdi = signal;
    regs->rax = 0;
    return 1;
}

bool restore_signal_context(syscall_regs_t *regs) {
    if (!gManager.initialized || !regs) return false;
    const uint64_t process_id = current_id();
    const uint64_t task_id = welovec::x86_64::scheduler::current_id();
    if (!process_id || !task_id) return false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || !process->signal_active || process->signal_task_id != task_id) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    *regs = process->saved_signal_context;
    process->signal_active = false;
    process->signal_task_id = 0;
    process->saved_signal_context = {};
    gManager.lock.unlock_irqrestore(irq);
    return true;
}

bool is_parent(uint64_t parent_id, uint64_t process_id) {
    if (!parent_id || !process_id || !gManager.initialized) return false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    const bool result = process && process->parent_id == parent_id;
    gManager.lock.unlock_irqrestore(irq);
    return result;
}

bool initialize_windowed_process(uint64_t process_id, uint32_t width,
                                uint32_t height) {
    if (!gManager.initialized || !process_id) return false;
    const uint64_t parent_id = current_id();
    WelovecFramebufferSurface *surface = nullptr;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->parent_id != parent_id ||
        process->state != State::Active || process->framebuffer_surface ||
        process->window_input) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    ++process->active_operations;
    gManager.lock.unlock_irqrestore(irq);

    surface = static_cast<WelovecFramebufferSurface *>(
        welovec::memory::heap::allocate_zeroed(sizeof(WelovecFramebufferSurface)));
    const bool surface_ready = surface &&
        framebuffer_surface_create(surface, width, height);
    auto *input = surface_ready ? static_cast<WindowInput *>(
        welovec::memory::heap::allocate_zeroed(sizeof(WindowInput))) : nullptr;
    const bool ready = surface_ready && input;
    if (ready) {
        surface->references = 1;
        input->references = 1;
    }
    const uint64_t finish_irq = gManager.lock.lock_irqsave();
    process = find_locked(process_id);
    if (process && process->active_operations) --process->active_operations;
    const bool attached = ready && process && process->state == State::Active &&
        process->parent_id == parent_id && !process->window_input &&
        !process->framebuffer_surface;
    if (attached) {
        process->window_input = input;
        process->framebuffer_surface = surface;
        process->owns_window_surface = true;
        process->window_root_id = process_id;
    }
    gManager.lock.unlock_irqrestore(finish_irq);
    if (!attached) {
        if (input) welovec::memory::heap::deallocate(input);
        if (surface_ready) {
            framebuffer_surface_destroy(surface);
            welovec::memory::heap::deallocate(surface);
        } else if (surface) {
            welovec::memory::heap::deallocate(surface);
        }
    }
    return attached;
}

bool current_framebuffer_surface(WelovecFramebufferSurface **out_surface) {
    if (!out_surface || !gManager.initialized) return false;
    *out_surface = nullptr;
    const uint64_t process_id = current_id();
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    const bool found = process && process->window_input &&
        process->framebuffer_surface && process->framebuffer_surface->pixels;
    if (found) *out_surface = process->framebuffer_surface;
    gManager.lock.unlock_irqrestore(irq);
    return found;
}

bool child_framebuffer_surface(uint64_t parent_id, uint64_t process_id,
                               WelovecFramebufferSurface **out_surface) {
    if (!out_surface || !parent_id || !process_id || !gManager.initialized)
        return false;
    *out_surface = nullptr;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    const bool found = process && process->parent_id == parent_id &&
        process->window_input && process->framebuffer_surface &&
        process->framebuffer_surface->pixels;
    if (found && process->active_operations != UINT64_MAX) {
        ++process->active_operations;
        *out_surface = process->framebuffer_surface;
    }
    gManager.lock.unlock_irqrestore(irq);
    return found && *out_surface;
}

void release_child_framebuffer_surface(uint64_t process_id) {
    if (!process_id || !gManager.initialized) return;
    finish_operation(process_id);
}

bool resize_child_framebuffer_surface(uint64_t parent_id, uint64_t process_id,
                                      uint32_t width, uint32_t height) {
    if (!parent_id || !process_id || !gManager.initialized) return false;
    WelovecFramebufferSurface *surface = nullptr;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->parent_id != parent_id ||
        process->state == State::Terminating || !process->window_input ||
        !process->framebuffer_surface || !process->framebuffer_surface->pixels) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    ++process->active_operations;
    surface = process->framebuffer_surface;
    gManager.lock.unlock_irqrestore(irq);

    const bool resized = framebuffer_surface_resize(surface, width, height);
    finish_operation(process_id);
    return resized;
}

bool forward_window_input(uint64_t parent_id, uint64_t process_id,
                          const char *characters, uint32_t character_count,
                          const welovec_pointer_event *events,
                          uint32_t event_count) {
    if (!parent_id || !process_id || character_count > kWindowCharacterQueueSize - 1 ||
        event_count > kWindowPointerQueueSize - 1 ||
        (character_count && !characters) || (event_count && !events) ||
        !gManager.initialized) return false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->parent_id != parent_id ||
        process->state != State::Active || !process->window_input) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    WindowInput *input = process->window_input;
    for (uint32_t i = 0; i < character_count; ++i) {
        const uint32_t next = (input->character_write + 1) % kWindowCharacterQueueSize;
        if (next == input->character_read)
            input->character_read = (input->character_read + 1) % kWindowCharacterQueueSize;
        input->characters[input->character_write] = characters[i];
        input->character_write = next;
    }
    for (uint32_t i = 0; i < event_count; ++i) {
        const uint32_t next = (input->pointer_write + 1) % kWindowPointerQueueSize;
        if (next == input->pointer_read)
            input->pointer_read = (input->pointer_read + 1) % kWindowPointerQueueSize;
        input->pointer_events[input->pointer_write] = events[i];
        input->pointer_write = next;
    }
    gManager.lock.unlock_irqrestore(irq);
    return true;
}

int64_t read_current_window_input(char *buffer, uint64_t capacity,
                                  bool wait_for_input) {
    if (!buffer || !capacity) return -1;
    if (capacity > kWindowCharacterQueueSize) capacity = kWindowCharacterQueueSize;
    const uint64_t process_id = current_id();
    for (;;) {
        uint64_t count = 0;
        const uint64_t irq = gManager.lock.lock_irqsave();
        Process *process = find_locked(process_id);
        WindowInput *input = process ? process->window_input : nullptr;
        if (!input || !process || process->state == State::Zombie) {
            gManager.lock.unlock_irqrestore(irq);
            return -1;
        }
        while (count < capacity && input->character_read != input->character_write) {
            buffer[count++] = input->characters[input->character_read];
            input->character_read = (input->character_read + 1) % kWindowCharacterQueueSize;
        }
        gManager.lock.unlock_irqrestore(irq);
        if (count || !wait_for_input) return static_cast<int64_t>(count);
        if (!welovec::x86_64::scheduler::sleep(1)) return -1;
    }
}

int64_t poll_current_window_pointer(welovec_pointer_event *events,
                                    uint64_t capacity) {
    if (!events || !capacity || capacity > kWindowPointerQueueSize) return -1;
    const uint64_t process_id = current_id();
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    WindowInput *input = process ? process->window_input : nullptr;
    if (!input || !process || process->state == State::Zombie) {
        gManager.lock.unlock_irqrestore(irq);
        return -1;
    }
    uint64_t count = 0;
    while (count < capacity && input->pointer_read != input->pointer_write) {
        events[count++] = input->pointer_events[input->pointer_read];
        input->pointer_read = (input->pointer_read + 1) % kWindowPointerQueueSize;
    }
    gManager.lock.unlock_irqrestore(irq);
    return static_cast<int64_t>(count);
}

bool current_has_window_input() {
    if (!gManager.initialized) return false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(current_id());
    const bool found = process && process->window_input;
    gManager.lock.unlock_irqrestore(irq);
    return found;
}

uint64_t current_id() { return welovec::x86_64::scheduler::current_process_id(); }

bool get_credentials(uint64_t process_id, security::Credentials *out) {
    if (!gManager.initialized || !process_id || !out) return false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state != State::Active) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    *out = process->credentials;
    gManager.lock.unlock_irqrestore(irq);
    return true;
}

bool current_credentials(security::Credentials *out) {
    if (!out) return false;
    const uint64_t process_id = current_id();
    if (!process_id) return security::root_credentials(out);
    return get_credentials(process_id, out);
}

bool current_session_authenticated() {
    const uint64_t process_id = current_id();
    if (!process_id || !gManager.initialized) return false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    const bool authenticated = process && process->state == State::Active &&
                              process->session_authenticated;
    gManager.lock.unlock_irqrestore(irq);
    return authenticated;
}

bool set_current_credentials(const security::Credentials &credentials,
                             bool authenticated) {
    if (!gManager.initialized || !security::credentials_are_valid(credentials))
        return false;
    const uint64_t process_id = current_id();
    if (!process_id) return false;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state != State::Active) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    process->credentials = credentials;
    process->session_authenticated = authenticated;
    gManager.lock.unlock_irqrestore(irq);
    return true;
}

bool copy_from_current_user(uint64_t source, void *destination, uint64_t size) {
    const uint64_t process_id = current_id();
    if (!process_id || !gManager.initialized) return false;
    PhysicalAddress root;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state == State::Zombie) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    root = process->root;
    ++process->active_operations;
    gManager.lock.unlock_irqrestore(irq);
    const bool result = copy_from_process(root, source, destination, size);
    finish_operation(process_id);
    return result;
}

bool copy_to_current_user(uint64_t destination, const void *source, uint64_t size) {
    const uint64_t process_id = current_id();
    if (!process_id || !gManager.initialized) return false;
    PhysicalAddress root;
    const uint64_t irq = gManager.lock.lock_irqsave();
    Process *process = find_locked(process_id);
    if (!process || process->state == State::Zombie) {
        gManager.lock.unlock_irqrestore(irq);
        return false;
    }
    root = process->root;
    ++process->active_operations;
    gManager.lock.unlock_irqrestore(irq);
    const bool result = copy_to_process(root, destination, source, size, true);
    finish_operation(process_id);
    return result;
}

[[noreturn]] void exit_current(int64_t status) {
    const uint64_t process_id = current_id();
    if (process_id) terminate(process_id, status);
    welovec::x86_64::scheduler::exit_current(status);
}

} // namespace welovec::process

extern "C" int welovec_process_manager_initialize(void) {
    return welovec::process::initialize() ? 1 : 0;
}

extern "C" uint64_t welovec_process_create(const char *name) {
    return welovec::process::create(name);
}

extern "C" int welovec_process_allocate_user_pages(uint64_t process_id, uint64_t base,
                                                     uint64_t page_count, int writable,
                                                     int executable) {
    return welovec::process::allocate_user_pages(process_id, base, page_count,
                                                  writable != 0, executable != 0) ? 1 : 0;
}

extern "C" int welovec_process_protect_user_pages(uint64_t process_id, uint64_t base,
                                                    uint64_t page_count, int writable,
                                                    int executable) {
    return welovec::process::protect_user_pages(process_id, base, page_count,
                                                 writable != 0, executable != 0) ? 1 : 0;
}

extern "C" int welovec_process_write_user_memory(uint64_t process_id, uint64_t destination,
                                                   const void *source, uint64_t size) {
    return welovec::process::write_user_memory(process_id, destination, source, size) ? 1 : 0;
}

extern "C" uint64_t welovec_process_create_user_thread(uint64_t process_id, const char *name,
                                                         uint64_t entry, uint64_t argument,
                                                         uint8_t priority) {
    return welovec::process::create_user_thread(process_id, name, entry, argument, priority);
}

extern "C" int welovec_process_terminate(uint64_t process_id, int64_t status) {
    return welovec::process::terminate(process_id, status) ? 1 : 0;
}

extern "C" int welovec_process_query(uint64_t process_id, struct welovec_process_info *out) {
    if (!out) return 0;
    welovec::process::Information info{};
    if (!welovec::process::query(process_id, &info)) return 0;
    out->id = info.id;
    out->parent_id = info.parent_id;
    out->live_threads = info.live_threads;
    out->exit_status = info.exit_status;
    out->state = static_cast<uint32_t>(info.state);
    out->uid = info.credentials.uid;
    out->gid = info.credentials.gid;
    out->capabilities = info.credentials.capabilities;
    for (uint32_t i = 0; i < sizeof(out->name); ++i) out->name[i] = info.name[i];
    return 1;
}

extern "C" int welovec_process_destroy(uint64_t process_id) {
    return welovec::process::destroy(process_id) ? 1 : 0;
}

extern "C" uint64_t welovec_process_current_id(void) {
    return welovec::process::current_id();
}

extern "C" int welovec_process_get_credentials(
    uint64_t process_id, struct welovec_security_identity *out) {
    if (!out) return 0;
    welovec::security::Credentials credentials{};
    if (!welovec::process::get_credentials(process_id, &credentials)) return 0;
    out->uid = credentials.uid;
    out->gid = credentials.gid;
    out->capabilities = credentials.capabilities;
    return 1;
}
