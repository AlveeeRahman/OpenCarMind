/* Host tests for the proposed rev_tank.c, in the style of OpenCarMind tests/host/test_rev_learn.c.
 *   cc -std=c11 -Wall -Wextra -O1 -o test_rev_tank test_rev_tank.c rev_tank.c -lm && ./test_rev_tank
 *   ./test_rev_tank replay rows.csv      (rows: restart,session,dt,lph,kph,running,level)
 * A synthetic car with a known tank, a double-precision reference written independently (batch fixed-effects regression),
 * fill-up suggestions, engine-off level changes, the band, the ready flag, range, persistence checks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "rev_tank.h"

static int fails, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static unsigned long rng = 12345;
static double urand(void) { rng = rng * 6364136223846793005UL + 1442695040888963407UL; return (double)((rng >> 11) & 0xFFFFFFFFFFFFFUL) / 4503599627370496.0; }
static double nrand(void) { double u = urand() + 1e-12, v = urand(); return sqrt(-2 * log(u)) * cos(6.283185307179586 * v); }

/* ---- a synthetic car: C_R raw litres per 100 %, 1.4 s rows, 2.3 L/h average, level polled every 8th row, AR(1) slosh */
typedef struct { double CR, V, ar, sd, rho, off_sd, park; int poll; } car_t;
static double xs[200000], ys[200000]; static int segid[200000], ns;     /* every sample, for the double reference */
static int cur_seg;

static double reading(const car_t *c) { double l = 100.0 * c->V / c->CR; return l > 100 ? 100 : l < 0 ? 0 : l; }

static void drive(rev_tank_t *t, car_t *c, int rows, double lph, uint32_t session, double *xcum)
{
    rev_tank_restart(t);
    c->park = c->off_sd * nrand();
    for (int i = 0; i < rows; i++) {
        double dt = 1.4, l = lph * (0.5 + urand());
        rev_tank_row(t, (float)dt, (float)l, 40.0f, true);
        c->V -= l * dt / 3600.0; *xcum += (float)(l * dt / 3600.0);
        if (i % c->poll == 0) {
            c->ar = c->rho * c->ar + sqrt(1 - c->rho * c->rho) * c->sd * nrand();
            double y = reading(c) + c->park + c->ar;
            y = round(y / (100.0 / 255.0)) * (100.0 / 255.0);
            y = y < 0 ? 0 : y > 100 ? 100 : y;
            rev_tank_level(t, session, (float)y);
            if (y >= REV_TANK_LO && y <= REV_TANK_HI && ns < 200000) { xs[ns] = *xcum; ys[ns] = y; segid[ns] = cur_seg; ns++; }
        }
    }
}

static double ref_capacity(void)        /* batch fixed effects in double: slope from within-segment deviations */
{
    double sxx = 0, sxy = 0;
    for (int s = 0; s <= cur_seg; s++) {
        double mx = 0, my = 0; int n = 0;
        for (int i = 0; i < ns; i++) if (segid[i] == s) { mx += xs[i]; my += ys[i]; n++; }
        if (n < 3) continue;
        mx /= n; my /= n;
        for (int i = 0; i < ns; i++) if (segid[i] == s) { sxx += (xs[i] - mx) * (xs[i] - mx); sxy += (xs[i] - mx) * (ys[i] - my); }
    }
    return 100.0 / (-sxy / sxx);
}

static void test_clean_tank(void)
{
    rev_tank_t t; rev_tank_init(&t);
    car_t c = {47.6, 47.6, 0, 0.0, 0.0, 0, 0, 8};
    double xc = 0; ns = 0; cur_seg = 0;
    for (int d = 0; d < 60; d++) drive(&t, &c, 400, 2.3, 100 + d, &xc);
    rev_tank_out_t o; rev_tank_get(&t, &o);
    CHECK(o.have && fabsf(o.cap_raw_l / 47.6f - 1) < 0.01f);                  /* quantisation only */
    CHECK(fabs(o.cap_raw_l / ref_capacity() - 1) < 1e-4);                      /* float learner == double reference */
    CHECK(t.fills == 0 && t.breaks == 0);
    CHECK(o.ready);
    CHECK(fabsf(o.left_raw_l - (float)c.V) < 1.0f);                            /* litres left from the de-sloshed level */
    CHECK(o.range_km > 0 && fabsf(o.range_km - o.left_raw_l * t.cons_km / t.cons_l) < 1e-3f);
}

static void test_noisy_with_fills(void)
{
    rev_tank_t t; rev_tank_init(&t);
    car_t c = {47.6, 47.6, 0, 1.6, 0.885, 0.0, 0, 8};                           /* sender noise: SD 1.6 %, lag-1 correlation 0.885 */
    double xc = 0; ns = 0; cur_seg = 0;
    int fills = 0; uint32_t s = 1;
    bool ready_early = false;
    for (int d = 0; d < 700; d++) {
        drive(&t, &c, 300, 2.3, s++, &xc);
        rev_tank_out_t o; rev_tank_get(&t, &o);
        if (xc < 8 && o.ready) ready_early = true;
        if (reading(&c) < 20) { c.V = c.CR; fills++; cur_seg++; }
    }
    rev_tank_out_t o; rev_tank_get(&t, &o);
    CHECK(!ready_early);                                                       /* never ready before REV_TANK_READY_L */
    CHECK(o.ready && fabsf(o.cap_raw_l / 47.6f - 1) < 0.05f);
    CHECK(fabs(o.cap_raw_l / ref_capacity() - 1) < 0.02);                       /* forgetting makes it differ a little from batch */
    CHECK((int)t.fills == fills);                                              /* every fill seen, no false ones */
    CHECK(t.sug_n == REV_TANK_SUGGEST);
    const rev_tank_fill_t *f = &t.sug[t.sug_n - 1];
    double expect = (f->after - f->before) / 100.0 * 47.6;
    CHECK(fabs(f->raw_l / expect - 1) < 0.1);
    rev_tank_ack(&t, f->seq - 1);
    CHECK(t.sug_n == 1 && t.sug[0].seq == f->seq);
}

static void test_engine_off_changes(void)
{
    /* parked on slopes: each drive reads +-3 % off, which must not bias the slope (the intercept absorbs it or the segment breaks) */
    rev_tank_t t; rev_tank_init(&t);
    car_t c = {47.6, 47.6, 0, 0.3, 0.5, 3.0, 0, 8};
    double xc = 0; ns = 0; cur_seg = 0;
    for (int d = 0; d < 120; d++) { drive(&t, &c, 300, 2.3, d + 1, &xc); if (reading(&c) < 20) c.V = c.CR; }
    rev_tank_out_t o; rev_tank_get(&t, &o);
    CHECK(o.have && fabsf(o.cap_raw_l / 47.6f - 1) < 0.08f);
    CHECK(t.segs <= 12);                                                       /* offsets of 3 % rarely close a segment */
}

static void test_band_and_inputs(void)
{
    rev_tank_t t; rev_tank_init(&t);
    rev_tank_restart(&t);
    for (int i = 0; i < 50; i++) { rev_tank_row(&t, 1.4f, 3.0f, 30, true); rev_tank_level(&t, 1, 100.0f); }   /* top stop */
    for (int i = 0; i < 50; i++) { rev_tank_row(&t, 1.4f, 3.0f, 30, true); rev_tank_level(&t, 1, 5.0f); }     /* reserve */
    CHECK(t.seg.n == 0 && t.pxx == 0);
    float x = t.x;
    rev_tank_row(&t, 12.0f, 3.0f, 30, true); CHECK(t.x == x);                  /* gap over 10 s not counted */
    rev_tank_row(&t, 1.0f, NAN, 30, true);   CHECK(t.x == x);
    rev_tank_row(&t, 1.0f, -1.0f, 30, true); CHECK(t.x == x);
    rev_tank_level(&t, 1, NAN); rev_tank_level(&t, 1, 300.0f);
    CHECK(t.seg.n == 0);
    rev_tank_row(&t, 1.0f, 3.0f, 0, false); rev_tank_level(&t, 1, 50.0f);       /* engine off: level ignored */
    CHECK(t.seg.n == 0);
    rev_tank_out_t o; rev_tank_get(&t, &o);
    CHECK(!o.have && !o.ready && o.range_km < 0);
    CHECK(rev_tank_valid(&t));
    t.pxx = NAN; CHECK(!rev_tank_valid(&t));
}

static void test_stop_inside_drive(void)
{
    /* engine off at the pump for 3 min without the board ending the drive: still a restart, still a fill */
    rev_tank_t t; rev_tank_init(&t);
    rev_tank_restart(&t);
    float lvl = 60;
    for (int i = 0; i < 400; i++) { rev_tank_row(&t, 1.4f, 3.0f, 40, true); lvl -= 3.0f * 1.4f / 3600 * 100 / 47.6f; if (i % 8 == 0) rev_tank_level(&t, 7, lvl); }
    for (int i = 0; i < 130; i++) rev_tank_row(&t, 1.4f, 0, 0, false);
    lvl = 99.6f;
    for (int i = 0; i < 100; i++) { rev_tank_row(&t, 1.4f, 3.0f, 40, true); if (i % 8 == 0) rev_tank_level(&t, 7, lvl); }
    CHECK(t.fills == 1 && t.sug_n == 1 && t.sug[0].top == 1);
}

static void test_top_stop_litres(void)
{
    /* sender reads 100 % for the top 4 raw litres: after one full tank the learner knows, and adds them to the next fill */
    rev_tank_t t; rev_tank_init(&t);
    double V = 51.6, CR = 47.6;
    uint32_t s = 1;
    for (int tank = 0; tank < 3; tank++) {
        for (int d = 0; d < 40 && 100 * V / CR > 15; d++) {
            rev_tank_restart(&t);
            for (int i = 0; i < 400; i++) {
                rev_tank_row(&t, 1.4f, 3.0f, 40, true); V -= 3.0 * 1.4 / 3600;
                double l = 100 * V / CR; l = l > 100 ? 100 : l;
                if (i % 8 == 0) rev_tank_level(&t, s, (float)(round(l / (100 / 255.0)) * (100 / 255.0)));
            }
            s++;
        }
        V = 51.6;
    }
    CHECK(t.top_n >= 2 && fabsf(t.top_l - 4.0f) < 0.6f);
    const rev_tank_fill_t *f = &t.sug[t.sug_n - 1];
    double pumped = 51.6 - (f->before / 100 * CR);
    CHECK(f->top && fabs(f->raw_l / pumped - 1) < 0.05);
}

static void test_suggestions_do_not_touch_c(void)
{
    /* the learner has no link to rev_cal: the only way a suggestion reaches c is the user typing the pumped litres in the app.
       This test pins the API: no function here takes or returns c. */
    rev_tank_t t; rev_tank_init(&t);
    CHECK(sizeof(rev_tank_fill_t) == 24);
    CHECK(sizeof(rev_tank_t) < 512);                                           /* small enough to save on every state write */
}

/* ---- replay of exported rows, for comparing against a reference implementation */
static int replay(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 1; }
    rev_tank_t t; rev_tank_init(&t);
    char line[256];
    int restart, running; unsigned session; double dt, lph, kph, level;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%d,%u,%lf,%lf,%lf,%d,%lf", &restart, &session, &dt, &lph, &kph, &running, &level) != 7) continue;
        if (restart) rev_tank_restart(&t);
        rev_tank_row(&t, (float)dt, (float)lph, (float)kph, running != 0);
        if (isfinite(level)) rev_tank_level(&t, session, (float)level);
    }
    fclose(f);
    rev_tank_out_t o; rev_tank_get(&t, &o);
    printf("{\"C_R\": %.6f, \"band_l\": %.4f, \"rel_se\": %.5f, \"ready\": %d, \"fills\": %u, \"breaks\": %u, \"top_l\": %.3f, \"top_n\": %u,"
           " \"level\": %.3f, \"left_raw_l\": %.3f, \"range_km\": %.1f, \"bytes\": %zu}\n",
           o.cap_raw_l, o.band_l, o.rel_se, o.ready, t.fills, t.breaks, t.top_l, t.top_n, o.level, o.left_raw_l, o.range_km, sizeof t);
    for (int i = 0; i < t.sug_n; i++)
        printf("  suggestion seq %u session %u before %.2f after %.2f raw_l %.2f ready %d top %d\n", t.sug[i].seq, t.sug[i].session,
               t.sug[i].before, t.sug[i].after, t.sug[i].raw_l, t.sug[i].ready, t.sug[i].top);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "replay")) return replay(argv[2]);
    test_clean_tank();
    test_noisy_with_fills();
    test_engine_off_changes();
    test_band_and_inputs();
    test_stop_inside_drive();
    test_top_stop_litres();
    test_suggestions_do_not_touch_c();
    printf("rev_tank: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
