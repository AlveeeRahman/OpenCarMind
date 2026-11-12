/* bat_anom_core: see bat_anom_core.h. The body is anom_host.c's an_step line for line; the changes are marked there. */
#include "bat_anom_core.h"
#include <math.h>

static float sgn(float x) { return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f); }

void an_init(const an_params_t *p, an_state_t *s, float samp_s)
{
    float *z = (float *)s;
    for (unsigned i = 0; i < sizeof(an_state_t) / sizeof(float); i++) z[i] = 0.0f;
    for (int j = 0; j < AN_NF; j++) {
        s->med[j] = p->m0[j]; s->mad[j] = p->d0[j]; s->mu[j] = p->m0[j]; s->var[j] = (1.4826f * p->d0[j]) * (1.4826f * p->d0[j]);
    }
    s->samp_s = samp_s > 0.0f ? samp_s : 1.0f;
    s->win_n = roundf((float)AN_WIN / s->samp_s);
    s->min_n = 10.0f / s->samp_s;
}

/* inputs: voltage, current (A, + charge), temperature, dt, valid flag, and the SoC filter's innovation,
 * z and v1 (so OCV-related sag can be formed without a second model) */
void an_step(const an_params_t *p, an_state_t *s, float v, float i, float t_c, float dt, int valid,
             float innov, float ocv_z, float v1, an_out_t *o)
{
    const float c = i / p->qnom_ah;
    const float ft = fminf(1.5f, fmaxf(0.5f, 1.0f - p->kt * (t_c - 25.0f)));
    if (s->init < 0.5f) { s->tm = t_c; s->tamb = t_c; s->v_prev = v; s->c_prev = c; s->init = 1.0f; }
    /* thermal model and ambient tracker (ambient follows the temperature only at rest) */
    s->tm += dt * ((s->tamb - s->tm) / p->tau_th + p->k_heat * c * c);
    const int rest0 = fabsf(c) < p->c_rest;
    s->t_rest_run = rest0 ? s->t_rest_run + dt : 0.0f;
    const int rest = rest0 && s->t_rest_run >= p->t_rest_min;
    if (rest0) s->tamb += dt / (3.0f * 3600.0f) * (t_c - s->tamb);

    s->n += 1.0f;
    if (valid) {
        s->n_valid += 1.0f; s->s_e += innov; s->s_e2 += innov * innov;
        const float dv = v - s->v_prev, dc = c - s->c_prev;
        if (s->have_prev > 0.5f && fabsf(dv) > p->v_spike && fabsf(dc) < 0.1f) s->n_spk += 1.0f;
        if (s->have_prev > 0.5f && fabsf(dc) > p->c_step) {
            s->n_step += 1.0f;
            s->s_rstep += (dv / dc) / (((c >= 0.0f) ? p->r0c : p->r0d) * ft);
        }
        if (fabsf(c) > p->c_load) {
            s->n_load += 1.0f;
            s->s_rload += ((v - ocv_z - v1) / c) / (((c >= 0.0f) ? p->r0c : p->r0d) * ft);
        }
        if (rest) {
            if (s->n_rest < 0.5f) { s->v_rest0 = v; s->t_rest0 = s->n; }
            s->n_rest += 1.0f; s->s_crest += c; s->v_rest1 = v; s->t_rest1 = s->n;
        }
        s->v_prev = v; s->c_prev = c; s->have_prev = 1.0f;
    } else {
        s->n_spk += 1.0f;                                   /* a missing / invalid read counts as a glitch */
    }
    if (c < -p->c_load) { s->n_dis += 1.0f; s->s_cdis += -c; }
    s->s_tres += t_c - s->tm;

    o->win_done = 0; o->a_rz = o->a_g = o->a_ae = 0;
    if (s->n < s->win_n) return;

    /* ---- window end: features and masks ---- */
    float f[AN_NF], m[AN_NF];
    const float nv = fmaxf(s->n_valid, 1.0f);
    if (s->n_valid > s->min_n) {
        s->ema_e += p->a_slow * (s->s_e / nv - s->ema_e);
        s->ema_r += p->a_slow * (sqrtf(s->s_e2 / nv) - s->ema_r);
    }
    f[0] = s->ema_e;                                        m[0] = 1.0f;
    f[1] = s->ema_r;                                        m[1] = 1.0f;
    const int rest_ok = s->n_rest > 0.8f * s->win_n;
    f[2] = rest_ok ? (s->v_rest1 - s->v_rest0) / fmaxf((s->t_rest1 - s->t_rest0) * s->samp_s, 1.0f) * 3600.0f : 0.0f;  m[2] = rest_ok;
    f[3] = rest_ok ? s->s_crest / s->n_rest : 0.0f;         m[3] = rest_ok;
    f[4] = s->s_tres / s->n;                                m[4] = 1.0f;
    if (s->n_load > s->min_n) {
        if (s->sag_ok < 0.5f) { s->ema_sag = s->s_rload / s->n_load; s->sag_ok = 1.0f; }
        else s->ema_sag += p->a_slow * (s->s_rload / s->n_load - s->ema_sag);
    }
    f[5] = s->ema_sag;                                      m[5] = s->sag_ok;
    f[6] = s->n_step > 0.5f ? s->s_rstep / s->n_step : 0.0f;  m[6] = s->n_step > 0.5f;
    f[7] = s->n_dis > s->min_n ? s->s_cdis / s->n_dis : 0.0f;  m[7] = s->n_dis > s->min_n;
    f[8] = s->n_spk;                                        m[8] = 1.0f;

    /* ---- detectors ---- */
    float rz = 0.0f, g = 0.0f, nm = 0.0f, u[AN_NF];
    for (int j = 0; j < AN_NF; j++) {
        const float dev = f[j] - s->med[j];
        const float scale = 1.4826f * fmaxf(s->mad[j], p->d_min[j]);
        float zr = dev / scale;
        if (p->side[j] > 0.5f && zr < 0.0f) zr = 0.0f;
        zr = fabsf(zr) * m[j] / p->thr_f[j];
        if (zr > rz) rz = zr;
        const float sd = sqrtf(fmaxf(s->var[j], (1.4826f * p->d_min[j]) * (1.4826f * p->d_min[j])));
        float zg = (f[j] - s->mu[j]) / sd;
        if (p->side[j] > 0.5f && zg < 0.0f) zg = 0.0f;
        const float zgn = fabsf(zg) * m[j] / p->thr_gf[j];
        if (zgn > g) g = zgn;
        nm += m[j];
        u[j] = fminf(5.0f, fmaxf(-5.0f, zg)) * m[j];
    }
    float h[AN_AEH], ae = 0.0f;
    for (int k = 0; k < AN_AEH; k++) {
        float a = p->ae_b1[k];
        for (int j = 0; j < AN_NF; j++) a += p->ae_w1[AN_NF * k + j] * u[j];
        h[k] = tanhf(a);
    }
    for (int j = 0; j < AN_NF; j++) {
        float r = p->ae_b2[j];
        for (int k = 0; k < AN_AEH; k++) r += p->ae_w2[AN_AEH * j + k] * h[k];
        o->ae_err[j] = (r - u[j]) * (r - u[j]) * m[j];
        ae += o->ae_err[j];
    }
    ae /= fmaxf(nm, 1.0f);

    s->nwin += 1.0f;
    const int warm = s->nwin <= p->warmup_win;
    s->run_rz = (!warm && rz > p->thr_rz) ? s->run_rz + 1.0f : 0.0f;
    s->run_g = (!warm && g > p->thr_g) ? s->run_g + 1.0f : 0.0f;
    s->run_ae = (!warm && ae > p->thr_ae) ? s->run_ae + 1.0f : 0.0f;
    o->a_rz = s->run_rz >= p->k_consec; o->a_g = s->run_g >= p->k_consec; o->a_ae = s->run_ae >= p->k_consec;
    s->alarm = (o->a_rz || o->a_g || o->a_ae) ? 1.0f : 0.0f;

    /* ---- poisoning-resistant updates ---- */
    const int ok = (s->alarm < 0.5f) && (rz < p->upd_frac * p->thr_rz) && (g < p->upd_frac * p->thr_g);
    if (ok || warm) {
        const float em = warm ? 4.0f * p->eta_med : p->eta_med, eg = warm ? 4.0f * p->eta_g : p->eta_g;
        for (int j = 0; j < AN_NF; j++) {
            if (m[j] < 0.5f) continue;                       /* fixed bound, skip invalid features */
            const float dev = f[j] - s->med[j];
            const float sc = fmaxf(s->mad[j], p->d_min[j]);
            s->med[j] += em * sc * sgn(dev);
            s->mad[j] += p->eta_mad * (warm ? 4.0f : 1.0f) * sc * sgn(fabsf(dev) - s->mad[j]);
            if (s->mad[j] < p->d_min[j]) s->mad[j] = p->d_min[j];
            const float d2 = f[j] - s->mu[j];
            s->mu[j] += eg * d2;
            s->var[j] += eg * (d2 * d2 - s->var[j]);
        }
    }
    for (int j = 0; j < AN_NF; j++) { o->f[j] = f[j]; o->mask[j] = m[j]; }
    o->s_rz = rz; o->s_g = g; o->s_ae = ae; o->win_done = 1;
    /* reset window accumulators (first 18 floats of the state) */
    float *acc = (float *)s;
    for (int j = 0; j < 18; j++) acc[j] = 0.0f;
}

const char *an_class_name(const an_out_t *o)
{
    static const char *const N[AN_NF] = { "voltage model drift", "voltage model drift", "self-discharge", "rest current leak",
                                          "unexpected heating", "resistance rise", "resistance rise", "heavy load",
                                          "gauge read glitches" };
    int k = 0;
    for (int j = 1; j < AN_NF; j++) if (o->ae_err[j] > o->ae_err[k]) k = j;
    return N[k];
}
