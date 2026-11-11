/* swipe_intent: a tiny on-device learner that predicts, about 40 ms after touch-down, whether a launcher gesture will end
 * on the next page, the previous page or stay put (tap / cancelled drag), plus the runtime A/B gate that decides whether
 * the firmware may act on it.
 *
 * Model: online-sequential extreme learning machine (OS-ELM): 7 features -> 16 tanh units with fixed random input weights
 * (regenerated from a 32-bit seed on every use, never stored) -> 3 linear outputs learned by recursive least squares
 * with forgetting. A page-transition prior (uint8 counts) says which page to keep warm while the user rests.
 *
 * The module never acts. It only answers: which page is likely next, and is this gesture in the arm where the firmware
 * may refresh a picture early and start the snap at lift (si_acts). The firmware keeps the page under the finger 1:1.
 *
 * C99, no heap, no globals, no platform includes: the caller owns one si_t (static storage) and the clock (ms).
 * Not thread-safe: call every method from one task (the LVGL task on the Mosaico). */
#ifndef SWIPE_INTENT_H
#define SWIPE_INTENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SI_MAX_PAGES   8      /* pages the prior and the fresh mask can describe */
#define SI_H           16     /* hidden tanh units */
#define SI_K           3      /* outputs: next, previous, stay */
#define SI_D           7      /* input features (a bias input is added inside) */
#define SI_T_PREDICT   40     /* ms after touch-down at which the prediction is made */
#define SI_MIN_PER_ARM 50     /* completed swipes per arm before the gate may decide */
#define SI_WORDS       420    /* opaque storage, 4-byte words; the .c checks it matches the real layout */

enum { SI_NEXT = 0, SI_PREV = 1, SI_STAY = 2, SI_NONE = -1 };
enum { SI_LEARNING = 0, SI_KEEP = 1, SI_OFF = 2 };                         /* gate state */
enum { SI_R_NONE = 0, SI_R_PASS, SI_R_SLOWER, SI_R_UNCERTAIN, SI_R_READY_LOW, SI_R_RECHECK_FAIL };

/* Opaque and caller-owned: `static si_t g_si;`. Costs nothing over a visible struct and keeps fields private. */
typedef struct si { uint32_t priv_[SI_WORDS]; } si_t;

typedef struct {
    int8_t  cls;      /* SI_NEXT / SI_PREV / SI_STAY, or SI_NONE if no prediction (yet) */
    int8_t  dest;     /* the page that class means from the start page, -1 if none */
    uint8_t conf;     /* top linear output minus the runner-up, 0..255 for 0..1 (the margin, not a probability) */
    bool    valid;    /* si_move: true only on the call that made it; si_end: true if any prediction exists */
} si_pred_t;

typedef struct {
    uint8_t  state, reason;      /* SI_LEARNING / SI_KEEP / SI_OFF and why */
    uint16_t n_a, n_b;           /* completed swipes with a latency sample, per arm (A = learner acts, B = shadow) */
    uint16_t p95_a_ms, p95_b_ms; /* lift -> settled, 95th percentile per arm (0 if no samples) */
    uint16_t ready_a_pm, ready_b_pm;  /* per mille of completed swipes whose predicted page was right and fresh at lift */
    int16_t  z100;               /* Mann-Whitney z x 100, > 0 when arm A is faster */
    uint16_t acc_pm;             /* recent accuracy of the 40 ms prediction over all gestures, per mille */
    uint32_t updates;            /* RLS updates since init */
    uint16_t resets;             /* numerical-safety resets of P */
} si_metrics_t;

/* lifecycle */
void      si_init(si_t *self, uint8_t n_pages, uint32_t seed);   /* seed != 0 fixes the hidden layer (and the A/B coin) */
void      si_retest(si_t *self);                                 /* clear gate and metrics, back to LEARNING; keeps the model */

/* one gesture: begin at touch-down, move per touch sample, end at lift, settle when the page rests */
void      si_begin(si_t *self, uint32_t t_ms, int16_t x, int16_t y, uint8_t page);
si_pred_t si_move(si_t *self, uint32_t t_ms, int16_t x, int16_t y);          /* valid once, at >= SI_T_PREDICT ms */
si_pred_t si_end(si_t *self, uint32_t t_ms, int16_t x, int16_t y, uint8_t fresh_mask);   /* lift: returns the prediction
                                                       in force; bit p of fresh_mask = page p's picture is fresh now */
void      si_settle(si_t *self, uint32_t t_ms, uint8_t final_page);          /* learn, measure, draw the next coin */
void      si_abort(si_t *self);                                              /* gesture interrupted: drop it */

/* what the firmware may do */
bool      si_acts(const si_t *self);       /* the current (or next, between gestures) gesture is in arm A */
int8_t    si_hint_page(const si_t *self, uint8_t page, uint8_t *pct);   /* the prior's likeliest next page from `page` */

/* measurement and the gate */
void      si_metrics_get(const si_t *self, si_metrics_t *m);
uint8_t   si_metrics_decide(const si_metrics_t *m, uint8_t *reason);   /* pure: SI_LEARNING (wait), SI_KEEP or SI_OFF */
int       si_metrics_line(const si_t *self, char *buf, size_t n);      /* "Smart swipe: on, p95 212 ms, ready 86 %" */

/* persistence (NVS blob, versioned, checksummed). A different fw_tag keeps the model but re-runs the A/B test. */
#define SI_BLOB_BYTES 840
size_t    si_save(const si_t *self, void *buf, size_t cap, uint32_t fw_tag);
bool      si_load(si_t *self, const void *buf, size_t n, uint32_t fw_tag);

#ifdef __cplusplus
}
#endif
#endif
