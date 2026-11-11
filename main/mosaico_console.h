/* ESP-Mosaico console (ESP32-S31 only): TinyUSB CDC-ACM on the Type-C (USB OTG) port, with the 1200-baud door into the
 * ROM download mode, and the byte stream the USB door (usb_door.c) reads its OCM1 commands from. */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

void mosaico_console_init(void);                  /* first thing in app_main: logs go to the CDC port from here on */
bool mosaico_console_ready(void);
bool mosaico_console_connected(void);             /* a host has the port open (DTR) */
int  mosaico_console_read(void *buf, size_t n, TickType_t wait);   /* bytes the host sent; 0 on timeout */
int  mosaico_console_write(const void *buf, size_t n);             /* bytes queued; waits up to 500 ms for room */
void mosaico_console_download(void);              /* reboot into the ROM download mode (no BOOT button) */
