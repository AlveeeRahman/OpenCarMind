/* Host stub for test_wifi_creds.c: no storage mounted, so wifi_creds_load imports no wifi.csv */
#pragma once
#include <stdbool.h>
#include "sdkconfig.h"                 /* as the real sdlog.h: wifi_creds.c sees the target through it */
#define SDLOG_DIR "/tmp/ocm_wifi_creds_test"
static inline bool sdlog_ok(void) { return false; }
