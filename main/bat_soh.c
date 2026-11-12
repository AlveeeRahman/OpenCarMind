/* bat_soh: see bat_soh.h. The feature definitions follow p3_soh.py (raw_features, q_between, ica_peak, relative). */
#include "bat_soh.h"
#include <math.h>
#include <string.h>
#include "bat_est_core.h"
#include "bat_soh_model.h"

#define K_WIN_LO  30                    /* grid index of 3.90 V */
#define K_WIN_HI  50                    /* grid index of 4.10 V */
#define F_ALL     ((1u << BAT_SOH_NRAW) - 1u)

static float grid_v(int k) { return BAT_SOH_GRID_LO + (float)k * BAT_SOH_GRID_STEP; }

bool bat_soh_ref_valid(const soh_ref_t *r)
{
    if (!r || r->magic != BAT_SOH_MAGIC || r->n > BAT_SOH_NREF) return false;
    for (int i = 0; i < r->n; i++)
        for (int j = 0; j < BAT_SOH_NREL; j++) if (!isfinite(r->ref[i][j]) || r->ref[i][j] <= 0.0f) return false;
    return isfinite(r->last_soh) && isfinite(r->full_soh);
}

static void cycle_reset(soh_cycle_t *c)
{
    memset(c, 0, sizeof *c);
    c->next_k = -1;
}

void bat_soh_init(bat_soh_t *s, float qnom_ah, const soh_ref_t *saved)
{
    memset(s, 0, sizeof *s);
    cycle_reset(&s->cyc);
    s->qnom_ah = qnom_ah > 0.0f ? qnom_ah : 1.0f;
    if (saved && bat_soh_ref_valid(saved)) s->ref = *saved;
    else { memset(&s->ref, 0, sizeof s->ref); s->ref.magic = BAT_SOH_MAGIC; }
}

float bat_soh_q_between(const float g[BAT_SOH_GRID_N], float v1, float v2)
{
    float a = (v1 - BAT_SOH_GRID_LO) / BAT_SOH_GRID_STEP, b = (v2 - BAT_SOH_GRID_LO) / BAT_SOH_GRID_STEP;
    int ka = (int)lroundf(a), kb = (int)lroundf(b);
    if (ka < 0 || kb < 0 || ka >= BAT_SOH_GRID_N || kb >= BAT_SOH_GRID_N) return NAN;
    return fabsf(g[kb] - g[ka]);
}

void bat_soh_ica_peak(const float q[BAT_SOH_GRID_N], float *h, float *v)
{
    float gr[BAT_SOH_GRID_N];
    const float st = BAT_SOH_GRID_STEP;
    gr[0] = (q[1] - q[0]) / st;                                   /* np.gradient: one-sided at the ends */
    gr[BAT_SOH_GRID_N - 1] = (q[BAT_SOH_GRID_N - 1] - q[BAT_SOH_GRID_N - 2]) / st;
    for (int k = 1; k < BAT_SOH_GRID_N - 1; k++) gr[k] = (q[k + 1] - q[k - 1]) / (2.0f * st);
    float best = -1.0f;
    int bk = 2;
    for (int k = 2; k < BAT_SOH_GRID_N - 2; k++) {                /* the 5-point mean, trimmed [2:-2] */
        float d = (gr[k - 2] + gr[k - 1] + gr[k] + gr[k + 1] + gr[k + 2]) / 5.0f;
        if (fabsf(d) > best) { best = fabsf(d); bk = k; }        /* np.argmax: the first of equal values */
    }
    *h = best;
    *v = grid_v(bk);
}

float bat_soh_median(const float *x, int n)
{
    float t[BAT_SOH_NREF];
    if (n <= 0 || n > BAT_SOH_NREF) return NAN;
    memcpy(t, x, (size_t)n * sizeof *t);
    for (int i = 1; i < n; i++)                                   /* insertion sort: n <= 10 */
        for (int j = i; j > 0 && t[j - 1] > t[j]; j--) { float u = t[j]; t[j] = t[j - 1]; t[j - 1] = u; }
    return n & 1 ? t[n / 2] : 0.5f * (t[n / 2 - 1] + t[n / 2]);
}

static const uint8_t REL_OF[BAT_SOH_NREL] = { SOH_F_DQ, SOH_F_ICA_H, SOH_F_CV_TAU, SOH_F_DCIR, SOH_F_EKF };

float bat_soh_predict_rel(const soh_ref_t *r, const float raw[BAT_SOH_NRAW], float x[BAT_SOH_NRAW])
{
    float med[BAT_SOH_NREL], col[BAT_SOH_NREF];
    for (int j = 0; j < BAT_SOH_NREL; j++) {
        for (int i = 0; i < r->n; i++) col[i] = r->ref[i][j];
        med[j] = bat_soh_median(col, r->n);
    }
    /* the model's order: rel_dq_chg_win, ica_chg_v, rel_ica_chg_h, cv_dur_h, rel_cv_tau_h, rel_dcir, T_avg, rel_ekf_ratio */
    x[0] = raw[SOH_F_DQ] / med[0];
    x[1] = raw[SOH_F_ICA_V];
    x[2] = raw[SOH_F_ICA_H] / med[1];
    x[3] = raw[SOH_F_CV_DUR];
    x[4] = raw[SOH_F_CV_TAU] / med[2];
    x[5] = raw[SOH_F_DCIR] / med[3];
    x[6] = raw[SOH_F_TAVG];
    x[7] = raw[SOH_F_EKF] / med[4];
    return bat_soh_predict(x);
}

static void set_f(soh_cycle_t *c, int f, float v)
{
    if (!isfinite(v)) return;
    c->raw[f] = v;
    c->have |= (uint8_t)(1u << f);
}

/* the charge just ended: its features */
static void charge_end(bat_soh_t *s)
{
    soh_cycle_t *c = &s->cyc;
    c->charging = false;
    if (c->next_k > K_WIN_HI) set_f(c, SOH_F_DQ, bat_soh_q_between(c->grid, BAT_SOH_WIN_LO, BAT_SOH_WIN_HI) / s->qnom_ah);
    if (c->next_k >= BAT_SOH_GRID_N) {
        float qn[BAT_SOH_GRID_N], h, v;
        for (int k = 0; k < BAT_SOH_GRID_N; k++) qn[k] = c->grid[k] / s->qnom_ah;
        bat_soh_ica_peak(qn, &h, &v);
        set_f(c, SOH_F_ICA_H, h);
        set_f(c, SOH_F_ICA_V, v);
    }
    if (c->cv_seen && c->cv_s > 3.0f) set_f(c, SOH_F_CV_DUR, c->cv_s / 3600.0f);
    if (c->cv_n > 3.0) {
        double den = c->cv_n * c->cv_stt - c->cv_st * c->cv_st;
        double slope = den != 0.0 ? (c->cv_n * c->cv_sty - c->cv_st * c->cv_sy) / den : 0.0;
        if (slope < 0.0) set_f(c, SOH_F_CV_TAU, (float)(-1.0 / slope / 3600.0));
    }
    c->ended_full = c->cv_seen;
    c->dcir_armed = true;
    c->ekf_armed = c->ended_full;
}

/* the cycle ended (the next charge starts): a reference entry or a model output */
static bool cycle_end(bat_soh_t *s)
{
    soh_cycle_t *c = &s->cyc;
    if (!c->cc_seen && !c->cv_seen) return false;
    if (c->t_n > 0.0f) set_f(c, SOH_F_TAVG, c->t_sum / c->t_n);
    s->ref.cycles++;
    if ((c->have & F_ALL) != F_ALL) return false;
    s->ref.usable++;
    if (s->ref.n < BAT_SOH_NREF) {
        for (int j = 0; j < BAT_SOH_NREL; j++) s->ref.ref[s->ref.n][j] = c->raw[REL_OF[j]];
        for (int j = 0; j < BAT_SOH_NREL; j++) if (!(c->raw[REL_OF[j]] > 0.0f)) return false;   /* a ratio needs a positive reference */
        s->ref.n++;
        s->gen++;
        return true;
    }
    s->ref.last_soh = bat_soh_predict_rel(&s->ref, c->raw, s->last_x);
    s->have_x = true;
    s->gen++;
    return true;
}

bool bat_soh_step(bat_soh_t *s, const soh_in_t *in)
{
    soh_cycle_t *c = &s->cyc;
    bool news = false;
    const float dt = in->dt > 0.0f && in->dt < 10.0f ? in->dt : 0.0f;
    const bool chg = in->mode == BAT_MODE_CHARGE || in->mode == BAT_MODE_CV;
    if (!in->valid) return false;
    if (chg && !c->charging) {                                    /* a charge starts: the last cycle ends */
        news = cycle_end(s);
        cycle_reset(c);
        c->charging = true;
        c->next_k = in->v <= BAT_SOH_GRID_LO ? 0 : -1;            /* the grid needs the charge to start at or below 3.60 V */
    } else if (!chg && c->charging) {
        charge_end(s);
    }
    c->t_sum += in->t_c;
    c->t_n += 1.0f;
    if (c->charging) {
        float q0 = c->q_ah;
        c->q_ah += (in->c > 0.0f ? in->c : 0.0f) * s->qnom_ah * dt / 3600.0f;
        if (in->mode == BAT_MODE_CHARGE) {
            c->cc_seen = true;
            if (c->next_k >= 0) {
                float vp = c->have_prev ? c->v_prev : in->v, qp = c->have_prev ? q0 : c->q_ah;
                while (c->next_k < BAT_SOH_GRID_N && in->v >= grid_v(c->next_k)) {
                    float g = grid_v(c->next_k);
                    c->grid[c->next_k] = in->v > vp && g > vp ? qp + (g - vp) / (in->v - vp) * (c->q_ah - qp) : qp;
                    c->next_k++;
                }
            }
        } else {                                                  /* CV: duration and the current's decay */
            if (c->next_k >= 0)                                   /* grid lines above the last CC sample: reached at CV */
                while (c->next_k < BAT_SOH_GRID_N && in->v >= grid_v(c->next_k)) c->grid[c->next_k++] = c->q_ah;
            if (!c->cv_seen) c->cv_t = 0.0f; else c->cv_t += dt;
            c->cv_seen = true;
            c->cv_s += dt;
            if (in->c > BAT_SOH_REST_C) {
                double t = c->cv_t, y = log((double)in->c * s->qnom_ah);
                c->cv_n += 1.0; c->cv_st += t; c->cv_stt += t * t; c->cv_sy += y; c->cv_sty += t * y;
            }
        }
        c->v_prev = in->v;
        c->have_prev = true;
        return news;
    }
    /* after the charge: the DCIR step and the EKF ratio */
    if (c->dcir_armed && !c->dcir_done) {
        if (!c->dcir_on) {
            if (fabsf(in->c) < BAT_SOH_REST_C) { c->rest_s += dt; c->dcir_v0 = in->v; c->dcir_ocv0 = in->ocv_z; }
            else if (in->c <= -BAT_SOH_DCIR_C && c->rest_s > 0.0f) { c->dcir_on = true; c->dcir_t = dt; }
            else c->rest_s = 0.0f;
        } else if (in->c <= -BAT_SOH_REST_C) {
            c->dcir_t += dt;
            if (c->dcir_t >= BAT_SOH_DCIR_S) {
                set_f(c, SOH_F_DCIR, (c->dcir_v0 - in->v - (c->dcir_ocv0 - in->ocv_z)) / -in->c);
                c->dcir_done = true;
            }
        } else { c->dcir_on = false; c->rest_s = 0.0f; }          /* the load stopped early: wait for the next step */
    }
    if (c->ekf_armed && !c->ekf_done) {
        if (in->c < 0.0f) c->q_dis_ah += -in->c * s->qnom_ah * dt / 3600.0f;
        if (in->z_ekf <= 0.3f) {
            set_f(c, SOH_F_EKF, c->q_dis_ah / s->qnom_ah / (1.0f - in->z_ekf));
            c->ekf_done = true;
        }
    }
    return news;
}

uint8_t bat_soh_source(const bat_soh_t *s, uint32_t n_full)
{
    if (n_full > 0) return SOH_SRC_FULL;
    if (s->ref.n >= BAT_SOH_NREF && s->ref.last_soh > 0.0f) return SOH_SRC_MODEL;
    return SOH_SRC_LEARNING;
}
