/* ESP-Mosaico board bring-up and power (ESP32-S31 only), from ~/opencarmind-mosaico/main/power.c (layer 0, runs on this
 * board): safe pin states, the 3.3 V peripheral rail (GPIO60, active low) kept ON, the shared I2C0 bus, the BQ27220 fuel
 * gauge, and the Function key (GPIO7, active low) handled by app_main's button task.
 *
 * Power management: the car firmware does not enable esp_pm, so the clock stays at 240 MHz and the chip never
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
} mosaico_gauge_t;

typedef struct { bool ok; uint32_t total_kb, free_kb; } mosaico_storage_t;

/* Work the UI hands to the board task (internal-RAM stack, lowest priority), so the LVGL task never touches FatFs, NVS,
   BLE or Wi-Fi itself. Never blocks: false when the queue (8 deep) is full. */
typedef enum { MB_FILES_SCAN = 0, MB_FILES_DETAIL, MB_FILES_DELETE, MB_FILES_DELETE_ALL, MB_BLE_FORGET, MB_BLE_SEARCH,
               MB_WIFI_NOW, MB_FORMAT, MB_BUZZ } mb_cmd_t;
bool mosaico_board_request(mb_cmd_t cmd, uint32_t arg);

void mosaico_board_init(void);                     /* after the console, before the display */
i2c_master_bus_handle_t mosaico_i2c_bus(void);
bool mosaico_gauge_read(mosaico_gauge_t *g);       /* last reading (refreshed every 2 s by the board task); any task */
bool mosaico_gauge_uncalibrated(const mosaico_gauge_t *g);
/* free space of the log FAT, measured by the board task every 10 s (the UI never calls into FatFs) */
void mosaico_storage_read(mosaico_storage_t *out);
/* UI settings blob: the board task (internal-RAM stack) writes it to NVS when it changed */
void mosaico_settings_save_request(const void *blob, size_t n);
bool mosaico_settings_load(void *blob, size_t n);
