/* Recent events on the ESP-Mosaico (ESP32-S31): the shared events.c ring (EVENT_MAX entries since boot, RAM only) shown
 * the way the S3's page shows it (display.c, "RECENT EVENTS"): newest first, the time since boot, the S3 colours (good
 * green, warning amber, error red, normal text), the newest "Safe to turn off" line showing the power state now, and the
 * pattern-novelty footer. Pure C, no LVGL and no locks: the Summary app formats rows from its status copy with these,
 * and tests/host/test_ui_events.c checks them. Lines may carry a Wi-Fi network name ("Wi-Fi connected: <name>"): they
 * are for the board's own screen and nothing here logs. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "events.h"
#include "power_state.h"

#define UEV_LINE         96          /* time + mark + the longest event text (EVENT_TEXT - 1), with room to spare */
#define UEV_DRIVE_TAIL_MS 30000      /* a drive's window also covers the save after its end ("Drive sN saved") */

typedef enum { UEV_TEXT = 0, UEV_GOOD, UEV_WARN, UEV_ERROR, UEV_MUTED } uev_tone_t;
typedef enum { UEV_ALL = 0, UEV_WARNINGS, UEV_ERRORS, UEV_WIFI, UEV_DRIVE, UEV_FILTERS } uev_filter_t;
#define UEV_CAT_WIFI  1u             /* Wi-Fi and the clock sync over it */
#define UEV_CAT_DRIVE 2u             /* the car link, drives, their saves and the learning they feed */

typedef struct {
    power_state_t power;             /* power_state() now: the newest "Safe to turn off" line shows it */
    const char   *mark_ok, *mark_warn;   /* glyphs before that line (the app passes LV_SYMBOL_OK / LV_SYMBOL_WARNING) */
    bool          win_ok;            /* a drive window is known: the Drive filter shows the events inside it */
    int64_t       win_start_ms;      /* uptime at the drive's start */
    int32_t       win_dur_s;
} uev_ctx_t;

typedef struct {
    int        ring;                 /* index in the ring */
    int        age;                  /* 0 = the newest event in the ring */
    uev_tone_t tone;
    bool       save_line;            /* a "Safe to turn off" entry */
    char       when[16];             /* "0:42", "1:02:03" */
    char       text[UEV_LINE];       /* the line after the time ("Saved", the live power state, or the event text) */
    char       line[UEV_LINE];       /* when + " " + text, cut on a UTF-8 boundary */
} uev_row_t;

/* "m:ss" below an hour, "h:mm:ss" from one hour on; a negative time reads "0:00". Returns strlen. */
size_t      uev_uptime(char *b, size_t n, int64_t uptime_ms);
/* strlcpy that never cuts a UTF-8 sequence in half (a cut text stays valid for the font renderer) */
void        uev_copy(char *dst, size_t n, const char *src);
unsigned    uev_category(const char *text);
/* the ring indices newest first; returns how many (count clamped to EVENT_MAX) */
int         uev_order(uint8_t head, uint8_t count, int idx[EVENT_MAX]);
bool        uev_in_drive(int64_t ev_up_ms, int64_t start_up_ms, int32_t dur_s);
bool        uev_match(const sys_event_t *e, uev_filter_t f, const uev_ctx_t *c);
/* the rows that pass the filter, newest first, at most max; returns how many */
int         uev_rows(const sys_event_t ring[EVENT_MAX], uint8_t head, uint8_t count, uev_filter_t f, const uev_ctx_t *c,
                     uev_row_t *out, int max);
uint32_t    uev_session(const char *text);           /* "Drive s000123 saved (..)" / "Drive log s000123 deleted" -> 123, else 0 */
const char *uev_level_name(event_level_t l);         /* "Information", "Warning", "Error" */
const char *uev_filter_name(uev_filter_t f);         /* the chip labels */
/* the S3 footer: "pattern 7  novelty 0.8  (9/412)", or "pattern: first window pending" before the first window */
void        uev_pattern(char *b, size_t n, uint32_t windows, int16_t id, float novelty, uint8_t used);
/* the clock time of an event: now's epoch less the uptime since it (valid only while the clock is known) */
int64_t     uev_epoch_ms(int64_t ev_up_ms, int64_t now_up_ms, int64_t now_epoch_ms);
/* a cheap fingerprint of what the rows show: the app reformats only when it changes */
uint32_t    uev_sig(const sys_event_t ring[EVENT_MAX], uint8_t head, uint8_t count, power_state_t power, uint32_t pattern_windows);
