/* ui_events_logic.c: the Mosaico's recent-events rows (Summary app). Time since boot, newest first over the ring, the
   S3's colours by level and its "Safe to turn off" rule, the filters (Warnings, Errors, Wi-Fi, Drive, the drive's time
   window), UTF-8-safe truncation, the session in a "Drive sN saved" line, the pattern footer, the change fingerprint. */
#include <stdio.h>
#include <string.h>
#include "ui_events_logic.h"

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

#define OK_MARK   "\xEF\x80\x8C"                     /* LV_SYMBOL_OK */
#define WARN_MARK "\xEF\x81\xB1"                     /* LV_SYMBOL_WARNING */

static sys_event_t ring[EVENT_MAX];
static uint8_t head, count;

/* the same ring rule as events.c (an identical repeat of the newest only refreshes its time) */
static void ev(int64_t ms, event_level_t lv, const char *t)
{
    sys_event_t *newest = count ? &ring[(head + EVENT_MAX - 1) % EVENT_MAX] : NULL;
    if (newest && strcmp(newest->text, t) == 0) { newest->uptime_ms = ms; return; }
    sys_event_t *e = &ring[head];
    e->uptime_ms = ms; e->level = lv;
    snprintf(e->text, sizeof e->text, "%s", t);
    head = (head + 1) % EVENT_MAX;
    if (count < EVENT_MAX) count++;
}
static void reset(void) { memset(ring, 0, sizeof ring); head = count = 0; }

static bool utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0) return false;
        p++;
        for (int i = 0; i < n; i++, p++) if ((*p & 0xC0) != 0x80) return false;
    }
    return true;
}

int main(void)
{
    char b[32];
    /* ---- time since boot ---- */
    CHECK(uev_uptime(b, sizeof b, 0) == 4 && !strcmp(b, "0:00"));
    uev_uptime(b, sizeof b, 999); CHECK(!strcmp(b, "0:00"));
    uev_uptime(b, sizeof b, 8000); CHECK(!strcmp(b, "0:08"));
    uev_uptime(b, sizeof b, 42999); CHECK(!strcmp(b, "0:42"));
    uev_uptime(b, sizeof b, 59 * 60000 + 59999); CHECK(!strcmp(b, "59:59"));
    uev_uptime(b, sizeof b, 3600000); CHECK(!strcmp(b, "1:00:00"));
    uev_uptime(b, sizeof b, 3723000); CHECK(!strcmp(b, "1:02:03"));
    uev_uptime(b, sizeof b, 100LL * 3600000 + 5000); CHECK(!strcmp(b, "100:00:05"));
    uev_uptime(b, sizeof b, -5000); CHECK(!strcmp(b, "0:00"));
    CHECK(uev_uptime(b, 3, 3723000) == 2 && !strcmp(b, "1:"));         /* cut, terminated */
    CHECK(uev_uptime(b, 0, 1000) == 0);

    /* ---- UTF-8 safe copy ---- */
    char d[8];
    uev_copy(d, sizeof d, "abc"); CHECK(!strcmp(d, "abc"));
    uev_copy(d, sizeof d, "abcdefghij"); CHECK(!strcmp(d, "abcdefg"));
    uev_copy(d, sizeof d, "abcde\xC3\xA9xyz"); CHECK(!strcmp(d, "abcde\xC3\xA9"));   /* fits exactly */
    uev_copy(d, sizeof d, "abcdef\xC3\xA9"); CHECK(!strcmp(d, "abcdef"));             /* 2-byte char at the edge */
    uev_copy(d, sizeof d, "abcde" OK_MARK); CHECK(!strcmp(d, "abcde"));               /* 3-byte glyph never half */
    uev_copy(d, sizeof d, "\xF0\x9F\x9A\x97\xF0\x9F\x9A\x97"); CHECK(!strcmp(d, "\xF0\x9F\x9A\x97"));
    uev_copy(d, 1, "abc"); CHECK(d[0] == 0);

    /* ---- categories: every sys_event text of the firmware that names Wi-Fi or a drive ---- */
    CHECK(uev_category("Wi-Fi connected: example-net") == UEV_CAT_WIFI);
    CHECK(uev_category("Wi-Fi lost (reason 8)") == UEV_CAT_WIFI);
    CHECK(uev_category("Clock synced over Wi-Fi") == UEV_CAT_WIFI);
    CHECK(uev_category("Clock sync from pool failed (timeout)") == UEV_CAT_WIFI);
    CHECK(uev_category("Clock-sync task could not start (internal RAM 100 B)") == UEV_CAT_WIFI);
    CHECK(uev_category("OBD adapter not responding (ATZ)") == UEV_CAT_DRIVE);
    CHECK(uev_category("Adapter linked: OBDII") == UEV_CAT_DRIVE);
    CHECK(uev_category("ECU online: ISO 15765-4 CAN") == UEV_CAT_DRIVE);
    CHECK(uev_category("Engine ECU 7E8 picked (2 answering)") == UEV_CAT_DRIVE);
    CHECK(uev_category("Drive s000123 saved (812 rows)") == UEV_CAT_DRIVE);
    CHECK(uev_category("End drive pressed on the screen") == UEV_CAT_DRIVE);
    CHECK(uev_category("Safe to turn off") == UEV_CAT_DRIVE);
    CHECK(uev_category("Fill-up applied: fuel x1.020, 1 tank") == UEV_CAT_DRIVE);
    CHECK(uev_category("Searching for an OBD adapter for 90 s") == UEV_CAT_DRIVE);
    CHECK(uev_category("config.json: 2 setting(s) applied") == 0);
    CHECK(uev_category("ui hung past 3x deadline, restarting") == 0);

    /* ---- the owner's S3 picture, oldest first ---- */
    reset();
    ev(8000, EV_ERROR, "OBD adapter not responding (ATZ)");
    ev(25000, EV_INFO, "Wi-Fi connected: example-net");
    ev(30000, EV_INFO, "Clock synced over Wi-Fi");
    ev(42000, EV_INFO, "Safe to turn off");
    uev_ctx_t c = { .power = POWER_SAFE, .mark_ok = OK_MARK, .mark_warn = WARN_MARK };
    uev_row_t r[EVENT_MAX];
    int n = uev_rows(ring, head, count, UEV_ALL, &c, r, EVENT_MAX);
    CHECK(n == 4);
    CHECK(!strcmp(r[0].line, "0:42 " OK_MARK " Safe to turn off") && r[0].tone == UEV_GOOD && r[0].save_line && r[0].age == 0);
    CHECK(!strcmp(r[1].line, "0:30 Clock synced over Wi-Fi") && r[1].tone == UEV_TEXT);
    CHECK(!strcmp(r[2].line, "0:25 Wi-Fi connected: example-net") && r[2].tone == UEV_TEXT);
    CHECK(!strcmp(r[3].line, "0:08 OBD adapter not responding (ATZ)") && r[3].tone == UEV_ERROR);
    CHECK(!strcmp(r[3].when, "0:08") && !strcmp(r[3].text, "OBD adapter not responding (ATZ)"));
    /* not safe now: the newest save line turns red with the reason, as on the S3 */
    c.power = POWER_LOGGING;
    uev_rows(ring, head, count, UEV_ALL, &c, r, EVENT_MAX);
    CHECK(!strcmp(r[0].text, WARN_MARK " Not safe: logging") && r[0].tone == UEV_ERROR);
    /* no marks given: no stray space */
    uev_ctx_t c0 = { .power = POWER_SAFE };
    uev_rows(ring, head, count, UEV_ALL, &c0, r, EVENT_MAX);
    CHECK(!strcmp(r[0].text, "Safe to turn off"));
    CHECK(uev_rows(ring, head, count, UEV_ALL, NULL, r, EVENT_MAX) == 4 && !strcmp(r[0].text, "Safe to turn off"));
    /* a warning is amber, an older save line reads "Saved" (muted), the newest one stays live */
    ev(50000, EV_WARN, "ECU not answering: ignition off?");
    ev(61000, EV_INFO, "Safe to turn off");
    c.power = POWER_SAFE;
    n = uev_rows(ring, head, count, UEV_ALL, &c, r, EVENT_MAX);
    CHECK(n == 6);
    CHECK(r[0].save_line && r[0].tone == UEV_GOOD && !strcmp(r[0].when, "1:01"));
    CHECK(r[1].tone == UEV_WARN);
    CHECK(r[2].save_line && r[2].tone == UEV_MUTED && !strcmp(r[2].line, "0:42 Saved"));
    /* a repeat of the newest only refreshes its time */
    ev(70000, EV_INFO, "Safe to turn off");
    CHECK(uev_rows(ring, head, count, UEV_ALL, &c, r, EVENT_MAX) == 6 && !strcmp(r[0].when, "1:10"));
    /* max limits the rows, newest first */
    CHECK(uev_rows(ring, head, count, UEV_ALL, &c, r, 2) == 2 && r[1].tone == UEV_WARN);
    CHECK(uev_rows(ring, head, count, UEV_ALL, &c, r, 0) == 0);

    /* ---- filters ---- */
    CHECK(uev_rows(ring, head, count, UEV_WARNINGS, &c, r, EVENT_MAX) == 1 && r[0].tone == UEV_WARN);
    CHECK(uev_rows(ring, head, count, UEV_ERRORS, &c, r, EVENT_MAX) == 1 && r[0].tone == UEV_ERROR && r[0].age == 5);
    n = uev_rows(ring, head, count, UEV_WIFI, &c, r, EVENT_MAX);
    CHECK(n == 2 && strstr(r[0].text, "Clock synced") && strstr(r[1].text, "Wi-Fi connected"));
    /* Drive with no window: by type (the adapter error, the ECU warning, both saves) */
    n = uev_rows(ring, head, count, UEV_DRIVE, &c, r, EVENT_MAX);
    CHECK(n == 4);
    /* the filtered list keeps the live rule of the whole ring: the newest save is live even when filtered */
    CHECK(r[0].tone == UEV_GOOD && r[2].tone == UEV_MUTED);
    /* Drive with a window: a drive from 20 s to 45 s (dur 25 s) plus the 30 s tail -> 25, 30, 42, 50, 61 s (75 s end) */
    uev_ctx_t cw = c;
    cw.win_ok = true; cw.win_start_ms = 20000; cw.win_dur_s = 25;
    n = uev_rows(ring, head, count, UEV_DRIVE, &cw, r, EVENT_MAX);
    CHECK(n == 5);
    CHECK(uev_in_drive(20000, 20000, 25) && uev_in_drive(75000, 20000, 25) && !uev_in_drive(75001, 20000, 25));
    CHECK(!uev_in_drive(19999, 20000, 25) && !uev_in_drive(30000, 20000, -1));
    cw.win_start_ms = 200000;                              /* a window after every event: none */
    CHECK(uev_rows(ring, head, count, UEV_DRIVE, &cw, r, EVENT_MAX) == 0);

    /* ---- a full ring wraps: the newest EVENT_MAX survive, newest first ---- */
    reset();
    for (int i = 0; i < 25; i++) { char t[24]; snprintf(t, sizeof t, "event %d", i); ev(1000LL * i, EV_INFO, t); }
    CHECK(count == EVENT_MAX);
    n = uev_rows(ring, head, count, UEV_ALL, &c, r, EVENT_MAX);
    CHECK(n == EVENT_MAX && !strcmp(r[0].text, "event 24") && !strcmp(r[EVENT_MAX - 1].text, "event 15"));
    int idx[EVENT_MAX];
    CHECK(uev_order(0, 0, idx) == 0);
    CHECK(uev_order(3, 3, idx) == 3 && idx[0] == 2 && idx[2] == 0);
    CHECK(uev_order(0, EVENT_MAX, idx) == EVENT_MAX && idx[0] == EVENT_MAX - 1);
    CHECK(uev_order(2, 200, idx) == EVENT_MAX && idx[0] == 1 && idx[1] == 0 && idx[2] == EVENT_MAX - 1);

    /* ---- truncation: the longest text the ring keeps, with the longest time and mark, fits whole and valid ---- */
    reset();
    char longt[EVENT_TEXT];
    memset(longt, 'x', sizeof longt - 1); longt[sizeof longt - 1] = 0;
    ev(100LL * 3600000, EV_WARN, longt);
    n = uev_rows(ring, head, count, UEV_ALL, &c, r, 1);
    CHECK(n == 1 && strlen(r[0].line) == strlen("100:00:00 ") + EVENT_TEXT - 1 && utf8_valid(r[0].line));
    /* a name that events.c cut in the middle of a character (vsnprintf at EVENT_TEXT) is cut clean here */
    reset();
    char nm[EVENT_TEXT + 8];
    snprintf(nm, sizeof nm, "Wi-Fi connected: a%s", "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                                                   "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9");
    ev(1000, EV_INFO, nm);                                  /* stored cut to EVENT_TEXT - 1 bytes: a lone lead byte last */
    CHECK(strlen(ring[0].text) == EVENT_TEXT - 1 && !utf8_valid(ring[0].text));
    n = uev_rows(ring, head, count, UEV_ALL, &c, r, 1);
    CHECK(n == 1 && utf8_valid(r[0].text) && utf8_valid(r[0].line) && strlen(r[0].text) == EVENT_TEXT - 2);
    uev_copy(d, sizeof d, "ab\xC3"); CHECK(!strcmp(d, "ab"));   /* short but cut: dropped too */

    /* ---- session in a line, level names, chips ---- */
    CHECK(uev_session("Drive s000123 saved (812 rows)") == 123);
    CHECK(uev_session("Drive log s004567 deleted on the board (3 files)") == 4567);
    CHECK(uev_session("Drive s12 saved") == 0 && uev_session("Safe to turn off") == 0 && uev_session("Drive summary") == 0);
    CHECK(!strcmp(uev_level_name(EV_INFO), "Information") && !strcmp(uev_level_name(EV_WARN), "Warning")
          && !strcmp(uev_level_name(EV_ERROR), "Error"));
    CHECK(!strcmp(uev_filter_name(UEV_ALL), "All") && !strcmp(uev_filter_name(UEV_DRIVE), "Drive") && !strcmp(uev_filter_name(UEV_FILTERS), ""));

    /* ---- the S3 footer ---- */
    char f[64];
    uev_pattern(f, sizeof f, 412, 7, 0.8f, 9); CHECK(!strcmp(f, "pattern 7  novelty 0.8  (9/412)"));
    uev_pattern(f, sizeof f, 0, 0, 0, 0); CHECK(!strcmp(f, "pattern: first window pending"));
    uev_pattern(f, 8, 412, 7, 0.8f, 9); CHECK(strlen(f) == 7);

    /* ---- clock time of an event, the fingerprint ---- */
    CHECK(uev_epoch_ms(42000, 100000, 1800000000000LL) == 1800000000000LL - 58000);
    reset();
    ev(1000, EV_INFO, "a");
    uint32_t s1 = uev_sig(ring, head, count, POWER_SAFE, 0);
    CHECK(s1 == uev_sig(ring, head, count, POWER_SAFE, 0));
    CHECK(s1 != uev_sig(ring, head, count, POWER_LOGGING, 0));
    CHECK(s1 != uev_sig(ring, head, count, POWER_SAFE, 1));
    ev(5000, EV_INFO, "a");                                 /* a repeat: same rows count, new time */
    CHECK(s1 != uev_sig(ring, head, count, POWER_SAFE, 0));
    uint32_t s2 = uev_sig(ring, head, count, POWER_SAFE, 0);
    ev(6000, EV_INFO, "b");
    CHECK(s2 != uev_sig(ring, head, count, POWER_SAFE, 0));

    printf("ui_events: %d checks, %d failures\n", runs, fails);
    return fails ? 1 : 0;
}
