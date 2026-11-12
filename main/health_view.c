/* health_view: see health_view.h. Copies from the services' lock-free results; no lock, no wait, no storage. */
#include "health_view.h"
#include "sdkconfig.h"
#if CONFIG_OCM_AI_SVC
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ai_svc.h"
#include "os_core.h"
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

const char *hv_ladder_name(uint8_t os_level) { return os_ladder_name(os_level); }

void health_view_read(health_view_t *v)
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
    }
    bat_anom_last_event(v->bat_event, sizeof v->bat_event, &v->bat_event_age_s);
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
        v->block_left_s = r.left_s;
        for (int k = 0; k < HV_HEADS && k < IDS_NH; k++) { v->head_on[k] = r.on[k]; v->head_q8[k] = r.q8[k]; }
        v->blocks_today = r.blocks_today;
    }
    obd_ble_guard_counts(&v->fp_checks, &v->repair_refusals);
#if CONFIG_OCM_WIFI_CLOCK_SYNC
    v->clock_refusals = wifi_sync_clock_refusals();
#endif
    ids_events(v->guard_ev, v->guard_ev_age_s, HV_EV_N);
#endif

    /* ---- AI services ---- */
    ai_svc_status_t st;
    v->os_level = ai_svc_os_level();
    if (ai_svc_status(&st)) {
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
        }
    } else v->ai_core = -1;
}
#endif
