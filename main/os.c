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

static const char *TAG = "os";

/* ---------- resets ---------- */
#define RTC_OS_MAGIC 0x05A5E7u                          /* low 24 bits; the reason sits in the top byte */
static RTC_NOINIT_ATTR uint32_t s_rtc_last;             /* the reset the previous run asked for */
static RTC_NOINIT_ATTR char     s_rtc_detail[16];

void sys_reset(sys_reset_reason_t reason, const char *detail)
{
    const os_reset_plan_t *p = os_reset_plan(reason);
    uint8_t st[5];
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
    uint32_t v = s_rtc_last;
    s_rtc_last = 0;
    if ((v & 0xFFFFFFu) != RTC_OS_MAGIC) return;
    const os_reset_plan_t *p = os_reset_plan((sys_reset_reason_t)(v >> 24));
    s_rtc_detail[sizeof s_rtc_detail - 1] = 0;
    if (p) ESP_LOGI(TAG, "the last run ended with sys_reset(%s%s%s)", p->name, s_rtc_detail[0] ? ": " : "", s_rtc_detail);
}
