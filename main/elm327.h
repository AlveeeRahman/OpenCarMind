/* ELM327 command/response layer and OBD-II polling loop, transport-agnostic. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Transport hooks implemented by obd_ble.c */
int  obd_ble_write(const uint8_t *data, size_t len);

/* Called by the transport */
void elm_init(void);
/* A drive is being set up or logged (poll lists built from the selected model): no model switch now. */
bool elm_drive_active(void);
void elm_rx_bytes(const uint8_t *data, size_t len);   /* from BLE notifications */
void elm_link_changed(bool up);                        /* link up = subscribed and ready */

/* Send a command (without CR) and wait for the '>' prompt. Returns response length,
 * -1 on timeout, -2 if the link is down. The response has echo, CR/LF and the prompt removed. */
int  elm_transact(const char *cmd, char *resp, size_t resp_sz, uint32_t timeout_ms);

/* Power-off support. elm_request_stop() stops polling at once and no new drive starts; elm_wait_stopped() also waits up to
   timeout_ms for the poll loop to save and close the drive, and returns true when it has. elm_resume() undoes the stop. */
void elm_request_stop(void);
bool elm_wait_stopped(uint32_t timeout_ms);
void elm_resume(void);

/* Connection search (automatic after a minute without engine data, or a BOOT double-click). With no adapter linked it
   lifts the saved-adapter lock and clears the skip list for 90 s (obd_ble_explore). With an adapter linked it stops the
   current attempt and, read-only, tries every protocol, lists the computers that answer (broadcast request with headers,
   plus each 11-bit physical address 7E0..7E7), asks each for RPM on its own address and keeps the engine for this boot
   (checked again at the next connect). The result is shown on screen and saved in the session JSON ("search"). */
void     elm_explore(void);
bool     elm_exploring(void);
bool     elm_link_is_up(void);
int64_t  elm_link_up_ms(void);   /* esp_timer ms when the current link came up, -1 when down */
uint32_t elm_link_epoch(void);   /* +1 at every adapter link-up: the automatic search runs once per connection */
void     elm_link_summary(char *out, size_t n);   /* "Link OK: polling 7E0" for the double-click status line */
