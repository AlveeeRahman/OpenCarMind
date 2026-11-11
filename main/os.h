/* os: the mossREV OS layer on the ESP-Mosaico (S31 only; the rules are in os_core.h, host-tested).
 *
 * It adds no task. Its functions run in the task that calls them (the board task, the LVGL task, the door, sched_mon)
 * and its state is a few words of internal RAM plus RTC words that survive a reset. */
#pragma once
#include "os_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The one reset path: marker, storage quiesce (NOR then NAND), reset, in that order (os_core.h). Never returns.
   detail: a short note for the next boot's log line (may be NULL). */
void sys_reset(sys_reset_reason_t reason, const char *detail) __attribute__((noreturn));
/* app_main, once the boot sequence is done: logs the reset the previous run asked for (if any). */
void os_boot_done(void);

#ifdef __cplusplus
}
#endif
