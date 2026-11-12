/* The network guard (main/ids_model.h, main/ids_core.c, main/ids_hooks.h): the rotation detector bit-exact against
 * carmind-ml's tests/ids_vectors.csv, the guard's states and blocking policy, and the hooks' bitmaps from writer to
 * reader.   make ids  (IDS_VECTORS = carmind-ml experiments/net_ids/tests/ids_vectors.csv) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ids_core.h"
#include "ids_hooks.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void vectors(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { printf("FAIL: no IDS vectors at %s (set IDS_VECTORS)\n", path); fails++; return; }
    char line[256];
    if (!fgets(line, sizeof line, f)) { fclose(f); fails++; return; }
    ids_rot_t s;
    ids_rot_init(&s);
    long rows = 0, bad = 0, onsets = 0;
    unsigned w, n, sb, nb, su, ex, bl, on;
    int sc;
    while (fscanf(f, "%u,%u,%u,%u,%u,%d,%u,%u,%u", &w, &n, &sb, &nb, &su, &sc, &ex, &bl, &on) == 9) {
        ids_rot_out_t o = ids_rot_step(&s, (uint16_t)n, (uint16_t)sb, (uint16_t)nb, (uint16_t)su);
        if (o.score_q8 != sc || o.exceed != (ex != 0) || o.block != (bl != 0) || o.onset != (on != 0)) bad++;
        onsets += o.onset;
        rows++;
    }
    fclose(f);
    CHECK(rows == 21601 && bad == 0 && onsets == 10, "vectors: rows %ld mismatches %ld onsets %ld", rows, bad, onsets);
    printf("ids vectors: %ld rows, %ld mismatches, %ld onsets, state %zu B\n", rows, bad, onsets, sizeof(ids_rot_t));
}

static ids_in_t crowd(void)              /* a normal crowd: 130 devices, few new, none single-use and new */
{
    ids_in_t in = { .nwin = 1, .blocking_on = true };
    in.scan = (ids_win_t){ 1230, 130, 4, 1, true };
    return in;
}
static ids_in_t flood(void)              /* a rotation flood: 60 single-use new addresses per window */
{
    ids_in_t in = crowd();
    in.scan = (ids_win_t){ 1300, 190, 64, 60, true };
    return in;
}

static void guard(void)
{
    static ids_t g;
    ids_init(&g);
    CHECK(g.state == IDS_ST_LEARNING, "learning before any window");
    uint32_t ch = 0;
    ids_in_t c = crowd(), fl = flood();
    ids_step(&g, &c);
    CHECK(g.state == IDS_ST_SHADOW, "shadow after the first window");
    /* a flood inside the shadow: nothing acts, nothing blocks (the scan head never blocks in its own shadow either) */
    for (int k = 0; k < 20; k++) ch |= ids_step(&g, &fl);
    CHECK(!g.acting && !(ch & IDS_CH_BLOCK_ON), "no block in the shadow");
    for (uint32_t k = g.win; k < IDS_SHADOW_WIN + 5; k++) ch |= ids_step(&g, &c);
    CHECK(ch & IDS_CH_SHADOW_END, "shadow end reported");
    CHECK(g.state == IDS_ST_WATCHING && !g.acting, "watching after 2 h of crowd");
    /* a flood after the shadow: blocks within 3 windows (3 of 6) */
    ch = 0;
    int k;
    for (k = 1; k <= 6 && !g.acting; k++) ch |= ids_step(&g, &fl);
    CHECK(g.acting && g.state == IDS_ST_BLOCKING && g.reason == IDS_R_SCAN && (ch & IDS_CH_BLOCK_ON), "flood blocked after %d windows", k - 1);
    CHECK(k - 1 == 3, "blocked on the 3rd window (got %d)", k - 1);
    CHECK(g.blocks_today == 1, "one block today");
    CHECK(ids_block_left_s(&g) == IDS_HOLD_WIN * 5, "10 min left at the onset: %u s", ids_block_left_s(&g));
    /* the scan stops (accept list on): no windows; the block still lifts after 10 min of wall time */
    ids_in_t quiet = crowd();
    quiet.scan.on = false;
    ch = 0;
    for (k = 0; k < (int)IDS_HOLD_WIN + 2 && g.acting; k++) ch |= ids_step(&g, &quiet);
    CHECK(!g.acting && (ch & IDS_CH_BLOCK_OFF), "lifted by itself after %d windows", k);
    CHECK(k >= (int)IDS_HOLD_WIN - 1 && k <= (int)IDS_HOLD_WIN + 1, "about 10 min (%d windows)", k);
    /* the user's Clear */
    for (k = 0; k < 6 && !g.acting; k++) ids_step(&g, &fl);
    CHECK(g.acting, "blocked again");
    ids_in_t clr = crowd();
    clr.clear = true;
    ch = ids_step(&g, &clr);
    CHECK(!g.acting && (ch & IDS_CH_CLEARED) && !(ch & IDS_CH_BLOCK_OFF), "cleared by the user");
    /* blocking off in Settings: the heads still decide, nothing acts, "would block" is logged once */
    ids_in_t off = flood();
    off.blocking_on = false;
    ch = 0;
    for (k = 0; k < 6; k++) ch |= ids_step(&g, &off);
    CHECK(!g.acting && g.would && (ch & IDS_CH_WOULD) && !(ch & IDS_CH_BLOCK_ON), "setting off: would block only");
    CHECK(g.state == IDS_ST_WATCHING, "setting off: state watching");
    ids_in_t on = crowd();
    ch = ids_step(&g, &on);
    CHECK(g.acting && (ch & IDS_CH_BLOCK_ON), "setting back on: the running block acts");
    clr.clear = true;
    ids_step(&g, &clr);
    /* blocks today reset on a new day */
    ids_in_t day = crowd();
    day.day = 77;
    ids_step(&g, &day);
    CHECK(g.blocks_today == 0, "a new day resets the count");
}

static void wifi_and_link(void)
{
    static ids_t g;
    ids_init(&g);
    ids_in_t c = crowd();
    for (uint32_t k = 0; k < IDS_SHADOW_WIN + 1; k++) ids_step(&g, &c);
    /* a first round in a new place: 24 APs all new and single-use; one window cannot block (3 of 6) */
    ids_in_t r = crowd();
    r.round_start = true;
    r.n_wifi = 1;
    r.wifi[0] = (ids_win_t){ 24, 24, 24, 24, true };
    ids_step(&g, &r);
    CHECK(!g.acting && g.q8[IDS_H_WIFI] > 256, "a busy first scan exceeds but does not block (q8 %d)", g.q8[IDS_H_WIFI]);
    /* three rounds in three new places: the persistence restarts each round, so still no block */
    for (int k = 0; k < 3; k++) ids_step(&g, &r);
    CHECK(!g.acting, "new places round after round: no block");
    /* a beacon flood: three scans in one round full of new single-use BSSIDs */
    ids_in_t b = crowd();
    b.n_wifi = 2;
    b.wifi[0] = b.wifi[1] = (ids_win_t){ 24, 24, 24, 24, true };
    b.round_start = true;
    ids_step(&g, &b);
    b.round_start = false;
    b.n_wifi = 1;
    uint32_t ch = ids_step(&g, &b);
    CHECK(g.acting && g.reason == IDS_R_WIFI_ROT && (ch & IDS_CH_BLOCK_ON), "beacon flood blocks the round");
    ids_in_t clr = crowd();
    clr.clear = true;
    ids_step(&g, &clr);
    CHECK(!g.acting, "cleared");
    /* deauth rule: 3 in a round */
    ids_in_t d = crowd();
    d.deauth_round = 2;
    ids_step(&g, &d);
    CHECK(!g.acting, "two deauths: no block");
    CHECK(!ids_deauth_rule(2) && ids_deauth_rule(3), "deauth rule at 3");
    d.deauth_round = 3;
    ids_step(&g, &d);
    CHECK(g.acting && g.reason == IDS_R_DEAUTH, "three deauths in a round block it");
    ids_step(&g, &clr);
    /* link head: learn 31 ms, then a +80 % relay delay flags (advisory: never blocks) */
    ids_in_t l = crowd();
    l.link_n = 40;
    l.link_sum16 = 40u * 31u * 16u;
    for (int k = 0; k < 200; k++) ids_step(&g, &l);
    CHECK(!g.link.flag && g.q8[IDS_H_LINK] < 256, "steady link: no note (q8 %d)", g.q8[IDS_H_LINK]);
    l.link_sum16 = 40u * 56u * 16u;
    uint32_t chl = 0;
    for (int k = 0; k < 6; k++) chl |= ids_step(&g, &l);
    CHECK(g.link.flag && (chl & IDS_CH_LINK_ON) && !g.acting, "relay delay: note on, no block");
    l.link_sum16 = 40u * 31u * 16u;
    chl = 0;
    for (int k = 0; k < IDS_LINK_CLEAN + 1; k++) chl |= ids_step(&g, &l);
    CHECK(!g.link.flag && (chl & IDS_CH_LINK_OFF), "note off after clean windows");
    /* the slowed rung: one step covers 4 windows, so the shadow passes 4x faster in steps */
    static ids_t s;
    ids_init(&s);
    ids_in_t sl = crowd();
    sl.nwin = 4;
    for (uint32_t k = 0; k < IDS_SHADOW_WIN / 4; k++) ids_step(&s, &sl);
    CHECK(!ids_shadow(&s), "slowed: the shadow is 2 h of wall time");
}

static void hooks(void)
{
    static ids_bm_t slots[IDS_BM_SLOTS], cur, prev;
    memset(slots, 0, sizeof slots);
    uint8_t a[6] = { 1, 2, 3, 4, 5, 0 };
    /* window 10 and 11: 100 devices, each 5 adverts per window, the same in both */
    for (uint32_t w = 10; w <= 11; w++)
        for (int rep = 0; rep < 5; rep++)
            for (int d = 0; d < 100; d++) { a[5] = (uint8_t)d; ids_bm_add(slots, w, ids_fnv(a, 6, 0)); }
    CHECK(ids_bm_copy(slots, 11, &cur) && ids_bm_copy(slots, 10, &prev), "both windows readable");
    uint16_t n, sb, nb, su, pat;
    ids_bm_window(&cur, &prev, &n, &sb, &nb, &su, &pat);
    CHECK(n == 500 && sb >= 95 && sb <= 100 && nb == 0 && su == 0, "steady crowd: n %u set %u new %u su %u", n, sb, nb, su);
    /* window 12: the same crowd plus 40 addresses seen once each (rotation) */
    for (int d = 0; d < 100; d++) { a[5] = (uint8_t)d; ids_bm_add(slots, 12, ids_fnv(a, 6, 0)); ids_bm_add(slots, 12, ids_fnv(a, 6, 0)); }
    for (int d = 0; d < 40; d++) { uint8_t r[6] = { 9, 9, (uint8_t)d, 7, 7, 7 }; ids_bm_add(slots, 12, ids_fnv(r, 6, 1)); }
    CHECK(ids_bm_copy(slots, 12, &cur) && ids_bm_copy(slots, 11, &prev), "readable");
    ids_bm_window(&cur, &prev, &n, &sb, &nb, &su, &pat);
    CHECK(su >= 36 && su <= 40 && nb >= 36, "rotation: su_new %u new %u", su, nb);
    /* slot reuse: window 14 reuses window 10's slot; window 10 is no longer readable */
    ids_bm_add(slots, 14, 123);
    CHECK(!ids_bm_copy(slots, 10, &cur), "a reused slot is not read as its old window");
    CHECK(ids_bm_copy(slots, 14, &cur) && cur.n == 1, "the new window has only its own advert");
    CHECK(!ids_bm_copy(slots, 13, &cur), "an empty window is not readable");
    CHECK(ids_reason_deauth(3) && ids_reason_deauth(8) && !ids_reason_deauth(201) && !ids_reason_deauth(15), "deauth reasons");
}

int main(void)
{
    const char *v = getenv("IDS_VECTORS");
    vectors(v && *v ? v : "ids_vectors.csv");
    guard();
    wifi_and_link();
    hooks();
    printf("ids: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
