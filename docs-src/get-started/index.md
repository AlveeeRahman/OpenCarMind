# Get started

OpenCarMind runs on two boards. Pick the page for yours. Both use the same Bluetooth LE OBD-II adapter and the same desktop app.

| | ESP32-S3 board | ESP-Mosaico |
|---|---|---|
| Board | Waveshare ESP32-S3-LCD-1.47B | Espressif ESP-Mosaico V1.2 (ESP32-S31) |
| Firmware | OpenCarMind 2.5.1, beta 2.5.1b | OpenCarMind EdgeOS 2.1.0 |
| Screen | 1.47 inch LCD, one BOOT button | 480 × 480 AMOLED touch screen |
| Drive storage | a FAT-formatted SD card | the board's 128 MB NAND |
| Download | [latest release](https://github.com/AlveeeRahman/OpenCarMind/releases/latest) | [OpenCarMind EdgeOS 2.1.0 release](https://github.com/AlveeeRahman/OpenCarMind/releases/tag/edgeos-2.1.0) |
| Start here | [ESP32-S3 board](esp32-s3.md) | [ESP-Mosaico](esp-mosaico.md) |

The two images are not interchangeable. Do not flash the ESP-Mosaico image on an S3, and do not flash the S3 image on the ESP-Mosaico. The [compatibility page](../hardware/compatibility.md) has the details.

```{toctree}
:maxdepth: 1

esp32-s3
esp-mosaico
flash
first-drive
```
