/* ui_settings_logic.c: the ESP-Mosaico Settings blob migration (version 5 -> 6: the Wi-Fi app, no app hiding), Home
   order moves, what each change refreshes, and the debounce behind the deferred NVS write and launcher rebuild. */
#include <stdio.h>
#include <string.h>
#include "ui_settings_logic.h"

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

/* registry shape of both firmware lines after S1: 15 apps, Wi-Fi at 13, Settings last at 14 (version 5 had 14 apps,
   Settings at 13) */
#define NAPPS 15
#define WIFI  13
#define SETT  14

static ui_settings_t v5(const int *order, int n, uint16_t hidden)
{
    ui_settings_t s;
    memset(&s, 0, sizeof s);
    s.ver = 5;
    s.n = (uint8_t)n;
    for (int i = 0; i < n; i++) s.order[i] = (uint8_t)order[i];
    s.hidden_v5 = hidden;
    s.widget = 2; s.brightness = 40; s.screen_off_s = 20; s.econ_l100 = 1; s.theme = 2;
    return s;
}

static bool order_is(const ui_settings_t *s, const int *want, int n)
{
    if (s->n != n) return false;
    for (int i = 0; i < n; i++) if (s->order[i] != want[i]) return false;
    return true;
}

/* the board task's save loop, reduced to its timing: requests at the given times (ms), the task wakes when the
   debounce says so (or every 2 s), writes when it fires. Returns the number of writes, the write times in w[]. */
static int simulate_saves(const int64_t *req, int nreq, int64_t until, int64_t *w)
{
    ocm_debounce_t d = {0};
    int nw = 0, k = 0;
    int64_t now = 0;
    while (now <= until) {
        while (k < nreq && req[k] <= now) ocm_debounce_touch(&d, req[k++], OCM_SAVE_QUIET_MS);
        if (ocm_debounce_fire(&d, now, OCM_SAVE_MAX_MS)) w[nw++] = now;
        int32_t wait = ocm_debounce_wait_ms(&d, now, OCM_SAVE_MAX_MS, 2000);
        int64_t next = now + (wait > 0 ? wait : 1);
        if (k < nreq && req[k] < next) next = req[k];      /* a request wakes the task (the queue kick) */
        now = next;
    }
    return nw;
}

int main(void)
{
    /* ---- migration 5 -> 6 ---- */
    int def5[14] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    ui_settings_t in = v5(def5, 14, 0), out;
    CHECK(ocm_settings_migrate(&in, &out, NAPPS, WIFI) == OCM_SET_CHANGED);
    int want_def[15] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, WIFI, SETT};
    CHECK(order_is(&out, want_def, 15));
    CHECK(out.ver == UI_SETTINGS_VER && out.hidden_v5 == 0);
    CHECK(out.widget == 2 && out.brightness == 40 && out.screen_off_s == 20 && out.econ_l100 == 1 && out.theme == 2);

    /* a user order with hidden apps: every app shows again, the order stays, Wi-Fi goes in just before Settings */
    int user5[14] = {13, 5, 0, 1, 12, 2, 3, 4, 6, 7, 8, 9, 10, 11};      /* Settings (13) moved to the front */
    in = v5(user5, 14, (1u << 5) | (1u << 9) | (1u << 12));
    CHECK(ocm_settings_migrate(&in, &out, NAPPS, WIFI) == OCM_SET_CHANGED);
    int want_user[15] = {WIFI, SETT, 5, 0, 1, 12, 2, 3, 4, 6, 7, 8, 9, 10, 11};
    CHECK(order_is(&out, want_user, 15));
    CHECK(out.hidden_v5 == 0);

    /* the migration runs once: the saved result loads as is */
    ui_settings_t again;
    CHECK(ocm_settings_migrate(&out, &again, NAPPS, WIFI) == OCM_SET_OK);
    CHECK(memcmp(&again, &out, sizeof out) == 0);

    /* Settings last in a user order */
    int last5[14] = {1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    in = v5(last5, 14, 0);
    ocm_settings_migrate(&in, &out, NAPPS, WIFI);
    int want_last[15] = {1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, WIFI, SETT};
    CHECK(order_is(&out, want_last, 15));

    /* a short version-5 order (an older firmware had fewer apps): the rest is appended, Wi-Fi before Settings */
    int short5[3] = {2, 0, 1};
    in = v5(short5, 3, 1u << 2);
    CHECK(ocm_settings_migrate(&in, &out, NAPPS, WIFI) == OCM_SET_CHANGED);
    int want_short[15] = {2, 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, WIFI, SETT};
    CHECK(order_is(&out, want_short, 15));

    /* repeated and out-of-range indices are dropped */
    int dup5[6] = {13, 13, 40, 0, 0, 1};
    in = v5(dup5, 6, 0);
    ocm_settings_migrate(&in, &out, NAPPS, WIFI);
    int want_dup[15] = {WIFI, SETT, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    CHECK(order_is(&out, want_dup, 15));

    /* a version-6 blob with a stray hidden mask (never written by 6, but cleared if found) */
    ui_settings_t s6 = out;
    s6.hidden_v5 = 1u << 3;
    CHECK(ocm_settings_migrate(&s6, &again, NAPPS, WIFI) == OCM_SET_CHANGED && again.hidden_v5 == 0);
    CHECK(order_is(&again, want_dup, 15));

    /* unknown versions and broken blobs: defaults */
    in = v5(def5, 14, 0); in.ver = 4;
    CHECK(ocm_settings_migrate(&in, &out, NAPPS, WIFI) == OCM_SET_REJECT);
    in.ver = 7;
    CHECK(ocm_settings_migrate(&in, &out, NAPPS, WIFI) == OCM_SET_REJECT);
    in = v5(def5, 14, 0); in.n = UI_MAX_APPS + 1;
    CHECK(ocm_settings_migrate(&in, &out, NAPPS, WIFI) == OCM_SET_REJECT);

    /* a firmware without a Wi-Fi app (wifi = -1): indices unchanged */
    in = v5(def5, 14, 1u << 4);
    CHECK(ocm_settings_migrate(&in, &out, 14, -1) == OCM_SET_CHANGED);
    CHECK(order_is(&out, def5, 14) && out.hidden_v5 == 0);

    /* every result holds every app exactly once */
    {
        int bad = 0;
        for (int trial = 0; trial < 200; trial++) {
            int o[14], n = 1 + trial % 14;
            unsigned x = 2463534242u + (unsigned)trial * 2654435761u;
            for (int i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; o[i] = (int)(x % 16); }
            in = v5(o, n, (uint16_t)x);
            if (ocm_settings_migrate(&in, &out, NAPPS, WIFI) == OCM_SET_REJECT) { bad++; continue; }
            uint32_t seen = 0;
            for (int i = 0; i < out.n; i++) seen |= 1u << out.order[i];
            int wpos = -1, spos = -1;
            for (int i = 0; i < out.n; i++) { if (out.order[i] == WIFI) wpos = i; if (out.order[i] == SETT) spos = i; }
            if (out.n != NAPPS || seen != (1u << NAPPS) - 1 || out.hidden_v5) bad++;
            bool had_sett = false;
            for (int i = 0; i < n; i++) if (o[i] == 13) had_sett = true;
            if (had_sett && wpos != spos - 1) bad++;              /* Wi-Fi lands next to Settings */
        }
        CHECK(bad == 0);
    }

    /* ---- order moves ---- */
    ui_settings_t m = again;                                  /* {WIFI, SETT, 0, 1, ...} */
    CHECK(ocm_order_move(&m, 0, +1) && m.order[0] == SETT && m.order[1] == WIFI);
    CHECK(ocm_order_move(&m, 1, -1) && m.order[0] == WIFI && m.order[1] == SETT);
    CHECK(!ocm_order_move(&m, 0, -1));                        /* top row: no Up */
    CHECK(!ocm_order_move(&m, m.n - 1, +1));                  /* bottom row: no Down */
    CHECK(!ocm_order_move(&m, 3, 2) && !ocm_order_move(&m, -1, 1) && !ocm_order_move(&m, m.n, -1));
    ui_settings_t walk = m;
    for (int p = 0; p < walk.n - 1; p++) CHECK(ocm_order_move(&walk, p, +1));   /* Down to the bottom */
    CHECK(walk.order[walk.n - 1] == WIFI);
    for (int p = walk.n - 1; p > 0; p--) CHECK(ocm_order_move(&walk, p, -1));  /* and back up */
    CHECK(memcmp(&walk, &m, sizeof m) == 0);

    /* ---- what each change refreshes ---- */
    ui_settings_t a = again, b = again;
    CHECK(ocm_settings_diff(&a, &b) == 0 && ocm_settings_actions(0) == 0);
    b.theme = (uint8_t)((a.theme + 1) % 3);
    CHECK(ocm_settings_diff(&a, &b) == OCM_CHG_THEME);
    CHECK(ocm_settings_actions(OCM_CHG_THEME) == (OCM_DO_SAVE | OCM_DO_THEME));        /* no launcher rebuild per tap */
    b = a; b.econ_l100 ^= 1;
    CHECK(ocm_settings_diff(&a, &b) == OCM_CHG_UNITS && ocm_settings_actions(OCM_CHG_UNITS) == OCM_DO_SAVE);
    b = a; b.screen_off_s = 60;
    CHECK(ocm_settings_diff(&a, &b) == OCM_CHG_SCREEN_OFF && ocm_settings_actions(OCM_CHG_SCREEN_OFF) == OCM_DO_SAVE);
    b = a; b.brightness = 90;
    CHECK(ocm_settings_actions(ocm_settings_diff(&a, &b)) == (OCM_DO_SAVE | OCM_DO_PANEL));
    b = a; ocm_order_move(&b, 4, +1);
    CHECK(ocm_settings_diff(&a, &b) == OCM_CHG_ORDER);
    CHECK(ocm_settings_actions(OCM_CHG_ORDER) == (OCM_DO_SAVE | OCM_DO_LAUNCHER));
    b = a; b.order[a.n] = 9;                                  /* bytes past n are not part of the order */
    CHECK(ocm_settings_diff(&a, &b) == 0);

    /* ---- debounce ---- */
    ocm_debounce_t d = {0};
    CHECK(!ocm_debounce_fire(&d, 0, OCM_SAVE_MAX_MS) && ocm_debounce_wait_ms(&d, 0, OCM_SAVE_MAX_MS, 2000) == 2000);
    ocm_debounce_touch(&d, 1000, 500);
    CHECK(ocm_debounce_wait_ms(&d, 1000, OCM_SAVE_MAX_MS, 2000) == 500);
    CHECK(ocm_debounce_wait_ms(&d, 1000, OCM_SAVE_MAX_MS, 100) == 100);
    CHECK(!ocm_debounce_fire(&d, 1499, OCM_SAVE_MAX_MS));
    ocm_debounce_touch(&d, 1400, 500);                        /* a new tap moves the deadline */
    CHECK(!ocm_debounce_fire(&d, 1600, OCM_SAVE_MAX_MS) && d.first_ms == 1000);
    CHECK(ocm_debounce_fire(&d, 1900, OCM_SAVE_MAX_MS));
    CHECK(!ocm_debounce_fire(&d, 1901, OCM_SAVE_MAX_MS));     /* once */
    ocm_debounce_touch(&d, 5000, 500);
    ocm_debounce_cancel(&d);
    CHECK(!ocm_debounce_fire(&d, 9000, OCM_SAVE_MAX_MS));

    /* the board task: a burst of 8 taps 120 ms apart -> one write, 500 ms after the last tap */
    int64_t req[64], w[64];
    for (int i = 0; i < 8; i++) req[i] = 1000 + i * 120;
    int nw = simulate_saves(req, 8, 6000, w);
    CHECK(nw == 1 && w[0] == 1000 + 7 * 120 + OCM_SAVE_QUIET_MS);
    /* the old board loop wrote on every 2 s tick that found the flag set: the same burst could cost two writes */
    /* taps that never pause (every 400 ms for 10 s): a write at least every OCM_SAVE_MAX_MS, then one at the end */
    int n = 0;
    for (int64_t t = 0; t <= 10000; t += 400) req[n++] = t;
    nw = simulate_saves(req, n, 14000, w);
    CHECK(nw >= 3 && nw <= 5);
    CHECK(w[0] <= OCM_SAVE_MAX_MS);
    {
        int gap_bad = 0;
        for (int i = 1; i < nw; i++) if (w[i] - w[i - 1] > OCM_SAVE_MAX_MS + 400) gap_bad++;
        CHECK(gap_bad == 0);
    }
    CHECK(w[nw - 1] == 10000 + OCM_SAVE_QUIET_MS);           /* the last change is always written */
    /* two separate taps 3 s apart: two writes */
    req[0] = 100; req[1] = 3100;
    nw = simulate_saves(req, 2, 6000, w);
    CHECK(nw == 2 && w[0] == 600 && w[1] == 3600);
    /* no request: no write */
    CHECK(simulate_saves(req, 0, 6000, w) == 0);

    printf("ui_settings: %d checks, %d failures\n", runs, fails);
    return fails ? 1 : 0;
}
