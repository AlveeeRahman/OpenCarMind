/* Host test: the Health app's plain words (main/health_text.c) and the Wi-Fi guard head's first-scan skip
   (main/ids_core.h). Each state -> its card (severity, headline, meaning, action), the issue count and the worst
   severity, the anomaly reason from the detector's per-feature scores, and no jargon on screen. */
#include <stdio.h>
#include <string.h>
#include "health_text.h"
#include "ids_core.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL %s (line %d)\n", msg, __LINE__); } } while (0)

/* a healthy board: battery counted on a full charge, the guard watching, the AI on time */
static void good(health_view_t *v)
{
    memset(v, 0, sizeof *v);
    v->bat_valid = v->bat_anchored = true;
    v->soh_src = HV_SOH_FULL; v->soh_pct = 98; v->soh_ref_n = 10; v->soh_ref_need = 10;
    v->anom_state = HV_ANOM_NORMAL; v->anom_warm_need_h = 24;
    v->ids_state = HV_IDS_WATCHING; v->blocking_on = true; v->shadow_need_h = 2;
    v->ai_started = true; v->ai_n = 3; v->ai_core = 1;
    for (int i = 0; i < 3; i++) { v->ai[i].state = 1; v->ai[i].deadline_ms = 1000; }
    snprintf(v->ai[0].name, 8, "bat"); snprintf(v->ai[1].name, 8, "banom"); snprintf(v->ai[2].name, 8, "ids");
}

static bool has(const char *s, const char *sub) { return strstr(s, sub) != NULL; }

/* nothing on screen may use the detector's or the scheduler's jargon */
static void no_jargon(const hs_card_t *c, const char *what)
{
    static const char *const J[] = { "SoH", "DCIR", "innovation", "autoencoder", "z-score", "RTA", "WCRT", "score", "q8" };
    for (size_t k = 0; k < sizeof J / sizeof J[0]; k++) {
        char m[96];
        snprintf(m, sizeof m, "%s: no \"%s\"", what, J[k]);
        CHECK(!has(c->head, J[k]) && !has(c->mean, J[k]) && !has(c->act, J[k]), m);
    }
    CHECK(c->head[0] && c->mean[0] && c->act[0] && c->word[0], what);
    CHECK(!has(c->head, ";") && !has(c->mean, ";") && !has(c->act, ";"), "no semicolons");
}

static void card(const health_view_t *v, int page, hs_card_t *c, const char *what)
{
    hs_card(v, page, 852, c);                              /* 14:12 */
    no_jargon(c, what);
}

int main(void)
{
    health_view_t v;
    hs_card_t c;
    uint8_t w;
    char b[64];

    /* ---- battery ---- */
    good(&v);
    card(&v, HS_P_BAT, &c, "battery good");
    CHECK(c.sev == HS_GOOD && !strcmp(c.head, "Battery is healthy"), "good: headline");
    CHECK(!strcmp(c.mean, "Holds 98 % of its rated charge") && !strcmp(c.act, "Nothing to do"), "good: meaning, action");
    CHECK(hs_issues(&v, &w) == 0 && w == HS_GOOD, "good: no issue");
    hs_header(0, b, sizeof b);
    CHECK(!strcmp(b, "All good"), "header: All good");

    v.soh_src = HV_SOH_MODEL; v.soh_pct = 97.4f;
    card(&v, HS_P_BAT, &c, "battery model");
    CHECK(!strcmp(c.mean, "Holds about 97 % of its starting charge"), "model: a relative figure, said so");

    good(&v); v.soh_src = HV_SOH_LEARNING; v.soh_ref_n = 7;
    card(&v, HS_P_BAT, &c, "battery learning");
    CHECK(c.sev == HS_INFO && !strcmp(c.head, "Learning your battery") && !strcmp(c.word, "Learning"), "learning: headline");
    CHECK(!strcmp(c.mean, "Needs 3 more full charges to judge its health"), "learning: 3 more");
    CHECK(has(c.act, "charge it full"), "learning: action");
    CHECK(hs_issues(&v, NULL) == 0, "learning is not an issue");
    v.soh_ref_n = 9; card(&v, HS_P_BAT, &c, "learning 1");
    CHECK(!strcmp(c.mean, "Needs 1 more full charge to judge its health"), "learning: singular");
    v.soh_ref_n = 10; card(&v, HS_P_BAT, &c, "learning 0");
    CHECK(has(c.mean, "one more full charge"), "learning: all references but no figure yet");

    good(&v); v.bat_valid = false;
    card(&v, HS_P_BAT, &c, "battery starting");
    CHECK(c.sev == HS_INFO && has(c.head, "Starting"), "starting");

    good(&v); v.soh_pct = 80.0f;
    CHECK(hs_issues(&v, NULL) == 0, "80 %: not worn yet");
    v.soh_pct = 76;
    card(&v, HS_P_BAT, &c, "battery worn");
    CHECK(c.sev == HS_CHECK && c.issue == HI_BAT_WORN && !strcmp(c.head, "Battery is wearing out"), "worn: amber");
    CHECK(!strcmp(c.mean, "Holds 76 % of its rated charge, runs shorter on battery"), "worn: meaning");
    CHECK(!strcmp(c.act, "Plan to replace the cell"), "worn: action");
    CHECK(hs_issues(&v, &w) == 1u << HI_BAT_WORN && w == HS_CHECK, "worn: one amber issue");
    v.soh_pct = 69;
    card(&v, HS_P_BAT, &c, "battery worn out");
    CHECK(c.sev == HS_ACT && !strcmp(c.word, "Act now"), "below 70 %: red");

    /* ---- the anomaly reason from the per-feature scores ---- */
    static const struct { int feat, reason; uint8_t sev; const char *head; } AN[] = {
        { 0, AR_DRIFT, HS_CHECK, "Battery charge reading drifts" }, { 1, AR_DRIFT, HS_CHECK, "Battery charge reading drifts" },
        { 2, AR_SELFDIS, HS_CHECK, "Battery loses charge while resting" }, { 3, AR_LEAK, HS_CHECK, "Battery current reading looks wrong" },
        { 4, AR_HEAT, HS_ACT, "Battery warms up more than usual" }, { 5, AR_RES, HS_CHECK, "Voltage drops more under load" },
        { 6, AR_RES, HS_CHECK, "Voltage drops more under load" }, { 7, AR_LOAD, HS_CHECK, "Board draws more power than usual" },
        { 8, AR_GLITCH, HS_CHECK, "Connection to the battery glitches" } };
    for (size_t i = 0; i < sizeof AN / sizeof AN[0]; i++) {
        good(&v);
        v.anom_state = HV_ANOM_ALARM;
        for (int k = 0; k < HV_ANOM_NF; k++) v.anom_err[k] = 0.1f + 0.01f * (float)k;
        v.anom_err[AN[i].feat] = 2.5f;
        char m[64];
        snprintf(m, sizeof m, "anomaly feature %d", AN[i].feat);
        CHECK(hs_anom_reason(v.anom_err) == AN[i].reason, m);
        card(&v, HS_P_BAT, &c, m);
        CHECK(c.issue == HI_BAT_ANOM && c.sev == AN[i].sev && !strcmp(c.head, AN[i].head), m);
    }
    float zero[HV_ANOM_NF] = { 0 };
    CHECK(hs_anom_reason(zero) == AR_N && !strcmp(hs_anom_reason_text(AR_N), "acts unusually"), "no scores: a plain fallback");
    CHECK(!strcmp(hs_anom_reason_text(AR_HEAT), "warms up more than usual"), "reason text: heat");
    CHECK(!strcmp(hs_anom_reason_text(AR_LEAK), "current reading looks wrong"), "reason text: current");
    CHECK(!strcmp(hs_anom_reason_text(AR_GLITCH), "connection to the battery glitches"), "reason text: connection");
    CHECK(has(hs_anom_reason_text(AR_RES), "resistance went up"), "reason text: resistance");
    /* the heat card's action, the connector's */
    good(&v); v.anom_state = HV_ANOM_ALARM; v.anom_err[4] = 1;
    card(&v, HS_P_BAT, &c, "heat");
    CHECK(!strcmp(c.act, "Let it cool, keep it out of the sun"), "heat: action");
    v.anom_err[4] = 0; v.anom_err[8] = 1;
    card(&v, HS_P_BAT, &c, "glitch");
    CHECK(!strcmp(c.act, "Check the battery connector"), "glitch: action");
    /* an alarm beats wear on the card, both count */
    v.soh_pct = 75;
    card(&v, HS_P_BAT, &c, "alarm + worn");
    CHECK(hs_issue_count(hs_issues(&v, &w)) == 2 && w == HS_CHECK, "alarm + worn: 2 issues");
    hs_header(hs_issues(&v, NULL), b, sizeof b);
    CHECK(!strcmp(b, "2 things to check"), "header: 2 things");
    v.anom_err[8] = 0; v.anom_err[4] = 1;
    card(&v, HS_P_BAT, &c, "heat + worn");
    CHECK(c.issue == HI_BAT_ANOM && c.sev == HS_ACT, "the red one leads the card");
    hs_issues(&v, &w);
    CHECK(w == HS_ACT, "worst: red");

    /* ---- guard ---- */
    good(&v);
    card(&v, HS_P_GUARD, &c, "guard watching");
    CHECK(c.sev == HS_GOOD && !strcmp(c.head, "No threats seen") && !strcmp(c.mean, "Watching Bluetooth and Wi-Fi around the car"),
          "watching");
    v.ids_state = HV_IDS_SHADOW; v.shadow_h = 1.2f;
    card(&v, HS_P_GUARD, &c, "guard shadow");
    CHECK(c.sev == HS_INFO && !strcmp(c.head, "Learning this place (1.2 of 2 h)") && !strcmp(c.mean, "Blocks nothing yet"), "shadow");
    CHECK(hs_issues(&v, NULL) == 0, "shadow is not an issue");
    v.ids_state = HV_IDS_BLOCKING; v.block_why = HV_GR_BLE; v.block_left_s = 541;
    card(&v, HS_P_GUARD, &c, "guard blocking");
    CHECK(c.sev == HS_CHECK && c.issue == HI_GUARD_BLOCK && !strcmp(c.head, "Blocked a flood of Bluetooth devices"), "blocking: headline");
    CHECK(!strcmp(c.mean, "New adapters and pairing paused until 14:22"), "blocking: until (14:12 + 10 min, rounded up)");
    CHECK(!strcmp(c.act, "Nothing to do. Tap Clear block if this is your own device"), "blocking: action");
    hs_card(&v, HS_P_GUARD, -1, &c);
    CHECK(!strcmp(c.mean, "New adapters and pairing paused for 10 more min"), "blocking: no clock");
    v.block_why = HV_GR_DEAUTH; v.block_left_s = 600;
    hs_card(&v, HS_P_GUARD, 1435, &c);
    CHECK(!strcmp(c.head, "Blocked a Wi-Fi disconnect attack") && !strcmp(c.mean, "Clock sync over Wi-Fi paused until 00:05"),
          "deauth: past midnight");
    good(&v); v.adapter_mismatch = true;
    card(&v, HS_P_GUARD, &c, "adapter mismatch");
    CHECK(c.issue == HI_ADAPTER && c.sev == HS_CHECK && !strcmp(c.head, "An adapter that looks like yours behaved differently"),
          "mismatch: headline");
    CHECK(!strcmp(c.mean, "Its data is kept out of learning") && !strcmp(c.act, "Check that the adapter in the car is yours"),
          "mismatch: meaning, action");
    hs_header(hs_issues(&v, NULL), b, sizeof b);
    CHECK(!strcmp(b, "1 thing to check"), "header: 1 thing");
    good(&v); v.ids_would = true; v.blocking_on = false; v.block_why = HV_GR_WIFI;
    card(&v, HS_P_GUARD, &c, "would block");
    CHECK(c.sev == HS_INFO && !strcmp(c.head, "Saw a flood of Wi-Fi networks") && has(c.act, "Settings"), "would block, setting off");
    good(&v); v.link_note = true;
    card(&v, HS_P_GUARD, &c, "link note");
    CHECK(c.sev == HS_INFO && has(c.head, "slower"), "link note");
    good(&v); v.ids_state = HV_IDS_OFF;
    card(&v, HS_P_GUARD, &c, "guard off");
    CHECK(c.sev == HS_INFO, "guard off: not an issue");

    /* ---- AI ---- */
    good(&v);
    card(&v, HS_P_AI, &c, "ai ok");
    CHECK(c.sev == HS_GOOD && !strcmp(c.head, "All AI features on time") && !strcmp(c.mean, "Battery and guard checks run in the background"),
          "ai ok");
    v.ai_level = AI_DL_SLOW; v.os_level = 1;
    card(&v, HS_P_AI, &c, "ai slowed");
    CHECK(c.sev == HS_INFO && !strcmp(c.head, "Board busy: AI features slowed") && !strcmp(c.mean, "Keeps the screen and the drive log smooth")
          && !strcmp(c.act, "Nothing to do") && !strcmp(c.word, "Slowed"), "ai slowed");
    CHECK(hs_issues(&v, NULL) == 0, "slowed is not an issue");
    good(&v); v.ai[0].misses = 2;
    card(&v, HS_P_AI, &c, "ai late");
    CHECK(c.sev == HS_INFO && !strcmp(c.mean, "2 runs finished late since the start"), "late runs: not 'all on time'");
    good(&v); v.ai_started = false; v.ai_core = -1;
    card(&v, HS_P_AI, &c, "ai starting");
    CHECK(c.sev == HS_INFO && hs_issues(&v, NULL) == 0, "before the start: no issue");
    good(&v); v.ai[2].state = 2;
    card(&v, HS_P_AI, &c, "ai refused");
    CHECK(c.issue == HI_AI_OFF && c.sev == HS_CHECK && !strcmp(c.mean, "Network guard did not pass its start check"), "refused");
    snprintf(v.ai_why, sizeof v.ai_why, "self-test over budget: one retry in 30 s");
    card(&v, HS_P_AI, &c, "ai retry");
    CHECK(c.sev == HS_INFO && hs_issues(&v, NULL) == 0, "a pending retry is not an issue");
    good(&v); v.ai_core = -1; snprintf(v.ai_why, sizeof v.ai_why, "off in safe mode");
    card(&v, HS_P_AI, &c, "ai safe");
    CHECK(c.sev == HS_INFO && hs_issues(&v, NULL) == 0, "safe mode: off on purpose");
    good(&v); v.ai_core = -1; snprintf(v.ai_why, sizeof v.ai_why, "no memory for the worker");
    card(&v, HS_P_AI, &c, "ai no worker");
    CHECK(c.issue == HI_AI_OFF && !strcmp(c.head, "AI features are off"), "no worker");

    /* ---- every issue: its page, its end line; many issues at once ---- */
    CHECK(hs_issue_page(HI_BAT_WORN) == HS_P_BAT && hs_issue_page(HI_BAT_ANOM) == HS_P_BAT && hs_issue_page(HI_GUARD_BLOCK) == HS_P_GUARD
          && hs_issue_page(HI_ADAPTER) == HS_P_GUARD && hs_issue_page(HI_AI_OFF) == HS_P_AI, "issue pages");
    for (int k = 0; k < HI_N; k++) CHECK(hs_issue_over(k)[0], "every issue has an end line");
    good(&v); v.soh_pct = 60; v.anom_state = HV_ANOM_ALARM; v.anom_err[2] = 1; v.ids_state = HV_IDS_BLOCKING; v.adapter_mismatch = true;
    v.ai[1].state = 4;
    uint32_t m = hs_issues(&v, &w);
    CHECK(m == 0x1F && hs_issue_count(m) == 5 && w == HS_ACT, "five issues, worst red");
    hs_header(m, b, sizeof b);
    CHECK(!strcmp(b, "5 things to check"), "header: 5 things");

    /* ---- thousands ---- */
    hs_thousands(b, sizeof b, 1000); CHECK(!strcmp(b, "1,000"), "1,000");
    hs_thousands(b, sizeof b, 999); CHECK(!strcmp(b, "999"), "999");
    hs_thousands(b, sizeof b, 4294967295u); CHECK(!strcmp(b, "4,294,967,295"), "max");
    hs_thousands(b, sizeof b, 0); CHECK(!strcmp(b, "0"), "0");

    /* ---- the Wi-Fi guard head: a scan with no earlier scan is not scored (V21: 290 of 256 after boot) ---- */
    CHECK(!ids_wifi_scan_scored(false), "first scan after boot: skipped");
    CHECK(ids_wifi_scan_scored(true), "a scan with one before it: scored");
    {
        /* end to end through ids_step: the skipped scan never reaches the head, the next one scores low */
        ids_t g;
        ids_init(&g);
        ids_in_t in = { 0 };
        in.nwin = 1; in.blocking_on = true; in.round_start = true;
        bool have_prev = false;                            /* boot: slot q-1 does not exist */
        if (ids_wifi_scan_scored(have_prev)) { in.wifi[0] = (ids_win_t){ 24, 24, 24, 24, true }; in.n_wifi = 1; }
        ids_step(&g, &in);
        CHECK(!g.on[IDS_H_WIFI] && g.q8[IDS_H_WIFI] == 0 && g.wifi_scans == 0, "boot scan: the Wi-Fi head shows no data, not 290");
        in.round_start = false;
        have_prev = true;                                  /* the same 24 networks again */
        if (ids_wifi_scan_scored(have_prev)) { in.wifi[0] = (ids_win_t){ 24, 24, 0, 0, true }; in.n_wifi = 1; }
        ids_step(&g, &in);
        CHECK(g.on[IDS_H_WIFI] && g.q8[IDS_H_WIFI] < 256 && g.wifi_scans == 1, "second scan: compared, below the limit");
    }

    printf("health text: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
