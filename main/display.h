#pragma once
#include "esp_err.h"
#include "sdkconfig.h"
/* The car board's info page, adapter line (display.c). Pairing confirm on: "paired" / "not paired", the address is never
   shown. Off (the car board's default): the address and " (saved)", as in its tested EdgeOS 2.0.0 image. */
#if CONFIG_OBD_BLE_PAIR_CONFIRM
#define DISPLAY_ADAPTER_FMT      "%s"
#define DISPLAY_ADAPTER_ARGS(st) (st).adapter_saved ? "paired" : "not paired"
#else
#define DISPLAY_ADAPTER_FMT      "%s%s"
#define DISPLAY_ADAPTER_ARGS(st) (st).adapter_addr, (st).adapter_saved ? " (saved)" : ""
#endif
esp_err_t display_init(void);
void      display_next_page(void);
void      display_sleep(void);
int       display_current_page(void);
void      display_set_backlight(uint8_t pct);   /* backlight off, panel off (before deep sleep) */
/* Power-off: fade the backlight to black over ms, turn the panel off, hold the backlight pin low through deep sleep. */
void      display_fade_off(int ms);
