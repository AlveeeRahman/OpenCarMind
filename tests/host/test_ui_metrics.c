/* Host tests for main/ui_metrics.c: histograms, the rank test, Holm, the settle detector, the gate and persistence.
   Build: make uimetrics */
#include "ui_metrics.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
static void report(const char *name, int ok, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static void report(const char *name, int ok, const char *fmt, ...)
{
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    printf("%-40s %s  %s\n", name, ok ? "PASS" : "FAIL", b);
    if (!ok) fails++;
}

static uint32_t rs = 12345;
static double ur(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (rs >> 8) / 16777216.0; }
static double gauss(void) { double u = ur() + 1e-12, v = ur(); return sqrt(-2 * log(u)) * cos(6.283185307179586 * v); }
static double nr(void) { double u = ur() + 1e-12, v = ur(); return sqrt(-2 * log(u)) * cos(6.283185307 * v); }
static int cmpu(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }

/* 1. percentiles from the bins vs the exact nearest-rank percentile of the samples */
static void test_pct(void)
{
    double worst = 0;
    int cases = 0, ok = 1;
    static uint32_t v[2000];
    for (int shape = 0; shape < 3; shape++)
        for (int n = 20; n <= 2000; n = n * 3 / 2) {
            uint16_t h[UM_NB] = { 0 };
            for (int i = 0; i < n; i++) {
                double x = shape == 0 ? 180 + 40 * nr() : shape == 1 ? 60 + 120 * -log(ur() + 1e-9) : (ur() < 0.7 ? 90 + 10 * nr() : 400 + 60 * nr());
                if (x < 0) x = 0;
                if (x > 1000) x = 1000;
                v[i] = (uint32_t)x;
                uint32_t b = v[i] / UM_BIN_MS;
                h[b < UM_NB ? b : UM_NB - 1]++;
            }
            qsort(v, (size_t)n, sizeof *v, cmpu);
            for (int pi = 0; pi < 2; pi++) {
                unsigned pct = pi ? 95 : 50;
                uint32_t rank = (pct * (uint32_t)n + 99) / 100, exact = v[rank - 1];
                double e = fabs((double)um_hist_pct(h, UM_NB, UM_BIN_MS, pct) - exact);
                if (e > worst) worst = e;
                if (e > UM_BIN_MS / 2.0) ok = 0;
                cases++;
            }
        }
    report("p50 / p95 from bins vs exact", ok, "%d cases (normal, exponential, bimodal; n 20..2000): worst %.1f ms (limit half a bin = %d ms)",
           cases, worst, UM_BIN_MS / 2);
}

/* 1b. the Smart UI latency bins (8 ms to 1 s, then 32 ms to 3 s): monotone, and p50 / p95 vs exact on wide data */
static void test_lat_bins(void)
{
    int ok = 1, prev = -1;
    for (uint32_t ms = 0; ms < 4000; ms++) {
        int b = um_lat_bin(ms);
        if (b < prev || b >= UM_NB) ok = 0;
        uint16_t c = um_lat_centre(b);
        double half = b < UM_NB1 ? UM_BIN_MS / 2.0 : UM_BIN2_MS / 2.0;
        if (ms < 3072 && fabs((double)c - ms) > half) ok = 0;
        prev = b;
    }
    double worst_rel = 0;
    static uint32_t v[3000];
    for (int t = 0; t < 40; t++) {
        int n = 50 + t * 70;
        uint16_t h[UM_NB] = { 0 };
        for (int i = 0; i < n; i++) {
            double x = t % 2 ? 150 + 900 * -log(ur() + 1e-9) : 600 + 500 * nr();
            if (x < 0) x = 0;
            if (x > 3000) x = 3000;
            v[i] = (uint32_t)x;
            h[um_lat_bin(v[i])]++;
        }
        qsort(v, (size_t)n, sizeof *v, cmpu);
        for (int pi = 0; pi < 2; pi++) {
            unsigned pct = pi ? 95 : 50;
            uint32_t exact = v[(pct * (uint32_t)n + 99) / 100 - 1];
            double e = fabs((double)um_lat_pct(h, pct) - exact), lim = exact < 1024 ? UM_BIN_MS / 2.0 : UM_BIN2_MS / 2.0;
            if (e > lim) ok = 0;
            if (e / (exact + 1) > worst_rel) worst_rel = e / (exact + 1);
        }
    }
    report("latency bins to 3 s, p50 / p95", ok, "%d bins (%d x %d ms + %d x %d ms), monotone, centre within half a bin; 40 wide "
           "cases (to 3000 ms): within half a bin, worst %.2f %% of the value", UM_NB, UM_NB1, UM_BIN_MS, UM_NB2, UM_BIN2_MS, 100 * worst_rel);
}

/* 2. the binned Mann-Whitney z vs U counted pair by pair on the binned values */
static void test_mw(void)
{
    int ok = 1;
    double worst = 0;
    for (int t = 0; t < 60; t++) {
        int na = 30 + t * 3, nb = 40 + t * 2;
        double shift = (t % 5) * 8.0;
        uint16_t a[UM_NB] = { 0 }, b[UM_NB] = { 0 };
        static int av[400], bv[400];
        for (int i = 0; i < na; i++) { double x = 200 - shift + 50 * nr(); x = x < 0 ? 0 : x; int k = (int)x / UM_BIN_MS; k = k < UM_NB ? k : UM_NB - 1; av[i] = k; a[k]++; }
        for (int i = 0; i < nb; i++) { double x = 200 + 50 * nr(); x = x < 0 ? 0 : x; int k = (int)x / UM_BIN_MS; k = k < UM_NB ? k : UM_NB - 1; bv[i] = k; b[k]++; }
        double u = 0;
        for (int i = 0; i < na; i++) for (int j = 0; j < nb; j++) u += av[i] < bv[j] ? 1 : av[i] == bv[j] ? 0.5 : 0;
        double z = (u - na * nb / 2.0) / sqrt(na * nb * (na + nb + 1) / 12.0);
        double e = fabs(um_mw_z100(a, b, UM_NB) / 100.0 - z);
        if (e > worst) worst = e;
        if (e > 0.011) ok = 0;
    }
    uint16_t e1[UM_NB] = { 0 }, e2[UM_NB] = { 0 };
    e1[10] = 5;
    report("Mann-Whitney z from bins", ok && um_mw_z100(e1, e2, UM_NB) == 0, "60 cases vs pairwise U: worst |dz| %.4f (limit 0.011); empty arm -> 0", worst);
}

/* 3. Holm on z values vs the textbook procedure on p-values */
static double pz(double z) { return 0.5 * erfc(z / sqrt(2.0)); }
static void test_holm(void)
{
    int ok = 1, cases = 0, rej = 0;
    for (int t = 0; t < 20000; t++) {
        int m = 1 + t % 5;
        int16_t z[8];
        uint8_t has[8], sig[8];
        for (int i = 0; i < m; i++) { z[i] = (int16_t)(100 * (1.2 + 1.3 * nr())); has[i] = ur() < 0.85; }
        um_holm(z, has, m, sig);
        /* reference: p from the same truncated z (as the gate sees it), sorted ascending, alpha / (m - j) */
        int ord[8], k = 0;
        for (int i = 0; i < m; i++) if (has[i]) ord[k++] = i;
        for (int i = 1; i < k; i++) for (int j = i; j > 0 && z[ord[j]] > z[ord[j - 1]]; j--) { int x = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = x; }
        uint8_t ref[8] = { 0 };
        for (int j = 0; j < k; j++) { if (pz(z[ord[j]] / 100.0) <= 0.05 / (m - j)) ref[ord[j]] = 1; else break; }
        for (int i = 0; i < m; i++) { if (ref[i] != sig[i]) ok = 0; rej += sig[i]; }
        cases++;
    }
    /* crafted: family of 2, z 1.80 and 1.70: Holm needs 1.96 first, so neither; alone each passes 1.645 */
    int16_t z2[2] = { 180, 170 };
    uint8_t h2[2] = { 1, 1 }, s2[2], h1[2] = { 1, 0 };
    um_holm(z2, h2, 2, s2);
    int c1 = !s2[0] && !s2[1];
    um_holm(z2, h1, 1, s2);
    int c2 = s2[0];
    int16_t z3[2] = { 250, 170 };
    um_holm(z3, h2, 2, s2);
    int c3 = s2[0] && s2[1];                                    /* 2.50 > 1.96, then 1.70 > 1.645 */
    int16_t z4[3] = { 164, 165, 300 };
    uint8_t h3[3] = { 1, 1, 0 };
    um_holm(z4, h3, 3, s2);
    int c4 = !s2[0] && !s2[1];                                  /* m = 3: 1.65 < 2.13; the data-less test still counts in m */
    report("Holm step-down vs p-value reference", ok && c1 && c2 && c3 && c4,
           "%d random families (m 1..5, some without data): %d rejections, all equal; crafted 4/4 (z 1.80+1.70 in 2 -> none; "
           "alone 1.80 -> yes; 2.50+1.70 -> both; m counts data-less tests)", cases, rej);
}

/* 4. the settle detector */
static void test_settle(void)
{
    um_settle_t s = { 0 };
    uint32_t lat = 0;
    int r[10], ok;
    /* a: nothing drawn after the lift: settles at the lift, confirmed after the quiet gap */
    um_settle_lift(&s, 1000);
    r[0] = um_settle_poll(&s, 1016, false, &lat) == UM_S_WAIT;
    r[1] = um_settle_poll(&s, 1032, false, &lat) == UM_S_SETTLED && lat == 0;
    /* b: invalidate, draw, one frame at +40: settled = 40, not before +72 */
    um_settle_lift(&s, 2000);
    um_settle_invalidated(&s);
    r[2] = um_settle_poll(&s, 2050, false, &lat) == UM_S_WAIT;              /* dirty */
    um_settle_render_start(&s);
    um_settle_frame(&s, 2040);
    r[3] = um_settle_poll(&s, 2071, false, &lat) == UM_S_WAIT && um_settle_poll(&s, 2072, false, &lat) == UM_S_SETTLED && lat == 40;
    /* c: an animation: a frame every 20 ms to +300, busy while it runs */
    um_settle_lift(&s, 3000);
    int w = 1;
    for (uint32_t t = 3020; t <= 3300; t += 20) { um_settle_invalidated(&s); um_settle_render_start(&s); um_settle_frame(&s, t); w &= um_settle_poll(&s, t + 1, false, &lat) == UM_S_WAIT; }
    r[4] = w && um_settle_poll(&s, 3332, false, &lat) == UM_S_SETTLED && lat == 300;
    /* d: busy (blit mode) holds it even with no frames; timeout after UM_TIMEOUT_MS */
    um_settle_lift(&s, 10000);
    w = 1;
    for (uint32_t t = 10016; t < 10000 + UM_TIMEOUT_MS; t += 16) w &= um_settle_poll(&s, t, true, &lat) == UM_S_WAIT;
    r[5] = w && um_settle_poll(&s, 10000 + UM_TIMEOUT_MS, true, &lat) == UM_S_TIMEOUT && lat == UM_TIMEOUT_MS;
    /* e: an area invalidated before the lift and not drawn yet holds the settle until it is drawn */
    um_settle_invalidated(&s);
    um_settle_lift(&s, 20000);
    r[6] = um_settle_poll(&s, 20100, false, &lat) == UM_S_WAIT;
    um_settle_render_start(&s); um_settle_frame(&s, 20110);
    r[7] = um_settle_poll(&s, 20142, false, &lat) == UM_S_SETTLED && lat == 110;
    /* f: a frame from before the lift does not count; abort and polls when idle */
    um_settle_lift(&s, 30000);
    um_settle_frame(&s, 29990);
    r[8] = um_settle_poll(&s, 30032, false, &lat) == UM_S_SETTLED && lat == 0;
    um_settle_lift(&s, 40000);
    um_settle_abort(&s);
    r[9] = um_settle_poll(&s, 40100, false, &lat) == UM_S_IDLE;
    ok = 1;
    for (int i = 0; i < 10; i++) ok &= r[i];
    report("settle detector state machine", ok, "10 checks: idle lift -> 0 ms after 32 ms quiet; one frame -> 40; animation -> 300; "
           "busy -> timeout %d; pending area holds; old frame ignored; abort (%d%d%d%d%d%d%d%d%d%d)", UM_TIMEOUT_MS,
           r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9]);
}

/* 5. crafted decisions */
static void test_decide(void)
{
    uint8_t r[7], st[7];
    st[0] = um_decide(60, 60, 200, 150, 900, true, &r[0]);     /* all pass */
    st[1] = um_decide(60, 60, 200, 150, 800, true, &r[1]);     /* ready exactly 80 %: not above */
    st[2] = um_decide(60, 60, 150, 150, 950, true, &r[2]);     /* p95 tie */
    st[3] = um_decide(60, 60, 200, 150, 950, false, &r[3]);    /* z short, more data to come */
    st[4] = um_decide(150, 150, 200, 150, 950, false, &r[4]);  /* z short at the cap */
    st[5] = um_decide(49, 60, 200, 150, 950, true, &r[5]);     /* too few */
    st[6] = um_decide(60, 60, 140, 150, 950, true, &r[6]);     /* slower */
    int ok = st[0] == UM_KEEP && r[0] == UM_R_PASS && st[1] == UM_OFF && r[1] == UM_R_READY_LOW && st[2] == UM_OFF && r[2] == UM_R_SLOWER &&
             st[3] == UM_LEARNING && r[3] == UM_R_UNCERTAIN && st[4] == UM_OFF && r[4] == UM_R_UNCERTAIN && st[5] == UM_LEARNING &&
             st[6] == UM_OFF && r[6] == UM_R_SLOWER;
    report("gate decision, crafted", ok, "7 cases -> %s %s %s %s %s %s %s", um_state_name(st[0]), um_state_name(st[1]), um_state_name(st[2]),
           um_state_name(st[3]), um_state_name(st[4]), um_state_name(st[5]), um_state_name(st[6]));
}

/* 6. the runtime gate on simulated use: app (A1 helps, ready 95 %), back (A1 does nothing, ready 95 %), and a third
   family member whose prefetch is rarely ready */
static double lat_of(int c, int arm)
{
    double base = c == UM_APP ? 230 : c == UM_BACK ? 160 : 120, sd = c == UM_APP ? 40 : 30;
    if (arm == UM_A1 && c == UM_APP) base -= 70;
    if (arm == UM_A1 && c == UM_LIST) base -= 30;
    double x = base + sd * nr();
    return x < 0 ? 0 : x;
}
static void test_gate(void)
{
    static um_t m;
    uint8_t fam = (1u << UM_APP) | (1u << UM_BACK) | (1u << UM_LIST);
    um_init(&m, 777, fam, 0xABCD);
    int arms[UM_NCLASS][2] = { { 0 } }, keep_at = -1, ev = 0, aw = 1;
    for (; ev < 3000; ev++) {
        uint8_t c = (uint8_t)(ev % 3 == 0 ? UM_APP : ev % 3 == 1 ? UM_BACK : UM_LIST);
        uint8_t st0 = m.k[c].v.state, a = um_arm(&m, c);
        if (st0 == UM_WARMUP) aw &= a == UM_A0;
        if (st0 == UM_LEARNING) arms[c][a]++;
        bool ready = a == UM_A1 && (c == UM_LIST ? ur() < 0.5 : ur() < 0.95);
        um_record(&m, c, a, (uint32_t)lat_of(c, a), ready, false);
        if (um_gate_step(&m) && m.k[UM_APP].v.state == UM_KEEP && keep_at < 0) keep_at = ev;
        if (m.k[UM_APP].v.state != UM_LEARNING && m.k[UM_APP].v.state != UM_WARMUP && m.k[UM_BACK].v.state != UM_LEARNING &&
            m.k[UM_BACK].v.state != UM_WARMUP && m.k[UM_LIST].v.state != UM_LEARNING && m.k[UM_LIST].v.state != UM_WARMUP) break;
    }
    double split = arms[UM_APP][1] / (double)(arms[UM_APP][0] + arms[UM_APP][1]);
    int keep_all = 1;
    for (int i = 0; i < 50; i++) keep_all &= um_arm(&m, UM_APP) == UM_A1;
    int off_all = 1;
    for (int i = 0; i < 50; i++) off_all &= um_arm(&m, UM_BACK) == UM_A0;
    int ok = m.k[UM_APP].v.state == UM_KEEP && m.k[UM_BACK].v.state == UM_OFF && m.k[UM_LIST].v.state == UM_OFF &&
             m.k[UM_LIST].v.reason == UM_R_READY_LOW && split > 0.4 && split < 0.6 && aw && keep_all && off_all &&
             (m.family & 1u) == 0 && um_arm(&m, UM_SWIPE) == UM_A0 && um_arm(&m, UM_CONTROL) == UM_A0;
    report("runtime gate with Holm (3 classes)", ok,
           "app KEEP (z %.2f, p95 %u -> %u ms, ready %u %%, n %u/%u, at event %d); back %s (%s, z %.2f); list %s (ready %u %%); "
           "A1 share while learning %.2f; warm-up all A0; KEEP -> A1, OFF -> A0; swipe not gated until it registers", m.k[UM_APP].v.z100 / 100.0,
           m.k[UM_APP].v.p95_0, m.k[UM_APP].v.p95_1, m.k[UM_APP].v.ready_pm / 10, m.k[UM_APP].v.n0, m.k[UM_APP].v.n1, keep_at,
           um_state_name(m.k[UM_BACK].v.state), m.k[UM_BACK].v.reason == UM_R_SLOWER ? "slower" : m.k[UM_BACK].v.reason == UM_R_UNCERTAIN ? "unsure" : "?",
           m.k[UM_BACK].v.z100 / 100.0, um_state_name(m.k[UM_LIST].v.state), m.k[UM_LIST].v.ready_pm / 10, split);
}

/* 7. persistence */
/* ---------- the OS feature gate: Smart swipe as a registered feature with the learner policy ---------- */
static int g_actions_on, g_actions_off;
static void swipe_action(bool on) { if (on) g_actions_on++; else g_actions_off++; }

/* n swipe events: A1 (the learner acts) settles `gain` ms sooner, ready with probability rdy */
static int run_swipes(um_t *m, int n, double gain, double rdy, int *a0_in_keep, int *a1_in_keep, int *keep_at, int *off_at)
{
    int ev;
    for (ev = 0; ev < n; ev++) {
        uint8_t st0 = m->k[UM_SWIPE].v.state, a = um_arm(m, UM_SWIPE);
        if (st0 == UM_KEEP) { if (a == UM_A0) (*a0_in_keep)++; else (*a1_in_keep)++; }
        double lat = 205 + 18 * gauss() - (a == UM_A1 ? gain : 0);
        um_record(m, UM_SWIPE, a, (uint32_t)(lat < 100 ? 100 : lat), a == UM_A1 && ur() < rdy, 0);
        if ((m->k[UM_SWIPE].n[0] + m->k[UM_SWIPE].n[1]) % 10u == 0) um_gate_step(m);
        if (keep_at && *keep_at < 0 && m->k[UM_SWIPE].v.state == UM_KEEP) *keep_at = ev;
        if (off_at && *off_at < 0 && m->k[UM_SWIPE].v.state == UM_OFF) { *off_at = ev; break; }
    }
    return ev;
}

static void test_feature_gate(void)
{
    static um_t m, l;
    static uint8_t buf[UM_BLOB_MAX];
    const um_feature_t sw = { "Smart swipe", UM_SWIPE, UM_POL_LEARNER, UM_MET_LIFT_SETTLE, swipe_action };
    const um_feature_t bad1 = { "x", UM_NCLASS, UM_POL_LEARNER, UM_MET_LIFT_SETTLE, NULL };
    const um_feature_t bad2 = { "x", UM_SWIPE, UM_POL_N, UM_MET_LIFT_SETTLE, NULL };
    const um_feature_t bad3 = { "x", UM_SWIPE, UM_POL_LEARNER, 7, NULL };
    char line[96];
    um_init(&m, 4242, (1u << UM_APP) | (1u << UM_BACK), 0x1234);
    int reg = um_register(&m, &sw) && !um_register(&m, &bad1) && !um_register(&m, &bad2) && !um_register(&m, &bad3) &&
              (m.family >> UM_SWIPE & 1u) && m.k[UM_SWIPE].v.state == UM_WARMUP;
    /* warm-up: 30 events, every one A0, none counted */
    int warm_a0 = 1;
    for (int i = 0; i < 30; i++) { warm_a0 &= um_arm(&m, UM_SWIPE) == UM_A0; um_record(&m, UM_SWIPE, UM_A0, 200, false, 0); }
    um_line(&m, UM_SWIPE, line, sizeof line);
    int warm = warm_a0 && m.k[UM_SWIPE].v.state == UM_LEARNING && m.k[UM_SWIPE].n[0] == 0 && !strncmp(line, "Smart swipe: learning 0/100", 27);
    /* helpful: KEEP, then about 10 % A0 kept, re-checks pass */
    int a0k = 0, a1k = 0, keep_at = -1;
    g_actions_on = g_actions_off = 0;
    run_swipes(&m, 3000, 25, 0.9, &a0k, &a1k, &keep_at, NULL);
    double share = a0k / (double)(a0k + a1k);
    um_line(&m, UM_SWIPE, line, sizeof line);
    int helpful = m.k[UM_SWIPE].v.state == UM_KEEP && keep_at > 0 && share > 0.06 && share < 0.14 && g_actions_on == 1 &&
                  g_actions_off == 0 && !strncmp(line, "Smart swipe: on, p95 ", 21);
    printf("    learner policy, helpful: KEEP at swipe event %d, A0 share in KEEP %.1f %%; \"%s\"\n", keep_at, 100 * share, line);
    /* the gain goes away: a re-check turns it off (reason re-check) */
    int off_at = -1;
    run_swipes(&m, 20000, 0, 0.9, &a0k, &a1k, NULL, &off_at);
    um_line(&m, UM_SWIPE, line, sizeof line);
    int recheck = off_at > 0 && m.k[UM_SWIPE].v.state == UM_OFF && m.k[UM_SWIPE].v.reason == UM_R_RECHECK_FAIL &&
                  g_actions_off == 1 && um_arm(&m, UM_SWIPE) == UM_A0 && strstr(line, "off (re-check)");
    printf("    learner policy, gain gone: OFF after %d more swipe events; \"%s\"\n", off_at, line);
    /* useless from the start: OFF, the verdict persists for the same firmware and re-tests on a new one */
    um_init(&m, 99, (1u << UM_APP) | (1u << UM_BACK), 0x1234);
    um_register(&m, &sw);
    for (int i = 0; i < 30; i++) um_record(&m, UM_SWIPE, UM_A0, 200, false, 0);
    int off2 = -1;
    run_swipes(&m, 5000, 0, 0.9, &a0k, &a1k, NULL, &off2);
    size_t n = um_save(&m, buf, sizeof buf);
    um_init(&l, 5, (1u << UM_APP) | (1u << UM_BACK), 0x1234);
    um_register(&l, &sw);
    int same = um_load(&l, buf, n) && l.k[UM_SWIPE].v.state == UM_OFF && um_arm(&l, UM_SWIPE) == UM_A0;
    um_init(&l, 5, (1u << UM_APP) | (1u << UM_BACK), 0x5678);
    um_register(&l, &sw);
    int newfw = um_load(&l, buf, n) && l.k[UM_SWIPE].v.state == UM_WARMUP;
    um_init(&l, 5, (1u << UM_APP) | (1u << UM_BACK), 0x1234);
    int unreg = !um_load(&l, buf, n);                      /* a blob with the feature does not load without it */
    um_reset_class(&m, UM_SWIPE);
    int reset = m.k[UM_SWIPE].v.state == UM_WARMUP && m.k[UM_SWIPE].n[1] == 0;
    /* Holm families: Smart swipe is tested alone (z 1.79 passes 1.645, a family of 3 would need 2.13) while two prefetch
       classes learn the same data in their own family (theirs needs 1.96 first) */
    um_init(&m, 7, (1u << UM_APP) | (1u << UM_BACK), 0x1);
    um_register(&m, &sw);
    for (int c = 0; c < 3; c++) {
        uint8_t cls = c == 0 ? UM_SWIPE : c == 1 ? UM_APP : UM_BACK;
        m.k[cls].v.state = UM_LEARNING;
        for (int i = 0; i < 60; i++) {   /* A1 a bit faster: z about 1.8 */
            um_record(&m, cls, UM_A0, (uint32_t)(200 + (i % 20) * 5), false, 0);
            um_record(&m, cls, UM_A1, (uint32_t)(190 + (i % 20) * 5), true, 0);
        }
    }
    int16_t zs = um_mw_z100(m.hist[UM_SWIPE][1], m.hist[UM_SWIPE][0], UM_NB);
    um_gate_step(&m);
    int fam = zs > 165 && zs < 196 && m.k[UM_SWIPE].v.state == UM_KEEP && m.k[UM_APP].v.state != UM_KEEP && m.k[UM_BACK].v.state != UM_KEEP;
    /* the range: two arms that differ only above 475 ms are told apart (the old 96 x 5 ms bins put both in one bin) */
    static uint16_t ha[UM_NB], hb[UM_NB];
    for (int i = 0; i < 80; i++) { ha[um_lat_bin((uint32_t)(700 + 200 * ur()))]++; hb[um_lat_bin((uint32_t)(950 + 200 * ur()))]++; }
    int16_t zr = um_mw_z100(ha, hb, UM_NB);
    /* the owner's swipes (p50 about 400, p95 about 1055 ms): the gate's p95 from the bins against an exact sort */
    double worst_pct = 0;
    for (int rep = 0; rep < 80; rep++) {
        static uint16_t ho[UM_NB];
        static uint32_t v[400];
        memset(ho, 0, sizeof ho);
        for (int i = 0; i < 400; i++) {
            double ms = exp(5.99 + 0.59 * gauss());
            v[i] = (uint32_t)(ms > 3000 ? 3000 : ms);
            ho[um_lat_bin(v[i])]++;
        }
        for (int i = 1; i < 400; i++) for (int j = i; j > 0 && v[j - 1] > v[j]; j--) { uint32_t t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
        uint32_t exact = v[(95 * 400 + 99) / 100 - 1];
        double e = fabs((double)um_lat_pct(ho, 95) - exact), bin = exact < UM_NB1 * UM_BIN_MS ? UM_BIN_MS : UM_BIN2_MS;
        if (e > bin && 100 * e / exact > worst_pct) worst_pct = 100 * e / exact;
    }
    zr = worst_pct <= 2 ? zr : 0;
    report("feature gate: Smart swipe registered", reg && warm && helpful && recheck && off2 > 0 && same && newfw && unreg && reset && fam && zr > 164,
           "register %s, warm-up 30 A0 %s, helpful KEEP at %d (A0 share %.1f %%, action on x%d), gain gone OFF by re-check after %d, "
           "useless OFF at %d, reload %s, new fw warm-up %s, unregistered blob rejected %s, reset %s, own Holm family z %.2f -> %s, "
           "arms 700-900 vs 950-1150 ms z %.2f, owner-like p95 within a bin or 2 %%",
           reg ? "ok" : "BAD", warm ? "ok" : "BAD", keep_at, 100 * share, g_actions_on, off_at, off2, same ? "ok" : "BAD",
           newfw ? "ok" : "BAD", unreg ? "ok" : "BAD", reset ? "ok" : "BAD", zs / 100.0, fam ? "KEEP" : "BAD", zr / 100.0);
}

static void test_persist(void)
{
    static um_t m, l;
    static uint8_t buf[UM_BLOB_MAX];
    uint8_t fam = (1u << UM_APP) | (1u << UM_BACK);
    um_init(&m, 5, fam, 0x1111);
    for (int i = 0; i < 400; i++) {
        uint8_t c = (uint8_t)(i & 1 ? UM_APP : UM_BACK), a = um_arm(&m, c);
        um_record(&m, c, a, (uint32_t)lat_of(c, a), a == UM_A1, false);
        um_gate_step(&m);
    }
    um_record(&m, UM_CONTROL, UM_A0, 50, false, false);
    size_t n = um_save(&m, buf, sizeof buf);
    um_init(&l, 9, fam, 0x1111);
    int same = um_load(&l, buf, n) && !memcmp(&l.k[UM_APP].v, &m.k[UM_APP].v, sizeof m.k[0].v) && !memcmp(&l.k[UM_BACK].v, &m.k[UM_BACK].v, sizeof m.k[0].v) && !memcmp(l.hist[UM_APP], m.hist[UM_APP], sizeof m.hist[UM_APP]) &&
               l.k[UM_BACK].n[1] == m.k[UM_BACK].n[1] && l.k[UM_CONTROL].n[0] == 0;
    um_init(&l, 9, fam, 0x2222);
    int newfw = um_load(&l, buf, n) && l.k[UM_APP].v.state == UM_WARMUP && l.k[UM_APP].n[0] == 0;
    um_init(&l, 9, fam, 0x1111);
    buf[40] ^= 0x20;
    int corrupt = !um_load(&l, buf, n) && l.k[UM_APP].v.state == UM_WARMUP;
    buf[40] ^= 0x20;
    um_init(&l, 9, (uint8_t)(fam | (1u << UM_LIST)), 0x1111);
    int famchg = !um_load(&l, buf, n);
    um_init(&l, 9, fam, 0x1111);
    um_load(&l, buf, n);
    um_reset_verdicts(&l);
    int reset = l.k[UM_APP].v.state == UM_WARMUP && l.k[UM_BACK].v.state == UM_WARMUP && l.k[UM_APP].n[1] == 0;
    report("persistence", same && newfw && corrupt && famchg && reset && n <= UM_BLOB_MAX,
           "blob %zu B (max %d): reload same fw ok (app %s, back %s); new fw -> warm-up; flipped bit rejected; other family rejected; reset -> warm-up",
           n, (int)UM_BLOB_MAX, um_state_name(m.k[UM_APP].v.state), um_state_name(m.k[UM_BACK].v.state));
}

/* 8. cost of one event on the host (the board figure comes from VIEW uimetrics) */
static void test_cost(void)
{
    static um_t m;
    um_init(&m, 3, (1u << UM_APP) | (1u << UM_BACK), 1);
    um_settle_t s = { 0 };
    uint32_t lat;
    clock_t c0 = clock();
    const int N = 2000000;
    for (int i = 0; i < N; i++) {
        uint8_t c = (uint8_t)(i % 5), a = um_arm(&m, c);
        um_settle_lift(&s, (uint32_t)i * 300u);
        um_settle_invalidated(&s); um_settle_render_start(&s); um_settle_frame(&s, (uint32_t)i * 300u + 40u);
        um_settle_poll(&s, (uint32_t)i * 300u + 80u, false, &lat);
        um_record(&m, c, a, lat + (uint32_t)(i & 63), a, false);
    }
    double us = (double)(clock() - c0) / CLOCKS_PER_SEC * 1e6 / N;
    report("cost per event (host)", us < 1.0, "%.3f us per event (arm + settle + record), sizeof(um_t) %zu B", us, sizeof(um_t));
}

int main(void)
{
    printf("%-40s %s\n", "test", "result");
    test_pct();
    test_lat_bins();
    test_mw();
    test_holm();
    test_settle();
    test_decide();
    test_gate();
    test_feature_gate();
    test_persist();
    test_cost();
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
