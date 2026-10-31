// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for password hash in the security module.

#ifndef WELOVEC_SECURITY_PASSWORD_HASH_HPP
#define WELOVEC_SECURITY_PASSWORD_HASH_HPP

#include <stdint.h>

namespace welovec::security::crypto {

void sha256(const uint8_t *data, uint64_t size, uint8_t output[32]);
void hmac_sha256(const uint8_t *key, uint64_t key_size,
                 const uint8_t *data, uint64_t data_size, uint8_t output[32]);
void pbkdf2_sha256(const uint8_t *password, uint64_t password_size,
                   const uint8_t *salt, uint64_t salt_size,
                   uint32_t iterations, uint8_t output[32]);
bool constant_time_equal(const uint8_t *left, const uint8_t *right, uint64_t size);
void secure_zero(void *memory, uint64_t size);

}

#endif
