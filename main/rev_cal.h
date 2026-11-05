/* Fill-up calibration of the board's fuel figures. Pure C, host-tested (tests/host/test_rev_learn.c).
 *
 * One factor c such that c x (litres the board counted) = litres actually pumped. It multiplies every fuel source (car PID, MAF,
 * physics, learned and factory estimates), so the screen, the CSV and the trip totals show calibrated fuel; the learners keep
 * learning on raw values. With no fill-ups c = 1, which leaves every figure exactly as before.
 *
 * The board keeps its own per-drive ledger (the last REV_CAL_SESSIONS drives): raw litres, km, and engine seconds with and without
 * a fuel rate. The user's fill-ups come from the desktop app. The tank-to-tank method:
 *   - A full fill (to the pump's automatic click-off) closes an interval that started at the previous full fill; partial fills in
 *     between add their litres. Interval i: P_i = litres pumped, R_i = raw litres the board counted over the drives in between.
 *   - An interval counts only if R_i >= 5 L, fuel-rate coverage >= 95 % of engine time, all its drives are still in the ledger,
 *     and 0.5 <= P_i / R_i <= 2 (a typo such as 400 L fails).
 *   - A trip-computer figure (L/100 km over a range of drives) gives P = L/100km x km / 100 and counts half as much as a pump interval.
 *   - c = sum_i g^(n-i) w_i P_i / sum_i g^(n-i) w_i R_i over the accepted intervals, newest weighted most (g = 0.7 per interval),
 *     clamped to [0.7, 1.4]. A ratio of sums, so a short top-up interval cannot swing c. The first full fill only sets the start. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define REV_CAL_SESSIONS  64
#define REV_CAL_FILLUPS   16
#define REV_CAL_GAMMA     0.7
#define REV_CAL_MIN       0.7f
#define REV_CAL_MAX       1.4f
#define REV_CAL_MIN_RAW_L 5.0f
#define REV_CAL_COVERAGE  0.95f

typedef struct { uint32_t session; float raw_l, km, cov_s, unc_s; } rev_ledger_t;

typedef enum { REV_FILL_PUMP = 0, REV_FILL_TRIP = 1 } rev_fill_kind_t;
typedef struct {
    uint32_t id;                 /* the app's entry id */
    uint32_t after_session;      /* pump: the last drive before the fill; trip computer: the last drive of the range */
    uint32_t from_session;       /* trip computer: the first drive of the range */
    float    litres;             /* pump */
    float    l_per_100km;        /* trip computer */
    uint8_t  kind;               /* rev_fill_kind_t */
    uint8_t  full;               /* pump: filled to the click-off */
} rev_fill_t;

typedef enum { REV_IV_USED = 0, REV_IV_SMALL, REV_IV_COVERAGE, REV_IV_GONE, REV_IV_RATIO } rev_iv_why_t;
typedef struct { uint32_t fill_id, from_session, to_session; float pumped_l, raw_l; uint8_t why, trip; } rev_interval_t;

typedef struct {
    rev_ledger_t ledger[REV_CAL_SESSIONS];  /* oldest first */
    int          ledger_n;
    uint32_t     dropped_max;               /* newest session that fell out of the ledger (0 = none) */
    rev_fill_t   fills[REV_CAL_FILLUPS];
    int          n_fills;
    uint32_t     fill_rev;                  /* the app's revision of its fill-up list */
    float        c;
    int          used, skipped;
    rev_interval_t iv[REV_CAL_FILLUPS];
    int          n_iv;
    double       life_l, life_km;           /* lifetime: calibrated litres and the km driven while the fuel rate was known */
} rev_cal_t;

void  rev_cal_init(rev_cal_t *c);
/* One OBD row: raw fuel rate (negative = unknown), speed (negative = unknown), engine running (rpm > 0), seconds since the
   last row of this drive. Gaps over 10 s are not counted. cal is the factor in force for the lifetime totals. */
void  rev_cal_row(rev_cal_t *c, uint32_t session, float dt_s, float raw_lph, float kph, bool engine_running);
/* Replace the fill-up list (any order; at most REV_CAL_FILLUPS) and recompute c. Returns false and changes nothing if an
   entry is not plausible (litres 0..300, L/100 km 1..60, a trip range backwards). */
bool  rev_cal_set_fills(rev_cal_t *c, const rev_fill_t *fills, int n, uint32_t fill_rev);
/* Recompute c, used, skipped and the interval list from the fill-ups and the ledger. */
float rev_cal_compute(rev_cal_t *c);
/* Forget the fill-ups and go back to c = 1 (the ledger and lifetime totals stay). */
void  rev_cal_reset(rev_cal_t *c);
const char *rev_iv_why_name(int why);
