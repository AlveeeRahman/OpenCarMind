# Changelog

## Unreleased

- Input checks (`main/harden.c`): implausible SNTP times are refused, Wi-Fi entries are validated, over-long `wifi.csv` lines are dropped whole, NaN and infinite model inputs become missing samples, and `config.json` `rev` is range-checked. Protected management frames are used when offered, and light heap poisoning is on.
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
