# Security

OpenCarMind runs on a board plugged into your car's diagnostic port, so this page says what the firmware does with the network and your data, what it does not do, and how to report a problem.

## Reporting a vulnerability

Please report privately through GitHub: open the repository's Security tab and choose "Report a vulnerability". Do not open a public issue for anything that could be misused. Include the firmware version (shown on page 2 of the screen), your board, and the steps to reproduce.

## What the firmware does and does not do

- Opens no listening sockets and runs no web server. The only network client is the optional clock sync: it connects to a Wi-Fi network you saved, asks a public time server (`pool.ntp.org`) for the time over SNTP, and disconnects. The firmware has no HTTP client, no TLS and no update mechanism.
- Starts Wi-Fi only if you saved a network, and never scans or connects while the car's OBD link is up. Turn Wi-Fi off completely with `OCM_WIFI_CLOCK_SYNC=n`.
- Runs Wi-Fi as a station only. The soft access point and enterprise Wi-Fi are compiled out, and the stack protector is on (`sdkconfig.defaults`).
- Keeps saved Wi-Fi passwords in the board's flash (NVS) in plain text. Anyone with physical access to the board can read them. Use a phone hotspot or a guest network, not your main network.
- Writes drive data to the SD card as plain CSV, JSON and text. Anyone who holds the card can read it, including the VIN and timestamps. Treat the card like any file that records where and when you drove.
- Talks to the car only through the paired Bluetooth LE adapter, by its saved address. It sends standard OBD-II read requests (modes 01, 03, 07, 09, 0A) and the custom PIDs you list in `custom_pids.csv`. It never sends mode 04 (clear codes). A custom PID sends exactly the command you put in the file, so list read requests only.
- Keeps the model's weights in the firmware image. The board does not download or change them.

The board has no authentication on its USB port, which is how you flash it. Whoever can plug a cable in can reflash it.

## Supply chain

Releases are built by the `release` workflow from a tagged commit and carry `SHA256SUMS`. Every GitHub Action used by the workflows is pinned to a commit, the ESP-IDF build image is pinned by digest, and the OpenSSF Scorecard workflow reviews the repository weekly. The firmware depends on ESP-IDF, ESP-DL, LVGL, led_strip and cJSON, fetched by the IDF component manager at build time. Their versions are in `main/idf_component.yml`.

## Supported versions

Only the latest release gets fixes.
