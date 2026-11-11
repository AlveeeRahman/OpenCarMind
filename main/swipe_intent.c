/* swipe_intent.c: OS-ELM swipe predictor, page prior, latency histograms and the A/B gate. See swipe_intent.h.
 *
 * ---------------------------------------------------------------------------------------------------------------
 * 1. The model (extreme learning machine, ELM)
 *    x in R^D (features), a bias input x_D = 1. Hidden layer h_j = tanh(sum_i W_ji x_i), j = 0..H-1, with W drawn
 *    once at random and never trained. Output y = beta^T h, beta in R^{H x K}. Only beta is learned, so training is
 *    linear least squares in h: the hidden layer is a fixed random feature map. W is not stored: hidden() replays the
 *    same xorshift32 sequence from the seed every time (144 draws, about 1 us), so W costs no flash and no RAM.
 *
 * 2. Recursive least squares with forgetting (derived here so the code stands on its own)
 *    After n samples (h_i, t_i), t_i the one-hot label, we want
 *        beta_n = argmin_B  sum_{i<=n} lam^{n-i} |t_i - B^T h_i|^2  +  lam^n delta |B|_F^2 .
 *    Setting the gradient to zero: A_n beta_n = b_n with
 *        A_n = lam^n delta I + sum lam^{n-i} h_i h_i^T  =  lam A_{n-1} + h_n h_n^T ,      A_0 = delta I
 *        b_n = sum lam^{n-i} h_i t_i^T                 =  lam b_{n-1} + h_n t_n^T ,      b_0 = 0.
 *    Keep P_n = A_n^{-1} instead of A_n. Sherman-Morrison on (lam A_{n-1}) + h h^T, with Q = (lam A_{n-1})^{-1}
 *    = P_{n-1}/lam:
 *        P_n = Q - Q h h^T Q / (1 + h^T Q h).
 *    Write u = P_{n-1} h and d = lam + h^T u. Then Q h = u/lam and 1 + h^T Q h = d/lam, so
 *        P_n = (P_{n-1} - u u^T / d) / lam .                                   (P update, rank-1, symmetric)
 *    For beta: beta_n = P_n b_n = P_n (lam b_{n-1} + h t^T) = P_n (lam A_{n-1} beta_{n-1} + h t^T).
 *    Since lam A_{n-1} = A_n - h h^T:  beta_n = beta_{n-1} + P_n h (t^T - h^T beta_{n-1}).
 *    And P_n h = (u - u (h^T u)/d)/lam = u (d - h^T u)/(lam d) = u/d. So with the prior error e = t - beta_{n-1}^T h:
 *        beta_n = beta_{n-1} + (u/d) e^T .                                     (beta update)
 *    Cost per sample: u = P h (H^2), d (H), P update (H(H+1)/2 x 2), beta update (H K). This is the K-output form of
 *    the OS-ELM update with one sample per chunk (Liang et al. 2006, as recalled, UNVERIFIED) with forgetting added.
 *
 * 3. Numerical safety
 *    - P is stored as its upper triangle only (H(H+1)/2 = 136 floats). The update writes P_ij once for i <= j, so P is
 *      exactly symmetric by construction: there is no lower half to drift away ("symmetrise" is free).
 *    - Forgetting divides P by lam each step. In directions the gestures never excite, P grows as lam^-n ("wind-up").
 *      Guard: if trace(P) would exceed trace(P_0) = H/delta, skip the 1/lam for that step (lam = 1 locally). P then
 *      never exceeds its initial scale.
 *    - Reset to P_0 when d <= tiny, when any diagonal of P goes <= 0 (P must stay positive definite), or when any value
 *      is not finite. beta is zeroed only if it is not finite itself.
 *    - Features are clipped, so no input can overflow tanh or the products.
 *
 * 4. Latency metric: fixed histograms, not P-square
 *    Two arms x 96 bins x 5 ms (uint16) = 384 B. P-square (Jain & Chlamtac 1985, as recalled, UNVERIFIED) would need
 *    5 heights + 5 positions per arm (about 40 B) but it gives no error bound for small n and cannot feed a rank test.
 *    The histogram gives the p95 within half a bin (2.5 ms), and the Mann-Whitney U of the two arms straight from the
 *    bins (section 5). That second use decided it: the gate needs an uncertainty figure, and the histogram is the
 *    smallest structure that gives one on the device. Bins halve when an arm reaches 1024 samples (recency decay).
 *
 * 5. Gate uncertainty: Mann-Whitney U from the binned counts
 *    U = #{(a, b): a < b} + 0.5 #{a == b} over all pairs of an A and a B sample (same bin = tie). Under "no difference"
 *    E[U] = nA nB / 2 and Var[U] = nA nB (nA + nB + 1) / 12 (no tie correction; ties lower the true variance, so the
 *    test is conservative). z = (U - E) / sqrt(Var) > 1.645 is one-sided 5 %: A tends to settle sooner than B. The
 *    gate also needs p95_A < p95_B and ready_A > 80 %. */
#include "swipe_intent.h"
#include <stdio.h>
#include <string.h>

#define H      SI_H
#define K      SI_K
#define D      SI_D
#define DI     (SI_D + 1)                 /* with the bias input */
#define NP     (H * (H + 1) / 2)
#define LAMBDA 0.995f
#define DELTA  1e-2f
#define P0     (1.0f / DELTA)
#define TRACE_MAX ((float)H * P0)
#define BINS   96
#define BIN_MS 5
#define HALVE_AT 1024
#define WARMUP 30                         /* RLS updates before arm A may act and metrics count */
#define MAX_PER_ARM (3 * SI_MIN_PER_ARM)  /* "A looks better but not significant": keep sampling up to this */
#define KEEP_B_PCT 10                     /* KEEP: shadow sampling rate that keeps checking */

enum { PH_IDLE = 0, PH_DOWN, PH_LIFTED };

/* everything that is persisted, contiguous, so save/load are one copy */
struct si_model {
    float    beta[H * K];                 /* beta[j*K + k] */
    float    P[NP];                       /* upper triangle, row-major */
    uint32_t seed, updates;
    uint16_t v_p95a, v_p95b, v_ready, v_na, v_nb;   /* the verdict's numbers */
    int16_t  v_z100;
    uint8_t  prior[SI_MAX_PAGES][SI_MAX_PAGES];
    uint8_t  state, reason, npages, pad_;
};

struct si_impl {
    struct si_model m;
    float    x[D];                        /* features of the gesture in flight */
    uint32_t rng, t0, tl, tp, t_lift, t_last_swipe;
    uint16_t hist[2][BINS];
    uint16_t rdy_hit[2], rdy_n[2];
    uint16_t acc_hit, acc_n, resets, b_since;
    int16_t  x0, y0, xl, yl, xp, yp;
    uint8_t  page0, arm, phase, pred_cls, pred_conf, fresh, early, have_x;
};

typedef char si_size_check[(sizeof(struct si_impl) == sizeof(si_t)) ? 1 : -1];   /* no pointers: same on 32/64 bit */
#define S(self) ((struct si_impl *)(void *)(self))
#define CS(self) ((const struct si_impl *)(const void *)(self))

/* ---------- small math ---------- */
static inline int fin(float v) { return v - v == 0.0f; }   /* false for NaN and +-Inf (needs IEEE, no -ffast-math) */
static inline float clipf(float v, float lim) { return v < -lim ? -lim : v > lim ? lim : v; }

static inline uint32_t xs32(uint32_t *r) { uint32_t v = *r; v ^= v << 13; v ^= v >> 17; v ^= v << 5; return *r = v; }

/* tanh by the [7/6] continued-fraction (Lambert) rational, clamped to +-1. Max error about 1e-4 on the real line
   (test_tanh); 9 multiply-adds and one divide, no table. Input clamped first so x^6 cannot overflow. */
static float si_tanh(float x)
{
    x = clipf(x, 7.0f);
    float x2 = x * x;
    float num = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
    float den = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f));
    return clipf(num / den, 1.0f);
}

/* log2(1 + v) for v >= 0 from the float's exponent and mantissa (error < 0.09): a feature squash, not a measurement */
static float log2_1p(float v)
{
    union { float f; uint32_t u; } b = { 1.0f + v };
    return (float)((int32_t)(b.u >> 23) - 127) + (float)(b.u & 0x7FFFFF) * (1.0f / 8388608.0f);
}

static inline int pidx(int i, int j) { return i * H - i * (i - 1) / 2 + (j - i); }   /* i <= j */

/* ---------- model ---------- */
static void hidden(const struct si_impl *s, const float *x, float *h)
{
    uint32_t r = s->m.seed;
    for (int j = 0; j < H; j++) {
        float a = (float)(int32_t)xs32(&r);                    /* bias weight, then D input weights, all in [-1, 1) */
        for (int i = 0; i < D; i++) a += (float)(int32_t)xs32(&r) * x[i];
        h[j] = si_tanh(a * (1.0f / 2147483648.0f));
    }
}

static void output(const struct si_impl *s, const float *h, float *y)
{
    for (int k = 0; k < K; k++) {
        float a = 0;
        for (int j = 0; j < H; j++) a += s->m.beta[j * K + k] * h[j];
        y[k] = a;
    }
}

static void p_reset(struct si_impl *s)
{
    memset(s->m.P, 0, sizeof s->m.P);
    for (int i = 0; i < H; i++) s->m.P[pidx(i, i)] = P0;
}

/* one RLS step (sections 2 and 3 of the comment above) */
static void rls_update(struct si_impl *s, const float *h, const float *t)
{
    float u[H], y[K], tr = 0, inv;
    int ok = 1;
    memset(u, 0, sizeof u);
    for (int i = 0, n = 0; i < H; i++)                         /* u = P h from the triangle: each P_ij used twice */
        for (int j = i; j < H; j++, n++) { u[i] += s->m.P[n] * h[j]; if (j != i) u[j] += s->m.P[n] * h[i]; }
    float d = LAMBDA;
    for (int i = 0; i < H; i++) d += h[i] * u[i];
    if (!(d > 1e-6f) || !fin(d)) goto reset;
    inv = 1.0f / d;
    output(s, h, y);
    for (int n = 0; n < H * K; n++) { s->m.beta[n] += u[n / K] * inv * (t[n % K] - y[n % K]); ok &= fin(s->m.beta[n]); }
    for (int i = 0, n = 0; i < H; i++)                         /* P - u u^T / d; P stays positive definite */
        for (int j = i; j < H; j++, n++) {
            float v = s->m.P[n] - u[i] * u[j] * inv;
            s->m.P[n] = v;
            if (j == i) { tr += v; ok &= v > 0 && fin(v); }
        }
    if (!ok) goto reset;
    if (tr * (1.0f / LAMBDA) <= TRACE_MAX)                     /* forget, unless that winds P up past trace(P_0) */
        for (int n = 0; n < NP; n++) s->m.P[n] *= 1.0f / LAMBDA;
    s->m.updates++;
    return;
reset:
    p_reset(s);
    for (int n = 0; n < H * K; n++) if (!fin(s->m.beta[n])) { memset(s->m.beta, 0, sizeof s->m.beta); break; }
    s->resets++;
}

/* features at time t from the samples so far. Each one is scaled to its typical swipe magnitude at 40 ms (about 40 px,
   1.5 px/ms) so the random tanh units see inputs of order 1 and work in their nonlinear range; at 1/120 they stay nearly
   linear and cannot separate a tap (|dx| small) from both swipe directions. Host sweep: 90.5 -> 95.6 % accuracy. */
static void features(struct si_impl *s, uint32_t t)
{
    static const float SC[D] = { 1.0f / 30, 1.0f / 30, 2.0f, 2.0f, 2.0f / 480, 1.0f, 0.2f };
    float dt = (float)(s->tl - s->tp), dx = (float)(s->xl - s->x0), dy = (float)(s->yl - s->y0);
    float vx = dx, vy = dy;                                    /* no earlier sample: mean velocity since touch-down */
    if (dt > 0) { vx = (float)(s->xl - s->xp); vy = (float)(s->yl - s->yp); }
    else dt = (float)(t - s->t0) + 1.0f;
    uint32_t since = s->t_last_swipe ? t - s->t_last_swipe : 600000u;
    float raw[D] = { dx, dy, vx / dt, vy / dt, (float)s->x0 - 240.0f,
                     s->m.npages > 1 ? 2.0f * s->page0 / (s->m.npages - 1) - 1.0f : 0.0f,
                     log2_1p((float)(since > 600000u ? 600000u : since)) - 10.0f };
    for (int i = 0; i < D; i++) s->x[i] = clipf(raw[i] * SC[i], 4.0f);
    s->have_x = 1;
}

static int8_t dest_of(const struct si_impl *s, int cls)
{
    int d = s->page0 + (cls == SI_NEXT) - (cls == SI_PREV);
    return (int8_t)(cls >= 0 && d >= 0 && d < s->m.npages ? d : -1);
}

static si_pred_t predict(struct si_impl *s, uint32_t t)
{
    float h[H], y[K];
    features(s, t);
    hidden(s, s->x, h);
    output(s, h, y);
    int a = y[1] > y[0], b = !a;                               /* a = argmax, b = runner-up */
    if (y[2] > y[a]) { b = a; a = 2; } else if (y[2] > y[b]) b = 2;
    float m = y[a] - y[b];
    s->pred_cls = (uint8_t)a;
    s->pred_conf = (uint8_t)(m >= 1.0f ? 255 : m * 255.0f + 0.5f);
    si_pred_t p = { (int8_t)a, dest_of(s, a), s->pred_conf, true };
    return p;
}

/* ---------- histograms ---------- */
static uint32_t hist_n(const uint16_t *hb) { uint32_t n = 0; for (int b = 0; b < BINS; b++) n += hb[b]; return n; }

static uint16_t hist_p95(const uint16_t *hb)
{
    uint32_t n = hist_n(hb), c = 0, rank = (95 * n + 99) / 100;   /* nearest rank: ceil(0.95 n) */
    int b = 0;
    if (!n) return 0;
    while (b < BINS - 1 && (c += hb[b]) < rank) b++;
    return (uint16_t)(b * BIN_MS + BIN_MS / 2);                  /* the bin centre: within 2.5 ms of the exact value */
}

static void hist_add(struct si_impl *s, int arm, uint32_t ms)
{
    uint32_t b = ms / BIN_MS;
    uint16_t *hb = s->hist[arm];
    hb[b < BINS ? b : BINS - 1]++;                             /* last bin = overflow (>= 475 ms) */
    if (hist_n(hb) >= HALVE_AT) {                              /* recency decay */
        for (int i = 0; i < BINS; i++) hb[i] = (uint16_t)((hb[i] + 1) / 2);
        s->rdy_hit[arm] /= 2;
        s->rdy_n[arm] /= 2;
    }
}

/* Mann-Whitney z x 100 (section 5), > 0 when arm A tends to settle sooner */
static int16_t mw_z100(const uint16_t *a, const uint16_t *b)
{
    float na = (float)hist_n(a), nb = (float)hist_n(b), u = 0, above = nb;
    if (na < 1 || nb < 1) return 0;
    for (int i = 0; i < BINS; i++) { above -= b[i]; u += (float)a[i] * (above + 0.5f * (float)b[i]); }
    float m = u - 0.5f * na * nb, z2 = m * m / (na * nb * (na + nb + 1) * (1.0f / 12.0f)), g = z2 + 1;
    for (int k = 0; k < 20; k++) g = 0.5f * (g + z2 / g);      /* sqrt by Newton, no libm */
    g = g > 320 ? 320 : g;
    return (int16_t)(m < 0 ? -g * 100 : g * 100);
}

/* ---------- gate ---------- */
uint8_t si_metrics_decide(const si_metrics_t *m, uint8_t *reason)
{
    uint8_t r = SI_R_NONE, st = SI_LEARNING;
    if (m->n_a >= SI_MIN_PER_ARM && m->n_b >= SI_MIN_PER_ARM) {
        r = m->ready_a_pm <= 800 ? SI_R_READY_LOW : !(m->p95_a_ms < m->p95_b_ms) ? SI_R_SLOWER
          : m->z100 < 165 ? SI_R_UNCERTAIN : SI_R_PASS;
        st = r == SI_R_PASS ? SI_KEEP : SI_OFF;
        /* A ahead on p95 and ready, but the rank test is not sure yet: one more look at MAX_PER_ARM, then decide */
        if (r == SI_R_UNCERTAIN && (m->n_a < MAX_PER_ARM || m->n_b < MAX_PER_ARM)) st = SI_LEARNING;
    }
    if (reason) *reason = r;
    return st;
}

static void coin(struct si_impl *s)
{
    uint32_t v = xs32(&s->rng) % 100u;
    s->arm = (uint8_t)(s->m.state == SI_LEARNING ? v >= 50 : s->m.state == SI_KEEP ? v < KEEP_B_PCT : 1);
}

static void gate_step(struct si_impl *s)
{
    si_metrics_t m;
    uint8_t r, st;
    if (s->m.state == SI_OFF || (s->m.state == SI_KEEP && s->b_since < SI_MIN_PER_ARM)) return;   /* KEEP: re-check
                                                                               after every 50 new shadow samples */
    si_metrics_get((const si_t *)(void *)s, &m);
    st = si_metrics_decide(&m, &r);
    if (s->m.state == SI_LEARNING && st == SI_LEARNING) return;
    if (s->m.state == SI_KEEP) { st = st == SI_OFF ? SI_OFF : SI_KEEP; r = st == SI_OFF ? SI_R_RECHECK_FAIL : SI_R_PASS; }
    s->m.state = st; s->m.reason = r;
    s->m.v_p95a = m.p95_a_ms; s->m.v_p95b = m.p95_b_ms; s->m.v_ready = m.ready_a_pm;
    s->m.v_na = m.n_a; s->m.v_nb = m.n_b; s->m.v_z100 = m.z100;
    s->b_since = 0;
}

/* ---------- public ---------- */
void si_init(si_t *self, uint8_t n_pages, uint32_t seed)
{
    struct si_impl *s = S(self);
    memset(s, 0, sizeof *s);
    s->m.npages = n_pages < 1 ? 1 : n_pages > SI_MAX_PAGES ? SI_MAX_PAGES : n_pages;
    s->m.seed = seed ? seed : 0x9E3779B9u;
    s->rng = s->m.seed ^ 0xA5A5A5A5u;
    if (!s->rng) s->rng = 1;
    p_reset(s);
    s->arm = 1;                                                 /* warm-up: shadow */
}

void si_retest(si_t *self)
{
    struct si_impl *s = S(self);
    memset(s->hist, 0, sizeof s->hist);
    memset(s->rdy_hit, 0, sizeof s->rdy_hit);
    memset(s->rdy_n, 0, sizeof s->rdy_n);
    memset(&s->m.v_p95a, 0, 6 * sizeof(uint16_t));             /* v_p95a .. v_z100 */
    s->m.state = SI_LEARNING; s->m.reason = SI_R_NONE;
    s->b_since = 0;
    coin(s);
}

void si_begin(si_t *self, uint32_t t_ms, int16_t x, int16_t y, uint8_t page)
{
    struct si_impl *s = S(self);
    s->phase = PH_DOWN;
    s->t0 = s->tl = s->tp = t_ms;
    s->x0 = s->xl = s->xp = x;
    s->y0 = s->yl = s->yp = y;
    s->page0 = page < s->m.npages ? page : (uint8_t)(s->m.npages - 1);
    s->pred_cls = (uint8_t)SI_NONE; s->pred_conf = 0; s->early = 0; s->have_x = 0; s->fresh = 0;
}

si_pred_t si_move(si_t *self, uint32_t t_ms, int16_t x, int16_t y)
{
    struct si_impl *s = S(self);
    si_pred_t none = { SI_NONE, -1, 0, false };
    if (s->phase != PH_DOWN) return none;
    if (t_ms != s->tl) { s->xp = s->xl; s->yp = s->yl; s->tp = s->tl; }
    s->xl = x; s->yl = y; s->tl = t_ms;
    if (s->have_x || t_ms - s->t0 < SI_T_PREDICT) return none;
    s->early = 1;
    return predict(s, t_ms);
}

si_pred_t si_end(si_t *self, uint32_t t_ms, int16_t x, int16_t y, uint8_t fresh_mask)
{
    struct si_impl *s = S(self);
    si_pred_t p = { SI_NONE, -1, 0, false };
    if (s->phase != PH_DOWN) return p;
    si_move(self, t_ms, x, y);
    if (!s->have_x) predict(s, t_ms);                          /* lifted before 40 ms: a late (not early) prediction */
    s->phase = PH_LIFTED;
    s->t_lift = t_ms;
    s->fresh = fresh_mask;
    p.cls = (int8_t)s->pred_cls; p.dest = dest_of(s, s->pred_cls); p.conf = s->pred_conf; p.valid = true;
    return p;                                                  /* the prediction in force at lift */
}

void si_abort(si_t *self) { S(self)->phase = PH_IDLE; }

void si_settle(si_t *self, uint32_t t_ms, uint8_t final_page)
{
    struct si_impl *s = S(self);
    int lab = final_page == s->page0 + 1 ? SI_NEXT : final_page + 1 == s->page0 ? SI_PREV :
              final_page == s->page0 ? SI_STAY : SI_NONE;
    if (s->phase != PH_LIFTED || lab == SI_NONE) { s->phase = PH_IDLE; return; }   /* a 2+ page jump is no outcome */
    s->phase = PH_IDLE;

    float h[H], t[K] = { 0, 0, 0 };
    t[lab] = 1;
    hidden(s, s->x, h);                                        /* recomputed: cheaper than keeping 64 B of h */
    int hit = s->pred_cls == lab, counted = s->m.updates >= WARMUP && s->m.state != SI_OFF;
    if (s->acc_n >= 256) { s->acc_n /= 2; s->acc_hit /= 2; }
    s->acc_n++; s->acc_hit = (uint16_t)(s->acc_hit + hit);
    if (lab != SI_STAY) {
        if (counted) {
            int a = s->arm;
            s->rdy_n[a]++;
            s->rdy_hit[a] = (uint16_t)(s->rdy_hit[a] + (s->early && hit && ((s->fresh >> final_page) & 1u)));
            hist_add(s, a, t_ms - s->t_lift);
            if (a && s->b_since < 0xFFFF) s->b_since++;
        }
        s->t_last_swipe = s->t_lift | 1u;
        uint8_t *row = s->m.prior[s->page0];
        if (++row[final_page] == 255) for (int j = 0; j < SI_MAX_PAGES; j++) row[j] /= 2;   /* decay */
    }
    rls_update(s, h, t);
    if (counted) gate_step(s);
    if (s->m.updates >= WARMUP) coin(s);
}

bool si_acts(const si_t *self)
{
    const struct si_impl *s = CS(self);
    return s->m.state != SI_OFF && s->m.updates >= WARMUP && s->arm == 0;
}

int8_t si_hint_page(const si_t *self, uint8_t page, uint8_t *pct)
{
    const struct si_impl *s = CS(self);
    unsigned sum = 0, best = 0;
    int arg = -1;
    if (pct) *pct = 0;
    if (page >= s->m.npages) return -1;
    for (int j = 0; j < s->m.npages; j++) { sum += s->m.prior[page][j]; if (s->m.prior[page][j] > best) { best = s->m.prior[page][j]; arg = j; } }
    if (arg >= 0 && pct) *pct = (uint8_t)(100u * best / sum);
    return (int8_t)arg;
}

void si_metrics_get(const si_t *self, si_metrics_t *m)
{
    const struct si_impl *s = CS(self);
    memset(m, 0, sizeof *m);
    m->state = s->m.state; m->reason = s->m.reason;
    m->updates = s->m.updates; m->resets = s->resets;
    m->acc_pm = s->acc_n ? (uint16_t)(1000u * s->acc_hit / s->acc_n) : 0;
    if (s->m.state == SI_OFF) {                                 /* frozen at the decision */
        m->n_a = s->m.v_na; m->n_b = s->m.v_nb; m->p95_a_ms = s->m.v_p95a; m->p95_b_ms = s->m.v_p95b;
        m->ready_a_pm = s->m.v_ready; m->z100 = s->m.v_z100;
        return;
    }
    m->n_a = (uint16_t)hist_n(s->hist[0]); m->n_b = (uint16_t)hist_n(s->hist[1]);
    m->p95_a_ms = hist_p95(s->hist[0]); m->p95_b_ms = hist_p95(s->hist[1]);
    m->ready_a_pm = s->rdy_n[0] ? (uint16_t)(1000u * s->rdy_hit[0] / s->rdy_n[0]) : 0;
    m->ready_b_pm = s->rdy_n[1] ? (uint16_t)(1000u * s->rdy_hit[1] / s->rdy_n[1]) : 0;
    m->z100 = mw_z100(s->hist[0], s->hist[1]);
}

/* "Smart swipe: learning 37/100, p95 212/230 ms, ready 84 %" (p95 with / without the learner; the count only while
   learning: warm-up gestures, then completed A/B swipes against the 2 x 50 needed) */
int si_metrics_line(const si_t *self, char *buf, size_t n)
{
    static const char *const R[] = { "learning", "on", "off (slower)", "off (unsure)", "off (not ready)", "off (re-check)" };
    si_metrics_t m;
    char prog[24] = "";
    si_metrics_get(self, &m);
    unsigned k = m.state == SI_KEEP ? 1u : m.state == SI_OFF ? (m.reason >= 2 && m.reason < 6 ? m.reason : 2u) : 0u;
    if (m.state == SI_LEARNING)
        snprintf(prog, sizeof prog, m.updates < WARMUP ? " warm-up %u/%u" : " %u/%u",
                 m.updates < WARMUP ? (unsigned)m.updates : (unsigned)(m.n_a + m.n_b), m.updates < WARMUP ? WARMUP : 2 * SI_MIN_PER_ARM);
    return snprintf(buf, n, "Smart swipe: %s%s, p95 %u/%u ms, ready %u %%", R[k], prog, m.p95_a_ms, m.p95_b_ms,
                    (m.ready_a_pm + 5u) / 10u);
}

/* ---------- persistence ----------
   Blob: magic, dims, fw_tag (3 x u32), struct si_model (824 B), FNV-1a of all that (u32) = SI_BLOB_BYTES. Written and
   checked in the caller's buffer, so there is no 840 B copy on the task stack; the buffer may be unaligned. */
#define SI_MAGIC 0x31495353u                                     /* "SSI1": bump the digit when the layout changes */
#define SI_DIMS  ((uint32_t)(H | K << 8 | D << 16 | 1u << 24))   /* hidden, outputs, inputs, version */
#define HDR      12u
typedef char si_blob_check[(HDR + sizeof(struct si_model) + 4 == SI_BLOB_BYTES) ? 1 : -1];

static uint32_t fnv1a(const uint8_t *b, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

size_t si_save(const si_t *self, void *buf, size_t cap, uint32_t fw_tag)
{
    uint8_t *b = (uint8_t *)buf;
    uint32_t hdr[3] = { SI_MAGIC, SI_DIMS, fw_tag };
    if (!b || cap < SI_BLOB_BYTES) return 0;
    memcpy(b, hdr, HDR);
    memcpy(b + HDR, &CS(self)->m, sizeof(struct si_model));
    uint32_t sum = fnv1a(b, SI_BLOB_BYTES - 4);
    memcpy(b + SI_BLOB_BYTES - 4, &sum, 4);
    return SI_BLOB_BYTES;
}

/* call right after si_init (the histograms are not persisted) */
bool si_load(si_t *self, const void *buf, size_t n, uint32_t fw_tag)
{
    struct si_impl *s = S(self);
    const uint8_t *b = (const uint8_t *)buf, *m = b + HDR;
    if (!b || n != SI_BLOB_BYTES || rd32(b) != SI_MAGIC || rd32(b + 4) != SI_DIMS ||
        rd32(b + SI_BLOB_BYTES - 4) != fnv1a(b, SI_BLOB_BYTES - 4)) return false;
    for (int i = 0; i < H * K + NP; i++) {                     /* beta then P: all finite, P's diagonal positive */
        float v;
        memcpy(&v, m + 4 * i, 4);
        int j = i - H * K, diag = 0;
        for (int r = 0; r < H && j >= 0; r++) diag |= j == pidx(r, r);
        if (!fin(v) || (diag && !(v > 0))) return false;
    }
    uint8_t st = m[offsetof(struct si_model, state)], np = m[offsetof(struct si_model, npages)];
    if (st > SI_OFF || np != s->m.npages) return false;
    memcpy(&s->m, m, sizeof s->m);
    if (rd32(b + 8) != fw_tag || st == SI_LEARNING) si_retest(self);   /* new firmware: keep the model, re-test */
    else coin(s);                                                    /* same firmware: the verdict stands */
    return true;
}
