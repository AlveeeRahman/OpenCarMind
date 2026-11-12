#pragma once
/* Which resets count toward the boot-loop safe mode (no IDF dependency, host-tested in tests/host/test_reset_class.c).
 *
 * Only a crash counts: a panic (also the ui-stall guard's abort), an interrupt or task watchdog, another watchdog, or a
 * software reset the monitor itself asked for (restart_hung leaves a reason behind). A reset somebody asked for never
 * counts: the USB download door (1200-baud touch -> ROM download -> esptool's reset), the REBOOT command, the restart after
 * a format. Those set an RTC marker just before resetting, which wins over the reset reason, because on the ESP-Mosaico
 * the boot after a door flash reads as a panic (measured: 3 flashes in 10 minutes put the board into safe mode). */
#include <stdbool.h>

typedef enum {
    RESET_KIND_POWERON, RESET_KIND_SW, RESET_KIND_PANIC, RESET_KIND_INT_WDT, RESET_KIND_TASK_WDT, RESET_KIND_WDT,
    RESET_KIND_USB, RESET_KIND_DEEPSLEEP, RESET_KIND_BROWNOUT, RESET_KIND_OTHER
} reset_kind_t;

/* monitor_asked: the scheduler monitor left a reason before its esp_restart(); intentional: the RTC marker was set */
bool reset_is_abnormal(reset_kind_t kind, bool monitor_asked, bool intentional);
