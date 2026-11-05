#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include "esp_heap_caps.h"
#include "driver/temperature_sensor.h"
#include "esp_ipc.h"
#include "xtensa_perfmon_access.h"
#include "xtensa/xt_perf_consts.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "status.h"
#include "cpu_monitor.h"
#include "remote_config.h"

/* Power estimate. The board has no current sensor, so this is a model built from typical currents in
 * the Espressif ESP32-S3 datasheet (tables "Current Consumption in Modem-sleep Mode", "...for Wi-Fi in
 * Active Mode", "...for Bluetooth LE in Active Mode", https://documentation.espressif.com/esp32-s3_datasheet_en.pdf):
 *   CPU @240 MHz: 32.9 mA idle (WAITI) to 66.2 mA with both cores busy
 *   Wi-Fi: RX 91 mA, TX 283 mA (802.11n HT20)      BLE: RX 93 mA
 * Radio duty cycles below are assumptions for this firmware: BLE scan window 30 of 60 ms (50 %), a BLE
 * connection ~10 %, Wi-Fi connected with modem sleep ~10 % RX.
 * LCD with backlight is not in any datasheet we have: assumed 45 mA at full brightness.
 * Everything runs from a 5 V USB supply through the board's 3.3 V regulator, assumed 85 % efficient.
 * Treat the result as an indication, not a measurement; an INA219 on the I2C header would replace it. */
static float estimate_power_w(const app_status_t *st)
{
    float load = (st->cpu_pct[0] + st->cpu_pct[1]) / 200.0f;
    float ma = 32.9f + (66.2f - 32.9f) * load;
    if (st->ble == BLE_ST_SCANNING)      ma += 93.0f * 0.50f;
    else if (st->ble >= BLE_ST_CONNECTING) ma += 93.0f * 0.10f;
    if (st->wifi == WIFI_ST_CONNECTED || st->wifi == WIFI_ST_CONNECTING) ma += 91.0f * 0.10f;
    else if (st->wifi == WIFI_ST_SCANNING) ma += 91.0f * 0.03f;
    ma += 45.0f * remote_config_get()->backlight_pct / 100.0f;
    ma += 2.0f;                                   /* SD card idle, LED */
    return 3.3f * ma / 1000.0f / 0.85f;
}

#if CONFIG_CARMIND_TASK_STATS_S > 0
/* Diagnostic dump every CONFIG_CARMIND_TASK_STATS_S seconds: per-task CPU share over the interval, priority, core,
   state and the least stack it has had free, then internal RAM and PSRAM free, lowest-ever free and largest block.
   A steady fall in "lowest" across dumps is a leak; a task near 0 B stack free is about to overflow. Heap use is not
   attributed to tasks (that needs heap tracing). */
static void task_stats(const TaskStatus_t *snap, UBaseType_t n, uint32_t total)
{
    static TaskHandle_t prev_h[48];
    static uint32_t prev_rt[48], prev_total;
    static uint8_t order[48];
    uint32_t delta[48];
    uint32_t dt = total - prev_total;
    for (UBaseType_t i = 0; i < n && i < 48; i++) {
        uint32_t before = 0;
        for (int k = 0; k < 48; k++) if (prev_h[k] == snap[i].xHandle) { before = prev_rt[k]; break; }
        delta[i] = prev_total ? snap[i].ulRunTimeCounter - before : 0;
        order[i] = (uint8_t)i;
    }
    for (UBaseType_t i = 1; i < n && i < 48; i++)          /* insertion sort, busiest first */
        for (UBaseType_t j = i; j > 0 && delta[order[j]] > delta[order[j - 1]]; j--) {
            uint8_t t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    static const char STATE[] = "RrBSD?";                 /* running, ready, blocked, suspended, deleted */
    ESP_LOGI("tasks", "%u tasks over %lu ms (share of one core):", (unsigned)n, (unsigned long)(dt / 1000));
    for (UBaseType_t r = 0; r < n && r < 48 && prev_total; r++) {
        const TaskStatus_t *t = &snap[order[r]];
        BaseType_t core = xTaskGetCoreID(t->xHandle);
        ESP_LOGI("tasks", "  %-16s core %c prio %2u %c cpu %5.1f%%  stack free %5u B", t->pcTaskName,
                 core == tskNO_AFFINITY ? '*' : (char)('0' + core), (unsigned)t->uxCurrentPriority,
                 STATE[t->eCurrentState < 5 ? t->eCurrentState : 5], dt ? delta[order[r]] * 100.0f / dt : 0.0f,
                 (unsigned)t->usStackHighWaterMark);
    }
    ESP_LOGI("tasks", "internal free %u B, lowest %u B, largest block %u B | PSRAM free %u KB, lowest %u KB, largest %u KB",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    for (UBaseType_t i = 0; i < 48; i++) { prev_h[i] = i < n ? snap[i].xHandle : NULL; prev_rt[i] = i < n ? snap[i].ulRunTimeCounter : 0; }
    prev_total = total;
}
#endif

/* The ESP32-S3's built-in sensor reads the die, which runs warmer than the air around the board. The -10..80 C range
 * is the most accurate one, at most 1 C off (ESP-IDF esp_hal_ana_conv/esp32s3/temperature_sensor_periph.c). */
static temperature_sensor_handle_t s_tsens;

static float chip_temp(void)
{
    static bool tried;
    if (!tried) {
        tried = true;
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK || temperature_sensor_enable(s_tsens) != ESP_OK) s_tsens = NULL;
    }
    float c;
    return s_tsens && temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK ? c : NAN;
}

/* Hardware performance counters. Each Xtensa LX7 core has its own PMU, so it is programmed and read on that core
 * (core 1 through esp_ipc), with the ESP-IDF perfmon example's settings (kernelcnt 0, tracelevel -1). Counter 0 counts
 * cycles and checks the window (240 M per second at 240 MHz); counter 1 counts retired instructions. MIPS = instructions
 * per second, idle included, so it shows how much real work each core does. The data and instruction stall counters
 * read 0 on this chip and are not used. 32-bit counters wrap after ~17 s; they are read and cleared every second. */
typedef struct { uint32_t cyc, insn; } pmu_sample_t;

static void pmu_setup(void *arg)
{
    xtensa_perfmon_stop();
    xtensa_perfmon_init(0, XTPERF_CNT_CYCLES, XTPERF_MASK_CYCLES, 0, -1);
    xtensa_perfmon_init(1, XTPERF_CNT_INSN, XTPERF_MASK_INSN_ALL, 0, -1);
    xtensa_perfmon_start();
}

static void pmu_take(void *arg)
{
    pmu_sample_t *p = arg;
    xtensa_perfmon_stop();
    p->cyc = xtensa_perfmon_value(0); p->insn = xtensa_perfmon_value(1);
    xtensa_perfmon_reset(0); xtensa_perfmon_reset(1);
    xtensa_perfmon_start();
}

static void cpu_task(void *arg)
{
    EXT_RAM_BSS_ATTR static TaskStatus_t snap[40];   /* PSRAM: 1.6 KB internal RAM is worth more to TLS */
#if CONFIG_CARMIND_TASK_STATS_S > 0
    int stats_tick = 0;
#endif
    uint32_t prev_idle[2] = {0, 0};
    uint32_t prev_total = 0;
    pmu_setup(NULL);
    esp_ipc_call_blocking(1, pmu_setup, NULL);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t total = 0;
        UBaseType_t n = uxTaskGetSystemState(snap, 40, &total);
        uint32_t idle[2] = {0, 0};
        for (UBaseType_t i = 0; i < n; i++) {
            if (strcmp(snap[i].pcTaskName, "IDLE0") == 0 || strcmp(snap[i].pcTaskName, "IDLE") == 0) idle[0] += snap[i].ulRunTimeCounter;
            else if (strcmp(snap[i].pcTaskName, "IDLE1") == 0) idle[1] += snap[i].ulRunTimeCounter;
        }
        uint32_t dt = total - prev_total;
        if (prev_total && dt > 0) {
            /* total is the run-time clock (portGET_RUN_TIME_COUNTER_VALUE), not a sum over cores: each idle task can
               use at most dt of it */
            uint32_t per_core = dt;
            float temp_c = chip_temp();
            pmu_sample_t pm[2] = {0};
            pmu_take(&pm[0]);                                   /* this task is pinned to core 0 */
            esp_ipc_call_blocking(1, pmu_take, &pm[1]);
            STATUS_LOCK();
            g_status.chip_temp_c = temp_c;
            for (int c = 0; c < 2; c++)
                g_status.pmu_mips[c] = pm[c].cyc ? (float)pm[c].insn / pm[c].cyc * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ : 0;
            for (int c = 0; c < 2; c++) {
                uint32_t di = idle[c] - prev_idle[c];
                if (di > per_core) di = per_core;
                g_status.cpu_pct[c] = (uint8_t)(100 - (di * 100) / (per_core ? per_core : 1));
            }
            g_status.heap_free_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
            g_status.psram_free_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
            g_status.psram_total_kb = (uint32_t)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024);
            g_status.power_w = estimate_power_w(&g_status);
            STATUS_UNLOCK();
        }
        prev_total = total;
        prev_idle[0] = idle[0];
        prev_idle[1] = idle[1];
#if CONFIG_CARMIND_TASK_STATS_S > 0
        if (++stats_tick >= CONFIG_CARMIND_TASK_STATS_S) { stats_tick = 0; task_stats(snap, n, total); }
#endif
        if (n == 0) ESP_LOGW("cpumon", "more than 40 tasks: run-time snapshot skipped");
    }
}

void cpu_monitor_start(void)
{
    xTaskCreatePinnedToCore(cpu_task, "cpumon", 3072, NULL, 1, NULL, 0);
}
