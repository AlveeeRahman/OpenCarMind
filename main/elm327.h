/* ELM327 command/response layer and OBD-II polling loop, transport-agnostic. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Transport hooks implemented by obd_ble.c */
int  obd_ble_write(const uint8_t *data, size_t len);

/* Called by the transport */
void elm_init(void);
void elm_rx_bytes(const uint8_t *data, size_t len);   /* from BLE notifications */
void elm_link_changed(bool up);                        /* link up = subscribed and ready */

/* Send a command (without CR) and wait for the '>' prompt. Returns response length,
 * -1 on timeout, -2 if the link is down. The response has echo, CR/LF and the prompt removed. */
int  elm_transact(const char *cmd, char *resp, size_t resp_sz, uint32_t timeout_ms);
