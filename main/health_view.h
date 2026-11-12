/* health_view: everything the Health app shows, in one plain struct (OpenCarMind EdgeOS 2.1.0, ESP-Mosaico).
 * health_view_read() fills it from the AI services' lock-free results (bat_est, bat_anom, ids_svc, ai_svc), so the
 * Health app's update() reads one copy and never waits. The host renderer (tests/host/uishot) has its own demo copy.
 * Read-only: the only action is the guard's "Clear block" (ids_request_clear), which sets a flag the IDS step reads. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ai_svc_core.h"

enum { HV_SOH_NONE = 0, HV_SOH_LEARNING, HV_SOH_MODEL, HV_SOH_FULL };          /* where the SoH figure comes from */
enum { HV_ANOM_OFF = 0, HV_ANOM_WARMUP, HV_ANOM_NORMAL, HV_ANOM_ALARM };
enum { HV_IDS_OFF = 0, HV_IDS_LEARNING, HV_IDS_SHADOW, HV_IDS_WATCHING, HV_IDS_BLOCKING };
enum { HV_HEAD_SCAN = 0, HV_HEAD_LINK, HV_HEAD_WIFI, HV_HEADS };
#define HV_EV_N   3
#define HV_EV_LEN 64

typedef struct {
    /* ---- page 1: battery ---- */
    bool     bat_valid;            /* the estimator has run */
    bool     bat_anchored;         /* false: SoC still "learning" */
    uint8_t  bat_mode;             /* BAT_MODE_* (rest, discharge, charge, CV) */
    bool     usb;                  /* powered from USB (current >= 0): the current is labelled "charging" */
    float    soc_pct, i_ma, p_mw, temp_c, v;
    uint8_t  soh_src;              /* HV_SOH_* */
    float    soh_pct;              /* valid when soh_src is MODEL or FULL */
    uint8_t  soh_ref_n, soh_ref_need;   /* reference charges learnt (n of 10) */
    float    dcir_ratio;           /* resistance now / first (1.0 = new); 0 = no samples yet */
    uint8_t  anom_state;           /* HV_ANOM_* */
    float    anom_warm_h, anom_warm_need_h;   /* warm-up done of 24 h */
    float    anom_score, anom_thr; /* autoencoder score and its alarm threshold */
    char     anom_class[32];       /* the fault class while alarmed ("resistance rise", "self-discharge", ...) */
    char     bat_event[HV_EV_LEN]; /* the last battery event, "" for none */
    int32_t  bat_event_age_s;      /* -1 for none */

    /* ---- page 2: network guard ---- */
    uint8_t  ids_state;            /* HV_IDS_* */
    bool     blocking_on;          /* Settings > Network guard: blocking */
    float    shadow_h, shadow_need_h;      /* shadow done of 2 h */
    char     block_reason[40];     /* while blocking: "radio flood nearby", "Wi-Fi deauth", ... */
    uint32_t block_left_s;         /* while blocking: until it lifts by itself */
    bool     head_on[HV_HEADS];    /* the head has data now (scan running, link up, a round ran) */
    int32_t  head_q8[HV_HEADS];    /* score, Q8: 256 = the threshold */
    uint32_t fp_checks, repair_refusals, clock_refusals, blocks_today;
    char     guard_ev[HV_EV_N][HV_EV_LEN];     /* newest first, "" for none */
    int32_t  guard_ev_age_s[HV_EV_N];

    /* ---- page 3: AI services ---- */
    uint8_t  ai_n;                 /* services in the table */
    int8_t   ai_core;              /* -1: no worker */
    uint8_t  ai_level;             /* AI_DL_* */
    uint8_t  os_level;             /* the OS ladder rung */
    char     ai_why[72];
    struct {
        char     name[8];
        uint8_t  state;            /* AI_ST_* (ai_svc.h) */
        uint32_t runs, misses, skipped, wcrt_us, budget_us, period_ms;
    } ai[AI_SVC_MAX];
} health_view_t;

void health_view_read(health_view_t *out);     /* any task; never blocks */
const char *hv_ladder_name(uint8_t os_level);  /* "normal", "AI slowed", "UI slowed", ... */

/* the guard's controls (ids_svc.c): flags only, the IDS step applies them */
void ids_request_clear(void);                  /* the Health app's "Clear block" */
bool ids_blocking_enabled(void);               /* Settings > Network guard: blocking (default on) */
void ids_set_blocking(bool on);                /* applied at once, saved in NVS by the board task */
