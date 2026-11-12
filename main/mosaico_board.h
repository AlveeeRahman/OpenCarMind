/* ESP-Mosaico board bring-up and power (ESP32-S31 only), from the ESP-Mosaico bring-up power code (layer 0, runs on this
 * board): safe pin states, the 3.3 V peripheral rail (GPIO60, active low) kept ON, the shared I2C0 bus, the BQ27220 fuel
 * gauge, and the Function key (GPIO7, active low) handled by app_main's button task.
 *
 * Power management: the car firmware does not enable esp_pm, so the clock stays at 320 MHz and the chip never
 * light-sleeps; USB-OTG stays enumerated (the clean project needed NO_LIGHT_SLEEP + CPU_FREQ_MAX locks while USB was
 * powered for exactly this; without esp_pm both hold by construction). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

typedef struct {
    bool ok;
    int  mv;          /* Voltage() 0x08 */
    int  ma;          /* Current() 0x0C, signed, negative while discharging (TI convention; UNVERIFIED on this board) */
    int  soc;         /* StateOfCharge() 0x2C, % */
    int  rem_mah, full_mah;
    int  design_mah;  /* DesignCapacity() 0x3C: 3000 = TI default, the gauge was never configured for the 65 mAh cell */
    int  tte_min;     /* TimeToEmpty() 0x16, minutes; 65535 = not available (standard TI command; UNVERIFIED on this chip) */
    int  temp_dc;     /* Temperature() 0x06 in 0.1 K, here 0.1 C: the gauge's own sensor (no thermistor on this board) */
    int  soh;         /* StateOfHealth() 0x2E, % (meaningful only once the gauge knows the cell) */
    int  cycles;      /* CycleCount() 0x2A */
    int  bstat;       /* BatteryStatus() 0x0A, the gauge's status and safety flags (raw) */
} mosaico_gauge_t;

/* total/free = the drive store (the NAND when nand is true, else the flash FAT); flash_* = the flash FAT with the state files */
typedef struct { bool ok; uint32_t total_kb, free_kb; bool nand; uint32_t flash_total_kb, flash_free_kb; } mosaico_storage_t;

/* Work the UI hands to the board task (internal-RAM stack, lowest priority), so the LVGL task never touches FatFs, NVS,
   BLE or Wi-Fi itself. Never blocks: false when the queue (8 deep) is full. */
typedef enum { MB_FILES_SCAN = 0, MB_FILES_DETAIL, MB_FILES_DELETE, MB_FILES_DELETE_ALL, MB_BLE_FORGET, MB_BLE_SEARCH,
               MB_WIFI_NOW, MB_FORMAT, MB_BUZZ, MB_CLOCK_SET, MB_MODEL_SET, MB_SETTINGS_KICK, MB_WIFI_LIST,
               MB_WIFI_FORGET, MB_WIFI_SCAN, MB_WIFI_JOIN, MB_GAUGE_CAL,
               MB_BLE_PAIR /* the pairing sheet's answer: arg 1 = Use this adapter, 0 = Not this one */ } mb_cmd_t;
bool mosaico_board_request(mb_cmd_t cmd, uint32_t arg);
/* Settings > Model (MB_MODEL_SET, arg = ml_model_id_t): the board task switches with ml_select_model, the path the USB door's
   MODEL SET takes, after the same "drive running" check. gen counts finished requests; why is the refusal when ok is false. */
typedef struct { uint32_t gen; bool ok; int model; char why[80]; } mosaico_model_result_t;
void mosaico_board_model_result(mosaico_model_result_t *out);

/* the Wi-Fi app: the saved networks (wifi_creds), copied by the board task on MB_WIFI_LIST and after a forget; gen
   counts the copies. Network names are shown on the screen only, never logged. */
#define MB_WIFI_NETS 8                                 /* WIFI_CREDS_MAX */
typedef struct {
    uint32_t gen;
    int      n;
    struct { char ssid[33]; int8_t rssi; uint8_t fails; } net[MB_WIFI_NETS];
    char     note[72];                                 /* the last forget's result, "" before one */
    char     clock_note[72];                           /* the last "Sync clock now" answer, "" before one */
    uint32_t clock_gen;                                /* counts those answers */
} mosaico_wifi_list_t;
void mosaico_board_wifi_list(mosaico_wifi_list_t *out);
/* forget one saved network (wifi_sync_forget, the USB door's WIFI-FORGET path); refused while the Wi-Fi task runs a
   round, because it holds a pointer into the list then. False when the queue is full. */
bool mosaico_board_wifi_forget(const char *ssid);
/* Search: the board task runs one active scan (wifi_sync_scan) and publishes the merged list (wifi_ui_logic.h: one row
   per name, strongest first, hidden names left out, saved ones marked). state is WS_ST_*; gen counts the changes. The
   scan records and this list live in PSRAM. */
#include "wifi_ui_logic.h"
typedef struct {
    uint32_t gen;
    int      state, n, n_seen;
    ws_row_t row[WS_ROWS_MAX];
} mosaico_wifi_scan_t;
bool mosaico_board_wifi_search(void);                  /* false when the queue is full (the state says "failed") */
/* copies the list when its gen differs from have_gen; false when nothing changed */
bool mosaico_board_wifi_scan(mosaico_wifi_scan_t *out, uint32_t have_gen);
/* Join one network: the board task hands it to the Wi-Fi task (wifi_sync_join), which saves it in the WIFI-ADD store,
   connects once and sets the clock. password NULL: use the saved one. The password is copied, then wiped from every
   buffer the board and the Wi-Fi task used; it is never logged. result is a wj_result_t (WJ_WAIT until the end). */
typedef struct { uint32_t gen; int result; char ssid[33]; } mosaico_wifi_join_t;
bool mosaico_board_wifi_join(const char *ssid, const char *password);
void mosaico_board_wifi_join_state(mosaico_wifi_join_t *out);

void mosaico_board_init(void);                     /* after the console, before the display */
i2c_master_bus_handle_t mosaico_i2c_bus(void);
bool mosaico_gauge_read(mosaico_gauge_t *g);       /* last reading (refreshed every 2 s by the board task); any task */
bool mosaico_gauge_uncalibrated(const mosaico_gauge_t *g);
/* free space of the log FAT, measured by the board task every 10 s (the UI never calls into FatFs) */
void mosaico_storage_read(mosaico_storage_t *out);
/* UI settings blob: the board task (internal-RAM stack) writes it to NVS 500 ms after the last change, at most 3 s after
   the first one of a burst (ui_settings_logic.h), so a burst of taps is one write and the LVGL task never waits on flash */
void mosaico_settings_save_request(const void *blob, size_t n);
bool mosaico_settings_load(void *blob, size_t n);
/* CONFIG_OCM_SWIPE_INTENT: the smart-swipe learner blob (swipe_intent.h, SI_BLOB_BYTES), NVS "ocm_ui"/"si", written
   by the board task; and its one-word summary for the SYS console line (mosaico_display.c, any task) */
void mosaico_si_save_request(const void *blob, size_t n);
bool mosaico_si_load(void *blob, size_t *n);
void mosaico_display_swipe_brief(char *out, size_t n);
/* CONFIG_OCM_UI_METRICS: the Smart UI gate blob (ui_metrics.h), NVS "ocm_ui"/"um", written by the board task */
void mosaico_um_save_request(const void *blob, size_t n);
bool mosaico_um_load(void *blob, size_t *n);
