/* bat_est: the battery SoC/SoH estimator as an EdgeOS AI service (CONFIG_OCM_BAT_EST). The math is bat_est_core.c;
 * this is the glue: the gauge sample in (read-only, the board task's existing 2 s poll), the result out for the UI,
 * the learnt values in NVS ("ocm_bat"/"est", at most every 10 minutes and on the safe-shutdown path), and the battery
 * log for training data (CONFIG_OCM_BAT_LOG). The gauge is never written: no data flash, no control commands. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "ai_svc.h"
#include "bat_est_core.h"

typedef struct {
    bool     valid;                 /* at least one step ran */
    bool     anchored;              /* false: "learning" (no OCV or full-charge anchor yet) */
    bool     placeholder;           /* bat_model.h is the placeholder */
    uint8_t  mode;                  /* BAT_MODE_* */
    float    soc_pct, soh_pct, dcir_ratio, iavg_ma;  /* soc_pct: the device window (soc_user) */
    uint32_t steps, log_rows, log_dropped, log_skipped;
} bat_est_out_t;

#if CONFIG_OCM_BAT_EST
const ai_svc_def_t *bat_est_service(void);
bool bat_est_read(bat_est_out_t *out);       /* any task, lock-free; false before the first step */
#if CONFIG_OCM_BAT_LOG
#include "nand_store.h"
#define BAT_LOG_DIR NAND_LOG_DIR "/bat"
/* the battery log's file names, bNNNNNN.csv (the USB door's LIST and GET) */
bool bat_log_name_valid(const char *name);
#endif
#endif
