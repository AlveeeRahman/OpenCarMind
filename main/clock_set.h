/* Setting the clock by hand (ESP-Mosaico Settings > Clock): the pure parts, host-tested (tests/host `make clock`).
 *
 * Clock source priority, highest first:
 *   Wi-Fi        the HTTPS time sync (wifi_sync.c, g_status.time_synced). Always wins when it syncs, also over a time
 *                set by hand, because a by-hand set clears time_synced and the sync task then keeps trying.
 *   set by hand  Settings > Clock > Set clock (shown in the normal colour)
 *   estimated    resumed from the last save on the storage after a power cut (sdlog.c, g_status.clock_estimated, amber)
 *   none         "no clock yet"
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { CLOCK_SRC_NONE = 0, CLOCK_SRC_ESTIMATED, CLOCK_SRC_MANUAL, CLOCK_SRC_WIFI } clock_src_t;

typedef struct {
    bool wifi;        /* g_status.time_synced */
    bool manual;      /* set by hand since the last Wi-Fi sync */
    bool estimated;   /* g_status.clock_estimated */
} clock_flags_t;

clock_src_t clock_source(const clock_flags_t *f);
const char *clock_source_name(clock_src_t s);     /* "Wi-Fi", "set by hand", "estimated", "none" */
/* transitions: a by-hand set, a Wi-Fi sync, and the boot after a software reset (the RTC kept the time and app_main
   marked it synced; rtc_manual = the RTC-memory note says the last source was a by-hand set) */
void clock_on_manual(clock_flags_t *f);
void clock_on_wifi(clock_flags_t *f);
void clock_on_boot(clock_flags_t *f, bool rtc_manual);

/* the picker's range: harden_epoch_plausible() accepts 2026-01-01 to 2099-12-31 (UTC), so earlier years never reach it */
#define CLOCK_YEAR_MIN 2026
#define CLOCK_YEAR_MAX 2099
bool clock_leap(int year);
int  clock_days_in_month(int year, int month);    /* month 1..12; 0 for a bad month */
bool clock_fields_ok(int year, int month, int day, int hour, int minute);

/* time zones the picker offers (POSIX TZ strings, as remote_config and harden_timezone_ok take them) */
typedef struct { const char *label; const char *tz; } clock_tz_t;
int               clock_tz_count(void);
const clock_tz_t *clock_tz(int i);
int               clock_tz_find(const char *tz);  /* index of an exact match, -1 if the string is not in the list */

/* local date and time in zone tz -> Unix seconds. Sets TZ for the conversion and puts the caller's TZ back. False when
   the fields or the zone are not valid or the result is outside harden_epoch_plausible(). A time inside a spring-forward
   gap comes back moved by the gap (mktime's normalisation). */
bool clock_local_to_epoch(const char *tz, int year, int month, int day, int hour, int minute, int64_t *epoch);
