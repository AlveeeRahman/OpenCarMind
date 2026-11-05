/* Host tests for CarMind R.E.V (main/rev_learn.c, rev_cal.c, rev_state.c) and the fuel source picker in econ.c.
 * Separate from test_main.c, which needs sys_event. Build and run:  make -C tests/host rev
 * Covers a synthetic car whose fuel head differs from the factory prior, the gate, the reset, persistence with damaged files, plus a double-precision reference solver beside the
 * board's float one. */
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "rev_learn.h"
#include "rev_cal.h"
#include "rev_state.h"
#include "econ.h"
#include "ml_params_rev.h"

static int fails, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
static bool verbose;

/* ---------------------------------------------------------------- a synthetic car */
static unsigned long rng = 12345;
static double urand(void) { rng = rng * 6364136223846793005UL + 1442695040888963407UL; return (double)((rng >> 11) & 0xFFFFFFFFFFFFFUL) / 4503599627370496.0; }
static double nrand(void) { double u = urand() + 1e-12, v = urand(); return sqrt(-2 * log(u)) * cos(6.283185307179586 * v); }

/* z like the device's (int16 at 2^-9 after a ReLU): sparse, non-negative, up to about 50, driven by a latent engine load. */
static float Za[64], Zb[64], Zs[64];
static void car_features_init(void)
{
    for (int i = 0; i < 64; i++) { Za[i] = (float)(2 * urand() - 1); Zb[i] = (float)(urand() - 0.6); Zs[i] = (float)(5 + 40 * urand()); }
}
static void features(float phi[REV_NF])
{
    double s = urand(), s2 = urand();
    for (int i = 0; i < 64; i++) {
        double v = Za[i] * (i % 2 ? s : s2) + Zb[i] + 0.15 * nrand();
        phi[i] = v > 0 ? (float)(v * Zs[i]) : 0.0f;
    }
    phi[64] = 1.0f;
}
static float head(const float *w, const float *phi) { double s = 0; for (int i = 0; i < REV_NF; i++) s += (double)w[i] * phi[i]; return (float)s; }

static float W_CAR[REV_NF];                        /* this car's true head: the factory head plus a change */
static void car_init(float change)
{
    for (int i = 0; i < REV_NF; i++) W_CAR[i] = MLR_W0[i] + change * (float)(0.04 * nrand());
    W_CAR[64] = MLR_W0[64] + change * 0.3f;
}

/* ---------------------------------------------------------------- a double-precision reference, written independently:
   full matrices, float64, the same gate, cap and reset */
typedef struct {
    double w[REV_NF], A[REV_NF][REV_NF], b[REV_NF];
    double ring[REV_HOLDOUT][REV_NF + 1];
    int head, n_ring, n, commits, resets;
} ref_t;
static ref_t R;
static void ref_init(void) { memset(&R, 0, sizeof R); for (int i = 0; i < REV_NF; i++) R.w[i] = MLR_W0[i]; }
static double ref_mse(const double *w)
{
    double e = 0;
    for (int k = 0; k < R.n_ring; k++) { double s = 0; for (int i = 0; i < REV_NF; i++) s += R.ring[k][i] * w[i]; double d = R.ring[k][REV_NF] - s; e += d * d; }
    return e / R.n_ring;
}
static int ref_commit(double cand[REV_NF])         /* 1 accepted, 2 reset, 0 kept, -1 failed */
{
    static double M[REV_NF][REV_NF];
    double x[REV_NF];
    for (int i = 0; i < REV_NF; i++) { for (int j = 0; j < REV_NF; j++) M[i][j] = R.A[i][j] + (i == j ? REV_ALPHA : 0); x[i] = R.b[i] + REV_ALPHA * MLR_W0[i]; }
    for (int i = 0; i < REV_NF; i++)
        for (int j = 0; j <= i; j++) {
            double s = M[i][j];
            for (int k = 0; k < j; k++) s -= M[i][k] * M[j][k];
            if (i == j) { if (s <= 0) return -1; M[i][i] = sqrt(s); } else M[i][j] = s / M[j][j];
        }
    for (int i = 0; i < REV_NF; i++) { double s = x[i]; for (int k = 0; k < i; k++) s -= M[i][k] * x[k]; x[i] = s / M[i][i]; }
    for (int i = REV_NF - 1; i >= 0; i--) { double s = x[i]; for (int k = i + 1; k < REV_NF; k++) s -= M[k][i] * x[k]; x[i] = s / M[i][i]; }
    memcpy(cand, x, sizeof x);
    double w0[REV_NF], n0 = 0, dd = 0;
    for (int i = 0; i < REV_NF; i++) { w0[i] = MLR_W0[i]; n0 += w0[i] * w0[i]; dd += (x[i] - w0[i]) * (x[i] - w0[i]); }
    double cur = ref_mse(R.w), cm = ref_mse(x), pr = ref_mse(w0);
    if (cm < REV_GATE * cur && sqrt(dd / n0) < REV_DRIFT) { memcpy(R.w, x, sizeof x); R.commits++; return 1; }
    if (cur > REV_RESET * pr) { memcpy(R.w, w0, sizeof w0); R.resets++; return 2; }
    return 0;
}
static int ref_add(const float *phi, float y)      /* returns the commit outcome, or 3 when no commit was due */
{
    if (R.n_ring == REV_HOLDOUT) {
        double *p = R.ring[R.head];
        for (int i = 0; i < REV_NF; i++) { for (int j = 0; j < REV_NF; j++) R.A[i][j] = REV_LAMBDA * R.A[i][j] + p[i] * p[j]; R.b[i] = REV_LAMBDA * R.b[i] + p[i] * p[REV_NF]; }
        for (int i = 0; i < REV_NF; i++) p[i] = phi[i];
        p[REV_NF] = y;
        R.head = (R.head + 1) % REV_HOLDOUT;
    } else {
        double *p = R.ring[(R.head + R.n_ring) % REV_HOLDOUT];
        for (int i = 0; i < REV_NF; i++) p[i] = phi[i];
        p[REV_NF] = y;
        R.n_ring++;
    }
    R.n++;
    if (R.n % REV_EVERY == 0 && R.n_ring == REV_HOLDOUT) { double c[REV_NF]; return ref_commit(c); }
    return 3;
}

static rev_fuel_t L;                               /* 75 KB: static, as on the board (PSRAM) */

/* Prequential stream: score each pair with the live head before it is learned. Returns the mean squared error; *prior_mse gets
   the factory head's error on the same pairs. */
static double stream(int rows, double noise, double *prior_mse)
{
    double e = 0, ep = 0;
    float phi[REV_NF];
    char msg[96];
    for (int i = 0; i < rows; i++) {
        features(phi);
        float y = head(W_CAR, phi) + (float)(noise * nrand());
        double d = rev_fuel_predict(&L, phi) - y, dp = rev_fuel_predict_prior(&L, phi) - y;
        e += d * d; ep += dp * dp;
        if (rev_fuel_add(&L, phi, y)) rev_fuel_commit(&L, msg, sizeof msg);
    }
    if (prior_mse) *prior_mse = ep / rows;
    return e / rows;
}

static void test_fuel_head(void)
{
    car_features_init();
    double nw0 = 0; for (int i = 0; i < REV_NF; i++) nw0 += (double)MLR_W0[i] * MLR_W0[i];
    CHECK(nw0 > 0 && isfinite(nw0));                                      /* the generated prior is usable */

    /* 1. a car whose head differs from the factory one: six drives of 500 pairs, then the learned head beats the prior */
    rev_fuel_init(&L, MLR_W0);
    car_init(1.0f);
    double pr0, e0 = stream(500, 0.1, &pr0);
    for (int d = 0; d < 5; d++) stream(500, 0.1, NULL);
    double pr1, e1 = stream(500, 0.1, &pr1);
    if (verbose) fprintf(stderr, "car: first drive %.4f (prior %.4f), last drive %.4f (prior %.4f), commits %u resets %u kept %u drift %.3f\n",
                         e0, pr0, e1, pr1, L.commits, L.resets, L.kept, (double)rev_fuel_drift(&L));
    CHECK(L.commits > 0);
    CHECK(e1 < 0.5 * pr1);                                                /* learning on this car beats the factory head */
    CHECK(rev_fuel_drift(&L) < REV_DRIFT);
    CHECK(L.learned == 3500 - REV_HOLDOUT);                               /* every pair but the held-out ones reached A and b */

    /* 2. the car is the factory car: noise only. The head must stay near w0 and finite, and almost never move */
    rev_fuel_init(&L, MLR_W0);
    memcpy(W_CAR, MLR_W0, sizeof W_CAR);
    double prn, en = stream(3000, 0.3, &prn);
    if (verbose) fprintf(stderr, "noise: learned %.4f prior %.4f commits %u kept %u drift %.3f\n", en, prn, L.commits, L.kept, (double)rev_fuel_drift(&L));
    CHECK(L.commits <= 5);                                                /* of 95 solves */
    CHECK(en < 1.05 * prn);
    CHECK(rev_fuel_drift(&L) < 0.5);
    float phi[REV_NF]; features(phi);
    CHECK(isfinite(rev_fuel_predict(&L, phi)));

    /* 3. drift cap and reset: statistics from a car far from the factory head (the candidate drifts past 3 |w0|), a holdout
       from the factory car, and live weights that have gone bad. The candidate must be refused and the head reset to w0. */
    rev_fuel_init(&L, MLR_W0);
    for (int i = 0; i < REV_NF; i++) W_CAR[i] = MLR_W0[i] * 12.0f;
    for (int i = 0; i < 2000; i++) { features(phi); rev_fuel_add(&L, phi, head(W_CAR, phi)); }
    memcpy(W_CAR, MLR_W0, sizeof W_CAR);
    for (int i = 0; i < REV_HOLDOUT; i++) {                               /* refill the holdout only, A and b keep the far car */
        features(phi);
        memcpy(L.ring[i].phi, phi, sizeof phi); L.ring[i].y = head(W_CAR, phi);
    }
    L.ring_head = 0; L.ring_n = REV_HOLDOUT;
    for (int i = 0; i < REV_NF; i++) L.w[L.live][i] = MLR_W0[i] * 1.3f; /* live head 30 % off: worse than the factory head */
    char msg[96];
    rev_commit_t r = rev_fuel_commit(&L, msg, sizeof msg);
    if (verbose) fprintf(stderr, "cap/reset: %s\n", msg);
    CHECK(r == REV_RESET_PRIOR && L.resets == 1 && L.commits == 0);
    CHECK(memcmp(L.w[L.live], MLR_W0, sizeof L.w[0]) == 0);
    CHECK(rev_fuel_drift(&L) == 0.0f);
    /* a far candidate alone (live head = prior, so no reset): refused by the cap, kept */
    r = rev_fuel_commit(&L, msg, sizeof msg);
    CHECK(r == REV_KEPT && L.commits == 0);

    /* not enough data yet, and pairs that are not finite */
    rev_fuel_init(&L, MLR_W0);
    CHECK(rev_fuel_commit(&L, msg, sizeof msg) == REV_WAIT && strstr(msg, "waiting"));
    features(phi); phi[3] = NAN;
    CHECK(!rev_fuel_add(&L, phi, 1.0f) && L.ring_n == 0);
    features(phi);
    CHECK(!rev_fuel_add(&L, phi, INFINITY) && L.ring_n == 0);
    rev_fuel_reset(&L);
}

/* 6. float on the board against the double reference, same pairs: the gate must decide the same way */
static void test_float_vs_double(void)
{
    rev_fuel_init(&L, MLR_W0);
    ref_init();
    rng = 777; car_features_init(); car_init(1.0f);
    int solves = 0, same = 0;
    double worst = 0, worst_mse = 0;
    float phi[REV_NF];
    char msg[96];
    for (int drive = 0; drive < 8; drive++) {
        if (drive == 4) car_init(1.5f);                                   /* a change part way: the car is serviced, a new tank */
        for (int i = 0; i < 600; i++) {
            features(phi);
            float y = head(W_CAR, phi) + (float)(0.15 * nrand());
            bool due = rev_fuel_add(&L, phi, y);
            int ro = ref_add(phi, y);
            if (!due) continue;
            rev_commit_t fo = rev_fuel_commit(&L, msg, sizeof msg);
            int fo_i = fo == REV_ACCEPTED ? 1 : fo == REV_RESET_PRIOR ? 2 : fo == REV_SOLVE_FAILED ? -1 : 0;
            solves++;
            if (fo_i == ro) same++;
            /* the live weights after each decision, relative to their norm, and the predictions they give */
            double num = 0, den = 0, fm = 0;
            for (int k = 0; k < REV_NF; k++) { double d = (double)L.w[L.live][k] - R.w[k]; num += d * d; den += R.w[k] * R.w[k]; }
            double rel = sqrt(num / den);
            if (rel > worst) worst = rel;
            for (int k = 0; k < 100; k++) {
                features(phi);
                double e = 0; for (int j = 0; j < REV_NF; j++) e += R.w[j] * phi[j];
                fm += (rev_fuel_predict(&L, phi) - e) * (rev_fuel_predict(&L, phi) - e);
            }
            if (fm / 100 > worst_mse) worst_mse = fm / 100;
            /* keep both learners on the same weights so one early disagreement cannot snowball */
            for (int k = 0; k < REV_NF; k++) R.w[k] = L.w[L.live][k];
        }
    }
    if (verbose) fprintf(stderr, "float vs double: %d of %d gate decisions agree, worst relative weight difference %.2e, worst prediction MSE %.2e, commits %u/%d\n",
                         same, solves, worst, worst_mse, L.commits, R.commits);
    CHECK(solves > 100);
    CHECK(same * 100 >= solves * 99);
    CHECK(worst < 1e-3);
}

/* 5. persistence */
static void test_state(void)
{
    const char *path = "/tmp/carmind_rev_test.bin";
    char tmp[64]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    remove(path); remove(tmp);
    static rev_phys_t P, P2; static rev_pick_t K, K2; static rev_cal_t C, C2;
    rev_phys_init(&P); rev_pick_init(&K); rev_cal_init(&C);
    rev_fuel_init(&L, MLR_W0);
    rng = 99; car_features_init(); car_init(1.0f);
    stream(1500, 0.1, NULL);
    CHECK(L.commits > 0);
    P.abs.k = 3.1e-4f; P.abs.sxx = 5.0; P.abs.sxy = 1.55e-3; P.abs.n = P.abs.n_sig = 40;
    K.e[1] = 0.25f; K.n[1] = 77;
    rev_cal_row(&C, 7, 1.0f, 3.6f, 50, true);
    C.c = 1.07f;
    float phi[REV_NF]; features(phi);
    float before = rev_fuel_predict(&L, phi);
    uint32_t commits = L.commits;
    CHECK(rev_state_save(path, &L, &P, &K, &C, MLR_PRIOR_ID));
    CHECK(access(tmp, F_OK) != 0);                                        /* the .tmp was renamed into place */

    rev_fuel_init(&L, MLR_W0); rev_phys_init(&P2); rev_pick_init(&K2); rev_cal_init(&C2);
    CHECK(rev_state_load(path, &L, &P2, &K2, &C2, MLR_PRIOR_ID) == REV_LOAD_OK);
    CHECK(rev_fuel_predict(&L, phi) == before && L.commits == commits);
    CHECK(P2.abs.k == 3.1e-4f && K2.n[1] == 77 && C2.c == 1.07f && C2.ledger_n == 1);

    /* another model's state: the fuel head starts from the prior, physics and calibration stay */
    rev_fuel_init(&L, MLR_W0); rev_cal_init(&C2);
    CHECK(rev_state_load(path, &L, &P2, &K2, &C2, MLR_PRIOR_ID ^ 1) == REV_LOAD_OTHER_MODEL);
    CHECK(rev_fuel_predict(&L, phi) == rev_fuel_predict_prior(&L, phi) && L.commits == 0 && C2.c == 1.07f);

    /* a flipped byte: the CRC refuses it, nothing changes */
    FILE *f = fopen(path, "r+b"); fseek(f, 400, SEEK_SET); int ch = fgetc(f); fseek(f, 400, SEEK_SET); fputc(ch ^ 0x40, f); fclose(f);
    rev_fuel_init(&L, MLR_W0); rev_cal_init(&C2);
    CHECK(rev_state_load(path, &L, &P2, &K2, &C2, MLR_PRIOR_ID) == REV_LOAD_NONE);
    CHECK(L.commits == 0 && C2.c == 1.0f);

    /* a power cut between remove and rename: only the .tmp is left, and it is used */
    rev_fuel_init(&L, MLR_W0); stream(1500, 0.1, NULL);
    features(phi); before = rev_fuel_predict(&L, phi);
    CHECK(rev_state_save(path, &L, &P, &K, &C, MLR_PRIOR_ID));
    rename(path, tmp);
    rev_fuel_init(&L, MLR_W0);
    CHECK(rev_state_load(path, &L, &P2, &K2, &C2, MLR_PRIOR_ID) == REV_LOAD_TMP);
    CHECK(rev_fuel_predict(&L, phi) == before);

    /* a truncated file */
    f = fopen(tmp, "r+b"); CHECK(ftruncate(fileno(f), 5000) == 0); fclose(f);
    rev_fuel_init(&L, MLR_W0);
    CHECK(rev_state_load(path, &L, &P2, &K2, &C2, MLR_PRIOR_ID) == REV_LOAD_NONE);
    CHECK(rev_state_load("/tmp/carmind_rev_missing.bin", &L, &P2, &K2, &C2, MLR_PRIOR_ID) == REV_LOAD_NONE);

    /* a file without a fuel head (saved while E.V was selected) loads physics and calibration */
    CHECK(rev_state_save(path, NULL, &P, &K, &C, MLR_PRIOR_ID));
    rev_cal_init(&C2);
    CHECK(rev_state_load(path, &L, &P2, &K2, &C2, MLR_PRIOR_ID) == REV_LOAD_OK && C2.c == 1.07f && L.commits == 0);
    remove(path); remove(tmp);

    /* CRC-32 (IEEE) check value */
    CHECK(rev_crc32(0, "123456789", 9) == 0xCBF43926u);
}

/* labels: 5 s means of the measured fuel rate */
static void test_labels(void)
{
    rev_label_t r; rev_label_reset(&r);
    float m;
    CHECK(!rev_label_mean(&r, 10000, &m));
    rev_label_push(&r, 7000, 2.0f);
    CHECK(!rev_label_mean(&r, 7000, &m));                                 /* one row is not a mean */
    rev_label_push(&r, 8400, 4.0f);
    CHECK(!rev_label_mean(&r, 8400, &m));                                 /* 1.4 s span: too short */
    rev_label_push(&r, 9800, 6.0f);
    CHECK(rev_label_mean(&r, 9800, &m) && fabsf(m - 4.0f) < 1e-6f);
    rev_label_push(&r, 11200, 0.0f);                                      /* decel fuel cut is a real 0 */
    CHECK(rev_label_mean(&r, 11200, &m) && fabsf(m - 3.0f) < 1e-6f);     /* 4 rows in (6200, 11200] */
    CHECK(rev_label_mean(&r, 12500, &m) && fabsf(m - (4 + 6 + 0) / 3.0f) < 1e-6f);   /* (7500, 12500] */
    rev_label_push(&r, 12600, 400.0f);
    CHECK(rev_label_mean(&r, 12600, &m) && m == 100.0f);                  /* (4 + 6 + 0 + 400) / 4: clipped as econ clips estimates */
    rev_label_push(&r, 13000, -1.0f);                                     /* unknown: not pushed */
    CHECK(r.n == 5);
}

/* 8. physics */
static void test_physics(void)
{
    static rev_phys_t P;
    rev_phys_init(&P);
    float est, mm;
    bool ready;
    /* before any data: absolute load uses the factory k, calculated load has none */
    CHECK(rev_phys_estimate(&P, true, 40, false, 0, true, 2000, &est, &ready) && !ready && fabsf(est - REV_PHYS_K0 * 80000) < 1e-3f);
    CHECK(!rev_phys_estimate(&P, false, 0, true, 40, true, 2000, &est, &ready));
    /* engine off and missing fields teach nothing */
    for (int i = 0; i < 50; i++) rev_phys_row(&P, i * 1400, true, 0, true, 0, true, 0, true, 0.0f, &mm);
    CHECK(!rev_k_ready(&P.abs) && P.abs.k == REV_PHYS_K0);
    CHECK(!rev_phys_row(&P, 80000, true, 30, true, 30, true, 2000, false, 0, &mm));
    CHECK(!rev_phys_row(&P, 81000, false, 0, false, 0, true, 2000, true, 10, &mm));
    /* fewer than 20 rows with real airflow keep k0 */
    rev_phys_init(&P);
    int64_t t = 0;
    rng = 5;
    for (int i = 0; i < 15; i++) { t += 1400; float al = 20 + 40 * (float)urand(), rpm = 900 + 2500 * (float)urand(); rev_phys_row(&P, t, true, al, false, 0, true, rpm, true, 3e-4f * al * rpm, &mm); }
    CHECK(!rev_k_ready(&P.abs) && P.abs.k == REV_PHYS_K0);
    /* a car with k = 3e-4 (about a 3 L engine) and 5 % noise: converges within 2 % */
    for (int i = 0; i < 400; i++) {
        t += 1400; float al = 20 + 40 * (float)urand(), rpm = 900 + 2500 * (float)urand();
        rev_phys_row(&P, t, true, al, false, 0, true, rpm, true, 3e-4f * al * rpm * (float)(1 + 0.05 * nrand()), &mm);
    }
    if (verbose) fprintf(stderr, "physics: k %.4e after %u rows\n", (double)P.abs.k, P.abs.n);
    CHECK(rev_k_ready(&P.abs) && fabsf(P.abs.k / 3e-4f - 1) < 0.02f);
    CHECK(rev_phys_estimate(&P, true, 50, false, 0, true, 2000, &est, &ready) && ready && fabsf(est / (3e-4f * 100000) - 1) < 0.02f);
    /* one glitched row (MAF 600 g/s) moves k by a bounded amount, and forgetting brings it back */
    float k_before = P.abs.k;
    t += 1400; rev_phys_row(&P, t, true, 30, false, 0, true, 1500, true, 600.0f, &mm);
    CHECK(P.abs.k > k_before && P.abs.k < 3 * k_before);
    for (int i = 0; i < 400; i++) { t += 1400; float al = 20 + 40 * (float)urand(), rpm = 900 + 2500 * (float)urand(); rev_phys_row(&P, t, true, al, false, 0, true, rpm, true, 3e-4f * al * rpm, &mm); }
    CHECK(fabsf(P.abs.k / 3e-4f - 1) < 0.02f);
    /* EXPERIMENTAL calculated-load path: one k per rpm bin, used only once that bin has learned */
    rev_phys_init(&P);
    CHECK(rev_phys_bin(1499) == 0 && rev_phys_bin(1500) == 1 && rev_phys_bin(3499) == 2 && rev_phys_bin(6000) == 3);
    for (int i = 0; i < 100; i++) { t += 1400; rev_phys_row(&P, t, false, 0, true, 50, true, 1000, true, 2e-4f * 50 * 1000, &mm); }
    CHECK(rev_k_ready(&P.calc[0]) && fabsf(P.calc[0].k / 2e-4f - 1) < 0.01f && !rev_k_ready(&P.calc[2]) && !rev_k_ready(&P.abs));
    CHECK(rev_phys_estimate(&P, false, 0, true, 50, true, 1200, &est, &ready) && ready && fabsf(est - 2e-4f * 50 * 1200) < 0.05f);
    CHECK(!rev_phys_estimate(&P, false, 0, true, 50, true, 3000, &est, &ready));   /* bin 2 has not learned */
    rev_phys_reset_k(&P);
    CHECK(!rev_k_ready(&P.calc[0]) && P.abs.k == REV_PHYS_K0);
}

/* 9. fill-up calibration */
static void drives(rev_cal_t *c, uint32_t from, uint32_t to, float lph, float kph, float cover)
{
    for (uint32_t s = from; s <= to; s++)
        for (int i = 0; i < 3600; i++) {                                  /* one hour per drive, one row per second */
            bool known = (i % 100) < (int)(cover * 100 + 0.5f);
            rev_cal_row(c, s, 1.0f, known ? lph : -1, kph, true);
        }
}

static void test_calibration(void)
{
    static rev_cal_t c, d;
    rev_cal_init(&c);
    drives(&c, 1, 6, 6.0f, 60, 1.0f);                                     /* 6 L per drive, 60 km */
    CHECK(fabs(c.ledger[0].raw_l - 6.0) < 1e-3 && fabs(c.ledger[0].km - 60.0) < 1e-2);
    CHECK(fabs(c.life_l - 36) < 0.01 && fabs(c.life_km - 360) < 0.1);

    /* the first full fill only sets the start */
    rev_fill_t f[REV_CAL_FILLUPS] = {{.id = 1, .after_session = 1, .litres = 40, .full = 1}};
    CHECK(rev_cal_set_fills(&c, f, 1, 1) && c.c == 1.0f && c.used == 0 && c.n_iv == 0);
    /* a second full fill after drive 3: drives 2 and 3 burned 12 L on the board, 13.2 L pumped: c = 1.1 */
    f[1] = (rev_fill_t){.id = 2, .after_session = 3, .litres = 13.2f, .full = 1};
    CHECK(rev_cal_set_fills(&c, f, 2, 2) && fabsf(c.c - 1.1f) < 1e-4f && c.used == 1 && c.fill_rev == 2);
    /* a partial fill in between adds its litres: 6 + 7.2 = 13.2 again */
    f[1] = (rev_fill_t){.id = 3, .after_session = 2, .litres = 6.0f, .full = 0};
    f[2] = (rev_fill_t){.id = 2, .after_session = 3, .litres = 7.2f, .full = 1};
    CHECK(rev_cal_set_fills(&c, f, 3, 3) && fabsf(c.c - 1.1f) < 1e-4f && c.used == 1);
    /* the order the app sends does not matter */
    rev_fill_t g[3] = {f[2], f[0], f[1]};
    rev_cal_init(&d); drives(&d, 1, 6, 6.0f, 60, 1.0f);
    CHECK(rev_cal_set_fills(&d, g, 3, 3) && d.c == c.c);
    /* newest interval weighs most: (0.7 * 13.2 + 1 * 10.8) / (0.7 * 12 + 1 * 12) = 0.9824 */
    f[3] = (rev_fill_t){.id = 4, .after_session = 5, .litres = 10.8f, .full = 1};
    CHECK(rev_cal_set_fills(&c, f, 4, 4) && fabsf(c.c - (float)((0.7 * 13.2 + 10.8) / (0.7 * 12 + 12))) < 1e-4f && c.used == 2);
    /* a typo (400 L) is skipped; editing it to the right value gives the same c as a fresh list */
    f[3].litres = 300.0f;
    CHECK(rev_cal_set_fills(&c, f, 4, 5) && c.used == 1 && c.skipped == 1 && c.iv[1].why == REV_IV_RATIO && fabsf(c.c - 1.1f) < 1e-4f);
    f[3].litres = 10.8f;
    CHECK(rev_cal_set_fills(&c, f, 4, 6) && fabsf(c.c - (float)((0.7 * 13.2 + 10.8) / (0.7 * 12 + 12))) < 1e-4f);
    /* deleting the last entry goes back to the earlier answer */
    CHECK(rev_cal_set_fills(&c, f, 3, 7) && fabsf(c.c - 1.1f) < 1e-4f);
    /* implausible entries are refused and change nothing */
    rev_fill_t bad = {.id = 9, .after_session = 6, .litres = 0, .full = 1};
    CHECK(!rev_cal_set_fills(&c, &bad, 1, 8) && c.fill_rev == 7 && c.n_fills == 3);
    rev_fill_t many[REV_CAL_FILLUPS + 1] = {{0}};
    CHECK(!rev_cal_set_fills(&c, many, REV_CAL_FILLUPS + 1, 8));
    /* clamped to [0.7, 1.4] */
    f[2].litres = 20.0f;                                                  /* 26 L for 12 L: ratio 2.17, refused */
    CHECK(rev_cal_set_fills(&c, f, 3, 9) && c.used == 0 && c.c == 1.0f);
    f[2].litres = 17.4f;                                                  /* 23.4 / 12 = 1.95: used, clamped to 1.4 */
    CHECK(rev_cal_set_fills(&c, f, 3, 10) && c.used == 1 && c.c == REV_CAL_MAX);

    /* coverage under 95 %: the board missed fuel data, so the ratio would absorb the gap. Skipped. */
    rev_cal_init(&c);
    drives(&c, 1, 1, 6.0f, 60, 1.0f);
    drives(&c, 2, 3, 6.0f, 60, 0.9f);
    rev_fill_t h[2] = {{.id = 1, .after_session = 1, .litres = 40, .full = 1}, {.id = 2, .after_session = 3, .litres = 12, .full = 1}};
    CHECK(rev_cal_set_fills(&c, h, 2, 1) && c.used == 0 && c.iv[0].why == REV_IV_COVERAGE && c.c == 1.0f);
    /* under 5 L counted: too small to judge */
    rev_cal_init(&c);
    drives(&c, 1, 2, 4.0f, 60, 1.0f);
    rev_fill_t s2[2] = {{.id = 1, .after_session = 1, .litres = 40, .full = 1}, {.id = 2, .after_session = 2, .litres = 4.4f, .full = 1}};
    CHECK(rev_cal_set_fills(&c, s2, 2, 1) && c.iv[0].why == REV_IV_SMALL && c.c == 1.0f);

    /* a trip-computer figure: 11 L/100 km over drives 2-3 (120 km, 12 L counted) = 13.2 L, half weight */
    rev_cal_init(&c);
    drives(&c, 1, 3, 6.0f, 60, 1.0f);
    rev_fill_t tc = {.id = 5, .kind = REV_FILL_TRIP, .from_session = 2, .after_session = 3, .l_per_100km = 11.0f};
    CHECK(rev_cal_set_fills(&c, &tc, 1, 1) && c.used == 1 && c.iv[0].trip && fabsf(c.c - 1.1f) < 1e-3f);
    tc.from_session = 4;
    CHECK(!rev_cal_set_fills(&c, &tc, 1, 2));                            /* a range backwards */
    /* with a pump interval of ratio 1.0 after it: (0.5 * 0.7 * 13.2 + 12) / (0.5 * 0.7 * 12 + 12) */
    drives(&c, 4, 5, 6.0f, 60, 1.0f);
    rev_fill_t mix[3] = {{.id = 5, .kind = REV_FILL_TRIP, .from_session = 2, .after_session = 3, .l_per_100km = 11.0f},
                         {.id = 6, .after_session = 3, .litres = 30, .full = 1}, {.id = 7, .after_session = 5, .litres = 12, .full = 1}};
    CHECK(rev_cal_set_fills(&c, mix, 3, 3) && c.used == 2 && fabsf(c.c - (float)((0.35 * 13.2 + 12) / (0.35 * 12 + 12))) < 1e-4f);

    /* drives that fell out of the 64-drive ledger cannot be judged */
    rev_cal_init(&c);
    drives(&c, 1, 70, 6.0f, 60, 1.0f);
    CHECK(c.ledger_n == REV_CAL_SESSIONS && c.ledger[0].session == 7 && c.dropped_max == 6);
    rev_fill_t old[2] = {{.id = 1, .after_session = 2, .litres = 40, .full = 1}, {.id = 2, .after_session = 10, .litres = 48, .full = 1}};
    CHECK(rev_cal_set_fills(&c, old, 2, 1) && c.iv[0].why == REV_IV_GONE && c.c == 1.0f);
    rev_cal_reset(&c);
    CHECK(c.c == 1.0f && c.n_fills == 0 && c.ledger_n == REV_CAL_SESSIONS);
    /* rows from an older session id and gaps are ignored */
    double l0 = c.ledger[c.ledger_n - 1].raw_l;
    rev_cal_row(&c, 3, 1.0f, 10.0f, 50, true);
    rev_cal_row(&c, 70, 30.0f, 10.0f, 50, true);
    CHECK(c.ledger[c.ledger_n - 1].raw_l == l0);
    CHECK(strcmp(rev_iv_why_name(REV_IV_USED), "used") == 0);
}

/* 10. which estimate to use without MAF, and the fuel source picker in econ.c */
static void test_picker(void)
{
    rev_pick_t p; rev_pick_init(&p);
    CHECK(rev_pick_choose(&p, false, false) == REV_SRC_PRIOR);
    CHECK(rev_pick_choose(&p, false, true) == REV_SRC_LEARNED);
    CHECK(rev_pick_choose(&p, true, true) == REV_SRC_PHYS);
    for (int i = 0; i < 50; i++) rev_pick_score(&p, 5.0f, true, 6.0f, true, 5.2f, true, 5.5f);   /* learned is closest */
    CHECK(rev_pick_choose(&p, true, true) == REV_SRC_LEARNED);
    rev_pick_init(&p);
    for (int i = 0; i < 50; i++) rev_pick_score(&p, 5.0f, true, 5.5f, true, 5.5f, true, 5.5f);   /* a tie: physics */
    CHECK(rev_pick_choose(&p, false, false) == REV_SRC_PHYS);
    rev_pick_init(&p);
    for (int i = 0; i < 50; i++) rev_pick_score(&p, 5.0f, true, 5.1f, true, 6.0f, false, 0);      /* the car where learning hurt */
    CHECK(rev_pick_choose(&p, false, true) == REV_SRC_PRIOR);
    CHECK(!strcmp(rev_src_name(REV_SRC_PHYS), "phys"));

    fuel_src_t src;
    econ_in_t in = {.prefer = FUEL_LEARNED, .model_ok = true, .model_lph = 3.0f, .learned_ok = true, .learned_lph = 2.5f,
                    .phys_ok = true, .phys_maf_gps = 4.44f};
    CHECK(econ_fuel_pick(&in, &src) == 2.5f && src == FUEL_LEARNED);
    {   /* MAF 0xFFFF (655.35 g/s) is a sensor fault: the picker falls through to the physics estimate */
        econ_in_t m = {.maf_ok = true, .maf_gps = 655.35f, .prefer = FUEL_PHYS, .phys_ok = true, .phys_maf_gps = 4.44f};
        CHECK(econ_fuel_pick(&m, &src) > 0 && src == FUEL_PHYS);
        m.maf_gps = 12.0f;
        CHECK(econ_fuel_pick(&m, &src) > 0 && src == FUEL_MAF);
    }
    in.prefer = FUEL_PHYS;
    CHECK(fabsf(econ_fuel_pick(&in, &src) - 1.4595f) < 0.001f && src == FUEL_PHYS);   /* airflow through the MAF formula */
    in.lambda_ok = true; in.lambda = 0.0f;
    CHECK(econ_fuel_pick(&in, &src) == 0.0f && src == FUEL_PHYS);        /* fuel cut applies to the physics estimate too */
    in.lambda_ok = false;
    in.prefer = FUEL_MODEL;
    CHECK(econ_fuel_pick(&in, &src) == 3.0f && src == FUEL_MODEL);
    in.model_ok = false;                                                  /* the preferred one is missing: physics first */
    CHECK(fabsf(econ_fuel_pick(&in, &src) - 1.4595f) < 0.001f && src == FUEL_PHYS);
    in.phys_ok = false;
    CHECK(econ_fuel_pick(&in, &src) == 2.5f && src == FUEL_LEARNED);
    in.learned_lph = 150.0f;                                              /* absurd: refused like the model's */
    CHECK(econ_fuel_pick(&in, &src) < 0 && src == FUEL_NONE);
    in.maf_ok = true; in.maf_gps = 4.44f;                                 /* MAF beats every estimate */
    CHECK(fabsf(econ_fuel_pick(&in, &src) - 1.4595f) < 0.001f && src == FUEL_MAF);
    in.rate_ok = true; in.rate_lph = 2.0f;                                /* and the car's own PID beats MAF */
    CHECK(econ_fuel_pick(&in, &src) == 2.0f && src == FUEL_PID);
    CHECK(econ_src_is_estimate(FUEL_PHYS) && econ_src_is_estimate(FUEL_LEARNED) && econ_src_is_estimate(FUEL_MODEL));
    CHECK(!econ_src_is_estimate(FUEL_MAF) && !econ_src_is_estimate(FUEL_PID));
    CHECK(!strcmp(fuel_src_name(FUEL_PHYS), "phys") && !strcmp(fuel_src_name(FUEL_LEARNED), "learned"));
    CHECK(FUEL_MODEL == 3);                                               /* CSV values of the older sources did not move */
    /* econ_fuel_lph is the picker with the R.E.V sources off */
    CHECK(fabsf(econ_fuel_lph(false, 0, true, 4.44f, false, 0, 0, true, 3.0f, &src) - 1.4595f) < 0.001f && src == FUEL_MAF);
    CHECK(econ_fuel_lph(false, 0, false, 0, false, 0, 0, true, 1.3f, &src) == 1.3f && src == FUEL_MODEL);
    CHECK(fabsf(econ_maf_lph(14.5f, true, 3.0f, 4) - 3600.0f / 832.0f / 3.0f) < 0.001f);
}

int main(void)
{
    verbose = getenv("REV_VERBOSE") != NULL;
    /* the generated header's tables are used here so the compiler sees them read */
    CHECK(MLR_N_ESTIMATES == 3 && MLR_FUEL_INDEX == 2 && MLR_Z_DIM + 1 == REV_NF && MLR_NORM_STD[0] > 0 && MLR_EST_STD[2] > 0 &&
          MLR_TRAINED[9] == 0 && MLR_NORM_MEAN[3] > 0 && MLR_EST_MEAN[2] > 0 && MLR_CLASSES[0] && MLR_ESTIMATES[2]);
    test_fuel_head();
    test_float_vs_double();
    test_state();
    test_labels();
    test_physics();
    test_calibration();
    test_picker();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
