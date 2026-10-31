// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the process manager structures and operations.

#ifndef WELOVEC_PROCESS_MANAGER_HPP
#define WELOVEC_PROCESS_MANAGER_HPP

#include <stdint.h>
#include "IDM/DisplayService/framebuffer.hpp"
#include "user/input_event.h"
#include "security/privileges.hpp"

struct syscall_regs_t;

namespace welovec::process {

enum class State : uint32_t {
    Active = 0,
    Terminating = 1,
    Zombie = 2
};

enum class SignalDisposition : uint8_t {
    Queued,
    Ignored,
    Default,
    Invalid
};

struct Information {
    uint64_t id;
    uint64_t parent_id;
    uint64_t live_threads;
    int64_t exit_status;
    State state;
    security::Credentials credentials;
    char name[32];
};

struct ProgramStartupInfo {
    uint64_t program_headers;
    uint64_t program_header_entry_size;
    uint64_t program_header_count;
    uint64_t entry;
    uint64_t image_end;
};

bool initialize();
uint64_t create(const char *name);
uint64_t create_with_credentials(const char *name, const security::Credentials &credentials);
bool allocate_user_pages(uint64_t process_id, uint64_t base, uint64_t page_count,
                         bool writable, bool executable);
bool protect_user_pages(uint64_t process_id, uint64_t base, uint64_t page_count,
                        bool writable, bool executable);
bool write_user_memory(uint64_t process_id, uint64_t destination,
                       const void *source, uint64_t size);
uint64_t create_user_thread(uint64_t process_id, const char *name, uint64_t entry,
                            uint64_t argument, uint8_t priority);
uint64_t create_user_program_thread(uint64_t process_id, const char *name, uint64_t entry,
                                    uint32_t argument_count, const char *const *arguments,
                                    const ProgramStartupInfo *startup, uint8_t priority);
bool terminate(uint64_t process_id, int64_t status);
bool query(uint64_t process_id, Information *out);
bool destroy(uint64_t process_id);
uint64_t map_current_user_memory(uint64_t size);
uint64_t maximum_user_mmap_size();
bool unmap_current_user_memory(uint64_t address, uint64_t size);
bool is_parent(uint64_t parent_id, uint64_t process_id);
bool initialize_windowed_process(uint64_t process_id, uint32_t width,
                                 uint32_t height);
bool current_framebuffer_surface(WelovecFramebufferSurface **out_surface);
bool child_framebuffer_surface(uint64_t parent_id, uint64_t process_id,
                               WelovecFramebufferSurface **out_surface);
void release_child_framebuffer_surface(uint64_t process_id);
bool resize_child_framebuffer_surface(uint64_t parent_id, uint64_t process_id,
                                      uint32_t width, uint32_t height);
bool terminate_windowed_tree(uint64_t parent_id, uint64_t process_id,
                            int64_t status);
bool windowed_tree_terminating(uint64_t process_id);
bool reap_windowed_descendants(uint64_t process_id);
bool forward_window_input(uint64_t parent_id, uint64_t process_id,
                          const char *characters, uint32_t character_count,
                          const welovec_pointer_event *events,
                          uint32_t event_count);
int64_t read_current_window_input(char *buffer, uint64_t capacity,
                                  bool wait_for_input);
int64_t poll_current_window_pointer(welovec_pointer_event *events,
                                    uint64_t capacity);
bool current_has_window_input();
bool current_user_address_executable(uint64_t address);
bool set_current_signal_handler(uint32_t signal, uint64_t handler, uint64_t *old_handler);
SignalDisposition send_signal(uint64_t process_id, uint32_t signal);
int prepare_pending_signal(syscall_regs_t *regs, uint64_t return_trampoline,
                           uint32_t *out_signal);
bool restore_signal_context(syscall_regs_t *regs);
uint64_t current_id();
bool get_credentials(uint64_t process_id, security::Credentials *out);
bool current_credentials(security::Credentials *out);
bool current_session_authenticated();
bool set_current_credentials(const security::Credentials &credentials,
                            bool authenticated);
bool copy_from_current_user(uint64_t source, void *destination, uint64_t size);
bool copy_to_current_user(uint64_t destination, const void *source, uint64_t size);
[[noreturn]] void exit_current(int64_t status);

}

#endif
