/* CarMind R.E.V on the board: the glue between the OBD task, the model task, the USB door and the learners in rev_learn.h and
 * rev_cal.h.
 *
 *   obd task (core 1, prio 5)  rev_estimates(): physics and learned estimates and which one to trust, before econ picks a source
 *                              rev_row(): label ring, airflow constant, calibration ledger, after econ; hands the label to ml_feed
 *   ml task  (core 1, prio 3)  second, MAF-hidden pass; rev_predict() and rev_pair() hand (phi, label) to the rev task
 *   rev task (core 1, prio 2)  folds pairs, solves every 30, scores the estimates, saves the state; job "rev" in sched_mon with a
 *                              2 % CPU reservation (sched_cpu_available before each bounded step)
 *   usb_door (internal stack)  REV-STATUS, REV-FILLUPS, REV-RESET; writes the calibration factor to NVS
 *
 * Learning (fuel head, physics) runs only while CarMind R.E.V is the selected model. Fill-up calibration applies to both models.
 * In safe mode nothing learns, the state file is not opened, and the calibration factor comes read-only from NVS. */
#pragma once
#include "rev_tank.h"
#include "rev_torque.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "status.h"
#include "econ.h"
#include "rev_learn.h"
#include "rev_cal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* After NVS, the SD card and ml_init(). */
void  rev_init(void);
/* The selected model changed (ml_select_model): learning on or off. */
void  rev_model_changed(bool rev_active);

/* ---- obd task ---- */
typedef struct { bool phys_ok; float phys_maf_gps; bool learned_ok; float learned_lph; fuel_src_t prefer; } rev_est_t;
void  rev_drive_started(void);
void  rev_estimates(const obd_values_t *row, rev_est_t *out);
/* After econ picked raw_lph from src (negative = unknown): labels, physics, ledger. dt_s since the last row, 0 for the first. */
void  rev_row(const obd_values_t *row, int64_t now_ms, fuel_src_t src, float raw_lph, float dt_s, bool level_fresh);
/* After rev_row: torque (NAN when none; *idle while the car stands) and the tank figures (negative when not ready). */
void  rev_row_outputs(float *torque_nm, uint8_t *torque_src, bool *torque_idle, float *tank_left_l, float *tank_range_km);
void  rev_drive_ended(void);
/* Save now and wait for it (at most timeout_ms): before deep sleep, after a fill-up or a reset. Not from the rev task. */
void  rev_flush(uint32_t timeout_ms);
/* Poll absolute load (PID 0x43) in every cycle: R.E.V is active and the physics learner is built in. */
bool  rev_wants_abs_load(void);
/* The fill-up calibration factor, 1.0 without fill-ups. Any task. */
float rev_cal_factor(void);

/* ---- ml task ---- */
/* Live and factory head on phi = [z, 1], standardised units. False when learning is off. */
bool  rev_predict(const float phi[REV_NF], float *prior_std, float *learned_std);
/* Queue a labelled pair for the rev task; never blocks (a full queue drops the pair and counts it). */
void  rev_pair(const float phi[REV_NF], float y_std, float label_lph, float prior_lph, float learned_lph, bool phys_ok, float phys_lph);

/* ---- status, any task ---- */
typedef struct {
    bool     enabled, learning, safe, saved, sd;
    uint32_t commits, resets, kept, failed, learned, pairs, dropped, saves;
    float    drift, mse_live, mse_prior;
    float    err[3]; uint32_t n_err[3];        /* running squared error, L/h^2: factory, learned, physics */
    rev_src_t prefer;
    float    k; bool k_ready; uint32_t k_rows; bool k_calc;
    float    cal; uint32_t fill_rev; int used, skipped, fills;
    double   life_l, life_km;
    float    row_ms;                           /* mean row period of this drive, for the 0x43 polling cost */
    rev_tank_out_t tank; uint32_t tank_segs, tank_fills, tank_sug_n, tank_sug_seq;
    rev_tank_fill_t tank_sug[REV_TANK_SUGGEST];
    float    torque_nm, vd_l; uint8_t torque_src; bool torque_ok, torque_idle;
} rev_status_t;
void  rev_get_status(rev_status_t *out);
/* One interval of the fill-up calculation, for REV-STATUS. Returns false past the last one. */
bool  rev_get_interval(int i, rev_interval_t *out);

/* ---- usb_door task ---- */
/* Replace the fill-up list and recompute c; writes NVS and asks for a save. false with the reason in why. */
bool  rev_set_fills(const rev_fill_t *fills, int n, uint32_t fill_rev, char *why, size_t why_n);
/* "fuel", "phys", "cal" or "all": back to the factory values for that part, then save. */
bool  rev_reset(const char *what);
/* The app has taken the tank's fill-up suggestions up to and including seq. */
void  rev_tank_suggestions_ack(uint32_t seq);

#ifdef __cplusplus
}
#endif
