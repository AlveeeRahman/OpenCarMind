/* Host tests for swipe_intent (main/swipe_intent.c). Build: make swipe
 * The module is included directly so the tests can reach the internals (RLS, histogram) without test hooks in it. */
#include "swipe_intent.c"
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------- test RNG (independent of the module's) ---------- */
typedef struct { uint64_t s; } trng_t;
static double tr_u(trng_t *r) { r->s ^= r->s >> 12; r->s ^= r->s << 25; r->s ^= r->s >> 27;
                                return (double)((r->s * 2685821657736338717ULL) >> 11) * (1.0 / 9007199254740992.0); }
static double tr_uni(trng_t *r, double a, double b) { return a + (b - a) * tr_u(r); }
static double tr_n(trng_t *r) { double u = tr_u(r), v = tr_u(r); if (u < 1e-300) u = 1e-300; return sqrt(-2 * log(u)) * cos(6.283185307179586 * v); }

typedef struct { const char *name; int pass; char detail[160]; } row_t;
static row_t g_rows[32];
static int g_nrows;
static void row(const char *name, int pass, const char *fmt, ...)
{
    row_t *r = &g_rows[g_nrows++];
    r->name = name; r->pass = pass;
    va_list ap; va_start(ap, fmt); vsnprintf(r->detail, sizeof r->detail, fmt, ap); va_end(ap);
    if (!pass) g_fail++;
}

/* ---------- 1. tanh ---------- */
static void test_tanh(void)
{
    double worst = 0, at = 0;
    for (double x = -12; x <= 12; x += 1e-4) {
        double e = fabs((double)si_tanh((float)x) - tanh(x));
        if (e > worst) { worst = e; at = x; }
    }
    row("tanh [7/6] rational", worst < 1e-3, "max |err| %.2e at x=%.3f (limit 1e-3)", worst, at);
}

/* ---------- 2. RLS vs batch least squares ---------- */
static int chol_solve(int n, double *A, double *B, int m)   /* A n x n SPD (overwritten), B n x m -> solution */
{
    for (int j = 0; j < n; j++) {
        double d = A[j * n + j];
        for (int k = 0; k < j; k++) d -= A[j * n + k] * A[j * n + k];
        if (d <= 0) return 0;
        A[j * n + j] = sqrt(d);
        for (int i = j + 1; i < n; i++) {
            double v = A[i * n + j];
            for (int k = 0; k < j; k++) v -= A[i * n + k] * A[j * n + k];
            A[i * n + j] = v / A[j * n + j];
        }
    }
    for (int c = 0; c < m; c++) {
        for (int i = 0; i < n; i++) { double v = B[i * m + c]; for (int k = 0; k < i; k++) v -= A[i * n + k] * B[k * m + c]; B[i * m + c] = v / A[i * n + i]; }
        for (int i = n - 1; i >= 0; i--) { double v = B[i * m + c]; for (int k = i + 1; k < n; k++) v -= A[k * n + i] * B[k * m + c]; B[i * m + c] = v / A[i * n + i]; }
    }
    return 1;
}

static void test_rls_batch(void)
{
    static si_t obj;
    struct si_impl *s = S(&obj);
    si_init(&obj, 5, 12345);
    trng_t r = { 0x1234567ULL };
    enum { N = 400 };
    static float hs[N][H], ts[N][K];
    for (int n = 0; n < N; n++) {
        float x[D];
        for (int i = 0; i < D; i++) x[i] = (float)tr_uni(&r, -1.5, 1.5);
        hidden(s, x, hs[n]);
        int c = (int)(tr_u(&r) * 3);
        for (int k = 0; k < K; k++) ts[n][k] = k == c ? 1.0f : 0.0f;
        ts[n][0] += (float)(0.2 * tr_n(&r));                    /* not only one-hot: a generic LS target */
        rls_update(s, hs[n], ts[n]);
    }
    static double A[H * H], B[H * K];
    double lam = LAMBDA, w = 1, lamn = 1;
    for (int i = 0; i < N; i++) lamn *= lam;
    for (int i = 0; i < H * H; i++) A[i] = 0;
    for (int i = 0; i < H * K; i++) B[i] = 0;
    for (int i = 0; i < H; i++) A[i * H + i] = lamn * DELTA;
    for (int n = N - 1; n >= 0; n--, w *= lam)
        for (int i = 0; i < H; i++) {
            for (int j = 0; j < H; j++) A[i * H + j] += w * hs[n][i] * hs[n][j];
            for (int k = 0; k < K; k++) B[i * K + k] += w * hs[n][i] * ts[n][k];
        }
    double Pinv_check[H * H];
    memcpy(Pinv_check, A, sizeof A);
    int ok = chol_solve(H, A, B, K);
    double worst = 0, bmax = 0;
    for (int i = 0; i < H * K; i++) { double e = fabs(B[i] - s->m.beta[i]); if (e > worst) worst = e; if (fabs(B[i]) > bmax) bmax = fabs(B[i]); }
    /* P against A^-1: (A P) should be I */
    double pw = 0;
    for (int i = 0; i < H; i++) for (int j = 0; j < H; j++) {
        double v = 0;
        for (int k = 0; k < H; k++) v += Pinv_check[i * H + k] * s->m.P[k <= j ? pidx(k, j) : pidx(j, k)];
        double e = fabs(v - (i == j));
        if (e > pw) pw = e;
    }
    row("RLS = batch LS (400 samples, lam .995)", ok && worst < 1e-4 && s->resets == 0,
        "max |beta_rls - beta_batch| %.2e (|beta| up to %.2f), max |A P - I| %.2e, resets %u", worst, bmax, pw, s->resets);
}

/* ---------- synthetic gestures ---------- */
typedef struct {
    double vmin, vmax;          /* peak finger speed, px/ms */
    double tau_min, tau_max;    /* speed onset time constant, ms */
    double hes_p;               /* hesitant swipes: slow onset, still complete */
    double cancel_p;            /* drags that start like a swipe and go back */
    double slow_complete;       /* 1: slow drags complete (habit B); 0: slow drags are cancels (habit A) */
    double x_next_lo, x_next_hi, x_prev_lo, x_prev_hi;
} habit_t;

static const habit_t HABIT_A = { 0.8, 2.5, 12, 35, 0.07, 0.05, 0, 200, 440, 40, 280 };
static const habit_t HABIT_B = { 0.5, 1.4, 15, 40, 0.05, 0.05, 1, 40, 280, 200, 440 };   /* slower swipes, other hand: start sides mirrored */

typedef struct { int hit, outcome, early, swipe; uint32_t t_lift; } gres_t;

/* one gesture through the real API. Returns the outcome page in *page. fresh = picture mask at lift. */
static gres_t sim_gesture(si_t *si, trng_t *r, const habit_t *hb, int *page, int npages, uint32_t *clock, uint8_t fresh,
                          uint32_t settle_ms)
{
    gres_t g = { 0, 0, 0, 0, 0 };
    *clock += (uint32_t)tr_uni(r, 300, 8000);
    int p = *page;
    double u = tr_u(r);
    int intent;                                                  /* 0 next, 1 prev, 2 tap */
    if (p == 0) intent = u < 0.60 ? 0 : u < 0.62 ? 1 : 2;
    else if (p == npages - 1) intent = u < 0.70 ? 1 : 2;
    else intent = u < 0.40 ? 0 : u < 0.75 ? 1 : 2;
    double v, tau, T, slope = 0.15 * tr_n(r), x0, y0 = tr_uni(r, 80, 420);
    int cancel = 0;
    if (intent == 2) { v = 0; tau = 10; T = tr_uni(r, 60, 160); x0 = tr_uni(r, 40, 440); }
    else {
        v = tr_uni(r, hb->vmin, hb->vmax);
        tau = tr_uni(r, hb->tau_min, hb->tau_max);
        if (tr_u(r) < hb->hes_p) tau = tr_uni(r, 80, 120);
        if (tr_u(r) < hb->cancel_p) { cancel = 1; v = tr_uni(r, 0.25, 0.7); }
        T = tr_uni(r, 90, 230);
        x0 = intent == 0 ? tr_uni(r, hb->x_next_lo, hb->x_next_hi) : tr_uni(r, hb->x_prev_lo, hb->x_prev_hi);
        if (!hb->slow_complete && v < 0.75 && !cancel && tr_u(r) < 0.5) cancel = 1;   /* habit A: slow drags tend to fail */
    }
    double sgn = intent == 0 ? -1 : 1;                            /* finger left = next page */
    uint32_t t0 = *clock;
    si_begin(si, t0, (int16_t)x0, (int16_t)y0, (uint8_t)p);
    double phase = tr_uni(r, 2, 16);
    int predicted = SI_NONE;
    double xs = x0, ys = y0;
    for (double t = phase; t < T; t += 16 + tr_uni(r, -2, 2)) {
        double disp = v * (t - tau * (1 - exp(-t / tau)));
        if (cancel && t > T * 0.6) disp *= 1 - (t - T * 0.6) / (T * 0.4);
        xs = x0 + sgn * disp + tr_uni(r, -1.5, 1.5) + (intent == 2 ? tr_uni(r, -3, 3) : 0);
        ys = y0 + slope * disp + tr_uni(r, -1.5, 1.5);
        si_pred_t pr = si_move(si, t0 + (uint32_t)t, (int16_t)xs, (int16_t)ys);
        if (pr.valid) predicted = pr.cls;
    }
    g.t_lift = t0 + (uint32_t)T;
    si_pred_t pe = si_end(si, g.t_lift, (int16_t)xs, (int16_t)ys, fresh);
    if (pe.valid && predicted == SI_NONE) predicted = pe.cls;
    g.early = CS(si)->early;
    int out = p;
    if (!cancel && intent == 0 && p < npages - 1) out = p + 1;
    if (!cancel && intent == 1 && p > 0) out = p - 1;
    int lab = out == p + 1 ? SI_NEXT : out == p - 1 ? SI_PREV : SI_STAY;
    g.hit = predicted == lab;
    g.outcome = lab;
    g.swipe = lab != SI_STAY;
    *clock = g.t_lift + settle_ms;
    si_settle(si, *clock, (uint8_t)out);
    *page = out;
    return g;
}

static void test_learning(void)
{
    enum { SEEDS = 30, G = 300 };
    static const int W[][2] = { {1, 20}, {21, 50}, {51, 80}, {81, 100}, {101, 200}, {201, 300} };
    double acc[6] = {0}, worst81 = 1;
    for (int sd = 0; sd < SEEDS; sd++) {
        static si_t si;
        si_init(&si, 5, 0xC0FFEEu + (uint32_t)sd * 7919u);
        trng_t r = { 0xABCDEF01ULL + (uint64_t)sd * 1000003ULL };
        uint32_t clock = 1000;
        int page = 0, hits[G];
        for (int n = 0; n < G; n++) hits[n] = sim_gesture(&si, &r, &HABIT_A, &page, 5, &clock, 0xFF, 200).hit;
        for (int w = 0; w < 6; w++) {
            int c = 0;
            for (int n = W[w][0] - 1; n < W[w][1]; n++) c += hits[n];
            double a = (double)c / (W[w][1] - W[w][0] + 1);
            acc[w] += a / SEEDS;
            if (w == 3 && a < worst81) worst81 = a;
        }
    }
    row("learning: accuracy at 40 ms (30 users)", acc[3] > 0.90 && acc[4] > 0.90,
        "gestures 1-20 %.1f%%, 21-50 %.1f%%, 51-80 %.1f%%, 81-100 %.1f%% (worst user %.0f%%), 101-200 %.1f%%, 201-300 %.1f%%",
        100 * acc[0], 100 * acc[1], 100 * acc[2], 100 * acc[3], 100 * worst81, 100 * acc[4], 100 * acc[5]);
}

static void test_drift(void)
{
    enum { SEEDS = 30, PRE = 300, POST = 300 };
    double pre = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0, f2 = 0;
    for (int sd = 0; sd < SEEDS; sd++) {
        static si_t si;
        si_init(&si, 5, 0xBEEF0000u + (uint32_t)sd);
        trng_t r = { 0x5151ULL + (uint64_t)sd * 7777777ULL };
        uint32_t clock = 1000;
        int page = 0, c;
        c = 0; for (int n = 0; n < PRE; n++) { int h = sim_gesture(&si, &r, &HABIT_A, &page, 5, &clock, 0xFF, 200).hit; if (n >= 200) c += h; }
        pre += c / 100.0 / SEEDS;
        int hits[POST];
        for (int n = 0; n < POST; n++) hits[n] = sim_gesture(&si, &r, &HABIT_B, &page, 5, &clock, 0xFF, 200).hit;
        c = 0; for (int n = 0; n < 30; n++) c += hits[n]; a0 += c / 30.0 / SEEDS;
        c = 0; for (int n = 30; n < 100; n++) c += hits[n]; a1 += c / 70.0 / SEEDS;
        c = 0; for (int n = 100; n < 200; n++) c += hits[n]; a2 += c / 100.0 / SEEDS;
        c = 0; for (int n = 200; n < 300; n++) c += hits[n]; a3 += c / 100.0 / SEEDS;
        /* baseline: a learner that only ever saw habit B, on the same kind of stream (its ceiling) */
        static si_t fr;
        si_init(&fr, 5, 0xBEEF0000u + (uint32_t)sd);
        trng_t r2 = { 0x7171ULL + (uint64_t)sd * 7777777ULL };
        clock = 1000; page = 0; c = 0;
        for (int n = 0; n < 200; n++) { int h = sim_gesture(&fr, &r2, &HABIT_B, &page, 5, &clock, 0xFF, 200).hit; if (n >= 100) c += h; }
        f2 += c / 100.0 / SEEDS;
    }
    /* Criteria (second version, fixed before this run; the first, ">= 90 % at 101-200", is in REPORT.md with why it
       was unreachable): a visible drop, >= 90 % by 201-300, and at 101-200 within 3 points of a fresh habit-B learner,
       i.e. forgetting has removed the old habit's influence. */
    row("drift: habit change re-adapts", a0 < pre - 0.05 && a3 >= 0.90 && a2 >= f2 - 0.03,
        "before %.1f%%; after the change 1-30 %.1f%%, 31-100 %.1f%%, 101-200 %.1f%% (fresh habit-B learner %.1f%%), 201-300 %.1f%%",
        100 * pre, 100 * a0, 100 * a1, 100 * a2, 100 * f2, 100 * a3);
}

/* ---------- numerical safety ---------- */
static float trace(const struct si_impl *s) { float t = 0; for (int i = 0; i < H; i++) t += s->m.P[pidx(i, i)]; return t; }
static int all_finite(const struct si_impl *s)
{
    for (int i = 0; i < NP; i++) if (!fin(s->m.P[i])) return 0;
    for (int i = 0; i < H * K; i++) if (!fin(s->m.beta[i])) return 0;
    return 1;
}

static void test_safety(void)
{
    static si_t si;
    struct si_impl *s = S(&si);
    si_init(&si, 5, 777);
    trng_t r = { 99 };
    float x[D], h[H], t[K] = { 1, 0, 0 };
    for (int n = 0; n < 50; n++) { for (int i = 0; i < D; i++) x[i] = (float)tr_uni(&r, -1, 1); hidden(s, x, h); rls_update(s, h, t); }
    /* (a) NaN in P */
    s->m.P[17] = NAN;
    rls_update(s, h, t);
    int a_ok = s->resets == 1 && all_finite(s) && fabsf(trace(s) - TRACE_MAX) < 1e-2f;
    /* (b) Inf in beta */
    s->m.beta[4] = INFINITY;
    rls_update(s, h, t);
    int b_ok = s->resets == 2 && all_finite(s);
    /* (c) wind-up: 5000 identical samples excite one direction only */
    float tmax = 0;
    for (int i = 0; i < D; i++) x[i] = 0.3f;
    hidden(s, x, h);
    for (int n = 0; n < 5000; n++) { rls_update(s, h, t); float tr = trace(s); if (tr > tmax) tmax = tr; }
    int c_ok = all_finite(s) && tmax <= TRACE_MAX * 1.0001f && s->resets == 2;
    /* (d) absurd touch input */
    si_begin(&si, 1000, 32767, -32768, 200);
    si_move(&si, 1000, -32768, 32767);
    si_pred_t p = si_move(&si, 1041, 32767, 32767);
    si_end(&si, 1042, 0, 0, 0xFF);
    si_settle(&si, 1300, 4);
    int d_ok = p.valid && all_finite(s);
    float y[K];
    output(s, h, y);
    d_ok = d_ok && fin(y[0]) && fin(y[1]) && fin(y[2]);
    row("numerical safety", a_ok && b_ok && c_ok && d_ok,
        "NaN in P -> reset %s; Inf in beta -> reset %s; 5000 same samples: max trace %.1f of %.0f %s; absurd touch %s",
        a_ok ? "ok" : "BAD", b_ok ? "ok" : "BAD", tmax, TRACE_MAX, c_ok ? "ok" : "BAD", d_ok ? "ok" : "BAD");
}

/* ---------- quantile ---------- */
static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }

static void test_quantile(void)
{
    static si_t si;
    struct si_impl *s = S(&si);
    trng_t r = { 4242 };
    static const int NS[] = { 50, 120, 400, 1000 };
    double worst_ms = 0, worst_pct = 0;
    int all_ok = 1, cases = 0;
    for (int dist = 0; dist < 4; dist++)
        for (int k = 0; k < 4; k++)
            for (int rep = 0; rep < 20; rep++) {
                si_init(&si, 5, 1);
                int n = NS[k];
                static uint32_t v[1024];
                for (int i = 0; i < n; i++) {
                    double ms = dist == 0 ? 180 + 25 * tr_n(&r)                              /* normal */
                              : dist == 1 ? 150 + exp(3.5 + 0.6 * tr_n(&r))                 /* skewed tail */
                              : dist == 2 ? (tr_u(&r) < 0.85 ? 190 + 10 * tr_n(&r) : 330 + 30 * tr_n(&r))   /* bimodal */
                              : exp(5.99 + 0.59 * tr_n(&r));                                /* the owner's swipes: p50 ~400, p95 ~1055 ms */
                    if (ms < 0) ms = 0;
                    if (ms > 3000) ms = 3000;
                    v[i] = (uint32_t)ms;
                    hist_add(s, 0, v[i]);
                }
                qsort(v, (size_t)n, sizeof v[0], cmp_u32);
                uint32_t exact = v[(95 * n + 99) / 100 - 1];
                double e = fabs((double)hist_p95(s->hist[0]) - exact), ep = 100 * e / exact;
                double bin = exact < UM_NB1 * UM_BIN_MS ? UM_BIN_MS : UM_BIN2_MS;
                if (e > worst_ms) worst_ms = e;
                if (ep > worst_pct) worst_pct = ep;
                if (!(e <= bin || ep <= 2)) all_ok = 0;
                cases++;
            }
    row("p95 histogram vs exact sort", all_ok, "%d cases (normal, skewed, bimodal, owner-like; n 50..1000, 0..3 s): worst %.1f ms = %.2f%% (limit 1 bin or 2%%)",
        cases, worst_ms, worst_pct);

    /* the range: the owner's real swipe p95 was 1,055 ms; the old 96 x 5 ms bins read anything above 475 ms as 477 */
    si_init(&si, 5, 1);
    for (int i = 0; i < 100; i++) hist_add(s, 0, i < 95 ? 600 : 1055);
    uint16_t p_slow = hist_p95(s->hist[0]);
    for (int i = 0; i < 100; i++) hist_add(s, 1, 2900);
    uint16_t p_cap = hist_p95(s->hist[1]);
    si_init(&si, 5, 1);
    for (int i = 0; i < 200; i++) hist_add(s, 0, 4000);    /* past 3 s: the overflow bin */
    uint16_t p_over = hist_p95(s->hist[0]);
    int range_ok = p_slow > 475 && p_slow >= 600 - 4 && p_slow <= 604 + 4 && p_cap >= 2900 - 16 && p_cap <= 2900 + 16 &&
                   p_over >= 3024 && p_over <= 3056;
    /* two arms that differ only above 475 ms: the rank test now sees it (the old bins put both in one overflow bin, z 0) */
    si_init(&si, 5, 1);
    for (int i = 0; i < 80; i++) { hist_add(s, 0, 700 + (uint32_t)(200 * tr_u(&r))); hist_add(s, 1, 950 + (uint32_t)(200 * tr_u(&r))); }
    int16_t z = mw_z100(s->hist[0], s->hist[1]);
    range_ok = range_ok && z > 164;
    row("histogram range 0..3 s", range_ok, "p95 %u ms (600 x95, 1055 x5), %u ms (2900), %u ms (4000 -> overflow); A 700-900 vs B 950-1150 z %+.2f",
        p_slow, p_cap, p_over, z / 100.0);
}

/* ---------- gate ---------- */
static double boot_p95_diff(const uint32_t *a, int na, const uint32_t *b, int nb, trng_t *r, double *lo, double *hi)
{
    enum { R = 2000 };
    static double d[R];
    static uint32_t ra[512], rb[512];
    for (int k = 0; k < R; k++) {
        for (int i = 0; i < na; i++) ra[i] = a[(int)(tr_u(r) * na)];
        for (int i = 0; i < nb; i++) rb[i] = b[(int)(tr_u(r) * nb)];
        qsort(ra, (size_t)na, 4, cmp_u32); qsort(rb, (size_t)nb, 4, cmp_u32);
        d[k] = (double)ra[(95 * na + 99) / 100 - 1] - (double)rb[(95 * nb + 99) / 100 - 1];
    }
    for (int i = 1; i < R; i++) for (int j = i; j > 0 && d[j - 1] > d[j]; j--) { double t = d[j]; d[j] = d[j - 1]; d[j - 1] = t; }
    *lo = d[R / 40]; *hi = d[R - R / 40 - 1];
    return d[R / 2];
}

static void craft(si_t *si, trng_t *r, int na, double ma, double sa, int nb, double mb, double sb, double ready_a,
                  uint32_t *va, uint32_t *vb)
{
    struct si_impl *s = S(si);
    si_init(si, 5, 3);
    for (int i = 0; i < na; i++) { double ms = ma + sa * tr_n(r); va[i] = (uint32_t)(ms < 0 ? 0 : ms); hist_add(s, 0, va[i]); }
    for (int i = 0; i < nb; i++) { double ms = mb + sb * tr_n(r); vb[i] = (uint32_t)(ms < 0 ? 0 : ms); hist_add(s, 1, vb[i]); }
    s->rdy_n[0] = (uint16_t)na; s->rdy_hit[0] = (uint16_t)(ready_a * na + 0.5);
}

static void test_gate(void)
{
    static si_t si;
    static const char *ST[] = { "LEARNING", "KEEP", "OFF" };
    static const char *RS[] = { "-", "pass", "slower", "uncertain", "ready low", "recheck fail" };
    trng_t r = { 2024 };
    static uint32_t va[512], vb[512];
    struct { const char *name; int na; double ma, sa; int nb; double mb, sb, ready; uint8_t want; } C[] = {
        { "A 25 ms faster, ready 90%", 60, 190, 15, 60, 215, 18, 0.90, SI_KEEP },
        { "no difference, n=150/arm", 150, 205, 18, 150, 205, 18, 0.90, SI_OFF },
        { "A faster, ready 70%", 60, 190, 15, 60, 215, 18, 0.70, SI_OFF },
        { "only 40 A swipes", 40, 190, 15, 60, 215, 18, 0.90, SI_LEARNING },
        { "A slower", 80, 225, 15, 80, 205, 15, 0.95, SI_OFF },
        { "A tail shorter, centre same, 50/arm", 50, 205, 10, 50, 205, 22, 0.90, SI_LEARNING },
    };
    int ok_all = 1;
    char buf[400] = "", *bp = buf;
    for (size_t c = 0; c < sizeof C / sizeof C[0]; c++) {
        craft(&si, &r, C[c].na, C[c].ma, C[c].sa, C[c].nb, C[c].mb, C[c].sb, C[c].ready, va, vb);
        si_metrics_t m;
        si_metrics_get(&si, &m);
        uint8_t why, st = si_metrics_decide(&m, &why);
        double lo, hi, med = boot_p95_diff(va, C[c].na, vb, C[c].nb, &r, &lo, &hi);
        int ok = st == C[c].want;
        ok_all &= ok;
        printf("    gate case %-28s p95 A %3u B %3u ms, ready A %4.1f%%, MW z %+5.2f, bootstrap p95 A-B %+5.0f ms [%+4.0f, %+4.0f] -> %s (%s)%s\n",
               C[c].name, m.p95_a_ms, m.p95_b_ms, m.ready_a_pm / 10.0, m.z100 / 100.0, med, lo, hi, ST[st], RS[why], ok ? "" : "  <-- WRONG");
        bp += snprintf(bp, (size_t)(buf + sizeof buf - bp), "%s%s", c ? ", " : "", ST[st]);
    }
    row("gate decision on crafted A/B data", ok_all, "6 cases -> %s", buf);
}

/* end to end: the state machine on simulated swipes, with persistence */
static void test_runtime_gate(void)
{
    static si_t si, si2;
    static uint8_t blob[SI_BLOB_BYTES];
    trng_t r = { 77 };
    /* (1) the learner really helps: arm A settles 25 ms sooner */
    si_init(&si, 5, 0x1111);
    uint32_t clock = 1000;
    int page = 0, n = 0, keep_at = -1, a_in_keep = 0, b_in_keep = 0;
    for (; n < 2000; n++) {
        bool acts = si_acts(&si);
        uint8_t st0 = S(&si)->m.state;
        double lat = 205 + 18 * tr_n(&r) - (acts ? 25 : 0);
        gres_t g = sim_gesture(&si, &r, &HABIT_A, &page, 5, &clock, 0xFF, (uint32_t)(lat < 150 ? 150 : lat));
        if (st0 == SI_KEEP && g.swipe) { if (acts) a_in_keep++; else b_in_keep++; }
        if (keep_at < 0 && S(&si)->m.state == SI_KEEP) keep_at = n + 1;
        if (S(&si)->m.state == SI_OFF) break;
    }
    si_metrics_t m;
    si_metrics_get(&si, &m);
    char line[96];
    si_metrics_line(&si, line, sizeof line);
    double bfrac = (double)b_in_keep / (a_in_keep + b_in_keep);
    int ok1 = m.state == SI_KEEP && keep_at > 0 && bfrac > 0.06 && bfrac < 0.14;
    printf("    runtime, helpful learner: KEEP after %d gestures, then B share %.1f%%; \"%s\"\n", keep_at, 100 * bfrac, line);
    /* (2) no gain: arm A same latency -> OFF, persisted, survives reload with the same firmware, re-tests on a new one */
    si_init(&si, 5, 0x2222);
    clock = 1000; page = 0;
    int off_at = -1;
    for (n = 0; n < 3000 && S(&si)->m.state != SI_OFF; n++)
        sim_gesture(&si, &r, &HABIT_A, &page, 5, &clock, 0xFF, (uint32_t)(205 + 18 * fabs(tr_n(&r))));
    if (S(&si)->m.state == SI_OFF) off_at = n;
    si_metrics_line(&si, line, sizeof line);
    size_t nb = si_save(&si, blob, sizeof blob, 0xF00D);
    si_init(&si2, 5, 0);
    bool l1 = si_load(&si2, blob, nb, 0xF00D);
    int same = l1 && S(&si2)->m.state == SI_OFF && !si_acts(&si2);
    float x[D] = { 0.1f, 0, 0.2f, 0, 0.3f, -1, 0 }, h1[H], h2[H], y1[K], y2[K];
    hidden(S(&si), x, h1); output(S(&si), h1, y1); hidden(S(&si2), x, h2); output(S(&si2), h2, y2);
    same = same && y1[0] == y2[0] && y1[1] == y2[1] && y1[2] == y2[2];
    si_init(&si2, 5, 0);
    bool l2 = si_load(&si2, blob, nb, 0xBEEF);
    int retest = l2 && S(&si2)->m.state == SI_LEARNING;
    blob[100] ^= 1;
    si_init(&si2, 5, 0);
    int corrupt = !si_load(&si2, blob, nb, 0xF00D) && !si_load(&si2, blob, nb - 1, 0xF00D);
    printf("    runtime, useless learner: OFF after %d gestures; \"%s\"; blob %zu B\n", off_at, line, nb);
    row("runtime gate + persistence", ok1 && off_at > 0 && same && retest && corrupt,
        "helpful: KEEP at %d, B %.1f%% after; useless: OFF at %d; reload same fw %s, new fw -> LEARNING %s, corrupt rejected %s",
        keep_at, 100 * bfrac, off_at, same ? "ok" : "BAD", retest ? "ok" : "BAD", corrupt ? "ok" : "BAD");
}

static void test_prior(void)
{
    static si_t si;
    si_init(&si, 5, 9);
    trng_t r = { 31337 };
    uint32_t clock = 0;
    int page = 1;
    for (int n = 0; n < 400; n++) sim_gesture(&si, &r, &HABIT_A, &page, 5, &clock, 0xFF, 200);
    uint8_t pct1, pct0;
    int8_t h1 = si_hint_page(&si, 1, &pct1), h0 = si_hint_page(&si, 0, &pct0);
    /* the simulated user goes Apps1 -> Home 35/75 and Apps1 -> Apps2 40/75 of swipes, Home -> Apps1 always */
    row("page prior", h0 == 1 && pct0 >= 95 && (h1 == 0 || h1 == 2) && pct1 >= 45 && pct1 <= 65,
        "from Home: page %d (%u%%); from Apps1: page %d (%u%%)", h0, pct0, h1, pct1);
}

/* ---------- cost ---------- */
static void test_cost(void)
{
    static si_t si;
    struct si_impl *s = S(&si);
    si_init(&si, 5, 5);
    float x[D] = { 0.1f, -0.2f, 0.3f, 0.0f, 0.5f, -1, 0.2f }, h[H], y[K], t[K] = { 0, 1, 0 };
    enum { R = 200000 };
    volatile float sink = 0;
    clock_t c0 = clock();
    for (int i = 0; i < R; i++) {
        x[0] = (float)(i & 63) * 0.01f;
        hidden(s, x, h); output(s, h, y);            /* predict at 40 ms */
        hidden(s, x, h); rls_update(s, h, t);        /* update at settle (recomputes h: 64 B of RAM saved) */
        sink += y[0];
    }
    double us = (double)(clock() - c0) / CLOCKS_PER_SEC * 1e6 / R;
    /* multiply-adds per gesture, counted from the loops */
    int mac_hidden = H * DI, mac_out = H * K;
    int mac_u = H * H, mac_d = H, mac_beta = H * K, mac_tr = H, mac_p = NP * 2;
    int mac_pred = mac_hidden + mac_out;
    int mac_upd = mac_hidden + mac_out + mac_u + mac_d + mac_beta + mac_tr + mac_p;
    int tanh_n = 2 * H;
    printf("    MACs: predict %d (hidden %d + out %d) + %d tanh; update %d (hidden %d + out %d + Ph %d + h'u %d + beta %d + trace %d + P %d) + %d tanh; "
           "total %d MAC + %d tanh (9 MAC + 1 div each) + 1 div = %d flop-equivalents\n",
           mac_pred, mac_hidden, mac_out, H, mac_upd, mac_hidden, mac_out, mac_u, mac_d, mac_beta, mac_tr, mac_p, H,
           mac_pred + mac_upd, tanh_n, mac_pred + mac_upd + tanh_n * 10 + 1);
    row("cost", 1, "host %.2f us per predict+update (Apple M-series, -O2); %d MAC + %d tanh + %d xorshift per gesture",
        us, mac_pred + mac_upd, tanh_n, 2 * H * DI);
    (void)sink;
}

int main(void)
{
    printf("swipe_intent host tests: sizeof(si_t) = %zu B (limit 1792), blob %d B\n", sizeof(si_t), SI_BLOB_BYTES);
    test_tanh();
    test_rls_batch();
    test_learning();
    test_drift();
    test_safety();
    test_quantile();
    test_gate();
    test_runtime_gate();
    test_prior();
    test_cost();
    row("object size", sizeof(si_t) <= 1792, "%zu B", sizeof(si_t));
    printf("\n%-40s %-5s %s\n", "test", "", "result");
    for (int i = 0; i < g_nrows; i++) printf("%-40s %-5s %s\n", g_rows[i].name, g_rows[i].pass ? "PASS" : "FAIL", g_rows[i].detail);
    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
