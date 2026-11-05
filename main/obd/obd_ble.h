#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t obd_ble_start(void);
bool      obd_ble_connected(void);
/* Pause scanning (an existing link is never dropped); while paused, obd_ble_probe(ms) scans once for ms. */
void      obd_ble_set_paused(bool paused);
void      obd_ble_probe(uint32_t ms);
/* Erase the saved adapter address and go back to name/service discovery. */
void      obd_ble_forget_adapter(void);
/* Called by elm327.c once the device answered the ELM327 reset: keeps its address for later boots. */
void      obd_ble_adapter_confirmed(void);
/* Called by elm327.c when the device never answered like an ELM327: drops it, and skips it for a while if unsaved. */
void      obd_ble_adapter_rejected(const char *why);
/* Transport write used by elm327.c; splits to the ATT MTU; returns 0 on success. */
int       obd_ble_write(const uint8_t *data, size_t len);
