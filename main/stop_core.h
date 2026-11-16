/* Stop logging: the state machine behind "Hold: End drive", Settings > Stop logging and Turn off on the ESP-Mosaico
 * (OpenCarMind EdgeOS 2.1.1 field fix). Pure C, no ESP-IDF: tests/host/test_stop.c drives it with injected failures.
 *
 * A stop request always ends in one of two states, each inside a bounded time:
 *   STOP_READY   verified safe to unplug: the poll loop has stopped, the drive files are closed and synced, nothing was
 *                written for STOP_QUIET_MS and the background writers are paused. Logging stays off (the latch) until
 *                the user starts it again or the board restarts (the next ignition cycle).
 *   STOP_FAILED  a step did not finish in its time. Logging stays off. The reason says which step, the caller shows it
 *                with a Try again button (stop_retry re-runs from that step).
 * Steps, in order: STOP_LINK (wait for the poll loop to leave the car link), STOP_CLOSE (drive summary, session files
 * closed), STOP_SYNC (writers paused, everything flushed, a quiet storage confirmed). A write seen in STOP_READY (a
 * battery-log row, a learned-state save) sends it back to STOP_SYNC, so READY is never shown over unsaved data. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define STOP_LINK_MS   15000   /* one ELM transaction is at most 6 s (VIN read), a DTC read a few more */
#define STOP_CLOSE_MS  10000   /* the drive summary, the session JSON and four file closes on a busy NAND */
#define STOP_SYNC_MS   10000   /* flush, then STOP_QUIET_MS without a write */
#define STOP_QUIET_MS  1500

typedef enum { STOP_IDLE = 0, STOP_LINK, STOP_CLOSE, STOP_SYNC, STOP_READY, STOP_FAILED } stop_state_t;

typedef enum {
    STOP_WHY_NONE = 0,
    STOP_WHY_LINK,       /* the poll loop did not stop in STOP_LINK_MS */
    STOP_WHY_CLOSE,      /* the drive files did not close in STOP_CLOSE_MS (storage busy) */
    STOP_WHY_SYNC,       /* writes kept coming or the save did not finish in STOP_SYNC_MS */
    STOP_WHY_STORAGE,    /* no storage mounted: nothing can be saved */
} stop_why_t;

/* what the firmware reports each step (the glue in mosaico_drive.c fills it) */
typedef struct {
    bool    poll_running;    /* elm_drive_active(): the poll loop is in a drive */
    bool    session_open;    /* a drive session file is still open for rows */
    bool    closed_ok;       /* the last close attempt returned success */
    bool    saved;           /* sdlog_saved(): nothing waits in a buffer */
    int64_t ms_since_write;  /* sdlog_ms_since_write() and the battery log's last write, the smaller one */
    bool    writers_paused;  /* battery log and learned-state saves acknowledged the pause */
    bool    storage_ok;      /* sdlog_ok() */
} stop_in_t;

/* actions for the glue, a bit set returned by stop_step */
#define STOP_DO_HALT    0x01u   /* latch the user stop: halt the poll loop, no new drive */
#define STOP_DO_CLOSE   0x02u   /* finish the drive summary and close the session files (bounded) */
#define STOP_DO_PAUSE   0x04u   /* pause the battery log and the learned-state saves */
#define STOP_DO_SYNC    0x08u   /* flush everything now */
#define STOP_DO_RELEASE 0x10u   /* start logging again: clear the latch, resume the writers */

typedef struct {
    stop_state_t st;
    stop_why_t   why;
    stop_state_t failed_at;  /* the step that failed (for stop_retry) */
    int64_t      t_req;      /* when the stop was asked for */
    int64_t      t_state;    /* when the current step began */
    int64_t      t_done;     /* when READY or FAILED was reached */
    uint8_t      tries;      /* close attempts in the current STOP_CLOSE */
    bool         latched;    /* logging is held off */
} stop_ctl_t;

void stop_init(stop_ctl_t *c);
/* a stop request (any path). Ignored while a stop is running or READY; from FAILED it is a retry. Returns the actions. */
unsigned stop_request(stop_ctl_t *c, int64_t now_ms);
/* Try again after STOP_FAILED: re-runs from the failed step. */
unsigned stop_retry(stop_ctl_t *c, int64_t now_ms);
/* the user starts logging again (READY or FAILED): back to IDLE, latch cleared. */
unsigned stop_release(stop_ctl_t *c, int64_t now_ms);
/* one step, every 100 ms while a stop runs and every second after: returns the actions to perform now */
unsigned stop_step(stop_ctl_t *c, const stop_in_t *in, int64_t now_ms);

bool stop_active(const stop_ctl_t *c);          /* LINK, CLOSE or SYNC */
const char *stop_state_text(const stop_ctl_t *c);   /* one line for the screen, 16 px, no jargon */
const char *stop_why_text(stop_why_t w);
