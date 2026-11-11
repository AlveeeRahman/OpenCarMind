#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "board.h"
/* Starts the Wi-Fi clock sync: a short station connection to a known network (wifi.csv on the SD card, NVS, or the
 * Kconfig fallback) to set the clock over SNTP. Nothing is sent to or stored on any server. A no-op when no network
 * is known. */
esp_err_t wifi_sync_start(void);
bool      wifi_sync_connected(void);
/* Try the known networks now instead of at the next scheduled scan (the same scan, connect and clock sync).
   False when no network was known at boot or Wi-Fi is compiled out. */
bool      wifi_sync_connect_now(void);

#if BOARD_MOSAICO
/* The ESP-Mosaico Wi-Fi app: search for networks and join one from the screen. Wi-Fi still does one thing: the board
   scans, joins the network the user chose and asks the time server for the time, nothing else. Network names and
   passwords never go to a log. All of these run in the board task (mosaico_board.c), never in the LVGL task. */
typedef struct { char ssid[33]; int8_t rssi; uint8_t channel; bool secure; } wifi_seen_t;
/* app_main, right after wifi_sync_start(): from now on the functions below may start the radio themselves */
void      wifi_sync_started(void);
/* Bring the station up when no network was known at boot (the board then booted without Wi-Fi), and start the Wi-Fi
   task. False when Wi-Fi could not start (internal RAM) or app_main has not reached wifi_sync_started() yet. */
bool      wifi_sync_up(void);
/* One active scan, strongest first, hidden networks left out. Waits up to 3 s for the Wi-Fi task to finish a round.
   Returns the count, or -1 (busy, driving, or the radio could not start). */
int       wifi_sync_scan(wifi_seen_t *out, int max);
/* Join: the Wi-Fi task saves the network (wifi_creds, the WIFI-ADD store and rules; password NULL keeps the saved one),
   connects once, and on success sets the clock (the normal clock sync). done(result, ctx) runs in the Wi-Fi task with a
   wj_result_t (wifi_ui_logic.h). A wrong password does not stay saved: a new network is removed again, an old one gets
   its old password back. False when a join is already waiting (done is not called then). */
bool      wifi_sync_join(const char *ssid, const char *password, void (*done)(int result, void *ctx), void *ctx);
/* Forget a saved network unless the Wi-Fi task holds a pointer into the list (a round running): 1 forgotten,
   0 not saved, -1 busy. The screen's Forget and the USB door's WIFI-FORGET both go through here. */
int       wifi_sync_forget(const char *ssid);
int       wifi_sync_add(const char *ssid, const char *password);   /* the door's WIFI-ADD under the round lock: 1 ok, 0 rejected, -1 busy */
#endif
