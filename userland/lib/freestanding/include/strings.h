/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 31st 2026
 * Description: Declares ASCII case-insensitive string comparisons.
 */

#ifndef WELOVEC_FREESTANDING_STRINGS_H
#define WELOVEC_FREESTANDING_STRINGS_H

#include <stddef.h>

int strcasecmp(const char *left, const char *right);
int strncasecmp(const char *left, const char *right, size_t maximum);

#endif
