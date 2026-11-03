/* Input checks for data that arrives from outside the firmware: the network clock, the SD card and the OBD adapter. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when a Unix time from the network lies between 2026-01-01 and 2100-01-01 UTC. An unauthenticated SNTP reply can say
   anything; a time outside this range is never accepted. */
bool harden_epoch_plausible(int64_t epoch_s);

/* True when a Wi-Fi network entry can be used: SSID 1 to 32 bytes, password empty or 8 to 64 bytes (a WPA2 passphrase is
   8 to 63 characters, a raw key is 64 hex digits), and no control characters in either. */
bool harden_wifi_cred_ok(const char *ssid, const char *password);

/* Cleans one standardised model input. A value that is not a finite number is reported by returning false; the caller then
   treats the sample as missing. A finite value is limited to +-100, far outside anything seen in training. */
bool harden_model_input(float *v);

#ifdef __cplusplus
}
#endif
