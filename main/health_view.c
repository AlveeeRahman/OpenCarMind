/* health_view: see health_view.h. Copies from the services' lock-free results; no lock, no wait, no storage. */
#include "health_view.h"
#include "sdkconfig.h"
#if CONFIG_OCM_AI_SVC
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include "ai_snap.h"
#include "ai_svc.h"
#include "elm327.h"
#include "events.h"
#include "health_text.h"
#include "os.h"
#include "os_core.h"
#include "status.h"
#if CONFIG_OCM_BAT_EST
#include "bat_est.h"
#endif
#if CONFIG_OCM_BAT_ANOM
#include "bat_anom.h"
#endif
#if CONFIG_OCM_IDS
#include "ids_svc.h"
#include "obd_ble.h"
#include "wifi_sync.h"
#endif

_Static_assert(AI_ST_REFUSED == 2 && AI_ST_SAFE == 3 && AI_ST_NOTASK == 4, "health_text.c: its copy of AI_ST_*");
#if CONFIG_OCM_BAT_ANOM
_Static_assert(sizeof ((bat_anom_out_t *)0)->err == sizeof ((health_view_t *)0)->anom_err, "anom_err: one per feature");
#endif
#if CONFIG_OCM_IDS
_Static_assert((int)IDS_R_SCAN == (int)HV_GR_BLE && (int)IDS_R_WIFI_ROT == (int)HV_GR_WIFI && (int)IDS_R_DEAUTH == (int)HV_GR_DEAUTH, "HV_GR_* = IDS_R_*");
#endif

const char *hv_ladder_name(uint8_t os_level) { return os_ladder_name(os_level); }

/* the recent issues (health_mon writes on the board task, the UI reads lock-free) */
typedef struct { char text[HV_RI_LEN]; int64_t t_ms; uint8_t sev; } hm_ri_t;
EXT_RAM_BSS_ATTR static hm_ri_t s_ri[HV_PAGES][HV_RI_N];
static ai_seq_t s_ri_seq;

/* everything but the lists: health_mon reads this on the board task (the lists' readers use shared scratch buffers) */
static void hv_core(health_view_t *v)
{
    memset(v, 0, sizeof *v);
    v->bat_event_age_s = -1;
    for (int i = 0; i < HV_EV_N; i++) v->guard_ev_age_s[i] = -1;
    v->soh_ref_need = 10;
    v->anom_warm_need_h = 24.0f;
    v->shadow_need_h = 2.0f;
#if CONFIG_OCM_BAT_EST
    v->soh_ref_need = BAT_SOH_NREF;
#endif
#if CONFIG_OCM_IDS
    v->shadow_need_h = IDS_SHADOW_WIN * (IDS_WIN_MS / 1000.0f) / 3600.0f;
#endif

    /* ---- battery ---- */
    ai_gauge_sample_t gs;
    if (ai_svc_gauge_read(&gs) && gs.g.ok) {
        v->v = gs.g.mv / 1000.0f;
        v->i_ma = (float)gs.g.ma;
        v->p_mw = v->v * v->i_ma;
        v->temp_c = gs.g.temp_dc / 10.0f;
        v->usb = gs.g.ma > -5;                         /* the Board page's rule: not discharging = on USB */
    }
#if CONFIG_OCM_BAT_EST
    bat_est_out_t be;
    if (bat_est_read(&be)) {
        v->bat_valid = true;
        v->bat_anchored = be.anchored;
        v->bat_mode = be.mode;
        v->soc_pct = be.soc_pct;
        v->dcir_ratio = be.n_dcir > 1 ? be.dcir_ratio : 0.0f;
        v->soh_ref_n = be.soh_ref_n;
        v->soh_src = be.soh_src == SOH_SRC_FULL ? HV_SOH_FULL : be.soh_src == SOH_SRC_MODEL ? HV_SOH_MODEL : HV_SOH_LEARNING;
        v->soh_pct = be.soh_src == SOH_SRC_FULL ? be.soh_full_pct : be.soh_src == SOH_SRC_MODEL ? be.soh_model_pct : 0.0f;
    }
#endif
#if CONFIG_OCM_BAT_ANOM
    bat_anom_out_t ao;
    if (bat_anom_read(&ao)) {
        v->anom_state = ao.state == BAN_ALARM ? HV_ANOM_ALARM : ao.state == BAN_NORMAL ? HV_ANOM_NORMAL : HV_ANOM_WARMUP;
        v->anom_warm_h = ao.warm_h;
        v->anom_warm_need_h = ao.warm_need_h;
        v->anom_score = ao.s_ae;
        v->anom_thr = ao.thr_ae;
        snprintf(v->anom_class, sizeof v->anom_class, "%s", ao.cls);
        memcpy(v->anom_err, ao.err, sizeof v->anom_err);
    }
#endif

    /* ---- network guard ---- */
#if CONFIG_OCM_IDS
    ids_result_t r;
    v->blocking_on = ids_blocking_enabled();
    if (ids_read(&r)) {
        static const uint8_t ST[5] = { HV_IDS_OFF, HV_IDS_LEARNING, HV_IDS_SHADOW, HV_IDS_WATCHING, HV_IDS_BLOCKING };
        v->ids_state = r.state < 5 ? ST[r.state] : HV_IDS_OFF;
        v->blocking_on = r.blocking_on;
        v->shadow_h = fminf((float)r.win, (float)IDS_SHADOW_WIN) * (IDS_WIN_MS / 1000.0f) / 3600.0f;
        if (r.acting || r.would) snprintf(v->block_reason, sizeof v->block_reason, "%s", ids_reason_text(r.reason));
        v->block_why = r.acting || r.would ? r.reason : HV_GR_NONE;
        v->ids_would = r.would;
        v->link_note = r.link_flag;
        v->block_left_s = r.left_s;
        for (int k = 0; k < HV_HEADS && k < IDS_NH; k++) { v->head_on[k] = r.on[k]; v->head_q8[k] = r.q8[k]; }
        v->blocks_today = r.blocks_today;
    }
    obd_ble_guard_counts(&v->fp_checks, &v->repair_refusals);
    v->adapter_mismatch = obd_ble_adapter_mismatch();
#if CONFIG_OCM_WIFI_CLOCK_SYNC
    v->clock_refusals = wifi_sync_clock_refusals();
#endif
#endif

    /* ---- AI services ---- */
    ai_svc_status_t st;
    v->os_level = ai_svc_os_level();
    if (ai_svc_status(&st)) {
        v->ai_started = true;
        v->ai_n = st.n;
        v->ai_core = st.core;
        v->ai_level = st.level;
        snprintf(v->ai_why, sizeof v->ai_why, "%s", st.why);
        for (int i = 0; i < st.n && i < AI_SVC_MAX; i++) {
            snprintf(v->ai[i].name, sizeof v->ai[i].name, "%s", st.s[i].name);
            v->ai[i].state = st.s[i].state;
            v->ai[i].runs = st.s[i].done;
            v->ai[i].misses = st.s[i].missed;
            v->ai[i].skipped = st.s[i].skipped;
            v->ai[i].wcrt_us = st.s[i].wcrt_us;
            v->ai[i].budget_us = st.s[i].budget_us;
            v->ai[i].period_ms = st.s[i].period_ms;
            v->ai[i].deadline_ms = st.s[i].deadline_ms;
            v->ai[i].step_us = st.s[i].max_step_us;
        }
    } else v->ai_core = -1;
}

void health_view_read(health_view_t *v)
{
    hv_core(v);
#if CONFIG_OCM_BAT_ANOM
    bat_anom_last_event(v->bat_event, sizeof v->bat_event, &v->bat_event_age_s);
#endif
#if CONFIG_OCM_IDS
    ids_events(v->guard_ev, v->guard_ev_age_s, HV_EV_N);
#endif
    EXT_RAM_BSS_ATTR static hm_ri_t cp[HV_PAGES][HV_RI_N], tmp[HV_PAGES][HV_RI_N];   /* the UI task only */
    if (!ai_seq_read(&s_ri_seq, cp, s_ri, tmp, sizeof cp)) return;
    int64_t now = esp_timer_get_time() / 1000;
    for (int p = 0; p < HV_PAGES; p++)
        for (int i = 0; i < HV_RI_N; i++) {
            memcpy(v->recent[p][i].text, cp[p][i].text, HV_RI_LEN);
            v->recent[p][i].text[HV_RI_LEN - 1] = 0;
            v->recent[p][i].age_s = cp[p][i].text[0] ? (int32_t)((now - cp[p][i].t_ms) / 1000) : -1;
            v->recent[p][i].sev = cp[p][i].sev;
        }
}

/* ---------- health_mon: the board task ---------- */
static volatile uint8_t s_worst;
static struct { char text[64]; uint8_t page, sev, issue; } s_ban;     /* the banner health_mon raised last */
static ai_seq_t s_ban_seq;

uint8_t health_mon_worst(void) { return s_worst; }

int health_mon_banner(const char *alarm_text, uint8_t *sev)
{
    typeof(s_ban) b;
    if (!alarm_text || !alarm_text[0] || !AI_SEQ_GET(&s_ban_seq, &b, &s_ban) || !b.text[0] || strcmp(b.text, alarm_text)) return -1;
    if (sev) *sev = b.sev;
    return b.page;
}

static void ri_add(int page, const char *text, uint8_t sev)
{
    ai_seq_write_begin(&s_ri_seq);
    memmove(&s_ri[page][1], &s_ri[page][0], sizeof s_ri[page][0] * (HV_RI_N - 1));
    snprintf(s_ri[page][0].text, HV_RI_LEN, "%s", text);
    s_ri[page][0].t_ms = esp_timer_get_time() / 1000;
    s_ri[page][0].sev = sev;
    ai_seq_write_end(&s_ri_seq);
}

static int now_minutes(void)
{
    time_t t = time(NULL);
    if (t < 1767225600) return -1;                 /* 2026-01-01: no clock yet */
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

/* the issues that may raise the banner (an AI refusal is shown in the app and logged only) */
#define HM_BANNER ((1u << HI_BAT_WORN) | (1u << HI_BAT_ANOM) | (1u << HI_GUARD_BLOCK) | (1u << HI_ADAPTER))

void health_mon_tick(void)
{
    EXT_RAM_BSS_ATTR static health_view_t v;
    static uint32_t active;
    hv_core(&v);
    if (!v.ai_started) return;                     /* nothing is judged before the services run */
    uint8_t worst;
    uint32_t m = hs_issues(&v, &worst);
    s_worst = worst;
    uint32_t on = m & ~active, off = active & ~m;
    active = m;
    if (!on && !off) return;
    int now_min = now_minutes();
    bool driving = os_runlevel() == OS_RL_DRIVING || elm_drive_active();
    for (int k = 0; k < HI_N; k++) {
        if (off & (1u << k)) {
            ri_add(hs_issue_page(k), hs_issue_over(k), HS_GOOD);
            STATUS_LOCK();                         /* its banner, if still up, goes with it */
            if (s_ban.issue == k && s_ban.text[0] && !strcmp(g_status.alarm_text, s_ban.text)) {
                g_status.alarm_text[0] = 0;
                g_status.alarm_critical = false;
            }
            STATUS_UNLOCK();
        }
        if (!(on & (1u << k))) continue;
        hs_card_t c;
        if (!hs_issue_card(&v, k, now_min, &c)) continue;
        ri_add(hs_issue_page(k), c.head, c.sev);
        if (k == HI_BAT_WORN) sys_event(c.sev == HS_ACT ? EV_WARN : EV_INFO, "Health: %s", c.head);   /* the others log their own */
        if (!(HM_BANNER & (1u << k)) || (driving && c.sev < HS_ACT)) continue;   /* amber while driving: the log only */
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool shown = false;
        STATUS_LOCK();
        if (!g_status.alarm_text[0]) {             /* a car fault on the banner stays */
            snprintf(g_status.alarm_text, sizeof g_status.alarm_text, "%s", c.head);
            g_status.alarm_critical = true;
            g_status.alarm_until_ms = driving ? now_ms + 10000 : INT64_MAX;   /* red while driving: 10 s, never in the way */
            shown = true;
        }
        STATUS_UNLOCK();
        if (shown) {
            ai_seq_write_begin(&s_ban_seq);
            snprintf(s_ban.text, sizeof s_ban.text, "%s", c.head);
            s_ban.page = (uint8_t)hs_issue_page(k);
            s_ban.sev = c.sev;
            s_ban.issue = (uint8_t)k;
            ai_seq_write_end(&s_ban_seq);
        }
    }
}
#endif
