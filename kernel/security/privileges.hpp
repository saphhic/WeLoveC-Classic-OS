// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for privileges in the security module.

#ifndef WELOVEC_SECURITY_PRIVILEGES_HPP
#define WELOVEC_SECURITY_PRIVILEGES_HPP

#include "privileges.h"
#include "user/security_types.h"
#include <stdint.h>

struct WelovecBootInfo;

namespace welovec::security {

using Credentials = welovec_security_identity;

bool initialize(const WelovecBootInfo *boot_info);
bool setup_required();
bool default_credentials(Credentials *out);
bool root_credentials(Credentials *out);
bool credentials_for_uid(uint32_t uid, Credentials *out);
bool credentials_are_valid(const Credentials &credentials);
bool has_capability(const Credentials &credentials, uint64_t capability);
bool current_has_capability(uint64_t capability);
bool current_user_status(welovec_user_status *out);
int64_t login(const char *name, const char *password);
int64_t logout();
int64_t create_user(const char *name, const char *password, uint32_t flags);
int64_t create_initial_admin(const char *name, const char *password);
int64_t list_users(welovec_user_record *users, uint64_t capacity);
bool secure_random_bytes(uint8_t *output, uint64_t size);

}

#endif
