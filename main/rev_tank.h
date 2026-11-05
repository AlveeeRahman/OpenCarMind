/* CarMind R.E.V tank level learner. PROPOSAL for OpenCarMind main/rev_tank.c/.h (rev-v1); pure C, no ESP-IDF, host-tested.
 * Python mirror and validation: carmind-ml/src/carmindformer/regen_tank.py; design: docs/carmind-rev/REV_FRAMEWORK.md,
 * section "Tank level learner".
 *
 * Learns C_R, the tank's capacity in RAW counted litres per 100 % of the fuel level sender (PID 0x2F), from the level against
 * the litres the board counts (the same raw litres rev_cal_row() integrates, before the fill-up factor c). True litres are
 * c x raw litres, so the true capacity is c x C_R. Range needs no c: raw litres left / raw litres per km.
 *
 * Model between fill-ups: level = a_k - (100 / C_R) x raw_litres + noise, one intercept a_k per segment. Each segment keeps
 * Welford sums of (x = raw litres, y = level); closed segments fold into pooled sums with forgetting per litre; the slope is
 * beta = -Pxy / Pxx and C_R = 100 / beta. Intercepts drop out, so level changes while the engine is off never enter the slope.
 *
 * Fill-ups: at each restart (new drive, or engine off >= 60 s) the mean of the first K level samples is compared with the
 * mean of the last K before the stop. A jump >= REV_TANK_FILL % is a fill-up SUGGESTION for the desktop app. Suggestions never
 * feed rev_cal's c: their litres are computed from c and C_R, so feeding them back would make c a function of itself. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define REV_TANK_LO         10.0f    /* % : level band used for the slope (reserve and end stop below)                  */
#define REV_TANK_HI         95.0f    /* % : above it the sender rests on its top stop for several litres after a full fill */
#define REV_TANK_TOP        98.5f    /* % : an anchor at or above this is "on the top stop"                               */
#define REV_TANK_K          6        /* level samples per anchor (about 1 min at the Sentra's 10.8 s poll of 0x2F)        */
#define REV_TANK_FILL       8.0f     /* % : restart jump that is a fill-up                                                */
#define REV_TANK_BREAK      8.0f     /* % : fall while parked that closes the segment (fuel taken, sender fault). Not lower: */
                                     /*     random parking offsets of 2-3 % would break most drives (regen_tank.py probe)  */
#define REV_TANK_SIGMAS     4.0f     /* the fill and break thresholds rise to this many RMS of ordinary restart jumps      */
#define REV_TANK_STOP_S     60.0f    /* s : engine off this long inside a drive counts as a restart                       */
#define REV_TANK_MIN_SEG_L  0.5f     /* raw litres a segment must span to be folded                                       */
#define REV_TANK_MEM_L      400.0f   /* forgetting: a folded segment's weight falls by exp(-litres / MEM_L)               */
#define REV_TANK_READY_L    10.0f    /* ready: raw litres burned while the level was in the band ...                      */
#define REV_TANK_READY_SPAN 20.0f    /* ... covering this % of the sender span ...                                        */
#define REV_TANK_READY_SE   0.02f    /* ... and the AR(1)-inflated relative standard error of C_R under this              */
#define REV_TANK_KM_MEM     50.0f    /* km : memory of the recent consumption used for range                             */
#define REV_TANK_SUGGEST    4        /* fill-up suggestions kept for the app                                              */

typedef struct { uint32_t n; float mx, my, cxx, cxy, cyy; } rev_wf_t;

typedef struct {
    uint32_t seq;          /* increasing; the app acknowledges by seq */
    uint32_t session;      /* the drive that started after the fill */
    float    before, after;/* anchors, % */
    float    raw_l;        /* jump x C_R / 100, plus the learned top-stop litres when `after` is on the top stop */
    uint8_t  ready;        /* C_R was ready when the fill was seen */
    uint8_t  top;          /* after >= REV_TANK_TOP */
} rev_tank_fill_t;

typedef struct {
    /* ---- saved in rev_state.bin */
    float    pxx, pxy, pyy;          /* pooled within-segment sums of the closed segments */
    float    band_l;                 /* raw litres burned in band, closed segments, with the same forgetting */
    float    srr, srr1;              /* residual sum of squares and lag-1 products: how much memory the noise has */
    uint32_t dof, segs, fills, breaks;  /* breaks: level fell >= REV_TANK_BREAK while parked */
    rev_wf_t seg;                    /* the open segment */
    float    x, seg_l, bx0, bx1;     /* raw litres since the segment opened; litres at its first/last in-band sample */
    uint8_t  has_bx, has_anchor, tail_n, on_top;
    float    tail[REV_TANK_K];       /* last K level samples: the anchor before a stop */
    float    prev_anchor;
    float    top_l, top_run_l;       /* learned litres hidden above the top stop; litres burned since the last full fill */
    uint32_t top_n;
    float    cons_l, cons_km;        /* recent raw litres and km, forgetting per km */
    float    jump_ms;                /* mean square of ordinary restart jumps (parking slope, slosh at the anchors) */
    uint32_t n_jump;
    rev_tank_fill_t sug[REV_TANK_SUGGEST];
    uint8_t  sug_n;
    uint32_t sug_seq;
    /* ---- per drive, reset by rev_tank_restart() and after a load */
    float    head_x[REV_TANK_K], head_l[REV_TANK_K];
    uint8_t  head_n, judging, running, has_r;
    float    off_s, r_prev;
} rev_tank_t;

typedef struct {
    bool  have;          /* a capacity exists (slope > 0) */
    bool  ready;         /* show litres left and range */
    float cap_raw_l;     /* C_R */
    float rel_se;        /* relative standard error of C_R, inflated for correlated noise */
    float band_l;        /* raw litres burned in band */
    float span_pct;      /* 100 x band_l / C_R */
    float level;         /* de-sloshed level, % */
    float left_raw_l;    /* raw litres left to a 0 % reading (multiply by c for litres) */
    float range_km;      /* < 0 when unknown */
} rev_tank_out_t;

void rev_tank_init(rev_tank_t *t);
/* Forget everything learned (new car, sender replaced). */
void rev_tank_reset(rev_tank_t *t);
/* Start of a drive, and after rev_state_load(): the next K level samples are judged against the last K. */
void rev_tank_restart(rev_tank_t *t);
/* Every OBD row: seconds since the last row, raw fuel rate (negative = unknown), speed (negative = unknown), engine running. */
void rev_tank_row(rev_tank_t *t, float dt_s, float raw_lph, float kph, bool engine_running);
/* Only on rows where PID 0x2F was polled and answered: the level in %. session is the current session id (for suggestions). */
void rev_tank_level(rev_tank_t *t, uint32_t session, float level_pct);
void rev_tank_get(const rev_tank_t *t, rev_tank_out_t *o);
/* The app has taken the suggestions up to and including seq. */
void rev_tank_ack(rev_tank_t *t, uint32_t seq);
/* Finite-value check for the loader. */
bool rev_tank_valid(const rev_tank_t *t);
