/* bat_soh (main/bat_soh.c, main/bat_soh_model.h): the SoH ridge against the research vectors, the Q(V) features against
 * the reference functions (vec/gen_soh_feat.py runs p3_soh.py's q_between and ica_peak), and the online cycle logic on
 * synthetic cycles with known answers.
 *   make soh  (SOH_VECTORS = carmind-ml experiments/battery_soc/soh_vectors.csv) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bat_soh.h"
#include "bat_soh_model.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int split(char *line, char **f, int max)
{
    int n = 0; char *p = line;
    while (n < max) { f[n++] = p; char *c = strchr(p, ','); if (!c) break; *c = 0; p = c + 1; }
    char *nl = strpbrk(f[n - 1], "\r\n"); if (nl) *nl = 0;
    return n;
}

static double rel(double got, double exp) { return fabs(got - exp) / fmax(1.0, fabs(exp)); }

static void vectors(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) { printf("FAIL: no SoH vectors at %s (set SOH_VECTORS)\n", path); fails++; return; }
    char line[2048], *f[64];
    double worst = 0;
    int rows = 0;
    if (!fgets(line, sizeof line, fp)) { fclose(fp); fails++; return; }
    while (fgets(line, sizeof line, fp)) {
        int n = split(line, f, 64);
        float x[BAT_SOH_NF];
        for (int j = 0; j < BAT_SOH_NF; j++) x[j] = (float)atof(f[1 + j]);
        double d = rel(bat_soh_predict(x), atof(f[n - 1]));
        if (d > worst) worst = d;
        CHECK(d <= 1e-4, "soh row %d diff %g", rows, d);
        rows++;
    }
    fclose(fp);
    CHECK(rows >= 200, "soh vector rows %d", rows);
    printf("soh vectors: %d rows, worst rel diff %.3g\n", rows, worst);
}

static void feat_vectors(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) { printf("FAIL: no feature vectors at %s\n", path); fails++; return; }
    char line[4096], *f[80];
    double worst = 0;
    int rows = 0;
    if (!fgets(line, sizeof line, fp)) { fclose(fp); fails++; return; }
    while (fgets(line, sizeof line, fp)) {
        int n = split(line, f, 80);
        CHECK(n == 1 + BAT_SOH_GRID_N + 3, "columns %d", n);
        float q[BAT_SOH_GRID_N], h, v;
        for (int k = 0; k < BAT_SOH_GRID_N; k++) q[k] = (float)atof(f[1 + k]);
        float dq = bat_soh_q_between(q, BAT_SOH_WIN_LO, BAT_SOH_WIN_HI);
        bat_soh_ica_peak(q, &h, &v);
        double d1 = rel(dq, atof(f[n - 3])), d2 = rel(h, atof(f[n - 2])), d3 = fabs(v - atof(f[n - 1]));
        worst = fmax(worst, fmax(d1, d2));
        CHECK(d1 <= 1e-4 && d2 <= 1e-4 && d3 <= 1e-4, "feature case %d: dq %g ica_h %g ica_v %g", rows, d1, d2, d3);
        rows++;
    }
    fclose(fp);
    printf("Q(V) feature vectors: %d cases, worst rel diff %.3g\n", rows, worst);
}

/* one synthetic cycle: rest, a CC charge from 3.55 V to 4.15 V with a dQ/dV peak, CV with I = I0 exp(-t/tau), rest,
   a 0.5 C discharge from a rest (the DCIR step: the cell drops by r x c at once), down to z 0.25 */
#define Q_AH 0.065f
static float ocv_of(float z) { return 3.4f + 0.8f * z; }
static void run_cycle(bat_soh_t *s, float cap_frac, float r_vpc, float tau_s, float t_c, int *news)
{
    soh_in_t in = { .dt = 2.0f, .t_c = t_c, .valid = true };
    float z = 0.12f;
    for (int k = 0; k < 30; k++) {                           /* rest at the bottom */
        in.v = 3.55f; in.c = 0.0f; in.mode = BAT_MODE_REST; in.z_ekf = z; in.ocv_z = ocv_of(z);
        *news += bat_soh_step(s, &in);
    }
    float q = 0.0f, qcap = cap_frac * Q_AH;
    for (in.v = 3.55f; in.v < 4.149f;) {                    /* CC 0.5 C: Q(V) logistic, so V follows from Q */
        in.c = 0.5f; in.mode = BAT_MODE_CHARGE;
        q += 0.5f * Q_AH * in.dt / 3600.0f;
        float u = q / (0.8f * qcap);                         /* the share of the CC charge done */
        if (u > 0.9999f) u = 0.9999f;
        in.v = 3.80f + 0.04f * logf(u / (1.0f - u));          /* logistic: the dQ/dV peak at 3.80 V */
        if (in.v < 3.55f) in.v = 3.55f;
        if (in.v > 4.149f) in.v = 4.149f;
        z = 0.12f + q / Q_AH; in.z_ekf = z; in.ocv_z = ocv_of(z);
        *news += bat_soh_step(s, &in);
        if (u >= 0.9999f) break;
    }
    for (float t = 0; t < 3600.0f; t += in.dt) {             /* CV: the current decays with tau */
        in.v = 4.19f; in.c = 0.5f * expf(-t / tau_s); in.mode = in.c >= 0.05f ? BAT_MODE_CV : BAT_MODE_REST;
        if (in.mode == BAT_MODE_REST) break;
        *news += bat_soh_step(s, &in);
    }
    z = 1.0f;
    for (int k = 0; k < 60; k++) {                           /* rest at the top */
        in.c = 0.0f; in.mode = BAT_MODE_REST; in.z_ekf = z; in.ocv_z = ocv_of(z); in.v = in.ocv_z;
        *news += bat_soh_step(s, &in);
    }
    float qd = 0.0f;
    for (int k = 0; k < 20000 && z > 0.25f; k++) {           /* 0.5 C discharge: the voltage drops by r x 0.5 at once */
        in.c = -0.5f; in.mode = BAT_MODE_DISCHARGE;
        qd += 0.5f * Q_AH * in.dt / 3600.0f;
        z = 1.0f - qd / qcap;
        in.z_ekf = z; in.ocv_z = ocv_of(z);
        in.v = in.ocv_z - r_vpc * 0.5f;
        *news += bat_soh_step(s, &in);
    }
}

static void cycles(void)
{
    static bat_soh_t s;
    bat_soh_init(&s, Q_AH, NULL);
    CHECK(bat_soh_source(&s, 0) == SOH_SRC_LEARNING, "source before any cycle");
    int news = 0;
    run_cycle(&s, 1.0f, 0.10f, 600.0f, 25.0f, &news);
    soh_cycle_t *c = &s.cyc;                                 /* the first cycle, before the next charge closes it */
    CHECK(c->have == 0xBF, "features of the first cycle (all but T_avg): have 0x%02x", c->have);
    CHECK(fabsf(c->raw[SOH_F_CV_TAU] - 600.0f / 3600.0f) < 0.002f, "cv tau %g h, want %g", c->raw[SOH_F_CV_TAU], 600.0 / 3600);
    CHECK(fabsf(c->raw[SOH_F_DCIR] - 0.10f) < 0.002f, "dcir %g V/C, want 0.10", c->raw[SOH_F_DCIR]);
    CHECK(fabsf(c->raw[SOH_F_ICA_V] - 3.80f) <= 0.011f, "ica peak at %g V, want 3.80", c->raw[SOH_F_ICA_V]);
    CHECK(fabsf(c->raw[SOH_F_EKF] - 1.0f) < 0.02f, "ekf ratio %g, want 1 (Ah / (1 - z) with the true capacity)", c->raw[SOH_F_EKF]);
    for (int i = 1; i < BAT_SOH_NREF; i++) run_cycle(&s, 1.0f, 0.10f, 600.0f, 25.0f, &news);
    CHECK(s.ref.n == BAT_SOH_NREF - 1, "reference entries after 10 cycles (the 10th closes at the next charge): %u", s.ref.n);
    CHECK(bat_soh_source(&s, 0) == SOH_SRC_LEARNING, "still learning");
    run_cycle(&s, 1.0f, 0.10f, 600.0f, 25.0f, &news);         /* closes the 10th */
    CHECK(s.ref.n == BAT_SOH_NREF, "reference complete: %u", s.ref.n);
    run_cycle(&s, 1.0f, 0.10f, 600.0f, 25.0f, &news);         /* closes the 11th: the first model output */
    CHECK(bat_soh_source(&s, 0) == SOH_SRC_MODEL && s.ref.last_soh > 0.3f, "model output %g", s.ref.last_soh);
    CHECK(fabsf(s.last_x[0] - 1.0f) < 1e-3f && fabsf(s.last_x[5] - 1.0f) < 1e-3f, "same cell: relative features 1 (%g %g)",
          s.last_x[0], s.last_x[5]);
    CHECK(bat_soh_source(&s, 1) == SOH_SRC_FULL, "the full-ends count wins");
    CHECK(news >= 11, "outputs %d", news);
    /* a top-up charge from 3.9 V gives no SoH sample (counted, not used) */
    uint32_t used = s.ref.usable;
    soh_in_t in = { .dt = 2.0f, .t_c = 25.0f, .valid = true, .v = 3.9f, .c = 0.5f, .mode = BAT_MODE_CHARGE, .z_ekf = 0.6f };
    bat_soh_step(&s, &in);                                   /* closes the 12th (usable) */
    for (int k = 0; k < 100; k++) { in.v += 0.002f; bat_soh_step(&s, &in); }
    in.c = 0.0f; in.mode = BAT_MODE_REST; bat_soh_step(&s, &in);
    in.c = 0.5f; in.mode = BAT_MODE_CHARGE; in.v = 3.5f; bat_soh_step(&s, &in);   /* closes the top-up */
    CHECK(s.ref.usable == used + 1, "a top-up is not usable: usable %u (was %u)", s.ref.usable, used);
    /* the reference record round trip */
    soh_ref_t r = s.ref;
    CHECK(bat_soh_ref_valid(&r), "valid reference");
    r.ref[0][1] = -1.0f;
    CHECK(!bat_soh_ref_valid(&r), "a negative reference is rejected");
    float m4[4] = { 4, 1, 3, 2 }, m5[5] = { 5, 1, 4, 2, 3 };
    CHECK(bat_soh_median(m4, 4) == 2.5f && bat_soh_median(m5, 5) == 3.0f, "median");
}

int main(void)
{
    const char *v = getenv("SOH_VECTORS");
    vectors(v && *v ? v : "soh_vectors.csv");
    feat_vectors("vec/soh_feat_vectors.csv");
    cycles();
    printf("bat_soh: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
