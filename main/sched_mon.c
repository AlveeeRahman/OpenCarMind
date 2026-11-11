/* Soft real-time job monitor, see sched_mon.h.
 *
 * Task map (core, priority, period): rate-monotonic within each core, shorter period = higher priority.
 *   core 1: obd poll 5 (about 0.2 s), ml inference 3 (10 s window), led 2
 *   core 0: mode manager 5 (1 s), Wi-Fi clock sync 4 (I/O bound, not a scheduled job), cpumon 1, sched monitor 1 (5 s)
 * The ML task shares core 1 with the OBD task at a lower priority, so an inference can never delay a poll.
 *
 * Shared state is guarded by a spinlock critical section, not a mutex. Every critical section only copies a few
 * fields, never blocks and never calls out, so no task can wait on it for more than microseconds and it cannot take
 * part in a lock-order cycle. Anything that can block (watchdog calls, vTaskGetInfo, events, logging) runs outside.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "board.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "events.h"
#include "sched_mon.h"

static const char *TAG = "sched";

#define MAX_JOBS             8
#define MONITOR_MS           5000
#define SUMMARY_EVERY        12          /* monitor ticks: one summary line a minute */
#define STALL_PERIODS        3
#define SCHED_RESTART_FACTOR 3           /* an I/O job running this many deadlines is treated as hung */
#define SCHED_BOOTLOOP_LIMIT 3           /* abnormal resets in a row before safe mode */
#define STABLE_UPTIME_MS     (10 * 60 * 1000)
#define RTC_MAGIC            0xC0FFEE13u

typedef struct {
    sched_stats_t s;
    bool          watchdog, running, miss_flagged, stall_flagged;
    int64_t       start_us, last_end_us;
    uint32_t      stall_ms;      /* no run for this long while expected = a stall; 0 = STALL_PERIODS periods */
    uint32_t      cpu_start;
    TaskHandle_t  runner;
    /* CPU reservation bucket, in microseconds */
    int64_t       tokens_us, bucket_wall_us;
    uint32_t      bucket_cpu;
    TaskHandle_t  bucket_task;
} job_t;

static job_t         s_jobs[MAX_JOBS];
static volatile int  s_n;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
static sched_system_t s_sys;

/* survive a software reset, a panic and a watchdog reset; garbage after power-on (checked with the magic) */
static RTC_NOINIT_ATTR uint32_t s_rtc_magic;
static RTC_NOINIT_ATTR uint32_t s_rtc_abnormal;
static RTC_NOINIT_ATTR char     s_rtc_reason[24];

static uint32_t task_cpu_us(TaskHandle_t t)
{
    TaskStatus_t ts;
    vTaskGetInfo(t, &ts, pdFALSE, eRunning);       /* eRunning: skip the state lookup, we only need the counter */
    return (uint32_t)ts.ulRunTimeCounter;          /* esp_timer based: microseconds */
}

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt wdt";
    case ESP_RST_TASK_WDT:  return "task wdt";
    case ESP_RST_WDT:       return "other wdt";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_USB:       return "usb";
    default:                return "other";
    }
}

void sched_boot_check(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    if (s_rtc_magic != RTC_MAGIC || r == ESP_RST_POWERON) {
        s_rtc_magic = RTC_MAGIC;
        s_rtc_abnormal = 0;
        s_rtc_reason[0] = 0;
    }
    /* a software reset is abnormal only when the monitor itself asked for it (it leaves a reason behind) */
    bool abnormal = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
                    (r == ESP_RST_SW && s_rtc_reason[0]);
    s_rtc_abnormal = abnormal ? s_rtc_abnormal + 1 : 0;
    s_sys.boot_abnormal = s_rtc_abnormal;
    s_sys.safe_mode = s_rtc_abnormal >= SCHED_BOOTLOOP_LIMIT;
    if (r == ESP_RST_SW && s_rtc_reason[0]) snprintf(s_sys.last_reset, sizeof s_sys.last_reset, "%s", s_rtc_reason);
    else snprintf(s_sys.last_reset, sizeof s_sys.last_reset, "%s", reset_name(r));
    s_rtc_reason[0] = 0;
}

bool sched_safe_mode(void) { return s_sys.safe_mode; }

sched_job_t sched_register(const char *name, uint32_t period_ms, uint32_t deadline_ms, bool watchdog)
{
    int id = -1;
    taskENTER_CRITICAL(&s_mux);
    if (s_n < MAX_JOBS) {
        id = s_n;
        memset(&s_jobs[id], 0, sizeof s_jobs[id]);
        s_jobs[id].s.name = name;
        s_jobs[id].s.period_ms = period_ms;
        s_jobs[id].s.deadline_ms = deadline_ms;
        s_jobs[id].s.core = -1;
        s_jobs[id].watchdog = watchdog;
        s_jobs[id].last_end_us = esp_timer_get_time();
        s_n = id + 1;                                  /* published last: readers outside the lock see a full entry */
    }
    taskEXIT_CRITICAL(&s_mux);
    return id;
}

void sched_set_stall_ms(sched_job_t job, uint32_t stall_ms)
{
    if (job < 0 || job >= s_n) return;
    taskENTER_CRITICAL(&s_mux);
    s_jobs[job].stall_ms = stall_ms;
    taskEXIT_CRITICAL(&s_mux);
}

void sched_set_expected(sched_job_t job, bool expected)
{
    if (job < 0 || job >= s_n) return;
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    if (expected && !s_jobs[job].s.expected) s_jobs[job].last_end_us = now;   /* restart the stall clock */
    s_jobs[job].s.expected = expected;
    taskEXIT_CRITICAL(&s_mux);
}

void sched_begin(sched_job_t job)
{
    if (job < 0 || job >= s_n) return;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    uint32_t cpu = task_cpu_us(me);
    int core = xPortGetCoreID();
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    if (s_jobs[job].watchdog) esp_task_wdt_add(NULL);
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    job_t *j = &s_jobs[job];
    j->start_us = now;
    j->cpu_start = cpu;
    j->runner = me;
    j->s.core = (int8_t)core;
    j->s.prio = (uint8_t)prio;
    j->running = true;
    taskEXIT_CRITICAL(&s_mux);
}

void sched_end(sched_job_t job)
{
    if (job < 0 || job >= s_n) return;
    job_t *j = &s_jobs[job];
    if (j->watchdog) { esp_task_wdt_reset(); esp_task_wdt_delete(NULL); }
    uint32_t cpu_now = task_cpu_us(xTaskGetCurrentTaskHandle());
    int64_t now = esp_timer_get_time();
    bool recovered = false;
    taskENTER_CRITICAL(&s_mux);
    uint32_t us = (uint32_t)(now - j->start_us);
    uint32_t cpu = cpu_now - j->cpu_start;
    j->running = false;
    j->last_end_us = now;
    j->s.runs++;
    j->s.last_us = us;
    j->s.sum_us += us;
    if (us > j->s.max_us) j->s.max_us = us;
    j->s.cpu_last_us = cpu;
    j->s.cpu_sum_us += cpu;
    if (cpu > j->s.cpu_max_us) j->s.cpu_max_us = cpu;
    if (us > j->s.deadline_ms * 1000u) j->s.misses++;
    j->miss_flagged = false;
    if (j->stall_flagged) { j->stall_flagged = false; recovered = true; }
    taskEXIT_CRITICAL(&s_mux);
    if (recovered) sys_event(EV_INFO, "%s running again", j->s.name);
}

void sched_kick(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < s_n; i++) {
        /* racy read is fine: only the runner itself changes runner/running for its own job */
        if (s_jobs[i].running && s_jobs[i].runner == me && s_jobs[i].watchdog) { esp_task_wdt_reset(); return; }
    }
}

void sched_set_cpu_share(sched_job_t job, uint16_t share_permille, uint32_t burst_ms)
{
    if (job < 0 || job >= s_n) return;
    taskENTER_CRITICAL(&s_mux);
    job_t *j = &s_jobs[job];
    if (!j->s.share_permille) j->tokens_us = (int64_t)burst_ms * 1000;   /* a new reservation starts full */
    j->s.share_permille = share_permille;
    j->s.burst_ms = burst_ms;
    if (j->tokens_us > (int64_t)burst_ms * 1000) j->tokens_us = (int64_t)burst_ms * 1000;
    taskEXIT_CRITICAL(&s_mux);
}

bool sched_cpu_available(sched_job_t job)
{
    if (job < 0 || job >= s_n) return true;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    uint32_t cpu = task_cpu_us(me);
    int64_t now = esp_timer_get_time();
    bool ok;
    taskENTER_CRITICAL(&s_mux);
    job_t *j = &s_jobs[job];
    if (!j->s.share_permille) { taskEXIT_CRITICAL(&s_mux); return true; }
    if (j->bucket_task != me) {                     /* first call from this task: start accounting here */
        j->bucket_task = me;
        j->bucket_cpu = cpu;
        j->bucket_wall_us = now;
    }
    j->tokens_us += (now - j->bucket_wall_us) * j->s.share_permille / 1000;   /* earned */
    j->tokens_us -= (int64_t)(uint32_t)(cpu - j->bucket_cpu);                 /* spent */
    int64_t cap = (int64_t)j->s.burst_ms * 1000;
    if (j->tokens_us > cap) j->tokens_us = cap;
    /* debt is floored at one burst, so the longest pause after an expensive step is 2 x burst / share (work the task
       does outside its paced steps would otherwise push it into debt it never repays) */
    if (j->tokens_us < -cap) j->tokens_us = -cap;
    j->bucket_wall_us = now;
    j->bucket_cpu = cpu;
    j->s.tokens_ms = (int32_t)(j->tokens_us / 1000);
    ok = j->tokens_us > 0;
    if (!ok) j->s.throttles++;
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

int sched_get_stats(sched_stats_t *out, int max)
{
    int n = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_n && n < max; i++) out[n++] = s_jobs[i].s;
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

void sched_get_system(sched_system_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_sys;
    taskEXIT_CRITICAL(&s_mux);
}

/* Utilisation and response-time analysis per core, using each job's measured worst CPU time as C and its period as T.
   Only the monitored jobs are modelled; Wi-Fi, BLE and LVGL load shows up in the CPU monitor, not here. */
static void analyse(const job_t *jobs, int n, uint32_t wcrt[], sched_system_t *sys)
{
    bool ok = true;
    for (int c = 0; c < 2; c++) {
        int k = 0;
        float u = 0;
        for (int i = 0; i < n; i++) {
            const sched_stats_t *s = &jobs[i].s;
            if (s->core != c || s->runs == 0) continue;
            u += (float)s->cpu_max_us / (s->period_ms * 1000.0f);
            k++;
        }
        sys->util[c] = u;
        sys->rm_bound[c] = k ? k * (powf(2.0f, 1.0f / k) - 1.0f) : 1.0f;
    }
    for (int i = 0; i < n; i++) {
        const sched_stats_t *si = &jobs[i].s;
        wcrt[i] = 0;
        if (si->core < 0 || si->runs == 0) continue;
        uint64_t r = si->cpu_max_us, prev = 0;
        uint64_t limit = (uint64_t)si->deadline_ms * 1000;
        for (int it = 0; it < 50 && r != prev && r <= limit; it++) {
            prev = r;
            r = si->cpu_max_us;
            for (int j = 0; j < n; j++) {
                const sched_stats_t *sj = &jobs[j].s;
                if (j == i || sj->core != si->core || sj->runs == 0 || sj->prio <= si->prio) continue;
                uint64_t tj = (uint64_t)sj->period_ms * 1000;
                r += ((prev + tj - 1) / tj) * sj->cpu_max_us;
            }
        }
        wcrt[i] = r > UINT32_MAX ? UINT32_MAX : (uint32_t)r;
        if (r > limit) ok = false;
    }
    sys->schedulable = ok;
}

static void restart_hung(const char *name)
{
    snprintf(s_rtc_reason, sizeof s_rtc_reason, "%.14s hung", name);
    sys_event(EV_ERROR, "%s hung past %dx deadline, restarting", name, SCHED_RESTART_FACTOR);
    vTaskDelay(pdMS_TO_TICKS(200));                 /* let the log line out */
    esp_restart();
}

static void monitor_task(void *arg)
{
    (void)arg;
    static job_t snap[MAX_JOBS];
    TickType_t wake = xTaskGetTickCount();
    bool stable_noted = false;
    for (int tick = 1;; tick++) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(MONITOR_MS));        /* fixed-rate, no drift */
        int64_t now = esp_timer_get_time();
        if (!stable_noted && now / 1000 > STABLE_UPTIME_MS) { s_rtc_abnormal = 0; stable_noted = true; }

        int n;
        bool miss[MAX_JOBS] = {0}, stall[MAX_JOBS] = {0};
        const char *hung = NULL;
        taskENTER_CRITICAL(&s_mux);
        n = s_n;
        for (int i = 0; i < n; i++) {
            job_t *j = &s_jobs[i];
            int64_t run_us = now - j->start_us;
            if (j->running && !j->miss_flagged && run_us > (int64_t)j->s.deadline_ms * 1000) {
                j->miss_flagged = true;
                miss[i] = true;
            }
            if (j->running && !j->watchdog && run_us > (int64_t)SCHED_RESTART_FACTOR * j->s.deadline_ms * 1000)
                hung = j->s.name;
            if (!j->running && j->s.expected && !j->stall_flagged &&
                now - j->last_end_us > (j->stall_ms ? (int64_t)j->stall_ms : (int64_t)STALL_PERIODS * j->s.period_ms) * 1000) {
                j->stall_flagged = true;
                j->s.stalls++;
                stall[i] = true;
            }
            snap[i] = *j;
        }
        taskEXIT_CRITICAL(&s_mux);

        /* everything below runs without the lock */
        for (int i = 0; i < n; i++) {
            if (miss[i]) sys_event(EV_WARN, "%s past its %lu ms deadline", snap[i].s.name, (unsigned long)snap[i].s.deadline_ms);
            if (stall[i]) sys_event(EV_WARN, "%s stalled: no run in %d periods", snap[i].s.name, STALL_PERIODS);
        }
        if (hung) restart_hung(hung);

        uint32_t wcrt[MAX_JOBS];
        sched_system_t sys;
        taskENTER_CRITICAL(&s_mux);
        sys = s_sys;
        taskEXIT_CRITICAL(&s_mux);
        analyse(snap, n, wcrt, &sys);
        taskENTER_CRITICAL(&s_mux);
        for (int i = 0; i < n && i < s_n; i++) s_jobs[i].s.wcrt_us = wcrt[i];
        s_sys.util[0] = sys.util[0]; s_sys.util[1] = sys.util[1];
        s_sys.rm_bound[0] = sys.rm_bound[0]; s_sys.rm_bound[1] = sys.rm_bound[1];
        s_sys.schedulable = sys.schedulable;
        taskEXIT_CRITICAL(&s_mux);

        if (tick % SUMMARY_EVERY == 0) {
            char line[400];
            int o = 0;
            for (int i = 0; i < n && o < (int)sizeof line - 80; i++) {
                const sched_stats_t *s = &snap[i].s;
                o += snprintf(line + o, sizeof line - o, "%s n=%lu resp %.1f/%.1fms cpu %.2f/%.2fms wcrt %.2fms miss=%lu stall=%lu",
                              s->name, (unsigned long)s->runs, s->runs ? s->sum_us / 1000.0 / s->runs : 0.0, s->max_us / 1000.0,
                              s->runs ? s->cpu_sum_us / 1000.0 / s->runs : 0.0, s->cpu_max_us / 1000.0, wcrt[i] / 1000.0,
                              (unsigned long)s->misses, (unsigned long)s->stalls);
                if (s->share_permille)
                    o += snprintf(line + o, sizeof line - o, " share %.1f%% tokens %ldms paced=%lu", s->share_permille / 10.0,
                                  (long)s->tokens_ms, (unsigned long)s->throttles);
                o += snprintf(line + o, sizeof line - o, "; ");
            }
            ESP_LOGI(TAG, "%s", line);
            ESP_LOGI(TAG, "U core0 %.4f core1 %.4f (RM bound %.3f / %.3f), %s", sys.util[0], sys.util[1], sys.rm_bound[0],
                     sys.rm_bound[1], sys.schedulable ? "all WCRT within deadline" : "a WCRT exceeds its deadline");
        }
    }
}

void sched_mon_start(void)
{
#if BOARD_HAS_SD
    /* stack in PSRAM, as sys_mode.c: the monitor never runs with the flash cache disabled */
    xTaskCreatePinnedToCoreWithCaps(monitor_task, "sched", 4096, NULL, 1, NULL, 0, MALLOC_CAP_SPIRAM);
#else
    /* internal stack: restart_hung() calls esp_restart(), whose shutdown handler flushes the logs in the NOR flash */
    xTaskCreatePinnedToCore(monitor_task, "sched", 4096, NULL, 1, NULL, 0);
#endif
    if (s_sys.boot_abnormal)
        sys_event(s_sys.safe_mode ? EV_ERROR : EV_WARN, "Restarted after %s (%lu in a row)%s", s_sys.last_reset,
                  (unsigned long)s_sys.boot_abnormal, s_sys.safe_mode ? ", safe mode: ML off" : "");
}
