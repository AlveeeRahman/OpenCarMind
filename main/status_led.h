#pragma once
#include <stdint.h>
void status_led_init(void);
void status_led_set(uint8_t r, uint8_t g, uint8_t b);
/* Before deep sleep: LED dark, the RMT driver released and its data pin held low, so the LED cannot light up again
   while the board sleeps. status_led_set does nothing afterwards. */
void status_led_off_for_sleep(void);
