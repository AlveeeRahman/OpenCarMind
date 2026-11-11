/* ESP-Mosaico V1.2: puts every GPIO the firmware does not use into a defined low-leakage state at boot (the owner's
 * "block all GPIO"). Free pins: function GPIO, input and output off, no pull, held. Free strapping pins: the same, never
 * held. The NS4150B amplifier enable (PA_CTRL, GPIO45): driven low and held, so the amplifier stays off. Pins in use,
 * the flash, the USB Serial/JTAG and UART0 consoles and BOOT are not touched. gpio_reset_pin is never used: IDF 6.1
 * turns the pull-up on there. This is for power and determinism, not a security boundary (the ROM paths stay open). */
#pragma once
#include "esp_err.h"

/* call once, early in boot, before any later driver claims a pin (mosaico_board_init, after the rail and motor) */
esp_err_t board_pins_safe_apply(void);
/* every pad's input/output/pull/function (gpio_dump_io_configuration): CONFIG_OCM_PINS_SAFE_DUMP prints it before
   and after the apply, for the bench diff */
void board_pins_safe_dump(void);
