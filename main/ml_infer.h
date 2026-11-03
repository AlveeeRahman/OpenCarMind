/* On-board model: manoeuvre recogniser + estimator (ESP-DL), fed from the OBD poll loop.
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
#include "status.h"

#ifdef __cplusplus
extern "C" {
#endif

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
} ml_stats_t;
void ml_get_stats(ml_stats_t *out);
/* Class probabilities of the last window, in ML_CLASSES order. Returns how many were written, 0 before the first window. */
int  ml_get_last_probs(float *out, int max);

#ifdef __cplusplus
}
#endif
