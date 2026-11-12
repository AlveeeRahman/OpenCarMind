/* Host tests for the adapter choice and pairing rules (main/obd_pair.c; owner 2026-11-11, "ask + strong signal").
   Build and run: make -C tests/host pair (also runs pair_audit.sh, the source rules a unit test cannot see). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "obd_pair.h"

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define LINK_MIN (-90)      /* CONFIG_OBD_BLE_MIN_RSSI default */
#define PAIR_MIN (-70)      /* CONFIG_OBD_BLE_PAIR_MIN_RSSI default */
#define ASK_MS   30000u     /* the ESP-Mosaico sheet; the car board uses 5000 */

static const uint8_t K_NEIGHBOUR[OBD_PAIR_KEY_LEN] = {0, 1, 1, 1, 1, 1, 1};
static const uint8_t K_MINE[OBD_PAIR_KEY_LEN]      = {0, 2, 2, 2, 2, 2, 2};
static const uint8_t K_SAVED[OBD_PAIR_KEY_LEN]     = {1, 3, 3, 3, 3, 3, 3};

static obd_adv_t adv(const uint8_t *k, int rssi, bool have_saved, bool is_saved, bool user)
{
    return (obd_adv_t){.key = k, .rssi = rssi, .have_saved = have_saved, .is_saved = is_saved, .user_search = user,
                       .looks_obd = true, .failed = false};
}

/* 1. candidate selection by signal, with and without a saved adapter */
static void test_pick_rssi(void)
{
    obd_pair_t p;
    obd_pair_init(&p);
    /* no adapter saved: the bench case, a neighbour's "OBDBLE" at -90 dBm, must not be offered */
    for (int r = -100; r <= -20; r++) {
        obd_adv_t a = adv(K_NEIGHBOUR, r, false, false, false);
        obd_pick_t k = obd_pair_pick(&p, &a, LINK_MIN, PAIR_MIN);
        CHECK(k == (r >= PAIR_MIN ? OBD_PICK_OFFER : OBD_PICK_NONE), "not paired, %d dBm: %s", r, k == OBD_PICK_OFFER ? "offered" : "not offered");
        a.user_search = true;                                    /* Find OBD does not lower the pairing floor */
        CHECK(obd_pair_pick(&p, &a, LINK_MIN, PAIR_MIN) == k, "not paired, Find OBD, %d dBm: same as without", r);
    }
    obd_adv_t n90 = adv(K_NEIGHBOUR, -90, false, false, true);
    CHECK(obd_pair_pick(&p, &n90, LINK_MIN, PAIR_MIN) == OBD_PICK_NONE, "the bench neighbour at -90 dBm is never offered");
    obd_adv_t n71 = adv(K_NEIGHBOUR, -71, false, false, false), n70 = adv(K_NEIGHBOUR, -70, false, false, false);
    CHECK(obd_pair_pick(&p, &n71, LINK_MIN, PAIR_MIN) == OBD_PICK_NONE && obd_pair_pick(&p, &n70, LINK_MIN, PAIR_MIN) == OBD_PICK_OFFER,
          "the floor is inclusive: -70 offered, -71 not");
    /* a saved adapter: connects down to the old -90 floor, needs no question */
    for (int r = -100; r <= -20; r++) {
        obd_adv_t s = adv(K_SAVED, r, true, true, false);
        CHECK(obd_pair_pick(&p, &s, LINK_MIN, PAIR_MIN) == (r >= LINK_MIN ? OBD_PICK_SAVED : OBD_PICK_NONE), "paired, %d dBm", r);
        s.user_search = true;
        CHECK(obd_pair_pick(&p, &s, LINK_MIN, PAIR_MIN) == (r >= LINK_MIN ? OBD_PICK_SAVED : OBD_PICK_NONE), "paired, Find OBD, %d dBm", r);
    }
    /* not an adapter, or on the failure skip list */
    obd_adv_t x = adv(K_NEIGHBOUR, -40, false, false, false);
    x.looks_obd = false;
    CHECK(obd_pair_pick(&p, &x, LINK_MIN, PAIR_MIN) == OBD_PICK_NONE, "a device that does not look like an adapter");
    x.looks_obd = true; x.failed = true;
    CHECK(obd_pair_pick(&p, &x, LINK_MIN, PAIR_MIN) == OBD_PICK_NONE, "a device that failed the probe");
    printf("  pick by signal: not paired offers from %d dBm, paired connects from %d dBm\n", PAIR_MIN, LINK_MIN);
}

/* 2. the automatic search (and the plain scan) never replace or bypass a saved adapter; only Find OBD looks further */
static void test_saved_lock(void)
{
    obd_pair_t p;
    obd_pair_init(&p);
    int offered_auto = 0, offered_user = 0;
    for (int r = -100; r <= 0; r++) {
        obd_adv_t a = adv(K_MINE, r, true, false, false);
        obd_pick_t k = obd_pair_pick(&p, &a, LINK_MIN, PAIR_MIN);
        CHECK(k == OBD_PICK_NONE, "paired, automatic, another adapter at %d dBm: never", r);
        offered_auto += k != OBD_PICK_NONE;
        a.user_search = true;
        k = obd_pair_pick(&p, &a, LINK_MIN, PAIR_MIN);
        CHECK(k == (r >= PAIR_MIN ? OBD_PICK_OFFER : OBD_PICK_NONE), "paired, Find OBD, another adapter at %d dBm", r);
        CHECK(k != OBD_PICK_SAVED, "another adapter is never taken as the saved one");
        offered_user += k == OBD_PICK_OFFER;
    }
    CHECK(offered_auto == 0 && offered_user == 71, "automatic 0 offers, Find OBD %d (-70..0 dBm)", offered_user);
    /* exhaustive: every combination of inputs, the properties the owner asked for */
    int n = 0;
    for (int hs = 0; hs < 2; hs++) for (int is = 0; is < 2; is++) for (int us = 0; us < 2; us++)
    for (int lo = 0; lo < 2; lo++) for (int fa = 0; fa < 2; fa++) for (int ph = 0; ph < 3; ph++) for (int sk = 0; sk < 2; sk++)
    for (int r = -110; r <= 10; r += 5) {
        obd_pair_t q;
        obd_pair_init(&q);
        q.phase = (obd_pair_phase_t)ph;
        if (sk) { q.skip[0].why = OBD_SKIP_DECLINED; memcpy(q.skip[0].key, K_MINE, OBD_PAIR_KEY_LEN); }
        obd_adv_t a = {.key = K_MINE, .rssi = r, .have_saved = hs, .is_saved = hs && is, .user_search = us, .looks_obd = lo, .failed = fa};
        obd_pick_t k = obd_pair_pick(&q, &a, LINK_MIN, PAIR_MIN);
        if (k == OBD_PICK_OFFER)
            CHECK(r >= PAIR_MIN && lo && !fa && !sk && ph == OBD_PAIR_IDLE && (!hs || us) && !a.is_saved, "offer only when every rule holds");
        CHECK((k == OBD_PICK_SAVED) == (a.is_saved && r >= LINK_MIN), "saved iff the saved adapter above the link floor");
        if (hs && !us) CHECK(k != OBD_PICK_OFFER, "a saved adapter is never bypassed without the user's Find OBD");
        if (r < LINK_MIN) CHECK(k == OBD_PICK_NONE, "below the link floor: nothing");
        n++;
    }
    printf("  saved-adapter lock: %d input combinations\n", n);
}

/* 3-5. nothing saved without the user's yes; the timeout; "Not this one" */
static void ask(obd_pair_t *p, const uint8_t *k, int64_t now)
{
    obd_pair_probe(p, k, "OBDLink CX", -55);
    CHECK(obd_pair_probe_answered(p, now, ASK_MS), "probe answered: asking");
}

static void test_confirm(void)
{
    obd_pair_t p;
    obd_pair_init(&p);
    int64_t t = 1000;
    /* probe: nothing to save, one question at a time */
    obd_pair_probe(&p, K_MINE, "OBDLink\x01 CX\xC3\xA9", -55);
    CHECK(p.phase == OBD_PAIR_PROBING && !strcmp(p.name, "OBDLink CX"), "probing; the name kept printable ASCII ('%s')", p.name);
    obd_adv_t other = adv(K_NEIGHBOUR, -40, false, false, false);
    CHECK(obd_pair_pick(&p, &other, LINK_MIN, PAIR_MIN) == OBD_PICK_NONE, "while probing nothing else is offered");
    CHECK(!obd_pair_offer(&p, t, NULL, 0, NULL, NULL), "no question while probing");
    CHECK(obd_pair_answer(&p, OBD_ANS_YES, t).done == OBD_DONE_NONE, "a yes before the question saves nothing");
    CHECK(obd_pair_tick(&p, t + 999999).done == OBD_DONE_NONE, "the probe has no timeout of its own here");
    obd_pair_probe_failed(&p);
    CHECK(p.phase == OBD_PAIR_IDLE && !obd_pair_skipped(&p, K_MINE), "a failed probe: idle (the caller's failure list skips it)");
    CHECK(!obd_pair_probe_answered(&p, t, ASK_MS), "an answer without a probe asks nothing");

    /* the question, then yes: the only SAVE */
    ask(&p, K_MINE, t);
    char name[OBD_PAIR_NAME_LEN];
    const char *str = NULL;
    int left = -1;
    CHECK(obd_pair_offer(&p, t, name, sizeof name, &str, &left) && !strcmp(name, "OBDLink CX") && !strcmp(str, "strong") && left == 30,
          "the sheet: '%s', %s, %d s", name, str ? str : "-", left);
    CHECK(obd_pair_offer(&p, t + 29001, NULL, 0, NULL, &left) && left == 1, "1 s left");
    CHECK(obd_pair_pick(&p, &other, LINK_MIN, PAIR_MIN) == OBD_PICK_NONE, "while asking nothing else is offered");
    obd_adv_t saved = adv(K_SAVED, -85, true, true, false);
    CHECK(obd_pair_pick(&p, &saved, LINK_MIN, PAIR_MIN) == OBD_PICK_SAVED, "while asking the saved adapter still connects");
    CHECK(obd_pair_tick(&p, t + ASK_MS - 1).done == OBD_DONE_NONE, "29.999 s: still asking");
    obd_pair_result_t r = obd_pair_answer(&p, OBD_ANS_YES, t + 12000);
    CHECK(r.done == OBD_DONE_SAVE && !memcmp(r.key, K_MINE, OBD_PAIR_KEY_LEN) && !strcmp(r.name, "OBDLink CX"), "yes in time: SAVE this adapter");
    CHECK(p.phase == OBD_PAIR_IDLE && !obd_pair_skipped(&p, K_MINE), "after yes: idle, not skipped");
    CHECK(obd_pair_answer(&p, OBD_ANS_YES, t + 12001).done == OBD_DONE_NONE, "a second tap saves nothing more");

    /* timeout: not saved, not offered again until the user's Find OBD */
    obd_pair_init(&p);
    ask(&p, K_MINE, t);
    CHECK(obd_pair_tick(&p, t + ASK_MS).done == OBD_DONE_EXPIRED, "30 s without an answer: EXPIRED");
    CHECK(!obd_pair_offer(&p, t + ASK_MS, NULL, 0, NULL, NULL), "the sheet closes");
    obd_adv_t mine = adv(K_MINE, -50, false, false, false);
    CHECK(obd_pair_pick(&p, &mine, LINK_MIN, PAIR_MIN) == OBD_PICK_NONE, "not offered again by the plain scan");
    CHECK(obd_pair_answer(&p, OBD_ANS_YES, t + ASK_MS + 10).done == OBD_DONE_NONE, "a late yes saves nothing");
    obd_pair_user_search(&p);
    CHECK(obd_pair_pick(&p, &mine, LINK_MIN, PAIR_MIN) == OBD_PICK_OFFER, "the user's Find OBD offers it again");
    /* a yes that arrives after the time ran out, before the tick: still not saved */
    obd_pair_init(&p);
    ask(&p, K_MINE, t);
    CHECK(obd_pair_answer(&p, OBD_ANS_YES, t + ASK_MS).done == OBD_DONE_EXPIRED, "yes at 30.000 s (tick not run yet): EXPIRED, not saved");
    /* the car board's 5 s window */
    obd_pair_init(&p);
    obd_pair_probe(&p, K_MINE, "", -68);
    obd_pair_probe_answered(&p, t, 5000);
    CHECK(obd_pair_offer(&p, t, name, sizeof name, &str, &left) && !strcmp(name, "OBD adapter") && !strcmp(str, "good") && left == 5,
          "car board: no name -> 'OBD adapter', -68 dBm -> good, 5 s");
    CHECK(obd_pair_tick(&p, t + 5000).done == OBD_DONE_EXPIRED, "car board: no tap in 5 s, not saved");

    /* "Not this one": skipped for this boot, also through Find OBD */
    obd_pair_init(&p);
    ask(&p, K_MINE, t);
    CHECK(obd_pair_answer(&p, OBD_ANS_NO, t + 3000).done == OBD_DONE_DECLINED, "Not this one: DECLINED");
    CHECK(obd_pair_pick(&p, &mine, LINK_MIN, PAIR_MIN) == OBD_PICK_NONE, "declined: not offered by the scan");
    obd_pair_user_search(&p);
    mine.user_search = true;
    CHECK(obd_pair_pick(&p, &mine, LINK_MIN, PAIR_MIN) == OBD_PICK_NONE, "declined: not offered by Find OBD either (this boot)");
    obd_adv_t nb = adv(K_NEIGHBOUR, -60, false, false, true);
    CHECK(obd_pair_pick(&p, &nb, LINK_MIN, PAIR_MIN) == OBD_PICK_OFFER, "another adapter can still be offered");
    obd_pair_init(&p);                                              /* the next boot */
    CHECK(obd_pair_pick(&p, &mine, LINK_MIN, PAIR_MIN) == OBD_PICK_OFFER, "next boot: offered again");

    /* the skip list is bounded: the oldest of 9 declines drops out, the 8 newest stay */
    obd_pair_init(&p);
    uint8_t keys[9][OBD_PAIR_KEY_LEN];
    for (int i = 0; i < 9; i++) {
        memset(keys[i], 0, OBD_PAIR_KEY_LEN);
        keys[i][6] = (uint8_t)(10 + i);
        ask(&p, keys[i], t);
        obd_pair_answer(&p, OBD_ANS_NO, t + 1);
    }
    int still = 0;
    for (int i = 1; i < 9; i++) still += obd_pair_skipped(&p, keys[i]);
    CHECK(still == 8 && !obd_pair_skipped(&p, keys[0]), "skip list keeps the 8 newest (%d)", still);
    CHECK(!strcmp(obd_pair_strength(-60), "strong") && !strcmp(obd_pair_strength(-61), "good"), "strong from -60 dBm");
}

/* random event sequences: SAVE comes only from a yes given while the question is open, for the device asked about */
static void test_random(void)
{
    srand(7);
    int saves = 0, events = 0;
    for (int run = 0; run < 2000; run++) {
        obd_pair_t p;
        obd_pair_init(&p);
        int64_t t = 0;
        bool open = false;
        int64_t until = 0;
        uint8_t asked[OBD_PAIR_KEY_LEN] = {0};
        for (int step = 0; step < 60; step++, events++) {
            t += rand() % 8000;
            uint8_t k[OBD_PAIR_KEY_LEN] = {0, 0, 0, 0, 0, 0, (uint8_t)(rand() % 4)};
            obd_pair_result_t r = {.done = OBD_DONE_NONE};
            bool yes_now = false;
            switch (rand() % 6) {
            case 0: { obd_adv_t a = adv(k, -40 - rand() % 60, rand() % 2, false, rand() % 2);
                      if (obd_pair_pick(&p, &a, LINK_MIN, PAIR_MIN) == OBD_PICK_OFFER) obd_pair_probe(&p, k, "A", a.rssi); break; }
            case 1: if (obd_pair_probe_answered(&p, t, ASK_MS)) { open = true; until = t + ASK_MS; memcpy(asked, p.key, sizeof asked); } break;
            case 2: obd_pair_probe_failed(&p); break;
            case 3: yes_now = open && t < until; r = obd_pair_answer(&p, OBD_ANS_YES, t); break;
            case 4: r = obd_pair_answer(&p, OBD_ANS_NO, t); break;
            case 5: r = obd_pair_tick(&p, t); break;
            }
            if (r.done != OBD_DONE_NONE) open = false;
            if (r.done == OBD_DONE_SAVE) {
                saves++;
                CHECK(yes_now && !memcmp(r.key, asked, sizeof asked), "run %d step %d: SAVE only for a timely yes on the asked device", run, step);
            } else if (yes_now) CHECK(0, "run %d step %d: a timely yes did not save", run, step);
        }
    }
    CHECK(saves > 0, "the random runs reached a save");
    printf("  random sequences: %d events, %d saves, each from a yes in time\n", events, saves);
}

int main(void)
{
    test_pick_rssi();
    test_saved_lock();
    test_confirm();
    test_random();
    printf("pair: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
