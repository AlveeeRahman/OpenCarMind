#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "status.h"

#define SDLOG_DIR          CONFIG_SDLOG_MOUNT_POINT "/obd"
#define SDLOG_SESSIONS_DIR SDLOG_DIR "/sessions"

esp_err_t sdlog_init(void);
bool      sdlog_ok(void);
/* Flush everything to the card now and raise "Safe to turn off" (the car link dropped, or a periodic save). */
void      sdlog_sync_now(void);
uint32_t  sdlog_session_id(void);

/* Append one CSV row built from the current OBD values. */
void sdlog_write_row(const obd_values_t *v);

/* Rewrite the session metadata JSON. Any argument may be NULL to keep the previous value. */
void sdlog_meta_set_adapter(const char *name, const char *addr, const char *ati, const char *batch);
void sdlog_meta_set_vehicle(const char *vin, const char *protocol);
void sdlog_meta_set_supported(const char *pid_cmd, const char *bitmap_hex);
void sdlog_meta_set_time_sync(int64_t epoch_ms, int64_t uptime_ms);
void sdlog_meta_add_dtcs(int64_t uptime_ms, const char *dtcs);
void sdlog_meta_set_faults(const char *insight, const char *ranked_codes, uint8_t max_severity);
void sdlog_meta_set_custom(size_t idx, const char *cmd, const char *name, const char *unit, const char *formula, const char *header);

/* Raw hex of a supported PID the firmware does not decode: appended to sNNNNNN.raw as "uptime_ms,01XX,HEX". */
void sdlog_write_raw(uint8_t pid, const char *hex);

/* Pattern layer files: one line per closed window in sNNNNNN.pat; the k-means model in /sdcard/obd/patterns.json. */
void sdlog_write_pattern_window(uint32_t window_seq, int64_t start_ms, int64_t end_ms, uint16_t samples, int pattern_id,
                                float dist, float novelty, bool spawned, const char *sax_rpm, const char *sax_speed,
                                const float *features, int nfeatures);
void sdlog_set_window_seq(uint32_t seq);
/* On-board model outputs: one line per window in sNNNNNN.ml.csv; header is written when the file is first opened. */
void sdlog_write_ml_line(const char *header, const char *line);         /* value written into the CSV window_seq column */
bool sdlog_load_text(const char *path, char *buf, size_t n);
bool sdlog_save_text_atomic(const char *path, const char *text);

/* Flush and unmount before deep sleep. */
void sdlog_prepare_sleep(void);
