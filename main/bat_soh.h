/* bat_soh: the battery's state of health from partial charge cycles (OpenCarMind EdgeOS, part of the "bat" AI service on
 * the ESP-Mosaico). No IDF, no heap: host-tested in tests/host/test_bat_soh.c. The model is the research agent's ridge
 * regressor (bat_soh_model.h, carmind-ml experiments/battery_soc: p3_soh.py, gen_p3.py); this file computes its eight
 * features online from the samples bat_est already has, one sample at a time, in fixed memory.
 *
 * One cycle = a charge and the rest or discharge after it, until the next charge starts. Its features:
 *   dq_chg_win   Ah charged between 3.9 V and 4.1 V during the constant-current part, over the nominal capacity
 *   ica_chg_v    the voltage of the dQ/dV peak during that part: Q(V) on a 10 mV grid from 3.60 to 4.15 V (56 floats,
 *                the charge recorded where the voltage crosses each grid line, linear between two samples), its
 *                gradient (np.gradient: central differences, one-sided at the ends), a 5-point moving mean, the largest
 *                |dQ/dV| inside the trimmed grid
 *   ica_chg_h    the height of that peak (per volt, Q in units of the nominal capacity)
 *   cv_dur_h     the constant-voltage part's duration (charging at V >= BAT_FULL_V - 30 mV), hours
 *   cv_tau_h     the CV current's decay constant: least squares of ln(I) on t over the CV samples with I > 0.0067 C,
 *                tau = -1 / slope, hours
 *   dcir         the first rest -> load step after the charge (rest |c| < 0.0067 C, then c <= -BAT_SOH_DCIR_C):
 *                (V_rest - V_120s - (OCV(z_rest) - OCV(z_120s))) / -c_120s, volts per C, 120 s into the load
 *   T_avg        the mean cell temperature over the cycle
 *   ekf_ratio    on a discharge that starts full: Ah discharged / Q_nom / (1 - z_EKF) at the first z_EKF <= 0.3
 * The model takes five of them relative to the cell's own reference: the median of each over the first BAT_SOH_NREF
 * (10) usable cycles this firmware sees, learnt on the board and kept in NVS by bat_est.c. Until then the source is
 * "learning (n of 10)". A cycle is usable when all eight features exist: the charge must start at or below 3.60 V and
 * reach the CV part, so a top-up charge gives no SoH sample (it is counted, not used).
 *
 * The full-ends coulomb count wins when it exists (0.08 % error in the research, against 1.45 % for the model): a capacity
 * sample from two anchors at least BAT_SOH_FULL_DZ apart (bat_est_core.c anchor(): rest OCV or full), so a nearly
 * empty-to-full charge. bat_est_core keeps that count; bat_soh_source() picks.
 *
 * UNVERIFIED: the model was fitted on lab cells (3 Ah, CC 0.5 C) and its NASA cross-cell test failed; on this 65 mAh cell
 * its output is a relative figure (the capacity against the first learnt cycles), not an absolute SoH.
 */
#ifndef BAT_SOH_H
#define BAT_SOH_H
#include <stdbool.h>
#include <stdint.h>
#include "bat_est_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BAT_SOH_GRID_LO   3.60f
#define BAT_SOH_GRID_STEP 0.01f
#define BAT_SOH_GRID_N    56            /* np.arange(3.6, 4.15, 0.01): 56 points, the last 4.15 (float rounding) */
#define BAT_SOH_WIN_LO    3.9f
#define BAT_SOH_WIN_HI    4.1f
#define BAT_SOH_NREF      10
#define BAT_SOH_NRAW      8
#define BAT_SOH_NREL      5             /* dq_chg_win, ica_chg_h, cv_tau_h, dcir, ekf_ratio */
#define BAT_SOH_REST_C    0.0067f       /* 0.02 A of the research cells' 3 Ah */
#define BAT_SOH_DCIR_C    0.33f         /* a load step of at least this many C (1 A of the research cells' 3 Ah) */
#define BAT_SOH_DCIR_S    120.0f
#define BAT_SOH_FULL_DZ   BAT_FULL_DZ   /* anchors this far apart: the full-ends count (bat_est_core.h) */

enum { SOH_F_DQ = 0, SOH_F_ICA_V, SOH_F_ICA_H, SOH_F_CV_DUR, SOH_F_CV_TAU, SOH_F_DCIR, SOH_F_TAVG, SOH_F_EKF };

typedef struct {
    float v, c, t_c, dt, z_ekf, ocv_z;  /* volts, C-rate (+ charge), deg C, s, SoC fraction, OCV(z) volts */
    uint8_t mode;                       /* BAT_MODE_* */
    bool valid;
} soh_in_t;

/* the reference, learnt on the board (NVS "ocm_bat"/"soh" holds this struct) */
#define BAT_SOH_MAGIC 0x534F4831u       /* "SOH1" */
typedef struct {
    uint32_t magic;
    uint8_t  n, pad[3];                 /* usable cycles in ref[] */
    float    ref[BAT_SOH_NREF][BAT_SOH_NREL];
    float    last_soh;                  /* the model's last output, 0 before one */
    uint32_t cycles, usable;            /* cycles seen, cycles with all features */
    float    full_soh;                  /* the full-ends coulomb count (bat_est_core), 0 before one */
    uint32_t n_full;
} soh_ref_t;

typedef struct {
    /* this cycle */
    float    q_ah;                      /* Ah since the charge started (charge only) */
    float    grid[BAT_SOH_GRID_N];      /* Q at each grid voltage, Ah */
    int16_t  next_k;                    /* the next grid line to cross; -1 = the grid is void (started above 3.60 V) */
    float    v_prev, q_prev;
    bool     have_prev, charging, cc_seen, cv_seen, ended_full;
    float    cv_s;                      /* CV seconds */
    double   cv_n, cv_st, cv_stt, cv_sy, cv_sty;  /* least squares sums, t from the first CV sample */
    float    cv_t;
    float    t_sum, t_n;                /* temperature over the cycle */
    /* after the charge */
    float    rest_s, dcir_v0, dcir_ocv0, dcir_t;
    bool     dcir_armed, dcir_on, dcir_done;
    float    q_dis_ah;
    bool     ekf_armed, ekf_done;
    float    raw[BAT_SOH_NRAW];
    uint8_t  have;                      /* one bit per raw feature */
} soh_cycle_t;

typedef struct {
    soh_cycle_t cyc;
    soh_ref_t   ref;
    float       qnom_ah;
    float       last_x[BAT_SOH_NRAW];   /* the last usable cycle's model input (relative features) */
    bool        have_x;
    uint32_t    gen;                    /* counts model outputs and reference changes (bat_est.c saves on a change) */
} bat_soh_t;

void  bat_soh_init(bat_soh_t *s, float qnom_ah, const soh_ref_t *saved);   /* saved NULL or invalid: learn from scratch */
bool  bat_soh_ref_valid(const soh_ref_t *r);
/* one sample; true when a cycle ended with a new model output (s->ref.last_soh) or a new reference entry */
bool  bat_soh_step(bat_soh_t *s, const soh_in_t *in);

/* the pure pieces, for the host test against p3_soh.py (q_between, ica_peak) */
float bat_soh_q_between(const float grid[BAT_SOH_GRID_N], float v1, float v2);  /* Ah, from the grid */
void  bat_soh_ica_peak(const float grid_q[BAT_SOH_GRID_N], float *h, float *v);  /* grid_q in units of Q_nom */
float bat_soh_median(const float *x, int n);                                     /* np.median of n values */
float bat_soh_predict_rel(const soh_ref_t *r, const float raw[BAT_SOH_NRAW], float x_out[BAT_SOH_NRAW]);

enum { SOH_SRC_NONE = 0, SOH_SRC_LEARNING, SOH_SRC_MODEL, SOH_SRC_FULL };
/* full: the full-ends count exists (n_full > 0) */
uint8_t bat_soh_source(const bat_soh_t *s, uint32_t n_full);

#ifdef __cplusplus
}
#endif
#endif
