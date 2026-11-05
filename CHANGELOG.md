# Changelog

## Unreleased

A new brand, a screen made for the LCD, and a visible power-off.

- **Brand**: a new night-drive palette (navy, warm ink, cobalt, amber) across the README, the website, the desktop app and its icons, and a sharper vector logo. Banner chips now read ESP32-S3, Bluetooth LE, TinyML, Desktop app and Active Learning.
- **Screen colours for a backlit LCD**: the dark theme is pure black, because any tint in the background glows through the backlight. Text is 4.5:1 or more on every background in both themes. Page 5 gives each of its seven sections its own colour (classifier orange, estimator red), checked for normal and colour-blind vision.
- **Power-off you can see**: holding BOOT shows "Turning off" while the drive is saved, then "Off, safe to unplug". The backlight fades and the LED stays dark in deep sleep.
- **Page 1**: CPU (both cores, out of 100 %) and estimated power, each as a name, a bar and a value. The connection and MIL line moved to the Faults page. Coolant temperature shows in °F where the units are US.
- **Page 5**: instructions per second on each core from the chip's hardware counters, PSRAM used out of total, the chip's own temperature (°C or °F), uptime.
- **Page 3**: the pattern line sits above the page dots.
- **New boards start on CarMind R.E.V** when the firmware carries it. A board that already chose a model keeps it.
- **Supply chain**: CodeQL, a fuzz harness over the input parsers, Dependabot, hash-pinned test dependencies and release provenance.
- **USB door**: unbiased pairing codes. The desktop app explains an expired session and can restart the board.
- **Desktop app**: says new boards start on R.E.V, and shows the running model's mark.
- **Fixes**: the status LED no longer drops a frame when the button and the LED task update it at once ("RMT transaction is busy"). The boot log no longer lists the saved Wi-Fi networks. The model self-test log says plainly when the board differs from the PC simulation.
- **Figures**: the seven workflow figures were redrawn with straight connectors and larger text, the three README GIFs re-rendered with the brand fonts, and the screen mock-ups and the Mosaico concept updated to the new colours.

## 2.5.1

CarMind R.E.V, corrected and harder to break. The model name stays CarMind R.E.V.

- **CarMind R.E.V, corrected classifier head** (`6e6d698c44c5`, replaces `f70bdcd3151b`): R.E.V no longer calls steady cruising with the throttle open "braking". On two held-out drives of my car, cruising 10.7 % → 87.5 % right and all labelled windows 78.3 % → 95.1 %; on the public KIA test set accuracy 88.9 % → 87.1 %. Only the classifier head changed (126 bytes), so learned fuel, airflow and tank state carry over. See "Looking inside R.E.V with explainable AI" in the README.
- **Outlier-robust fuel learner**: Huber weights on a running error scale, pairs with an error over 10 times that scale dropped, labels above 120 L/h ignored (also by the source picker), refits judged with the Huber loss. With 1 % implausible labels on 31 public test cars, fuel R² with the airflow hidden stays 0.867 (2.5.0: 0.696). Learned state from 2.5.0 loads unchanged. REV-STATUS reports `rejected=`.
- **Checked against meta-learning**: ANIL and Meta-SGD (learn2learn) on the same frozen trunk scored 0.778 and 0.197 on clean labels, so the closed-form learner stays.
- `tools/gen_ml_params.py`: the learned-state id can name an earlier model through `prior_sha256` in `export.json`, for model changes that keep the trunk and the estimate head.

## 2.5.0

Two on-board models, chosen in the desktop app: CarMind E.V (fixed, the default) and CarMind R.E.V, the first model that keeps learning in your car.

- **CarMind R.E.V** (19,815 parameters, 65,648 bytes, `f70bdcd3151b`, about 2.9 ms per run): learns your car's fuel use on the board with a fuel read-out that is refit while you drive and goes live only when it beats the current one on held-back rows, plus an airflow constant for cars that lose MAF. Page 5 shows the learned fuel rate. Switch models on the desktop app's Board page while parked; the choice is kept after a restart.
- **Engine torque from physics** (both models, shown by R.E.V): fuel energy flow over engine speed minus friction, sized to your engine through the learned airflow constant; uses the car's own torque PIDs (0x62, 0x63) when it reports them. Amber, "idle" when stopped. New CSV columns `torque_nm`, `torque_src`.
- **Fuel tank learner** (both models): learns the tank size from the fuel level against the fuel counted; shows litres left and range on the details page once it is sure, and suggests fill-ups it sees. New CSV columns `tank_left_l`, `tank_range_km`; state in `/sdcard/obd/rev_tank.bin`.
- **Fill-up calibration** (both models): fill-ups typed on the app's new Fuel page give one correction factor for every fuel figure (tank to tank, limited to 0.7 to 1.4). New CSV columns `fuel_lph_raw`, `fuel_cal`, `trip_fuel_raw_l`.
- **Wi-Fi only sets the clock**: one HTTPS request to one time server, its certificate checked against trusted roots and its name; the default is `www.google.com` and the desktop app sets your own (`time_host`). SNTP is gone, and the board listens on no port. Session JSON `clock` is now `https`.
- **Wi-Fi upload to the desktop app is off** while it is reworked to avoid head-of-line blocking; import drives over the USB cable. Pairing still protects the USB commands.
- Page 5: "Learning" job, the time since the model's last answer beside AI ESTIMATES, brake "holding" while stopped, fuel rate in L/h or gal/h with the display units. A model switch clears the last answer, and is refused while a drive is starting or running.
- Fix: a PID that stopped answering kept its last value, marked valid, until the drive ended. Fields now count as missing after 3 poll cycles (at least 4 s), or 3 rounds of the slow list (at least 10 s), and the CSV leaves them empty.
- Fix: a MAF reading of 0xFFFF (655.35 g/s, a sensor fault) is ignored everywhere, including by the airflow learner.
- Fix: R.E.V's learned-state file is read by its own task, not at start-up, which overflowed the main task's stack.
- Version 2.5.0 for the firmware and the desktop app. Host tests: 524 checks.
- README: two models, what R.E.V learns and what stays fixed, how the models are made, limitations and future work, larger workflow figures, an ESP-Mosaico concept with R.E.V.

Known limits: R.E.V is in its first drives; torque is an estimate checked on one public car and reads high at a standstill; the tank needs about 10 L of driving before it shows anything; the Wi-Fi clock has not yet been checked on a board.

## 2.4.0

- CarMind E.V (`dfa9af416dfb`, 89,152 bytes), a new on-board model that holds up better on cars it has not seen. About 3.5 ms per run. Scores are in `docs/MODEL.md`.
- Fixes from a code review, with no change to the model, the scheduler's default behaviour, the OBD polling or the log format:
  - Diesel fuel use from MAF accepts the lean commanded ratios diesels run at (up to 8). It read about 3x high before.
  - A fuel-cut reading of PID 0x44, which is polled slowly, no longer zeroes the fuel rate once the throttle opens again.
  - Fuel and economy start fresh with each drive, and the previous drive's model fuel estimate is no longer borrowed.
  - The amber "estimate" colour is cleared when a value stops being an estimate.
  - Unit captions are redrawn only when the units change.
  - The units event no longer prints the country code when the time zone decided.
  - The 10 s model stall rule applies only when the model runs every second.
  - Host tests: 321 checks.
- The desktop app shows fuel as unknown, not 0 L, for a drive where the car gave no fuel source.
- The events screen names the model "ML Model - CarMind E.V". Its hash stays in the serial log and in every `.ml.csv` row.
- Repository: removed six screen renders no page used and five desktop design mockups of screens the app does not have. `ml/onboard/export.json` lists the architecture only. `.gitignore` covers those renders and assistant, scratch and editor files.
- Architecture proposals: a new issue template. We train what you send on the same data and splits and post the scores.
- Figures for the inference path updated. The old model's bench summary (`ml/onboard/bench_summary.json`) is removed.
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
- On-board model (hash `85d63ec33020`) that classifies the manoeuvre and estimates brake pressure, flywheel torque and fuel rate. The window is cut when each OBD row arrives.
- Soft real-time scheduler with deadlines, a task watchdog and a safe mode after repeated resets.
- Optional Wi-Fi clock sync (`OCM_WIFI_CLOCK_SYNC`) and settings from `/obd/config.json` on the card.
- Dark and light themes, switched by time of day or fixed in `config.json`.
- Dashboard: `tools/ocm_report.py` builds local reports from the card, and `dashboard/` runs them next to Dashy.
- CI, a release workflow and an OpenSSF Scorecard workflow.
