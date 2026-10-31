// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares the function that runs a program specified in a configuration file.

#ifndef WELOVEC_USER_EXEC_CONF_H
#define WELOVEC_USER_EXEC_CONF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads exec=/absolute/path.elf (or binary=...) and launches as the caller.
   Returns the new PID, a negative errno, or the exec loader's -100 error. */
int64_t welovec_exec_conf(const char *config_path);

/* Short alias for shell programs. */
int64_t exec_conf(const char *config_path);

#ifdef __cplusplus
}
#endif

#endif
