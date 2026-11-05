/* Authenticated setup door on the USB cable, used by the OpenCarMind desktop app so nobody has to open the SD card.
 *
 * Every line the app reads starts with "OCM1 "; log lines share the cable and are ignored by the app.
 *   OCM1 HELLO                       -> OCM1 OK HELLO fw=<version> board=<mac> paired=<0|1> nonce=<32 hex>
 *   OCM1 CODE                        shows a 6-digit code on the board's screen for 2 minutes
 *   OCM1 AUTH-CODE <code>            first pairing: proves the user can see this board (3 tries per code)
 *   OCM1 AUTH-HMAC <64 hex>          later: HMAC-SHA256 of the HELLO nonce, keyed with the paired app's token
 * After authentication (10 minutes idle closes the session):
 *   OCM1 PAIR <host> <port> <token> <cert PEM in base64>    OCM1 UNPAIR
 *   OCM1 WIFI-LIST | WIFI-ADD <ssid b64> [password b64] | WIFI-FORGET <ssid b64>
 *   OCM1 CONFIG <config.json in base64>                      same keys as /sdcard/obd/config.json
 *   OCM1 LIST                        -> OCM1 D <name> <size> per session file
 *   OCM1 GET <name> <offset> <len>   -> OCM1 D <base64 of up to 3072 bytes>
 *   OCM1 MODEL GET                   -> OCM1 OK MODEL active=<ev|rev> available=ev,rev loaded=<0|1> learning=<0|1> run_us=<n>
 *   OCM1 MODEL SET <ev|rev>          switch the on-board model (unload, load, kept in NVS); same answer as MODEL GET
 *   OCM1 REV-STATUS                  -> OCM1 D <group> key=value ... (state, fuel, error, phys, cal, life, one iv per interval)
 *   OCM1 REV-FILLUPS <json b64>      the whole fill-up list: {"rev":n,"entries":[{"id","after","litres","full"} or
 *                                    {"id","kind":"trip","from","after","l100"}]} -> OCM1 OK REV-FILLUPS rev= cal= used= skipped=
 *   OCM1 REV-RESET <fuel|phys|cal|all>
 *   OCM1 REBOOT
 * Answers end with "OCM1 OK ..." or "OCM1 ERR <reason>". Anything that changes the board or reads the card is refused
 * while the car link is up. */
#pragma once

void usb_door_start(void);
