/* Host tests for the battery SoC/SoH estimator (main/bat_est_core.c), deterministic. A simulated cell (the model's
 * own circuit in double precision, unless a test says otherwise; 2 s samples quantised to 1 mV and 1 mA like the
 * BQ27220) runs a discharge, a CC-CV charge with its taper, rests, a wrong initial SoC, a circuit 30 % off the model,
 * a worn cell (capacity learning) and current steps (DCIR). Then the research agent's test_vectors.csv is replayed
 * through the C port and compared to 1e-4 (when the vectors belong to the bat_model.h compiled in).
 * Also times one step on this Mac: a proxy for the board, not a measurement of it.
 *   make -C tests/host bat                                   main/bat_model.h, vectors at BAT_VECTORS
 *   make -C tests/host bat BAT_MODEL=/path/bat_model.h       a header before it lands in main/ */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bat_est_core.h"

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define DT 2.0
static double Q;                                     /* the model's nominal capacity, Ah */
static double R0C, R0D, R1, TAU;                     /* the simulated cell's circuit, V per C and s */

typedef struct { double z, v1, q_ah; } cell_t;       /* q_ah: the cell's true capacity */

static void circuit_from_model(double scale)
{
    float a, b, c, d;
    bat_model_circuit(&a, &b, &c, &d);
    R0C = a * scale; R0D = b * scale; R1 = c * scale; TAU = d * scale;
}

static double cell_v(const cell_t *s, double i_a)
{
    double c = i_a / Q;
    return bat_ocv_v((float)s->z) + s->v1 + (c >= 0 ? R0C : R0D) * c;
}

static void cell_step(cell_t *s, double i_a)
{
    double a = exp(-DT / TAU), c = i_a / Q;
    s->z += i_a * DT / (3600.0 * s->q_ah);
    if (s->z < 0) s->z = 0;
    if (s->z > 1) s->z = 1;
    s->v1 = a * s->v1 + (1 - a) * R1 * c;
}

static double s_last_i;
/* one 2 s gauge sample at i_ma (rounded like the gauge), then the estimator step; i_avg as the firmware forms it */
static void sample(cell_t *s, bat_est_t *e, double i_ma)
{
    cell_step(s, i_ma / 1000.0);
    double ii = (double)lround(i_ma) / 1000.0;
    bat_in_t in = { (float)(lround(cell_v(s, i_ma / 1000.0) * 1000.0) / 1000.0), (float)ii, (float)(0.5 * (s_last_i + ii)), 25.0f,
                    (float)DT, 1 };
    s_last_i = ii;
    bat_est_step(e, &in);
}

static void run(cell_t *s, bat_est_t *e, double i_ma, double seconds, double *max_err)
{
    for (double t = 0; t < seconds; t += DT) {
        sample(s, e, i_ma);
        if (max_err) { double d = fabs(e->z_corr - s->z); if (d > *max_err) *max_err = d; }
    }
}

/* CC at c_rate until the terminal voltage reaches 4.2 V, then CV at 4.2 V until the current falls under C/20 */
static int cc_cv(cell_t *s, bat_est_t *e, double c_rate, int *seen_cv, double *max_err)
{
    int steps = 0;
    double i_cc = c_rate * Q * 1000.0;
    while (steps < 40000) {
        double i = i_cc;
        if (cell_v(s, i / 1000.0) >= 4.2) {
            double a = exp(-DT / TAU), ocv = bat_ocv_v((float)s->z);
            i = 1000.0 * Q * (4.2 - ocv - a * s->v1) / (R0C + R1 * (1 - a));
            if (i > i_cc) i = i_cc;
        }
        if (i < 0.05 * Q * 1000.0) break;
        sample(s, e, i);
        if (e->mode == BAT_MODE_CV) *seen_cv = 1;
        if (max_err) { double d = fabs(e->z_corr - s->z); if (d > *max_err) *max_err = d; }
        steps++;
    }
    sample(s, e, 0);                                  /* the charger ends: zero current */
    return steps;
}

static bat_est_t fresh(const cell_t *s)
{
    bat_est_t e;
    s_last_i = 0;
    bat_est_boot(&e, NULL, (float)cell_v(s, 0), 0.0f);
    return e;
}

static void test_ocv(void)
{
    double worst = 0;
    for (int i = 0; i <= 1000; i++) {
        float z = i / 1000.0f, back = bat_ocv_inv(bat_ocv_v(z));
        if (fabsf(back - z) > worst) worst = fabsf(back - z);
    }
    CHECK(worst < 1e-4, "OCV inverse round trip, worst %.2e", worst);
    CHECK(bat_ocv_inv(1.0f) == 0 && bat_ocv_inv(9.0f) == 1 && bat_ocv_v(1.0f) > bat_ocv_v(0.5f), "OCV ends and rise");
    CHECK(bat_mode(-0.5f, 3.8f) == BAT_MODE_DISCHARGE && bat_mode(0.0f, 3.8f) == BAT_MODE_REST && bat_mode(0.5f, 3.9f) == BAT_MODE_CHARGE &&
          bat_mode(0.2f, 4.19f) == BAT_MODE_CV, "modes from the current's sign and size");
    CHECK(!strcmp(bat_mode_name(BAT_MODE_CV), "CV taper"), "mode names");
}

static void test_discharge(void)
{
    cell_t s = { 0.95, 0.0, Q };
    bat_est_t e = fresh(&s);
    CHECK(fabsf(e.z - 0.95f) < 0.02f, "boot from the OCV at rest: %.3f", e.z);
    double err = 0;
    run(&s, &e, -0.5 * Q * 1000, 3600, &err);        /* C/2 for an hour */
    CHECK(fabs(s.z - 0.45) < 1e-3, "the simulated cell is at %.3f", s.z);
    CHECK(err < 0.03, "discharge C/2: worst SoC error %.4f", err);
    CHECK(e.mode == BAT_MODE_DISCHARGE && !e.anchored, "discharge mode, no anchor yet (learning)");
    run(&s, &e, 0.0, bat_model_rest_min_s() + 60, &err);
    CHECK(e.anchored && e.n_anchor == 1 && e.mode == BAT_MODE_REST, "rest: one OCV anchor (%u)", e.n_anchor);
    CHECK(fabs(e.z_corr - s.z) < 0.015, "after the anchor: error %.4f", fabs(e.z_corr - s.z));
    run(&s, &e, 0.0, 600, NULL);
    CHECK(e.n_anchor == 1, "one anchor per rest");
    CHECK(e.p00 > 0 && e.p11 > 0 && e.p00 * e.p11 - e.p01 * e.p01 > 0, "P stays positive definite");
}

static void test_charge(void)
{
    cell_t s = { 0.2, 0.0, Q };
    bat_est_t e = fresh(&s);
    int cv = 0;
    double err = 0;
    int steps = cc_cv(&s, &e, 0.5, &cv, &err);
    CHECK(cv, "the CV taper was seen (%d steps)", steps);
    CHECK(e.anchored && e.n_anchor >= 1, "full-charge anchor at the end of the taper");
    CHECK(s.z > 0.95 && fabs(e.z_corr - s.z) < 0.03, "charged: cell %.3f, estimate %.3f", s.z, e.z_corr);
    CHECK(err < 0.08, "charge: worst SoC error %.4f", err);
    uint32_t n = e.n_anchor;
    run(&s, &e, 0.0, 60, NULL);
    CHECK(e.n_anchor == n, "the full anchor fires once");
}

static void test_wrong_init(void)
{
    cell_t s = { 0.8, 0.0, Q };
    bat_est_t e = fresh(&s);
    bat_est_init(&e, 0.5f, bat_model_p0_z(), bat_model_p0_v(), 1.0f, true);   /* 30 points wrong */
    run(&s, &e, -0.2 * Q * 1000, 1800, NULL);
    run(&s, &e, 0.0, bat_model_rest_min_s() + 60, NULL);
    CHECK(fabs(e.z_corr - s.z) < 0.02, "wrong initial SoC: error after the rest anchor %.4f", fabs(e.z_corr - s.z));
}

/* the inverse crime: the tests above simulate the model the filter assumes. Here the circuit is 30 % off */
static void test_mismatch(void)
{
    circuit_from_model(1.3);
    cell_t s = { 0.9, 0.0, Q };
    bat_est_t e = fresh(&s);
    double err = 0;
    run(&s, &e, -0.4 * Q * 1000, 3000, &err);
    CHECK(err < 0.06, "circuit 30 %% off the model, C/2.5 discharge: worst error %.4f", err);
    circuit_from_model(1.0);
}

static void test_capacity_and_dcir(void)
{
    cell_t s = { 0.5, 0.0, 0.8 * Q };                 /* a worn cell: 80 % of the nominal capacity */
    bat_est_t e = fresh(&s);
    int cv;
    for (int cyc = 0; cyc < 5; cyc++) {
        cv = 0;
        cc_cv(&s, &e, 0.5, &cv, NULL);                /* full anchor */
        for (int k = 0; k < 64; k++) {                /* 0.53 Q nominal out = 67 % of this cell, in steps of 0.6 C */
            run(&s, &e, -0.2 * Q * 1000, 30, NULL);
            run(&s, &e, -0.8 * Q * 1000, 30, NULL);
        }
        run(&s, &e, 0.0, bat_model_rest_min_s() + 60, NULL);   /* OCV anchor */
    }
    CHECK(e.n_q >= 3, "capacity samples: %u", e.n_q);
    CHECK(fabsf(bat_soh_pct(&e) - 80.0f) < 8.0f, "SoH of a cell with 80 %% capacity: %.1f %%", bat_soh_pct(&e));
    CHECK(e.n_dcir >= 10, "DCIR samples from the current steps: %u", e.n_dcir);
    CHECK(e.dcir > R0D * 0.9 && e.dcir < R0D + R1, "DCIR %.4f V/C between R0 %.4f and R0 + R1", e.dcir, R0D);
    CHECK(fabsf(bat_dcir_ratio(&e) - 1.0f) < 0.2f, "DCIR trend of an unchanged circuit: %.3f", bat_dcir_ratio(&e));

    bat_persist_t p;
    bat_est_persist(&e, &p);
    CHECK(bat_persist_valid(&p), "persist blob valid");
    bat_est_t g;
    bat_est_boot(&g, &p, 3.0f, 0.0f);
    CHECK(g.anchored && g.soh == e.soh && g.dcir == e.dcir && fabsf(g.z - e.z) < 1e-6f, "restored from the blob");
    p.magic = 0;
    CHECK(!bat_persist_valid(&p), "a bad magic is refused");
    bat_est_persist(&e, &p);
    p.soh = 5;
    CHECK(!bat_persist_valid(&p), "an impossible SoH is refused");
}

static void test_cost(void)
{
    bat_est_t e;
    bat_est_init(&e, 0.6f, 0.04f, 0.001f, 1.0f, true);
    bat_model_set_ridge(true);                        /* the worst case: ridge on */
    bat_in_t in = { 3.9f, -0.02f, -0.02f, 25.0f, 2.0f, 1 };
    const int N = 2000000;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < N; i++) {
        in.v = 3.9f - 0.0005f * (float)(i % 200);
        in.i_inst = in.i_avg = (i & 64) ? -0.02f : -0.04f;
        bat_est_step(&e, &in);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    bat_model_set_ridge(false);
    double ns = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / N;
    printf("bat_est_step on this host: %.1f ns per step, ridge on (a proxy only, not the S31)\n", ns);
    CHECK(ns < 5000, "a step is cheap on the host (%.1f ns)", ns);
    CHECK(e.z >= -0.05f && e.z <= 1.05f, "still sane after %d steps", N);
}

/* test_vectors.csv as gen_c.py writes it: scenario,step,kind,z0,p0_z,p0_v,soh,use_ridge,v,i_inst,i_avg,t_c,dt,valid,
   exp_z_ekf,exp_z_corr,exp_soc_user,exp_innov,exp_v_hat,exp_p00. kind "init" restarts the filter. Learning off. */
static int split(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = 0;
        p = c + 1;
    }
    char *nl = strpbrk(f[n - 1], "\r\n");
    if (nl) *nl = 0;
    return n;
}

static int col(char **h, int n, const char *name)
{
    for (int i = 0; i < n; i++) if (!strcmp(h[i], name)) return i;
    return -1;
}

static int vectors(const char *path, double *worst_out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    static char hdr[1024], line[1024];
    char *h[32], *c[32];
    if (!fgets(hdr, sizeof hdr, f)) { fclose(f); return -2; }
    int nh = split(hdr, h, 32);
    static const char *const IN[] = { "kind", "z0", "p0_z", "p0_v", "soh", "use_ridge", "v", "i_inst", "i_avg", "t_c", "dt", "valid" };
    static const char *const OUT[] = { "exp_z_ekf", "exp_z_corr", "exp_soc_user", "exp_innov", "exp_v_hat", "exp_p00" };
    int ci[12], co[6];
    for (int k = 0; k < 12; k++) if ((ci[k] = col(h, nh, IN[k])) < 0) { printf("vectors: no column %s\n", IN[k]); fclose(f); return -2; }
    for (int k = 0; k < 6; k++) co[k] = col(h, nh, OUT[k]);
    bat_est_t e;
    int rows = 0, bad = 0;
    double worst = 0;
    bool started = false;
    while (fgets(line, sizeof line, f)) {
        int n = split(line, c, 32);
        if (n < nh) continue;
        bat_model_set_ridge(atof(c[ci[5]]) > 0.5);
        if (!strcmp(c[ci[0]], "init")) {
            bat_est_init(&e, (float)atof(c[ci[1]]), (float)atof(c[ci[2]]), (float)atof(c[ci[3]]), (float)atof(c[ci[4]]), false);
            started = true;
            continue;
        }
        if (!started) continue;
        bat_in_t in = { (float)atof(c[ci[6]]), (float)atof(c[ci[7]]), (float)atof(c[ci[8]]), (float)atof(c[ci[9]]),
                        (float)atof(c[ci[10]]), atoi(c[ci[11]]) };
        bat_est_step(&e, &in);
        const double got[6] = { e.z_ekf, e.z_corr, e.soc_user, e.innov, e.v_hat, e.p00 };
        for (int k = 0; k < 6; k++) {
            if (co[k] < 0 || !c[co[k]][0]) continue;
            double d = fabs(got[k] - atof(c[co[k]]));
            if (d > worst) worst = d;
            if (d > 1e-4 && bad++ < 5) printf("vector %s row %d %s: got %.7g, expected %s\n", c[0], rows, OUT[k], got[k], c[co[k]]);
        }
        rows++;
    }
    fclose(f);
    bat_model_set_ridge(false);
    *worst_out = worst;
    printf("vectors %s: %d steps, worst difference %.2e\n", path, rows, worst);
    return bad ? -3 : rows;
}

/* the reader itself: vectors written by this port must read back with no difference */
static void test_vector_reader(void)
{
    const char *path = "bat_selfvec.csv";
    FILE *f = fopen(path, "w");
    if (!f) { CHECK(0, "cannot write %s", path); return; }
    fprintf(f, "scenario,step,kind,z0,p0_z,p0_v,soh,use_ridge,v,i_inst,i_avg,t_c,dt,valid,exp_z_ekf,exp_z_corr,exp_soc_user,exp_innov,exp_v_hat,exp_p00\n");
    cell_t s = { 0.7, 0.0, Q };
    bat_est_t e;
    bat_est_init(&e, 0.5f, 0.04f, 0.001f, 0.9f, false);
    fprintf(f, "self,-1,init,0.5,0.04,0.001,0.9,0,,,,,,,,,,,,\n");
    for (int k = 0; k < 300; k++) {
        double i = k < 150 ? -0.3 * Q : 0.2 * Q;
        cell_step(&s, i);
        bat_in_t in = { (float)cell_v(&s, i), (float)i, (float)i, 25.0f, 2.0f, k % 17 != 0 };
        bat_est_step(&e, &in);
        fprintf(f, "self,%d,step,,,,0.9,0,%.9g,%.9g,%.9g,25,2,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n", k, in.v, in.i_inst, in.i_avg, in.valid,
                e.z_ekf, e.z_corr, e.soc_user, e.innov, e.v_hat, e.p00);
    }
    fclose(f);
    double w = 0;
    int r = vectors(path, &w);
    CHECK(r == 300 && w < 1e-6, "vector reader: %d steps, worst %.2e", r, w);
    remove(path);
}

int main(void)
{
    Q = bat_model_qnom_ah();
    circuit_from_model(1.0);
    printf("bat_model.h: %s%s, Q %.4f Ah, R0 %.4f/%.4f V/C, R1 %.4f V/C, tau %.0f s\n", bat_model_id(),
           bat_model_placeholder() ? " (PLACEHOLDER)" : "", Q, R0C, R0D, R1, TAU);
    test_vector_reader();
    test_ocv();
    test_discharge();
    test_charge();
    test_wrong_init();
    test_mismatch();
    test_capacity_and_dcir();
    test_cost();
    const char *vp = getenv("BAT_VECTORS");
    double w = 0;
    if (bat_model_placeholder()) printf("test_vectors.csv: skipped, the vectors belong to the fitted bat_model.h (this one is the placeholder)\n");
    else {
        int r = vp ? vectors(vp, &w) : -1;
        if (r == -1) printf("test_vectors.csv: not found (%s), vector check skipped\n", vp ? vp : "BAT_VECTORS unset");
        else CHECK(r > 0, "test_vectors.csv: the C port within 1e-4 of the reference (%d, worst %.2e)", r, w);
    }
    printf("bat_est: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
