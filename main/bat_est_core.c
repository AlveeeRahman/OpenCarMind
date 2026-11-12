/* bat_est_core: see bat_est_core.h. Single precision throughout (the S31 FPU is single precision).
 * BAT_MODEL_FILE (a quoted path) selects another bat_model.h, for the host test of a header before it lands in main/. */
#include "bat_est_core.h"
#include <math.h>
#include <string.h>
#ifdef BAT_MODEL_FILE
#include BAT_MODEL_FILE
#else
#include "bat_model.h"
#endif
#ifndef BAT_MODEL_PLACEHOLDER
#define BAT_MODEL_PLACEHOLDER 0
#endif
#ifndef BAT_MODEL_ID
#define BAT_MODEL_ID "fitted"
#endif
#define BAT_CV_MARGIN_V  0.03f                         /* charging this close to BAT_FULL_V: the CV taper */
#define BAT_ANCHOR_P00   4.0e-4f                       /* (2 %)^2 after an anchor */

static const bat_params_t *const M = &BAT_MODEL;     /* read in place (flash): no RAM copy of the table */
static float s_use_ridge = -1.0f, s_qnom_ah = -1.0f; /* overrides: the vector check's use_ridge, the Kconfig nominal */
#define USE_RIDGE (s_use_ridge >= 0.0f ? s_use_ridge : M->use_ridge)
#define QNOM_AH   (s_qnom_ah > 0.0f ? s_qnom_ah : M->qnom_ah)

static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

bool bat_model_placeholder(void) { return BAT_MODEL_PLACEHOLDER != 0; }
const char *bat_model_id(void) { return BAT_MODEL_ID; }
float bat_model_qnom_ah(void) { return QNOM_AH; }
void bat_model_set_qnom_ah(float ah) { if (ah > 0.0f) s_qnom_ah = ah; }
float bat_model_p0_z(void) { return BAT_P0_Z; }
float bat_model_p0_v(void) { return BAT_P0_V; }
float bat_model_rest_min_s(void) { return BAT_REST_MIN_S; }
void bat_model_set_ridge(bool on) { s_use_ridge = on ? 1.0f : 0.0f; }
void bat_model_circuit(float *r0c, float *r0d, float *r1, float *tau_s)
{
    *r0c = M->r0c; *r0d = M->r0d; *r1 = M->r1; *tau_s = 1.0f / M->inv_tau;
}

float bat_ocv_v(float z)
{
    const float zl = clampf(z, M->z_clamp_lo, M->z_clamp_hi);
    const float pos = zl * (float)(BAT_OCV_N - 1);
    int k = (int)floorf(pos);
    k = k < 0 ? 0 : (k > BAT_OCV_N - 2 ? BAT_OCV_N - 2 : k);
    return M->ocv[k] + (pos - (float)k) * (M->ocv[k + 1] - M->ocv[k]);
}

float bat_ocv_inv(float v)
{
    if (v <= M->ocv[0]) return 0.0f;
    if (v >= M->ocv[BAT_OCV_N - 1]) return 1.0f;
    int k = 0;
    while (k < BAT_OCV_N - 2 && v > M->ocv[k + 1]) k++;
    float d = M->ocv[k + 1] - M->ocv[k];
    float f = d > 0.0f ? (v - M->ocv[k]) / d : 0.0f;
    return ((float)k + f) / (float)(BAT_OCV_N - 1);
}

uint8_t bat_mode(float c, float v)
{
    if (c <= -BAT_REST_C_RATE) return BAT_MODE_DISCHARGE;
    if (c < BAT_REST_C_RATE) return BAT_MODE_REST;
    return v >= BAT_FULL_V - BAT_CV_MARGIN_V ? BAT_MODE_CV : BAT_MODE_CHARGE;
}

const char *bat_mode_name(uint8_t m)
{
    static const char *const N[4] = { "rest", "discharge", "charge", "CV taper" };
    return m < 4 ? N[m] : "?";
}

void bat_est_init(bat_est_t *e, float z0, float p0_z, float p0_v, float soh, bool learn)
{
    memset(e, 0, sizeof *e);
    e->z = z0; e->v1 = 0.0f; e->p00 = p0_z; e->p01 = 0.0f; e->p11 = p0_v;
    e->v_prev = 0.0f; e->have_prev = 0.0f; e->soh = soh;
    e->z_ekf = e->z_corr = z0;
    e->soc_user = clampf((z0 - M->z_lo) / (M->z_hi - M->z_lo), 0.0f, 1.0f);
    e->learn = learn;
    e->mode = BAT_MODE_REST;
}

void bat_est_boot(bat_est_t *e, const bat_persist_t *s, float v, float i)
{
    if (s && bat_persist_valid(s)) {
        bat_est_init(e, s->z, 0.0025f, BAT_P0_V, s->soh, true);       /* (5 %)^2: the time it was off is unknown */
        e->dcir = s->dcir; e->dcir_new = s->dcir_new; e->n_q = s->n_q; e->n_dcir = s->n_dcir;
        e->anchored = (s->flags & 1u) != 0;
        e->anchor_z = s->z;
    } else {
        float c = i / QNOM_AH;
        float r0 = c >= 0.0f ? M->r0c : M->r0d;
        bat_est_init(e, bat_ocv_inv(v - r0 * c), BAT_P0_Z, BAT_P0_V, 1.0f, true);
    }
}

/* the reference step (bat_est_host.c bat_step), unchanged apart from ridge_off (the OS ladder's AI rung) */
static void filter_step(bat_est_t *s, const bat_in_t *in)
{
    const float v = in->v, i_inst = in->i_inst, i_avg = in->i_avg, t_c = in->t_c;
    const int valid = in->valid;
    const float dt = clampf(in->dt, 0.1f, 10.0f);
    const float inv_q = 1.0f / QNOM_AH;
    const float c_avg = i_avg * inv_q;
    const float c_i = i_inst * inv_q;
    const float ft = clampf(1.0f - M->kt * (t_c - 25.0f), M->ft_min, M->ft_max);
    const float a = expf(-dt * M->inv_tau);

    s->z += M->eta * c_avg * dt / (3600.0f * s->soh);
    s->v1 = a * s->v1 + (1.0f - a) * M->r1 * ft * c_avg;
    const float qz = M->q_z * dt, qv = M->q_v * dt;
    s->p00 += qz;
    s->p01 *= a;
    s->p11 = a * a * s->p11 + qv;

    const float zl = clampf(s->z, M->z_clamp_lo, M->z_clamp_hi);
    const float pos = zl * (float)(BAT_OCV_N - 1);
    int k = (int)floorf(pos);
    k = k < 0 ? 0 : (k > BAT_OCV_N - 2 ? BAT_OCV_N - 2 : k);
    const float fr = pos - (float)k;
    const float slope = (M->ocv[k + 1] - M->ocv[k]) * (float)(BAT_OCV_N - 1);
    const float ocv = M->ocv[k] + fr * (M->ocv[k + 1] - M->ocv[k]);
    const float r0 = (c_i >= 0.0f ? M->r0c : M->r0d) * ft;
    const float v_hat = ocv + s->v1 + r0 * c_i;
    float innov = 0.0f;

    if (valid) {
        innov = v - v_hat;
        const float ph0 = s->p00 * slope + s->p01;
        const float ph1 = s->p01 * slope + s->p11;
        const float sv = slope * ph0 + ph1 + M->r_v;
        const float k0 = ph0 / sv, k1 = ph1 / sv;
        s->z += k0 * innov;
        s->v1 += k1 * innov;
        s->p00 -= k0 * ph0;
        s->p01 -= k0 * ph1;
        s->p11 -= k1 * ph1;
        if (s->p00 < 1e-9f) s->p00 = 1e-9f;
        if (s->p11 < 1e-9f) s->p11 = 1e-9f;
    }
    s->z = clampf(s->z, M->z_clamp_lo, M->z_clamp_hi);

    const float dvdt = (s->have_prev > 0.5f && valid) ? (v - s->v_prev) / dt : 0.0f;
    float corr = 0.0f;
    if (USE_RIDGE > 0.5f && !s->ridge_off) {
        const float f[BAT_NF] = { v, c_i, t_c, innov, dvdt, s->z, fabsf(c_i) };
        float acc = M->b;
        for (int j = 0; j < BAT_NF; j++)
            acc += M->w[j] * (f[j] - M->mu[j]) * M->inv_sd[j];
        corr = clampf(acc, -M->corr_max, M->corr_max);
    }
    if (valid) { s->v_prev = v; s->have_prev = 1.0f; }
    const float zc = clampf(s->z + corr, 0.0f, 1.0f);

    s->z_ekf = s->z;
    s->z_corr = zc;
    s->soc_user = clampf((zc - M->z_lo) / (M->z_hi - M->z_lo), 0.0f, 1.0f);
    s->innov = innov;
    s->v_hat = v_hat;
}

static void anchor(bat_est_t *e, float z)
{
    if (e->anchored) {
        float dz = z - e->anchor_z;
        if (fabsf(dz) >= BAT_SOH_MIN_DZ) {
            float m = e->ah_since / (dz * QNOM_AH);          /* the capacity seen, over the nominal */
            if (m >= 0.3f && m <= 1.3f) {
                e->soh = e->n_q ? e->soh + 0.2f * (m - e->soh) : 0.5f * (e->soh + m);
                e->n_q++;
                if (fabsf(dz) >= BAT_FULL_DZ) { e->soh_full = e->n_full ? e->soh_full + 0.2f * (m - e->soh_full) : m; e->n_full++; }
            }
        }
    }
    e->z = z;
    e->p00 = BAT_ANCHOR_P00;
    e->p01 = 0.0f;
    e->z_ekf = z;
    e->z_corr = clampf(z, 0.0f, 1.0f);
    e->soc_user = clampf((e->z_corr - M->z_lo) / (M->z_hi - M->z_lo), 0.0f, 1.0f);
    e->anchor_z = z;
    e->ah_since = 0.0f;
    e->anchored = true;
    e->n_anchor++;
}

void bat_est_step(bat_est_t *e, const bat_in_t *in)
{
    filter_step(e, in);
    const float c = in->i_inst / QNOM_AH, dt = clampf(in->dt, 0.1f, 10.0f);
    uint8_t mode = bat_mode(c, in->v);
    if (e->learn) {
        e->ah_since += M->eta * in->i_avg * dt / 3600.0f;
        if (in->valid && e->have_last && in->dt <= 2.5f) {          /* a step within one sample: DCIR */
            float dc = c - e->last_c;
            if (fabsf(dc) >= BAT_DCIR_MIN_DC) {
                float r = (in->v - e->last_v) / dc;
                if (r > 0.2f * M->r0d && r < 5.0f * (M->r0d + M->r1)) {
                    e->dcir = e->n_dcir ? e->dcir + 0.1f * (r - e->dcir) : r;
                    if (e->dcir_new <= 0.0f) e->dcir_new = r;
                    e->n_dcir++;
                }
            }
        }
        if (mode == BAT_MODE_REST) e->rest_s += dt; else { e->rest_s = 0.0f; e->rest_done = false; }
        if (in->valid && mode == BAT_MODE_REST && !e->rest_done && e->rest_s >= BAT_REST_MIN_S) {
            anchor(e, bat_ocv_inv(in->v));
            e->rest_done = true;
        }
        if (mode == BAT_MODE_DISCHARGE) e->full_done = false;
        if (in->valid && !e->full_done && (e->mode == BAT_MODE_CV || mode == BAT_MODE_CV) && c >= 0.0f &&
            c <= BAT_FULL_C_RATE && in->v >= BAT_FULL_V) {
            anchor(e, 1.0f);                                         /* the taper ended: full, once per charge */
            e->full_done = true;
        }
    }
    if (in->valid) { e->last_v = in->v; e->last_c = c; e->have_last = true; }
    e->mode = mode;
    e->steps++;
}

float bat_soh_pct(const bat_est_t *e) { return 100.0f * e->soh; }
float bat_dcir_ratio(const bat_est_t *e) { return e->n_dcir > 1 && e->dcir_new > 0.0f ? e->dcir / e->dcir_new : 1.0f; }
float bat_soh_r_pct(const bat_est_t *e) { return 100.0f * (BAT_SOH_RMAP_A + BAT_SOH_RMAP_B * bat_dcir_ratio(e)); }

void bat_est_persist(const bat_est_t *e, bat_persist_t *o)
{
    memset(o, 0, sizeof *o);
    o->magic = BAT_PERSIST_MAGIC;
    o->ver = BAT_PERSIST_VER;
    o->flags = e->anchored ? 1u : 0u;
    o->z = clampf(e->z, 0.0f, 1.0f);
    o->soh = e->soh;
    o->dcir = e->dcir;
    o->dcir_new = e->dcir_new;
    o->n_q = e->n_q;
    o->n_dcir = e->n_dcir;
}

bool bat_persist_valid(const bat_persist_t *p)
{
    return p->magic == BAT_PERSIST_MAGIC && p->ver == BAT_PERSIST_VER && p->z >= 0.0f && p->z <= 1.0f &&
           p->soh >= 0.3f && p->soh <= 1.3f && p->dcir >= 0.0f && p->dcir < 10.0f && p->dcir_new >= 0.0f && p->dcir_new < 10.0f;
}
