/* ESP-Mosaico Settings > Clock: the board side of setting the clock by hand (the pure rules are in clock_set.h).
 *
 * The LVGL task only validates and queues a request; the board task (internal-RAM stack, the only task that writes NVS for
 * the UI) refuses it during a drive, converts the local time in the chosen zone, calls settimeofday(), stores the zone the
 * way the desktop app's CONFIG does (NVS rconf/cfg, so a later CONFIG "timezone" still wins), marks the session meta
 * "clock":"manual" and writes clock.txt. A by-hand time clears time_synced, so the Wi-Fi sync keeps trying and replaces
 * it when it succeeds. A note in RTC memory keeps "set by hand" through a software reset (the RTC keeps the time). */
#pragma once
#include <stdbool.h>
#include "clock_set.h"
#include "status.h"

typedef enum { MCLK_IDLE = 0, MCLK_BUSY, MCLK_OK, MCLK_ERR_DRIVE, MCLK_ERR_TIME, MCLK_ERR_ZONE, MCLK_ERR_QUEUE } mclk_result_t;

void        mosaico_clock_boot(void);                         /* mosaico_board_init: after app_main's RTC check */
void        mosaico_clock_poll(void);                         /* any task, cheap: a Wi-Fi sync ends "set by hand" */
clock_src_t mosaico_clock_source(const app_status_t *st);
bool        mosaico_clock_known(const app_status_t *st);      /* any source but none: show the time, auto theme */
/* LVGL task: queue a set; false (and the result says why) when refused before queueing */
bool        mosaico_clock_request(int year, int month, int day, int hour, int minute, const char *tz);
mclk_result_t mosaico_clock_result(void);
const char *mosaico_clock_result_text(mclk_result_t r);
void        mosaico_clock_do_set(void);                       /* board task (MB_CLOCK_SET) */
