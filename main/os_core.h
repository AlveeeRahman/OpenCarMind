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
 *
 * 3. Boot health. About a minute after boot (after sched_mon's first summary) the OS combines the model self-test,
 *    the flash FAT preflight and audit, the NAND mount, the first UI tick, the touch probe and the scheduler summary
 *    into one verdict. Bad means this firmware image is not fit to keep: no UI tick at all, or safe mode (three
 *    crashes in a row). Degraded means something works less well than it should but the image runs: a model
 *    self-test that differs or did not run, a repaired or damaged store, no NAND, touch failing, deadline misses or
 *    stalls, a failed response-time analysis, a slow first frame. Good is everything else. A damaged store is not
 *    Bad: older firmware would not repair the data, so it is no reason to go back to it.
 *
 * 4. Model admission by response-time analysis. Fixed-priority tasks i with worst-case CPU time C_i, period T_i and
 *    deadline D_i on one core. Task i's worst-case response time R_i is the smallest fixed point of
 *        R = C_i + sum over higher-priority tasks j on the same core of ceil(R / T_j) * C_j
 *    (Joseph and Pandya 1986), found by iterating from R = C_i. Task i is schedulable when R_i <= D_i. The sum counts
 *    how many times each higher-priority task can be released inside a window of length R and charges its full C_j for
 *    each release: the critical instant, all tasks released together. Equal priorities do not interfere here, as in
 *    sched_mon.c. A model is admitted on a switch or a load only if every task on its core still meets its deadline when
 *    the model's C is its measured self-test run times OS_ADMIT_FACTOR_PM / 1000. The factor is a safety margin: one
 *    measured run is not a worst case (cache state, PSRAM contention). The recurrence assumes D_i <= T_i or, for a
 *    job with a longer deadline (the R.E.V learner, D 30 s, T 1 s), that its R_i stays below T_i, which the log shows.
 *
 * 5. AI service contract. Every model and learner declares, in one table (os_ai_contract): its period, deadline and
 *    stall limit (sched_mon), its CPU reservation (sched_mon's token bucket: share per mille of a core, burst ms; 0 =
 *    none, a model is admitted by the analysis instead), its self-test, its fallback model and the runlevel service
 *    whose mask decides when it may run. The firmware registers the jobs from the table (os_ai_apply), the admission
 *    takes its period, deadline, priority and core from it, and ml_init falls back along it.
 *
 * 6. UPS power state (CONFIG_OCM_UPS, default n until measured on the board). The board runs from the car's USB and
 *    has a 65 mAh cell. USB loss is seen in the gauge's current: the BQ27220 reports discharge as a negative current
 *    (TI's sign convention; on this board UNVERIFIED until measured). The HUSB320 Type-C controller is not on I2C0 in
 *    the board's user guide (no address listed) and has no VBUS line to a GPIO, so VBUS_DET is not used. States:
 *      USB      on car power.
 *      GRACE    the current has been below -discharge_ma for two samples in a row: flush the logs, dim the screen, and
 *               wait grace_ms, because cranking the engine drops the USB rail for a few seconds. USB back (current
 *               above -5 mA) returns to USB and restores the screen.
 *      OFF      grace ran out on battery: sys_reset(POWER_OFF) quiesces storage and pulls GPIO57 (the SAM8108
 *               POWER_SWITCH request, documented as open-drain low = shutdown) or, without that, deep sleep.
 *    A hard floor: at or below floor_mv on battery the board goes to OFF at once, from any state, before a brownout
 *    can tear a write. GRACE is the Saving runlevel.
 *
 * 7. Degradation ladder (mixed-criticality style). Every 10 s the OS reads the ml job's deadline misses. A window with
 *    a new miss climbs one rung, six clean windows in a row (a minute) step one rung down:
 *      0 normal,
 *      1 the UI refresh period goes from 16 to 33 ms (half the frames, LVGL's own timer),
 *      2 the R.E.V learner's CPU reservation drops to 1 per mille (it pauses, its learned values stay),
 *      3 the model's inference pauses (windows are skipped, the same model stays loaded).
 *    The ladder never changes which model is loaded: no model switch while driving is the owner's rule, and a switch
 *    while parked stays the owner's own action. Rung 3 is left only when parked, so the model does not go on and off
 *    during one drive.
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
    SYS_RESET_POWER_OFF,     /* the UPS state: on battery past the grace, or at the voltage floor (CONFIG_OCM_UPS) */
    SYS_RESET_N
} sys_reset_reason_t;

enum { OS_RST_RESTART = 0, OS_RST_ROM_DOWNLOAD, OS_RST_ABORT, OS_RST_POWER_OFF };
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

/* ---------- 3. boot health ---------- */
enum { OS_H_PENDING = 0, OS_H_GOOD, OS_H_DEGRADED, OS_H_BAD };
enum {
    OS_HF_MODEL_DIFF = 1u << 0, OS_HF_MODEL_NOT_RUN = 1u << 1, OS_HF_NOR_REPAIRED = 1u << 2, OS_HF_NOR_WARN = 1u << 3,
    OS_HF_NOR_DOWN = 1u << 4, OS_HF_NAND = 1u << 5, OS_HF_NO_FRAME = 1u << 6, OS_HF_SLOW_FRAME = 1u << 7,
    OS_HF_TOUCH = 1u << 8, OS_HF_MISSES = 1u << 9, OS_HF_STALLS = 1u << 10, OS_HF_UNSCHED = 1u << 11, OS_HF_SAFE = 1u << 12,
};
#define OS_H_BAD_FLAGS   (OS_HF_NO_FRAME | OS_HF_SAFE)
#define OS_H_SLOW_FRAME_MS 15000          /* the first UI tick later than this is Degraded */
#define OS_H_VERDICT_MS    65000          /* uptime at which the verdict is formed (sched_mon summarises at 60 s) */
typedef struct {
    int8_t   selftest;        /* ml_selftest_state(): 1 exact, 0 differs, -1 not run */
    bool     model_expected;  /* a model should be loaded (the ML service runs, a model is built in) */
    int8_t   nor;             /* the flash FAT: 1 mounted, 0 not mounted, -1 damaged (left unmounted to protect it) */
    uint16_t nor_repairs;     /* FAT sectors the preflight rewrote from the other copy */
    bool     nor_warn;        /* the audit found lost clusters or a single FAT copy */
    int8_t   nand;            /* 1 mounted, 0 absent (logs on the flash FAT), -1 unreadable or failed, 2 not built */
    uint32_t first_tick_ms;   /* uptime of the first UI tick, 0 = none yet */
    bool     touch_ok;        /* the touch controller opened and is not failing */
    uint32_t misses, stalls;  /* sched_mon, every job */
    bool     schedulable;     /* sched_mon's response-time analysis: every analysed job meets its deadline */
    bool     safe;
} os_health_in_t;
uint8_t os_health_verdict(const os_health_in_t *in, uint16_t *flags);    /* OS_H_GOOD / DEGRADED / BAD and why */
const char *os_health_name(uint8_t verdict);                             /* "pending", "good", "degraded", "bad" */
/* "good" or "degraded: model self-test differs, NAND not mounted" (truncated to n) */
int os_health_text(uint8_t verdict, uint16_t flags, char *buf, size_t n);

/* ---------- 4. response-time analysis and model admission ---------- */
#define OS_RTA_MAX         12
#define OS_ADMIT_FACTOR_PM 2000           /* C = measured self-test run x 2 */
#define OS_RTA_MISS        UINT32_MAX
typedef struct {
    const char *name;
    uint32_t    c_us, t_us, d_us;         /* worst CPU time, period, deadline */
    uint8_t     prio;                     /* FreeRTOS priority: higher number = higher priority */
    int8_t      core;                     /* -1: not analysed (has not run yet) */
} os_rt_task_t;
/* R_i for every task on `core` (OS_RTA_MISS when it passes D_i or does not converge; 0 for tasks elsewhere).
   Returns the number of tasks on that core that miss. */
int os_rta(const os_rt_task_t *t, int n, int core, uint32_t *r_us);
typedef struct {
    bool     ok;                          /* every task on the core meets its deadline */
    uint32_t c_us;                        /* the candidate's C used (measured x factor) */
    int32_t  margin_us;                   /* min over the core's tasks of D_i - R_i (negative: the worst miss) */
    int      worst;                       /* the task with the smallest margin (index into the analysed set) */
    int      n;                           /* tasks analysed: set + the candidate (the last one) */
    uint32_t r_us[OS_RTA_MAX];
} os_admit_t;
/* The set without the candidate's old entry, plus the candidate with C = measured_us x factor_pm / 1000. */
bool os_admit(const os_rt_task_t *set, int n, const os_rt_task_t *cand, uint32_t measured_us, uint32_t factor_pm, os_admit_t *out);

/* ---------- 5. AI service contract ---------- */
enum { OS_AI_EV = 0, OS_AI_REV = 1, OS_AI_REV_LEARNER, OS_AI_N };     /* the first two equal ml_model_id_t */
enum { OS_AI_MODEL = 0, OS_AI_LEARNER };
enum { OS_ST_NONE = 0, OS_ST_VECTOR };    /* VECTOR: ESP-DL Model::test() on the file's embedded input (1 LSB) + one run */
typedef struct {
    const char *name;
    const char *job;                      /* the sched_mon job it runs as ("ml" is shared by both models) */
    uint8_t     kind;                     /* OS_AI_MODEL / OS_AI_LEARNER */
    uint32_t    period_ms, deadline_ms, stall_ms;
    uint16_t    share_pm;                 /* CPU reservation: per mille of a core, 0 = none */
    uint16_t    burst_ms;
    uint8_t     selftest;                 /* OS_ST_* */
    int8_t      fallback;                 /* OS_AI_* model to load when this one does not, -1 none */
    uint8_t     svc;                      /* os_svc_t: the runlevel mask that applies */
    uint8_t     prio;
    int8_t      core;
} os_ai_contract_t;
const os_ai_contract_t *os_ai_contract(int id);                        /* NULL for an unknown id */
/* The table's rules (host-tested): sane times, a reservation with a burst, fallbacks to models only and without a
   cycle, models that share a job declare the same timing. Returns the number of broken rules (why: the first). */
int os_ai_check(const os_ai_contract_t *t, int n, char *why, size_t why_n);

/* ---------- 6. UPS power state ---------- */
enum { OS_UPS_USB = 0, OS_UPS_GRACE, OS_UPS_OFF };
enum { OS_UPS_A_NONE = 0, OS_UPS_A_FLUSH_DIM = 1, OS_UPS_A_RESTORE = 2, OS_UPS_A_POWER_OFF = 4 };
typedef struct { int16_t discharge_ma; uint32_t grace_ms; uint16_t floor_mv; } os_ups_cfg_t;
typedef struct { uint8_t state, low_samples; uint32_t since_ms; } os_ups_t;
/* One gauge sample (every 2 s). ok = false: the gauge did not answer, nothing changes. Returns OS_UPS_A_* to do now. */
uint8_t os_ups_step(os_ups_t *u, const os_ups_cfg_t *c, bool ok, int16_t ma, uint16_t mv, uint32_t now_ms);
const char *os_ups_name(uint8_t state);

/* ---------- 7. degradation ladder ---------- */
enum { OS_DL_NORMAL = 0, OS_DL_UI_SLOW, OS_DL_NO_LEARNER, OS_DL_MODEL_OFF };
#define OS_DL_WINDOW_MS 10000
#define OS_DL_CLEAN     6
typedef struct { uint8_t level, clean; uint32_t last_misses; bool primed; } os_ladder_t;
/* one 10 s window: ml_misses = the ml job's miss counter so far. Returns the level now. */
uint8_t os_ladder_step(os_ladder_t *l, uint32_t ml_misses, bool driving);
const char *os_ladder_name(uint8_t level);

#ifdef __cplusplus
}
#endif
#endif
