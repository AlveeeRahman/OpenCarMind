/* health_text: the Health app's plain words (OpenCarMind EdgeOS 2.1.0, ESP-Mosaico). Pure C, no LVGL, no IDF: host-tested
 * in tests/host/test_health_text.c. From one health_view_t it gives each page ONE status card (a severity, a short word,
 * a headline, one line "what it means", one line "what to do"), the list of active issues for the header count, the
 * Home icon dot and the banner, and the anomaly detector's reason in plain words.
 *
 * Severities: GOOD (green, nothing to do), INFO (blue: learning, starting, slowed on purpose; not an issue), CHECK
 * (amber: check soon), ACT (red: act now). Only CHECK and ACT count as issues.
 *
 * Thresholds that come from the firmware or the owner, not from this file's guesses:
 *   battery worn   the capacity estimate below 80 % of rated (the usual end-of-life mark), below 70 % = act now
 *   anomaly        the detector's own alarm (bat_anom.c: the autoencoder, 3 windows in a row, after the 24 h warm-up);
 *                  the reason is the feature with the largest reconstruction error at the alarm's onset window
 *   guard          the network guard's own states (ids_core.h); the adapter question is obd_ble.c's fingerprint rule R1
 *   AI             the scheduler's own counters (deadline misses, refusals, the OS ladder rung) */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "health_view.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { HS_GOOD = 0, HS_INFO, HS_CHECK, HS_ACT };
enum { HS_P_BAT = 0, HS_P_GUARD, HS_P_AI, HS_PAGES };
/* the issues (each one a bit in hs_issues' mask) */
enum { HI_BAT_WORN = 0, HI_BAT_ANOM, HI_GUARD_BLOCK, HI_ADAPTER, HI_AI_OFF, HI_N };
#define HI_NONE 0xFF
/* the anomaly detector's reasons, from its 9 features (bat_anom_core.c an_step: f0..f8) */
enum { AR_DRIFT = 0, AR_SELFDIS, AR_LEAK, AR_HEAT, AR_RES, AR_LOAD, AR_GLITCH, AR_N };

#define HS_HEAD 64
#define HS_LINE 64
typedef struct {
    uint8_t sev;                 /* HS_* */
    uint8_t issue;               /* HI_* shown, or HI_NONE */
    char    word[16];            /* the severity in words: "Good", "Learning", "Check soon", "Act now", ... */
    char    head[HS_HEAD];       /* "Battery is healthy" */
    char    mean[HS_LINE];       /* "Holds 98 % of its rated charge" */
    char    act[HS_LINE];        /* "Nothing to do" */
} hs_card_t;

/* now_min: minutes since local midnight, -1 without a clock (then "for 10 more min" instead of "until 14:12") */
void        hs_card(const health_view_t *v, int page, int now_min, hs_card_t *c);
/* one issue's card (used by hs_card, the banner and the recent list); false if the issue is not active */
bool        hs_issue_card(const health_view_t *v, int issue, int now_min, hs_card_t *c);
/* the active issues as a mask of 1 << HI_*; *worst = the highest severity among them (HS_GOOD for none) */
uint32_t    hs_issues(const health_view_t *v, uint8_t *worst);
int         hs_issue_count(uint32_t mask);
int         hs_issue_page(int issue);
/* the header: "All good", "1 thing to check", "2 things to check" */
void        hs_header(uint32_t mask, char *b, size_t n);
/* the recent-list line when an issue ends ("Battery behaves normally again") */
const char *hs_issue_over(int issue);
const char *hs_sev_word(uint8_t sev);

/* the anomaly reason: the feature with the largest diagnostic score (per-feature squared reconstruction error) */
int         hs_anom_reason(const float err[HV_ANOM_NF]);
const char *hs_anom_reason_text(int r);       /* "warms up more than usual" */

/* "1,000": thousands separators for the details */
void        hs_thousands(char *b, size_t n, uint32_t v);

#ifdef __cplusplus
}
#endif
