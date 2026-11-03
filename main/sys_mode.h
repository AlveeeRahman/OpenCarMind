/* System modes: what the board is doing.
 *
 *   SEEK   no drive: BLE scans for the adapter or waits for a quiet ECU, the model is loaded and ready. Wi-Fi may run its short clock sync.
 *   DRIVE  connecting to the adapter, or the ECU is answering and rows are logged: the OBD poll and the model run. Wi-Fi scans and connects are held off so
 *          the shared 2.4 GHz radio and core 0 belong to the OBD link.
 *
 * DRIVE always wins: logging is the board's job, the clock sync can wait.
 */
#pragma once
#include <stdbool.h>

typedef enum { MODE_SEEK = 0, MODE_DRIVE } sys_mode_t;

void        sys_mode_start(void);
sys_mode_t  sys_mode_get(void);
const char *sys_mode_name(sys_mode_t m);
