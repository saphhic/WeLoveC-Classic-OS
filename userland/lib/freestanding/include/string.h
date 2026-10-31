// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares string and memory operations for the freestanding environment.

#ifndef WELOVEC_FREESTANDING_STRING_H
#define WELOVEC_FREESTANDING_STRING_H

#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t size);
void *memmove(void *destination, const void *source, size_t size);
void *memset(void *destination, int value, size_t size);
int memcmp(const void *left, const void *right, size_t size);
size_t strlen(const char *text);
size_t strnlen(const char *text, size_t maximum);
char *strstr(const char *text, const char *needle);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t maximum);
char *strncpy(char *destination, const char *source, size_t maximum);
char *strchr(const char *text, int character);

#endif
