/* Sends session files to the OpenCarMind desktop app on the same network, over TLS pinned to the app's certificate.
 *
 * Pairing happens over the USB cable through the authenticated door (usb_door.c): the desktop app sends its address, port,
 * token and certificate, and the board keeps them in NVS. Uploads run only while the car link is down and Wi-Fi is up.
 *
 * Protocol (board is the client, the desktop keeps the offsets, like an append blob):
 *   GET  /api/v1/files                     -> "name size" per line for every file the desktop holds from this board
 *   POST /api/v1/append?name=N&offset=O    -> body is the bytes from O; 200 with the new size, 409 with the size it has
 *   PUT  /api/v1/put?name=N                -> whole file (session JSON, rewritten as the drive goes on)
 * Every request carries "Authorization: Bearer <token>" and "X-OCM-Board: <MAC>". */
#pragma once
#include <stdbool.h>

void uploader_init(void);        /* load the pairing from NVS */
bool uploader_configured(void);
/* Paired AND this build sends drives over Wi-Fi (CONFIG_OCM_WIFI_UPLOAD). Pairing alone still protects the USB door. */
bool uploader_wifi_enabled(void);
/* Store a new pairing (replaces any earlier one). Returns false and changes nothing if a value is not valid. */
bool uploader_pair(const char *host, unsigned port, const char *token, const char *cert_pem);
void uploader_unpair(void);
const char *uploader_token(void);  /* the paired desktop's token, "" when unpaired; the USB door uses it as an HMAC key */
const char *uploader_board_id(void);
/* One pass: catch the desktop up on every session file, within a byte budget. Returns false if the desktop could not be
   reached. Call it from the Wi-Fi task only, while connected and parked. */
bool uploader_pass(void);
bool uploader_busy(void);        /* a pass is running */
