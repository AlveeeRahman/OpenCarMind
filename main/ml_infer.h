/* On-board model: manoeuvre recogniser + estimator (ESP-DL), fed from the OBD poll loop.
 *
 * Two models are embedded in flash and one runs at a time: CarMind E.V (the default) and, with CONFIG_CARMIND_REV_ENABLE,
 * CarMind R.E.V (the ESP-Mosaico build, esp32s31, carries R.E.V only), about half the size, which also outputs its 64-value feature z for on-board fuel learning (rev_task.h).
 * The desktop app switches between them over USB (ml_select_model); the choice is kept in NVS (namespace "ml", key "model").
 *
 * ml_feed() is called for every logged row and builds 1 s bins of the 10 pattern-layer channels.
 * ml_window_closed() is called when the pattern layer closes a 10 s window: it builds the model input from the last
 * ML_WINDOW seconds and hands it to the inference task through a message buffer, so the OBD task never waits on the
 * model. The inference task writes one line per window to sNNNNNN.ml.csv (keyed by window_seq, joins to the .pat).
 * If an input is still queued when the next one arrives, the new one is dropped and counted (soft real-time: the
 * deadline is the next window, a late answer is worth less than a fresh one later).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ML_MODEL_EV = 0, ML_MODEL_REV = 1, ML_MODEL_COUNT } ml_model_id_t;
const char *ml_model_key(int id);        /* "ev", "rev" */
const char *ml_model_name(int id);       /* "CarMind E.V", "CarMind R.E.V" */
bool ml_model_available(int id);         /* built into this firmware */
int  ml_model_active(void);              /* the selected model, ML_MODEL_EV until NVS says otherwise */
const char *ml_model_version(int id);    /* first 12 hex of the model file's sha256, "" when not built in */
const char *ml_model_target(int id);     /* the chip the model file was exported for ("esp32s3", "esp32s31") */
/* The loaded model's self-test at load (CONFIG_CARMIND_ML_SELFTEST): 1 = its outputs on the embedded test input are within
   1 LSB of ESP-PPQ's PC simulation, 0 = further off, -1 = not run. */
int  ml_selftest_state(void);
/* Switch models: unload the running one, load the other from flash, keep the choice in NVS. Writes NVS, so call it from a task
   with an internal-RAM stack (the USB door), and only while parked. If the new model does not load, the old one is loaded back
   and false is returned with the reason in why. */
bool ml_select_model(int id, char *why, size_t why_n);
/* R.E.V learning can run: R.E.V selected and loaded, with a 64-value "z" output, and not in safe mode. */
bool ml_rev_active(void);

/* What the OBD task knows about the window it is about to feed (rev_task.c): the measured fuel label for the last 5 s and the
   physics estimate for the same 5 s, both in L/h. Same task as ml_feed(), so no lock. */
typedef struct { bool label_ok; float label_lph; bool phys_ok; float phys_lph; } ml_rev_in_t;
void ml_set_rev_inputs(const ml_rev_in_t *in);

void ml_init(void);
void ml_feed(const obd_values_t *v, int64_t now_ms);
void ml_window_closed(uint32_t window_seq, int64_t start_ms, int64_t end_ms);
/* Resets the 1 s history, e.g. when a new adapter session starts. */
void ml_reset_history(void);
/* The drive ended: stop expecting model runs and drop the window history. */
void ml_drive_ended(void);
/* Free the model and load it back later (saves about 75 KB of PSRAM); safe while the task runs. Unused by default. */
void ml_unload(void);
bool ml_load(void);
/* Pause without freeing: feeding, windows and inference stop, the model stays resident (no reload, no allocation). */
void ml_set_paused(bool paused);

typedef struct {
    bool     ready;
    uint32_t windows, dropped, failed;
    uint32_t last_us, max_us;
    int      last_class;          /* index into ML_CLASSES, -1 before the first window */
    float    last_prob;
    float    last_est[3];         /* brake pressure bar, flywheel torque Nm, fuel rate L/h of the last inference */
    int64_t  last_at_ms;          /* esp_timer time of the last inference, 0 before the first */
    /* appended for R.E.V: zero-initialised for E.V */
    int      model;               /* ml_model_id_t of the loaded model */
    bool     z_ok;                /* the loaded model has a usable z output */
    bool     learned_ok;          /* learned_lph and prior_lph are from the last inference (MAF hidden when MAF was there) */
    float    learned_lph, prior_lph;
    uint32_t pass2_us;            /* the MAF-hidden second pass of the last inference, 0 when MAF was not in the window */
    uint32_t selftest_us;         /* one timed run of the loaded model at load */
} ml_stats_t;
void ml_get_stats(ml_stats_t *out);
/* Class probabilities of the last window, in ML_CLASSES order. Returns how many were written, 0 before the first window. */
int  ml_get_last_probs(float *out, int max);

#ifdef __cplusplus
}
#endif
