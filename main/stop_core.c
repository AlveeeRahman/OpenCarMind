/* Stop logging state machine: see stop_core.h. */
#include "stop_core.h"
#include <string.h>

void stop_init(stop_ctl_t *c) { memset(c, 0, sizeof *c); }

bool stop_active(const stop_ctl_t *c) { return c->st == STOP_LINK || c->st == STOP_CLOSE || c->st == STOP_SYNC; }

static unsigned enter(stop_ctl_t *c, stop_state_t st, int64_t now)
{
    c->st = st;
    c->t_state = now;
    if (st == STOP_CLOSE) c->tries = 0;
    if (st == STOP_READY) c->t_done = now;
    switch (st) {
    case STOP_LINK:  return STOP_DO_HALT;
    case STOP_CLOSE: return STOP_DO_HALT | STOP_DO_CLOSE;
    case STOP_SYNC:  return STOP_DO_HALT | STOP_DO_PAUSE | STOP_DO_SYNC;
    default:         return STOP_DO_HALT;
    }
}

static unsigned fail(stop_ctl_t *c, stop_why_t why, int64_t now)
{
    c->failed_at = c->st;
    c->why = why;
    c->st = STOP_FAILED;
    c->t_state = c->t_done = now;
    return STOP_DO_HALT;                       /* logging stays off: a failed stop never restarts the drive */
}

unsigned stop_request(stop_ctl_t *c, int64_t now_ms)
{
    if (c->st == STOP_FAILED) return stop_retry(c, now_ms);
    if (c->st != STOP_IDLE) return STOP_DO_HALT;   /* already stopping or stopped: the latch is asserted again */
    c->why = STOP_WHY_NONE;
    c->t_req = now_ms;
    c->t_done = 0;
    c->latched = true;
    return enter(c, STOP_LINK, now_ms);
}

unsigned stop_retry(stop_ctl_t *c, int64_t now_ms)
{
    if (c->st != STOP_FAILED) return c->st == STOP_IDLE ? 0 : STOP_DO_HALT;
    stop_state_t from = c->failed_at == STOP_CLOSE || c->failed_at == STOP_SYNC ? c->failed_at : STOP_LINK;
    c->why = STOP_WHY_NONE;
    c->t_req = now_ms;
    c->t_done = 0;
    return enter(c, from, now_ms);
}

unsigned stop_release(stop_ctl_t *c, int64_t now_ms)
{
    (void)now_ms;
    if (c->st == STOP_IDLE && !c->latched) return 0;
    stop_init(c);
    return STOP_DO_RELEASE;
}

unsigned stop_step(stop_ctl_t *c, const stop_in_t *in, int64_t now_ms)
{
    int64_t in_state = now_ms - c->t_state;
    switch (c->st) {
    case STOP_IDLE:
        return 0;
    case STOP_LINK:
        if (!in->poll_running) return enter(c, STOP_CLOSE, now_ms);
        if (in_state >= STOP_LINK_MS) return fail(c, STOP_WHY_LINK, now_ms);
        return STOP_DO_HALT;
    case STOP_CLOSE:
        if (in->poll_running) return enter(c, STOP_LINK, now_ms);            /* never expected: the latch holds it */
        if (!in->storage_ok) return fail(c, STOP_WHY_STORAGE, now_ms);
        if (!in->session_open && in->closed_ok) return enter(c, STOP_SYNC, now_ms);
        if (in_state >= STOP_CLOSE_MS) return fail(c, STOP_WHY_CLOSE, now_ms);
        c->tries++;
        return STOP_DO_HALT | STOP_DO_CLOSE;                                 /* the next bounded close attempt */
    case STOP_SYNC:
        if (in->poll_running) return enter(c, STOP_LINK, now_ms);
        if (in->session_open) return enter(c, STOP_CLOSE, now_ms);
        if (!in->storage_ok) return fail(c, STOP_WHY_STORAGE, now_ms);
        if (in->writers_paused && in->saved && in->ms_since_write >= STOP_QUIET_MS) return enter(c, STOP_READY, now_ms);
        if (in_state >= STOP_SYNC_MS) return fail(c, STOP_WHY_SYNC, now_ms);
        return STOP_DO_HALT | STOP_DO_PAUSE | (in->saved ? 0u : STOP_DO_SYNC);
    case STOP_READY:
        /* READY is re-checked every step: anything written since sends it back to SYNC */
        if (in->poll_running) return enter(c, STOP_LINK, now_ms);
        if (in->session_open) return enter(c, STOP_CLOSE, now_ms);
        if (!in->storage_ok) return fail(c, STOP_WHY_STORAGE, now_ms);
        if (!in->saved || !in->writers_paused || in->ms_since_write < STOP_QUIET_MS) return enter(c, STOP_SYNC, now_ms);
        return STOP_DO_HALT | STOP_DO_PAUSE;
    case STOP_FAILED:
        return STOP_DO_HALT;
    }
    return 0;
}

const char *stop_why_text(stop_why_t w)
{
    switch (w) {
    case STOP_WHY_LINK:    return "The car link did not stop";
    case STOP_WHY_CLOSE:   return "Drive files did not close, storage busy";
    case STOP_WHY_SYNC:    return "The storage kept writing";
    case STOP_WHY_STORAGE: return "No storage, nothing could be saved";
    default:               return "";
    }
}

const char *stop_state_text(const stop_ctl_t *c)
{
    switch (c->st) {
    case STOP_LINK:   return "Stopping the car link";
    case STOP_CLOSE:  return "Saving the drive";
    case STOP_SYNC:   return "Checking that everything is saved";
    case STOP_READY:  return "Logging stopped. Safe to unplug.";
    case STOP_FAILED: return "Not safe to unplug yet";
    default:          return "Logging";
    }
}
