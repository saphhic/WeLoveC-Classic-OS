/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 31st 2026
 * Description: Supplies freestanding ASCII case-insensitive comparisons.
 */

#include <strings.h>

static unsigned char ascii_lower(unsigned char character) {
    return character >= 'A' && character <= 'Z'
        ? (unsigned char)(character + ('a' - 'A')) : character;
}

int strncasecmp(const char *left, const char *right, size_t maximum) {
    if (!left || !right) return left == right ? 0 : left ? 1 : -1;
    for (size_t index = 0; index < maximum; ++index) {
        const unsigned char a = ascii_lower((unsigned char)left[index]);
        const unsigned char b = ascii_lower((unsigned char)right[index]);
        if (a != b) return (int)a - (int)b;
        if (!a) return 0;
    }
    return 0;
}

int strcasecmp(const char *left, const char *right) {
    size_t index = 0;
    if (!left || !right) return left == right ? 0 : left ? 1 : -1;
    while (left[index] && right[index]) {
        const unsigned char a = ascii_lower((unsigned char)left[index]);
        const unsigned char b = ascii_lower((unsigned char)right[index]);
        if (a != b) return (int)a - (int)b;
        ++index;
    }
    return (int)ascii_lower((unsigned char)left[index]) -
           (int)ascii_lower((unsigned char)right[index]);
}
