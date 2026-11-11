/* ESP-Mosaico bring-up without the launcher (this layer only): the display calls app_main.c makes are no-ops and the
 * log goes to the USB console (mosaico_console.c). The launcher (mosaico_display.c) replaces this file. */
#include "display.h"

esp_err_t display_init(void) { return ESP_OK; }
void display_next_page(void) {}
void display_sleep(void) {}
int  display_current_page(void) { return 0; }
void display_set_backlight(uint8_t pct) { (void)pct; }
void display_fade_off(int ms) { (void)ms; }
