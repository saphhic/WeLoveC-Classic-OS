// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for tf psa user config in the tls module.

#ifndef WELOVEC_TF_PSA_USER_CONFIG_H
#define WELOVEC_TF_PSA_USER_CONFIG_H

#include <stddef.h>

void *welovec_tls_calloc(size_t count, size_t size);
void welovec_tls_free(void *memory);
long welovec_tls_time(long *result);

/* Seed PSA cryptography from the kernel's entropy-backed CSPRNG syscall. */
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
#undef MBEDTLS_PSA_BUILTIN_GET_ENTROPY
#undef MBEDTLS_PSA_DRIVER_GET_ENTROPY
#undef MBEDTLS_FS_IO
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C
#define MBEDTLS_PLATFORM_TIME_TYPE_MACRO long
#define MBEDTLS_PLATFORM_TIME_MACRO welovec_tls_time
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_CALLOC_MACRO welovec_tls_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO welovec_tls_free

#endif
