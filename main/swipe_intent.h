/* swipe_intent: a tiny on-device learner that predicts, about 40 ms after touch-down, whether a launcher gesture will end
 * on the next page, the previous page or stay put (tap / cancelled drag). Whether the firmware may act on it is decided
 * by the OS feature gate (ui_metrics.h): Smart swipe registers there as class UM_SWIPE with the learner policy.
 *
 * Model: online-sequential extreme learning machine (OS-ELM): 7 features -> 16 tanh units with fixed random input weights
 * (regenerated from a 32-bit seed on every use, never stored) -> 3 linear outputs learned by recursive least squares
 * with forgetting. A page-transition prior (uint8 counts) says which page to keep warm while the user rests.
 *
 * The module never acts. It only answers which page is likely next and whether it has learned enough to be used
 * (si_warm). The firmware acts (refresh a picture early, start the snap at lift) only on swipes the OS gate puts in the
 * feature's arm. The firmware keeps the page under the finger 1:1.
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
#define SI_WORDS       226    /* opaque storage, 4-byte words; the .c checks it matches the real layout (904 B) */

enum { SI_NEXT = 0, SI_PREV = 1, SI_STAY = 2, SI_NONE = -1 };

/* Opaque and caller-owned: `static si_t g_si;`. Costs nothing over a visible struct and keeps fields private. */
typedef struct si { uint32_t priv_[SI_WORDS]; } si_t;

typedef struct {
    int8_t  cls;      /* SI_NEXT / SI_PREV / SI_STAY, or SI_NONE if no prediction (yet) */
    int8_t  dest;     /* the page that class means from the start page, -1 if none */
    uint8_t conf;     /* top linear output minus the runner-up, 0..255 for 0..1 (the margin, not a probability) */
    bool    valid;    /* si_move: true only on the call that made it; si_end: true if any prediction exists */
} si_pred_t;

typedef struct {
    uint16_t acc_pm;             /* recent accuracy of the 40 ms prediction over all gestures, per mille */
    uint32_t updates;            /* RLS updates since init */
    uint16_t resets;             /* numerical-safety resets of P */
} si_metrics_t;

/* lifecycle */
void      si_init(si_t *self, uint8_t n_pages, uint32_t seed);   /* seed != 0 fixes the hidden layer (and the A/B coin) */

/* one gesture: begin at touch-down, move per touch sample, end at lift, settle when the page rests */
void      si_begin(si_t *self, uint32_t t_ms, int16_t x, int16_t y, uint8_t page);
si_pred_t si_move(si_t *self, uint32_t t_ms, int16_t x, int16_t y);          /* valid once, at >= SI_T_PREDICT ms */
si_pred_t si_end(si_t *self, uint32_t t_ms, int16_t x, int16_t y, uint8_t fresh_mask);   /* lift: returns the prediction
                                                       in force; bit p of fresh_mask = page p's picture is fresh now */
void      si_settle(si_t *self, uint32_t t_ms, uint8_t final_page);          /* learn, measure, draw the next coin */
void      si_abort(si_t *self);                                              /* gesture interrupted: drop it */

/* what the firmware may use */
bool      si_warm(const si_t *self);       /* enough updates (30) for its predictions to be used */
int8_t    si_hint_page(const si_t *self, uint8_t page, uint8_t *pct);   /* the prior's likeliest next page from `page` */

/* the learner's own numbers (the A/B verdict is the OS gate's: um_line) */
void      si_metrics_get(const si_t *self, si_metrics_t *m);

/* persistence (NVS blob, versioned, checksummed): the model and the page prior. fw_tag is written, not read. */
#define SI_BLOB_BYTES 840
size_t    si_save(const si_t *self, void *buf, size_t cap, uint32_t fw_tag);
bool      si_load(si_t *self, const void *buf, size_t n, uint32_t fw_tag);

#ifdef __cplusplus
}
#endif
#endif
