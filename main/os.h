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
/* app_main, once the boot sequence is done: ends the Boot runlevel, logs the reset the previous run asked for. */
void os_boot_done(void);

/* Runlevels (os_core.h). os_allows: every active runlevel enables svc. os_refusal: NULL when allowed, else the
   phrase for the runlevel that refuses ("a drive is running", ...). os_runlevel: the one to show (Board page, log). */
bool          os_allows(os_svc_t svc);
const char   *os_refusal(os_svc_t svc);
os_runlevel_t os_blocker(os_svc_t svc);                 /* the runlevel that refuses svc, OS_RL_N if none (texts only) */
os_runlevel_t os_runlevel(void);
/* Update runlevel while a format runs (the board task brackets it). sys_reset enters it by itself. */
void          os_update_begin(void);
void          os_update_end(void);
/* the board task, every 2 s: logs runlevel changes, forms the boot health verdict at OS_H_VERDICT_MS */
void          os_tick(void);
/* the boot health verdict (os_core.h): OS_H_PENDING until it is formed; flags = the reasons */
uint8_t       os_health(uint16_t *flags);

/* Model admission (os_core.h section 4): the jobs sched_mon measured plus the model's job with C = its self-test run x 2,
   response-time analysis on its core. Logs every job's R and the margin. false (why filled) when a deadline would fail. */
bool          os_admit_model(int ai_id, uint32_t measured_us, char *why, size_t why_n);
/* The AI service contract (os_core.h section 5): register the service's sched_mon job from its table entry, with its
   stall limit and CPU reservation. Returns the job. */
int           os_ai_register(int ai_id, bool watchdog);

#ifdef __cplusplus
}
#endif
