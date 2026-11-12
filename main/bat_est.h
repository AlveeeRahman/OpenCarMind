/* bat_est: the battery SoC/SoH estimator as an OpenCarMind EdgeOS AI service (CONFIG_OCM_BAT_EST). The math is bat_est_core.c;
 * this is the glue: the gauge sample in (read-only, the board task's existing 2 s poll), the result out for the UI,
 * the learnt values in NVS ("ocm_bat"/"est", at most every 10 minutes and on the safe-shutdown path), and the battery
 * log for training data (CONFIG_OCM_BAT_LOG). The gauge is never written: no data flash, no control commands.
 * 2.1.0: the SoH regressor's charge features (bat_soh.h) run in the same step; its reference cycles and the full-ends
 * count are kept in NVS ("ocm_bat"/"soh") with the same 10-minute rule. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "ai_svc.h"
#include "bat_est_core.h"
#include "bat_soh.h"

/* the filter's inputs and outputs of the newest step, for the anomaly detector (bat_anom.c): one writer (the bat step) */
typedef struct {
    float    v, i, t_c, dt, innov, ocv_z, v1;
    int      valid;
    uint32_t n;                     /* counts the steps; 0 = none yet */
} bat_filt_t;

typedef struct {
    bool     valid;                 /* at least one step ran */
    bool     anchored;              /* false: "learning" (no OCV or full-charge anchor yet) */
    bool     placeholder;           /* bat_model.h is the placeholder */
    uint8_t  mode;                  /* BAT_MODE_* */
    float    soc_pct, soh_pct, dcir_ratio, iavg_ma;  /* soc_pct: the device window (soc_user) */
    uint32_t steps, log_rows, log_dropped, log_skipped;
    /* the SoH regressor (bat_soh.h) */
    uint8_t  soh_src;               /* SOH_SRC_*: full-ends count, the partial-cycle model, or learning */
    float    soh_model_pct, soh_full_pct;            /* 0 before one */
    uint8_t  soh_ref_n;             /* reference cycles learnt, of BAT_SOH_NREF */
    uint32_t soh_cycles, soh_usable;
    uint32_t n_dcir;
} bat_est_out_t;

#if CONFIG_OCM_BAT_EST
const ai_svc_def_t *bat_est_service(void);
bool bat_est_read(bat_est_out_t *out);       /* any task, lock-free; false before the first step */
bool bat_est_filter(bat_filt_t *out);        /* any task, lock-free; false before the first step */
#if CONFIG_OCM_BAT_LOG
#include "nand_store.h"
#define BAT_LOG_DIR NAND_LOG_DIR "/bat"
/* the battery log's file names, bNNNNNN.csv (the USB door's LIST and GET) */
bool bat_log_name_valid(const char *name);
#endif
#endif
