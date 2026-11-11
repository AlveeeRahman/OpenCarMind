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

void os_tick(void)
{
    static uint8_t last = 0xFF;
    os_rl_in_t in;
    rl_inputs(&in);
    uint8_t a = os_rl_active(&in);
    if (a != last) {
        if (last != 0xFF) ESP_LOGI(TAG, "runlevel %s (active 0x%02x)", os_rl_name(os_rl_primary(a)), a);
        last = a;
    }
}
