/* clock_set.c: Settings > Clock on the ESP-Mosaico (see main/clock_set.h). Expected epochs come from Python's zoneinfo
   (IANA tzdata 2026e): datetime(..., tzinfo=ZoneInfo(zone)).timestamp(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clock_set.h"
#include "harden.h"

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static int64_t conv(const char *tz, int y, int mo, int d, int h, int mi)
{
    int64_t e = -1;
    return clock_local_to_epoch(tz, y, mo, d, h, mi, &e) ? e : -1;
}

/* R6, the Wi-Fi clock's jump guard (clock_set.h clock_jump_check): what a sync may do to the running clock */
static void test_clock_jump(void)
{
    const int64_t now = 1794700800;                                  /* 2026-11-15 00:00 UTC */
    /* Wi-Fi was the source: up to 24 h ahead and 10 min behind pass, one second more is refused */
    CHECK(clock_jump_check(CLOCK_SRC_WIFI, now, now) == CLOCK_JUMP_OK);
    CHECK(clock_jump_check(CLOCK_SRC_WIFI, now, now + 3) == CLOCK_JUMP_OK);                /* normal drift */
    CHECK(clock_jump_check(CLOCK_SRC_WIFI, now, now + CLOCK_JUMP_AHEAD_S) == CLOCK_JUMP_OK);
    CHECK(clock_jump_check(CLOCK_SRC_WIFI, now, now + CLOCK_JUMP_AHEAD_S + 1) == CLOCK_JUMP_AHEAD);
    CHECK(clock_jump_check(CLOCK_SRC_WIFI, now, now + 400LL * 86400) == CLOCK_JUMP_AHEAD);   /* a year on, still in range */
    CHECK(clock_jump_check(CLOCK_SRC_WIFI, now, now - CLOCK_JUMP_BEHIND_S) == CLOCK_JUMP_OK);
    CHECK(clock_jump_check(CLOCK_SRC_WIFI, now, now - CLOCK_JUMP_BEHIND_S - 1) == CLOCK_JUMP_BEHIND);
    CHECK(clock_jump_check(CLOCK_SRC_WIFI, now, now - 3600) == CLOCK_JUMP_BEHIND);
    /* not guarded: no clock yet, an estimate after a power cut, a clock set by hand */
    const clock_src_t free_src[] = {CLOCK_SRC_NONE, CLOCK_SRC_ESTIMATED, CLOCK_SRC_MANUAL};
    for (int i = 0; i < 3; i++) {
        CHECK(clock_jump_check(free_src[i], now, now + 400LL * 86400) == CLOCK_JUMP_OK);
        CHECK(clock_jump_check(free_src[i], now, now - 400LL * 86400) == CLOCK_JUMP_OK);
        CHECK(clock_jump_check(free_src[i], 0, now) == CLOCK_JUMP_OK);                      /* epoch 0: never set */
    }
    /* the source comes from the same flags Settings shows: a by-hand set clears Wi-Fi, a later sync makes it Wi-Fi again */
    clock_flags_t f = {.wifi = true};
    CHECK(clock_jump_check(clock_source(&f), now, now - 7200) == CLOCK_JUMP_BEHIND);
    clock_on_manual(&f);
    CHECK(clock_jump_check(clock_source(&f), now, now - 7200) == CLOCK_JUMP_OK);
    clock_on_wifi(&f);
    CHECK(clock_jump_check(clock_source(&f), now, now + 2 * 86400) == CLOCK_JUMP_AHEAD);
    /* the 2026-2099 range check stays in front of the guard in wifi_sync.c */
    CHECK(!harden_epoch_plausible(1735689599) && harden_epoch_plausible(1767225600));
}

int main(void)
{
    test_clock_jump();
    /* leap years and month lengths */
    CHECK(clock_leap(2028) && clock_leap(2000) && clock_leap(2096));
    CHECK(!clock_leap(2026) && !clock_leap(2100) && !clock_leap(2027));
    CHECK(clock_days_in_month(2028, 2) == 29 && clock_days_in_month(2026, 2) == 28);
    static const int D[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int bad = 0;
    for (int m = 1; m <= 12; m++) if (clock_days_in_month(2027, m) != D[m - 1]) bad++;
    CHECK(bad == 0);
    CHECK(clock_days_in_month(2026, 0) == 0 && clock_days_in_month(2026, 13) == 0);

    /* field checks */
    CHECK(clock_fields_ok(2026, 1, 1, 0, 0));
    CHECK(clock_fields_ok(2099, 12, 31, 23, 59));
    CHECK(clock_fields_ok(2028, 2, 29, 12, 0));
    CHECK(!clock_fields_ok(2026, 2, 29, 12, 0));        /* not a leap year */
    CHECK(!clock_fields_ok(2026, 4, 31, 12, 0));        /* April has 30 days */
    CHECK(!clock_fields_ok(2026, 13, 1, 12, 0));
    CHECK(!clock_fields_ok(2026, 6, 0, 12, 0));
    CHECK(!clock_fields_ok(2026, 6, 1, 24, 0));
    CHECK(!clock_fields_ok(2026, 6, 1, 12, 60));
    CHECK(!clock_fields_ok(2023, 6, 1, 12, 0));         /* before 2024 is refused (the floor is 2026) */
    CHECK(!clock_fields_ok(2025, 12, 31, 23, 59));
    CHECK(!clock_fields_ok(2100, 1, 1, 0, 0));

    /* the time-zone list: every entry passes the same check config.json's "timezone" does, labels are unique, the
       zones the owner asked for are there, and lookup is exact */
    int n = clock_tz_count(), ok = 0;
    for (int i = 0; i < n; i++) ok += harden_timezone_ok(clock_tz(i)->tz) && strlen(clock_tz(i)->label) <= 28;
    CHECK(ok == n);
    int dup = 0;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) dup += !strcmp(clock_tz(i)->tz, clock_tz(j)->tz);
    CHECK(dup == 0);
    const char *need[] = {"UTC0", "EST5EDT,M3.2.0,M11.1.0", "CST6CDT,M3.2.0,M11.1.0", "MST7MDT,M3.2.0,M11.1.0", "MST7",
                          "PST8PDT,M3.2.0,M11.1.0", "AKST9AKDT,M3.2.0,M11.1.0", "HST10", "GMT0BST,M3.5.0/1,M10.5.0",
                          "CET-1CEST,M3.5.0,M10.5.0/3", "IST-5:30", "JST-9", "AEST-10AEDT,M10.1.0,M4.1.0/3"};
    int found = 0;
    for (size_t i = 0; i < sizeof need / sizeof need[0]; i++) found += clock_tz_find(need[i]) >= 0;
    CHECK(found == (int)(sizeof need / sizeof need[0]));
    CHECK(clock_tz_find("CST6CDT") == -1 && clock_tz_find("") == -1 && clock_tz_find(NULL) == -1);
    CHECK(clock_tz(n) == NULL && clock_tz(-1) == NULL);

    /* local time -> epoch, against zoneinfo */
    CHECK(conv("CST6CDT,M3.2.0,M11.1.0", 2026, 7, 4, 12, 0) == 1783184400);    /* CDT, UTC-5 */
    CHECK(conv("CST6CDT,M3.2.0,M11.1.0", 2026, 1, 15, 8, 30) == 1768487400);   /* CST, UTC-6 */
    CHECK(conv("CST6CDT,M3.2.0,M11.1.0", 2026, 3, 8, 3, 30) == 1772958600);    /* first hour of CDT */
    CHECK(conv("CST6CDT,M3.2.0,M11.1.0", 2026, 3, 8, 2, 30) == 1772958600);    /* in the gap: moved forward one hour */
    CHECK(conv("IST-5:30", 2026, 11, 11, 12, 0) == 1794378600);
    CHECK(conv("AEST-10AEDT,M10.1.0,M4.1.0/3", 2026, 6, 1, 9, 0) == 1780268400);
    CHECK(conv("GMT0BST,M3.5.0/1,M10.5.0", 2026, 6, 1, 9, 0) == 1780300800);
    CHECK(conv("UTC0", 2026, 1, 1, 0, 0) == 1767225600);                         /* the first accepted second */
    CHECK(conv("UTC0", 2099, 12, 31, 23, 59) == 4102444740);
    CHECK(conv("<+06>-6", 2099, 12, 31, 23, 59) == 4102423140);
    CHECK(conv("AEST-10AEDT,M10.1.0,M4.1.0/3", 2026, 1, 1, 0, 0) == -1);       /* 2025-12-31 13:00 UTC: too early */
    CHECK(conv("CST6CDT;rm", 2026, 7, 4, 12, 0) == -1);                          /* not a TZ string */
    CHECK(conv("CST6CDT,M3.2.0,M11.1.0", 2026, 2, 30, 12, 0) == -1);

    /* the caller's TZ comes back after a conversion */
    setenv("TZ", "JST-9", 1);
    conv("CST6CDT,M3.2.0,M11.1.0", 2026, 7, 4, 12, 0);
    CHECK(getenv("TZ") && !strcmp(getenv("TZ"), "JST-9"));
    unsetenv("TZ");
    conv("UTC0", 2026, 7, 4, 12, 0);
    CHECK(getenv("TZ") == NULL);

    /* source names and priority: Wi-Fi > set by hand > estimated > none */
    clock_flags_t f = {0};
    CHECK(clock_source(&f) == CLOCK_SRC_NONE && !strcmp(clock_source_name(clock_source(&f)), "none"));
    f.estimated = true;
    CHECK(clock_source(&f) == CLOCK_SRC_ESTIMATED);
    clock_on_manual(&f);                                   /* by hand after an estimate: normal colour, not amber */
    CHECK(clock_source(&f) == CLOCK_SRC_MANUAL && !f.estimated && !f.wifi);
    CHECK(!strcmp(clock_source_name(CLOCK_SRC_MANUAL), "set by hand"));
    clock_on_wifi(&f);                                     /* Wi-Fi overrides a by-hand time when it syncs */
    CHECK(clock_source(&f) == CLOCK_SRC_WIFI && !f.manual);
    clock_on_manual(&f);                                   /* by hand after Wi-Fi: by hand until the next sync */
    CHECK(clock_source(&f) == CLOCK_SRC_MANUAL);
    clock_flags_t both = {.wifi = true, .manual = true};   /* a sync seen before the poll cleared the flag */
    CHECK(clock_source(&both) == CLOCK_SRC_WIFI);
    /* soft reset: app_main marks a kept RTC time as synced; the RTC note puts "set by hand" back */
    clock_flags_t b = {.wifi = true};
    clock_on_boot(&b, true);
    CHECK(clock_source(&b) == CLOCK_SRC_MANUAL);
    clock_flags_t c = {.wifi = true};
    clock_on_boot(&c, false);
    CHECK(clock_source(&c) == CLOCK_SRC_WIFI);
    clock_flags_t e = {.estimated = true};                 /* power cut: no RTC time, so the note means nothing */
    clock_on_boot(&e, true);
    CHECK(clock_source(&e) == CLOCK_SRC_ESTIMATED);

    printf("clock_set: %d checks, %d failures\n", runs, fails);
    return fails ? 1 : 0;
}
