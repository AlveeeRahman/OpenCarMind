/* ids_svc: see ids_svc.h. Three parts: the hooks (radio callbacks, counters and bitmap bits only), the worker step (the
 * windows, ids_core's rules, the lock-free result), and the board-task side (the setting in NVS, events, the log line). */
#include "ids_svc.h"
#if CONFIG_OCM_IDS
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "ai_snap.h"
#include "events.h"
#include "ids_hooks.h"
#include "ids_selftest.h"

static const char *TAG = "ids";

#define IDS_PERIOD_MS   IDS_WIN_MS
#define IDS_DEADLINE_MS 1000
#define IDS_BUDGET_US   2000
#define IDS_SLOW        4                       /* the ladder's AI rung: every 4th window (DESIGN.md 6.5) */
#define IDS_LOG_US      (60LL * 1000000)

/* ============ HOOKS BEGIN: one writer task per block; counters and bitmap bits only (tests/host/ids_audit.sh) ============ */
static ids_host_blk_t  s_host;                  /* NimBLE host task */
static ids_obd_blk_t   s_obd;                   /* OBD task */
static ids_wev_blk_t   s_wev;                   /* esp_event task */
static ids_wtask_blk_t s_wtask;                 /* Wi-Fi task */
static uint32_t        s_deauth_base;           /* Wi-Fi task: s_wev.deauth when the round started */

void ids_hook_ble_adv(const uint8_t addr[6], uint8_t type)
{
    uint32_t w = (uint32_t)(esp_timer_get_time() / (IDS_WIN_MS * 1000LL));
    ids_bm_add(s_host.slot, w, ids_fnv(addr, 6, type));
    ids_st(&s_host.n_adv, s_host.n_adv + 1u);
}

void ids_hook_link(bool up)
{
    if (up) ids_st(&s_host.link_up, s_host.link_up + 1u);
    else ids_st(&s_host.link_down, s_host.link_down + 1u);
}

void ids_hook_link_rtt(uint32_t us)
{
    uint32_t v16 = us > 10000000u ? 160000u : us * 16u / 1000u;   /* 1/16 ms, capped at 10 s */
    ids_st(&s_obd.sum16, s_obd.sum16 + v16);
    ids_st(&s_obd.n_resp, s_obd.n_resp + 1u);
}

void ids_hook_wifi_disc(int reason)
{
    if (!ids_reason_deauth(reason) || ids_ld(&s_wtask.leaving) == ids_ld(&s_wtask.rounds)) return;   /* our own leave */
    ids_st(&s_wev.deauth, s_wev.deauth + 1u);
}

void ids_hook_wifi_round(void)
{
    s_deauth_base = ids_ld(&s_wev.deauth);
    ids_st(&s_wtask.rounds, s_wtask.rounds + 1u);
}

void ids_hook_wifi_leave(void) { ids_st(&s_wtask.leaving, ids_ld(&s_wtask.rounds)); }

void ids_hook_wifi_scan_begin(void) { ids_st(&s_wtask.scan_seq, s_wtask.scan_seq + 1u); }

void ids_hook_wifi_ap(const uint8_t bssid[6]) { ids_bm_add(s_wtask.slot, s_wtask.scan_seq, ids_fnv(bssid, 6, 0)); }

void ids_hook_wifi_scan_done(void) { __atomic_store_n(&s_wtask.scans_done, s_wtask.scan_seq, __ATOMIC_RELEASE); }
/* ============ HOOKS END ============ */

/* ---------- consumers: volatile flags the worker writes, any task reads ---------- */
static volatile bool s_ble_block, s_wifi_block, s_suspect;
static volatile uint8_t s_wifi_reason;
static volatile bool s_acting_allowed;          /* past the shadow and the setting on: the inline deauth rule may act */

bool ids_ble_blocking(void) { return s_ble_block; }
bool ids_rows_suspect(void) { return s_suspect; }

/* the Wi-Fi task, before it connects and before it sets the clock: the service's verdict, or the deauth rule inline
   (one compare on the counters, so a deauth burst during this round counts before the next 5 s window) */
bool ids_wifi_refuse(const char **why)
{
    uint8_t r = 0;
    if (s_wifi_block) r = s_wifi_reason;
    else if (s_acting_allowed && ids_deauth_rule((uint16_t)(ids_ld(&s_wev.deauth) - s_deauth_base))) r = IDS_R_DEAUTH;
    if (r && why) *why = ids_reason_text(r);
    return r != 0;
}

/* ---------- the setting and the Clear (UI task writes the flag, the worker applies it) ---------- */
static volatile bool s_block_on = true;
static volatile uint32_t s_clear_req;
static volatile uint32_t s_day;                 /* board task: local day number, 0 without a clock */
bool ids_blocking_enabled(void) { return s_block_on; }
void ids_set_blocking(bool on) { s_block_on = on; }
void ids_request_clear(void) { s_clear_req = s_clear_req + 1u; }

/* ---------- worker side ---------- */
EXT_RAM_BSS_ATTR static ids_t s_g;
static uint32_t s_last_scan_w, s_last_done, s_last_rounds, s_last_resp, s_last_sum16, s_clear_seen;
static bool s_started;

static ids_result_t s_res;
static ai_seq_t s_res_seq;

/* verdict changes: worker -> board task, which writes the events (no event, log or NVS call in the step) */
#define EVQ 8
typedef struct { uint8_t code, reason; uint16_t pattern; } ids_evq_t;
static struct { uint32_t head, tail, dropped; ids_evq_t e[EVQ]; } s_evq;
static void evq_push(uint8_t code, uint8_t reason, uint16_t pattern)
{
    uint32_t h = __atomic_load_n(&s_evq.head, __ATOMIC_RELAXED), t = __atomic_load_n(&s_evq.tail, __ATOMIC_ACQUIRE);
    if (h - t >= EVQ) { s_evq.dropped++; return; }
    s_evq.e[h % EVQ] = (ids_evq_t){ code, reason, pattern };
    __atomic_store_n(&s_evq.head, h + 1, __ATOMIC_RELEASE);
}

static bool bm_pair(const ids_bm_t *slots, uint32_t w, ids_win_t *out, uint16_t *pattern)
{
    EXT_RAM_BSS_ATTR static ids_bm_t cur, prev; /* worker only: 264 B each, off the stack and out of internal RAM */
    memset(out, 0, sizeof *out);
    if (!ids_bm_copy(slots, w, &cur)) return false;
    bool have_prev = w > 0 && ids_bm_copy(slots, w - 1, &prev);
    if (!have_prev) return false;               /* the first window after a gap is only the next one's "before" */
    ids_bm_window(&cur, &prev, &out->n_adv, &out->set_bits, &out->new_bits, &out->su_new, pattern);
    out->on = out->n_adv > 0;
    return out->on;
}

static void step(int64_t release_us)
{
    if (!s_started) { ids_init(&s_g); s_started = true; }
    ids_in_t in = { 0 };
    uint8_t lv = ai_svc_level();
    in.nwin = lv == AI_DL_SLOW ? IDS_SLOW : 1;
    in.blocking_on = s_block_on;
    in.day = s_day;
    uint16_t pattern = 0;

    uint32_t w = (uint32_t)(release_us / (IDS_WIN_MS * 1000LL));
    if (w > 0 && w - 1 != s_last_scan_w) {      /* the newest complete window, once */
        s_last_scan_w = w - 1;
        bm_pair(s_host.slot, w - 1, &in.scan, &pattern);
    }
    uint32_t done = __atomic_load_n(&s_wtask.scans_done, __ATOMIC_ACQUIRE);
    if (done != s_last_done) {
        uint32_t first = done - s_last_done > 2 ? done - 1 : s_last_done + 1;
        for (uint32_t q = first; q != done + 1 && in.n_wifi < 2; q++) {
            EXT_RAM_BSS_ATTR static ids_bm_t cur, prev;
            if (!ids_bm_copy(s_wtask.slot, q, &cur)) continue;
            bool hp = ids_bm_copy(s_wtask.slot, q - 1, &prev);
            if (!ids_wifi_scan_scored(hp)) continue;   /* nothing earlier to compare with: not scored */
            ids_win_t *x = &in.wifi[in.n_wifi];
            ids_bm_window(&cur, &prev, &x->n_adv, &x->set_bits, &x->new_bits, &x->su_new, NULL);
            x->on = true;
            in.n_wifi++;
        }
        s_last_done = done;
    }
    uint32_t rounds = ids_ld(&s_wtask.rounds);
    in.round_start = rounds != s_last_rounds;
    s_last_rounds = rounds;
    in.deauth_round = (uint16_t)(ids_ld(&s_wev.deauth) - s_deauth_base);
    uint32_t n = ids_ld(&s_obd.n_resp), sum = ids_ld(&s_obd.sum16);
    in.link_n = n - s_last_resp;
    in.link_sum16 = sum - s_last_sum16;
    s_last_resp = n;
    s_last_sum16 = sum;
    uint32_t cr = s_clear_req;
    in.clear = cr != s_clear_seen;
    s_clear_seen = cr;

    uint32_t ch = ids_step(&s_g, &in);

    s_ble_block = s_g.acting && s_g.scan_block;
    s_wifi_reason = s_g.wifi_reason;
    s_wifi_block = s_g.acting && s_g.wifi_block;
    s_acting_allowed = !ids_shadow(&s_g) && s_block_on;
    s_suspect = s_g.acting || (s_g.link.flag && !ids_shadow(&s_g));
    if (ch & IDS_CH_SHADOW_END) evq_push(IDS_CH_SHADOW_END, 0, 0);
    if (ch & IDS_CH_BLOCK_ON) evq_push(IDS_CH_BLOCK_ON, s_g.reason, pattern);
    if (ch & IDS_CH_BLOCK_OFF) evq_push(IDS_CH_BLOCK_OFF, 0, 0);
    if (ch & IDS_CH_CLEARED) evq_push(IDS_CH_CLEARED, 0, 0);
    if (ch & IDS_CH_WOULD) evq_push(IDS_CH_WOULD, s_g.reason, pattern);
    if (ch & IDS_CH_LINK_ON) evq_push(IDS_CH_LINK_ON, 0, 0);
    if (ch & IDS_CH_LINK_OFF) evq_push(IDS_CH_LINK_OFF, 0, 0);

    ids_result_t r = {
        .state = s_g.state, .reason = s_g.reason, .acting = s_g.acting, .would = s_g.would, .blocking_on = s_block_on,
        .link_flag = s_g.link.flag, .win = s_g.win, .left_s = s_g.acting || s_g.would ? ids_block_left_s(&s_g) : 0,
        .blocks_today = s_g.blocks_today, .would_n = s_g.would_n, .scan_windows = s_g.scan_windows,
        .wifi_scans = s_g.wifi_scans, .n_adv = ids_ld(&s_host.n_adv), .link_resp = n, .deauth = ids_ld(&s_wev.deauth),
        .rounds = rounds,
    };
    for (int k = 0; k < IDS_NH; k++) { r.q8[k] = s_g.q8[k]; r.on[k] = s_g.on[k]; }
    AI_SEQ_PUT(&s_res_seq, &s_res, &r);
}

bool ids_read(ids_result_t *out) { return AI_SEQ_GET(&s_res_seq, out, &s_res) && out->state != IDS_ST_OFF; }

/* the first 64 rows of ids_vectors.csv through a fresh detector, plus one bitmap window (us); a mismatch refuses it */
static uint32_t selftest(void)
{
    EXT_RAM_BSS_ATTR static ids_rot_t d;
    EXT_RAM_BSS_ATTR static ids_bm_t a, b;
    int64_t t0 = esp_timer_get_time();
    ids_rot_init(&d);
    int bad = 0;
    for (int k = 0; k < 64; k++) {
        const ids_vec_t *v = &IDS_SELFTEST[k];
        ids_rot_out_t o = ids_rot_step(&d, v->n_adv, v->set_bits, v->new_bits, v->su_new);
        uint8_t f = (uint8_t)((o.exceed ? 1 : 0) | (o.block ? 2 : 0) | (o.onset ? 4 : 0));
        if (o.score_q8 != v->score_q8 || f != v->flags) bad++;
    }
    memset(&a, 0xA5, sizeof a);
    memset(&b, 0x5A, sizeof b);
    uint16_t x0, x1, x2, x3, pat;
    ids_bm_window(&a, &b, &x0, &x1, &x2, &x3, &pat);
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (bad) ESP_LOGE(TAG, "self-test: %d of 64 vectors differ", bad);
    return bad ? UINT32_MAX : us;
}

static void prepare(void)
{
    nvs_handle_t h;
    uint8_t on = 1;
    if (nvs_open("ocm_ids", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "block", &on); nvs_close(h); }
    s_block_on = on != 0;
    ESP_LOGI(TAG, "network guard: shadow %u min first, then blocking %s (Settings)", (unsigned)(IDS_SHADOW_WIN * IDS_WIN_MS / 60000u),
             s_block_on ? "on" : "off");
}

/* ---------- board task: events, the setting's NVS, the log line ---------- */
#define GEV_N 3                                 /* the Health app's guard log (HV_EV_N) */
static struct { char text[64]; int64_t t_ms; } s_gev[GEV_N];
static ai_seq_t s_gev_seq;

static void gev_add(const char *t)
{
    typeof(s_gev) nv;
    memcpy(nv, s_gev, sizeof nv);
    memmove(&nv[1], &nv[0], sizeof nv[0] * (GEV_N - 1));
    snprintf(nv[0].text, sizeof nv[0].text, "%s", t);
    nv[0].t_ms = esp_timer_get_time() / 1000;
    ai_seq_write_begin(&s_gev_seq);
    memcpy(s_gev, nv, sizeof nv);
    ai_seq_write_end(&s_gev_seq);
}

void ids_events(char ev[][64], int32_t *age_s, int n)
{
    typeof(s_gev) cp;
    EXT_RAM_BSS_ATTR static typeof(s_gev) tmp;
    memset(cp, 0, sizeof cp);
    ai_seq_read(&s_gev_seq, cp, s_gev, tmp, sizeof cp);
    int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < n; i++) {
        if (i < GEV_N && cp[i].text[0]) { memcpy(ev[i], cp[i].text, 64); ev[i][63] = 0; age_s[i] = (int32_t)((now - cp[i].t_ms) / 1000); }
        else { ev[i][0] = 0; age_s[i] = -1; }
    }
}

static void drain(void)
{
    for (;;) {
        uint32_t t = __atomic_load_n(&s_evq.tail, __ATOMIC_RELAXED), h = __atomic_load_n(&s_evq.head, __ATOMIC_ACQUIRE);
        if (t == h) break;
        ids_evq_t e = s_evq.e[t % EVQ];
        __atomic_store_n(&s_evq.tail, t + 1, __ATOMIC_RELEASE);
        char b[64];
        switch (e.code) {
        case IDS_CH_SHADOW_END:
            sys_event(EV_INFO, "Network guard: shadow over, now watching");
            snprintf(b, sizeof b, "Shadow over, now watching");
            break;
        case IDS_CH_BLOCK_ON:
            sys_event(EV_WARN, "Network guard blocking: %s (p%04x)", ids_reason_text(e.reason), e.pattern);
            snprintf(b, sizeof b, "Blocking: %s (p%04x)", ids_reason_text(e.reason), e.pattern);
            break;
        case IDS_CH_BLOCK_OFF:
            sys_event(EV_INFO, "Network guard: block lifted, 10 min clean");
            snprintf(b, sizeof b, "Block lifted, 10 min clean");
            break;
        case IDS_CH_CLEARED:
            sys_event(EV_INFO, "Network guard: block cleared by the user");
            snprintf(b, sizeof b, "Block cleared by the user");
            break;
        case IDS_CH_WOULD:
            sys_event(EV_INFO, "Network guard would block: %s (p%04x)", ids_reason_text(e.reason), e.pattern);
            snprintf(b, sizeof b, "Would block: %s (p%04x)", ids_reason_text(e.reason), e.pattern);
            break;
        case IDS_CH_LINK_ON:
            sys_event(EV_WARN, "Car link slower than usual: rows kept out");
            snprintf(b, sizeof b, "Car link slower than usual");
            break;
        case IDS_CH_LINK_OFF:
            sys_event(EV_INFO, "Car link timing normal again");
            snprintf(b, sizeof b, "Car link timing normal again");
            break;
        default: continue;
        }
        gev_add(b);
    }
}

static void board(bool shutdown)
{
    static bool saved_known;
    static bool saved_on;
    static int64_t last_log;
    drain();
    time_t now = time(NULL);
    if (now > 1767225600) {                     /* 2026-01-01: the clock is set, so "today" means something */
        struct tm tm;
        localtime_r(&now, &tm);
        s_day = (uint32_t)(tm.tm_year * 400 + tm.tm_yday + 1);
    } else s_day = 0;
    bool on = s_block_on;
    if (!saved_known) { saved_on = on; saved_known = true; }
    if (on != saved_on) {                       /* Settings changed it: saved here, the board task owns NVS */
        nvs_handle_t h;
        esp_err_t e = nvs_open("ocm_ids", NVS_READWRITE, &h);
        if (e == ESP_OK) { e = nvs_set_u8(h, "block", on ? 1 : 0); if (e == ESP_OK) e = nvs_commit(h); nvs_close(h); }
        saved_on = on;
        sys_event(EV_INFO, "Network guard blocking %s", on ? "on" : "off");
        ESP_LOGI(TAG, "blocking %s saved: %s", on ? "on" : "off", esp_err_to_name(e));
    }
    int64_t t = esp_timer_get_time();
    if (!shutdown && t - last_log >= IDS_LOG_US) {
        last_log = t;
        ids_result_t r;
        if (ids_read(&r))
            ESP_LOGI(TAG, "%s %.2f h, scan q8 %ld (%lu windows, %lu adverts), link q8 %ld%s, wifi q8 %ld (%lu scans, %lu deauth), "
                     "blocks %lu, would %lu, blocking %s", ids_state_name(r.state), r.win * (IDS_WIN_MS / 1000.0) / 3600.0,
                     (long)r.q8[IDS_H_SCAN], (unsigned long)r.scan_windows, (unsigned long)r.n_adv, (long)r.q8[IDS_H_LINK],
                     r.link_flag ? " (note)" : "", (long)r.q8[IDS_H_WIFI], (unsigned long)r.wifi_scans, (unsigned long)r.deauth,
                     (unsigned long)r.blocks_today, (unsigned long)r.would_n, r.blocking_on ? "on" : "off");
    }
}

static const ai_svc_def_t SVC = {
    .name = "ids", .period_ms = IDS_PERIOD_MS, .deadline_ms = IDS_DEADLINE_MS, .budget_us = IDS_BUDGET_US, .slow_factor = IDS_SLOW,
    .step = step, .selftest = selftest, .prepare = prepare, .board = board,
};

const ai_svc_def_t *ids_service(void) { return &SVC; }
#endif
