/* ids_svc: the network guard as an OpenCarMind EdgeOS AI service (CONFIG_OCM_IDS, ESP-Mosaico only). The rules are
 * ids_core.h, the hook data ids_hooks.h. This header is included by the radio code on both boards: without
 * CONFIG_OCM_IDS (the S3) every hook and consumer is an empty inline, so the S3 image does not change.
 *
 * Hooks (radio callbacks, each in its own writer task; they only count and set bitmap bits):
 *   ids_hook_ble_adv      obd_ble.c BLE_GAP_EVENT_DISC, every advert           NimBLE host task
 *   ids_hook_link         obd_ble.c CONNECT (up) and DISCONNECT (down)        NimBLE host task
 *   ids_hook_link_rtt     elm327.c, each mode 01 request's round trip          OBD task
 *   ids_hook_wifi_disc    wifi_sync.c STA_DISCONNECTED, its reason            esp_event task
 *   ids_hook_wifi_round / _scan_begin / _ap / _scan_done / _leave              Wi-Fi task (wifi_sync.c sync_once)
 * Consumers (poll the verdict, never wait; the IDS calls none of them):
 *   ids_ble_blocking      obd_ble.c: no pairing offers and no re-pair, the scan narrowed to the paired adapter
 *   ids_wifi_refuse       wifi_sync.c: before connecting and before settimeofday (the round stops, the clock is kept)
 *   ids_rows_suspect      obd_ble_adapter_changed(): the learners skip rows while the link head flags or a block runs
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"

#if CONFIG_OCM_IDS
#include "ai_svc.h"
#include "ids_core.h"

typedef struct {
    uint8_t  state, reason;            /* IDS_ST_*, IDS_R_* (also while "would block") */
    bool     acting, would, blocking_on, link_flag;
    uint32_t win, left_s, blocks_today, would_n, scan_windows, wifi_scans;
    int32_t  q8[IDS_NH];
    bool     on[IDS_NH];
    uint32_t n_adv, link_resp, deauth, rounds;   /* raw counters, for the log */
} ids_result_t;

const ai_svc_def_t *ids_service(void);
bool ids_read(ids_result_t *out);                /* any task, lock-free */
/* the last guard events (board task writes, any task reads lock-free): newest first, ages in s, "" for none */
void ids_events(char ev[][64], int32_t *age_s, int n);

void ids_hook_ble_adv(const uint8_t addr[6], uint8_t type);
void ids_hook_link(bool up);
void ids_hook_link_rtt(uint32_t us);
void ids_hook_wifi_disc(int reason);
void ids_hook_wifi_round(void);
void ids_hook_wifi_scan_begin(void);
void ids_hook_wifi_ap(const uint8_t bssid[6]);
void ids_hook_wifi_scan_done(void);
void ids_hook_wifi_leave(void);

bool ids_ble_blocking(void);
bool ids_wifi_refuse(const char **why);
bool ids_rows_suspect(void);
#define IDS_HOOK(x) x
#else
static inline bool ids_ble_blocking(void) { return false; }
static inline bool ids_wifi_refuse(const char **why) { (void)why; return false; }
static inline bool ids_rows_suspect(void) { return false; }
#define IDS_HOOK(x) ((void)0)
#endif
