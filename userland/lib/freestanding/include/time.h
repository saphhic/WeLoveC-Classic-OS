// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares time types available in the freestanding environment.

#ifndef WELOVEC_FREESTANDING_TIME_H
#define WELOVEC_FREESTANDING_TIME_H

#include <stdint.h>

typedef int64_t time_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

time_t time(time_t *result);
struct tm *gmtime_r(const time_t *timestamp, struct tm *result);

#endif
