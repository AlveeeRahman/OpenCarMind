/* ui_settings_logic.c: the ESP-Mosaico Settings blob migration (version 5 -> 6: the Wi-Fi app, no app hiding; 6 -> 7:
   the E.V app after R.E.V), Home order moves, what each change refreshes, and the debounce behind the deferred NVS write
   and launcher rebuild. */
#include <stdio.h>
#include <string.h>
#include "ui_settings_logic.h"

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

/* registry shape of both firmware lines after S1 and of the test line after M1 (no E.V app): 15 apps, Wi-Fi at 13,
   Settings last at 14 (version 5 had 14 apps, Settings at 13) */
#define NAPPS 15
#define WIFI  13
#define SETT  14
/* the public registry after M1: the E.V app right after R.E.V, 16 apps (version 6 had 15, version 5 had 14) */
#define NAPPS7 16
#define REV7   3
#define EV7    4
#define WIFI7  14
#define SETT7  15

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

static ui_settings_t v6(const int *order, int n)
{
    ui_settings_t s = v5(order, n, 0);
    s.ver = 6;
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
    CHECK(ocm_settings_migrate(&in, &out, NAPPS, WIFI, -1, -1) == OCM_SET_CHANGED);
    int want_def[15] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, WIFI, SETT};
    CHECK(order_is(&out, want_def, 15));
    CHECK(out.ver == UI_SETTINGS_VER && out.hidden_v5 == 0);
    CHECK(out.widget == 2 && out.brightness == 40 && out.screen_off_s == 20 && out.econ_l100 == 1 && out.theme == 2);

    /* a user order with hidden apps: every app shows again, the order stays, Wi-Fi goes in just before Settings */
    int user5[14] = {13, 5, 0, 1, 12, 2, 3, 4, 6, 7, 8, 9, 10, 11};      /* Settings (13) moved to the front */
    in = v5(user5, 14, (1u << 5) | (1u << 9) | (1u << 12));
    CHECK(ocm_settings_migrate(&in, &out, NAPPS, WIFI, -1, -1) == OCM_SET_CHANGED);
    int want_user[15] = {WIFI, SETT, 5, 0, 1, 12, 2, 3, 4, 6, 7, 8, 9, 10, 11};
    CHECK(order_is(&out, want_user, 15));
    CHECK(out.hidden_v5 == 0);

    /* the migration runs once: the saved result loads as is */
    ui_settings_t again;
    CHECK(ocm_settings_migrate(&out, &again, NAPPS, WIFI, -1, -1) == OCM_SET_OK);
    CHECK(memcmp(&again, &out, sizeof out) == 0);

    /* Settings last in a user order */
    int last5[14] = {1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    in = v5(last5, 14, 0);
    ocm_settings_migrate(&in, &out, NAPPS, WIFI, -1, -1);
    int want_last[15] = {1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, WIFI, SETT};
    CHECK(order_is(&out, want_last, 15));

    /* a short version-5 order (an older firmware had fewer apps): the rest is appended, Wi-Fi before Settings */
    int short5[3] = {2, 0, 1};
    in = v5(short5, 3, 1u << 2);
    CHECK(ocm_settings_migrate(&in, &out, NAPPS, WIFI, -1, -1) == OCM_SET_CHANGED);
    int want_short[15] = {2, 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, WIFI, SETT};
    CHECK(order_is(&out, want_short, 15));

    /* repeated and out-of-range indices are dropped */
    int dup5[6] = {13, 13, 40, 0, 0, 1};
    in = v5(dup5, 6, 0);
    ocm_settings_migrate(&in, &out, NAPPS, WIFI, -1, -1);
    int want_dup[15] = {WIFI, SETT, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    CHECK(order_is(&out, want_dup, 15));

    /* a version-6 blob with a stray hidden mask (never written by 6, but cleared if found) */
    ui_settings_t s6 = out;
    s6.hidden_v5 = 1u << 3;
    CHECK(ocm_settings_migrate(&s6, &again, NAPPS, WIFI, -1, -1) == OCM_SET_CHANGED && again.hidden_v5 == 0);
    CHECK(order_is(&again, want_dup, 15));

    /* unknown versions and broken blobs: defaults */
    in = v5(def5, 14, 0); in.ver = 4;
    CHECK(ocm_settings_migrate(&in, &out, NAPPS, WIFI, -1, -1) == OCM_SET_REJECT);
    in.ver = 9;
    CHECK(ocm_settings_migrate(&in, &out, NAPPS, WIFI, -1, -1) == OCM_SET_REJECT);
    in = v5(def5, 14, 0); in.n = UI_MAX_APPS + 1;
    CHECK(ocm_settings_migrate(&in, &out, NAPPS, WIFI, -1, -1) == OCM_SET_REJECT);

    /* a firmware without a Wi-Fi app (wifi = -1): indices unchanged */
    in = v5(def5, 14, 1u << 4);
    CHECK(ocm_settings_migrate(&in, &out, 14, -1, -1, -1) == OCM_SET_CHANGED);
    CHECK(order_is(&out, def5, 14) && out.hidden_v5 == 0);

    /* every result holds every app exactly once */
    {
        int bad = 0;
        for (int trial = 0; trial < 200; trial++) {
            int o[14], n = 1 + trial % 14;
            unsigned x = 2463534242u + (unsigned)trial * 2654435761u;
            for (int i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; o[i] = (int)(x % 16); }
            in = v5(o, n, (uint16_t)x);
            if (ocm_settings_migrate(&in, &out, NAPPS, WIFI, -1, -1) == OCM_SET_REJECT) { bad++; continue; }
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


    /* ---- migration 6 -> 7 (M1): the E.V app goes in right after R.E.V, the rest of the order is kept ---- */
    {
        int def6[15] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
        ui_settings_t i6 = v6(def6, 15), o7, o7b;
        CHECK(ocm_settings_migrate(&i6, &o7, NAPPS7, WIFI7, EV7, -1) == OCM_SET_CHANGED);
        int want7[16] = {0, 1, 2, REV7, EV7, 5, 6, 7, 8, 9, 10, 11, 12, 13, WIFI7, SETT7};
        CHECK(order_is(&o7, want7, 16));
        CHECK(o7.ver == UI_SETTINGS_VER && UI_SETTINGS_VER == 8);
        CHECK(o7.widget == 2 && o7.brightness == 40 && o7.screen_off_s == 20 && o7.econ_l100 == 1 && o7.theme == 2);
        CHECK(ocm_settings_migrate(&o7, &o7b, NAPPS7, WIFI7, EV7, -1) == OCM_SET_OK);    /* once: then it loads as is */
        CHECK(memcmp(&o7, &o7b, sizeof o7) == 0);

        /* R.E.V moved to the front: E.V lands second, every other app keeps its place relative to the rest */
        int front6[15] = {3, 0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
        i6 = v6(front6, 15);
        CHECK(ocm_settings_migrate(&i6, &o7, NAPPS7, WIFI7, EV7, -1) == OCM_SET_CHANGED);
        int want_front[16] = {REV7, EV7, 0, 1, 2, 5, 6, 7, 8, 9, 10, 11, 12, 13, WIFI7, SETT7};
        CHECK(order_is(&o7, want_front, 16));

        /* R.E.V last (after Settings) */
        int last6[15] = {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 3};
        i6 = v6(last6, 15);
        ocm_settings_migrate(&i6, &o7, NAPPS7, WIFI7, EV7, -1);
        int want_rlast[16] = {0, 1, 2, 5, 6, 7, 8, 9, 10, 11, 12, 13, WIFI7, SETT7, REV7, EV7};
        CHECK(order_is(&o7, want_rlast, 16));

        /* a mixed user order: Settings first, Wi-Fi in the middle, R.E.V after Files */
        int mix6[15] = {14, 0, 13, 12, 3, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11};
        i6 = v6(mix6, 15);
        ocm_settings_migrate(&i6, &o7, NAPPS7, WIFI7, EV7, -1);
        int want_mix[16] = {SETT7, 0, WIFI7, 13, REV7, EV7, 1, 2, 5, 6, 7, 8, 9, 10, 11, 12};
        CHECK(order_is(&o7, want_mix, 16));

        /* a short order without R.E.V: E.V is appended with the other missing apps, in registry order (after R.E.V) */
        int short6[3] = {2, 0, 1};
        i6 = v6(short6, 3);
        ocm_settings_migrate(&i6, &o7, NAPPS7, WIFI7, EV7, -1);
        int want_s6[16] = {2, 0, 1, REV7, EV7, 5, 6, 7, 8, 9, 10, 11, 12, 13, WIFI7, SETT7};
        CHECK(order_is(&o7, want_s6, 16));

        /* a version-5 blob on the M1 registry takes both steps: Wi-Fi before Settings, E.V after R.E.V */
        int def5b[14] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
        ui_settings_t i5 = v5(def5b, 14, 1u << 3);
        CHECK(ocm_settings_migrate(&i5, &o7, NAPPS7, WIFI7, EV7, -1) == OCM_SET_CHANGED);
        int want_d57[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        CHECK(order_is(&o7, want_d57, 16) && o7.hidden_v5 == 0 && o7.ver == UI_SETTINGS_VER);
        int user5b[14] = {13, 5, 0, 1, 12, 2, 3, 4, 6, 7, 8, 9, 10, 11};
        i5 = v5(user5b, 14, (1u << 5) | (1u << 9));
        ocm_settings_migrate(&i5, &o7, NAPPS7, WIFI7, EV7, -1);
        int want_u57[16] = {WIFI7, SETT7, 6, 0, 1, 13, 2, REV7, EV7, 5, 7, 8, 9, 10, 11, 12};
        CHECK(order_is(&o7, want_u57, 16));

        /* a version-6 blob on a firmware without the E.V app (the test line): the order stays, the version moves */
        i6 = v6(mix6, 15);
        CHECK(ocm_settings_migrate(&i6, &o7, NAPPS, WIFI, -1, -1) == OCM_SET_CHANGED);
        CHECK(order_is(&o7, mix6, 15) && o7.ver == UI_SETTINGS_VER);

        /* registry indices that cannot be right: defaults */
        CHECK(ocm_settings_migrate(&i6, &o7, NAPPS7, WIFI7, WIFI7, -1) == OCM_SET_REJECT);
        CHECK(ocm_settings_migrate(&i6, &o7, NAPPS7, WIFI7, NAPPS7, -1) == OCM_SET_REJECT);
        CHECK(ocm_settings_migrate(&i6, &o7, NAPPS7, NAPPS7, EV7, -1) == OCM_SET_REJECT);

        /* random version-5 and version-6 blobs: every app exactly once, E.V right after R.E.V when R.E.V was saved,
           Wi-Fi right before Settings when a version-5 blob held Settings, and a second load changes nothing */
        int bad = 0, cases = 0;
        for (int trial = 0; trial < 400; trial++) {
            int ver = 5 + trial % 2, nreg = ver == 5 ? 14 : 15, o[15], n = 1 + trial % nreg;
            unsigned x = 2463534242u + (unsigned)trial * 2654435761u;
            for (int i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; o[i] = (int)(x % 18); }
            ui_settings_t in7 = ver == 5 ? v5(o, n, (uint16_t)x) : v6(o, n), r7, r7b;
            if (ocm_settings_migrate(&in7, &r7, NAPPS7, WIFI7, EV7, -1) == OCM_SET_REJECT) { bad++; continue; }
            cases++;
            uint32_t seen = 0;
            int rpos = -1, epos = -1, wpos = -1, spos = -1;
            for (int i = 0; i < r7.n; i++) {
                seen |= 1u << r7.order[i];
                if (r7.order[i] == REV7) rpos = i;
                if (r7.order[i] == EV7) epos = i;
                if (r7.order[i] == WIFI7) wpos = i;
                if (r7.order[i] == SETT7) spos = i;
            }
            if (r7.n != NAPPS7 || seen != (1u << NAPPS7) - 1 || r7.hidden_v5 || r7.ver != UI_SETTINGS_VER) bad++;
            if (epos != rpos + 1) bad++;          /* R.E.V saved: E.V follows it; R.E.V missing: appended in order */
            bool had_sett = false;
            for (int i = 0; i < n; i++) if (o[i] == nreg - 1) had_sett = true;
            if (ver == 5 && had_sett && wpos != spos - 1) bad++;
            if (ocm_settings_migrate(&r7, &r7b, NAPPS7, WIFI7, EV7, -1) != OCM_SET_OK || memcmp(&r7, &r7b, sizeof r7)) bad++;
        }
        CHECK(bad == 0 && cases == 400);

        /* order moves on the 16-app order: E.V can move like any app */
        i6 = v6(def6, 15);
        ocm_settings_migrate(&i6, &o7, NAPPS7, WIFI7, EV7, -1);
        CHECK(ocm_order_move(&o7, 4, -1) && o7.order[3] == EV7 && o7.order[4] == REV7);
        CHECK(!ocm_order_move(&o7, 15, +1) && ocm_order_move(&o7, 14, +1) && o7.order[15] == WIFI7);
    }

    /* ---- migration 7 -> 8 (EdgeOS 2.1.0): the Health app goes in right after Board, the rest of the order is kept ---- */
    {
#define NAPPS8  17
#define EV8     4
#define BOARD8  12
#define HEALTH8 13
#define WIFI8   15
#define SETT8   16
        int def7[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        ui_settings_v7_t b7;
        memset(&b7, 0, sizeof b7);
        b7.ver = 7; b7.n = 16;
        for (int i = 0; i < 16; i++) b7.order[i] = (uint8_t)def7[i];
        b7.hidden_v5 = 0; b7.widget = 2; b7.brightness = 40; b7.screen_off_s = 20; b7.econ_l100 = 1; b7.theme = 2;
        CHECK(sizeof(ui_settings_v7_t) == 26);              /* the NVS blob of versions 5-7 */
        ui_settings_t i7, o8, o8b;
        ocm_settings_from_v7(&b7, &i7);
        CHECK(i7.ver == 7 && i7.n == 16 && i7.brightness == 40 && i7.screen_off_s == 20 && i7.theme == 2 && i7.order[15] == 15);
        CHECK(ocm_settings_migrate(&i7, &o8, NAPPS8, WIFI8, EV8, HEALTH8) == OCM_SET_CHANGED);
        int want8[17] = {0, 1, 2, 3, EV8, 5, 6, 7, 8, 9, 10, 11, BOARD8, HEALTH8, 14, WIFI8, SETT8};
        CHECK(order_is(&o8, want8, 17) && o8.ver == 8);
        CHECK(o8.widget == 2 && o8.brightness == 40 && o8.screen_off_s == 20 && o8.econ_l100 == 1 && o8.theme == 2);
        CHECK(ocm_settings_migrate(&o8, &o8b, NAPPS8, WIFI8, EV8, HEALTH8) == OCM_SET_OK && memcmp(&o8, &o8b, sizeof o8) == 0);

        /* a user order (Settings first, Board after Live, Wi-Fi second): Health follows Board, the indices above it move */
        int mix7[16] = {15, 14, 0, 6, 12, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 13};
        ui_settings_t m7 = i7;
        for (int i = 0; i < 16; i++) m7.order[i] = (uint8_t)mix7[i];
        ocm_settings_migrate(&m7, &o8, NAPPS8, WIFI8, EV8, HEALTH8);
        int want_mix8[17] = {SETT8, WIFI8, 0, 6, BOARD8, HEALTH8, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 14};
        CHECK(order_is(&o8, want_mix8, 17));

        /* a version-6 blob on the 2.1.0 registry: E.V after R.E.V, then Health after Board */
        int def6[15] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
        ui_settings_t i6 = v6(def6, 15);
        CHECK(ocm_settings_migrate(&i6, &o8, NAPPS8, WIFI8, EV8, HEALTH8) == OCM_SET_CHANGED);
        CHECK(order_is(&o8, want8, 17));
        /* a version-5 blob: all three steps */
        int def5c[14] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
        ui_settings_t i5 = v5(def5c, 14, 1u << 3);
        CHECK(ocm_settings_migrate(&i5, &o8, NAPPS8, WIFI8, EV8, HEALTH8) == OCM_SET_CHANGED);
        CHECK(order_is(&o8, want8, 17) && o8.hidden_v5 == 0);

        /* bad indices, and a version-7 blob that claims more than 16 entries */
        CHECK(ocm_settings_migrate(&i7, &o8, NAPPS8, WIFI8, EV8, WIFI8) == OCM_SET_REJECT);
        CHECK(ocm_settings_migrate(&i7, &o8, NAPPS8, WIFI8, EV8, NAPPS8) == OCM_SET_REJECT);
        ui_settings_t big = i7;
        big.n = 17;
        CHECK(ocm_settings_migrate(&big, &o8, NAPPS8, WIFI8, EV8, HEALTH8) == OCM_SET_REJECT);

        /* random version-5, 6 and 7 blobs: every app exactly once, Health right after Board when Board was saved, and a
           second load changes nothing */
        int bad = 0;
        for (int trial = 0; trial < 600; trial++) {
            int ver = 5 + trial % 3, nreg = ver == 5 ? 14 : ver == 6 ? 15 : 16, o[16], n = 1 + trial % nreg;
            unsigned x = 88172645u + (unsigned)trial * 2654435761u;
            for (int i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; o[i] = (int)(x % 19); }
            ui_settings_t iv = ver == 5 ? v5(o, n, (uint16_t)x) : v6(o, n), r, rb;
            iv.ver = (uint8_t)ver;
            if (ocm_settings_migrate(&iv, &r, NAPPS8, WIFI8, EV8, HEALTH8) == OCM_SET_REJECT) { bad++; continue; }
            uint32_t seen = 0;
            int bpos = -1, hpos = -1;
            for (int i = 0; i < r.n; i++) {
                seen |= 1u << r.order[i];
                if (r.order[i] == BOARD8) bpos = i;
                if (r.order[i] == HEALTH8) hpos = i;
            }
            if (r.n != NAPPS8 || seen != (1u << NAPPS8) - 1 || r.ver != 8) bad++;
            if (hpos != bpos + 1) bad++;
            if (ocm_settings_migrate(&r, &rb, NAPPS8, WIFI8, EV8, HEALTH8) != OCM_SET_OK || memcmp(&r, &rb, sizeof r)) bad++;
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
