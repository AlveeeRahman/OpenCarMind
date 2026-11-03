#pragma once
#include "esp_err.h"
#include <stdbool.h>
/* Starts the Wi-Fi clock sync: a short station connection to a known network (wifi.csv on the SD card, NVS, or the
 * Kconfig fallback) to set the clock over SNTP. Nothing is sent to or stored on any server. A no-op when no network
 * is known. */
esp_err_t wifi_sync_start(void);
bool      wifi_sync_connected(void);
