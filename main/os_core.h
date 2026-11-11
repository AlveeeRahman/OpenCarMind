/* os_core: the pure rules of the mossREV OS layer (ESP-Mosaico, mossREV 2.0.0). No IDF, no heap, no globals that
 * change: host-tested in tests/host/test_os.c. The firmware glue that acts on these rules is os.c (S31 only).
 *
 * 1. Resets. Every reset somebody or something asks for goes through sys_reset(reason) (os.h). The plan for a reason
 *    is a fixed sequence, always in this order:
 *      a. wait pre_ms, so a reply or a log line gets out (0 where the caller already waited),
 *      b. write the marker: "asked for" (the next boot does not count it toward safe mode) or nothing (a crash path:
 *         the stall guard and the monitor's restart must count),
 *      c. quiesce storage: the NOR FAT, then the NAND (sdlog_quiesce_for_reset),
 *      d. wait post_us (busy wait: the door runs in an esp_timer callback),
 *      e. reset with the reason's method.
 *    The marker comes first so that a hang inside the quiesce, which the task watchdog then resets, still reads as
 *    the reset that was asked for.
 *
 * 2. Runlevels (after PebbleOS's runlevel service). A runlevel is a condition the board is in: Boot (app_main has not
 *    finished), Safe (3 abnormal resets in a row), Driving (a drive is recorded or the car link is up), Saving (the
 *    UPS state: USB power lost, CONFIG_OCM_UPS), Update (a reset, a door entry or a storage format is running), and
 *    Parked when none of these holds. Each service has an enable mask over the runlevels. Several conditions can hold
 *    at once (safe mode while driving), so a service runs only if every active runlevel enables it: each active
 *    runlevel acts like a circuit breaker. A service never asks which runlevel is in force, it asks os_allows(svc).
 *    The runlevel shown on the Board page and in the log is the active one highest in the order
 *    Update > Saving > Driving > Safe > Boot > Parked.
 */
#ifndef OS_CORE_H
#define OS_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 1. resets ---------- */
typedef enum {
    SYS_RESET_REBOOT = 0,    /* the door's REBOOT command */
    SYS_RESET_DOOR,          /* the 1200-baud touch: ROM download mode for esptool */
    SYS_RESET_FORMAT,        /* the restart after a damaged NOR volume was formatted */
    SYS_RESET_UI_STALL,      /* the UI tick stopped for 15 s: abort for a core dump */
    SYS_RESET_MONITOR,       /* sched_mon: an I/O job hung past 3 deadlines */
    SYS_RESET_N
} sys_reset_reason_t;

enum { OS_RST_RESTART = 0, OS_RST_ROM_DOWNLOAD, OS_RST_ABORT };
enum { OS_STEP_WAIT = 1, OS_STEP_MARK, OS_STEP_QUIESCE, OS_STEP_SETTLE, OS_STEP_RESET };

typedef struct {
    const char *name;        /* for the boot log: "REBOOT", "door", ... */
    uint8_t     asked_for;   /* 1: write the marker, the next boot does not count it toward safe mode */
    uint8_t     quiesce;     /* 1: flush and lock the NOR FAT and the NAND first */
    uint8_t     method;      /* OS_RST_* */
    uint16_t    pre_ms;      /* step a */
    uint32_t    post_us;     /* step d */
} os_reset_plan_t;

const os_reset_plan_t *os_reset_plan(sys_reset_reason_t r);        /* NULL for an unknown reason */
/* The steps in execution order (OS_STEP_*), steps that do nothing left out. Returns the count (<= 5). */
int os_reset_steps(sys_reset_reason_t r, uint8_t steps[5]);

/* ---------- 2. runlevels ---------- */
typedef enum { OS_RL_BOOT = 0, OS_RL_SAFE, OS_RL_PARKED, OS_RL_DRIVING, OS_RL_SAVING, OS_RL_UPDATE, OS_RL_N } os_runlevel_t;
typedef enum {
    OS_SVC_MODEL_SWITCH = 0, /* MODEL SET from the door, the Settings segment, the model apps */
    OS_SVC_CLOCK_SET,        /* Settings > Clock by hand */
    OS_SVC_FORMAT,           /* Settings > Format storage */
    OS_SVC_WIFI_EDIT,        /* forget or add a saved network (screen, door) */
    OS_SVC_WIFI_RADIO,       /* Wi-Fi search, join and the clock round */
    OS_SVC_DOOR,             /* the USB door's commands after authentication */
    OS_SVC_ML,               /* loading the model at boot */
    OS_SVC_LEARNER,          /* the R.E.V learner */
    OS_SVC_N
} os_svc_t;

typedef struct {
    bool boot;               /* app_main has not finished */
    bool safe;               /* sched_safe_mode() */
    bool drive;              /* elm_drive_active(): a drive is being recorded */
    bool link;               /* sys_mode DRIVE: connecting to the adapter or logging */
    bool saving;             /* the UPS state is saving (CONFIG_OCM_UPS) */
    bool update;             /* a reset, a door entry or a format is running */
} os_rl_in_t;

uint8_t       os_rl_active(const os_rl_in_t *in);                /* bit r set: runlevel r is active (Parked alone if none) */
os_runlevel_t os_rl_primary(uint8_t active);                     /* the one to show */
bool          os_rl_allows(uint8_t active, os_svc_t svc);        /* every active runlevel enables svc */
os_runlevel_t os_rl_blocker(uint8_t active, os_svc_t svc);       /* the highest active runlevel that refuses, or OS_RL_N */
uint8_t       os_svc_mask(os_svc_t svc);                         /* bit r: svc runs in runlevel r */
const char   *os_rl_name(os_runlevel_t r);                       /* "Parked", "Driving", ... */
/* a short phrase for a refusal by runlevel r: "a drive is running", "the car link is up" (Driving without a drive),
   "safe mode is on", "the board is starting", "the board is saving power", "an update or format is running" */
const char   *os_rl_phrase(os_runlevel_t r, const os_rl_in_t *in);
const char   *os_svc_name(os_svc_t svc);

#ifdef __cplusplus
}
#endif
#endif
