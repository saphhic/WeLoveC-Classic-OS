// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Defines error codes available in the freestanding environment.

#ifndef WELOVEC_FREESTANDING_ERRNO_H
#define WELOVEC_FREESTANDING_ERRNO_H

#define EINVAL 22
#define ENOMEM 12
#define EIO 5
#define EAGAIN 11
#define EINTR 4
#define EWOULDBLOCK EAGAIN
#define ENOSYS 38

extern int errno;

#endif
