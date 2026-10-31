// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the types, constants, and functions for mbedtls user config in the tls module.

#ifndef WELOVEC_MBEDTLS_USER_CONFIG_H
#define WELOVEC_MBEDTLS_USER_CONFIG_H

#include <stddef.h>

void *welovec_tls_calloc(size_t count, size_t size);
void welovec_tls_free(void *memory);
long welovec_tls_time(long *result);

/* Networking is supplied by WeLoveC's socket syscalls. */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_DEBUG_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_SELF_TEST

#endif
