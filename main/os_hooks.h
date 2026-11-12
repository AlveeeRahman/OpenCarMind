/* os_hooks: how files shared with the car board (S3) call the mossREV OS layer (S31 only).
 *
 * On the ESP-Mosaico these macros call the OS (os.h). On the S3 each expands to the statement or expression that the
 * site had before, on the same line, so the S3 objects do not change (the S3 build is compared byte for byte).
 *   OCM_SYS_RESET(reason, s3)    the one reset path, sys_reset(reason) (S3: the old statement s3)
 *   OCM_REFUSED(svc, s3)         true when the runlevels refuse svc (S3: the old check s3)
 *   OCM_WIFI_BOOT_START(s3)      app_main's Wi-Fi start: the S31 holds Wi-Fi until the boot health verdict
 *                                (wifi_sync_start_held, os_core.h section 3); S3: the old call s3 */
#pragma once
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S31
#include "os.h"
#define OCM_SYS_RESET(reason, s3) sys_reset(reason, NULL)
#define OCM_REFUSED(svc, s3) (!os_allows(svc))
#define OCM_WIFI_BOOT_START(s3) wifi_sync_start_held()
#else
#define OCM_SYS_RESET(reason, s3) s3
#define OCM_REFUSED(svc, s3) (s3)
#define OCM_WIFI_BOOT_START(s3) s3
#endif
