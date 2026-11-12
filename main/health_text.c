/* health_text: see health_text.h. Every text here is what the firmware knows, said plainly: no model names, no scores. */
#include "health_text.h"
#include <stdio.h>
#include <string.h>

/* ai_svc.h's AI_ST_* (that header needs IDF; health_view.c checks these match) */
enum { HS_ST_OFF = 0, HS_ST_RUNNING, HS_ST_REFUSED, HS_ST_SAFE, HS_ST_NOTASK };

#define WORN_PCT 80.0f            /* below: "wearing out" (the usual end-of-life mark) */
#define WORN_RED_PCT 70.0f        /* below: act now */

const char *hs_sev_word(uint8_t sev)
{
    static const char *const W[4] = { "Good", "Note", "Check soon", "Act now" };
    return sev < 4 ? W[sev] : "";
}

static void set(hs_card_t *c, uint8_t sev, uint8_t issue, const char *word, const char *head, const char *mean, const char *act)
{
    c->sev = sev;
    c->issue = issue;
    snprintf(c->word, sizeof c->word, "%s", word ? word : hs_sev_word(sev));
    snprintf(c->head, sizeof c->head, "%s", head);
    snprintf(c->mean, sizeof c->mean, "%s", mean);
    snprintf(c->act, sizeof c->act, "%s", act);
}

/* ---------- the anomaly reason ---------- */
/* an_step's features: 0 mean and 1 RMS of the charge filter's voltage error, 2 voltage slope at rest, 3 current at rest,
   4 temperature over the thermal model, 5 voltage sag under load and 6 at current steps (resistance), 7 discharge rate,
   8 voltage spikes and missing reads */
int hs_anom_reason(const float err[HV_ANOM_NF])
{
    static const uint8_t R[HV_ANOM_NF] = { AR_DRIFT, AR_DRIFT, AR_SELFDIS, AR_LEAK, AR_HEAT, AR_RES, AR_RES, AR_LOAD, AR_GLITCH };
    int k = 0;
    for (int j = 1; j < HV_ANOM_NF; j++) if (err[j] > err[k]) k = j;
    return err[k] > 0.0f ? R[k] : AR_N;
}

const char *hs_anom_reason_text(int r)
{
    static const char *const T[AR_N + 1] = {
        "charge reading drifts", "loses charge while resting", "current reading looks wrong", "warms up more than usual",
        "voltage drops more under load (resistance went up)", "draws more power than usual",
        "connection to the battery glitches", "acts unusually" };
    return r >= 0 && r <= AR_N ? T[r] : T[AR_N];
}

static void anom_card(const health_view_t *v, hs_card_t *c)
{
    switch (hs_anom_reason(v->anom_err)) {
    case AR_HEAT:   set(c, HS_ACT, HI_BAT_ANOM, NULL, "Battery warms up more than usual", "It runs warmer than its load explains",
                        "Let it cool, keep it out of the sun"); break;
    case AR_SELFDIS: set(c, HS_CHECK, HI_BAT_ANOM, NULL, "Battery loses charge while resting",
                         "Its voltage falls with nothing drawing power", "If it keeps happening, replace the cell"); break;
    case AR_LEAK:   set(c, HS_CHECK, HI_BAT_ANOM, NULL, "Battery current reading looks wrong",
                        "A current shows while the board rests", "Check the battery connector"); break;
    case AR_RES:    set(c, HS_CHECK, HI_BAT_ANOM, NULL, "Voltage drops more under load",
                        "The battery's resistance went up", "If it keeps happening, replace the cell"); break;
    case AR_LOAD:   set(c, HS_CHECK, HI_BAT_ANOM, NULL, "Board draws more power than usual",
                        "The battery empties faster than it learnt", "Check what is plugged into the board"); break;
    case AR_GLITCH: set(c, HS_CHECK, HI_BAT_ANOM, NULL, "Connection to the battery glitches",
                        "Some battery readings jump or go missing", "Check the battery connector"); break;
    case AR_DRIFT:  set(c, HS_CHECK, HI_BAT_ANOM, NULL, "Battery charge reading drifts",
                        "The charge level and the voltage disagree", "Charge it fully once so it can reset"); break;
    default:        set(c, HS_CHECK, HI_BAT_ANOM, NULL, "Battery acts unusually",
                        "It behaves differently from what it learnt", "If it keeps happening, check the battery"); break;
    }
}

/* ---------- issues ---------- */
static bool soh_known(const health_view_t *v) { return v->soh_src == HV_SOH_MODEL || v->soh_src == HV_SOH_FULL; }
static bool ai_soft_off(const health_view_t *v)        /* off on purpose or about to retry: not an issue */
{
    return strstr(v->ai_why, "safe mode") || strstr(v->ai_why, "retry");
}
static int ai_off_svc(const health_view_t *v)          /* the first service that is refused or has no task, else -1 */
{
    for (int i = 0; i < v->ai_n && i < AI_SVC_MAX; i++)
        if (v->ai[i].state == HS_ST_REFUSED || v->ai[i].state == HS_ST_NOTASK) return i;
    return -1;
}
static const char *svc_plain(const char *name)
{
    if (!strcmp(name, "bat")) return "Battery charge";
    if (!strcmp(name, "banom")) return "Battery check";
    if (!strcmp(name, "ids")) return "Network guard";
    return name;
}

static void until_text(char *b, size_t n, uint32_t left_s, int now_min)
{
    unsigned m = (unsigned)((left_s + 59) / 60);
    if (now_min < 0) { snprintf(b, n, "for %u more min", m); return; }
    int t = (now_min + (int)m) % 1440;
    snprintf(b, n, "until %02d:%02d", t / 60, t % 60);
}

bool hs_issue_card(const health_view_t *v, int issue, int now_min, hs_card_t *c)
{
    char b[HS_LINE], u[24];
    switch (issue) {
    case HI_BAT_WORN:
        if (!v->bat_valid || !soh_known(v) || v->soh_pct >= WORN_PCT) return false;
        if (v->soh_src == HV_SOH_FULL) snprintf(b, sizeof b, "Holds %.0f %% of its rated charge, runs shorter on battery", (double)v->soh_pct);
        else snprintf(b, sizeof b, "Holds about %.0f %% of its starting charge, runs shorter", (double)v->soh_pct);
        if (v->soh_pct < WORN_RED_PCT) set(c, HS_ACT, HI_BAT_WORN, NULL, "Battery is worn out", b, "Replace the cell soon");
        else set(c, HS_CHECK, HI_BAT_WORN, NULL, "Battery is wearing out", b, "Plan to replace the cell");
        return true;
    case HI_BAT_ANOM:
        if (v->anom_state != HV_ANOM_ALARM) return false;
        anom_card(v, c);
        return true;
    case HI_GUARD_BLOCK:
        if (v->ids_state != HV_IDS_BLOCKING) return false;
        until_text(u, sizeof u, v->block_left_s, now_min);
        if (v->block_why == HV_GR_WIFI || v->block_why == HV_GR_DEAUTH) {
            snprintf(b, sizeof b, "Clock sync over Wi-Fi paused %s", u);
            set(c, HS_CHECK, HI_GUARD_BLOCK, "Blocked", v->block_why == HV_GR_WIFI ? "Blocked a flood of Wi-Fi networks"
                : "Blocked a Wi-Fi disconnect attack", b, "Nothing to do. Tap Clear block if this is your own device");
        } else {
            snprintf(b, sizeof b, "New adapters and pairing paused %s", u);
            set(c, HS_CHECK, HI_GUARD_BLOCK, "Blocked", "Blocked a flood of Bluetooth devices", b,
                "Nothing to do. Tap Clear block if this is your own device");
        }
        return true;
    case HI_ADAPTER:
        if (!v->adapter_mismatch) return false;
        set(c, HS_CHECK, HI_ADAPTER, NULL, "An adapter that looks like yours behaved differently",
            "Its data is kept out of learning", "Check that the adapter in the car is yours");
        return true;
    case HI_AI_OFF: {
        if (!v->ai_started || ai_soft_off(v)) return false;
        int k = ai_off_svc(v);
        if (v->ai_core >= 0 && k < 0) return false;
        if (v->ai_core < 0) set(c, HS_CHECK, HI_AI_OFF, NULL, "AI features are off", "They did not start on this boot",
                                "Restart the board once");
        else {
            snprintf(b, sizeof b, "%s did not pass its start check", svc_plain(v->ai[k].name));
            set(c, HS_CHECK, HI_AI_OFF, NULL, "Some AI features are off", b, "Restart the board once");
        }
        return true;
    }
    default: return false;
    }
}

uint32_t hs_issues(const health_view_t *v, uint8_t *worst)
{
    uint32_t m = 0;
    uint8_t w = HS_GOOD;
    hs_card_t c;
    for (int k = 0; k < HI_N; k++)
        if (hs_issue_card(v, k, -1, &c)) { m |= 1u << k; if (c.sev > w) w = c.sev; }
    if (worst) *worst = w;
    return m;
}

int hs_issue_count(uint32_t mask)
{
    int n = 0;
    for (; mask; mask &= mask - 1) n++;
    return n;
}

int hs_issue_page(int issue)
{
    return issue == HI_BAT_WORN || issue == HI_BAT_ANOM ? HS_P_BAT : issue == HI_AI_OFF ? HS_P_AI : HS_P_GUARD;
}

void hs_header(uint32_t mask, char *b, size_t n)
{
    int k = hs_issue_count(mask);
    if (!k) snprintf(b, n, "All good");
    else snprintf(b, n, "%d %s to check", k, k == 1 ? "thing" : "things");
}

const char *hs_issue_over(int issue)
{
    static const char *const T[HI_N] = { "Battery no longer reads as worn", "Battery behaves normally again", "Block lifted",
                                         "Adapter check settled", "AI features running again" };
    return issue >= 0 && issue < HI_N ? T[issue] : "";
}

/* ---------- the page cards: the most severe active issue of the page, else the page's plain state ---------- */
static bool page_issue(const health_view_t *v, int page, int now_min, hs_card_t *c)
{
    hs_card_t t;
    bool any = false;
    for (int k = 0; k < HI_N; k++) {
        if (hs_issue_page(k) != page || !hs_issue_card(v, k, now_min, &t)) continue;
        if (!any || t.sev > c->sev) { *c = t; any = true; }
    }
    return any;
}

static void bat_card(const health_view_t *v, hs_card_t *c)
{
    char b[HS_LINE];
    if (!v->bat_valid) { set(c, HS_INFO, HI_NONE, "Starting", "Starting battery checks", "The first reading comes in a few seconds", "Nothing to do"); return; }
    if (soh_known(v)) {
        if (v->soh_src == HV_SOH_FULL) snprintf(b, sizeof b, "Holds %.0f %% of its rated charge", (double)v->soh_pct);
        else snprintf(b, sizeof b, "Holds about %.0f %% of its starting charge", (double)v->soh_pct);
        set(c, HS_GOOD, HI_NONE, NULL, "Battery is healthy", b, "Nothing to do");
        return;
    }
    int left = (int)v->soh_ref_need - (int)v->soh_ref_n;
    if (left > 0) snprintf(b, sizeof b, "Needs %d more full charge%s to judge its health", left, left == 1 ? "" : "s");
    else snprintf(b, sizeof b, "Needs one more full charge to judge its health");
    set(c, HS_INFO, HI_NONE, "Learning", "Learning your battery", b, "Now and then, run it low and charge it full");
}

static void guard_card(const health_view_t *v, hs_card_t *c)
{
    char h[HS_HEAD];
    const char *saw = v->block_why == HV_GR_WIFI ? "Saw a flood of Wi-Fi networks" : v->block_why == HV_GR_DEAUTH
                      ? "Saw a Wi-Fi disconnect attack" : "Saw a flood of Bluetooth devices";
    switch (v->ids_state) {
    case HV_IDS_LEARNING: set(c, HS_INFO, HI_NONE, "Starting", "Guard is starting", "It waits for Bluetooth or Wi-Fi data", "Nothing to do"); return;
    case HV_IDS_SHADOW:
        snprintf(h, sizeof h, "Learning this place (%.1f of %.0f h)", (double)v->shadow_h, (double)v->shadow_need_h);
        set(c, HS_INFO, HI_NONE, "Learning", h, v->ids_would ? "Blocks nothing yet, it only notes what it sees" : "Blocks nothing yet",
            "Nothing to do");
        return;
    case HV_IDS_WATCHING:
        if (v->ids_would && !v->blocking_on) { set(c, HS_INFO, HI_NONE, NULL, saw, "Blocking is off, so nothing was paused",
                                                   "Turn on blocking in Settings to pause it"); return; }
        if (v->link_note) { set(c, HS_INFO, HI_NONE, NULL, "Car link answers slower than usual", "Its data is kept out of learning",
                                "Nothing to do"); return; }
        set(c, HS_GOOD, HI_NONE, NULL, "No threats seen", v->blocking_on ? "Watching Bluetooth and Wi-Fi around the car"
            : "Watching only: blocking is off in Settings", "Nothing to do");
        return;
    default: set(c, HS_INFO, HI_NONE, "Off", "Guard is not running", "It starts with the AI features", "Nothing to do"); return;
    }
}

static void ai_card(const health_view_t *v, hs_card_t *c)
{
    char b[HS_LINE];
    if (!v->ai_started) { set(c, HS_INFO, HI_NONE, "Starting", "AI features starting", "They start once the board has booted well", "Nothing to do"); return; }
    if (strstr(v->ai_why, "safe mode")) { set(c, HS_INFO, HI_NONE, "Off", "AI features off in safe mode", "The board started in safe mode", "Nothing to do"); return; }
    if (strstr(v->ai_why, "retry")) { set(c, HS_INFO, HI_NONE, "Starting", "AI features start again soon", "A start check was slow, it runs once more", "Nothing to do"); return; }
    if (v->ai_level == AI_DL_PAUSED) { set(c, HS_INFO, HI_NONE, "Paused", "Board busy: AI features paused", "Keeps the screen and the drive log smooth", "Nothing to do"); return; }
    if (v->ai_level == AI_DL_SLOW || v->os_level) { set(c, HS_INFO, HI_NONE, "Slowed", "Board busy: AI features slowed", "Keeps the screen and the drive log smooth", "Nothing to do"); return; }
    uint32_t miss = 0;
    for (int i = 0; i < v->ai_n && i < AI_SVC_MAX; i++) miss += v->ai[i].misses;
    if (miss) {
        snprintf(b, sizeof b, "%lu run%s finished late since the start", (unsigned long)miss, miss == 1 ? "" : "s");
        set(c, HS_INFO, HI_NONE, NULL, "AI features mostly on time", b, "Nothing to do");
        return;
    }
    set(c, HS_GOOD, HI_NONE, NULL, "All AI features on time", "Battery and guard checks run in the background", "Nothing to do");
}

void hs_card(const health_view_t *v, int page, int now_min, hs_card_t *c)
{
    memset(c, 0, sizeof *c);
    if (page_issue(v, page, now_min, c)) return;
    if (page == HS_P_BAT) bat_card(v, c);
    else if (page == HS_P_GUARD) guard_card(v, c);
    else ai_card(v, c);
}

void hs_thousands(char *b, size_t n, uint32_t v)
{
    char t[16];
    int l = snprintf(t, sizeof t, "%lu", (unsigned long)v), o = 0;
    for (int i = 0; i < l && o + 2 < (int)n; i++) {
        if (i && (l - i) % 3 == 0) b[o++] = ',';
        b[o++] = t[i];
    }
    b[o] = 0;
}
