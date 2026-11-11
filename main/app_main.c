#include <string.h>
#include "sys_mode.h"
#include "ml_infer.h"
#include "rev_task.h"
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
#include "units.h"
#include "wifi_sync.h"
#include "uploader.h"
#include "usb_door.h"
#include "cpu_monitor.h"
#include "storage_rt.h"
#include "remote_config.h"
#if BOARD_MOSAICO
#include "mosaico_console.h"
#include "mosaico_board.h"
#include "mosaico_ui.h"
#endif

static const char *TAG = "app";

app_status_t g_status;
SemaphoreHandle_t g_status_mutex;

void status_init(void)
{
    g_status_mutex = xSemaphoreCreateMutex();
    memset(&g_status, 0, sizeof g_status);
    g_status.obd.fuel_lph_calc = g_status.obd.econ_trip_kpl = g_status.obd.econ_inst_kpl = -1;   /* unknown until a row */
    g_status.obd.fuel_lph_raw = -1;
    g_status.obd.fuel_cal = 1.0f;
}

void status_get(app_status_t *out)
{
    STATUS_LOCK();
    *out = g_status;
    STATUS_UNLOCK();
}

/* BOOT button (GPIO0): tap = next page; hold 2 s = power off (deep sleep, BOOT wakes);
 * hold 8 s = forget the saved adapter. RESET is wired to the chip enable pin and always reboots. */
static void set_shutdown(uint8_t stage)
{
    STATUS_LOCK(); g_status.shutdown_stage = stage; STATUS_UNLOCK();
}

/* Power-off you can see: "Turning off / Saving the drive" (LED breathes amber, caution: do not unplug) until the drive and
   the learned state are on the card, then "Off / Safe to unplug" for 1.5 s, then the backlight fades to black and the LED
   goes dark. Both pins are held low through deep sleep, so neither lights up again until BOOT wakes the board. */
static void power_off(void)
{
#if BOARD_MOSAICO
    /* no deep-sleep power-off from the key on the ESP-Mosaico (holds are ignored in button_task) */
    ESP_LOGW(TAG, "power-off is not mapped on the ESP-Mosaico");
    return;
#else
    set_shutdown(1);
    /* Logging already stopped when the hold passed 2 s; wait for the drive to be saved and closed. */
    if (!elm_wait_stopped(4000)) ESP_LOGW(TAG, "poll loop still busy after 4 s; saving what is logged");
    rev_flush(2000);                   /* the learned fuel state, before the card is unmounted */
    sdlog_prepare_sleep();
    set_shutdown(2);
    ESP_LOGW(TAG, "powering off (deep sleep); press BOOT to wake");
    vTaskDelay(pdMS_TO_TICKS(1500));   /* long enough to read "Off" */
    while (gpio_get_level(BOARD_BUTTON) == 0) vTaskDelay(pdMS_TO_TICKS(50));   /* wait for release */
    display_fade_off(600);
    status_led_off_for_sleep();
    gpio_deep_sleep_hold_en();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_sleep_enable_ext0_wakeup(BOARD_BUTTON, 0);
    esp_deep_sleep_start();
#endif
}

static void set_hold(uint8_t pct, uint8_t stage)
{
    STATUS_LOCK(); g_status.hold_pct = pct; g_status.hold_stage = stage; STATUS_UNLOCK();
}

#define DOUBLE_MS      450     /* a second tap within this is a double-click */
#define AUTO_SEARCH_MS 60000   /* no engine data this long: automatic connection search */
#define AUTO_LINKED_MS 40000   /* a fresh adapter link gets this long to start on its own first */

static void button_task(void *arg)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << BOARD_BUTTON, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
    int held_ms = 0;
    int64_t forget_prompt_until = 0;
    int64_t last_tap_ms = 0;
    int64_t bad_since = 0, last_nag = -60000, last_check = 0;
    uint32_t auto_epoch = 0;           /* adapter connection the automatic search last ran for */
    bool auto_scan_done = false;       /* the automatic adapter search runs once per boot */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        bool down = gpio_get_level(BOARD_BUTTON) == 0;
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_check >= 1000) {
            last_check = now_ms;
            STATUS_LOCK();
            bool ok = g_status.ble == BLE_ST_LOGGING && (OBD_HAS(&g_status.obd, F_rpm) || OBD_HAS(&g_status.obd, F_speed));
            bool prompt_busy = g_status.prompt_text[0] && now_ms < g_status.prompt_until_ms;
            STATUS_UNLOCK();
            if (ok || elm_exploring()) bad_since = 0;
            else if (!bad_since) bad_since = now_ms;
            /* automatic search after AUTO_SEARCH_MS without engine data: once per adapter connection, or once per boot
               when no adapter is linked. The double-click starts the same search by hand at any time. */
            if (bad_since && now_ms - bad_since > AUTO_SEARCH_MS && !elm_exploring()) {
                if (elm_link_is_up()) {
                    uint32_t ep = elm_link_epoch();
                    if (now_ms - elm_link_up_ms() >= AUTO_LINKED_MS && ep != auto_epoch) {
                        auto_epoch = ep;
                        ESP_LOGI(TAG, "no engine data: automatic search");
                        elm_explore();
                    }
                } else if (!auto_scan_done) {
                    auto_scan_done = true;
                    ESP_LOGI(TAG, "no adapter: automatic adapter search");
                    elm_explore();
                }
            }
            if (bad_since && now_ms - bad_since > 45000 && now_ms - last_nag > 60000 && !prompt_busy && !elm_exploring()) {
                last_nag = now_ms;
                STATUS_LOCK();
                snprintf(g_status.prompt_text, sizeof g_status.prompt_text, "No engine data: auto search soon, double-click = now");
                g_status.prompt_until_ms = now_ms + 10000;
                STATUS_UNLOCK();
            }
        }
#if BOARD_MOSAICO
        /* Function key (GPIO7): a press counts only after the key has read released once since boot, and holds do
           nothing. A 2 s hold mapped to download mode / deep sleep rebooted the board every ~2 s on USB (2026-11-11,
           cause UNVERIFIED: GPIO7 may read low after boot), so power-off and forget-adapter stay on the desktop app. */
        static bool seen_up;
        if (!down) seen_up = true;
        if (!seen_up) continue;
        if (down) { held_ms += 20; continue; }
        if (held_ms >= 1000) { held_ms = 0; continue; }
#endif
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

        if (held < 1000 && now_ms - last_tap_ms < DOUBLE_MS) {
            /* double-click: search when the link is not working, else show what the link is doing */
            last_tap_ms = 0;
            STATUS_LOCK();
            bool ok = g_status.ble == BLE_ST_LOGGING && (OBD_HAS(&g_status.obd, F_rpm) || OBD_HAS(&g_status.obd, F_speed));
            STATUS_UNLOCK();
            if (ok) {
                char line[64];
                elm_link_summary(line, sizeof line);
                STATUS_LOCK();
                snprintf(g_status.prompt_text, sizeof g_status.prompt_text, "%s", line);
                g_status.prompt_until_ms = now_ms + 6000;
                STATUS_UNLOCK();
            } else elm_explore();
        } else if (held < 1000) {
            /* tap: acknowledge a critical alarm, confirm a pending prompt, else next page */
            last_tap_ms = now_ms;
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

/* RGB LED language (few fixed meanings, per the HIG haptics rules), in the brand palette:
 *   colour  = the screen you are on: dashboard cobalt, details violet, internals green, system warm white,
 *             faults amber (red when a HIGH/CRITICAL fault is present)
 *   power-off: amber breathing while the drive is saved (do not unplug), then dark
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
        if (st.shutdown_stage == 1) {                                     /* saving before power-off: amber breathing */
            int ph = tick % 20; float l = 0.2f + 0.8f * (ph < 10 ? ph / 10.0f : (20 - ph) / 10.0f);
            status_led_set((uint8_t)(245 * l), (uint8_t)(154 * l), (uint8_t)(46 * l));
            continue;
        }
        if (st.shutdown_stage == 2) { status_led_set(0, 0, 0); continue; }
        if (st.hold_pct) continue;                                       /* button task owns the LED while held */

        int64_t now_ms = esp_timer_get_time() / 1000;
        if (st.alarm_text[0] && st.alarm_critical && now_ms < st.alarm_until_ms) { status_led_set(255, 0, 0); continue; }
        if (st.ble == BLE_ST_LOCKED) { status_led_set((tick / 2) % 2 ? 255 : 0, 0, 0); continue; }

        uint8_t r = 0, g = 0, b = 0;
        switch (display_current_page()) {
        case 0:  r = 60;  g = 110; b = 255; break;                       /* cobalt */
        case 1:  r = 150; g = 90;  b = 240; break;                       /* violet */
        case 2:  r = 70;  g = 170; b = 90;  break;                       /* green */
        case 3:                                                           /* faults */
            if (st.faults.max_severity >= SEV_HIGH) { r = 240; g = 70;  b = 60; }
            else                                    { r = 245; g = 154; b = 46; }
            break;
        default: r = 230; g = 215; b = 190; break;                        /* system: warm white */
        }
        float level = 1.0f;
        if (st.ble < BLE_ST_LOGGING) level = (tick / 5) % 2 ? 1.0f : 0.0f;                 /* slow blink: searching / init */
        status_led_set((uint8_t)(r * level), (uint8_t)(g * level), (uint8_t)(b * level));
    }
}

void app_main(void)
{
#if BOARD_MOSAICO
    mosaico_console_init();           /* TinyUSB CDC on the Type-C port, first, so every later line reaches the computer */
#endif
    sched_boot_check();               /* reset reason and boot-loop count, before anything can fail */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    remote_config_init();
    status_init();
    units_init();                     /* after the status lock exists, before config.json may override the units */
    {
        /* the RTC keeps running through deep sleep and software resets: reuse a clock set earlier */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (tv.tv_sec > 1700000000) { STATUS_LOCK(); g_status.time_synced = true; STATUS_UNLOCK(); }
    }
#if BOARD_MOSAICO
    mosaico_board_init();             /* 3.3 V rail on, I2C0, fuel gauge: before the display and the touch controller */
#else
    gpio_deep_sleep_hold_dis();       /* a power-off held the backlight and LED pins low: release them */
    gpio_hold_dis(BOARD_LCD_BL);
    gpio_hold_dis(BOARD_RGB_LED);
#endif
    status_led_init();
    status_led_set(64, 64, 64);

    if (display_init() != ESP_OK) ESP_LOGE(TAG, "display init failed");
    if (sdlog_init() != ESP_OK) ESP_LOGE(TAG, "SD logging unavailable");
    storage_rt_run();                 /* CONFIG_OCM_STORAGE_RESET_TEST only (ESP-Mosaico bench test); a no-op otherwise */
    remote_config_load_sd();          /* /sdcard/obd/config.json, if present */
    uploader_init();                  /* pairing with the desktop app, kept in NVS */
    sched_mon_start();                /* raises the "restarted after ..." event from sched_boot_check() */

    /* the model loads before BLE and Wi-Fi start, so the global small-allocation switch it needs (ml_infer.cpp)
       is on while the fewest other tasks allocate; it takes under 200 B of internal RAM either way */
    if (!sched_safe_mode()) ml_init();   /* after repeated abnormal resets, boot without the newest component */
    rev_init();                       /* R.E.V learning and fill-up calibration; in safe mode only the calibration factor, from NVS */
#if BOARD_MOSAICO
    mosaico_ui_producers_ready();     /* the launcher may now read the R.E.V status */
#endif
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
    /* The button task stays in internal RAM: forgetting the adapter writes NVS, and flash writes are not allowed from a
       PSRAM stack. The LED task only drives the LED, so its stack goes to PSRAM. A task that cannot start is an event. */
    if (xTaskCreate(button_task, "button", 4096, NULL, 3, NULL) != pdPASS)
        sys_event(EV_ERROR, "BOOT button task did not start (internal RAM)");
    if (xTaskCreateWithCaps(led_task, "led", 4096, NULL, 2, NULL, MALLOC_CAP_SPIRAM) != pdPASS)
        sys_event(EV_ERROR, "LED task did not start");
    ESP_LOGI(TAG, "internal heap after tasks: %u (largest block %u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "%s running", CARMIND_FW_STRING);
}
