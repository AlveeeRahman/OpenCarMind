/* ids_model.h: on-device unsupervised "rotation" anomaly detector for the OpenCarMind net IDS.
 *
 * Generated in carmind-ml experiments/net_ids (DESIGN.md, UNSUPERVISED.md). Header-only, integer-only, no allocation, no locks,
 * no ESP-IDF calls: it is fed one 5 s window of counters at a time by the IDS service step and keeps
 * its own learned baseline. One ids_rot_t instance per head (BLE scan, Wi-Fi management).
 *
 * Inputs per window (all computed by the service from single-writer counters and three bitmaps of
 * IDS_ROT_BITS bits: "seen" this window, "seen twice" this window, "seen" last window):
 *   n_adv     adverts (or management frames) counted in the window
 *   set_bits  popcount(seen)
 *   new_bits  popcount(seen & ~prev)
 *   su_new    popcount(seen & ~twice & ~prev)   single-use new transmitters (rotation signature)
 *
 * Score (Q8, 256 = threshold), one-sided:
 *   static  = su_new / IDS_ROT_U_STATIC                               physics prior, never learned
 *   learned = max_f dir_f (x_f - med_f) / (Z_TH (1.4826 mad_f + floor_f))   after warm-up, su_new >= U_MIN
 *   features x: su_new (high is bad), churn = new/set (high is bad), adverts per address (LOW is bad)
 * Learning (frugal streaming median/MAD, Ma, Muthukrishnan and Sandler 2013): only on windows that do
 * not exceed, outside a block, with su_new < U_ELIG; medians are clamped to a plausible envelope, so a
 * flood can neither train the baseline nor drag it (poisoning guard).
 * Decision: block when >= 3 of the last 6 windows exceed, released IDS_ROT_HOLD windows after the last
 * exceedance; never during the first IDS_ROT_SHADOW windows (cold start, learns only).
 *
 * Fixed per-step cost: about 60 integer ops, 3 int64 multiplies. State: sizeof(ids_rot_t) = 40 bytes (host build).
 * No parameter below was fitted on any dataset: all are design priors (see UNSUPERVISED.md section 4).
 * Bit-exact against scripts/ids_ref.py; host test: tests/test_ids_model.c with tests/ids_vectors.csv.
 */
#ifndef IDS_MODEL_H
#define IDS_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#define IDS_ROT_BITS      1024u  /* bitmap size; 256 saturated in a 150-device crowd (results/scan) */
#define IDS_ROT_SHADOW    1440u  /* 2 h of windows: learn only, never block */
#define IDS_ROT_WARM       720u  /* learned part active after this many accepted updates */
#define IDS_ROT_U_STATIC    15   /* static prior: 15 single-use new transmitters per 5 s window */
#define IDS_ROT_U_MIN        3   /* learned part needs at least this much rotation evidence */
#define IDS_ROT_U_ELIG       8   /* windows with su_new >= 8 never update the baseline */
#define IDS_ROT_Z_TH         6   /* robust z threshold */
#define IDS_ROT_LR_SHIFT     7   /* learning step = max(mad, floor) / 128 after warm-up */
#define IDS_ROT_WARM_SHIFT   3   /* step = max(mad, floor) / 8 during warm-up, so the median converges */
#define IDS_ROT_PERSIST_K    3
#define IDS_ROT_PERSIST_N    6
#define IDS_ROT_HOLD       120u  /* 10 min */

/* feature scales: su_new Q4, churn Q8 (0..256), adverts/address Q4 */
#define IDS_ROT_NF 3
static const int32_t IDS_ROT_DIR[IDS_ROT_NF]      = { 1, 1, -1 };
static const int32_t IDS_ROT_FLOOR[IDS_ROT_NF]    = { 16, 13, 5 };          /* 1.0, 0.05, 0.3 */
static const int32_t IDS_ROT_CLAMP_LO[IDS_ROT_NF] = { INT32_MIN / 4, INT32_MIN / 4, 24 };   /* app >= 1.5 */
static const int32_t IDS_ROT_CLAMP_HI[IDS_ROT_NF] = { 64, 154, INT32_MAX / 4 };            /* su <= 4, churn <= 0.6 */

typedef struct {
    int32_t  med[IDS_ROT_NF];
    int32_t  mad[IDS_ROT_NF];
    uint32_t windows;          /* windows seen */
    uint32_t accepted;         /* baseline updates accepted */
    uint32_t last_exc;         /* window index of the last exceedance */
    uint8_t  hist;             /* exceedance bits of the last IDS_ROT_PERSIST_N windows */
    uint8_t  hist_n;
    uint8_t  init;
    uint8_t  blocking;
} ids_rot_t;

typedef struct {
    int32_t score_q8;          /* 256 = threshold */
    bool    exceed;
    bool    block;             /* block active after this window */
    bool    onset;             /* the block started on this window */
} ids_rot_out_t;

static inline void ids_rot_init(ids_rot_t *s)
{
    for (int i = 0; i < IDS_ROT_NF; i++) { s->med[i] = 0; s->mad[i] = IDS_ROT_FLOOR[i]; }
    s->windows = s->accepted = s->last_exc = 0;
    s->hist = s->hist_n = s->init = s->blocking = 0;
}

/* For a head that sees too few windows to ever finish a shadow period (the Wi-Fi head: about 4 windows
   per clock round): skip the cold start so the static prior acts from the first window. The learned part
   still waits for IDS_ROT_WARM accepted updates. */
static inline void ids_rot_init_static_only(ids_rot_t *s) { ids_rot_init(s); s->windows = IDS_ROT_SHADOW; }

static inline int32_t ids_rot_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int32_t ids_rot_sgn(int32_t v) { return (v > 0) - (v < 0); }
static inline int32_t ids_rot_abs(int32_t v) { return v < 0 ? -v : v; }

static inline void ids_rot_features(uint16_t n_adv, uint16_t set_bits, uint16_t new_bits, uint16_t su_new,
                                    int32_t x[IDS_ROT_NF])
{
    uint32_t sb = set_bits ? set_bits : 1u;
    x[0] = (int32_t)su_new * 16;
    x[1] = (int32_t)(((uint32_t)new_bits * 256u) / sb);
    x[2] = (int32_t)(((uint32_t)n_adv * 16u) / sb);
}

static inline int32_t ids_rot_score_q8(const ids_rot_t *s, const int32_t x[IDS_ROT_NF], uint16_t su_new)
{
    int32_t best = (int32_t)(((int32_t)su_new * 256) / IDS_ROT_U_STATIC);
    if (s->init && s->accepted >= IDS_ROT_WARM && su_new >= IDS_ROT_U_MIN) {
        for (int f = 0; f < IDS_ROT_NF; f++) {
            int64_t num = (int64_t)IDS_ROT_DIR[f] * (int64_t)(x[f] - s->med[f]) * 10000;
            int64_t den = (int64_t)IDS_ROT_Z_TH * ((int64_t)14826 * s->mad[f] + (int64_t)10000 * IDS_ROT_FLOOR[f]);
            int64_t q = (num * 256) / den;
            if (q > best) best = (int32_t)(q > INT32_MAX ? INT32_MAX : q);
        }
    }
    return best;
}

static inline void ids_rot_learn(ids_rot_t *s, const int32_t x[IDS_ROT_NF])
{
    if (!s->init) {
        for (int f = 0; f < IDS_ROT_NF; f++) s->med[f] = ids_rot_clamp(x[f], IDS_ROT_CLAMP_LO[f], IDS_ROT_CLAMP_HI[f]);
        s->init = 1;
    } else {
        for (int f = 0; f < IDS_ROT_NF; f++) {
            int32_t base = s->mad[f] > IDS_ROT_FLOOR[f] ? s->mad[f] : IDS_ROT_FLOOR[f];
            int32_t step = base >> (s->accepted >= IDS_ROT_WARM ? IDS_ROT_LR_SHIFT : IDS_ROT_WARM_SHIFT);
            if (step < 1) step = 1;
            s->med[f] += step * ids_rot_sgn(x[f] - s->med[f]);
            s->mad[f] += step * ids_rot_sgn(ids_rot_abs(x[f] - s->med[f]) - s->mad[f]);
            s->med[f] = ids_rot_clamp(s->med[f], IDS_ROT_CLAMP_LO[f], IDS_ROT_CLAMP_HI[f]);
            if (s->mad[f] < 1) s->mad[f] = 1;
        }
    }
    s->accepted++;
}

/* One bounded step per window. Call only while the radio of this head is on. */
static inline ids_rot_out_t ids_rot_step(ids_rot_t *s, uint16_t n_adv, uint16_t set_bits, uint16_t new_bits,
                                         uint16_t su_new)
{
    ids_rot_out_t o = { 0, false, false, false };
    int32_t x[IDS_ROT_NF];
    ids_rot_features(n_adv, set_bits, new_bits, su_new, x);
    o.score_q8 = ids_rot_score_q8(s, x, su_new);
    uint32_t i = s->windows++;
    bool eligible = su_new < IDS_ROT_U_ELIG && n_adv > 0;
    if (i < IDS_ROT_SHADOW) {                       /* cold start: learn, never block */
        if (eligible) ids_rot_learn(s, x);
        o.block = false;
        return o;
    }
    o.exceed = o.score_q8 > 256;
    s->hist = (uint8_t)(((s->hist << 1) | (o.exceed ? 1u : 0u)) & ((1u << IDS_ROT_PERSIST_N) - 1u));
    if (s->hist_n < IDS_ROT_PERSIST_N) s->hist_n++;
    if (o.exceed) s->last_exc = i;
    int ones = 0;
    for (uint8_t h = s->hist; h; h &= (uint8_t)(h - 1)) ones++;
    if (!s->blocking && ones >= IDS_ROT_PERSIST_K) { s->blocking = 1; o.onset = true; }
    if (s->blocking && i - s->last_exc > IDS_ROT_HOLD) s->blocking = 0;
    o.block = s->blocking != 0;
    if (!o.exceed && !s->blocking && eligible) ids_rot_learn(s, x);
    return o;
}

/* The user's "clear" on the banner: end the block now; the baseline is kept. */
static inline void ids_rot_clear(ids_rot_t *s) { s->blocking = 0; s->hist = 0; }

#endif /* IDS_MODEL_H */
