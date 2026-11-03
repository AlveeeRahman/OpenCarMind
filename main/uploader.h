/* Sends session files to the OpenCarMind desktop app on the same network, over TLS pinned to the app's certificate.
 *
 * Pairing: the desktop app writes /sdcard/obd/receiver.json ({"host","port","token","cert"}). At boot the board copies it
 * to NVS and deletes the file, like wifi.csv. Uploads run only while the car link is down and Wi-Fi is up.
 *
 * Protocol (board is the client, the desktop keeps the offsets, like an append blob):
 *   GET  /api/v1/files                     -> "name size" per line for every file the desktop holds from this board
 *   POST /api/v1/append?name=N&offset=O    -> body is the bytes from O; 200 with the new size, 409 with the size it has
 *   PUT  /api/v1/put?name=N                -> whole file (session JSON, rewritten as the drive goes on)
 * Every request carries "Authorization: Bearer <token>" and "X-OCM-Board: <MAC>". */
#pragma once
#include <stdbool.h>

void uploader_init(void);        /* after the SD card is mounted: import receiver.json, load the pairing from NVS */
bool uploader_configured(void);
/* One pass: catch the desktop up on every session file, within a byte budget. Returns false if the desktop could not be
   reached. Call it from the Wi-Fi task only, while connected and parked. */
bool uploader_pass(void);
