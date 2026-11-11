/* os.c: the mossREV OS layer's firmware glue on the ESP-Mosaico. See os.h and os_core.h. */
#include "os.h"
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"
#include "sched_mon.h"
#include "sdlog.h"
#include "elm327.h"
#include "sys_mode.h"
#include "esp_timer.h"
#include "events.h"
#include "ml_infer.h"
#include "mosaico_ui.h"
#if CONFIG_OCM_NAND_LOGS
#include "nand_store.h"
#endif

static const char *TAG = "os";

static volatile bool s_boot = true, s_resetting;
static volatile uint8_t s_updates;                      /* formats in progress (board task) */

/* ---------- resets ---------- */
#define RTC_OS_MAGIC 0x05A5E7u                          /* low 24 bits; the reason sits in the top byte */
static RTC_NOINIT_ATTR uint32_t s_rtc_last;             /* the reset the previous run asked for */
static RTC_NOINIT_ATTR char     s_rtc_detail[16];

void sys_reset(sys_reset_reason_t reason, const char *detail)
{
    const os_reset_plan_t *p = os_reset_plan(reason);
    uint8_t st[5];
    s_resetting = true;                                 /* Update runlevel: every change is refused from here on */
    int n = os_reset_steps(reason, st);
    if (!p) { p = os_reset_plan(SYS_RESET_REBOOT); n = os_reset_steps(SYS_RESET_REBOOT, st); }
    s_rtc_last = RTC_OS_MAGIC | (uint32_t)reason << 24;
    snprintf(s_rtc_detail, sizeof s_rtc_detail, "%s", detail ? detail : "");
    for (int i = 0; i < n; i++) {
        switch (st[i]) {
        case OS_STEP_WAIT:    vTaskDelay(pdMS_TO_TICKS(p->pre_ms)); break;
        case OS_STEP_MARK:    sched_mark_intentional_reset(); break;
        case OS_STEP_QUIESCE: sdlog_quiesce_for_reset(); break;   /* the NOR FAT, then the NAND */
        case OS_STEP_SETTLE:  esp_rom_delay_us(p->post_us); break;
        case OS_STEP_RESET:
            if (p->method == OS_RST_ROM_DOWNLOAD) {
                REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
                esp_rom_software_reset_system();   /* the whole chip: esp_restart() resets only the CPU cores here */
            } else if (p->method == OS_RST_ABORT) {
                esp_system_abort(detail ? detail : p->name);   /* a panic: the core dump holds every task */
            }
            esp_restart();
        }
    }
    esp_restart();
}

void os_boot_done(void)
{
    s_boot = false;
    ESP_LOGI(TAG, "runlevel %s", os_rl_name(os_runlevel()));
    uint32_t v = s_rtc_last;
    s_rtc_last = 0;
    if ((v & 0xFFFFFFu) != RTC_OS_MAGIC) return;
    const os_reset_plan_t *p = os_reset_plan((sys_reset_reason_t)(v >> 24));
    s_rtc_detail[sizeof s_rtc_detail - 1] = 0;
    if (p) ESP_LOGI(TAG, "the last run ended with sys_reset(%s%s%s)", p->name, s_rtc_detail[0] ? ": " : "", s_rtc_detail);
}

/* ---------- runlevels ---------- */
static void rl_inputs(os_rl_in_t *in)
{
    in->boot = s_boot;
    in->safe = sched_safe_mode();
    in->drive = elm_drive_active();
    in->link = sys_mode_get() == MODE_DRIVE;
    in->saving = false;
    in->update = s_resetting || s_updates;
}

bool os_allows(os_svc_t svc)
{
    os_rl_in_t in;
    rl_inputs(&in);
    return os_rl_allows(os_rl_active(&in), svc);
}

const char *os_refusal(os_svc_t svc)
{
    os_rl_in_t in;
    rl_inputs(&in);
    os_runlevel_t b = os_rl_blocker(os_rl_active(&in), svc);
    return b == OS_RL_N ? NULL : os_rl_phrase(b, &in);
}

os_runlevel_t os_blocker(os_svc_t svc)
{
    os_rl_in_t in;
    rl_inputs(&in);
    return os_rl_blocker(os_rl_active(&in), svc);
}

os_runlevel_t os_runlevel(void)
{
    os_rl_in_t in;
    rl_inputs(&in);
    return os_rl_primary(os_rl_active(&in));
}

void os_update_begin(void) { s_updates++; }
void os_update_end(void) { if (s_updates) s_updates--; }

/* ---------- boot health ---------- */
static volatile uint8_t s_health;
static volatile uint16_t s_health_flags;

uint8_t os_health(uint16_t *flags)
{
    if (flags) *flags = s_health_flags;
    return s_health;
}

static void health_form(void)
{
    os_health_in_t in = { 0 };
    sched_stats_t js[8];
    sched_system_t sys;
    int n = sched_get_stats(js, 8);
    sched_get_system(&sys);
    in.selftest = (int8_t)ml_selftest_state();
    in.model_expected = os_allows(OS_SVC_ML) && (ml_model_available(ML_MODEL_EV) || ml_model_available(ML_MODEL_REV));
    in.nor = (int8_t)sdlog_flash_health(&in.nor_repairs, &in.nor_warn);
#if CONFIG_OCM_NAND_LOGS
    nand_state_t ns = nand_store_state();
    in.nand = ns == NAND_MOUNTED ? 1 : ns == NAND_ABSENT ? 0 : -1;
#else
    in.nand = 2;
#endif
    in.first_tick_ms = mosaico_ui_first_tick_ms();
    in.touch_ok = mosaico_touch_ok();
    for (int i = 0; i < n; i++) { in.misses += js[i].misses; in.stalls += js[i].stalls; }
    in.schedulable = sys.schedulable;
    in.safe = sys.safe_mode;
    uint16_t f = 0;
    uint8_t v = os_health_verdict(&in, &f);
    s_health_flags = f;
    s_health = v;
    char t[160];
    os_health_text(v, f, t, sizeof t);
    ESP_LOGI(TAG, "boot health %s | model self-test %d, flash FAT %d (%u repaired%s), NAND %d, first UI tick %lu ms, touch %s, "
             "%lu misses, %lu stalls, %sschedulable, runlevel %s", t, in.selftest, in.nor, (unsigned)in.nor_repairs,
             in.nor_warn ? ", audit warning" : "", in.nand, (unsigned long)in.first_tick_ms, in.touch_ok ? "ok" : "failing",
             (unsigned long)in.misses, (unsigned long)in.stalls, in.schedulable ? "" : "not ", os_rl_name(os_runlevel()));
    sys_event(v == OS_H_GOOD ? EV_INFO : v == OS_H_DEGRADED ? EV_WARN : EV_ERROR, "Boot health: %s", t);
}

void os_tick(void)
{
    static uint8_t last = 0xFF;
    if (s_health == OS_H_PENDING && esp_timer_get_time() >= (int64_t)OS_H_VERDICT_MS * 1000) health_form();
    os_rl_in_t in;
    rl_inputs(&in);
    uint8_t a = os_rl_active(&in);
    if (a != last) {
        if (last != 0xFF) ESP_LOGI(TAG, "runlevel %s (active 0x%02x)", os_rl_name(os_rl_primary(a)), a);
        last = a;
    }
}

/* ---------- model admission ---------- */
/* Called with the model lock held (ml_select_model) or before any other task can switch (ml_init), so the static
   buffers below are never shared. */
bool os_admit_model(const char *name, uint32_t measured_us, uint32_t period_ms, uint32_t deadline_ms, uint8_t prio,
                    int core, char *why, size_t why_n)
{
    /* in PSRAM, not on the caller's stack (app_main's is 3.5 KB): one caller at a time (above) */
    EXT_RAM_BSS_ATTR static sched_stats_t js[OS_RTA_MAX - 1];
    EXT_RAM_BSS_ATTR static os_rt_task_t set[OS_RTA_MAX - 1];
    EXT_RAM_BSS_ATTR static os_admit_t a;
    EXT_RAM_BSS_ATTR static char rl[96];
    int n = sched_get_stats(js, OS_RTA_MAX - 1), k = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(js[i].name, "ml")) continue;                  /* the model's own job: replaced by the candidate */
        set[k++] = (os_rt_task_t){ js[i].name, js[i].cpu_max_us, js[i].period_ms * 1000u, js[i].deadline_ms * 1000u,
                                   js[i].prio, (int8_t)(js[i].runs ? js[i].core : -1) };
    }
    const os_rt_task_t cand = { "ml", 0, period_ms * 1000u, deadline_ms * 1000u, prio, (int8_t)core };
    bool ok = os_admit(set, k, &cand, measured_us, OS_ADMIT_FACTOR_PM, &a);
    int o = 0;
    rl[0] = 0;
    for (int i = 0; i < a.n && o < (int)sizeof rl - 1; i++) {
        const os_rt_task_t *t = i < k ? &set[i] : &cand;
        if (t->core != core) continue;
        if (a.r_us[i] == OS_RTA_MISS) o += snprintf(rl + o, sizeof rl - (size_t)o, " %s miss", t->name);
        else o += snprintf(rl + o, sizeof rl - (size_t)o, " %s %.1f/%lu ms", t->name, a.r_us[i] / 1000.0, (unsigned long)(t->d_us / 1000));
    }
    const char *worst = a.worst < k ? set[a.worst].name : "ml";
    ESP_LOGI(TAG, "admission %s: C %lu us (self-test %lu us x %u.%u) on core %d, R:%s, margin %.1f ms (%s): %s", name,
             (unsigned long)a.c_us, (unsigned long)measured_us, OS_ADMIT_FACTOR_PM / 1000, OS_ADMIT_FACTOR_PM % 1000 / 100,
             core, rl, a.margin_us / 1000.0, worst, ok ? "admitted" : "REFUSED");
    if (!ok && why) snprintf(why, why_n, "%s refused: %s would miss its deadline", name, worst);
    return ok;
}
