/* Board pin maps, chosen by the build target:
 *   esp32s3   Waveshare ESP32-S3-LCD-1.47B (ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM), the car board.
 *             Source: https://www.waveshare.com/wiki/ESP32-S3-LCD-1.47B and the espp BSP header.
 *   esp32s31  Espressif ESP-Mosaico V1.2 (ESP32-S31NRV16, 32 MB in-package PSRAM: esp_psram_get_size() 33,554,432 B, the guide says 16 MB; 16 MB GD25Q128 NOR flash; 128 MB SPI NAND).
 *             Source: ESP-Mosaico V1.2 user guide, "Pin allocation" and component tables,
 *             https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide.html
 *             and the CoreBoard V1.2 schematic (overview page: power switch on VSYS, no RTC chip, no SD card). */
#pragma once
#include "sdkconfig.h"
#include "driver/gpio.h"

#if CONFIG_IDF_TARGET_ESP32S31
#define BOARD_MOSAICO        1
/* 2.16" CO5300 AMOLED, 480x480, QSPI (user guide: DATA0 36, DATA1 51, DATA2 35, DATA3 9, CLK 42, CS 50, RST 44, TE 43) */
#define BOARD_LCD_SPI_HOST   SPI2_HOST
#define BOARD_LCD_D0         GPIO_NUM_36
#define BOARD_LCD_D1         GPIO_NUM_51
#define BOARD_LCD_D2         GPIO_NUM_35
#define BOARD_LCD_D3         GPIO_NUM_9
#define BOARD_LCD_SCLK       GPIO_NUM_42
#define BOARD_LCD_CS         GPIO_NUM_50
#define BOARD_LCD_RST        GPIO_NUM_44
#define BOARD_LCD_TE         GPIO_NUM_43
#define BOARD_LCD_PWR        GPIO_NUM_60     /* VCC_3V3_CTRL, active low: the 3.3 V rail of the panel, the HUSB320 USB-C controller and the boost converter (kept on) */
#define BOARD_LCD_BL         GPIO_NUM_NC     /* AMOLED: no backlight; brightness is a panel command (0x51) */
#define BOARD_LCD_H_RES      480
#define BOARD_LCD_V_RES      480
#define BOARD_LCD_PCLK_HZ    (40 * 1000 * 1000)
/* the car board's 172x320 layout size, unused by the Mosaico UI (it draws natively at 480x480: mosaico_display.c) */
#define BOARD_UI_W           172
#define BOARD_UI_H           320
/* CST9220 touch and the other on-board I2C parts (codec 0x19, BMI270 0x69, BMM150 0x11/0x12, BQ27220 0x55) on I2C0 */
#define BOARD_I2C_PORT       0
#define BOARD_I2C_SDA        GPIO_NUM_56
#define BOARD_I2C_SCL        GPIO_NUM_3
#define BOARD_TOUCH_INT      GPIO_NUM_6
#define BOARD_TOUCH_RST      GPIO_NUM_NC     /* TP_RST exists on the schematic; the user guide gives no GPIO (UNVERIFIED) */
#define BOARD_FUEL_GAUGE_ADDR 0x55           /* BQ27220 */
/* Function (AI) key: GPIO7, active low, an LP IO, so it can wake the chip from deep sleep (EXT1). BOOT (GPIO61) is a strap. */
#define BOARD_BUTTON         GPIO_NUM_7
#define BOARD_POWER_SWITCH   GPIO_NUM_57     /* POWER_SWITCH: asks the SAM8108 load switch to cut power (polarity UNVERIFIED) */
#define BOARD_MOTOR          GPIO_NUM_8      /* vibration motor */
#define BOARD_RGB_LED        GPIO_NUM_NC     /* no RGB LED on the Mosaico */
/* no micro-SD slot: the log goes to a FAT partition ("storage") in the 16 MB NOR flash, mounted where the card was */
#define BOARD_HAS_SD         0

#else
#define BOARD_MOSAICO        0
/* 1.47" ST7789 LCD, 172x320, on SPI2 */
#define BOARD_LCD_SPI_HOST   SPI2_HOST
#define BOARD_LCD_MOSI       GPIO_NUM_45
#define BOARD_LCD_SCLK       GPIO_NUM_40
#define BOARD_LCD_CS         GPIO_NUM_42
#define BOARD_LCD_DC         GPIO_NUM_41
#define BOARD_LCD_RST        GPIO_NUM_39
#define BOARD_LCD_BL         GPIO_NUM_46     /* 1.47B: GPIO46 (the non-B board uses GPIO48) */
#define BOARD_LCD_H_RES      172
#define BOARD_LCD_V_RES      320
#define BOARD_LCD_GAP_X      34      /* the 172-wide glass sits at columns 34..205 of the 240-wide ST7789 RAM */
#define BOARD_LCD_GAP_Y      0
#define BOARD_LCD_PCLK_HZ    (40 * 1000 * 1000)
#define BOARD_UI_W           172
#define BOARD_UI_H           320

/* micro-SD on the SDMMC peripheral, 4-bit bus */
#define BOARD_HAS_SD         1
#define BOARD_SD_CLK         GPIO_NUM_14
#define BOARD_SD_CMD         GPIO_NUM_15
#define BOARD_SD_D0          GPIO_NUM_16
#define BOARD_SD_D1          GPIO_NUM_18
#define BOARD_SD_D2          GPIO_NUM_17
#define BOARD_SD_D3          GPIO_NUM_21

/* WS2812 RGB LED and BOOT button */
#define BOARD_RGB_LED        GPIO_NUM_38
#define BOARD_BUTTON         GPIO_NUM_0     /* active low */
#endif
