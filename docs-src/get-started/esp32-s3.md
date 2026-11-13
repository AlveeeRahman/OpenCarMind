# ESP32-S3 board

The S3 build runs on the Waveshare ESP32-S3-LCD-1.47B, and its tested release is OpenCarMind 2.5.1. A beta, [2.5.1b](../release-notes/251b.md), has every change on `main` since 2.5.1. It is a pre-release, not yet checked on an S3 board as a whole.

## What you need

- the board, a Waveshare ESP32-S3-LCD-1.47B,
- a Bluetooth LE OBD-II adapter that speaks ELM327 commands (Classic-only adapters will not pair, because the S3 has no Classic radio, see [Adapters](../hardware/adapters.md)),
- a FAT-formatted SD card,
- a USB cable from the car's USB port, which also powers the board.

## Flash the firmware

With the desktop app you pair it with the board over USB, then use its Flash firmware button. The [manual chapter on updating](../manuals/esp32-s3/update.md) walks through it. To build the image yourself:

```{include} ../_gen/readme-build-flash.md
```

## Pair the adapter

Plug the adapter into the car's OBD-II port and switch the ignition on. On first boot, the board connects to the strongest nearby adapter that looks like an OBD adapter and remembers it. Hold BOOT for 8 seconds to forget it again. The [first-boot section of the manual](../manuals/esp32-s3/hardware.md) has the adapter table and the planned pairing prompt.

## First drive

Drive. While you do, the board polls the car, shows the values on its five pages and saves the drive to the SD card every 10 seconds. Page 3 shows "Safe to turn off" in green once everything is on the card. A drive ends by itself 15 seconds after the engine computer stops answering. See [First drive](first-drive.md) and the [user manual](../manuals/esp32-s3/index.md).
