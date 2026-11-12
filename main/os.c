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
#include "driver/gpio.h"
#include "esp_sleep.h"
#if CONFIG_OCM_APP_ROLLBACK
#include "esp_ota_ops.h"
#endif
#include "soc/soc.h"
#include "sched_mon.h"
#include "sdlog.h"
#include "elm327.h"
#include "sys_mode.h"
#include "esp_timer.h"
#include "events.h"
#include "ml_infer.h"
#include "mosaico_ui.h"
#include "wifi_sync.h"
#if CONFIG_OCM_NAND_LOGS
#include "nand_store.h"
#endif

static const char *TAG = "os";

static volatile bool s_boot = true, s_resetting, s_saving, s_bat_saving;
static volatile uint8_t s_ui_req;
static volatile bool s_ml_paused;
static int s_ai_job[OS_AI_N] = { -1, -1, -1 };          /* the jobs os_ai_register made, for the ladder */
static volatile uint8_t s_updates;                      /* formats in progress (board task) */

/* ---------- resets ---------- */
#define RTC_OS_MAGIC 0x05A5E7u                          /* low 24 bits; the reason sits in the top byte */
static RTC_NOINIT_ATTR uint32_t s_rtc_last;             /* the reset the previous run asked for */
static RTC_NOINIT_ATTR char     s_rtc_detail[16];

/* SYS_RESET_POWER_OFF's last step: GPIO57 open-drain low is the SAM8108's power-off request (Mosaico V1.2 user guide);
   the safe pin state holds that pin, so the hold comes off first. If the board is still up, deep sleep with no wake
   source keeps it off until the POWER button or USB. */
#if CONFIG_OCM_UPS
static void power_off(void)
{
#if CONFIG_OCM_UPS_GPIO57
    gpio_hold_dis(57);
    gpio_config_t io = { .pin_bit_mask = 1ULL << 57, .mode = GPIO_MODE_OUTPUT_OD, .pull_up_en = 0, .pull_down_en = 0 };
    gpio_config(&io);
    gpio_set_level(57, 0);
    esp_rom_delay_us(500000);
#endif
    esp_deep_sleep_start();                     /* only with CONFIG_OCM_UPS: the sleep code costs about 12 KB of IRAM */
}
#endif

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
            }
#if CONFIG_OCM_UPS
            else if (p->method == OS_RST_POWER_OFF) power_off();
#endif
#if CONFIG_OCM_APP_ROLLBACK
            else if (p->method == OS_RST_ROLLBACK) esp_ota_mark_app_invalid_rollback_and_reboot();   /* resets */
#endif
            else if (p->method == OS_RST_ABORT) {
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
    in->saving = s_saving || s_bat_saving;
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
#if CONFIG_OCM_APP_ROLLBACK
    /* A/B rollback: an image the bootloader started as PENDING_VERIFY has this one boot to confirm itself */
    esp_ota_img_states_t st;
    bool pending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    uint8_t d = os_rollback_decision(pending, v);
    if (d == OS_RB_CONFIRM) ESP_LOGI(TAG, "app slot confirmed: %s", esp_ota_mark_app_valid_cancel_rollback() == ESP_OK ? "ok" : "FAILED");
    if (d == OS_RB_ROLLBACK) { sys_event(EV_ERROR, "Boot health bad: rolling back to the other app slot"); sys_reset(SYS_RESET_ROLLBACK, "health bad"); }
#endif
    /* the Wi-Fi boot hold (os_core.h section 3): app_main started no Wi-Fi; the verdict decides the automatic start */
    if (os_wifi_may_start(v, false)) wifi_sync_boot_release();
    else {
        ESP_LOGW(TAG, "Wi-Fi held: boot health %s; the Wi-Fi app (Sync clock now, search, join) starts it", os_health_name(v));
        sys_event(EV_WARN, "Wi-Fi held (boot health bad): start it in Wi-Fi");
    }
}

/* ---------- degradation ladder ---------- */
bool os_ml_paused(void) { return s_ml_paused; }

static void ladder_window(void)
{
    static os_ladder_t l;
    sched_stats_t js[8];
    int n = sched_get_stats(js, 8);
    uint32_t miss = 0;
    for (int i = 0; i < n; i++) if (!strcmp(js[i].name, "ml")) miss = js[i].misses;
    uint8_t was = l.level, lv = os_ladder_step(&l, miss, elm_drive_active());
    if (lv == was) return;
    if (lv >= OS_DL_UI_SLOW) s_ui_req |= OS_UI_SLOW; else s_ui_req &= (uint8_t)~OS_UI_SLOW;
    const os_ai_contract_t *c = os_ai_contract(OS_AI_REV_LEARNER);
    if (s_ai_job[OS_AI_REV_LEARNER] >= 0) sched_set_cpu_share(s_ai_job[OS_AI_REV_LEARNER], lv >= OS_DL_NO_LEARNER ? 1 : c->share_pm, c->burst_ms);
    s_ml_paused = lv >= OS_DL_MODEL_OFF;
    sys_event(lv > was ? EV_WARN : EV_INFO, "Model deadlines: %s (%lu misses)", os_ladder_name(lv), (unsigned long)miss);
}

void os_tick(void)
{
    static uint8_t last = 0xFF;
    static int64_t next_ladder_us = (int64_t)OS_DL_WINDOW_MS * 1000;
    int64_t now_us = esp_timer_get_time();
    if (s_health == OS_H_PENDING && now_us >= (int64_t)OS_H_VERDICT_MS * 1000) health_form();
    if (now_us >= next_ladder_us) { next_ladder_us = now_us + (int64_t)OS_DL_WINDOW_MS * 1000; ladder_window(); }
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
bool os_admit_model(int ai_id, uint32_t measured_us, char *why, size_t why_n)
{
    const os_ai_contract_t *c = os_ai_contract(ai_id);
    if (!c) { if (why) snprintf(why, why_n, "no contract"); return false; }
    const char *name = c->name;
    int core = c->core;
    /* in PSRAM, not on the caller's stack (app_main's is 3.5 KB): one caller at a time (above) */
    EXT_RAM_BSS_ATTR static sched_stats_t js[OS_RTA_MAX - 1];
    EXT_RAM_BSS_ATTR static os_rt_task_t set[OS_RTA_MAX - 1];
    EXT_RAM_BSS_ATTR static os_admit_t a;
    EXT_RAM_BSS_ATTR static char rl[96];
    int n = sched_get_stats(js, OS_RTA_MAX - 1), k = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(js[i].name, c->job)) continue;                /* the model's own job: replaced by the candidate */
        set[k++] = (os_rt_task_t){ js[i].name, js[i].cpu_max_us, js[i].period_ms * 1000u, js[i].deadline_ms * 1000u,
                                   js[i].prio, (int8_t)(js[i].runs ? js[i].core : -1) };
    }
    const os_rt_task_t cand = { c->job, 0, c->period_ms * 1000u, c->deadline_ms * 1000u, c->prio, c->core };
    bool ok = os_admit(set, k, &cand, measured_us, OS_ADMIT_FACTOR_PM, &a);
    int o = 0;
    rl[0] = 0;
    for (int i = 0; i < a.n && o < (int)sizeof rl - 1; i++) {
        const os_rt_task_t *t = i < k ? &set[i] : &cand;
        if (t->core != core) continue;
        if (a.r_us[i] == OS_RTA_MISS) o += snprintf(rl + o, sizeof rl - (size_t)o, " %s miss", t->name);
        else o += snprintf(rl + o, sizeof rl - (size_t)o, " %s %.1f/%lu ms", t->name, a.r_us[i] / 1000.0, (unsigned long)(t->d_us / 1000));
    }
    const char *worst = a.worst < k ? set[a.worst].name : c->job;
    ESP_LOGI(TAG, "admission %s: C %lu us (self-test %lu us x %u.%u) on core %d, R:%s, margin %.1f ms (%s): %s", name,
             (unsigned long)a.c_us, (unsigned long)measured_us, OS_ADMIT_FACTOR_PM / 1000, OS_ADMIT_FACTOR_PM % 1000 / 100,
             core, rl, a.margin_us / 1000.0, worst, ok ? "admitted" : "REFUSED");
    if (!ok && why) snprintf(why, why_n, "%s refused: %s would miss its deadline", name, worst);
    return ok;
}

/* ---------- AI service contract ---------- */
int os_ai_register(int ai_id, bool watchdog)
{
    const os_ai_contract_t *c = os_ai_contract(ai_id);
    if (!c) return -1;
    sched_job_t j = sched_register(c->job, c->period_ms, c->deadline_ms, watchdog);
    s_ai_job[ai_id] = j;
    if (c->stall_ms) sched_set_stall_ms(j, c->stall_ms);
    if (c->share_pm) sched_set_cpu_share(j, c->share_pm, c->burst_ms);   /* the token bucket, for any service that declares one */
    ESP_LOGI(TAG, "service %s: job %s, T %lu ms, D %lu ms, stall %lu ms, CPU %u per mille in %u ms bursts, prio %u core %d",
             c->name, c->job, (unsigned long)c->period_ms, (unsigned long)c->deadline_ms, (unsigned long)c->stall_ms,
             c->share_pm, c->burst_ms, c->prio, c->core);
    return j;
}

/* ---------- UPS power state ---------- */
uint8_t os_ui_request(void) { return s_ui_req; }

void os_power_sample(bool ok, int ma, int mv)
{
#if CONFIG_OCM_UPS
    static os_ups_t u;
    static const os_ups_cfg_t cfg = { CONFIG_OCM_UPS_DISCHARGE_MA, CONFIG_OCM_UPS_GRACE_S * 1000u, CONFIG_OCM_UPS_FLOOR_MV };
    uint8_t a = os_ups_step(&u, &cfg, ok, (int16_t)ma, (uint16_t)(mv > 0 ? mv : 0), (uint32_t)(esp_timer_get_time() / 1000));
    if (a & OS_UPS_A_FLUSH_DIM) {
        s_saving = true;                                   /* the Saving runlevel: every change is refused */
        s_ui_req |= OS_UI_DIM;
        sdlog_sync_now();
        sys_event(EV_WARN, "USB lost: on battery, %d mA, %d mV, off in %d s", ma, mv, CONFIG_OCM_UPS_GRACE_S);
    }
    if (a & OS_UPS_A_RESTORE) {
        s_saving = false;
        s_ui_req &= (uint8_t)~OS_UI_DIM;
        sys_event(EV_INFO, "USB power back");
    }
    if (a & OS_UPS_A_POWER_OFF) {
        s_saving = true;
        sys_event(EV_WARN, "Battery: powering off (%d mV)", mv);
        sys_reset(SYS_RESET_POWER_OFF, mv <= CONFIG_OCM_UPS_FLOOR_MV ? "voltage floor" : "battery grace");
    }
#else
    (void)ok; (void)ma; (void)mv;
#endif
}

/* ---------- battery safety ---------- */
static volatile uint8_t s_bat_state;
uint8_t os_battery_state(void) { return s_bat_state; }

void os_battery_sample(bool ok, int temp_dc, int mv, bool on_usb)
{
#if CONFIG_OCM_BATSAFE
    static os_bat_t b;
    static const os_bat_cfg_t cfg = { CONFIG_OCM_BATSAFE_WARN_C * 10, CONFIG_OCM_BATSAFE_HYST_C * 10, CONFIG_OCM_BATSAFE_CRIT_C * 10,
                                      CONFIG_OCM_BATSAFE_OV_MV, 2, 15 };    /* 2 samples = 4 s to act, 30 s cool before a restart */
    uint8_t a = os_bat_step(&b, &cfg, ok, (int16_t)temp_dc, (uint16_t)(mv > 0 ? mv : 0), on_usb);
    s_bat_state = b.state;
    if (a & OS_BAT_A_SHED) {
        s_bat_saving = true;                                /* the Saving runlevel: Wi-Fi and the learner stop */
        s_ui_req |= OS_UI_DIM;
        sys_event(EV_WARN, "Battery hot (%.1f C on USB): load shed", temp_dc / 10.0);
        /* the CPU clock stays: esp_pm (CONFIG_PM_ENABLE) is not part of this build */
        ESP_LOGW(TAG, "battery %.1f C on USB: load shed (Wi-Fi off, learner paused, screen dimmed)", temp_dc / 10.0);
    }
    if (a & OS_BAT_A_UNSHED) {
        s_bat_saving = false;
        if (!s_saving) s_ui_req &= (uint8_t)~OS_UI_DIM;
        sys_event(EV_INFO, "Battery cooled (%.1f C)", temp_dc / 10.0);
    }
    if (a & OS_BAT_A_STOP) {
        s_bat_saving = true;
        s_ui_req |= OS_UI_DIM;
        bool ov = b.state == OS_BAT_OVERVOLT;
        if (ov) sys_event(EV_ERROR, "Battery over-voltage (%d mV): charger fault, stopped", mv);
        else sys_event(EV_ERROR, "Battery too hot (%d C): stopped", temp_dc / 10);
        ESP_LOGE(TAG, "battery safety stop: %s, %d mV, %.1f C; logs flushed, storage stopped", os_bat_name(b.state), mv, temp_dc / 10.0);
        sdlog_sync_now();
#if CONFIG_OCM_BATSAFE_POWER_OFF
        sys_reset(SYS_RESET_POWER_OFF, ov ? "over-voltage" : "too hot");
#else
        sdlog_quiesce_for_reset();                          /* no write can be torn from here on; a restart resumes */
#endif
    }
    if (a & OS_BAT_A_RESUME) sys_reset(SYS_RESET_REBOOT, "battery cooled");
#else
    (void)ok; (void)temp_dc; (void)mv; (void)on_usb;
#endif
}
