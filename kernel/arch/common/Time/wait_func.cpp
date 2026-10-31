// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements the wait func component in the time module.

#include "rtc.h"
#include "wait_func.h"

void wait_seconds(uint32_t seconds) {
    RtcDateTime start, now;
    rtc_get_local_time(&start);

    uint32_t target = start.second + seconds;
    uint8_t target_min = start.minute + (target / 60);
    uint8_t target_sec = target % 60;

    do {
        rtc_get_local_time(&now);
    } while (now.minute < target_min || 
             (now.minute == target_min && now.second < target_sec));
}