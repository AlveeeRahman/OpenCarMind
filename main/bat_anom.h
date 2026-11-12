/* bat_anom: the battery anomaly detector as an OpenCarMind EdgeOS AI service (CONFIG_OCM_BAT_ANOM, ESP-Mosaico). The
 * math is bat_anom_core.c with bat_anom_model.h; this is the glue. Input: the estimator's newest sample and filter
 * outputs (bat_est_filter, a single-writer snapshot). Output: a lock-free result for the Health app. On an alarm (the
 * autoencoder, 3 windows in a row after the 24 h warm-up) the board task writes an event and shows the banner; the
 * detector never acts on power. Its statistics are kept in NVS ("ocm_bat"/"anom") every 10 minutes and on the safe-shutdown
 * path, so the warm-up and the learnt normal survive a restart. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "ai_svc.h"

enum { BAN_OFF = 0, BAN_WARMUP, BAN_NORMAL, BAN_ALARM };
typedef struct {
    uint8_t  state;                 /* BAN_* */
    float    warm_h, warm_need_h;   /* warm-up hours done, of 24 */
    float    s_ae, thr_ae, s_rz, s_g;
    char     cls[32];               /* the fault class while alarmed */
    uint32_t windows, alarms;
} bat_anom_out_t;

#if CONFIG_OCM_BAT_ANOM
const ai_svc_def_t *bat_anom_service(void);
bool bat_anom_read(bat_anom_out_t *out);     /* any task, lock-free */
/* the last battery event (board task writes it): "" for none, age in s */
void bat_anom_last_event(char *text, int n, int32_t *age_s);
#endif
