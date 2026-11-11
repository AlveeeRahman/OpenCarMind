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
/* True when everything written so far is on the card, so the power can be cut now. False between a write and the next
   save (at most SDLOG_SYNC_PERIOD_S seconds) and when there is no card. Reads one flag: safe to call from the display. */
bool      sdlog_saved(void);
int64_t   sdlog_ms_since_write(void);   /* since the last row or file write, INT64_MAX before any */
/* Flush everything to the card now and raise "Safe to turn off" (the car link dropped, or a periodic save). */
void      sdlog_sync_now(void);
/* Ends the current drive: everything is saved and the session files are closed. No-op when nothing was logged yet. */
void      sdlog_end_drive(void);
/* Opens a new session if the last drive was ended. Called when the next drive starts without a reboot. */
void      sdlog_begin_drive(void);
uint32_t  sdlog_session_id(void);

/* Append one CSV row built from the current OBD values. */
void sdlog_write_row(const obd_values_t *v);

/* Rewrite the session metadata JSON. Any argument may be NULL to keep the previous value. */
void sdlog_meta_set_adapter(const char *name, const char *addr, const char *ati, const char *batch);
void sdlog_meta_set_vehicle(const char *vin, const char *protocol);
/* Which model ran this drive ("ev", "rev") and the fill-up calibration factor at its start. */
void sdlog_meta_set_ml(const char *model_key, float fuel_cal);
void sdlog_meta_set_supported(const char *pid_cmd, const char *bitmap_hex);
void sdlog_meta_set_time_sync(int64_t epoch_ms, int64_t uptime_ms);
void sdlog_meta_add_dtcs(int64_t uptime_ms, const char *dtcs);
void sdlog_meta_set_faults(const char *insight, const char *ranked_codes, uint8_t max_severity);
void sdlog_meta_set_custom(size_t idx, const char *cmd, const char *name, const char *unit, const char *formula, const char *header);
/* Sets (or replaces) one top-level key of the session JSON to a JSON value given as text; invalid JSON is ignored. */
void sdlog_meta_set_json(const char *key, const char *json);

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

/* Drive-log file name: "s" + 6 digits + a suffix the logger writes (.csv, .ml.csv, .pat, .raw, .json,
   and .sum.json on the ESP-Mosaico). */
bool session_name_valid(const char *name);

/* Delete drive logs (every sNNNNNN.<known suffix> file in SDLOG_SESSIONS_DIR, nothing else) under the logger's own lock,
   so a delete never races a save. The current session is refused. Each delete is a sys_event. Returns the number of
   files removed, -1 when refused (current session, no storage). sdlog_delete_all_sessions keeps the current session. */
int  sdlog_delete_session(uint32_t id);
int  sdlog_delete_all_sessions(int *sessions_out);

/* Boards with the log in a flash FAT (ESP-Mosaico): format it under the logger's lock. Every plain file directly in
   SDLOG_DIR (learned R.E.V state, clock.txt, config.json, wifi.csv, ...; at most 512 KB in all) is copied to PSRAM
   first and written back; drive logs (sessions/) and drives.csv are erased. Refused while a session is open for rows.
   Returns ESP_OK and the free KB after, or an error with the reason in why. */
esp_err_t sdlog_format_storage(uint32_t *free_kb, int *kept_files, char *why, size_t why_n);

/* Flush and unmount before deep sleep. */
void sdlog_prepare_sleep(void);
