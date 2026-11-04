#include "rev_learn.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ================================================================ (a) fuel head */
static bool finite_vec(const float *v, int n)
{
    for (int i = 0; i < n; i++) if (!isfinite(v[i])) return false;
    return true;
}

static float dot(const float *a, const float *b)
{
    float s = 0;
    for (int i = 0; i < REV_NF; i++) s += a[i] * b[i];
    return s;
}

void rev_fuel_reset(rev_fuel_t *L)
{
    memcpy(L->w[0], L->w0, sizeof L->w[0]);
    memcpy(L->w[1], L->w0, sizeof L->w[1]);
    L->live = 0;
    memset(L->A, 0, sizeof L->A);
    memset(L->b, 0, sizeof L->b);
    L->ring_head = L->ring_n = 0;
    L->n = L->commits = L->resets = L->kept = L->failed = L->learned = 0;
    L->mse_live = L->mse_cand = L->mse_prior = 0;
}

void rev_fuel_init(rev_fuel_t *L, const float *w0)
{
    L->w0 = w0;
    rev_fuel_reset(L);
}

float rev_fuel_predict(const rev_fuel_t *L, const float phi[REV_NF]) { return dot(phi, L->w[L->live]); }
float rev_fuel_predict_prior(const rev_fuel_t *L, const float phi[REV_NF]) { return dot(phi, L->w0); }

static void fold(rev_fuel_t *L, const rev_pair_t *p)        /* a pair leaves the holdout and enters the statistics */
{
    for (int i = 0; i < REV_NF; i++) {
        float pi = p->phi[i];
        for (int j = i; j < REV_NF; j++) L->A[i][j] = REV_LAMBDA * L->A[i][j] + pi * p->phi[j];
        L->b[i] = REV_LAMBDA * L->b[i] + pi * p->y;
    }
    L->learned++;
}

bool rev_fuel_add(rev_fuel_t *L, const float phi[REV_NF], float y)
{
    if (!isfinite(y) || !finite_vec(phi, REV_NF)) return false;
    rev_pair_t *slot;
    if (L->ring_n == REV_HOLDOUT) {
        slot = &L->ring[L->ring_head];
        fold(L, slot);
        L->ring_head = (L->ring_head + 1) % REV_HOLDOUT;
    } else {
        slot = &L->ring[(L->ring_head + L->ring_n) % REV_HOLDOUT];
        L->ring_n++;
    }
    memcpy(slot->phi, phi, sizeof slot->phi);
    slot->y = y;
    L->n++;
    return L->n % REV_EVERY == 0 && L->ring_n == REV_HOLDOUT;
}

/* L->M x = L->x in place (M symmetric positive definite, lower triangle used). */
static bool cholesky_solve(rev_fuel_t *L)
{
    for (int i = 0; i < REV_NF; i++) {
        for (int j = 0; j <= i; j++) {
            float s = L->M[i][j];
            for (int k = 0; k < j; k++) s -= L->M[i][k] * L->M[j][k];
            if (i == j) { if (!(s > 1e-6f)) return false; L->M[i][i] = sqrtf(s); }
            else L->M[i][j] = s / L->M[j][j];
        }
    }
    for (int i = 0; i < REV_NF; i++) { float s = L->x[i]; for (int k = 0; k < i; k++) s -= L->M[i][k] * L->x[k]; L->x[i] = s / L->M[i][i]; }
    for (int i = REV_NF - 1; i >= 0; i--) { float s = L->x[i]; for (int k = i + 1; k < REV_NF; k++) s -= L->M[k][i] * L->x[k]; L->x[i] = s / L->M[i][i]; }
    return true;
}

static float holdout_mse(const rev_fuel_t *L, const float *w)
{
    float e = 0;
    for (int n = 0; n < L->ring_n; n++) {
        const rev_pair_t *p = &L->ring[(L->ring_head + n) % REV_HOLDOUT];
        float d = p->y - dot(p->phi, w);
        e += d * d;
    }
    return L->ring_n ? e / (float)L->ring_n : 0.0f;
}

static float dist(const float *a, const float *b)
{
    float s = 0;
    for (int i = 0; i < REV_NF; i++) { float d = a[i] - (b ? b[i] : 0.0f); s += d * d; }
    return sqrtf(s);
}

float rev_fuel_drift(const rev_fuel_t *L)
{
    float n0 = dist(L->w0, NULL);
    return n0 > 0 ? dist(L->w[L->live], L->w0) / n0 : 0.0f;
}

rev_commit_t rev_fuel_commit(rev_fuel_t *L, char *msg, size_t msg_n)
{
    char dummy[8];
    if (!msg) { msg = dummy; msg_n = sizeof dummy; }
    if (L->ring_n < REV_HOLDOUT) { snprintf(msg, msg_n, "waiting for data: %d of %d pairs", L->ring_n, REV_HOLDOUT); return REV_WAIT; }
    for (int i = 0; i < REV_NF; i++) {
        for (int j = 0; j <= i; j++) L->M[i][j] = L->A[j][i];          /* lower triangle from the maintained upper one */
        L->M[i][i] += REV_ALPHA;
        L->x[i] = L->b[i] + REV_ALPHA * L->w0[i];
    }
    if (!cholesky_solve(L) || !finite_vec(L->x, REV_NF)) {
        L->failed++;
        snprintf(msg, msg_n, "kept: solve failed");
        return REV_SOLVE_FAILED;
    }
    const float *live = L->w[L->live];
    float cur = holdout_mse(L, live), cm = holdout_mse(L, L->x), pr = holdout_mse(L, L->w0);
    float n0 = dist(L->w0, NULL);
    float drift = n0 > 0 ? dist(L->x, L->w0) / n0 : 0.0f;
    L->mse_live = cur; L->mse_cand = cm; L->mse_prior = pr;
    uint8_t other = (uint8_t)(L->live ^ 1);
    if (cm < REV_GATE * cur && drift < REV_DRIFT) {
        memcpy(L->w[other], L->x, sizeof L->w[other]);
        __atomic_store_n(&L->live, other, __ATOMIC_RELEASE);          /* the predictor sees either the old or the new set */
        L->commits++;
        snprintf(msg, msg_n, "updated: error %.4f to %.4f (factory %.4f)", (double)cur, (double)cm, (double)pr);
        return REV_ACCEPTED;
    }
    if (cur > REV_RESET * pr) {
        memcpy(L->w[other], L->w0, sizeof L->w[other]);
        __atomic_store_n(&L->live, other, __ATOMIC_RELEASE);
        L->resets++;
        snprintf(msg, msg_n, "reset to factory: %.4f worse than %.4f", (double)cur, (double)pr);
        return REV_RESET_PRIOR;
    }
    L->kept++;
    snprintf(msg, msg_n, "kept: candidate %.4f vs current %.4f", (double)cm, (double)cur);
    return REV_KEPT;
}

/* ================================================================ labels */
void rev_label_reset(rev_label_t *r) { r->head = r->n = 0; }

void rev_label_push(rev_label_t *r, int64_t t_ms, float lph)
{
    if (!isfinite(lph) || lph < 0) return;
    r->t_ms[r->head] = t_ms;
    r->lph[r->head] = lph;
    r->head = (r->head + 1) % REV_LABEL_RING;
    if (r->n < REV_LABEL_RING) r->n++;
}

bool rev_label_mean(const rev_label_t *r, int64_t end_ms, float *out)
{
    double s = 0;
    int cnt = 0;
    int64_t lo = end_ms, hi = end_ms - REV_LABEL_SPAN_MS;
    for (int i = 0; i < r->n; i++) {
        int k = (r->head - 1 - i + REV_LABEL_RING) % REV_LABEL_RING;
        int64_t t = r->t_ms[k];
        if (t > end_ms) continue;
        if (t <= end_ms - REV_LABEL_SPAN_MS) break;                  /* newest first: everything after this is older */
        s += r->lph[k]; cnt++;
        if (t < lo) lo = t;
        if (t > hi) hi = t;
    }
    if (cnt < 2 || hi - lo < 2000) return false;
    float m = (float)(s / cnt);
    *out = m > 100.0f ? 100.0f : m;
    return true;
}

/* ================================================================ (b) airflow constant */
int rev_phys_bin(float rpm) { return rpm < 1500 ? 0 : rpm < 2500 ? 1 : rpm < 3500 ? 2 : 3; }

static void k_reset(rev_k_t *k) { k->sxx = k->sxy = 0; k->k = REV_PHYS_K0; k->n = k->n_sig = 0; }

bool rev_k_ready(const rev_k_t *k) { return k->n_sig >= REV_PHYS_MIN_ROWS && k->sxx > 0; }

void rev_phys_reset_k(rev_phys_t *P)
{
    k_reset(&P->abs);
    for (int i = 0; i < REV_PHYS_BINS; i++) k_reset(&P->calc[i]);
}

void rev_phys_new_drive(rev_phys_t *P) { P->head = P->n = 0; }

void rev_phys_init(rev_phys_t *P)
{
    memset(P, 0, sizeof *P);
    rev_phys_reset_k(P);
}

static void k_learn(rev_k_t *k, float x, float y)
{
    k->sxx = REV_PHYS_LAMBDA * k->sxx + (double)x * x;
    k->sxy = REV_PHYS_LAMBDA * k->sxy + (double)x * y;
    k->n++;
    if (x >= REV_PHYS_X_MIN) k->n_sig++;
    if (rev_k_ready(k)) {
        float kk = (float)(k->sxy / k->sxx);
        if (isfinite(kk) && kk > 0) k->k = kk;
    }
}

static bool phys_x(bool abs_ok, float abs_load, bool load_ok, float load, bool rpm_ok, float rpm, float *x, uint8_t *path)
{
    if (!rpm_ok || !isfinite(rpm) || rpm < 0 || rpm > 16000) return false;
    if (abs_ok && isfinite(abs_load) && abs_load >= 0 && abs_load <= 400) { *x = abs_load * rpm; *path = 0; return true; }
    if (load_ok && isfinite(load) && load >= 0 && load <= 100) { *x = load * rpm; *path = (uint8_t)(1 + rev_phys_bin(rpm)); return true; }
    return false;
}

static rev_k_t *k_of(rev_phys_t *P, uint8_t path) { return path == 0 ? &P->abs : &P->calc[path - 1]; }

bool rev_phys_row(rev_phys_t *P, int64_t t_ms, bool abs_ok, float abs_load, bool load_ok, float load,
                  bool rpm_ok, float rpm, bool maf_ok, float maf, float *maf_mean_est)
{
    float x;
    uint8_t path;
    if (!maf_ok || !isfinite(maf) || maf < 0 || maf > 655.35f) return false;
    if (!phys_x(abs_ok, abs_load, load_ok, load, rpm_ok, rpm, &x, &path)) return false;
    if (P->n && t_ms - P->t_ms[(P->head - 1 + REV_PHYS_RING) % REV_PHYS_RING] > 10000) P->n = 0;   /* a gap: start the window over */
    P->t_ms[P->head] = t_ms; P->x[P->head] = x; P->y[P->head] = maf; P->path[P->head] = path;
    P->head = (P->head + 1) % REV_PHYS_RING;
    if (P->n < REV_PHYS_RING) P->n++;
    /* 5 s means over the rows of the same path in (t - 5 s, t] */
    double sx = 0, sy = 0;
    int cnt = 0;
    for (int i = 0; i < P->n; i++) {
        int k = (P->head - 1 - i + REV_PHYS_RING) % REV_PHYS_RING;
        if (P->t_ms[k] <= t_ms - 5000) break;
        if ((P->path[k] == 0) != (path == 0)) continue;              /* a car switching between paths: keep them apart */
        sx += P->x[k]; sy += P->y[k]; cnt++;
    }
    float mx = (float)(sx / cnt), my = (float)(sy / cnt);
    rev_k_t *k = k_of(P, path);
    float pred = k->k * mx;                                          /* prequential: score with k before it learns this row */
    k_learn(k, mx, my);
    if (maf_mean_est) *maf_mean_est = pred;
    return path == 0 || rev_k_ready(k);
}

bool rev_phys_estimate(const rev_phys_t *P, bool abs_ok, float abs_load, bool load_ok, float load, bool rpm_ok, float rpm,
                       float *maf_gps, bool *ready)
{
    float x;
    uint8_t path;
    if (!phys_x(abs_ok, abs_load, load_ok, load, rpm_ok, rpm, &x, &path)) return false;
    const rev_k_t *k = path == 0 ? &P->abs : &P->calc[path - 1];
    bool r = rev_k_ready(k);
    if (ready) *ready = r;
    if (path != 0 && !r) return false;                               /* no factory k for calculated load */
    *maf_gps = k->k * x;
    return true;
}

/* ================================================================ (c) picker */
void rev_pick_init(rev_pick_t *p) { memset(p, 0, sizeof *p); }

static void ema(rev_pick_t *p, int i, float err)
{
    float e2 = err * err;
    if (!isfinite(e2)) return;
    if (p->n[i] == 0) p->e[i] = e2;
    else p->e[i] += (e2 - p->e[i]) / (float)REV_PICK_ROWS;
    p->n[i]++;
}

void rev_pick_score(rev_pick_t *p, float label, bool prior_ok, float prior, bool learned_ok, float learned, bool phys_ok, float phys)
{
    if (!isfinite(label)) return;
    if (prior_ok) ema(p, REV_SRC_PRIOR, prior - label);
    if (learned_ok) ema(p, REV_SRC_LEARNED, learned - label);
    if (phys_ok) ema(p, REV_SRC_PHYS, phys - label);
}

rev_src_t rev_pick_choose(const rev_pick_t *p, bool phys_ready, bool learned_committed)
{
    static const rev_src_t order[3] = {REV_SRC_PHYS, REV_SRC_LEARNED, REV_SRC_PRIOR};   /* tie order */
    int best = -1;
    for (int i = 0; i < 3; i++) {
        rev_src_t s = order[i];
        if (p->n[s] < REV_PICK_MIN) continue;
        if (best < 0 || p->e[s] < p->e[best]) best = s;
    }
    if (best >= 0) return (rev_src_t)best;
    if (phys_ready) return REV_SRC_PHYS;
    if (learned_committed) return REV_SRC_LEARNED;
    return REV_SRC_PRIOR;
}

const char *rev_src_name(rev_src_t s) { return s == REV_SRC_PHYS ? "phys" : s == REV_SRC_LEARNED ? "learned" : "model"; }
