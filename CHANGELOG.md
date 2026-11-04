# Changelog

## 2.4.0

- Edge Model 1 (`dfa9af416dfb`, 89,152 bytes, 38,679 parameters, `w8a16`). Same architecture, new training: the public KIA drives plus 325 cars of the Vehicle Energy Dataset and twelve of the author's drives, partial labels where there is no brake switch, augmentation that imitates other cars, the adapter's real row spacing, and a fitted temperature folded into the class layer. Reads 5 s of nine channels (MAF and intake temperature now used, MAP stays off). On data held out from training: KIA macro-F1 0.887, 98.5 % of the author's held-out windows and 96.1 % of 37 unseen VED cars in an allowed class, fuel R² 0.688 on unseen cars with MAF hidden. It follows a manoeuvre change within 3 s 88 % of the time, against 55 % for a 10 s model. About 3.5 ms per run. Details are in `docs/MODEL.md`, and every variant tried, including contrastive pre-training and distillation, in `docs/MODEL_CASE_STUDY.md`.
- Architecture proposals: a new issue template. We train what you send on the same data and splits and post the scores.
- Figures for the model, the inference path and training-to-board updated. The old model's bench summary (`ml/onboard/bench_summary.json`) is removed.
- Field check: the author drove the board with the new features (the drive ending when the engine stops, the BOOT power-off, fuel economy, units) and the 10 s model, and reported every feature working.

- Fuel economy: the first screen's LOAD tile now shows the drive's economy in km/L, or mpg in the US and the UK (LOAD moved to page 3, replacing BARO). The fuel rate comes from PID 0x5E when the car has it, else from MAF with the commanded equivalence ratio (PID 0x44, where 0 means decel fuel cut), else from the on-board model's estimate, shown in amber. New CSV columns `fuel_lph_calc`, `fuel_src`, `trip_km`, `trip_fuel_l` (`main/econ.c`, host-tested).
- Units: metric by default. On the first Wi-Fi connection the access point's country code (802.11d) sets miles and mpg for the US and the UK, once, stored in NVS (`main/units.c`). A UK time zone is the fallback. Tested on the board with a US access point: it switched to miles and mpg and kept them after a reboot. `units` in `config.json` (`metric`, `us`, `uk`, `auto`) overrides it. Speed, odometer and MIL distance follow the units on screen, and the session JSON records them.
- Desktop app: fuel used and economy per drive in each drive's units, an average-economy card, and a table of every value the car reported in a drive (columns the car never filled are left out).
- Scheduler: the model job's stall limit is one 10 s label window instead of three 1 s periods, which the car's own row gaps (up to 3.6 s) could trip.

- A drive ends when the ECU stops answering for 15 s (before: 11 failed poll cycles, about 2.5 minutes on a real car). The session is saved and closed, the model stops expecting rows, and the board leaves DRIVE, so the power line turns green and the USB door and the Wi-Fi upload work again. An adapter left powered with the ignition off no longer keeps the board in "Not safe: logging". If the ECU answers again without a reboot, a new session starts.
- Power-off with BOOT: logging stops as soon as the hold reaches 2 s. On release the board waits for the drive to be saved and closed, then sleeps. Holding on to 8 s (forget adapter) resumes logging.
- DRIVE now means connecting to the adapter or logging rows. Waiting for a quiet ECU counts as parked.

## 2.3.0

- Wider adapter support (`main/adapter_match.c`, `main/obd_ble.c`), merged from what BetterDriver, SwiftOBD2, ha-obd-ble, EdiabasLib, LTSupportAutomotive and python-OBD do. Known layouts now include Vgate vLinker (E7810A71/BEF8D6C9) and Deep OBD (FFE1/FFE2). The serial line is picked by property and may use indicate. Adverts match by name words anywhere in the name and by the FFF0, 18F0, BEEF and Vgate services. The strongest candidate in a 1.5 s window wins.
- Pairing on demand: when an adapter refuses an unencrypted subscribe, the board pairs with Just Works and bonds, then retries once with the fixed PIN 123456 (OBDLink CX). Forgetting the adapter also clears the bonds.
- The adapter address is saved only after it answers `ATZ`. A device with no serial line, or one that never answers, is skipped for 10 minutes. Before this, the first matching device was saved even if it was not an adapter.
- Writes are cut to the ATT MTU and wait for each acknowledgement. If automatic protocol search fails, each OBD-II protocol is tried in turn.
- `docs/ADAPTERS.md` lists adapters with radio, layout, status, a price snapshot and an Amazon link. The README has a compatibility table with a checked Amazon link per row.
- The board screen shows the short name "CarMind", with even spacing in the top bar and a bold clock. The project, the docs and the code keep the name OpenCarMind.
- README: the screens are shown four to a row, and a References section lists the published work behind the model and the scheduler.
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
