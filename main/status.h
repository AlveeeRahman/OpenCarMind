/* Shared, mutex-protected snapshot of everything the display needs. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "obd_fields.h"
#include "fault_engine.h"
#include "events.h"

typedef enum {
    BLE_ST_IDLE = 0,
    BLE_ST_SCANNING,
    BLE_ST_CONNECTING,
    BLE_ST_DISCOVERING,
    BLE_ST_ELM_INIT,
    BLE_ST_LOGGING,
    BLE_ST_LOCKED,        /* adapter answered "NA": LELink security enabled */
} ble_state_t;

typedef enum {
    WIFI_ST_DISABLED = 0,
    WIFI_ST_SCANNING,
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,
    WIFI_ST_IDLE,         /* known networks exist, radio parked between clock syncs */
} wifi_state_t;

typedef struct {
    float    f[F_COUNT];        /* decoded mode-01 fields, see obd_fields.h */
    uint64_t valid[2];          /* bit per field: has been read at least once */
    float    adapter_voltage_v; /* ATRV */
    bool     adapter_v_valid;
    bool     mil_on;
    uint8_t  dtc_count;
    bool     status_valid;
    char     dtcs[64];          /* stored (mode 03), "P0300 P0171" */
    char     pending_dtcs[64];  /* mode 07 */
    char     permanent_dtcs[64];/* mode 0A */
    char     vin[18];
    char     protocol[24];
    /* user PIDs from /sdcard/obd/custom_pids.csv (tire pressure, oil pressure, ...) */
    float    custom[8];
    uint8_t  custom_valid;      /* bitmask */
    char     custom_name[8][16];
    char     custom_unit[8][8];
    uint8_t  custom_count;
    /* fuel and economy, worked out per row by elm327.c (econ.h); negative = unknown */
    float    fuel_lph_calc;     /* best available fuel rate, L/h */
    uint8_t  fuel_src;          /* fuel_src_t: none, car PID, MAF, model, physics or learned estimate */
    double   trip_km;           /* this drive, counted while the fuel rate was known */
    float    trip_fuel_l;
    float    econ_trip_kpl;     /* km/L over the drive so far */
    float    econ_inst_kpl;     /* km/L at this row */
    /* fill-up calibration (rev_cal.h): fuel_lph_calc and trip_fuel_l above are calibrated, these are as the source gave them */
    float    fuel_lph_raw;      /* negative = unknown */
    float    fuel_cal;          /* the factor in force, 1.0 without fill-ups */
    float    trip_fuel_raw_l;
    /* R.E.V physics torque (rev_torque.h) and tank learner (rev_tank.h), both models; NAN / negative = unknown */
    float    torque_nm;         /* 5 s mean brake torque */
    uint8_t  torque_src;        /* rev_tq_src_t: none, pid, rate, maf, k */
    float    tank_left_l;       /* calibrated litres left, once the tank learner is ready */
    float    tank_range_km;
} obd_values_t;

#define OBD_HAS(v, field) (((v)->valid[(field) / 64] >> ((field) % 64)) & 1ULL)
#define OBD_SET(v, field) ((v)->valid[(field) / 64] |= 1ULL << ((field) % 64))

typedef struct {
    ble_state_t  ble;
    char         adapter_name[32];
    char         adapter_addr[18];
    bool         adapter_saved;       /* address is in NVS: auto-connect mode */
    int8_t       adapter_rssi;

    bool         sd_mounted;
    uint32_t     session_id;
    uint32_t     rows_written;
    uint64_t     bytes_written;
    char         sd_error[32];

    wifi_state_t wifi;
    int8_t       wifi_rssi;
    char         wifi_ssid[33];
    bool         time_synced;
    bool         clock_estimated;     /* resumed from the last save on the card, not yet corrected over Wi-Fi */

    obd_values_t obd;

    fault_report_t faults;      /* latest analysis of the DTC lists */
    uint32_t     fault_rev;     /* increments when the analysis changes */
    char         alarm_text[64];/* non-empty while an alarm banner should show */
    int64_t      alarm_until_ms;/* uptime ms when the banner auto-dismisses (INT64_MAX = until acknowledged) */
    bool         alarm_critical;/* persists until the user taps */
    char         prompt_text[64];/* amber question banner, e.g. forget-adapter confirmation */
    int64_t      prompt_until_ms;
    sys_event_t  events[EVENT_MAX];   /* ring: newest at (event_head - 1) */
    uint8_t      event_head, event_count;
    char         sys_popup[EVENT_TEXT];
    int64_t      sys_popup_until_ms;
    bool         sys_popup_error;
    uint8_t      cpu_pct[2];    /* load per core, from FreeRTOS run-time stats */
    uint32_t     heap_free_kb, psram_free_kb, psram_total_kb;
    float        chip_temp_c;   /* die temperature from the ESP32-S3 sensor, NAN if it is unavailable */
    float        pmu_mips[2];   /* instructions retired per second per core, millions, Xtensa PMU (cpu_monitor.c) */
    float        power_w;       /* estimate from datasheet currents, see cpu_monitor.c */
    int16_t      pattern_id;    /* last closed window */
    float        pattern_novelty;
    uint32_t     pattern_windows;
    uint8_t      pattern_used;
    uint8_t      hold_pct;      /* 0..100 ring while BOOT is held, 0 = hidden */
    uint8_t      hold_stage;    /* 0 none, 1 power-off armed, 2 forget armed */
    uint8_t      shutdown_stage; /* 0 running, 1 saving before power-off, 2 off: screen says so, then fades out */
    uint8_t      units;         /* units_t: display units, see units.h */
} app_status_t;

extern app_status_t g_status;
extern SemaphoreHandle_t g_status_mutex;

#define STATUS_LOCK()   xSemaphoreTake(g_status_mutex, portMAX_DELAY)
#define STATUS_UNLOCK() xSemaphoreGive(g_status_mutex)

void status_init(void);
/* Copy the snapshot out under the lock. */
void status_get(app_status_t *out);
