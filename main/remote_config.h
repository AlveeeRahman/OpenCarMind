/* Runtime-tunable settings, persisted in NVS. Defaults come from Kconfig; /sdcard/obd/config.json (read at boot) overrides them. */
#pragma once
#include <stdint.h>
#include "cJSON.h"

typedef struct {
    uint32_t rev;
    uint16_t poll_interval_ms;
    uint8_t  slow_group_every;
    uint16_t dtc_interval_s;
    uint8_t  backlight_pct;
    uint8_t  theme_mode;        /* 0 auto by time of day, 1 dark, 2 light */
    uint8_t  day_start_hour;    /* light theme from this hour ... */
    uint8_t  day_end_hour;      /* ... until this hour (local time, 24 h) */
} remote_config_t;

void                  remote_config_init(void);           /* load from NVS, Kconfig defaults otherwise */
const remote_config_t *remote_config_get(void);
uint32_t              remote_config_rev(void);
/* Read /sdcard/obd/config.json after the SD card is mounted and apply it; does nothing when the file is absent. */
void                  remote_config_load_sd(void);
/* Apply a JSON object: known numeric fields, "wifi":[{ssid,password}], "custom_pids":"csv text".
 * Returns the number of settings applied; persists the settings. */
int                   remote_config_apply(cJSON *root);
