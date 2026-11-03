# Configuration

Everything you can change, and where. Build-time options live in `idf.py menuconfig` under OBD Logger Configuration. Run-time options come from files on the SD card and from the BOOT button.

## SD card files

The board writes everything under `/sdcard/obd/`.

| File | What it is |
|---|---|
| `sessions/sNNNNNN.csv` | one row per OBD poll cycle, 88 columns (see below) |
| `sessions/sNNNNNN.ml.csv` | one line per model run (see below) |
| `sessions/sNNNNNN.pat` | one row per closed 10 s pattern window |
| `sessions/sNNNNNN.raw` | raw hex of supported PIDs the firmware has no decoder for |
| `sessions/sNNNNNN.json` | session metadata: adapter, VIN, supported PIDs, trouble codes, counters |
| `patterns.json` | the k-means pattern model, kept across drives |
| `wifi.csv` | Wi-Fi networks to import (you create it) |
| `config.json` | settings to import (you create it) |
| `custom_pids.csv` | up to 8 custom PIDs (you create it) |
| `clock.txt` | the time at the last save, so the clock can resume after a power cut |

A new session starts at every power cycle. Everything is flushed to the card at least every 10 seconds (`SDLOG_SYNC_PERIOD_S`) and each time the event "Safe to turn off" appears on page 3.

## Wi-Fi networks

Wi-Fi sets the clock and, once the board is paired with the desktop app, carries new drive data to it. The desktop app can write these files for you. To do it by hand, put a file called `wifi.csv` in `/sdcard/obd/` with one network per line:

```
My Home Wifi,my-wifi-password
-Old Network Name
```

A line that starts with a minus sign forgets that network. The board imports the file at boot and then deletes it. It keeps up to 8 networks in flash (NVS), and the passwords are stored there in plain text. The ESP32-S3 has a 2.4 GHz radio only. If no network is known, the board writes `wifi.example.csv` as a template, and Wi-Fi stays off.

The board connects only while parked. It scans, connects, reads the time, stays connected for 30 seconds, disconnects, and tries again every 6 hours. Until the clock has been set it scans every 20 seconds. Once paired with the desktop app, it stays connected while parked and looks for the network again 20 seconds after losing it. It never scans or connects while the OBD link is up. All of these times are settings in the Wi-Fi clock sync menu.

## The clock

The board has no battery for its clock. Every save writes the time to `/sdcard/obd/clock.txt`. After a power cut the clock resumes from that file, behind by the time the car was off, and the screen shows it in amber until the next Wi-Fi sync corrects it. Each session JSON says where its time came from in the `clock` field: `sntp` (set over Wi-Fi), `rtc` (kept through a reset), `resumed` (from the card, an estimate) or `unset`. Rows also carry `uptime_ms`, and `time_sync` in the JSON maps uptime to real time, so a session can be re-timed once the clock was corrected.

## Setup over USB with the desktop app

With the desktop app, the SD card can stay in the board. The app talks to it over the USB cable through an authenticated door (`main/usb_door.h`), prepares everything itself, and sends the board only finished values to store or apply:

- Pairing: the first time, the board shows a 6-digit code on its screen and you type it into the app (3 tries per code, a new code at most every 5 seconds). The app then sends its address, port, certificate and token. Later connections prove themselves with an HMAC of a fresh random challenge, keyed with that token, so no code is needed.
- Wi-Fi networks, the time zone and the settings in the table below.
- Reading the session files from the card, so drives can be imported over the cable.
- Flashing: the app carries the matching firmware image and Espressif's `esptool`. Flashing keeps the Wi-Fi networks and the pairing, which live in a separate flash partition.

The board refuses all of this while the car link is up. While paired, the board stays on Wi-Fi when parked and sends new data to the app every 10 seconds.

The card files below (`wifi.csv`, `config.json`, `custom_pids.csv`) still work for anyone who prefers them.

## config.json

Put a file called `config.json` in `/sdcard/obd/` and reboot. The board applies the keys it knows and stores them in flash. It does not delete the file, so remove any `wifi` entries once they are imported.

| Key | Meaning | Range |
|---|---|---|
| `poll_interval_ms` | pause between polling cycles | 50 to 5000 |
| `slow_group_every` | poll slow-changing PIDs every N cycles | 1 to 50 |
| `dtc_interval_s` | read the warning light and trouble codes every N seconds | 10 to 3600 |
| `backlight_pct` | screen brightness | 5 to 100 |
| `theme_mode` | 0 by time of day, 1 dark, 2 light | 0 to 2 |
| `day_start_hour`, `day_end_hour` | light theme from the start hour until the end hour, local time. The defaults, 6 and 17, give light from 06:00 to 16:59 and dark from 17:00 | 0 to 23 |
| `timezone` | the board's local time zone as a POSIX string, for example `CST6CDT,M3.2.0,M11.1.0` (US Central) or `<+06>-6` (UTC+6). The desktop app fills it in from your computer. Without it the board uses `SYNC_TIMEZONE` from the build, which is UTC | text, 3 to 47 characters |
| `wifi` | list of `{"ssid", "password"}`, or `{"ssid", "forget": true}` | |
| `custom_pids` | the text of `custom_pids.csv` | |
| `rev` | a number you can use to track versions | |

Files larger than 4096 bytes and invalid JSON are ignored, and the board logs a warning event.

## Custom PIDs

Create `custom_pids.csv` next to the other files. The board writes `custom_pids.example.csv` the first time it runs, with the format. Up to 8 rows are logged as `custom_1` to `custom_8` and shown on page 3. Each row has a mode and PID, a description, a minimum and maximum, a unit, a formula over the response bytes (A, B, C and so on) and an optional header.

## Buttons

BOOT is on GPIO0. A tap shows the next page. Holding it for 2 seconds powers the board off into deep sleep, and BOOT wakes it. Holding it for 8 seconds forgets the saved adapter.

## Session CSV columns

The header is the `CSV_HEADER` string in `main/sdlog.c`, 88 columns in this order:

1. `seq`, `uptime_ms`, `epoch_ms`
2. 70 PID columns, from `fuel_sys1_status` to `odometer_km`, in the order of `OBD_FIELDS` in `main/obd_fields.h`
3. `adapter_voltage_v`, `mil_on`, `dtc_count`, `dtcs`, `pending_dtcs`, `permanent_dtcs`
4. `custom_1` to `custom_8`
5. `window_seq`, the number of the pattern window the row belongs to

Fields the car does not support stay empty. `epoch_ms` is 0 until the clock has been set. `schema/obd_log_schema.json` describes the same columns.

## Model results file

`sNNNNNN.ml.csv` has these columns: `window_seq`, `start_ms`, `end_ms`, `model`, `manoeuvre`, `p_stopped`, `p_braking`, `p_accelerating`, `p_cruising`, `est_brake_pressure_bar`, `est_flywheel_torque_nm`, `est_fuel_rate_lph`, `latency_us`, `queue_ms`. The `model` column holds the first 12 hex digits of the model file's hash, so every answer can be traced to the exact weights. `window_seq` counts model runs. To join an answer to the raw rows, use `start_ms` and `end_ms` against `uptime_ms`.

## menuconfig options

| Option | Default | What it does |
|---|---|---|
| `OBD_BLE_NAME_PREFIXES` | `OBDBLE,OBDII,OBD2,OBDLink,ELM327,VEEPEAK,vLinker,V-LINK,IOS-Vlink,Vgate,iCar,Viecar,KONNWEI` | comma-separated name prefixes for first-time pairing |
| `OBD_BLE_SVC_UUID`, `OBD_BLE_NOTIFY_UUID`, `OBD_BLE_WRITE_UUID` | `FFE0`, `FFE1`, `FFE1` | GATT profile to try first (empty means auto-detect) |
| `OBD_BLE_MIN_RSSI` | -90 | weakest signal worth connecting to, in dBm |
| `OBD_POLL_INTERVAL_MS` | 200 | pause between polling cycles |
| `OBD_SLOW_GROUP_EVERY` | 5 | poll slow PIDs every N cycles |
| `OBD_DTC_INTERVAL_S` | 60 | read the warning light and codes every N seconds |
| `OBD_RAW_EVERY_PASSES` | 10 | sample PIDs without a decoder every N passes through the slow list (1 to 100). Lower is more data and slower main rows. |
| `OBD_ELM_TIMEOUT_MS` | 2500 | per-command timeout |
| `SDLOG_MOUNT_POINT` | `/sdcard` | where the card mounts |
| `SDLOG_FSYNC_EVERY_ROWS` | 25 | flush the CSV after at most N rows |
| `SDLOG_SYNC_PERIOD_S` | 10 | save everything at least every N seconds (1 to 60) |
| `SDLOG_FORMAT_IF_MOUNT_FAILED` | off | format a card that will not mount. This erases it. |
| `OCM_WIFI_CLOCK_SYNC` | on | compile the Wi-Fi clock sync in or out |
| `SYNC_WIFI_SSID`, `SYNC_WIFI_PASSWORD` | empty | one fallback network, stored in plain text in `sdkconfig` |
| `SYNC_WIFI_MIN_RSSI` | -75 | weakest access point worth joining, in dBm |
| `SYNC_SCAN_INTERVAL_S` | 20 | seconds between scans until the clock is set |
| `SYNC_STAY_CONNECTED_S` | 30 | seconds to stay connected after a sync (5 to 600) |
| `SYNC_RESYNC_INTERVAL_H` | 6 | hours between re-syncs (1 to 168) |
| `SYNC_TIMEZONE` | `UTC0` | POSIX timezone string, for example `CST6CDT,M3.2.0,M11.1.0` |
| `LCD_INVERT_COLORS` | on | the Waveshare 1.47B panel needs this |
| `LCD_BACKLIGHT_PCT` | 80 | default brightness |
| `CARMIND_TASK_STATS_S` | 0 | log per-task CPU, stack and heap every N seconds (0 is off) |
| `CARMIND_ML_ENABLE` | on | run the model |
| `CARMIND_ML_SELFTEST` | on | run the model's self-test and one timed inference at boot |
| `CARMIND_ML_STRIDE_S` | 1 | run the model at most this often (0 to 10) |
| `CARMIND_ML_HOLD_S` | 3 | hold a channel's last value for up to N seconds when no new sample arrived (0 to 10) |
| `CARMIND_ML_SYNTH_TEST` | off | bench test: feed a synthetic 100 s drive through the model at boot |

Do not put a real Wi-Fi password in `SYNC_WIFI_PASSWORD` if you plan to share your `sdkconfig`. The file is git-ignored in this repository for that reason.
