/* Setting the clock by hand: the pure parts (clock_set.h). No ESP-IDF calls, so tests/host builds it as is. */
#include "clock_set.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "harden.h"

clock_src_t clock_source(const clock_flags_t *f)
{
    if (f->wifi) return CLOCK_SRC_WIFI;
    if (f->manual) return CLOCK_SRC_MANUAL;
    if (f->estimated) return CLOCK_SRC_ESTIMATED;
    return CLOCK_SRC_NONE;
}

const char *clock_source_name(clock_src_t s)
{
    switch (s) {
    case CLOCK_SRC_WIFI:      return "Wi-Fi";
    case CLOCK_SRC_MANUAL:    return "set by hand";
    case CLOCK_SRC_ESTIMATED: return "estimated";
    default:                  return "none";
    }
}

void clock_on_manual(clock_flags_t *f) { f->wifi = false; f->estimated = false; f->manual = true; }
void clock_on_wifi(clock_flags_t *f)   { f->wifi = true;  f->estimated = false; f->manual = false; }
void clock_on_boot(clock_flags_t *f, bool rtc_manual)
{
    if (rtc_manual && f->wifi) { f->wifi = false; f->manual = true; }
}

bool clock_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int clock_days_in_month(int y, int m)
{
    static const int D[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 0;
    return m == 2 && clock_leap(y) ? 29 : D[m - 1];
}

bool clock_fields_ok(int y, int mo, int d, int h, int mi)
{
    if (y < CLOCK_YEAR_MIN || y > CLOCK_YEAR_MAX) return false;
    if (d < 1 || d > clock_days_in_month(y, mo)) return false;   /* also rejects a bad month */
    return h >= 0 && h <= 23 && mi >= 0 && mi <= 59;
}

/* POSIX strings = the footer of each zone's TZif file in IANA tzdata 2026e (what glibc and newlib apply past the table) */
static const clock_tz_t TZ[] = {
    { "UTC",                          "UTC0" },
    { "US Eastern (New York)",        "EST5EDT,M3.2.0,M11.1.0" },
    { "US Central (Chicago)",         "CST6CDT,M3.2.0,M11.1.0" },
    { "US Mountain (Denver)",         "MST7MDT,M3.2.0,M11.1.0" },
    { "US Arizona (Phoenix)",         "MST7" },
    { "US Pacific (Los Angeles)",     "PST8PDT,M3.2.0,M11.1.0" },
    { "US Alaska (Anchorage)",        "AKST9AKDT,M3.2.0,M11.1.0" },
    { "US Hawaii (Honolulu)",         "HST10" },
    { "UK (London)",                  "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Central Europe (Berlin)",      "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Eastern Europe (Athens)",      "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "India (Kolkata)",              "IST-5:30" },
    { "Bangladesh (Dhaka)",           "<+06>-6" },
    { "China (Shanghai)",             "CST-8" },
    { "Japan (Tokyo)",                "JST-9" },
    { "Australia East (Sydney)",      "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia (Brisbane)",         "AEST-10" },
};

int clock_tz_count(void) { return (int)(sizeof TZ / sizeof TZ[0]); }
const clock_tz_t *clock_tz(int i) { return i >= 0 && i < clock_tz_count() ? &TZ[i] : NULL; }

int clock_tz_find(const char *tz)
{
    if (!tz) return -1;
    for (int i = 0; i < clock_tz_count(); i++) if (!strcmp(TZ[i].tz, tz)) return i;
    return -1;
}

bool clock_local_to_epoch(const char *tz, int y, int mo, int d, int h, int mi, int64_t *epoch)
{
    if (!harden_timezone_ok(tz) || !clock_fields_ok(y, mo, d, h, mi)) return false;
    char old[64];
    const char *cur = getenv("TZ");
    bool had = cur != NULL;
    if (had) snprintf(old, sizeof old, "%s", cur);
    setenv("TZ", tz, 1);
    tzset();
    struct tm tm = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_isdst = -1 };
    time_t t = mktime(&tm);
    if (had) setenv("TZ", old, 1); else unsetenv("TZ");
    tzset();
    if (t == (time_t)-1 || !harden_epoch_plausible((int64_t)t)) return false;
    *epoch = (int64_t)t;
    return true;
}
