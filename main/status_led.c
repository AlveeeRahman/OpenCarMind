#include "led_strip.h"
#include "esp_log.h"
#include "board.h"
#include "status_led.h"

static led_strip_handle_t s_strip;

void status_led_init(void)
{
    led_strip_config_t sc = { .strip_gpio_num = BOARD_RGB_LED, .max_leds = 1 };
    led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&sc, &rc, &s_strip) != ESP_OK) { s_strip = NULL; return; }
    led_strip_clear(s_strip);
}

void status_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r / 8, g / 8, b / 8);   /* keep it dim */
    led_strip_refresh(s_strip);
}
