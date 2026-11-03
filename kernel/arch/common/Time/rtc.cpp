// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Reads the real-time clock and provides date and time to the kernel.

#include "rtc.h"
#include "../arch/x86_64/io/io.h"
#include "../arch/x86_64/cpu/cpu.h"

static const uint16_t CMOS_INDEX = 0x70;
static const uint16_t CMOS_DATA = 0x71;

static const uint8_t RTC_SECONDS = 0x00;
static const uint8_t RTC_MINUTES = 0x02;
static const uint8_t RTC_HOURS = 0x04;
static const uint8_t RTC_DAY = 0x07;
static const uint8_t RTC_MONTH = 0x08;
static const uint8_t RTC_YEAR = 0x09;
static const uint8_t RTC_STATUS_A = 0x0A;
static const uint8_t RTC_STATUS_B = 0x0B;

static int timezone_offset = 0;

struct RtcRaw {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t status_b;
};

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, (uint8_t)(reg | 0x80));
    return inb(CMOS_DATA);
}

static bool rtc_wait_ready() {
    for (int i = 0; i < 10000; i++) {
        if ((cmos_read(RTC_STATUS_A) & 0x80) == 0) {
            return true;
        }
        pause_cpu();
    }
    return false;
}

static uint8_t bcd_to_bin(uint8_t val) {
    return (uint8_t)((val & 0x0Fu) + ((val >> 4) * 10u));
}

static bool same_raw(const RtcRaw& a, const RtcRaw& b) {
    return a.second == b.second &&
           a.minute == b.minute &&
           a.hour == b.hour &&
           a.day == b.day &&
           a.month == b.month &&
           a.year == b.year &&
           a.status_b == b.status_b;
}

static bool read_raw(RtcRaw* out) {
    if (!out || !rtc_wait_ready()) return false;

    out->second = cmos_read(RTC_SECONDS);
    out->minute = cmos_read(RTC_MINUTES);
    out->hour = cmos_read(RTC_HOURS);
    out->day = cmos_read(RTC_DAY);
    out->month = cmos_read(RTC_MONTH);
    out->year = cmos_read(RTC_YEAR);
    out->status_b = cmos_read(RTC_STATUS_B);
    return true;
}

static bool read_stable_raw(RtcRaw* out) {
    // A small visual clock error is preferable to blocking the whole system
    // for 500 ms, so read the RTC directly here.
    if (!read_raw(out)) return false;
    return true;
}

static bool is_leap_year(int year) {
    if ((year % 400) == 0) return true;
    if ((year % 100) == 0) return false;
    return (year % 4) == 0;
}

static int days_in_month(int year, int month) {
    static const int days[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    if (month == 2 && is_leap_year(year)) return 29;
    if (month < 1 || month > 12) return 31;
    return days[month - 1];
}

static void normalize_datetime(RtcDateTime* t) {
    if (!t) return;

    while (t->hour >= 24) {
        t->hour -= 24;
        t->day++;
        if (t->day > days_in_month(t->year, t->month)) {
            t->day = 1;
            t->month++;
            if (t->month > 12) {
                t->month = 1;
                t->year++;
            }
        }
    }

    while (t->hour < 0) {
        t->hour += 24;
        t->day--;
        if (t->day < 1) {
            t->month--;
            if (t->month < 1) {
                t->month = 12;
                t->year--;
            }
            t->day = days_in_month(t->year, t->month);
        }
    }
}

static void convert_raw(const RtcRaw& raw, RtcDateTime* out) {
    bool binary = (raw.status_b & 0x04) != 0;
    bool twenty_four_hour = (raw.status_b & 0x02) != 0;

    uint8_t raw_hour = raw.hour;
    bool pm = !twenty_four_hour && ((raw_hour & 0x80) != 0);
    raw_hour &= 0x7F;

    int second = binary ? raw.second : bcd_to_bin(raw.second);
    int minute = binary ? raw.minute : bcd_to_bin(raw.minute);
    int hour = binary ? raw_hour : bcd_to_bin(raw_hour);
    int day = binary ? raw.day : bcd_to_bin(raw.day);
    int month = binary ? raw.month : bcd_to_bin(raw.month);
    int year = binary ? raw.year : bcd_to_bin(raw.year);

    if (!twenty_four_hour) {
        if (pm && hour < 12) hour += 12;
        if (!pm && hour == 12) hour = 0;
    }

    out->second = second;
    out->minute = minute;
    out->hour = hour;
    out->day = day;
    out->month = month;
    out->year = year >= 70 ? 1900 + year : 2000 + year;
}

bool rtc_get_utc_time(RtcDateTime* out_time) {
    if (!out_time) return false;

    RtcRaw raw;
    if (!read_stable_raw(&raw)) return false;
    convert_raw(raw, out_time);
    return true;
}

bool rtc_get_local_time(RtcDateTime* out_time) {
    if (!rtc_get_utc_time(out_time)) return false;

    out_time->hour += timezone_offset;
    normalize_datetime(out_time);
    return true;
}

extern "C" unsigned char rtc_get_seconds() {
    RtcDateTime time;
    return rtc_get_utc_time(&time) ? (unsigned char)time.second : 0;
}

extern "C" unsigned char rtc_get_minutes() {
    RtcDateTime time;
    return rtc_get_utc_time(&time) ? (unsigned char)time.minute : 0;
}

extern "C" unsigned char rtc_get_hours() {
    RtcDateTime time;
    return rtc_get_utc_time(&time) ? (unsigned char)time.hour : 0;
}

int rtc_get_utc_hour() {
    return (int)rtc_get_hours();
}

int rtc_get_local_hour() {
    RtcDateTime time;
    return rtc_get_local_time(&time) ? time.hour : 0;
}

void set_timezone(int offset) {
    if (offset < -12) offset = -12;
    if (offset > 14) offset = 14;
    timezone_offset = offset;
}

uint32_t rtc_get_total_seconds() {
    RtcDateTime time;
    if (rtc_get_local_time(&time)) {
        return (uint32_t)time.hour * 3600 + (uint32_t)time.minute * 60 + (uint32_t)time.second;
    }
    return 0; // Fall back when the RTC is unavailable.
}

int rtc_get_timezone() {
    return timezone_offset;
}
