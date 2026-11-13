# Flash a board

| Board | Image | How |
|---|---|---|
| ESP32-S3 | the firmware on the [latest release](https://github.com/AlveeeRahman/OpenCarMind/releases/latest), or the [2.5.1b beta](https://github.com/AlveeeRahman/OpenCarMind/releases/tag/2.5.1b) | the desktop app's Flash firmware button, or `idf.py flash` from your own build |
| ESP-Mosaico | `OpenCarMind-EdgeOS-2.1.0-ESP-Mosaico-ESP32-S31-merged.bin` on the [OpenCarMind EdgeOS 2.1.0 release](https://github.com/AlveeeRahman/OpenCarMind/releases/tag/edgeos-2.1.0) | open the USB port at 1200 baud, then esptool 5.6.0 or later. No buttons |

Check every image against the `SHA256SUMS` file next to it on the release page.

## ESP32-S3

Steps for the desktop app are in the [manual chapter on updating](../manuals/esp32-s3/update.md). To build from source, follow [ESP32-S3 board](esp32-s3.md#flash-the-firmware). If the board stays in download mode after flashing, reset it with `python -m esptool --chip esp32s3 -p <port> --after watchdog-reset chip-id`.

## ESP-Mosaico

You find the commands on [ESP-Mosaico](esp-mosaico.md#flash-the-firmware). Writing the merged image erases the NVS partition, so the pairing with the desktop app, the saved Wi-Fi networks and the settings go, and you pair again. Drive logs on the NAND are not written. The desktop app's Flash firmware button is for the ESP32-S3 only, so do not use it for the ESP-Mosaico.
