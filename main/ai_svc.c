/* ai_svc: EdgeOS's small AI services (ESP-Mosaico, CONFIG_OCM_AI_SVC). See ai_svc.h and ai_svc_core.h.
 *
 * Scheduling. Every service runs in ONE worker task, pinned to the core with the most slack, at priority 1
 * (AI_SVC_PRIO): below every task with a deadline (ui 2, rev 2, ml 3, obd 5) and above idle. FreeRTOS's fixed-priority
 * preemption lets the UI and the models preempt the worker at any instruction. Inside the worker a release table
 * (ai_svc_core.c) picks the due service with the shortest deadline (deadline-monotonic); a step runs to its end.
 *   Why one worker and not one task per service: RAM and WCRT. One TCB and one 4 KB stack (in PSRAM, the worker never
 *   touches flash) for every service, instead of a stack each, and one sched_mon job slot of the 8. The price is that a
 *   service can wait for one step of another service (non-preemptive blocking). The admission covers it: the worker is
 *   analysed as one job with C = the sum of all budgets, T = the shortest period and D = the shortest deadline. When
 *   its response R <= D_min <= T_min, every service is released at most once inside that window, so the sum of the
 *   budgets bounds the demand of all of them, blocking included, and every service meets its own deadline.
 *   Admission: the response-time analysis of the models (os_admit, Joseph and Pandya 1986) over the jobs sched_mon has
 *   measured on that core, run when the boot health verdict is formed (about 65 s after boot, when sched_mon has data).
 *   The worker has the lowest priority, so adding it cannot make any other job late; the check is whether the worker
 *   itself meets D_min. A service whose self-test exceeds its budget is refused alone; if the worker is refused on both
 *   cores, the OS runs without AI services and the Board page says why.
 *
 * Soft real time. A late job is counted (missed), logged once per 10 s window and fed to the OS ladder's AI-side
 * input (os_core.h section 7: its first rung slows the services x10 with their learned heads off, before the UI or the
 * models shed anything; from the UI rung up they are paused). An overrun skips the
 * next release. A worker that falls behind runs the newest release only. Nothing is ever queued, so nothing piles up.
 *
 * Deadlock freedom, by construction:
 *   1. A step never blocks: it takes no mutex, waits on no queue, does no I/O and allocates nothing. The worker waits
 *      only in vTaskDelay between releases, holding nothing.
 *   2. Data in: single-writer snapshots (ai_snap.h seqlock; the gauge sample is written by the board task only). A
 *      reader never waits: a torn copy is retried at most AI_SEQ_TRIES times, then the last good copy is kept.
 *   3. Data out: a single-writer result struct per service (seqlock), read lock-free by the UI and the log line.
 *   4. Storage: a step hands rows to a single-producer single-consumer ring (drop and count when full); the board task,
 *      which already owns NVS and FatFs work, drains it. The worker never touches storage or I2C.
 *   5. The one lock a step path touches is sched_mon's spinlock (sched_begin / sched_end), a leaf held for a few field
 *      copies, never nested, never across a blocking call (sched_mon.h).
 *   6. No service calls or waits on another; services share nothing but the worker.
 *   So the worker never holds a resource while waiting for one (Coffman's hold-and-wait condition never holds), no
 *   wait-for cycle can include it, and it can never hold anything the UI or a model needs (no priority inversion).
 */
#include "ai_svc.h"
#if CONFIG_OCM_AI_SVC
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ai_snap.h"
#include "events.h"
#include "os.h"
#include "os_core.h"
#include "sched_mon.h"
#include "status.h"
#if CONFIG_OCM_BAT_EST
#include "bat_est.h"
#endif

static const char *TAG = "ai";

#define WORKER_STACK 4096

static const ai_svc_def_t *s_def[AI_SVC_MAX];
static ai_rt_t s_rt[AI_SVC_MAX];               /* written by the worker only, once it runs */
static uint32_t s_selftest_us[AI_SVC_MAX];
static uint8_t s_state[AI_SVC_MAX];
static int s_n;
static sched_job_t s_job = -1;
static volatile uint8_t s_level;               /* AI_DL_*: written by the board task (ladder), read by the worker */
static volatile int8_t s_core = -1;
static char s_why[72];
static uint32_t s_win_missed;                  /* board task: the services' misses at the last window */

/* the status the UI reads: one writer at a time (the board task before the worker exists, the worker after) */
EXT_RAM_BSS_ATTR static ai_svc_status_t s_pub;
static ai_seq_t s_pub_seq;

/* the gauge sample: written by the board task only */
static ai_gauge_sample_t s_gauge;
static ai_seq_t s_gauge_seq;

static void publish(void)
{
    EXT_RAM_BSS_ATTR static ai_svc_status_t st;   /* the publisher's scratch (one publisher at a time) */
    memset(&st, 0, sizeof st);
    st.level = s_level;
    st.core = s_core;
    st.n = (uint8_t)s_n;
    snprintf(st.why, sizeof st.why, "%s", s_why);
    for (int i = 0; i < s_n; i++) {
        ai_svc_stat_t *o = &st.s[i];
        const ai_rt_t *r = &s_rt[i];
        snprintf(o->name, sizeof o->name, "%s", s_def[i]->name);
        o->state = s_state[i];
        o->period_ms = r->period_ms; o->deadline_ms = r->deadline_ms; o->budget_us = r->budget_us;
        o->released = r->released; o->done = r->done; o->missed = r->missed; o->skipped = r->skipped;
        o->overruns = r->overruns; o->wcrt_us = r->wcrt_us; o->max_step_us = r->max_step_us;
        o->selftest_us = s_selftest_us[i];
    }
    AI_SEQ_PUT(&s_pub_seq, &s_pub, &st);
}

bool ai_svc_status(ai_svc_status_t *out)
{
    if (!AI_SEQ_GET(&s_pub_seq, out, &s_pub)) return false;
    out->level = s_level;                      /* the newest level, whoever published last */
    return true;
}

void ai_svc_gauge_publish(const mosaico_gauge_t *g)
{
    ai_gauge_sample_t v = { *g, esp_timer_get_time(), s_gauge.n + 1 };
    AI_SEQ_PUT(&s_gauge_seq, &s_gauge, &v);
}

bool ai_svc_gauge_read(ai_gauge_sample_t *out)
{
    return AI_SEQ_GET(&s_gauge_seq, out, &s_gauge) && out->n > 0;
}

static void worker(void *arg)
{
    (void)arg;
    uint8_t was = 0xFF;
    for (;;) {
        uint8_t lv = s_level;
        if (lv != was) {                       /* no stall alarm while paused; the stall limit covers the x10 period */
            sched_set_expected(s_job, lv != AI_DL_PAUSED);
            was = lv;
        }
        int64_t now = esp_timer_get_time(), wake;
        int i = ai_rt_pick(s_rt, s_n, now, lv, &wake);
        if (i < 0) {
            int64_t d = wake - now;
            TickType_t t = d <= 0 ? 1 : (TickType_t)((d + portTICK_PERIOD_MS * 1000 - 1) / (portTICK_PERIOD_MS * 1000));
            if (t > pdMS_TO_TICKS(1000)) t = pdMS_TO_TICKS(1000);   /* a level change is seen within a second */
            vTaskDelay(t ? t : 1);
            continue;
        }
        sched_begin(s_job);
        int64_t t0 = esp_timer_get_time();
        s_def[i]->step(s_rt[i].release_us);
        int64_t t1 = esp_timer_get_time();
        sched_end(s_job);
        ai_rt_finish(&s_rt[i], t0, t1, lv);
        publish();
    }
}

static int add(const ai_svc_def_t *d)
{
    if (s_n >= AI_SVC_MAX || !d || !d->step || !d->period_ms || !d->deadline_ms || d->deadline_ms > d->period_ms) return -1;
    int i = s_n++;
    s_def[i] = d;
    memset(&s_rt[i], 0, sizeof s_rt[i]);
    s_rt[i].name = d->name;
    s_rt[i].period_ms = d->period_ms;
    s_rt[i].deadline_ms = d->deadline_ms;
    s_rt[i].budget_us = d->budget_us;
    return i;
}

/* the response-time analysis of the worker on one core, with the jobs sched_mon has measured there */
static bool admit_on(int core, uint32_t c_us, uint32_t t_ms, uint32_t d_ms, os_admit_t *a)
{
    EXT_RAM_BSS_ATTR static sched_stats_t js[OS_RTA_MAX - 1];
    EXT_RAM_BSS_ATTR static os_rt_task_t set[OS_RTA_MAX - 1];
    int n = sched_get_stats(js, OS_RTA_MAX - 1), k = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(js[i].name, "ai")) continue;
        set[k++] = (os_rt_task_t){ js[i].name, js[i].cpu_max_us, js[i].period_ms * 1000u, js[i].deadline_ms * 1000u,
                                   js[i].prio, (int8_t)(js[i].runs ? js[i].core : -1) };
    }
    const os_rt_task_t cand = { "ai", 0, t_ms * 1000u, d_ms * 1000u, AI_SVC_PRIO, (int8_t)core };
    bool ok = os_admit(set, k, &cand, c_us, 1000, a);   /* the budgets are declared worst cases: factor 1.0 */
    ESP_LOGI(TAG, "admission of the AI worker on core %d: C %lu us (budgets), T %lu ms, D %lu ms, R %.2f ms, margin %.1f ms: %s",
             core, (unsigned long)c_us, (unsigned long)t_ms, (unsigned long)d_ms,
             a->r_us[a->n - 1] == OS_RTA_MISS ? -1.0 : a->r_us[a->n - 1] / 1000.0, a->margin_us / 1000.0, ok ? "admitted" : "REFUSED");
    return ok;
}

/* the core with the most slack: the response-time load sched_mon measured plus the CPU monitor's load */
static int pick_core(void)
{
    sched_system_t sys;
    sched_get_system(&sys);
    STATUS_LOCK();
    float l0 = sys.util[0] + g_status.cpu_pct[0] / 100.0f, l1 = sys.util[1] + g_status.cpu_pct[1] / 100.0f;
    STATUS_UNLOCK();
    ESP_LOGI(TAG, "core load: 0 %.3f, 1 %.3f (RTA U + CPU monitor)", l0, l1);
    return l1 <= l0 ? 1 : 0;
}

void ai_svc_start(void)
{
    static bool started;
    if (started) return;
    started = true;
#if CONFIG_OCM_BAT_EST
    add(bat_est_service());
#endif
    if (!s_n) return;
    if (sched_safe_mode()) {
        snprintf(s_why, sizeof s_why, "off in safe mode");
        for (int i = 0; i < s_n; i++) s_state[i] = AI_ST_SAFE;
        publish();
        ESP_LOGW(TAG, "safe mode: the AI services stay off");
        return;
    }
    for (int i = 0; i < s_n; i++) {
        if (s_def[i]->prepare) s_def[i]->prepare();
        s_selftest_us[i] = s_def[i]->selftest ? s_def[i]->selftest() : 0;
        bool ok = !s_def[i]->budget_us || s_selftest_us[i] <= s_def[i]->budget_us;
        s_rt[i].active = ok;
        s_state[i] = ok ? AI_ST_OFF : AI_ST_REFUSED;
        ESP_LOGI(TAG, "service %s: T %lu ms, D %lu ms, budget %lu us, self-test %lu us%s", s_def[i]->name,
                 (unsigned long)s_def[i]->period_ms, (unsigned long)s_def[i]->deadline_ms, (unsigned long)s_def[i]->budget_us,
                 (unsigned long)s_selftest_us[i], ok ? "" : ": REFUSED, over its budget");
        if (!ok) snprintf(s_why, sizeof s_why, "%s refused: self-test %lu us over %lu us", s_def[i]->name,
                          (unsigned long)s_selftest_us[i], (unsigned long)s_def[i]->budget_us);
    }
    uint32_t c, t, d;
    ai_rt_worker_load(s_rt, s_n, &c, &t, &d);
    if (!c && !t) { publish(); sys_event(EV_WARN, "AI services refused: %s", s_why); return; }
    int first = pick_core(), core = -1;
    EXT_RAM_BSS_ATTR static os_admit_t a;
    for (int k = 0; k < 2 && core < 0; k++) {
        int cc = k ? 1 - first : first;
        if (admit_on(cc, c, t, d, &a)) core = cc;
    }
    if (core < 0) {
        snprintf(s_why, sizeof s_why, "refused: the AI worker would miss its %lu ms deadline", (unsigned long)d);
        for (int i = 0; i < s_n; i++) if (s_rt[i].active) { s_rt[i].active = false; s_state[i] = AI_ST_REFUSED; }
        publish();
        sys_event(EV_WARN, "AI services %s", s_why);
        return;
    }
    s_job = sched_register("ai", t, d, false);
    sched_set_stall_ms(s_job, 3u * AI_DL_SLOW_FACTOR * t);
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_n; i++) if (s_rt[i].active) { ai_rt_start(&s_rt[i], now); s_state[i] = AI_ST_RUNNING; }
    s_core = (int8_t)core;
    s_why[0] = 0;
    publish();                                 /* the last publish from the board task: the worker owns it from here */
    if (xTaskCreatePinnedToCoreWithCaps(worker, "ai", WORKER_STACK, NULL, AI_SVC_PRIO, NULL, core, MALLOC_CAP_SPIRAM) != pdPASS) {
        snprintf(s_why, sizeof s_why, "no memory for the worker");
        for (int i = 0; i < s_n; i++) if (s_rt[i].active) { s_rt[i].active = false; s_state[i] = AI_ST_NOTASK; }
        s_core = -1;
        sched_set_expected(s_job, false);
        publish();
        sys_event(EV_ERROR, "AI services: %s", s_why);
        return;
    }
    ESP_LOGI(TAG, "AI worker on core %d, priority %d, %d service(s)", core, AI_SVC_PRIO, s_n);
}

uint32_t ai_svc_misses(void)
{
    ai_svc_status_t st;
    if (!ai_svc_status(&st)) return s_win_missed;
    uint32_t m = 0;
    for (int i = 0; i < st.n; i++) m += st.s[i].missed;
    if (m != s_win_missed) {                               /* once per window, whichever service */
        char b[96];
        int o = 0;
        b[0] = 0;
        for (int i = 0; i < st.n && o < (int)sizeof b - 24; i++)
            o += snprintf(b + o, sizeof b - (size_t)o, " %s %lu", st.s[i].name, (unsigned long)st.s[i].missed);
        ESP_LOGW(TAG, "AI deadline misses (total):%s; level %s", b, ai_dl_name(s_level));
    }
    s_win_missed = m;
    return m;
}

void ai_svc_set_os_level(uint8_t os_level) { s_level = ai_dl_from_os(os_level); }
uint8_t ai_svc_level(void) { return s_level; }

void ai_svc_board_tick(bool shutdown)
{
    for (int i = 0; i < s_n; i++) if (s_def[i]->board && s_state[i] == AI_ST_RUNNING) s_def[i]->board(shutdown);
}

int ai_svc_log_line(char *buf, size_t n)
{
    ai_svc_status_t st;
    if (!ai_svc_status(&st) || !st.n) return 0;
    int o = snprintf(buf, n, "ai core %d %s:", st.core, ai_dl_name(st.level));
    for (int i = 0; i < st.n && o < (int)n - 1; i++) {
        const ai_svc_stat_t *s = &st.s[i];
        o += snprintf(buf + o, n - (size_t)o, " %s rel=%lu done=%lu miss=%lu skip=%lu wcrt %.2fms budget %.2fms;", s->name,
                      (unsigned long)s->released, (unsigned long)s->done, (unsigned long)s->missed, (unsigned long)s->skipped,
                      s->wcrt_us / 1000.0, s->budget_us / 1000.0);
    }
    if (st.why[0] && o < (int)n - 1) o += snprintf(buf + o, n - (size_t)o, " %s", st.why);
    return o;
}
#endif
