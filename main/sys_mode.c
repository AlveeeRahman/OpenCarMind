/* System mode manager, see sys_mode.h. One low-priority task re-evaluates the inputs once a second. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "status.h"
#include "events.h"
#include "sys_mode.h"
#if CONFIG_IDF_TARGET_ESP32S31
#include "os_core.h"
#endif

static const char *TAG = "mode";

static volatile sys_mode_t s_mode = MODE_SEEK;

sys_mode_t sys_mode_get(void) { return s_mode; }

const char *sys_mode_name(sys_mode_t m)
{
    return m == MODE_DRIVE ? "DRIVE" : "SEEK";
}

static void mode_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000));
        STATUS_LOCK();
#if CONFIG_IDF_TARGET_ESP32S31
        /* EdgeOS (owner 2026-11-11): DRIVE, and with it the Driving runlevel, only once the car's computer answers. The
           attempts to link (a neighbour's adapter tried 35 times in 13 min) kept the board in Driving and refused the
           model switch, the clock and the format. */
        bool link = os_rl_link_input(g_status.ble);
#else
        /* Connecting and discovering count (short, and Wi-Fi scans would slow them), and so does logging. Waiting for the
           ECU does not: an adapter left powered with the ignition off must not keep the board in DRIVE. */
        bool link = g_status.ble == BLE_ST_CONNECTING || g_status.ble == BLE_ST_DISCOVERING || g_status.ble == BLE_ST_LOGGING;
#endif
        STATUS_UNLOCK();
        sys_mode_t target = link ? MODE_DRIVE : MODE_SEEK;
        if (target != s_mode) {
            sys_event(EV_INFO, "Mode %s -> %s", sys_mode_name(s_mode), sys_mode_name(target));
            s_mode = target;
        }
    }
}

void sys_mode_start(void)
{
    /* stack in PSRAM: the task only reads status and never runs with the flash cache disabled */
    xTaskCreatePinnedToCoreWithCaps(mode_task, "mode", 3072, NULL, 5, NULL, 0, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "mode manager started (SEEK)");
}
