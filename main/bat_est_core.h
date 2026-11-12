/* bat_est_core: the battery SoC/SoH estimator's math (EdgeOS AI service "bat", ESP-Mosaico). No IDF, float only:
 * host-tested in tests/host/test_bat_est.c and checked to 1e-4 against the research agent's test_vectors.csv.
 * The numbers come from bat_model.h (its contract is at the top of that file).
 *
 * The filter is the research agent's reference step (carmind-ml experiments/battery_soc: ref_estimator.py and its
 * float32 port bat_est_host.c), line for line, so the vectors match. With c = I / Q (C-rate, + = charge), z = SoC:
 *     predict   z  += eta c_avg dt / (3600 soh)                       (coulomb counting; soh = capacity / nominal)
 *               v1  = a v1 + (1 - a) r1 f(T) c_avg,  a = exp(-dt / tau)   (the RC branch, exact for c constant in dt)
 *               P   = F P F^T + Q dt,  F = diag(1, a)
 *     measure   V   = OCV(z) + v1 + r0 f(T) c_inst                      (r0 = r0c charging, r0d discharging)
 *     correct   H = [dOCV/dz, 1], S = H P H^T + r_v, K = P H^T / S, x += K (V - V_hat), P -= K S K^T
 * and an optional ridge correction on the output only (never fed back): z_corr = clamp(z + w . (f - mu) inv_sd + b).
 * soc_user maps z_corr onto the device window [z_lo, z_hi].
 *
 * The firmware's additions run after the filter step, only with learn on (the vector check runs with learn off):
 *   mode      from c: rest |c| < BAT_REST_C_RATE, discharge, charge, CV taper (charging at V >= BAT_FULL_V - 30 mV)
 *   anchors   rest for BAT_REST_MIN_S: z = OCV^-1(V); the end of a CV taper (V >= BAT_FULL_V, c <= BAT_FULL_C_RATE):
 *             z = 1. Until the first anchor the estimate is "learning".
 *   capacity  two anchors at least BAT_SOH_MIN_DZ apart: soh sample = Ah counted between them / (dz Q)
 *   DCIR      a current step of at least BAT_DCIR_MIN_DC within one sample: dV / dc (V per C), smoothed; its trend is
 *             DCIR / the first DCIR this cell gave, and BAT_SOH_RMAP_A + B x trend is the resistance-based SoH flag
 */
#ifndef BAT_EST_CORE_H
#define BAT_EST_CORE_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { BAT_MODE_REST = 0, BAT_MODE_DISCHARGE, BAT_MODE_CHARGE, BAT_MODE_CV };

typedef struct {
    float v, i_inst, i_avg, t_c, dt;                  /* V, A (charge positive), A over the sample, deg C, s */
    int   valid;                                      /* the voltage is usable */
} bat_in_t;

typedef struct {
    /* the reference filter's state and outputs (same names as bat_est_host.c) */
    float    z, v1, p00, p01, p11, v_prev, soh, have_prev;
    float    z_ekf, z_corr, soc_user, innov, v_hat;
    /* the firmware's additions */
    float    rest_s, anchor_z, ah_since, last_v, last_c, dcir, dcir_new;
    uint32_t n_anchor, n_q, n_dcir, steps;
    uint8_t  mode;
    bool     learn, ridge_off, anchored, rest_done, full_done, have_last;
} bat_est_t;

#define BAT_PERSIST_MAGIC 0x42455354u                 /* "BEST" */
#define BAT_PERSIST_VER   2
typedef struct {
    uint32_t magic;
    uint16_t ver, flags;                              /* flags bit 0: anchored at least once */
    float    z, soh, dcir, dcir_new;
    uint32_t n_q, n_dcir;
} bat_persist_t;

/* bat_init of the reference; learn = false for the vector check */
void  bat_est_init(bat_est_t *e, float z0, float p0_z, float p0_v, float soh, bool learn);
/* a boot: from the saved values when valid, else z from the OCV of the first sample (v volts, i amps) */
void  bat_est_boot(bat_est_t *e, const bat_persist_t *saved, float v, float i);
void  bat_est_step(bat_est_t *e, const bat_in_t *in);

float   bat_ocv_v(float z);
float   bat_ocv_inv(float v);                         /* z 0..1 */
uint8_t bat_mode(float c, float v);                   /* c = I / Q */
const char *bat_mode_name(uint8_t mode);              /* "rest", "discharge", "charge", "CV taper" */

float bat_soh_pct(const bat_est_t *e);                /* capacity-based, % */
float bat_dcir_ratio(const bat_est_t *e);             /* DCIR / the first DCIR, 1 before two samples */
float bat_soh_r_pct(const bat_est_t *e);              /* the resistance-based flag, % (BAT_SOH_RMAP) */
void  bat_est_persist(const bat_est_t *e, bat_persist_t *out);
bool  bat_persist_valid(const bat_persist_t *p);

/* the model, for the glue and the tests */
bool        bat_model_placeholder(void);
const char *bat_model_id(void);
float       bat_model_qnom_ah(void);
float       bat_model_p0_z(void);
float       bat_model_p0_v(void);
float       bat_model_rest_min_s(void);
void        bat_model_circuit(float *r0c, float *r0d, float *r1, float *tau_s);
void        bat_model_set_ridge(bool on);             /* the test vectors' use_ridge column */
void        bat_model_set_qnom_ah(float ah);          /* the firmware's CONFIG_OCM_BAT_NOMINAL_MAH (the vectors keep the header's) */

#ifdef __cplusplus
}
#endif
#endif
