/* CarMind R.E.V: the learners that run on the board. Pure C, no ESP-IDF, so the host tests cover them (tests/host/test_rev_learn.c).
 *
 * (a) Fuel head. The R.E.V network's 64-value feature z is fixed on the board; only a new linear read-out w (64 weights plus a
 *     bias, phi = [z, 1]) learns. It is fitted to this car's own fuel rate, with z computed as if MAF were missing, so that the
 *     estimate is ready for the moment MAF drops out. The fit is ridge regression with exponential forgetting and a pull toward
 *     the factory head w0:
 *         w* = argmin_w  sum_t lambda^(T-t) (y_t - phi_t.w)^2 + alpha |w - w0|^2
 *     whose normal equations are (A + alpha I) w* = b + alpha w0, with A = sum lambda^(T-t) phi phi^T and b = sum lambda^(T-t) phi y.
 *     Only A and b are kept (constant memory), plus a ring of the most recent REV_HOLDOUT pairs that is held out of A and b to judge
 *     a candidate: it replaces the live weights only if it beats them on that ring by REV_GATE and stays within REV_DRIFT times
 *     |w0| of w0; if the live weights fall REV_RESET behind the factory head on the ring, they go back to w0. Solved every
 *     REV_EVERY pairs during drives.
 *     Float throughout: the S3's FPU is single precision. With alpha = 80 on the diagonal, A + alpha I has a smallest eigenvalue of
 *     at least 80, so the float Cholesky stays far inside the 2 % gate; the host test runs a double solver beside it to check.
 * (b) Airflow constant. Absolute load (PID 0x43) is the air mass per intake stroke over the mass the cylinder holds at standard
 *     conditions, so  MAF [g/s] = k * absload [%] * rpm  with one constant per engine, k = 1.184 g/L * displacement / 12000.
 *     k is learned by scalar least squares with forgetting on 5 s means while MAF is reported, and used when MAF is missing.
 *     EXPERIMENTAL, untested on data: cars without PID 0x43 learn one k per rpm bin from calculated load (PID 0x04) instead.
 * (c) The source picker: running squared errors of the factory head, the learned head and the physics estimate against the same
 *     labels, so the board can tell which one to trust on this car when MAF is missing.
 * Labels are 5 s means of the measured fuel rate (PID 0x5E when the car reports it, else the MAF formula of econ.c), in L/h. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REV_NF        65            /* z (64 values) plus the bias */
#define REV_LAMBDA    0.999f        /* forgetting per pair: memory 1 / (1 - lambda) = 1000 pairs, about 23 min of driving */
#define REV_ALPHA     80.0f         /* pull toward w0, in pairs of data */
#define REV_HOLDOUT   150           /* most recent pairs, kept out of A and b to judge a candidate */
#define REV_EVERY     30            /* solve every N pairs */
#define REV_GATE      0.98f         /* a candidate must be 2 % better than the live weights on the holdout */
#define REV_DRIFT     3.0f          /* and stay within 3 x |w0| of w0 */
#define REV_RESET     1.05f         /* live weights 5 % worse than w0 on the holdout: back to w0 */
/* Outlier-robust learning (robust recursive least squares: Huber weights on a running residual scale, plus hard rejection).
   s is the exponential mean of |y - phi.w_live| over REV_SCALE_ROWS pairs, seeded by the mean of the first REV_SEED_ROWS. */
#define REV_HUBER_C   3.0f          /* a pair enters A and b with weight min(1, C s / |r|) */
#define REV_REJECT_R  10.0f         /* |r| > R s: the pair is dropped */
#define REV_SCALE_ROWS 200.0f
#define REV_SEED_ROWS 30
#define REV_LABEL_MAX_LPH 120.0f    /* labels above this (L/h) are implausible for a car and never reach a learner */

/* ---------------------------------------------------------------- (a) fuel head */
typedef struct { float phi[REV_NF]; float y, h; } rev_pair_t;   /* h: Huber weight */

typedef enum { REV_WAIT = 0, REV_ACCEPTED, REV_KEPT, REV_RESET_PRIOR, REV_SOLVE_FAILED } rev_commit_t;

typedef struct {
    const float *w0;                  /* factory head, standardised units (ml_params_rev.h MLR_W0) */
    float    w[2][REV_NF];            /* w[live] is what the predictor reads; a commit writes the other copy, then flips */
    volatile uint8_t live;
    float    A[REV_NF][REV_NF];       /* forgetting statistics; only the upper triangle (j >= i) is maintained */
    float    b[REV_NF];
    rev_pair_t ring[REV_HOLDOUT];
    int      ring_head, ring_n;
    uint32_t n;                       /* pairs seen since start: the REV_EVERY cadence */
    uint32_t commits, resets, kept, failed, learned;
    float    mse_live, mse_cand, mse_prior;   /* of the last commit, on the holdout (Huber loss at C s) */
    float    s;                       /* running residual scale, standardised units; not saved, re-seeded after a boot */
    uint32_t seen, rejected;          /* pairs taken since the scale was seeded; pairs dropped as outliers */
    float    M[REV_NF][REV_NF];       /* solve scratch */
    float    x[REV_NF];
} rev_fuel_t;

void  rev_fuel_init(rev_fuel_t *L, const float *w0);
/* Back to the factory head: weights, statistics, holdout and counters. */
void  rev_fuel_reset(rev_fuel_t *L);
float rev_fuel_predict(const rev_fuel_t *L, const float phi[REV_NF]);         /* live head, standardised units */
float rev_fuel_predict_prior(const rev_fuel_t *L, const float phi[REV_NF]);   /* factory head */
/* One labelled pair (y standardised). The oldest held-out pair moves into A and b with its Huber weight. Returns true when a
   commit is due. A pair with a value that is not finite, or with a residual over REV_REJECT_R times the running scale, is ignored. */
bool  rev_fuel_add(rev_fuel_t *L, const float phi[REV_NF], float y);
/* Solve, gate, swap. msg (may be NULL) gets one human line. */
rev_commit_t rev_fuel_commit(rev_fuel_t *L, char *msg, size_t msg_n);
/* |w_live - w0| / |w0| */
float rev_fuel_drift(const rev_fuel_t *L);

/* ---------------------------------------------------------------- labels: 5 s means of the measured fuel rate */
#define REV_LABEL_RING 16
#define REV_LABEL_SPAN_MS 5000
typedef struct { int64_t t_ms[REV_LABEL_RING]; float lph[REV_LABEL_RING]; int head, n; } rev_label_t;

void rev_label_reset(rev_label_t *r);
void rev_label_push(rev_label_t *r, int64_t t_ms, float lph);
/* Mean over (end - 5 s, end]: needs at least 2 rows spanning at least 2 s. Clipped to [0, 100] L/h. */
bool rev_label_mean(const rev_label_t *r, int64_t end_ms, float *out);

/* ---------------------------------------------------------------- (b) airflow constant */
#define REV_PHYS_K0        2.225e-4f   /* g/s per %-rpm: median over the training cars, about a 2.25 L engine */
#define REV_PHYS_LAMBDA    0.99        /* memory about 100 rows, 2.3 min */
#define REV_PHYS_MIN_ROWS  20          /* rows with real airflow before a learned k is trusted */
#define REV_PHYS_X_MIN     3000.0f     /* %-rpm: below this the engine is off or the row says nothing about k */
#define REV_PHYS_BINS      4           /* calculated-load path: rpm bins, edges 1500, 2500, 3500 */
#define REV_PHYS_RING      16

typedef struct { double sxx, sxy; float k; uint32_t n, n_sig; } rev_k_t;

typedef struct {
    rev_k_t abs;                        /* MAF = k * absolute load * rpm (PID 0x43) */
    rev_k_t calc[REV_PHYS_BINS];        /* EXPERIMENTAL: MAF = k_bin * calculated load * rpm (PID 0x04), when 0x43 is missing */
    /* 5 s window of rows with load, rpm and MAF all present: x = load * rpm, y = MAF */
    int64_t  t_ms[REV_PHYS_RING];
    float    x[REV_PHYS_RING], y[REV_PHYS_RING];
    uint8_t  path[REV_PHYS_RING];       /* 0 absolute load, 1 + bin for calculated load */
    int      head, n;
} rev_phys_t;

int   rev_phys_bin(float rpm);
void  rev_phys_init(rev_phys_t *P);
void  rev_phys_reset_k(rev_phys_t *P);          /* back to k0, statistics cleared */
void  rev_phys_new_drive(rev_phys_t *P);        /* drop the 5 s window, keep k */
/* One row. abs_ok/abs_load (PID 0x43), load_ok/load (PID 0x04), rpm_ok/rpm, maf_ok/maf. Learns when MAF is present.
   Returns true and sets *maf_mean_est to k * (5 s mean of x) when the row fed the window (for scoring against labels). */
bool  rev_phys_row(rev_phys_t *P, int64_t t_ms, bool abs_ok, float abs_load, bool load_ok, float load,
                   bool rpm_ok, float rpm, bool maf_ok, float maf, float *maf_mean_est);
/* Estimated MAF for this row, g/s, when MAF is missing. Absolute load uses k0 until it has learned; the experimental
   calculated-load path answers only once its bin has learned. ready (may be NULL) says whether k is learned. */
bool  rev_phys_estimate(const rev_phys_t *P, bool abs_ok, float abs_load, bool load_ok, float load, bool rpm_ok, float rpm,
                        float *maf_gps, bool *ready);
bool  rev_k_ready(const rev_k_t *k);

/* ---------------------------------------------------------------- (c) which estimate to trust without MAF */
#define REV_PICK_ROWS   200            /* time constant of the running errors, rows */
#define REV_PICK_MIN    20             /* scored rows before the running errors decide */
typedef enum { REV_SRC_PRIOR = 0, REV_SRC_LEARNED, REV_SRC_PHYS } rev_src_t;
typedef struct { float e[3]; uint32_t n[3]; } rev_pick_t;

void      rev_pick_init(rev_pick_t *p);
/* One label against whichever estimates exist for the same 5 s (L/h). */
void      rev_pick_score(rev_pick_t *p, float label, bool prior_ok, float prior, bool learned_ok, float learned, bool phys_ok, float phys);
/* The lowest running error among the scored ones, ties to physics (one parameter, cannot overfit). Before enough scores:
   physics if k is learned, then the learned head if it has committed, then the factory head. */
rev_src_t rev_pick_choose(const rev_pick_t *p, bool phys_ready, bool learned_committed);
const char *rev_src_name(rev_src_t s);
