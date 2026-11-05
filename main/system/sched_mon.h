/* Soft real-time job monitor.
 *
 * Every periodic activity on the board (OBD poll cycle, ML inference) is a "job" with a period and a
 * deadline. A job brackets each run with sched_begin() / sched_end(). The monitor records, per job:
 *   - response time (wall clock from begin to end: mean, max),
 *   - CPU time (the runner task's FreeRTOS run-time counter over the run: mean, max), which is the C in the
 *     schedulability analysis, as opposed to time spent blocked on BLE, SD or the network,
 *   - deadline misses, and stalls (an expected job that has not run for 3 periods).
 * Once a minute it logs a summary plus, per core, the utilisation U = sum(C_max / T) against the rate-monotonic bound
 * n(2^(1/n) - 1) (Liu and Layland 1973) and the worst-case response time of each job from response-time analysis
 * R = C_i + sum over higher-priority jobs on the same core of ceil(R / T_j) * C_j (Joseph and Pandya 1986).
 *
 * Hang handling, without deadlocks:
 *   - Jobs with watchdog=true are subscribed to the ESP-IDF task watchdog only while a run is in progress. Long runs
 *     made of bounded steps (each ELM transaction is at most CONFIG_OBD_ELM_TIMEOUT_MS) call sched_kick() after each
 *     step, so the watchdog fires on a step that never returns, not on a slow but healthy cycle.
 *   - I/O jobs (watchdog=false) that stay running past SCHED_RESTART_FACTOR x deadline get a logged, orderly restart.
 *   - Reset reasons are kept across reboots. After SCHED_BOOTLOOP_LIMIT abnormal resets in a row the board boots in
 *     safe mode (sched_safe_mode() is true) and optional components, such as the model, stay off.
 *   - Locks: the monitor's lock is a leaf. Nothing is called while holding it that takes another lock (events are
 *     raised after it is released) and every take is bounded, so it cannot be part of a lock-order cycle.
 *
 * Priorities follow rate-monotonic order per core: the shorter the period, the higher the priority (see sched_mon.c).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int sched_job_t;

typedef struct {
    const char *name;
    uint32_t    runs, misses, stalls, max_us, last_us;
    uint64_t    sum_us;
    uint32_t    period_ms, deadline_ms;
    bool        expected;
    /* CPU time of the runner task per run (us), and where it runs */
    uint32_t    cpu_last_us, cpu_max_us;
    uint64_t    cpu_sum_us;
    int8_t      core;           /* -1 until the first run */
    uint8_t     prio;
    uint32_t    wcrt_us;        /* worst-case response time from the last analysis, 0 if not analysed */
    /* CPU reservation (0 = none): the job may use share_permille of one core on average, with bursts up to
       burst_ms; tokens_ms is what it has left now (negative = in debt), throttles counts the times it was paced */
    uint16_t    share_permille;
    uint32_t    burst_ms;
    int32_t     tokens_ms;
    uint32_t    throttles;
} sched_stats_t;

typedef struct {
    float    util[2];           /* sum(C_max / T) per core over expected jobs */
    float    rm_bound[2];       /* n(2^(1/n) - 1) for the jobs counted on that core */
    bool     schedulable;       /* every analysed job's WCRT <= its deadline */
    uint32_t boot_abnormal;     /* abnormal resets in a row before this boot */
    bool     safe_mode;
    char     last_reset[24];
} sched_system_t;

void        sched_mon_start(void);
sched_job_t sched_register(const char *name, uint32_t period_ms, uint32_t deadline_ms, bool watchdog);
void        sched_begin(sched_job_t job);
void        sched_end(sched_job_t job);
/* Feed the task watchdog from inside a run, after each bounded step. No-op if the caller is not running a job. */
void        sched_kick(void);
/* Whether the job should currently be running periodically (e.g. the OBD poll only while an adapter is logging). */
void        sched_set_expected(sched_job_t job, bool expected);
/* Override the stall limit (default STALL_PERIODS periods). For jobs paced by something slower than their period. */
void        sched_set_stall_ms(sched_job_t job, uint32_t stall_ms);
int         sched_get_stats(sched_stats_t *out, int max);
/* CPU reservation, a token bucket over CPU time in the spirit of a constant bandwidth server: the calling task earns
   share_permille/1000 us of CPU per us of wall time, up to burst_ms, and spends the CPU it actually uses (its FreeRTOS
   run-time counter). Aperiodic background work checks sched_cpu_available() before each bounded step and
   stops when it returns false, so it can never take more than its share however much work is queued. */
void        sched_set_cpu_share(sched_job_t job, uint16_t share_permille, uint32_t burst_ms);
bool        sched_cpu_available(sched_job_t job);
void        sched_get_system(sched_system_t *out);
/* Call first thing in app_main: reads the reset reason, counts boot loops, decides safe mode. */
void        sched_boot_check(void);
bool        sched_safe_mode(void);

#ifdef __cplusplus
}
#endif
