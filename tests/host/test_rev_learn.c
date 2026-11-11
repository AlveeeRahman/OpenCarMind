/* Host tests for CarMind R.E.V (main/rev_learn.c, rev_cal.c, rev_state.c) and the fuel source picker in econ.c.
 * Separate from test_main.c, which needs sys_event. Build and run:  make -C tests/host rev
 * Covers a synthetic car whose fuel head differs from the factory prior, the gate, the reset, persistence with damaged files, plus a double-precision reference solver beside the
 * board's float one. */
#include <stdio.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "rev_learn.h"
#include "rev_torque.h"
#include "rev_tank.h"
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
   full matrices, float64, the same gate, cap, reset, Huber weights and outlier rejection */
typedef struct {
    double w[REV_NF], A[REV_NF][REV_NF], b[REV_NF];
    double ring[REV_HOLDOUT][REV_NF + 2];          /* phi, y, Huber weight */
    double s;
    int head, n_ring, n, commits, resets, seen, rejected;
} ref_t;
static ref_t R;
static void ref_init(void) { memset(&R, 0, sizeof R); for (int i = 0; i < REV_NF; i++) R.w[i] = MLR_W0[i]; }
static double ref_mse(const double *w)
{
    double e = 0;
    double kk = REV_HUBER_C * fmax(R.s, 1e-6);
    for (int k = 0; k < R.n_ring; k++) {
        double s = 0; for (int i = 0; i < REV_NF; i++) s += R.ring[k][i] * w[i];
        double d = fabs(R.ring[k][REV_NF] - s);
        e += d <= kk ? 0.5 * d * d : kk * (d - 0.5 * kk);
    }
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
    double pr = 0; for (int i = 0; i < REV_NF; i++) pr += R.w[i] * phi[i];
    double r = fabs(pr - y);
    if (R.seen < REV_SEED_ROWS) R.s += (r - R.s) / (R.seen + 1);
    else {
        if (r > REV_REJECT_R * fmax(R.s, 1e-6)) { R.rejected++; return 3; }
        R.s += (r - R.s) / REV_SCALE_ROWS;
    }
    R.seen++;
    double h = R.seen <= REV_SEED_ROWS ? 1.0 : fmin(1.0, REV_HUBER_C * R.s / fmax(r, 1e-9));
    double *p;
    if (R.n_ring == REV_HOLDOUT) {
        p = R.ring[R.head];
        for (int i = 0; i < REV_NF; i++) { for (int j = 0; j < REV_NF; j++) R.A[i][j] = REV_LAMBDA * R.A[i][j] + p[REV_NF + 1] * p[i] * p[j]; R.b[i] = REV_LAMBDA * R.b[i] + p[REV_NF + 1] * p[i] * p[REV_NF]; }
        R.head = (R.head + 1) % REV_HOLDOUT;
    } else {
        p = R.ring[(R.head + R.n_ring) % REV_HOLDOUT];
        R.n_ring++;
    }
    for (int i = 0; i < REV_NF; i++) p[i] = phi[i];
    p[REV_NF] = y; p[REV_NF + 1] = h;
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

/* 7. bad labels: the same car with 1 % of its labels replaced by an implausible value (a stuck MAF reading of 655 g/s gives
   215 L/h, about 100 standard units). The shipped learner made 0 commits on a real drive with 8 such labels in 898. The
   robust learner must keep learning, reject them, and end as good as on clean labels. */
static double stream_bad(int rows, double noise, int every_bad, float bad_y)
{
    double e = 0;
    float phi[REV_NF];
    char msg[96];
    for (int i = 0; i < rows; i++) {
        features(phi);
        float y = head(W_CAR, phi) + (float)(noise * nrand());
        double d = rev_fuel_predict(&L, phi) - y;                          /* scored against the true label */
        e += d * d;
        float yl = every_bad && i % every_bad == every_bad / 2 ? bad_y : y;
        if (rev_fuel_add(&L, phi, yl)) rev_fuel_commit(&L, msg, sizeof msg);
    }
    return e / rows;
}

static void test_bad_labels(void)
{
    car_features_init();
    rev_fuel_init(&L, MLR_W0); rng = 4242; car_init(1.0f);
    for (int d = 0; d < 6; d++) stream_bad(500, 0.1, 0, 0);
    double clean = stream_bad(500, 0.1, 0, 0);
    uint32_t clean_commits = L.commits;

    rev_fuel_init(&L, MLR_W0); rng = 4242; car_init(1.0f);
    for (int d = 0; d < 6; d++) stream_bad(500, 0.1, 100, 100.0f);
    double bad = stream_bad(500, 0.1, 100, 100.0f);
    if (verbose) fprintf(stderr, "bad labels: clean %.4f (%u commits), 1 %% bad %.4f (%u commits, %u rejected), drift %.3f\n",
                         clean, clean_commits, bad, L.commits, L.rejected, (double)rev_fuel_drift(&L));
    CHECK(L.commits > 0);
    CHECK(L.rejected >= 30);                                               /* 35 bad labels; the first 30 pairs seed the scale */
    CHECK(bad < 1.10 * clean);
    CHECK(rev_fuel_drift(&L) < REV_DRIFT);
    float phi[REV_NF]; features(phi);
    CHECK(isfinite(rev_fuel_predict(&L, phi)));

    /* the scale is seeded, not trusted from the start: a reset clears it */
    rev_fuel_reset(&L);
    CHECK(L.s == 0.0f && L.seen == 0 && L.rejected == 0);
    rev_fuel_reset(&L);
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

/* ---------------------------------------------------------------- torque from physics (expected values: a float C
   reference that matches a double-precision version to 3 decimals) */
static void phys_for(rev_phys_t *P, float vd_l)
{
    rev_phys_init(P);
    P->abs.k = vd_l * 1.184f / 12000.0f; P->abs.n = P->abs.n_sig = 100; P->abs.sxx = 1;
}

static void test_phys_maf_fault(void)
{
    rev_phys_t P; rev_phys_init(&P);
    float est;
    CHECK(!rev_phys_row(&P, 1000, true, 40, false, 0, true, 2000, true, 655.35f, &est));   /* 0xFFFF never teaches k */
    CHECK(P.n == 0 && P.abs.n == 0);
    CHECK(rev_phys_row(&P, 2000, true, 40, false, 0, true, 2000, true, 15.0f, &est));
}

static void test_torque(void)
{
    rev_tq_t T; rev_tq_init(&T);
    rev_phys_t P; phys_for(&P, 2.0f);
    rev_tq_row_t r;
    CHECK(fabsf(rev_tq_vd(&P) - 2.0f) < 1e-4f);
    rev_tq_in_t in = {.petrol = true, .rpm_ok = true, .rpm = 2000, .maf_ok = true, .maf_gps = 6, .map_ok = true, .map_kpa = 25,
                      .eqr_ok = true, .eqr = 0};
    CHECK(rev_tq_row(&T, &in, &P, &r) && r.cut == REV_CUT_KNOWN && r.t_fuel == 0 &&
          fabsf(r.t_fric - 35.539f) < 2e-3f && fabsf(r.t_b + 35.539f) < 2e-3f);                                   /* T1 */
    rev_tq_in_t a = {.petrol = true, .rpm_ok = true, .rpm = 750, .rate_ok = true, .rate_lph = 0.8f, .map_ok = true, .map_kpa = 33};
    CHECK(rev_tq_row(&T, &a, &P, &r) && r.src == REV_TQ_RATE && fabsf(r.t_fuel - 33.389f) < 2e-3f &&
          fabsf(r.t_fric - 28.546f) < 2e-3f && fabsf(r.t_b - 4.843f) < 2e-3f);                                    /* T2 */
    a.rpm = 2000; a.rate_lph = 3.0f; a.map_kpa = 45;
    CHECK(rev_tq_row(&T, &a, &P, &r) && fabsf(r.t_b - 14.598f) < 2e-3f);                                          /* T3 */
    a.rpm = 4000; a.rate_lph = 25.0f; a.map_kpa = 99;
    CHECK(rev_tq_row(&T, &a, &P, &r) && fabsf(r.t_fuel - 195.641f) < 2e-2f && fabsf(r.t_fric - 38.086f) < 2e-3f &&
          fabsf(r.t_b - 157.555f) < 2e-2f);                                                                         /* T4 */
    float t4 = r.t_b;
    rev_tq_in_t m = a; m.rate_ok = false; m.maf_ok = true; m.maf_gps = 85;
    CHECK(rev_tq_row(&T, &m, &P, &r) && r.src == REV_TQ_MAF && fabsf(r.t_b - 180.574f) < 2e-2f);                 /* T5 */
    rev_phys_t P4; phys_for(&P4, 4.0f);
    a.rate_lph = 50.0f;
    CHECK(rev_tq_row(&T, &a, &P4, &r) && fabsf(r.t_b - 315.111f) < 4e-2f && fabsf(r.t_b - 2 * t4) < 4e-2f);       /* T6 */
    rev_tq_in_t k = {.petrol = true, .rpm_ok = true, .rpm = 1500, .abs_ok = true, .abs_load = 50};
    CHECK(rev_tq_row(&T, &k, &P, &r) && r.src == REV_TQ_K && fabsf(r.t_fuel - 101.527f) < 2e-2f);                 /* T7 */
    k.rpm = 3000;
    CHECK(rev_tq_row(&T, &k, &P, &r) && fabsf(r.t_fuel - 101.527f) < 2e-2f);                                      /* rpm cancels */
    k.maf_ok = true; k.maf_gps = NAN;
    CHECK(rev_tq_row(&T, &k, &P, &r) && r.src == REV_TQ_K && fabsf(r.t_fuel - 101.527f) < 2e-2f);                 /* T11 */
    /* T8/T9: a car that reports torque. Its convention is decided from fuel-cut rows: here they read -15 %, so 0x62 is
       already brake torque; until decided, physics is used */
    rev_tq_t U; rev_tq_init(&U);
    rev_tq_in_t q = {.petrol = true, .rpm_ok = true, .rpm = 2000, .maf_ok = true, .maf_gps = 6, .eqr_ok = true, .eqr = 0,
                     .tq_ok = true, .tq_pct = -15, .ref_ok = true, .ref_nm = 200};
    CHECK(rev_tq_row(&U, &q, &P, &r) && r.src == REV_TQ_MAF);
    for (int i = 1; i < REV_TQ_CONV_ROWS; i++) rev_tq_row(&U, &q, &P, &r);
    CHECK(U.pid_conv == 2);
    q.eqr = 1.0f; q.tq_pct = 30;
    CHECK(rev_tq_row(&U, &q, &P, &r) && r.src == REV_TQ_PID && fabsf(r.t_b - 60.0f) < 1e-3f);                    /* T8 */
    rev_tq_in_t slow = a; slow.rpm = 300;
    CHECK(!rev_tq_row(&T, &slow, &P, &r));                                                                          /* T10 */
    rev_tq_in_t c = {.petrol = true, .rpm_ok = true, .rpm = 1500, .load_ok = true, .load = 20, .speed_ok = true, .speed_kph = 50,
                     .prev_speed_kph = 52, .maf_ok = true, .maf_gps = 5};
    CHECK(rev_tq_row(&T, &c, &P, &r) && r.cut == REV_CUT_RULE && r.t_fuel == 0);                                  /* T12 */
    rev_tq_in_t diesel = a; diesel.petrol = false;
    CHECK(!rev_tq_row(&T, &diesel, &P, &r));                                                                        /* no physics */
    /* T13/T14: the 5 s mean */
    rev_tq_t W; rev_tq_init(&W);
    rev_tq_row_t x = {.t_b = 40, .src = REV_TQ_MAF};
    float tb; rev_tq_src_t src; bool show;
    rev_tq_push(&W, 1000, &x, 30);
    CHECK(!rev_tq_mean(&W, 1000, &tb, &src, &show));
    rev_tq_push(&W, 2000, &x, 30);
    CHECK(!rev_tq_mean(&W, 2000, &tb, &src, &show));
    x.t_b = 46; rev_tq_push(&W, 4500, &x, 30);
    CHECK(rev_tq_mean(&W, 4500, &tb, &src, &show) && fabsf(tb - 42.0f) < 1e-4f && show && src == REV_TQ_MAF);
    rev_tq_t S; rev_tq_init(&S);
    rev_tq_push(&S, 1000, &x, 1); rev_tq_push(&S, 2500, &x, 1); rev_tq_push(&S, 4500, &x, 1);
    CHECK(rev_tq_mean(&S, 4500, &tb, &src, &show) && !show);                                                       /* T14 */
    rev_tq_push(&S, 20000, &x, 30);
    CHECK(!rev_tq_mean(&S, 20000, &tb, &src, &show));                                                               /* gap restarts */
    rev_phys_t Pbig; phys_for(&Pbig, 12.0f);
    CHECK(rev_tq_vd(&Pbig) == REV_TQ_VD_MAX);                                                                       /* T15 */
    rev_phys_t P0; rev_phys_init(&P0);
    CHECK(fabsf(rev_tq_vd(&P0) - 12000.0f * REV_PHYS_K0 / 1.184f) < 1e-4f);                                        /* k0 until learned */
    rev_tq_in_t big = a; big.rate_lph = 99; big.rpm = 1000;
    CHECK(rev_tq_row(&T, &big, &P, &r) && r.t_b <= 200.0f * 2.0f + 1e-3f);                                         /* clip */
}

/* rev_blob_save/load with a blob far over the 1 KB static scratch: a 35,168 B learned state. Before the fix
   rev_blob_load returned false for any size over 1 KB, so that state never reloaded. */
static void test_big_blob(void)
{
    const char *path = "/tmp/ocm_rev_big_blob_test.bin";
    char tmp[64]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    remove(path); remove(tmp);
    const uint32_t N = 35168, MAG = 0x31564541u;
    uint8_t *a = malloc(N), *b = malloc(N), *keep = malloc(N);
    for (uint32_t i = 0; i < N; i++) a[i] = (uint8_t)(i * 2654435761u >> 24);
    CHECK(rev_blob_save(path, MAG, a, N));
    memset(b, 0xA5, N);
    CHECK(rev_blob_load(path, MAG, b, N));                                /* the whole 35,168 B comes back */
    CHECK(memcmp(a, b, N) == 0);
    memset(b, 0xA5, N); memcpy(keep, b, N);
    CHECK(!rev_blob_load(path, MAG, b, N - 8));                            /* stored size differs from the caller's */
    CHECK(!rev_blob_load(path, MAG ^ 1, b, N));                            /* wrong magic */
    FILE *f = fopen(path, "r+b");                                          /* one payload byte flipped: CRC fails */
    fseek(f, 8 + 20000, SEEK_SET); int c = fgetc(f); fseek(f, 8 + 20000, SEEK_SET); fputc(c ^ 0x40, f); fclose(f);
    CHECK(!rev_blob_load(path, MAG, b, N));
    CHECK(memcmp(b, keep, N) == 0);                                        /* a rejected file leaves the caller's data alone */
    CHECK(rev_blob_save(path, MAG, a, N));
    f = fopen(path, "r+b"); fseek(f, 0, SEEK_END); long len = ftell(f); fclose(f);
    CHECK(truncate(path, len - 1) == 0);                                   /* truncated: rejected */
    CHECK(!rev_blob_load(path, MAG, b, N));
    CHECK(rev_blob_save(path, MAG, a, 64));                                /* small blobs still use the static path */
    memset(b, 0, 64);
    CHECK(rev_blob_load(path, MAG, b, 64) && memcmp(a, b, 64) == 0);
    /* a reset between the saver's remove() and rename() leaves only the .tmp: it loads, and the next save first moves
       it into place (it would otherwise be truncated while the new copy is written), then replaces it */
    CHECK(rename(path, tmp) == 0);
    memset(b, 0, 64);
    CHECK(rev_blob_load(path, MAG, b, 64) && memcmp(a, b, 64) == 0);
    CHECK(rev_blob_save(path, MAG, a + 64, 64));
    struct stat st;
    CHECK(stat(tmp, &st) != 0);
    CHECK(rev_blob_load(path, MAG, b, 64) && memcmp(a + 64, b, 64) == 0);
    remove(path); remove(tmp);
    free(a); free(b); free(keep);
}

static void test_tank_file(void)
{
    const char *path = "/tmp/ocm_rev_tank_test.bin";
    char tmp[64]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    remove(path); remove(tmp);
    rev_tank_t a, b; rev_tank_init(&a); rev_tank_init(&b);
    a.pxx = 12.5f; a.pxy = -3.0f; a.segs = 7; a.sug_seq = 3;
    CHECK(sizeof(rev_tank_t) <= 1024);
    CHECK(!rev_blob_load(path, 0x4B4E4154u, &b, sizeof b) && b.segs == 0);       /* no file: unchanged */
    CHECK(rev_blob_save(path, 0x4B4E4154u, &a, sizeof a));
    CHECK(rev_blob_load(path, 0x4B4E4154u, &b, sizeof b) && !memcmp(&a, &b, sizeof a) && rev_tank_valid(&b));
    CHECK(!rev_blob_load(path, 0x12345678u, &b, sizeof b));                       /* wrong magic */
    FILE *f = fopen(path, "r+b"); fseek(f, 12, SEEK_SET); fputc(0x5A, f); fclose(f);
    rev_tank_init(&b);
    CHECK(!rev_blob_load(path, 0x4B4E4154u, &b, sizeof b) && b.segs == 0);       /* flipped byte: CRC fails, unchanged */
    rename(path, tmp);
    CHECK(!rev_blob_load(path, 0x4B4E4154u, &b, sizeof b));                       /* only a damaged .tmp: refused */
    remove(tmp);
    CHECK(rev_blob_save(path, 0x4B4E4154u, &a, sizeof a) && rename(path, tmp) == 0);
    CHECK(rev_blob_load(path, 0x4B4E4154u, &b, sizeof b) && b.segs == 7);        /* power cut before rename: .tmp is used */
    remove(tmp);
}

int main(void)
{
    verbose = getenv("REV_VERBOSE") != NULL;
    /* the generated header's tables are used here so the compiler sees them read */
    CHECK(MLR_N_ESTIMATES == 3 && MLR_FUEL_INDEX == 2 && MLR_Z_DIM + 1 == REV_NF && MLR_NORM_STD[0] > 0 && MLR_EST_STD[2] > 0 &&
          MLR_TRAINED[9] == 0 && MLR_NORM_MEAN[3] > 0 && MLR_EST_MEAN[2] > 0 && MLR_CLASSES[0] && MLR_ESTIMATES[2]);
    test_fuel_head();
    test_float_vs_double();
    test_bad_labels();
    test_state();
    test_labels();
    test_physics();
    test_calibration();
    test_picker();
    test_phys_maf_fault();
    test_torque();
    test_tank_file();
    test_big_blob();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
