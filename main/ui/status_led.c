#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"
#include "esp_log.h"
#include "board.h"
#include "status_led.h"
#include "driver/gpio.h"

/* The LED task and the BOOT-button handler both set the LED. One RMT transfer must finish before the next pixel is
 * written, or the driver refuses with "RMT transaction is busy", so every access goes through one lock. */
static led_strip_handle_t s_strip;
static SemaphoreHandle_t s_lock;

void status_led_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    led_strip_config_t sc = { .strip_gpio_num = BOARD_RGB_LED, .max_leds = 1 };
    led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
    if (!s_lock || led_strip_new_rmt_device(&sc, &rc, &s_strip) != ESP_OK) { s_strip = NULL; return; }
    led_strip_clear(s_strip);
}

void status_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_strip) {
        led_strip_set_pixel(s_strip, 0, r / 8, g / 8, b / 8);   /* keep it dim */
        led_strip_refresh(s_strip);
    }
    xSemaphoreGive(s_lock);
}

void status_led_off_for_sleep(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_strip) {
        led_strip_clear(s_strip);
        led_strip_del(s_strip);
        s_strip = NULL;
    }
    if (s_lock) xSemaphoreGive(s_lock);
    gpio_reset_pin(BOARD_RGB_LED);
    gpio_set_direction(BOARD_RGB_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_RGB_LED, 0);
    gpio_hold_en(BOARD_RGB_LED);
}
