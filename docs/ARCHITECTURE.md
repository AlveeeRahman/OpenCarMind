# Architecture

How the firmware is laid out, which task runs what, and how a reading moves from the car to the SD card, the screen and the desktop app. The module names below are file names under `main/`.

## Subsystems

| Folder | Holds | Runs on |
|---|---|---|
| `main/` (root) | `app_main.c` (boot sequence and task creation), `board.h` (pin map for the Waveshare ESP32-S3-LCD-1.47B), `status.h` (the mutex-protected snapshot the screen reads), `version.h`, `Kconfig.projbuild` (build options) | |
| `obd/` | `obd_ble` (NimBLE central: scan, connect, subscribe), `adapter_match` (adapter names and GATT layouts), `elm327` (ELM327 command and response layer, the poll loop), `obd_fields.h` (the 70 decoded PIDs), `obd_fresh` (how old each value is), `custom_pids` (user PIDs from `custom_pids.csv`) | OBD task, core 1 |
| `model/` | `ml_infer` (ESP-DL inference for CarMind E.V and R.E.V), `ml_params*.h` (generated from `ml/onboard`), `rev_task` (the glue between the OBD task, the model task, the USB door and the learners), `rev_learn` (outlier-robust RLS fuel read-out), `rev_cal` (fill-up calibration), `rev_tank` (tank size), `rev_torque` (physics torque), `rev_state` (the learned state file) | model task, core 0; learner steps inside the OBD task's budget |
| `analysis/` | `econ` (fuel rate and economy from PID, airflow or the model), `fault_engine` (trouble codes ranked and grouped by cause), `pattern` (10 s pattern windows, online k-means), `units` (metric, US, UK) | OBD task |
| `ui/` | `display` (LVGL pages, themes, power-off overlay), `status_led` (the RGB LED) | display task |
| `storage/` | `sdlog` (session CSV, `.ml.csv`, `.pat`, JSON), `events` (the event ring shown on the System page), `remote_config` (`config.json` and NVS settings) | OBD task and display task |
| `net/` | `wifi_creds` (saved networks), `wifi_sync` (short station connection for the clock), `uploader` (TLS client to the paired desktop app, off in 2.5.x) | Wi-Fi task, only while no drive is active |
| `system/` | `sched_mon` (periods, deadlines, the rate-monotonic bound, watchdog), `sys_mode` (SEEK, DRIVE, PARK), `power_state` (safe to turn off), `cpu_monitor` (per-core load, PMU counters, chip temperature), `harden` (every check on outside input), `usb_door` (the authenticated setup and read-out door over USB) | various |

Pure-C modules with no ESP-IDF dependency (`analysis/`, `obd/custom_pids`, `obd/adapter_match`, `system/harden`, `system/power_state`, every `model/rev_*`) compile on a computer: `tests/host` unit-tests them and `tests/fuzz` fuzzes the parsers among them.

## Tasks

| Task | Core | Period | What it does |
|---|---|---|---|
| OBD | 1 | one poll cycle, 1 to 2 s | asks the adapter for every supported PID, decodes the row, updates `status`, logs it, feeds the pattern layer and the learners, queues a 5 s window for the model |
| model | 0 | per queued window | runs the selected model (E.V or R.E.V) through ESP-DL, writes the `.ml.csv` line, and for R.E.V a second pass with airflow hidden that feeds the fuel learner |
| display | 0 | 10 Hz | draws the five pages from `status`, handles BOOT presses and the power-off sequence |
| Wi-Fi | 0 | when no drive is active | clock sync; the uploader is compiled in but off in 2.5.x |
| USB door | 0 | on demand | serves the desktop app over the cable: pairing, settings, file read-out, model switch, learner status |

`sched_mon` brackets every OBD poll and model run with a period and a deadline, checks the total load against the rate-monotonic bound, resets a hung step through the watchdog, and drops the board into safe mode (logging only) after three bad restarts in a row.

## One reading, end to end

1. `obd_ble` receives a BLE notification and hands the bytes to `elm327`, which matches them to the outstanding request and decodes the PID into the row.
2. `econ` picks the fuel source for the row (the car's fuel-rate PID, airflow, or the model's estimate), and `rev_task` adds the physics torque and the learned fuel estimate when R.E.V is running.
3. `sdlog` appends the row to `sNNNNNN.csv`, flushing at least every 10 s. `pattern` closes a 10 s window and writes a `.pat` line.
4. Every 5 s window goes to the model task. `ml_infer` returns a manoeuvre class and three estimates, written to `sNNNNNN.ml.csv` and to `status`.
5. `display` shows the latest values, the class and the estimates. `power_state` says whether the card is flushed and the power can be cut.
6. After the drive, the desktop app reads the files over the USB door (`usb_door`), or you take the card out.

## Where the numbers in the model card come from

- Host tests (`tests/host`): the learners, the fuel source picker, the fault engine and the parsers, with fault injection.
- On-chip replay (`tools/espdl_bench` in the private model repository): every model flashed in turn and the same windows replayed through the chip, which is the source of the "measured on the ESP32-S3" rows.
- Drives of the owner's car: `sNNNNNN.csv` and `.ml.csv` files read over the USB door.

## Desktop app

`desktop/app` is a Tauri 2 application: a Rust core (`src-tauri/src`: `lib.rs` reads sessions and holds the commands, `board.rs` speaks the USB door protocol, `receiver.rs` is the TLS receiver for the reworked upload) and a plain HTML, CSS and JavaScript window (`ui/`). It bundles `esptool` as a sidecar to flash the firmware, and the firmware image of the same release.
