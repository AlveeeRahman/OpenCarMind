/* Waveshare ESP32-S3-LCD-1.47B pin map (ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM).
 * Source: https://www.waveshare.com/wiki/ESP32-S3-LCD-1.47B and the espp BSP header. */
#pragma once
#include "driver/gpio.h"

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

/* micro-SD on the SDMMC peripheral, 4-bit bus */
#define BOARD_SD_CLK         GPIO_NUM_14
#define BOARD_SD_CMD         GPIO_NUM_15
#define BOARD_SD_D0          GPIO_NUM_16
#define BOARD_SD_D1          GPIO_NUM_18
#define BOARD_SD_D2          GPIO_NUM_17
#define BOARD_SD_D3          GPIO_NUM_21

/* WS2812 RGB LED and BOOT button */
#define BOARD_RGB_LED        GPIO_NUM_38
#define BOARD_BUTTON         GPIO_NUM_0     /* active low */
