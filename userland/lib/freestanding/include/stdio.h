// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the minimal C input and output functions used by freestanding programs.

#ifndef WELOVEC_FREESTANDING_STDIO_H
#define WELOVEC_FREESTANDING_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct welovec_FILE FILE;
int printf(const char *format, ...);
int snprintf(char *buffer, size_t capacity, const char *format, ...);
int vsnprintf(char *buffer, size_t capacity, const char *format, va_list arguments);
int fprintf(FILE *stream, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list arguments);

#endif
