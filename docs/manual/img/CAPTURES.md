# Manual captures

Every screen capture the manuals use, which screen and theme it shows, and where it came from. All Mosaico images are 480 x 480 PNG files. Each was looked at before publishing: no network names, VIN, MAC address, pairing code, clock time zone line or test alarm text.

Two sources:

- Board capture: taken on an ESP-Mosaico running EdgeOS 2.0.0 build 06ddf1d, on a bench with no car connected.
- Host render of the 2.0.0 screen code: `tests/host/uishot`, the demo case. It builds the real `main/mosaico_display.c` and `main/mosaico_apps.c` with LVGL 9.5 on a computer and feeds them example values (a car connected at 64 km/h, R.E.V in use, no faults, clock 17:31). It is not a photo of the board. To refresh one: `cd tests/host && make uishot`, then copy `uishot/out/<name>.png` over the file.

To swap a capture, write the new PNG over the file with the same name and run `python3 docs/manual/build.py`.

## ESP-Mosaico (OpenCarMind EdgeOS 2.0.0 manual)

| File | Screen | Theme | Source |
|---|---|---|---|
| `mosaico-home-light.png` | Home before a car connects (Economy, WAITING, No car yet, Search) | light | host render of the 2.0.0 screen code (`home_demo_nocar_light`). The board capture from 06ddf1d shows the "ECONOMY NOW" overlap fixed in ce58ca7 |
| `mosaico-launcher-page1-light.png` | Launcher page 1 (Drive, AI, Learning, R.E.V) | light | board capture, 06ddf1d |
| `mosaico-launcher-page2-dark.png` | Launcher page 2 (E.V, Summary, Live, Fuel) | dark | board capture, 06ddf1d |
| `mosaico-drive-light.png` | Drive, R.E.V in use, no car | light | board capture, 06ddf1d |
| `mosaico-ai-dark.png` | AI, R.E.V in use, before the first window | dark | board capture, 06ddf1d |
| `mosaico-learning-rev-light.png` | Learning, R.E.V in use, new board | light | board capture, 06ddf1d |
| `mosaico-learning-ev-light.png` | Learning, E.V in use (learning off) | light | host render of the 2.0.0 screen code (`learn_demo_ev_light`) |
| `mosaico-rev-light.png` | R.E.V app, in use, new board | light | board capture, 06ddf1d |
| `mosaico-ev-dark.png` | E.V app, not active (R.E.V in use) | dark | board capture, 06ddf1d |
| `mosaico-summary-dark.png` | Summary, no drive yet, recent events | dark | board capture, 06ddf1d |
| `mosaico-live-light.png` | Live, during a drive | light | host render of the 2.0.0 screen code (`live_demo_light`) |
| `mosaico-fuel-dark.png` | Fuel, R.E.V in use | dark | host render of the 2.0.0 screen code (`fuel_demo_dark`) |
| `mosaico-brakes-dark.png` | Brakes and torque, after a minute of driving | dark | host render of the 2.0.0 screen code (`brakes_demo_60s_dark`) |
| `mosaico-trip-dark.png` | Trip, during a drive | dark | host render of the 2.0.0 screen code (`trip_demo_dark`) |
| `mosaico-engine-dark.png` | Engine, during a drive | dark | host render of the 2.0.0 screen code (`engine_demo_dark`) |
| `mosaico-faults-dark.png` | Faults, no codes | dark | board capture, 06ddf1d |
| `mosaico-board-dark.png` | Board page: Parked, health good, RAM and battery cards, log storage, soft deadlines | dark | board capture, 06ddf1d |
| `mosaico-files-dark.png` | Files, drive logs on the 128 MB NAND | dark | board capture, 06ddf1d |
| `mosaico-wifi-dark.png` | Wi-Fi, idle between clock syncs (no network name on screen) | dark | board capture, 06ddf1d |
| `mosaico-settings-dark.png` | Settings, Home apps order | dark | board capture, 06ddf1d |
| `mosaico-alarm-banner-dark.png` | Home with a critical-fault banner, example code P0300 | dark | host render of the 2.0.0 screen code (`home_demo_alarm_dark`). The board capture shows a bench test banner |

Summary, Faults and Files were light captures in 1.2.0. The clean 2.0.0 board captures of them are dark, so the files were renamed to `-dark`.

Not taken yet: the OBD adapter pairing sheet on the board.

## ESP32-S3 (OpenCarMind 2.5.1 manual)

The S3 manual uses the renders already in `docs/images/`, so no copy lives here. They are renders with example values, not photos, and do not change with Edge OS.

| File | Screen | Theme |
|---|---|---|
| `../../images/dash.png` | Page 1, Dashboard | dark |
| `../../images/dash-light.png` | Page 1, Dashboard | light |
| `../../images/faults.png` | Page 4, Faults | dark |
| `../../images/system-rev-light.png` | Page 5, System with R.E.V | light |

Pages 2 (Details) and 3 (Internals) show "Capture coming" frames: the manual waits for renders with no VIN and no network name on them.
