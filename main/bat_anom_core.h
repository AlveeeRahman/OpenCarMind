/* bat_anom_core: the unsupervised battery anomaly detector (OpenCarMind EdgeOS AI service "banom", ESP-Mosaico).
 * Ported from the research reference (carmind-ml experiments/battery_soc: anom_host.c, ref_anom.py; parameters in
 * bat_anom_model.h). No IDF, float32, fixed cost, no allocation: host-tested in tests/host/test_bat_anom.c against
 * anom_vectors.csv to 1e-4.
 *
 * Per sample it accumulates window statistics; every window (60 samples at 1 Hz in the reference, 60 s of samples on
 * the board) it forms AN_NF features and scores them three ways:
 *   robust z     |f - median| / (1.4826 MAD), frugal streaming median and MAD (Ma, Muthukrishnan, Sandler 2013)
 *   Gaussian     |f - mean| / sd, EWMA mean and variance
 *   autoencoder  9-4-9 tanh, trained offline on normal windows; input = the Gaussian z clipped to +-5; score = the mean
 *                squared reconstruction error over the valid features
 * Only the autoencoder can raise the user-facing alarm (BAT_ANOM_ALARM_MASK = 4): on the held-out benign data it gave
 * 0.0026 false alarms per hour, robust z 0.245 and the Gaussian 0.126, so those two are diagnostics only. An alarm needs
 * k_consec windows in a row over the threshold, after a warm-up of warmup_win windows (24 h).
 * Poisoning guard: the statistics are updated only on windows that score below upd_frac of the thresholds, never while
 * an alarm is active (during the warm-up every window updates, 4x faster).
 * Firmware additions, which leave the reference's numbers unchanged when samp_s = 1: the sample period (the board's gauge
 * gives a sample every 2 s, so a 60 s window is 30 samples and the count limits scale with it), and the per-feature
 * reconstruction error for the alarm's fault class. The detector only raises flags; it never acts on power.
 */
#ifndef BAT_ANOM_CORE_H
#define BAT_ANOM_CORE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AN_NF 9
#define AN_WIN 60
#define AN_AEH 4

typedef struct {
    float r0d, r0c, kt;                 /* from the SoC model, V per C */
    float qnom_ah;
    float tau_th, k_heat;               /* 1-node thermal model: dTm/dt = (Tamb - Tm)/tau_th + k_heat c^2 */
    float c_rest, c_load, c_step, v_spike;
    float eta_med, eta_mad, eta_g;      /* update rates per window */
    float warmup_win, k_consec;
    float thr_rz, thr_g, thr_ae, upd_frac;
    float t_rest_min, a_slow;           /* rest features only after t_rest_min s of rest; EMA rate across windows */
    float side[AN_NF];                  /* +1 = only high values are anomalous, 0 = both sides */
    float m0[AN_NF], d0[AN_NF], d_min[AN_NF];      /* priors for median, MAD; MAD floor */
    float thr_f[AN_NF], thr_gf[AN_NF];            /* per-feature limits (robust z, Gaussian z) */
    float ae_w1[AN_AEH * AN_NF], ae_b1[AN_AEH], ae_w2[AN_NF * AN_AEH], ae_b2[AN_NF];
} an_params_t;

typedef struct {
    /* window accumulators */
    float n, n_valid, s_e, s_e2, n_rest, s_crest, v_rest0, v_rest1, t_rest0, t_rest1, s_tres, n_load, s_rload,
          n_step, s_rstep, n_dis, s_cdis, n_spk;
    /* running context */
    float v_prev, c_prev, have_prev, tm, tamb, init, t_rest_run, ema_e, ema_r, ema_sag, sag_ok;
    /* detector state */
    float med[AN_NF], mad[AN_NF], mu[AN_NF], var[AN_NF];
    float nwin, run_rz, run_g, run_ae, alarm;
    /* the board's sample period (firmware addition, not in the reference): samp_s = 1 reproduces anom_host.c exactly */
    float samp_s, win_n, min_n;
} an_state_t;

typedef struct {
    float f[AN_NF], mask[AN_NF], s_rz, s_g, s_ae;
    float ae_err[AN_NF];               /* firmware addition: each feature's squared reconstruction error (the fault class) */
    int   win_done, a_rz, a_g, a_ae;
} an_out_t;

/* samp_s: the nominal sample period in seconds (1 = the reference) */
void an_init(const an_params_t *p, an_state_t *s, float samp_s);
/* inputs: voltage, current (A, + charge), temperature, dt, valid flag, and the SoC filter's innovation, OCV(z) and v1 */
void an_step(const an_params_t *p, an_state_t *s, float v, float i, float t_c, float dt, int valid,
             float innov, float ocv_z, float v1, an_out_t *o);
const char *an_class_name(const an_out_t *o);   /* the fault class of an alarm window: its largest reconstruction error */

#ifdef __cplusplus
}
#endif
#endif
