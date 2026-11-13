/* ESP-Mosaico drive recorder and summaries (ESP32-S31 only).
 *
 * A 1 Hz task (priority 1, the lowest, core 0) samples the published snapshots during a drive into PSRAM ring buffers
 * (speed, rpm, fuel rate, economy, model class + confidence, brake and torque estimates), at most 240 points per
 * series: when a series fills, neighbouring points are averaged and the step doubles. When the drive ends (the car, or
 * the End drive control on the screen) the same task writes sessions/sNNNNNN.sum.json, appends one line to
 * obd/drives.csv and publishes the summary for the Drive summary app. The UI only reads copies (a mutex held for the
 * copy) and never waits for a file.
 *
 * End drive / Stop logging / Turn off (OpenCarMind EdgeOS 2.1.1): the screen asks (mosaico_drive_end_request); this task
 * runs the stop state machine (stop_core.h): the OBD poll is latched off (elm_user_stop, so the OBD task closes the drive
 * through its own end_drive path), the summary is built, the session files are closed and synced, the battery log is
 * paused and a quiet storage is confirmed. It ends READY ("Logging stopped. Safe to unplug.") or FAILED with the
 * reason, in bounded time. Logging stays off until Start logging or a restart: the car still answering does not start a
 * new drive (2.1.0 resumed the link at once, so the drive started again). */
#pragma once
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include "ui_events_logic.h"

#define MD_PTS 240
#define MD_HIST 20

typedef struct {
    uint32_t gen, session;
    bool     ok;
    int64_t  start_epoch_ms;           /* 0 = no clock */
    int64_t  start_up_ms;              /* uptime at the start (this boot): the drive's window in the recent-events log */
    int32_t  dur_s;
    float    km, fuel_l, l100, avg_kph, max_kph, idle_pct;
    float    cls_share[4];             /* stopped, braking, accelerating, cruising: share of the classified time */
    int      brake_events, hard_brakes;
    int      n;                        /* points per series */
    float    step_s;                   /* seconds per point */
    float    spd[MD_PTS], fuel[MD_PTS];
    int8_t   cls[MD_PTS];              /* -1 = none */
    bool     past;                     /* read back from its files after a restart: no curves, start_up_ms of that boot */
    int      nev;                      /* the drive's own events, oldest first (newest UEV_DRIVE_EVENTS kept) */
    sys_event_t ev[UEV_DRIVE_EVENTS];
} md_summary_t;

typedef struct {
    uint32_t gen;
    int      n;                        /* oldest first */
    struct { uint32_t session; float km, fuel_l, l100; } d[MD_HIST];
} md_history_t;

/* Stop logging (stop_core.h): the state for the screen */
typedef struct {
    uint8_t  state;                    /* stop_state_t */
    bool     turn_off;                 /* asked by Turn off: the screen goes dark once READY */
    bool     requested;                /* a request is queued for the drive task */
    uint32_t secs;                     /* since the request (to READY or FAILED once there) */
    uint32_t since_done_ms;            /* since READY or FAILED */
    char     text[40];                 /* stop_state_text */
    char     why[48];                  /* stop_why_text, FAILED only */
} md_stop_t;

void mosaico_drive_start(void);
void mosaico_drive_end_request(void);          /* UI: Hold: End drive, or Stop logging: stop and verify (stop_core.h) */
void mosaico_drive_turn_off_request(void);     /* UI: Turn off: the same stop, then the screen dark and no adapter search */
void mosaico_drive_stop_retry(void);           /* UI: Try again after a failed stop */
void mosaico_drive_start_request(void);        /* UI: Start logging: the latch comes off, the link may start a drive */
bool mosaico_drive_ending(void);               /* a stop is running */
bool mosaico_drive_stopped(void);              /* logging is held off by a stop (READY or FAILED) */
void mosaico_drive_stop_get(md_stop_t *out);
void mosaico_drive_get_summary(md_summary_t *out);
void mosaico_drive_get_history(md_history_t *out);
void mosaico_drive_history_request(void);
bool mosaico_drive_take_auto_open(void);
void mosaico_drive_live(int *brake_events, int *hard_brakes);   /* the drive being recorded, 0 when none */       /* true once after an End drive: open the summary app */
