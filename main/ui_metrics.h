/* ui_metrics: the Smart UI measurement surface (LOCKED: predictor code may read it, never change it or its thresholds).
 *
 * One event stream for every touch interaction on the Mosaico, one latency per event (finger lift -> page settled),
 * per-class rolling histograms, and the A/B gate that decides, per class, whether a prefetcher may stay.
 *
 * Classes: UM_SWIPE (launcher page swipe), UM_APP (app-icon tap -> the app screen), UM_BACK (Back chevron -> launcher),
 * UM_CONTROL (an in-app button, toggle, segment, slider, dropdown or roller), UM_LIST (a drag that scrolls a list).
 *
 * Definitions (the firmware glue in mosaico_display.c feeds these; times in ms from esp_timer):
 *   press   the first touch read that reports the finger down.
 *   lift    the first touch read that reports it up, capped at the last "down" read + one read period
 *           (UM_READ_MS): if the LVGL task was busy (building a screen at press), the read that sees the lift comes
 *           late, and the cap keeps that delay inside the measured latency instead of hiding it.
 *   frame   LV_EVENT_RENDER_READY (LVGL handed the frame's last area to the panel driver; that area's DMA, at most one
 *           40-line buffer = about 2 ms, is not waited for) or one launcher blit frame (the slide pictures).
 *   settled the first poll at which, since the lift: no invalidated area is waiting to be drawn, the launcher is not
 *           in blit mode and its tileview has no animation, the finger is up, and no frame came out for UM_QUIET_MS =
 *           2 refresh periods (2 x 16 ms). The settle time is the time of the last frame before that quiet gap, or
 *           the lift if no frame came after it. Latency = settle - lift. No settle within UM_TIMEOUT_MS: a timeout
 *           (latency UM_TIMEOUT_MS). A new touch before the settle ends the event there: the user's touch stops a
 *           scroll throw or a slide, so latency = that press - lift, flagged UM_F_INTERRUPTED (a lower bound: the
 *           page would have taken at least that long; counted, not dropped, so fast repeated scrolls stay in).
 *   ready   the arm's prefetcher had the event's target built at lift and the target was the one the press predicted.
 *
 * Gate (per class in the family mask, i.e. classes that have a prefetch action; owner rule, 2026-11-11):
 *   after UM_WARMUP events of the class (all A0), each event is A0 or A1 at random (50/50). With n >= UM_MIN_PER_ARM
 *   per arm: OFF if ready_A1 <= 80 % or p95_A1 >= p95_A0; KEEP if the one-sided Mann-Whitney z (A1 faster) passes its
 *   Holm-corrected critical value (z > 1.645 for a family of one) and the two conditions hold; otherwise sample on to
 *   UM_MAX_PER_ARM per arm, then decide (no z pass = OFF). Holm (1979) step-down over the family: the classes' z sorted
 *   high to low, the i-th (0-based) needs z > z_crit(alpha / (m - i)), m = family size, stop at the first failure;
 *   classes still short of data rank last. A decided class keeps its verdict and its z; KEEP = A1 on every event,
 *   OFF = A0 on every event. A new firmware image (fw tag) clears the verdicts and the A/B data.
 *
 * C99, no heap, no platform includes: the caller owns the structs and the clock. Not thread-safe (LVGL task only). */
#ifndef UI_METRICS_H
#define UI_METRICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { UM_SWIPE = 0, UM_APP, UM_BACK, UM_CONTROL, UM_LIST, UM_NCLASS, UM_NONE = 0xFF };
enum { UM_A0 = 0, UM_A1 = 1 };
enum { UM_LEARNING = 0, UM_KEEP = 1, UM_OFF = 2, UM_WARMUP = 3 };              /* per-class gate state */
enum { UM_R_NONE = 0, UM_R_PASS, UM_R_SLOWER, UM_R_UNCERTAIN, UM_R_READY_LOW };

#define UM_READ_MS      16      /* touch read period (LV_DEF_REFR_PERIOD) */
#define UM_REFR_MS      16      /* display refresh period */
#define UM_QUIET_MS     (2 * UM_REFR_MS)
#define UM_TIMEOUT_MS   3000
#define UM_NB1          128     /* histogram bins: 128 of 8 ms (0..1023 ms), then */
#define UM_BIN_MS       8
#define UM_NB2          64      /* 64 of 32 ms (1024..3071 ms); the last bin also holds anything slower */
#define UM_BIN2_MS      32
#define UM_NB           (UM_NB1 + UM_NB2)
#define UM_HALVE_AT     1024    /* an arm's bins halve when it reaches this many samples (recency decay) */
#define UM_WARMUP       10
#define UM_MIN_PER_ARM  50
#define UM_MAX_PER_ARM  150
#define UM_READY_PM     800     /* ready must be ABOVE this, per mille */

/* ---------- binned histograms and the rank test (shared with swipe_intent.c) ---------- */
uint32_t um_hist_n(const uint16_t *h, int nb);
uint16_t um_hist_pct(const uint16_t *h, int nb, int bin_ms, unsigned pct);  /* nearest-rank percentile, bin centre */
int      um_lat_bin(uint32_t ms);                      /* the Smart UI latency bins (UM_NB): 8 ms to 1 s, then 32 ms */
uint16_t um_lat_centre(int b);
uint16_t um_lat_pct(const uint16_t *h, unsigned pct);  /* percentile over the UM_NB latency bins, bin centre */
int16_t  um_mw_z100(const uint16_t *a, const uint16_t *b, int nb);          /* Mann-Whitney z x 100, > 0: a is faster */
int16_t  um_holm_crit100(int k);                       /* z_crit(0.05 / k) x 100, k = 1..8 (k > 8 uses k = 8) */
/* Holm step-down over m tests given as z x 100: sig[i] = 1 if test i is significant at family-wise 5 %.
   has[i] = 0: test i has no data yet (ranks last, never significant). */
void     um_holm(const int16_t *z100, const uint8_t *has, int m, uint8_t *sig);

/* ---------- settle detector (one event at a time) ---------- */
enum { UM_S_IDLE = 0, UM_S_WAIT, UM_S_SETTLED, UM_S_TIMEOUT };
typedef struct { uint8_t phase, dirty; uint32_t t_lift, t_out; } um_settle_t;
void um_settle_lift(um_settle_t *s, uint32_t t_lift);
void um_settle_invalidated(um_settle_t *s);            /* LV_EVENT_INVALIDATE_AREA */
void um_settle_render_start(um_settle_t *s);           /* LV_EVENT_RENDER_START: the waiting areas are being drawn */
void um_settle_frame(um_settle_t *s, uint32_t t);      /* RENDER_READY or a blit frame */
/* busy: blit mode, a tileview animation or the finger down. Returns UM_S_WAIT, UM_S_SETTLED or UM_S_TIMEOUT
   (latency in *lat_ms for the last two; the detector goes back to idle) */
int  um_settle_poll(um_settle_t *s, uint32_t t, bool busy, uint32_t *lat_ms);
void um_settle_abort(um_settle_t *s);

/* ---------- the event stream ---------- */
typedef struct {
    uint32_t t_press;        /* ms since boot */
    uint16_t press_ms;       /* lift - press */
    uint16_t lat_ms;         /* settle - lift (UM_TIMEOUT_MS on a timeout) */
    uint16_t target;         /* class-specific id: app index, return page, (app << 8 | control), destination page */
    uint8_t  cls, arm;
    uint8_t  flags;          /* UM_F_* */
    uint8_t  pad_[3];
} um_event_t;
enum { UM_F_READY = 1, UM_F_TIMEOUT = 2, UM_F_BUILT = 4, UM_F_REPLAY = 8, UM_F_INTERRUPTED = 16 };

/* ---------- per-class statistics and the gate ---------- */
typedef struct {
    uint8_t  state, reason;
    int16_t  z100;
    uint16_t p95_0, p95_1, ready_pm, n0, n1;
} um_verdict_t;

/* one class's counters, together so that an event touches one cache line of them (plus one histogram bin) */
typedef struct {
    uint16_t n[2], rdy[2];              /* samples and ready samples per arm (A0, A1) */
    uint16_t seen, pad_;                /* events since the last reset of the class (warm-up counter, saturates) */
    uint32_t total, timeouts, dropped, interrupted;
    um_verdict_t v;
} um_cls_t;

typedef struct {
    uint16_t hist[UM_NCLASS][2][UM_NB];
    um_cls_t k[UM_NCLASS];
    uint32_t rng, fw_tag;
    uint8_t  family;                    /* bit c: class c has a prefetch action and is gated */
    uint8_t  pad_[3];
} um_t;

typedef struct { uint16_t n, p50, p95, ready_pm; } um_stats_t;

void    um_init(um_t *m, uint32_t seed, uint8_t family, uint32_t fw_tag);
void    um_reset_verdicts(um_t *m);                     /* every class back to warm-up, A/B data cleared */
uint8_t um_arm(um_t *m, uint8_t cls);                   /* the arm of the next event of cls (draws the coin) */
void    um_record(um_t *m, uint8_t cls, uint8_t arm, uint32_t lat_ms, bool ready, uint8_t flags);   /* UM_F_TIMEOUT / UM_F_INTERRUPTED */
void    um_dropped(um_t *m, uint8_t cls);                 /* an event that cannot be measured (a tap that opened nothing) */
bool    um_gate_step(um_t *m);                          /* decide what can be decided; true if a verdict changed */
void    um_stats(const um_t *m, uint8_t cls, uint8_t arm, um_stats_t *out);
/* pure decision for one class from its numbers and whether Holm let its z through */
uint8_t um_decide(uint16_t n0, uint16_t n1, uint16_t p95_0, uint16_t p95_1, uint16_t ready1_pm, bool z_sig,
                  uint8_t *reason);
const char *um_class_name(uint8_t cls);
const char *um_state_name(uint8_t st);

/* persistence: the verdicts and the gated classes' A/B histograms; a different fw_tag clears both */
#define UM_BLOB_MAX (12 + UM_NCLASS * (int)sizeof(um_verdict_t) + 2 * UM_NCLASS + UM_NCLASS * (2 * UM_NB * 2 + 8) + 4)
size_t  um_save(const um_t *m, void *buf, size_t cap);
bool    um_load(um_t *m, const void *buf, size_t n);    /* call after um_init (it carries the current fw tag) */

#ifdef __cplusplus
}
#endif
#endif
