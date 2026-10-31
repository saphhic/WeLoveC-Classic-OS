// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Declares clock and timer APIs for user programs.

#ifndef WELOVEC_USER_TIME_H
#define WELOVEC_USER_TIME_H

#include <stdint.h>
#include "user/syscall.h"

struct welovec_user_datetime {
    int32_t year;
    int32_t month;
    int32_t day;
    int32_t hour;
    int32_t minute;
    int32_t second;
};

static inline int64_t welovec_rtc_get_utc(struct welovec_user_datetime *time) {
    return welovec_user_syscall1(WELOVEC_SYS_RTC_UTC,
                                 (uint64_t)(uintptr_t)time);
}

#endif
