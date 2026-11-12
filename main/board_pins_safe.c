#include "board_pins_safe.h"
#include <stdio.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "soc/soc_caps.h"
#include "esp_log.h"
#include "board.h"

#define B(n) (1ULL << (n))

/* Pin classes of the ESP-Mosaico V1.2 (IDF 6.1 esp32s31 GPIO docs, soc_caps.h, the V1.2 user guide pin tables and the
   CoreBoard schematic). Every valid GPIO is in exactly one class, checked at build time below. */

/* Free: disabled, no pull, held. 0/1 = I2C1 to the module EEPROMs, 14 = left module EEPROM select (no module support in
   this firmware), 2 = IMU/magnetometer INT (not used), 43 = panel TE (no tear sync), 46-49/52-55 = codec I2S and slot
   lines (no audio), 57 = POWER_SWITCH: high-Z is its released state (user guide), never pull it down. */
#define PINS_FREE  (B(0) | B(1) | B(2) | B(4) | B(5) | B(10) | B(11) | B(12) | B(13) | B(14) | B(15) | B(16) | \
                    B(17) | B(18) | B(19) | B(43) | B(46) | B(47) | B(48) | B(49) | B(52) | B(53) | B(54) | \
                    B(55) | B(57))
/* Free strapping pins (37 I2S_BCK, 38, 39 right module EEPROM select, 40 I2S_DOUT): disabled, never held, because the
   chip samples them at reset and a held pad could reach the next one. */
#define PINS_STRAP_FREE (B(37) | B(38) | B(39) | B(40))
/* NS4150B CTRL (PA_CTRL, "amplifier enable"): driven low, so the amplifier stays off. */
#define PIN_PA_CTRL 45
/* Pins the firmware configures itself (the NAND pins 20-25 belong to nand_store.c). */
#define PINS_USED  (B(BOARD_I2C_SDA) | B(BOARD_I2C_SCL) | B(BOARD_TOUCH_INT) | B(BOARD_BUTTON) | B(BOARD_MOTOR) | \
                    B(BOARD_LCD_D0) | B(BOARD_LCD_D1) | B(BOARD_LCD_D2) | B(BOARD_LCD_D3) | B(BOARD_LCD_SCLK) | \
                    B(BOARD_LCD_CS) | B(BOARD_LCD_RST) | B(BOARD_LCD_PWR) | \
                    B(20) | B(21) | B(22) | B(23) | B(24) | B(25))
/* Never touched: SPI0/1 to the NOR flash, USB Serial/JTAG (33/34, the secondary console), UART0 (58/59, the primary
   console), BOOT (61): the recovery paths. */
#define PINS_KEEP  (B(26) | B(27) | B(28) | B(30) | B(31) | B(32) | B(33) | B(34) | B(58) | B(59) | B(61))

_Static_assert(((PINS_FREE | PINS_STRAP_FREE | B(PIN_PA_CTRL)) & (PINS_USED | PINS_KEEP)) == 0,
               "a safe-state pin is also used or reserved");
_Static_assert((PINS_FREE & PINS_STRAP_FREE) == 0 && (PINS_USED & PINS_KEEP) == 0, "a pin is in two classes");
_Static_assert((PINS_FREE | PINS_STRAP_FREE | B(PIN_PA_CTRL) | PINS_USED | PINS_KEEP) == SOC_GPIO_VALID_GPIO_MASK,
               "every valid GPIO must be classified");
_Static_assert(BOARD_LCD_TE == 43 && BOARD_POWER_SWITCH == 57, "board.h moved a pin this table names by number");

static const char *TAG = "pins";

esp_err_t board_pins_safe_apply(void)
{
#if CONFIG_OCM_PINS_SAFE_DUMP
    printf("pins: before the safe state\n");
    board_pins_safe_dump();
#endif
    const uint64_t held = PINS_FREE | B(PIN_PA_CTRL);
    for (int n = 0; n < SOC_GPIO_PIN_COUNT; n++)
        if ((held | PINS_STRAP_FREE) & B(n)) gpio_hold_dis((gpio_num_t)n);   /* a hold from before the reset blocks the config */
    const gpio_config_t off = {
        .pin_bit_mask = PINS_FREE | PINS_STRAP_FREE,
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&off);
    if (err != ESP_OK) return err;

    gpio_set_level(PIN_PA_CTRL, 0);                 /* level first, so the pin never drives high */
    const gpio_config_t pa = {
        .pin_bit_mask = B(PIN_PA_CTRL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&pa);
    if (err != ESP_OK) return err;
    gpio_set_level(PIN_PA_CTRL, 0);

    int n_held = 0;
    for (int n = 0; n < SOC_GPIO_PIN_COUNT; n++)
        if ((held & B(n)) && gpio_hold_en((gpio_num_t)n) == ESP_OK) n_held++;
    ESP_LOGI(TAG, "safe state: %d pins disabled (%d strapping, not held), %d held, amplifier off",
             __builtin_popcountll(PINS_FREE | PINS_STRAP_FREE), __builtin_popcountll(PINS_STRAP_FREE), n_held);
#if CONFIG_OCM_PINS_SAFE_DUMP
    printf("pins: after the safe state\n");
    board_pins_safe_dump();
#endif
    return ESP_OK;
}

void board_pins_safe_dump(void)
{
    gpio_dump_io_configuration(stdout, SOC_GPIO_VALID_GPIO_MASK);
}
