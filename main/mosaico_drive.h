/* ESP-Mosaico drive recorder and summaries (ESP32-S31 only).
 *
 * A 1 Hz task (priority 1, the lowest, core 0) samples the published snapshots during a drive into PSRAM ring buffers
 * (speed, rpm, fuel rate, economy, model class + confidence, brake and torque estimates), at most 240 points per
 * series: when a series fills, neighbouring points are averaged and the step doubles. When the drive ends (the car, or
 * the End drive control on the screen) the same task writes sessions/sNNNNNN.sum.json, appends one line to
 * obd/drives.csv and publishes the summary for the Drive summary app. The UI only reads copies (a mutex held for the
 * copy) and never waits for a file.
 *
 * End drive: the screen asks (mosaico_drive_end_request); this task stops the OBD poll (elm_request_stop, so the OBD
 * task closes the drive through its own end_drive path: model, R.E.V and session saved), builds the summary, then lets
 * the link run again (elm_resume): the next drive opens its own session. */
#pragma once
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>

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
} md_summary_t;

typedef struct {
    uint32_t gen;
    int      n;                        /* oldest first */
    struct { uint32_t session; float km, fuel_l, l100; } d[MD_HIST];
} md_history_t;

void mosaico_drive_start(void);
void mosaico_drive_end_request(void);          /* UI: the End drive control was held 1 s */
void mosaico_drive_start_request(void);        /* UI: Start drive */
bool mosaico_drive_ending(void);               /* the end sequence is running */
void mosaico_drive_get_summary(md_summary_t *out);
void mosaico_drive_get_history(md_history_t *out);
void mosaico_drive_history_request(void);
bool mosaico_drive_take_auto_open(void);
void mosaico_drive_live(int *brake_events, int *hard_brakes);   /* the drive being recorded, 0 when none */       /* true once after an End drive: open the summary app */
