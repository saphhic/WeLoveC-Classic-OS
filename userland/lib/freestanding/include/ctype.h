/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 31st 2026
 * Description: ASCII character classification for freestanding user programs.
 */

#ifndef WELOVEC_FREESTANDING_CTYPE_H
#define WELOVEC_FREESTANDING_CTYPE_H

static inline int tolower(int character) {
    return character >= 'A' && character <= 'Z' ? character + ('a' - 'A')
                                                : character;
}

static inline int toupper(int character) {
    return character >= 'a' && character <= 'z' ? character - ('a' - 'A')
                                                : character;
}

static inline int isdigit(int character) {
    return character >= '0' && character <= '9';
}

static inline int isspace(int character) {
    return character == ' ' || (character >= '\t' && character <= '\r');
}

#endif
