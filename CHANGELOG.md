# Changelog

## 2.0.1

First public release.

- Firmware for the Waveshare ESP32-S3-LCD-1.47B: reads every Mode 01 field the car reports over a Bluetooth LE ELM327-style adapter, ranks trouble codes, and saves sessions to the SD card at least every 10 seconds, with a "Safe to turn off" event on page 3.
- On-board model: a quantisation-aware 1-D CNN (38,679 parameters, `w8a16`, hash `85d63ec33020`) that classifies the manoeuvre and estimates brake pressure, flywheel torque and fuel rate. The window is cut when each OBD row arrives.
- Soft real-time scheduler with deadlines, a task watchdog and a safe mode after repeated resets.
- Optional Wi-Fi clock sync (`OCM_WIFI_CLOCK_SYNC`) and settings from `/obd/config.json` on the card.
- Dark and light themes, switched by time of day or fixed in `config.json`.
- Training code and the single-script trainer `ml/training/train_onboard.py`.
- Dashboard: `tools/ocm_report.py` builds local reports from the card, and `dashboard/` runs them next to Dashy.
- CI, a release workflow and an OpenSSF Scorecard workflow.
