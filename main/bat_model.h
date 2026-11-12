/* bat_model.h - SoC/SoH estimator constants (included by bat_est_core.c only).
 *
 * ============================================================================================================
 *  PLACEHOLDER. Generic values for a single-cell 4.2 V LiPo, NOT fitted to the ESP-Mosaico's 65 mAh cell.
 *  The research agent's generated header (carmind-ml experiments/battery_soc/gen_c.py -> bat_model.h) replaces
 *  this file as it is: same struct, same names. BAT_MODEL_PLACEHOLDER and BAT_MODEL_ID are optional in the
 *  fitted file (bat_est_core.c defaults them to 0 and "fitted").
 * ============================================================================================================
 *
 * Model: 1RC Thevenin equivalent circuit, OCV(z) + R0 + R1 || C1, with a 2-state EKF on x = [z, v1]. Units as in the
 * fitted header: c = I / BAT_QNOM_AH (C-rate, + = charge), z = SoC fraction, resistance in volts per 1C, tau in seconds,
 * temperature in deg C. The contract, every field and macro below:
 *   bat_params_t     qnom_ah; ocv[BAT_OCV_N] volts, uniform in z over [0, 1]; r0c / r0d (charge / discharge), r1, inv_tau;
 *                    kt, ft_min, ft_max (R(T) = R(25 C) clamp(1 - kt (T - 25), ft_min, ft_max)); eta; q_z, q_v (process
 *                    noise per s), r_v (V^2); z_clamp_lo / hi; use_ridge; ridge mu, inv_sd, w over BAT_NF features
 *                    { V, c, T, innov, dV/dt, z_ekf, |c| }, b, corr_max; z_lo / z_hi (device window: soc_user =
 *                    (z - z_lo) / (z_hi - z_lo))
 *   BAT_MODEL        the parameter values;  BAT_QNOM_AH, BAT_P0_Z, BAT_P0_V
 *   SoH and anchors  BAT_FULL_V, BAT_FULL_C_RATE, BAT_REST_C_RATE, BAT_REST_MIN_S, BAT_SOH_MIN_DZ, BAT_DCIR_MIN_DC,
 *                    BAT_SOH_RMAP_A, BAT_SOH_RMAP_B
 */
#ifndef BAT_MODEL_H
#define BAT_MODEL_H

#define BAT_MODEL_PLACEHOLDER 1
#define BAT_MODEL_ID "placeholder-lipo-4v2"

#define BAT_OCV_N 41          /* OCV table points, uniform in z over [0, 1] */
#define BAT_NF 7              /* ridge features: V, c, T, innov, dV/dt, z_ekf, |c| */

typedef struct {
    float qnom_ah;
    float ocv[BAT_OCV_N];
    float r0c, r0d, r1, inv_tau, kt, ft_min, ft_max, eta;
    float q_z, q_v, r_v;
    float z_clamp_lo, z_clamp_hi;
    float use_ridge;
    float mu[BAT_NF], inv_sd[BAT_NF], w[BAT_NF], b, corr_max;
    float z_lo, z_hi;
} bat_params_t;

/* 65 mAh is the cell's label, UNVERIFIED. The gauge's DesignCapacity is never used and never written. */
#define BAT_QNOM_AH 0.065f

#define BAT_P0_Z 0.04f        /* (20 %)^2: an unknown start */
#define BAT_P0_V 0.001f

static const bat_params_t BAT_MODEL = {
    /* qnom_ah */ 0.065f,
    /* ocv: a textbook LiPo curve, 3.30 V empty to 4.20 V full (UNVERIFIED for this cell) */
    { 3.3000f, 3.4250f, 3.5500f, 3.6150f, 3.6800f, 3.7000f, 3.7200f, 3.7300f, 3.7400f, 3.7500f, 3.7600f, 3.7700f, 3.7800f,
      3.7875f, 3.7950f, 3.8025f, 3.8100f, 3.8175f, 3.8250f, 3.8350f, 3.8450f, 3.8550f, 3.8650f, 3.8775f, 3.8900f, 3.9050f,
      3.9200f, 3.9350f, 3.9500f, 3.9675f, 3.9850f, 4.0025f, 4.0200f, 4.0400f, 4.0600f, 4.0800f, 4.1000f, 4.1250f, 4.1500f,
      4.1750f, 4.2000f },
    /* r0c, r0d: 0.8 ohm x 0.065 A per C = 0.052 V per C (generic small pouch) */
    0.052f, 0.052f,
    /* r1 0.4 ohm = 0.026 V per C, tau 30 s */
    0.026f, 1.0f / 30.0f,
    /* kt, ft_min, ft_max: no temperature model in the placeholder */
    0.0f, 0.5f, 1.5f,
    /* eta */
    1.0f,
    /* q_z, q_v, r_v */
    1.0e-8f, 1.0e-7f, 1.0e-4f,
    /* z clamp */
    -0.05f, 1.05f,
    /* use_ridge: off, the weights below are zero */
    0.0f,
    { 0, 0, 0, 0, 0, 0, 0 },
    { 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0 },
    0.0f, 0.1f,
    /* device window: the whole cell */
    0.0f, 1.0f,
};

/* ---- SoH and anchors ---- */
#define BAT_FULL_V        4.18f   /* CV at or above this with |I| <= BAT_FULL_C_RATE x Q: full, z = 1 */
#define BAT_FULL_C_RATE   0.1f
#define BAT_REST_C_RATE   0.05f   /* |I| below this for BAT_REST_MIN_S: z = OCV^-1(V) */
#define BAT_REST_MIN_S    300.0f
#define BAT_SOH_MIN_DZ    0.3f    /* two anchors this far apart: a capacity sample */
#define BAT_DCIR_MIN_DC   0.2f    /* a current step of this many C within one sample: a DCIR sample */
#define BAT_SOH_RMAP_A    1.0f    /* relative capacity ~ A + B x (DCIR / DCIR_new): no mapping in the placeholder */
#define BAT_SOH_RMAP_B    0.0f

#endif /* BAT_MODEL_H */
