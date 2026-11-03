#pragma once
#include "esp_err.h"
esp_err_t display_init(void);
void      display_next_page(void);
void      display_sleep(void);
int       display_current_page(void);
void      display_set_backlight(uint8_t pct);   /* backlight off, panel off (before deep sleep) */
