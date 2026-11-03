# Changelog

## Unreleased

- Wider adapter support (`main/adapter_match.c`, `main/obd_ble.c`), merged from what BetterDriver, SwiftOBD2, ha-obd-ble, EdiabasLib, LTSupportAutomotive and python-OBD do. Known layouts now include Vgate vLinker (E7810A71/BEF8D6C9) and Deep OBD (FFE1/FFE2). The serial line is picked by property and may use indicate. Adverts match by name words anywhere in the name and by the FFF0, 18F0, BEEF and Vgate services. The strongest candidate in a 1.5 s window wins.
- Pairing on demand: when an adapter refuses an unencrypted subscribe, the board pairs with Just Works and bonds, then retries once with the fixed PIN 123456 (OBDLink CX). Forgetting the adapter also clears the bonds.
- The adapter address is saved only after it answers `ATZ`. A device with no serial line, or one that never answers, is skipped for 10 minutes. Before this, the first matching device was saved even if it was not an adapter.
- Writes are cut to the ATT MTU and wait for each acknowledgement. If automatic protocol search fails, each OBD-II protocol is tried in turn.
- `docs/ADAPTERS.md` lists adapters with radio, layout, status, a price snapshot and an Amazon link.

- Page 3 shows live whether the power can be cut: green "Safe to turn off" only when car data has stopped for 5 s, everything is on the card and no upload is running, red with the reason otherwise (`main/power_state.c`, with a simulated drive in the host tests). The line used to stay the same between saves.

## 2.2.0

- Setup over USB, so the SD card stays in the board. An authenticated door on the USB cable (`main/usb_door.c`) takes the pairing, Wi-Fi networks, time zone and settings from the desktop app, and lets the app read the session files. The first pairing needs the 6-digit code the board shows on its screen, and later connections answer an HMAC challenge. Pairing through `receiver.json` on the card is gone.
- The desktop app does the setup: a Board page to pair, set Wi-Fi and settings, import drives over USB, and flash the firmware it carries with a bundled `esptool`, so no ESP-IDF install is needed.

## 2.1.0

Installers for the desktop app and the firmware images are attached to the GitHub release, with SHA-256 sums and build provenance.

- Wi-Fi upload to the desktop app (`main/uploader.c`): while parked and paired, the board sends new session data every 10 seconds over TLS pinned to the app's certificate, with a token. The desktop keeps the offsets, so an upload that is cut off resumes and nothing is stored twice. Tried on the board against a stand-in receiver.
- The clock keeps going through a power cut: every save writes the time to the card, the board resumes from it, shows it in amber, and Wi-Fi corrects it. Session JSON records the clock source.
- `timezone` in `config.json` sets local time for the clock and for the light theme (06:00 to 17:00 by default, dark the rest of the day). The desktop app fills it in from the computer. Before this, the hours were read in UTC.
- Desktop app for Windows, macOS and Linux: a receiver for the board's uploads and a Card and Wi-Fi page that writes `wifi.csv`, `config.json` and the pairing file to the SD card.
- mbedTLS uses PSRAM buffers sized per record, and heap poisoning stays off. With light poisoning on, the TLS handshake failed in PSA ECDH on the test board.
- `docs/FIELD_CHECKS.md` and a Field check issue template list the checks that need a car, with empty result tables.
- Input checks (`main/harden.c`): implausible SNTP times are refused, Wi-Fi entries are validated, over-long `wifi.csv` lines are dropped whole, NaN and infinite model inputs become missing samples, and `config.json` `rev` is range-checked. Protected management frames are used when offered.
- `SECURITY.md` lists the known limits and the optional permanent hardening steps.
- Adapter search matches a list of common Bluetooth LE adapter names (`OBD_BLE_NAME_PREFIXES`) instead of one prefix. Only the LELink has been tested.
- `OBD_RAW_EVERY_PASSES` sets how often PIDs without a decoder are logged raw (default unchanged at 10).

## 2.0.1

First public release.

- Firmware for the Waveshare ESP32-S3-LCD-1.47B: reads every Mode 01 field the car reports over a Bluetooth LE ELM327-style adapter, ranks trouble codes, and saves sessions to the SD card at least every 10 seconds, with a "Safe to turn off" event on page 3.
- On-board model: a quantisation-aware 1-D CNN (38,679 parameters, `w8a16`, hash `85d63ec33020`) that classifies the manoeuvre and estimates brake pressure, flywheel torque and fuel rate. The window is cut when each OBD row arrives.
- Soft real-time scheduler with deadlines, a task watchdog and a safe mode after repeated resets.
- Optional Wi-Fi clock sync (`OCM_WIFI_CLOCK_SYNC`) and settings from `/obd/config.json` on the card.
- Dark and light themes, switched by time of day or fixed in `config.json`.
- Dashboard: `tools/ocm_report.py` builds local reports from the card, and `dashboard/` runs them next to Dashy.
- CI, a release workflow and an OpenSSF Scorecard workflow.
