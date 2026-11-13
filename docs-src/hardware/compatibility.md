# Compatibility

| | Waveshare ESP32-S3-LCD-1.47B | ESP-Mosaico V1.2 (ESP32-S31) |
|---|---|---|
| Firmware | OpenCarMind 2.5.1, beta 2.5.1b | OpenCarMind EdgeOS 2.1.0 |
| Build target | `idf.py set-target esp32s3` | `idf.py --preview set-target esp32s31` |
| ESP-IDF | v6.1 | v6.1 |
| Release download | [latest release](https://github.com/AlveeeRahman/OpenCarMind/releases/latest) | [OpenCarMind EdgeOS 2.1.0 release](https://github.com/AlveeeRahman/OpenCarMind/releases/tag/edgeos-2.1.0) |
| Flashing | desktop app, or `idf.py flash` | 1200-baud door and esptool 5.6.0 or later |
| Drive storage | SD card | 128 MB NAND |
| Pairing prompt | in the source, off by default (`CONFIG_OBD_BLE_PAIR_CONFIRM`) | on |
| Health app and AI services | no | yes, since 2.1.0 |

Each image is built for one chip. Do not flash the ESP-Mosaico image on an S3. Other ESP32-S3 boards need the pin map in `main/board.h` edited.

## Adapters

Classic-only adapters will not work on the S3, which has no Classic radio. The ESP-Mosaico build uses the same Bluetooth LE link through NimBLE, so pick a Bluetooth LE adapter for either board. The full table with the service layouts is on [Adapters](adapters.md).

## Desktop app

It runs on Windows, macOS and Linux and talks to both boards over the same USB protocol. Its Flash firmware button is for the ESP32-S3 only. Where the app says "SD card", on the ESP-Mosaico it means the board's own storage.
