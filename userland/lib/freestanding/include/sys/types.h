/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 31st 2026
 * Description: Minimal POSIX-sized types used by freestanding userland ports.
 */

#ifndef WELOVEC_FREESTANDING_SYS_TYPES_H
#define WELOVEC_FREESTANDING_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef __PTRDIFF_TYPE__ ssize_t;
typedef __INT64_TYPE__ off_t;
typedef __UINT32_TYPE__ mode_t;
typedef __UINT32_TYPE__ uid_t;
typedef __UINT32_TYPE__ gid_t;

#endif
