/* Known Wi-Fi networks, stored in NVS, imported from /sdcard/obd/wifi.csv (one "ssid,password" per line). They are used
 * only to set the clock over SNTP (wifi_sync.c). The file is deleted after a successful import so the card does not
 * carry the password around; NVS holds it in plain text, readable by anyone with physical access to the flash. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define WIFI_CREDS_MAX 8

typedef struct {
    char    ssid[33];
    char    password[65];
    int8_t  last_rssi;
    uint8_t fail_count;      /* consecutive failed connects; reset on success */
    int64_t retry_after_ms;  /* uptime before which this network is skipped */
} wifi_cred_t;

size_t            wifi_creds_load(void);                 /* NVS + optional Kconfig fallback + SD import; returns count */
size_t            wifi_creds_count(void);
wifi_cred_t      *wifi_creds_get(size_t i);
wifi_cred_t      *wifi_creds_find(const char *ssid);
bool              wifi_creds_add(const char *ssid, const char *password);   /* add or update, persists */
bool              wifi_creds_remove(const char *ssid);
/* After a successful connection: remove entries that differ from this SSID only by case or spacing
 * (typically a mistyped copy of the same network). */
void              wifi_creds_dedupe(const char *good_ssid);
