/* ESP-Mosaico drive-log browser, worker side (ESP32-S31 only). The Files app never touches FatFs: it asks the board
 * task (mosaico_board_request) to scan, read one session's details or delete, and reads the published results here
 * (a mutex held only for the copy; gen changes when a result is new). Deletes go through sdlog_delete_session(), which
 * takes the logger's own lock, refuses the session being written and touches only sNNNNNN.* files in sessions/. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MF_MAX 48                      /* newest sessions listed */
#define MF_FILES 12

typedef struct {
    uint32_t id, bytes, rows;
    int64_t  start_epoch_ms;           /* 0 = the board had no clock */
    int16_t  model_year;               /* 0 = unknown */
    uint8_t  nfiles;
    bool     current;                  /* being written: shown, never deleted */
} mf_session_t;

typedef struct {
    uint32_t     gen;
    bool         ok, busy;
    int          n, total;             /* listed, and all sessions found */
    uint32_t     free_kb, total_kb;
    char         note[64];             /* result of the last delete */
    mf_session_t s[MF_MAX];
} mf_list_t;

typedef struct {
    uint32_t gen, id;
    bool     ok, current;
    int64_t  start_epoch_ms, first_up_ms, last_up_ms;
    float    km, fuel_l;               /* < 0 = unknown */
    uint32_t rows;
    int16_t  model_year;
    char     protocol[24], adapter[32];
    int      nf;
    struct { char name[24]; uint32_t bytes; } f[MF_FILES];
} mf_detail_t;

void mosaico_files_get_list(mf_list_t *out);
void mosaico_files_get_detail(mf_detail_t *out);
/* board task only */
void mosaico_files_scan(void);
void mosaico_files_detail(uint32_t id);
void mosaico_files_delete(uint32_t id);
void mosaico_files_note(const char *note);      /* shown on top of the list */          /* id 0 = every session but the current one */
