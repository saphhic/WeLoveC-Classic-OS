// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for manager in the process module.

#ifndef WELOVEC_PROCESS_MANAGER_C_API_H
#define WELOVEC_PROCESS_MANAGER_C_API_H

#include <stdint.h>
#include "security/privileges.h"

#ifdef __cplusplus
extern "C" {
#endif

enum welovec_process_state {
    WELOVEC_PROCESS_ACTIVE = 0,
    WELOVEC_PROCESS_TERMINATING = 1,
    WELOVEC_PROCESS_ZOMBIE = 2
};

struct welovec_process_info {
    uint64_t id;
    uint64_t parent_id;
    uint64_t live_threads;
    int64_t exit_status;
    uint32_t state;
    uint32_t uid;
    uint32_t gid;
    uint64_t capabilities;
    char name[32];
};

int welovec_process_manager_initialize(void);
uint64_t welovec_process_create(const char *name);
int welovec_process_allocate_user_pages(uint64_t process_id, uint64_t base,
                                        uint64_t page_count, int writable, int executable);
int welovec_process_protect_user_pages(uint64_t process_id, uint64_t base,
                                       uint64_t page_count, int writable, int executable);
int welovec_process_write_user_memory(uint64_t process_id, uint64_t destination,
                                      const void *source, uint64_t size);
uint64_t welovec_process_create_user_thread(uint64_t process_id, const char *name,
                                            uint64_t entry, uint64_t argument,
                                            uint8_t priority);
int welovec_process_terminate(uint64_t process_id, int64_t status);
int welovec_process_query(uint64_t process_id, struct welovec_process_info *out);
int welovec_process_destroy(uint64_t process_id);
uint64_t welovec_process_current_id(void);
int welovec_process_get_credentials(uint64_t process_id,
                                    struct welovec_security_identity *out);

#ifdef __cplusplus
}
#endif

#endif
