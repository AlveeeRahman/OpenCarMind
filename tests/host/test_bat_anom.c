/* bat_anom_core (main/bat_anom_core.c, main/bat_anom_model.h) against the research vectors anom_vectors.csv (warm-up
 * overridden to 2 windows, as gen_p3.py made them) to 1e-4, plus the firmware additions: the 2 s sample period, the
 * fault class, the alarm mask.   make anom  (ANOM_VECTORS = carmind-ml experiments/battery_soc/anom_vectors.csv) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bat_anom_core.h"
#include "bat_anom_model.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int split(char *line, char **f, int max)
{
    int n = 0; char *p = line;
    while (n < max) { f[n++] = p; char *c = strchr(p, ','); if (!c) break; *c = 0; p = c + 1; }
    char *nl = strpbrk(f[n - 1], "\r\n"); if (nl) *nl = 0;
    return n;
}

static int close_enough(double got, double exp, double *worst)
{
    double d = fabs(got - exp) / fmax(1.0, fabs(exp));
    if (d > *worst) *worst = d;
    return d <= 1e-4;
}

static void vectors(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) { printf("FAIL: no anomaly vectors at %s (set ANOM_VECTORS)\n", path); fails++; return; }
    char line[2048], *f[64];
    if (!fgets(line, sizeof line, fp) || !fgets(line, sizeof line, fp)) { fclose(fp); fails++; return; }
    an_params_t p = BAT_ANOM;
    p.warmup_win = 2.0f;
    static an_state_t s;
    an_out_t o;
    an_init(&p, &s, 1.0f);
    int rows = 0, wins = 0, alarms = 0;
    double worst = 0;
    while (fgets(line, sizeof line, fp)) {
        split(line, f, 64);
        an_step(&p, &s, (float)atof(f[1]), (float)atof(f[2]), (float)atof(f[3]), (float)atof(f[4]), atoi(f[5]),
                (float)atof(f[6]), (float)atof(f[7]), (float)atof(f[8]), &o);
        CHECK(o.win_done == atoi(f[9]), "window flag at k=%s", f[0]);
        if (o.win_done) {
            wins++;
            int bits = o.a_rz | (o.a_g << 1) | (o.a_ae << 2);
            CHECK(close_enough(o.s_rz, atof(f[10]), &worst) && close_enough(o.s_g, atof(f[11]), &worst) &&
                  close_enough(o.s_ae, atof(f[12]), &worst) && bits == atoi(f[13]),
                  "k=%s got %g %g %g bits %d", f[0], o.s_rz, o.s_g, o.s_ae, bits);
            float sum = 0;
            for (int j = 0; j < AN_NF; j++) sum += o.ae_err[j];
            float nm = 0;
            for (int j = 0; j < AN_NF; j++) nm += o.mask[j];
            CHECK(fabsf(sum / fmaxf(nm, 1.0f) - o.s_ae) <= 1e-4f * fmaxf(1.0f, o.s_ae), "per-feature errors add up to the score");
            if (bits & BAT_ANOM_ALARM_MASK) { alarms++; CHECK(an_class_name(&o)[0] != 0, "a fault class"); }
        }
        rows++;
    }
    fclose(fp);
    CHECK(wins == 14, "windows %d", wins);
    printf("anomaly vectors: %d rows, %d windows, %d autoencoder alarm windows, worst rel diff %.3g\n", rows, wins, alarms, worst);
}

static void two_second_samples(void)
{
    an_params_t p = BAT_ANOM;
    static an_state_t s;
    an_out_t o;
    an_init(&p, &s, 2.0f);
    int done = 0, k;
    for (k = 1; k <= 120 && !done; k++) {
        an_step(&p, &s, 3.9f, -0.002f, 25.0f, 2.0f, 1, 0.001f, 3.9f, 0.0f, &o);
        done = o.win_done;
    }
    CHECK(done && k - 1 == 30, "a 60 s window is 30 samples at 2 s (got %d)", k - 1);
    CHECK(!o.a_ae && !o.a_rz && !o.a_g, "no alarm in the warm-up");
    CHECK(s.nwin == 1.0f, "window count %g", s.nwin);
    /* a steady benign day: 24 h of windows, then still no alarm */
    int alarm = 0;
    for (int w = 0; w < 1500 * 30; w++) {
        an_step(&p, &s, 3.9f, -0.002f, 25.0f, 2.0f, 1, 0.001f, 3.9f, 0.0f, &o);
        if (o.win_done && o.a_ae) alarm++;
    }
    CHECK(!alarm, "steady benign input raised %d alarms", alarm);
    CHECK(s.nwin > p.warmup_win, "warm-up over after 25 h (%g windows)", s.nwin);
}

int main(void)
{
    const char *v = getenv("ANOM_VECTORS");
    vectors(v && *v ? v : "anom_vectors.csv");
    two_second_samples();
    printf("bat_anom: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
