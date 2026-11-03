#include <string.h>
#include "sys_mode.h"
#include "ml_infer.h"
#include "sched_mon.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "version.h"
#include "board.h"
#include "status.h"
#include "status_led.h"
#include "display.h"
#include "sdlog.h"
#include "obd_ble.h"
#include "elm327.h"
#include "wifi_sync.h"
#include "uploader.h"
#include "usb_door.h"
#include "cpu_monitor.h"
#include "remote_config.h"

static const char *TAG = "app";

app_status_t g_status;
SemaphoreHandle_t g_status_mutex;

void status_init(void)
{
    g_status_mutex = xSemaphoreCreateMutex();
    memset(&g_status, 0, sizeof g_status);
}

void status_get(app_status_t *out)
{
    STATUS_LOCK();
    *out = g_status;
    STATUS_UNLOCK();
}

/* BOOT button (GPIO0): tap = next page; hold 2 s = power off (deep sleep, BOOT wakes);
 * hold 8 s = forget the saved adapter. RESET is wired to the chip enable pin and always reboots. */
static void power_off(void)
{
    /* Logging already stopped when the hold passed 2 s; wait for the drive to be saved and closed, then sleep. */
    if (!elm_wait_stopped(4000)) ESP_LOGW(TAG, "poll loop still busy after 4 s; saving what is logged");
    ESP_LOGW(TAG, "powering off (deep sleep); press BOOT to wake");
    status_led_set(0, 0, 0);
    display_sleep();
    sdlog_prepare_sleep();
    while (gpio_get_level(BOARD_BUTTON) == 0) vTaskDelay(pdMS_TO_TICKS(50));   /* wait for release */
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_sleep_enable_ext0_wakeup(BOARD_BUTTON, 0);
    esp_deep_sleep_start();
}

static void set_hold(uint8_t pct, uint8_t stage)
{
    STATUS_LOCK(); g_status.hold_pct = pct; g_status.hold_stage = stage; STATUS_UNLOCK();
}

static void button_task(void *arg)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << BOARD_BUTTON, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
    int held_ms = 0;
    int64_t forget_prompt_until = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        bool down = gpio_get_level(BOARD_BUTTON) == 0;
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (down) {
            if (held_ms == 0) status_led_set(255, 255, 255);              /* press acknowledged at once */
            held_ms += 20;
            if (held_ms < 2000)      set_hold((uint8_t)(held_ms * 100 / 2000), 0);
            else if (held_ms < 8000) {
                if (held_ms == 2000) elm_request_stop();              /* power-off armed: stop logging now, not at release */
                set_hold(100, 1);
            }
            else                     set_hold(100, 2);
            continue;
        }
        if (held_ms == 0) continue;
        int held = held_ms;
        held_ms = 0;
        set_hold(0, 0);

        if (held < 1000) {
            /* tap: acknowledge a critical alarm, confirm a pending prompt, else next page */
            bool handled = false;
            STATUS_LOCK();
            if (g_status.alarm_text[0] && g_status.alarm_critical) { g_status.alarm_text[0] = 0; g_status.alarm_critical = false; handled = true; }
            else if (g_status.sys_popup[0] && now_ms < g_status.sys_popup_until_ms) { g_status.sys_popup[0] = 0; handled = true; }
            STATUS_UNLOCK();
            if (!handled && now_ms < forget_prompt_until) {
                forget_prompt_until = 0;
                STATUS_LOCK(); g_status.prompt_text[0] = 0; STATUS_UNLOCK();
                obd_ble_forget_adapter();
                handled = true;
            }
            if (!handled) display_next_page();
        } else if (held >= 2000 && held < 8000) {
            power_off();
        } else if (held >= 8000) {
            elm_resume();                                             /* not a power-off after all: logging may continue */
            forget_prompt_until = now_ms + 5000;
            STATUS_LOCK();
            snprintf(g_status.prompt_text, sizeof g_status.prompt_text, "Forget paired adapter?  tap = forget, wait = cancel");
            g_status.prompt_until_ms = forget_prompt_until;
            STATUS_UNLOCK();
        }
    }
}

/* RGB LED language (few fixed meanings, per the HIG haptics rules):
 *   colour  = the screen you are on: dashboard cyan, details blue, internals violet,
 *             faults amber (red when a HIGH/CRITICAL fault is present)
 *   pattern = the process: slow blink = searching for the adapter / waiting for the ECU,
 *             steady = logging
 *   overrides: steady red = critical alarm, fast red blink = adapter locked, white = button held,
 *              magenta flash = adapter forgotten */
static void led_task(void *arg)
{
    int tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        tick++;
        EXT_RAM_BSS_ATTR static app_status_t st;
        status_get(&st);
        if (st.hold_pct) continue;                                       /* button task owns the LED while held */

        int64_t now_ms = esp_timer_get_time() / 1000;
        if (st.alarm_text[0] && st.alarm_critical && now_ms < st.alarm_until_ms) { status_led_set(255, 0, 0); continue; }
        if (st.ble == BLE_ST_LOCKED) { status_led_set((tick / 2) % 2 ? 255 : 0, 0, 0); continue; }

        uint8_t r = 0, g = 0, b = 0;
        switch (display_current_page()) {
        case 0:  r = 0;   g = 211; b = 238; break;                       /* cyan */
        case 1:  r = 40;  g = 90;  b = 255; break;                       /* blue */
        case 2:  r = 150; g = 60;  b = 255; break;                       /* violet */
        case 3:                                                           /* faults */
            if (st.faults.max_severity >= SEV_HIGH) { r = 255; g = 0;   b = 0; }
            else                                    { r = 245; g = 158; b = 11; }
            break;
        default: r = 200; g = 200; b = 200; break;                        /* system: white */
        }
        float level = 1.0f;
        if (st.ble < BLE_ST_LOGGING) level = (tick / 5) % 2 ? 1.0f : 0.0f;                 /* slow blink: searching / init */
        status_led_set((uint8_t)(r * level), (uint8_t)(g * level), (uint8_t)(b * level));
    }
}

void app_main(void)
{
    sched_boot_check();               /* reset reason and boot-loop count, before anything can fail */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    remote_config_init();
    status_init();
    {
        /* the RTC keeps running through deep sleep and software resets: reuse a clock set earlier */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (tv.tv_sec > 1700000000) { STATUS_LOCK(); g_status.time_synced = true; STATUS_UNLOCK(); }
    }
    status_led_init();
    status_led_set(64, 64, 64);

    if (display_init() != ESP_OK) ESP_LOGE(TAG, "display init failed");
    if (sdlog_init() != ESP_OK) ESP_LOGE(TAG, "SD logging unavailable");
    remote_config_load_sd();          /* /sdcard/obd/config.json, if present */
    uploader_init();                  /* pairing with the desktop app, kept in NVS */
    sched_mon_start();                /* raises the "restarted after ..." event from sched_boot_check() */

    /* the model loads before BLE and Wi-Fi start, so the global small-allocation switch it needs (ml_infer.cpp)
       is on while the fewest other tasks allocate; it takes under 200 B of internal RAM either way */
    if (!sched_safe_mode()) ml_init();   /* after repeated abnormal resets, boot without the newest component */
    ESP_LOGI(TAG, "internal heap before BLE: %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_ERROR_CHECK(obd_ble_start());
    ESP_LOGI(TAG, "internal heap before Wi-Fi: %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    wifi_sync_start();
    ESP_LOGI(TAG, "internal heap after Wi-Fi: %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    sys_mode_start();                  /* DRIVE / SEEK arbitration, see sys_mode.h */
    usb_door_start();                  /* setup from the desktop app over the USB cable, see usb_door.h */
    ESP_LOGI(TAG, "internal heap after ML: %u (largest block %u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    cpu_monitor_start();
    xTaskCreate(button_task, "button", 4096, NULL, 3, NULL);
    xTaskCreate(led_task, "led", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "%s running", CARMIND_FW_STRING);
}
