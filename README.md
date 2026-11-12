<p align="center"><img src="docs/images/edgeos-banner.png" width="900" alt="OpenCarMind EdgeOS 2.0.0, new for the ESP-Mosaico: it decides what runs, when and how fast. A small operating-system layer for the ESP-Mosaico board. 2 models on one chip: CarMind R.E.V and E.V, switch in Settings, each admitted against its deadline. Measured on the board: boot health check rated good at 68 s, lowest free internal RAM 81 KB in any phase against a 60 KB target, model response 4.2 ms against a 1,000 ms deadline, 16 touch apps, a 128 MB NAND for the drive logs. On the board: boot health check, Wi-Fi held until healthy, internal RAM above 60 KB in every phase, battery protection. Firmware: Wi-Fi joined on the screen and used only for the clock, light and dark themes, the desktop app over USB, an alarm banner you clear with a tap or swipe, the graphics accelerator (PPA) on. Next: Active E.V, a model that learns more of your car, is in the works for the ESP-Mosaico."></p>
<p align="center"><sub>New: <a href="#opencarmind-edgeos-200-esp-mosaico">OpenCarMind EdgeOS 2.0.0</a> for the ESP-Mosaico. Banner photo: Espressif Systems, CC BY-SA 4.0, so this banner image is shared under CC BY-SA 4.0 too. Screen: captured on the board.</sub></p>

<p align="center">
<a href="https://github.com/AlveeeRahman/OpenCarMind/actions/workflows/ci.yml"><img src="https://github.com/AlveeeRahman/OpenCarMind/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI"></a>
<a href="https://scorecard.dev/viewer/?uri=github.com/AlveeeRahman/OpenCarMind"><img src="https://api.scorecard.dev/projects/github.com/AlveeeRahman/OpenCarMind/badge" alt="OpenSSF Scorecard"></a>
<a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License: MIT"></a>
<img src="https://img.shields.io/badge/ESP--IDF-v6.1-red.svg" alt="ESP-IDF v6.1">
<img src="https://img.shields.io/badge/chip-ESP32--S3-lightgrey.svg" alt="ESP32-S3">
</p>

OpenCarMind is an open-source OBD2 logger and dashboard for the ESP32, with a pruned, knowledge-distilled multi-task 1D CNN that adapts its read-out on the device. Its newest firmware, OpenCarMind EdgeOS 2.0.0, is an AI-first operating system for the chip. It runs on FreeRTOS's preemptive scheduler and treats every model as a soft real-time job with a deadline. A model is admitted only when every deadline on its core still holds, and when deadlines slip the board steps down in a fixed order (screen rate, then learning, then the model) so the drive log keeps running. OpenCarMind turns an ESP32 board and a Bluetooth OBD-II adapter into a car data logger that runs neural networks on the chip. It needs no phone, no account and no internet connection. Everything it records goes to the board's own storage (an SD card on the ESP32-S3 board, the 128 MB NAND on the ESP-Mosaico) and, if you pair the board, to the desktop app on your own computer.

It ships two on-board models, and you choose between them in the desktop app. CarMind E.V is fixed and tested. CarMind R.E.V is my first model that keeps learning in your car: the board itself updates part of the model from what your car reports while you drive, with no computer or cloud in the loop.

Version 2.5.1. Licence: MIT. Website: <https://alveeerahman.github.io/OpenCarMind/>. Downloads for the firmware and the desktop app are on the [Releases page](https://github.com/AlveeeRahman/OpenCarMind/releases/latest).

The same firmware also runs on Espressif's ESP-Mosaico touch-screen board. Its next version, [OpenCarMind EdgeOS 2.0.0](#opencarmind-edgeos-200-esp-mosaico), adds a small operating-system layer: a boot health check, refusals of risky changes while you drive, model admission by response-time analysis and battery protection. [User manuals](#user-manuals) for both boards are in [docs/manual](docs/manual).

## Contents

<ol type="I">
<li><a href="#choose-your-model">Choose your model</a></li>
<li><a href="#what-you-get">What you get</a></li>
<li><a href="#local-first">Local-first</a></li>
<li><a href="#how-it-works">How it works</a>
<ol type="A">
<li><a href="#on-board-inference">On-board inference</a></li>
<li><a href="#scheduler-and-safety">Scheduler and safety</a></li>
</ol>
</li>
<li><a href="#the-screens">The screens</a></li>
<li><a href="#dashboard">Dashboard</a></li>
<li><a href="#desktop-app">Desktop app</a></li>
<li><a href="#hardware">Hardware</a>
<ol type="A">
<li><a href="#adapter-compatibility">Adapter compatibility</a></li>
</ol>
</li>
<li><a href="#build-and-flash">Build and flash</a></li>
<li><a href="#user-manuals">User manuals</a></li>
<li><a href="#the-models">The models</a>
<ol type="A">
<li><a href="#the-test-drives">The test drives</a></li>
<li><a href="#carmind-rev-results-so-far">CarMind R.E.V results so far</a></li>
<li><a href="#how-the-models-are-made">How the models are made</a></li>
<li><a href="#what-rev-learns">What R.E.V learns</a></li>
<li><a href="#how-rev-learns-on-the-board">How R.E.V learns on the board</a></li>
<li><a href="#looking-inside-rev-with-explainable-ai">Looking inside R.E.V with explainable AI</a></li>
<li><a href="#how-rev-got-better-step-by-step">How R.E.V got better, step by step</a></li>
<li><a href="#edge-learning-rev-picks-what-to-learn">Edge learning: R.E.V picks what to learn</a></li>
</ol>
</li>
<li><a href="#status">Status</a></li>
<li><a href="#opencarmind-edgeos-200-esp-mosaico">OpenCarMind EdgeOS 2.0.0 (ESP-Mosaico)</a>
<ol type="A">
<li><a href="#what-it-is">What it is</a></li>
<li><a href="#the-parts-and-why-each-exists">The parts and why each exists</a></li>
<li><a href="#services-and-runlevels">Services and runlevels</a></li>
<li><a href="#model-admission-by-response-time-analysis">Model admission by response-time analysis</a></li>
<li><a href="#scheduler-budgets-per-core">Scheduler budgets per core</a></li>
<li><a href="#ai-services-in-edgeos">AI services in EdgeOS</a></li>
<li><a href="#what-it-costs">What it costs</a></li>
<li><a href="#what-you-see-on-the-board">What you see on the board</a></li>
<li><a href="#changes-after-the-board-check">Changes after the board check</a></li>
<li><a href="#pairing-safety-built-not-yet-checked-on-the-board">Pairing safety (built, not yet checked on the board)</a></li>
<li><a href="#flash-it-or-go-back-to-mossrev-120">Flash it, or go back to mossREV 1.2.0</a></li>
<li><a href="#prepared-but-off-ab-rollback-and-the-battery-as-a-ups">Prepared but off: A/B rollback and the battery as a UPS</a></li>
<li><a href="#what-is-verified-on-the-board">What is verified on the board</a></li>
</ol>
</li>
<li><a href="#limitations-and-future-work">Limitations and future work</a></li>
<li><a href="#planned">Planned</a></li>
<li><a href="#references">References</a></li>
<li><a href="#repository-layout">Repository layout</a></li>
<li><a href="#credits-and-licence">Credits and licence</a></li>
</ol>

## Choose your model

| | CarMind E.V | CarMind R.E.V |
|---|---|---|
| Status | released in 2.4.0, the default in the 2.5.1 release | new in 2.5.0, improved in 2.5.1, the default on a new board with the firmware from `main` |
| Size on the chip | about 89 KB | about 66 KB |
| Parameters | 38,679 | 19,815 |
| Time per run on the ESP32-S3 | about 3.5 ms in the car, 2.9 ms on the bench | about 2.9 ms in the car (median), 2.2 ms on the bench |
| Learns in your car | the model is fixed. The tank size and fill-up calibration learn with either model | its fuel estimate and an airflow constant that also gives the engine size, so its torque estimate learns your engine too. Plus the tank size and fill-up calibration, like E.V |
| Steady cruising with your foot on the gas | can be shown as braking | corrected in 2.5.1 |
| Fill-up calibration | yes | yes |
| Best for | a fixed, tested model | older cars, roughly pre-2020, that have no fuel-rate PID and can lose the airflow (MAF) reading |
| On the ESP-Mosaico (OpenCarMind EdgeOS 2.0.0, mossREV 1.2.0) | included, switch in Settings > MODEL | included, the default on a new board |

<p align="center"><img src="docs/images/ev-to-rev.gif" width="900" alt="Animated, from CarMind E.V to CarMind R.E.V in seven steps: E.V with 38,679 parameters, 89 KB and 3.5 ms per run. Structured pruning removes whole channels, down to 19,815 parameters. Knowledge distillation, E.V teaching R.E.V. Quantised for the chip: 66 KB and 2.9 ms. Learns in your car: fuel R2 from 0.709 to 0.867 on 31 unseen cars with the airflow hidden. Explainable AI fix: cruising called right on my drives from 10.7 to 87.5 percent. Shrugs off bad data: with 1 percent bad labels fuel R2 stays 0.867, where 2.5.0 fell to 0.696"></p>

From E.V to R.E.V in seven steps, each with the number it moved. How each was measured is in the [detailed README](https://github.com/AlveeeRahman/OpenCarMind/blob/rev-c2/README.md#how-rev-got-better-step-by-step).

Many older cars do not report their fuel rate, so the board works it out from the airflow sensor. When that reading is missing, a fixed model can only offer its factory estimate. R.E.V learns your car while a fuel-rate or airflow reading is there and uses what it learned when it is not.

To switch, connect the board to the desktop app with its USB cable while the car is parked, open the Board page and pick the model under "On-board model". The board keeps the choice after a restart, and what R.E.V learned stays on the SD card while E.V runs. More in [What R.E.V learns](#what-rev-learns).

<p align="center"><img src="docs/images/rev-changes.png" width="900" alt="Renders of what changes with R.E.V. Board page 5 with CarMind E.V and with CarMind R.E.V, which adds a Learning job. The desktop app's On-board model panel on the Board page and the CarMind R.E.V learning panel on the Fuel page"></p>

Renders, not photos, with example values. Left: page 5 with E.V. Middle: page 5 with R.E.V, which adds a third job, "Learning". Right: the model selector on the app's Board page and R.E.V's learning status on its Fuel page.

## What you get

- **Every standard Mode 01 field** the car reports (70 are decoded), battery voltage, the VIN, and stored, pending and permanent trouble codes. One CSV row per poll cycle.
- **A small neural network on the chip.** It classifies the last 5 seconds of driving as stopped, braking, accelerating or cruising, and estimates three values the OBD port does not report: brake pressure, flywheel torque and fuel rate.
- **A save to the SD card every 10 seconds** and a **power-off line** on page 3: green "Safe to turn off" once everything is on the card, red with the reason otherwise. The drive ends by itself 15 seconds after the engine computer stops answering.
- **Fuel economy** in km/L, or mpg in the US and the UK, from the best source your car has. A car that gives nothing shows "--", never a made-up number.
- **Units that follow you.** The first time the board joins Wi-Fi it reads the country of the access point and switches to miles and mpg in the US or the UK.
- **Fill-up calibration.** Enter fill-ups in the desktop app, and the board corrects its fuel figures from what you actually pumped.
- **A fault engine** that ranks trouble codes by severity and groups codes that share a cause.
- **Dark and light screen themes**, by time of day or fixed.
- **A soft real-time scheduler** that times every job against its deadline and restarts the board if a step hangs.
- **Optional Wi-Fi, for the clock only**: one HTTPS request to one time server you can choose.
- **A desktop app** for Windows, macOS and Linux that imports drives, charts them, shows fuel per drive, switches the model and takes your fill-ups.

## Local-first

<p align="center"><img src="docs/images/workflow/local-first.png" width="900" alt="Local-first: no account, no cloud, AI on the chip, your SD card, optional Wi-Fi, your choice of dashboard"></p>

Most car loggers stream your driving to a server and do the thinking there. OpenCarMind does the thinking on the board, the way TinyML work runs networks on microcontrollers<sup>[[1]](#ref-1)–[[3]](#ref-3)</sup>. R.E.V's learning also happens on the board, and what it learns stays on your SD card. Your data is plain CSV, JSON and text on a card you hold.

Wi-Fi is optional and does one thing: while the car is parked on a known network, the board asks one HTTPS time server for the time. It checks the server's certificate, sends nothing else, and listens on no port. You can compile Wi-Fi out with `OCM_WIFI_CLOCK_SYNC`. On the ESP-Mosaico you can also search for a network and join it from the touch screen, for the same clock sync.

## How it works

<p align="center"><img src="docs/images/workflow/tool.png" width="900" alt="How it works: the engine computer answers OBD-II requests through a Bluetooth LE adapter, the ESP32-S3 board reads the car, runs the AI model and saves each drive to the SD card, and the desktop app imports drives"></p>

The board asks the engine computer for every field it supports, shows the live values on its screen and writes each drive to the SD card as its own session. At home, the desktop app imports new drives over the USB cable, or you take the card out.

### On-board inference

<p align="center"><img src="docs/images/workflow/inference.png" width="900" alt="On-board inference: an OBD row arrives, the last 5 s are binned, the model task runs E.V or R.E.V, and the answer goes to the .ml.csv file, the screen and, for R.E.V, its learner. The OBD task never waits"></p>

When an OBD row arrives, the board cuts the last 5 seconds into five one-second bins and runs the model in its own task, so logging never waits for it. Every row adds a line to the drive's `.ml.csv` file, and the newest answer shows on page 5. With R.E.V, the same run also feeds its learner.

### Scheduler and safety

<p align="center"><img src="docs/images/workflow/scheduler.png" width="900" alt="Scheduler and safety: the OBD poll, the model and R.E.V learning are timed on every run and checked once a minute. A hung step makes the watchdog restart the board, and three bad restarts in a row start safe mode with logging still on"></p>

Every periodic job has a period and a deadline. The scheduler in `main/sched_mon.c` times each run and, once a minute, checks the load against the rate-monotonic bound<sup>[[4]](#ref-4)</sup> and each job's worst-case response time<sup>[[5]](#ref-5)</sup>. A step that never returns resets the board, and after three bad restarts in a row it starts in safe mode with logging still on. R.E.V's learning runs at the lowest priority on a small budget of about 2 % of one core, so it never holds up the OBD poll or the model.

## The screens

<p align="center"><img src="docs/images/lcd-amoled.png" width="900" alt="Made for LCD, Feels like AMOLED: true black on a backlit screen"></p>

<p align="center">
<img src="docs/images/dash.png" width="190" alt="Page 1: dashboard">
<img src="docs/images/internals.png" width="190" alt="Page 3: internals and recent events, including Safe to turn off">
<img src="docs/images/faults.png" width="190" alt="Page 4: faults">
<img src="docs/images/system.png" width="190" alt="Page 5: system, jobs and AI">
</p>
<p align="center">
<img src="docs/images/dash-light.png" width="190" alt="Page 1 in the light theme: dashboard">
<img src="docs/images/internals-light.png" width="190" alt="Page 3 in the light theme: internals and recent events">
<img src="docs/images/faults-light.png" width="190" alt="Page 4 in the light theme: faults">
<img src="docs/images/system-light.png" width="190" alt="Page 5 in the light theme: system">
</p>

Pages 1, 3, 4 and 5 in the dark and light themes (renders, not photos). Page 2 is plain text and has its own picture at [docs/images/details.png](docs/images/details.png).

The dark theme is pure black, because on an LCD any tint in the background glows through the backlight. Text and colours are bright enough to read at a glance, and every status colour comes with a word.

Tap BOOT to change page. Hold it for 2 seconds to power off. The screen says "Turning off" while it saves and closes the drive, then "Off, safe to unplug", and the backlight and LED go dark. Hold it for 8 seconds to forget the saved adapter.

1. Dashboard: RPM, speed, engine temperature, battery, fuel economy and fuel level, with a CPU bar and an estimated power bar.
2. Details: adapter, VIN, protocol, trouble codes, clock, SD state and fuel used.
3. Internals: twelve extra fields, up to eight custom PIDs, and recent events, including "Safe to turn off".
4. Faults: ranked trouble codes with severity, description and likely cause, plus the connection and MIL status line.
5. System: instructions per second on each core (hardware counters), PSRAM in use, chip temperature, uptime, the jobs and their deadlines, what the model sees and its three estimates.

The screen is light from 06:00 to 17:00 and dark otherwise. `theme_mode`, the hours and the time zone are set in `config.json` (see [docs/CONFIGURATION.md](docs/CONFIGURATION.md)).

## Dashboard

`tools/ocm_report.py` turns a session folder from the SD card into one HTML report per drive, and [Dashy](https://github.com/Lissy93/dashy) shows them on your own computer or home server. Nothing is sent to the internet.

```sh
python3 tools/ocm_report.py /Volumes/<card name>/obd dashboard/site
cd dashboard && docker compose up -d      # Dashy on http://localhost:4000
```

## Desktop app

<p align="center"><img src="docs/images/workflow/clock.png" width="900" alt="Clock over Wi-Fi: parked on a known network, the board sends one HTTPS request to one time server, checks its certificate and reads the time. No upload, no open port"></p>

The app in [desktop/](desktop/README.md) runs on Windows, macOS and Linux. Over the board's USB cable it pairs with the board (you type a code from the board's screen once), sets Wi-Fi, time zone, time server and settings, imports drives and flashes new firmware, so the SD card never has to come out. It also switches the on-board model and keeps your fill-up log. [docs/DASHBOARD.md](docs/DASHBOARD.md) has the setup.

## Hardware

- Board: Waveshare ESP32-S3-LCD-1.47B (the pin map in `main/board.h` matches it).
- Or the Espressif ESP-Mosaico V1.2 (ESP32-S31) with the OpenCarMind EdgeOS 2.0.0 build (called mossREV up to 1.2.0). It needs no SD card: see [docs/MOSAICO.md](docs/MOSAICO.md#the-hardware).
- Adapter: a Bluetooth LE adapter that speaks ELM327 commands (see below). Classic Bluetooth adapters will not work, because the S3 has no classic radio.
- A FAT-formatted SD card.

### Adapter compatibility

Only the LELink has been driven with this board. The other rows come from vendor documents and other open-source OBD projects. [docs/ADAPTERS.md](docs/ADAPTERS.md) has prices and sources. The Amazon links are plain product pages with no affiliate tag, all opened on 3 November 2026.

| Adapter | Bluetooth | Built-in profile | Status | Amazon |
|---|---|---|---|---|
| LELink², LELink | LE | FFE0 / FFE1 | tested | [B0755N61PW](https://www.amazon.com/dp/B0755N61PW), [B00QJRYMFC](https://www.amazon.com/dp/B00QJRYMFC) |
| Veepeak OBDCheck BLE+, BLE | LE | FFF0 / FFF1 / FFF2 | expected | [B076XVQMVS](https://www.amazon.com/dp/B076XVQMVS), [B073XKQQQW](https://www.amazon.com/dp/B073XKQQQW) |
| OBDLink CX (sold as the Bimmercode edition, which is the only CX model) | LE 5.1, encrypted | FFF0 / FFF1 / FFF2, pairs on demand | expected | [B08NFLL3NT](https://www.amazon.com/dp/B08NFLL3NT) |
| Carista OBD2, Carista EVO | LE | FFF0 / FFF1 / FFF2 | expected | [B00YVHGTBM](https://www.amazon.com/dp/B00YVHGTBM), [B0CM9W4MCW](https://www.amazon.com/dp/B0CM9W4MCW) |
| Vgate iCar Pro BLE 4.0 | LE | 18F0 / 2AF0 / 2AF1 | expected | [B06XGB4873](https://www.amazon.com/dp/B06XGB4873) |
| Vgate vLinker MC+ | LE and Classic | E7810A71 / BEF8D6C9 | expected | [B088LW211V](https://www.amazon.com/dp/B088LW211V) |
| Vgate vLinker FS, MS | LE and Classic | E7810A71 / BEF8D6C9 | switch to "BLE+BT" in the VgateFwUpdater app first | [B0BZTXV6MZ](https://www.amazon.com/dp/B0BZTXV6MZ), [B0D6YQX4R8](https://www.amazon.com/dp/B0D6YQX4R8) |
| Panlong OBD2 Scanner (PL-B04) | LE, works with Car Scanner on iOS | generic fallback | expected for the "For iOS Android" option only. The "For Android Only" option is likely Classic. The page title does not name the model | [B0C1GDJ9ZJ](https://www.amazon.com/dp/B0C1GDJ9ZJ) |
| DA200 (Bluetooth 5.4, "AI Repair Tips") | LE, PIC18F25K80 ELM327-type chip (maker's spec sheet) | generic fallback | expected. The page title does not name the model, check for DA200 on the box | [B0FH2GJTP8](https://www.amazon.com/dp/B0FH2GJTP8) |
| ELM327 BLE clones (for example RODEDIO) | LE on iOS | FFF0 or FFE0 | expected for most. Buy one whose listing says Bluetooth LE for iOS | [B0GWQKLTMR](https://www.amazon.com/dp/B0GWQKLTMR) |
| Adapters running the Deep OBD firmware | LE | FFE0 / FFE1 / FFE2 | expected | not sold on Amazon. Layout from the Deep OBD source, [BtLeGattSppAndroid.cs](https://github.com/uholeschak/ediabaslib/blob/master/EdiabasLib/EdiabasLib/BtLeGattSppAndroid.cs) |
| FIXD | Bluetooth | unknown | unverified | [B013RIQMEO](https://www.amazon.com/dp/B013RIQMEO) |
| BlueDriver | Bluetooth | private protocol | not expected | [B00652G4TS](https://www.amazon.com/dp/B00652G4TS) |
| V800 full-system scanners (Panlong PL-V800 and white-label copies) | Bluetooth | private app (INNOOBD+) | not expected. The title says "Engine Transmission ABS SRS", not V800 | [B0GQ3D57PK](https://www.amazon.com/dp/B0GQ3D57PK) |
| OBDLink MX+, OBDLink LX, KONNWEI KW902 | Classic only (LX: Bluetooth 3.0, KW902: Android and Windows only) | none | will not work. The KW902 listing showed "currently unavailable" on 3 Nov 2026 | [B07JFRFJG6](https://www.amazon.com/dp/B07JFRFJG6), [B00H9S71LW](https://www.amazon.com/dp/B00H9S71LW), [B073F57QT3](https://www.amazon.com/dp/B073F57QT3) |

## Build and flash

You need ESP-IDF v6.1 and Python 3.

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

For the ESP-Mosaico, build with `idf.py --preview set-target esp32s31` and flash as [docs/MOSAICO.md](docs/MOSAICO.md#flashing) describes (no buttons, esptool 5.6.0 or later).

The first build downloads ESP-DL, LVGL, led_strip and cJSON through the IDF component manager. If you built an earlier version in the same folder, delete `sdkconfig` first. If the board stays in download mode after flashing, reset it with `python -m esptool --chip esp32s3 -p <port> --after watchdog-reset chip-id`.

On first boot the board connects to the strongest nearby adapter that looks like an OBD adapter and remembers it. A later S3 release will ask first: a new adapter offered only at -70 dBm or better, and saved only when you tap BOOT within 5 s. It is in this source but off for the S3 by default (`CONFIG_OBD_BLE_PAIR_CONFIRM`). To try it, add `CONFIG_OBD_BLE_PAIR_CONFIRM=y` to `sdkconfig.defaults.esp32s3` and delete `sdkconfig` before you build. It is host-checked, not yet tested on a board. If yours is not found, add its name to `OBD_BLE_NAME_PREFIXES` in `idf.py menuconfig` under OBD Logger Configuration. Settings, SD card files, the Wi-Fi list and the CSV columns are in [docs/CONFIGURATION.md](docs/CONFIGURATION.md).

## User manuals

Two user manuals for drivers and test users, each as Markdown here and as a page on the website:

- [OpenCarMind 2.5.1 on the ESP32-S3 board](docs/manual/esp32-s3.md) ([web page](https://alveeerahman.github.io/OpenCarMind/manual/esp32-s3.html)): the five pages, the BOOT button, the two models, Wi-Fi for the clock and the desktop app.
- [OpenCarMind EdgeOS 2.0.0 on the ESP-Mosaico](docs/manual/edgeos-2.0.0.md) ([web page](https://alveeerahman.github.io/OpenCarMind/manual/edgeos-2.0.0.html)): the 16 apps, Settings, Wi-Fi, the two models, the health check and every message the board shows.

Both come from one source each in [docs/manual/src](docs/manual/src), built by [docs/manual/build.py](docs/manual/build.py).

## The models

<p align="center"><img src="docs/images/ev-multihead.gif" width="900" alt="Animated, CarMind E.V in four steps: five seconds of driving from 10 OBD signals cut into one-second bins, one shared trunk of 1-D convolutions, two heads (a manoeuvre class of stopped, braking, accelerating or cruising, and estimates of brake pressure, torque and fuel rate), and the answer for every OBD row on the road"></p>

Both models are small quantised 1-D convolutional networks run by ESP-DL<sup>[[6]](#ref-6)</sup>: 8-bit weights and 16-bit activations, which makes them small and fast enough for a microcontroller. With every OBD row the selected model classifies the last 5 seconds and estimates brake pressure, flywheel torque and fuel rate. They were trained on public data, the HCRL Driving Dataset<sup>[[7]](#ref-7)</sup> and the Vehicle Energy Dataset<sup>[[8]](#ref-8)</sup>, plus drives from my own car. Scores on held-out drives and unseen cars are in the [detailed README](https://github.com/AlveeeRahman/OpenCarMind/blob/rev-c2/README.md#the-models).

The [model card](MODEL_CARD.md) puts every step from E.V to the final R.E.V in one table: size, speed, classifier and fuel-learner scores, before and after, with a key to each metric and where it was measured.

### The test drives

Two additional drives serve as a held-out test set. They cover frequent stops, an extended idle, steady cruising with and without cruise control, and hard acceleration and braking. No model on `main` saw these drives during training or tuning. They are scored as data from an unseen vehicle: no per-vehicle settings carry over, and every learner starts from its factory state.

<p align="center"><img src="docs/images/test-drives-timeline.png" width="900" alt="Two held-out test drives: speed, throttle, accelerator pedal and engine rpm over time, with bands for stops, an extended stop, steady holds and holds with the pedal released"></p>

Method and a comparison with routine driving: [docs/TEST_DRIVES.md](docs/TEST_DRIVES.md).

### CarMind R.E.V results so far

<p align="center"><img src="docs/images/leaders.png" width="900" alt="CarMind model leaders: the models on main, tested on two drives they never saw, scored as an unknown car. R.E.V: 94.5 percent of windows right, cruising 88.9, braking 98.4, accelerating 87.7, stopped 99.4, earlier held-out drives 95.1, KIA test 87.1, fuel R squared 0.867 learned on 31 cars. R.E.V (old): 71.8 percent, cruising 7.9. E.V: 73.5 percent, cruising 14.6, does not learn"></p>

<p align="center"><img src="docs/images/board-bench.png" width="900" alt="On-board benchmark on two unseen test drives, 1,119 labelled windows, every number measured on the ESP32-S3. Accuracy, average recall over the four classes, macro-F1 and ROC-AUC: CarMind R.E.V 93.8 percent accuracy, 92.9 average recall, macro-F1 0.917, ROC-AUC 0.995; MLP 81.0; TCN 80.2; CarMind E.V 73.5; CarMind R.E.V (old) 71.8; GRU 40.7"></p>

<p align="center"><img src="docs/images/learner-board.png" width="900" alt="Fuel learner leaderboard on 31 unseen VED cars with the airflow hidden, ranked by R squared with 1 percent bad labels: Huber RLS (R.E.V) 0.867, ANIL 0.774, no learning 0.709, Plain RLS (R.E.V old) 0.696, Meta-SGD 0.151"></p>

R.E.V's learners have been tested in replay on recorded drives from public test cars, through the firmware's own code with faulty readings mixed in, and on two drives in my car. On cars it never saw, learning in the car made its fuel estimate clearly better than the factory one when the airflow reading was missing. The tables are in the [detailed README](https://github.com/AlveeeRahman/OpenCarMind/blob/rev-c2/README.md#carmind-rev-results-so-far).

### How the models are made

<p align="center"><img src="docs/images/workflow/training.png" width="900" alt="How the models are made: drive data trains E.V with quantisation-aware training. R.E.V starts from E.V's design, is compressed by structured pruning to about half the parameters, fine-tuned with knowledge distillation with E.V as the teacher, quantised to int8 weights and tested on unseen cars"></p>

E.V is trained so that the version on the chip scores like the one on a computer. R.E.V starts from E.V's design, is pruned to about half the parameters, and learns from E.V as its teacher on a wider set of public drives. The training code and data stay in a private repository. The shipped files are in [ml/onboard](ml/onboard) and [ml/onboard/rev](ml/onboard/rev), and [docs/MODEL.md](docs/MODEL.md) lists their inputs and outputs.

### What R.E.V learns

<p align="center"><img src="docs/images/workflow/rev-learning.png" width="900" alt="How CarMind R.E.V keeps learning in the car. The frozen network gives 64 features. Manoeuvre class and brake pressure are fixed, engine torque is a physics estimate sized from the airflow constant, and the fuel rate's read-out is refit on the board and goes live only if it is better on held-back rows. An airflow constant is learned from load and rpm, fill-ups give one calibration factor, and the tank size is learned from the fuel level"></p>

The network itself never changes in the car, which protects it from forgetting what it learned in training<sup>[[9]](#ref-9)</sup>. Small parts on top of it learn, each only from something the car gives it to check against.

| Output | In the car | Learns from |
|---|---|---|
| Fuel rate | learns | the car's own fuel rate, or fuel worked out from the airflow sensor |
| Airflow constant (also the engine size) | learns | airflow, engine load and rpm while the airflow sensor works |
| Fill-up calibration, both models | learns | the litres you pump, entered in the app |
| Fuel tank size, both models | learns | the fuel used against the drop in the fuel level |
| Engine torque | learns, through the engine size | the energy in the fuel minus friction, using the engine size R.E.V learns. Planned: a learned torque read-out on cars that report torque (see Limitations) |
| Manoeuvre class | fixed | nothing yet: the car gives no label for it |
| Brake pressure | fixed | nothing: no car reports it |

### How R.E.V learns on the board

<p align="center"><img src="docs/images/rev-learner.gif" width="900" alt="Animated, R.E.V's fuel learner in five steps: the network stays frozen and only the 65-weight fuel read-out learns, every label passes a gate that drops implausible ones, unusual labels count less and old drives fade, a refit every 30 windows goes live only if it wins on the last 150, and the learned estimate follows the true fuel rate better than the factory one"></p>

The last layer, which turns the network's features into a fuel rate, is refit from your car's own readings. A refit goes live only if it beats the current one on recent driving, and the board goes back to the factory version if learning ever makes things worse. Since 2.5.1 the learner ignores implausible readings and gives less weight to unusual ones<sup>[[10]](#ref-10)</sup>, so a faulty sensor cannot spoil what it learned. When the car reports neither fuel rate nor airflow, the board uses whichever estimate has been most accurate on your car so far and shows it in amber. What R.E.V learned is saved to the SD card while you drive.

### Looking inside R.E.V with explainable AI

On R.E.V's first drives in my car, the screen said "braking" while I held a steady speed with my foot on the gas. E.V does the same. I opened up R.E.V with explainable-AI methods, Grad-CAM<sup>[[11]](#ref-11)</sup> and a probe on its internal features, and found that the network's features already told cruising from braking. Only the last layer that picks the class had it wrong. Correcting that one layer fixed it, and everything R.E.V had already learned in the car stayed valid.

<p align="center"><img src="docs/images/rev-pca.png" width="900" alt="R.E.V's 64-value embedding of my drives drawn with PCA, with the first and the corrected classifier head side by side. Accelerating, braking and foot-on-gas cruising sit in their own regions. The first head gets most of the cruising region wrong, and the corrected head gets few windows wrong"></p>

Each dot is a 5-second window of my driving, placed by what the network sees. The points are the same on both sides. Only the layer that reads them changed. The Grad-CAM maps and the scores are in the [detailed README](https://github.com/AlveeeRahman/OpenCarMind/blob/rev-c2/README.md#looking-inside-rev-with-explainable-ai).

### How R.E.V got better, step by step

1. **2.5.0.** R.E.V ships: half of E.V's size, and it learns your car's fuel use, airflow and tank in the car.
2. **First drives.** The learners worked, and the classifier called steady cruising "braking".
3. **Explainable AI** showed the error was in the classifier's last layer, not in what the network sees.
4. **A corrected last layer** fixed it without touching what R.E.V had learned.
5. **Fault tests** showed that a few bad sensor readings could stop the fuel learner.
6. **A fuel learner that shrugs off bad readings**, checked on public cars and against two published meta-learning methods. It came out ahead, so it ships in 2.5.1.

The numbers for each step are in the [detailed README](https://github.com/AlveeeRahman/OpenCarMind/blob/rev-c2/README.md#how-rev-got-better-step-by-step).

### Edge learning: R.E.V picks what to learn

<p align="center"><img src="docs/images/workflow/edge-learning.png" width="900" alt="Edge learning, in preparation. Every second R.E.V scores a window and the board computes its margin, the gap between the two highest class probabilities, and holds the window in a small buffer. About 5 s later a physics judge gives a verdict or stays undecided. When it decided, the board learns from the window if the margin is under a threshold or the board was wrong, with a ridge refit of the class read-out behind a holdout gate. When it did not decide, the board keeps the lowest-margin windows, at most 40 per drive and 5 s apart, on the SD card for a label sheet. The threshold adjusts itself to a learn budget, and a drift alarm raises the budget"></p>

R.E.V's classifier does not learn in the car yet. Edge learning is how I plan to change that, and I am preparing it now. It is active learning that runs on the board itself: the board decides which windows are worth learning from, and which few are worth your time later. Nobody in the car labels anything, and no computer takes part while you drive.

The phrase "edge learning" already exists as a broad term for training models on edge devices<sup>[[15]](#ref-15)</sup>. Here I use it as the name for one specific method, four rules built for a microcontroller whose teacher is free, late and sometimes silent.

The teacher is a physics judge. About 5 seconds after a window, what the speed, throttle and rpm did next usually shows whether the car was stopped, braking, accelerating or cruising. Sometimes it cannot tell, and the window stays undecided. ESP-DL runs the network but does not train it<sup>[[6]](#ref-6)</sup>, so the learning happens in the class read-out, refit by least squares in C, the way the fuel read-out learns today.

The four rules:

1. Score every window by its margin, m = p₁ − p₂, the gap between the two highest class probabilities. A small gap means R.E.V is torn between two answers, most often braking and cruising. Margin is a standard active-learning score<sup>[[16]](#ref-16)</sup>, and the board already logs the probabilities in every `.ml.csv` line.
2. Keep the learn rate on budget with a threshold that moves itself, $\tau \leftarrow \tau + \eta\,(b - \mathbf{1}[m < \tau])$. Here b is the share of windows to learn from, η is a small step, and the indicator 1[m < τ] is 1 when a window's margin is under the threshold. If the board learns too often, the indicator is 1 more than b of the time and τ shrinks. If it learns too rarely, τ grows. It settles where a share b of the margins fall below it. This is Robbins–Monro stochastic approximation<sup>[[17]](#ref-17)</sup> with a constant step, so τ keeps following the margins as the car or the driving changes. Žliobaitė et al. move an uncertainty threshold after every decision in a similar way for drifting data streams<sup>[[18]](#ref-18)</sup>, with a multiplicative step and no target rate. The rule stores two numbers.
3. Learn from a window when its margin is under τ, or when the judge's verdict shows the board was wrong. The update is a ridge refit of the class read-out, with a holdout gate and one saved state per car, like the fuel learner.
4. Keep undecided windows by margin. These are the only windows where a label costs anything, because a person has to give it. The board will write up to 40 per drive to the SD card, at least 5 s apart because windows overlap, and the desktop tools turn them into a label sheet.

To test the rules before writing them for the board, I copied the board's read-out learner into Python and streamed seven drives from a second car, one R.E.V never trained on, in the order they were driven. It started from the shipped R.E.V. Each window was scored first and learned from afterwards, so no window graded a model that had already seen it.

| Rule, at a 10 % learn budget | Judged windows learned | Macro recall over the stream |
|---|---|---|
| No learning | 0 % | 0.857 |
| Learn every judged window (stopped ones 1 in 10, as on the board) | 56 % | 0.954 |
| Random, 5 seeds | 5.8 % | 0.935 ± 0.008 |
| Margin only | 5.5 % | 0.944 |
| Margin or mistake (edge learning) | 5.7 % | 0.953 |

Margin or mistake matched learning from every judged window while learning from about a tenth as many, so the board runs the solver about ten times less often. Two cautions. This is one stream of seven drives with one run of each margin rule, so the gap to random is about two standard deviations. That is a good sign, not a settled result. And a read-out adapted to the new car scored lower on other cars' validation windows, KIA from 0.778 to 0.728 and my car from 0.969 to 0.935. One saved state per car is there so that this adaptation never reaches another car's read-out.

Three ideas of mine failed, and each was meant to be the new part:

- Posterior flip probability. A ridge read-out is a Bayesian linear model, so I computed in closed form the chance that a read-out drawn from its posterior would change the board's answer. At ranking the board's real mistakes it scored an AUROC of 0.874, against 0.942 for plain margin. The mistakes here were systematic, such as a throttle that reads high at idle, not windows the read-out was unsure about.
- A loss predictor after Yoo and Kweon<sup>[[19]](#ref-19)</sup>, trained on the board by the judge's late verdicts. AUROC 0.908, still below margin.
- Learning only from high-loss windows, which are mostly the board's mistakes. At a 5 % budget it reached 0.927 against 0.943 for margin. A learner that sees mostly its own errors gets a skewed sample, the sampling bias Dasgupta describes for active learning<sup>[[20]](#ref-20)</sup>.

Status: the rules run in simulation only. The board code, the buffer in PSRAM and the drift alarm are not written, and none of this is in the firmware on `main`.

## Status

Checked on the bench or in code: the build, the host unit tests (including R.E.V's learners with faulty readings), the switch to miles and mpg, the 10-second save, "Safe to turn off" and the time server rules. CI builds the desktop app on Windows, macOS and Linux. I have driven the board with the drive-end, power-off, fuel economy and units features, and R.E.V on two drives in my car.

Not checked yet: R.E.V over many drives and other cars, the corrected R.E.V on the road, the HTTPS clock on the board, other boards, and any adapter other than the LELink. The checks that need a car are in [docs/FIELD_CHECKS.md](docs/FIELD_CHECKS.md), with tables for you to fill in. Known security limits are in [SECURITY.md](SECURITY.md#known-limits).

## OpenCarMind EdgeOS 2.0.0 (ESP-Mosaico)

<p align="center"><img src="docs/images/edgeos.gif" width="900" alt="Animated, OpenCarMind EdgeOS 2.0.0 on the ESP-Mosaico in six steps: a layer of rules on top of FreeRTOS, under the models and apps. Boot health check: each part checked once after a start, health good at 68.0 s, safe mode with Wi-Fi held if bad. Wi-Fi held until healthy: no radio at boot, released at 68.0 s with 131,975 B of internal RAM free. Model admission: CarMind R.E.V answers in 4.2 ms against a 1,000 ms deadline and is admitted with a margin of 995.8 ms, and a model that would miss its deadline is refused. Parked or Driving: during a drive a model switch, the clock and a format are refused because a drive is running, while the model keeps running. Degradation ladder, in a forced test with extra load: UI slowed at 110.6 s, learner paused at 122.6 s, model paused at 134.6 s, back to normal at 348.6 s, and the model is never swapped. Battery protection, in a board test at lowered limits: hot at 34.2 C sheds load, cooled at 33.7 C restores it, and over-voltage at 4188 mV saves the logs and stops. Internal RAM stays above 60 KB in every phase, lowest about 81 KB"></p>

<p align="center"><img src="docs/images/mosaico-rev.png" width="900" alt="OpenCarMind EdgeOS 2.0.0 on the ESP-Mosaico, rendered from the 2.0.0 screen code with example values: three 480 by 480 screens in the dark, light and dark themes. Drive at 64 km/h and 2350 rpm with the AI seeing Cruising 87 % sure, brake 6.3 bar, torque 85 N m and fuel 4.1 L/h, and the tank at 46 %, about 310 km. AI with the four class bars and its estimates. Learning with 14 updates kept, the air constant, a 50 L tank from 3 fill-ups and the model card"></p>

OpenCarMind also runs on Espressif's ESP-Mosaico<sup>[[12]](#ref-12)</sup>, an ESP32-S31 board with a 480 × 480 AMOLED touch screen, 32 MB of PSRAM and a 128 MB NAND for the drive logs. Up to version 1.2.0 that build was called mossREV. Version 2.0.0 is called OpenCarMind EdgeOS 2.0.0, or Edge OS for short. It is not related to Ubiquiti's EdgeOS router software.

Edge OS comes from the same source tree as the S3 version and keeps everything mossREV 1.2.0 has: both models (CarMind R.E.V, the default on a new board, and CarMind E.V, each re-quantized for the S31), the 16 apps on the touch screen with Wi-Fi search and join, a light and a dark theme, and per-drive event logs. What 2.0.0 adds is a small operating-system layer, described below. The image above is rendered on a computer from the 2.0.0 screen code (`tests/host/uishot`, the same LVGL code the board runs) with example values. It is not a photo of the board. The OS layer does not change the S3 build. Up to 4aeff43, every byte the S3 chip loads was the same as in the mossREV 1.2.0 tree. The pairing change merged after the board check is behind one switch, `CONFIG_OBD_BLE_PAIR_CONFIRM`, which is off for the S3 by default. With it off, the S3 image built from this source equals the 4aeff43 S3 image except the build time, the ELF SHA-256 and the image's checksum and hash (see [Changes after the board check](#changes-after-the-board-check)).

Edge OS 2.0.0 has been through most of its board check, on build 4aeff43. The soak and a few parts are still open, and four changes merged after the check have not been on a board yet. Until those are done, mossREV 1.2.0 stays the Mosaico release. The [status table](#what-is-verified-on-the-board) at the end of this section says what the board showed.

The full guide is [docs/MOSAICO.md](docs/MOSAICO.md): the hardware, flashing, every app, the two models, Smart swipe and Smart UI, the storage, the NAND, the OS layer and the known limits. The [Edge OS user manual](docs/manual/edgeos-2.0.0.md) explains every screen and message for a driver.

### What it is

Edge OS is not a new kernel. FreeRTOS, as ESP-IDF 6.1 ships it, still schedules the tasks on the S31's two cores. Edge OS is a set of rules on top of it that decide what may run, when and how fast, and what happens when something goes wrong. It adds no task of its own. Its code runs inside the tasks that already exist: the board task every 2 s, the screen task, the USB door and the scheduler monitor.

The rules live in one plain C file, `main/os_core.c`, with no ESP-IDF calls in it, so the host tests on a computer check every rule. `main/os.c` connects the rules to the firmware. Files that the S3 build shares call the OS through macros in `main/os_hooks.h`, which turn back into the old S3 statement on the same line. That is how the OS layer leaves the S3 image as it was.

### The parts and why each exists

| Part | What it does | Why it is there |
|---|---|---|
| One service table | Eight services (model switch, clock, format, Wi-Fi edit, Wi-Fi radio, the USB door's commands, the model, the R.E.V learner) each have a mask of the runlevels they may run in. Each model and learner also declares its period, deadline, stall limit, CPU share, self-test and fallback model in one contract table. | Before 2.0.0 these rules were separate `if` checks spread over the screen, the USB door and the board task. Now one table holds them, and a source audit in the host tests fails if a file decides a refusal by itself. |
| Runlevels | Boot, Safe, Parked, Driving, Saving and Update. Several can hold at once, and a service runs only if every active runlevel allows it. | In the middle of a drive, a model switch would mix two models' answers in one log, a clock change would break its timestamps, and a format would erase the drive being written. The board refuses them in one place and says why. |
| Boot health check | About 65 s after a start the board rates itself good, degraded or bad from the model self-test, the flash FAT check, the NAND, the first screen update, the touch controller and the scheduler's misses, stalls and response times. | It gives you one line to read on the Board page. It is also the signal an A/B rollback needs to keep or reject a new image. |
| Model admission | A model loads only if response-time analysis says every job on its core still meets its deadline (see [below](#model-admission-by-response-time-analysis)). | A model that is too slow would make the OBD poll or the learner miss deadlines. The check refuses it before it loads, and the old model stays. |
| Degradation ladder | When the model misses deadlines, the board steps down: first the AI services slow to a tenth of their rate with their learned heads off (new, being checked on the board), then the screen refreshes every 33 ms instead of 16 ms and the AI services pause, then the R.E.V learner pauses, then the model's inference pauses. A minute without misses steps back up, in reverse order. Misses of the screen or the AI services alone reach the first rung only. | Estimates such as the battery SoC, screen frames and learning are worth less than the log and the model during a drive, so they go first. The ladder never swaps models. |
| AI services | Small periodic AI jobs, the battery SoC and SoH estimator first, in one worker task below the screen and the models, admitted by the same response-time analysis (see [below](#ai-services-in-edgeos)). On build 13757fd the board admitted the worker on core 1 with R 2.00 ms. The estimates are being checked. | More small models are coming, a Bluetooth and Wi-Fi intrusion detector next. Each one gets a deadline, a CPU budget and a place on the ladder, instead of its own task and its own rules. |
| Battery protection | Every 2 s it reads the gauge's voltage, current and temperature. At 45 °C on USB it sheds load, at 60 °C it stops safely, and at 4.25 V or more it reports a charger fault and stops. | The TP4057 charger has no enable pin and no thermistor input, and its supply comes from USB through diodes. The firmware cannot stop charging, so it watches and stops cleanly instead. The thresholds are placeholders until the cell's datasheet is known. |
| Wi-Fi boot hold | No Wi-Fi starts at boot. The automatic start waits for the health verdict and goes ahead on good or degraded. Your own start in the Wi-Fi app is never held. | On mossREV 1.2.0 rc3 the boot window was the internal-RAM low point, 57.7 KB free while the Wi-Fi driver started (measured on the board). |
| One reset path | Every reset the firmware asks for goes through `sys_reset(reason)`: mark it as asked for, flush and lock the flash FAT and the NAND, then reset. The screen stall guard and the hung-job restart skip the mark, so three of them in a row still start safe mode. | A reset between a sector's erase and its write had torn the FAT and lost the whole volume twice on the bench, before the storage fixes. Now no asked-for reset skips the flush. |
| Feature gate | One OS service runs the A/B test for Smart swipe and the Smart UI prefetch: the 50/50 split, the Mann-Whitney test, the Holm correction and the verdict per firmware version. | Each feature had its own copy of the same statistics. Now the thresholds sit in one place that no feature can change, and Smart swipe gave back 776 bytes of internal RAM. |
| PPA and 2D-DMA graphics | The launcher slide's window copies, the page pictures' byte swap and large opaque LVGL fills and image copies run on the S31's pixel-processing accelerator and 2D-DMA, with a CPU fallback. | The CPU did these copies row by row. The chip has engines for them. A full frame still needs 23.6 ms on the panel's QSPI link, so the gain is in composing, not in sending. |

### Services and runlevels

<p align="center"><img src="docs/images/edgeos-runlevels.png" width="900" alt="How the OS decides what may run, drawn as a hand-drawn flow. Step 1, inputs the OS reads: fresh on every check, from start-up, the count of abnormal resets, the drive log and the car link, the battery gauge and USB power, and the reset path and the format, read in rl_inputs() in main/os.c. Step 2, it sets the active runlevels: Boot when start-up has not finished, Safe after 3 abnormal resets in a row, Driving when a drive is recorded or the car's computer answers, Saving when the battery is hot or over-voltage or USB is lost (UPS option, off), Update when a reset, a door entry or a format is running, and Parked when none of these holds. Several can hold at once, for example Safe while Driving. Step 3, a service asks to run: model switch, clock, format, Wi-Fi edit, Wi-Fi radio, USB door, model or R.E.V learner, one mask each in MASK[] in main/os_core.c. Step 4, os_allows(service): every active runlevel must allow the service, and one that does not is enough to stop it. Step 5, yes: the service runs. No: refused with the phrase of the highest blocking runlevel, highest first: Update, an update or format is running. Saving, the board is saving power. Driving, a drive is running, or the car link is up when only the link is on. Safe, safe mode is on. Boot, the board is starting. Parked never blocks, because every service allows it. Separately, once per boot: step 6, boot health at 65 s rates the board good, degraded or bad. Step 7, good or degraded: the Wi-Fi task starts. Bad: Wi-Fi is held until you start it, and a start you make yourself in the Wi-Fi app is never held."></p>

Which runlevels each service may run in, from the mask table in `main/os_core.c`:

| Service | Boot | Safe | Parked | Driving | Saving | Update |
|---|---|---|---|---|---|---|
| Model switch (screen, desktop app) | no | no | yes | no | no | no |
| Set the clock by hand | yes | yes | yes | no | no | no |
| Format storage | no | yes | yes | no | no | no |
| Add or forget a Wi-Fi network | yes | yes | yes | no | no | no |
| Wi-Fi radio (search, join, clock sync) | yes | yes | yes | no | no | no |
| USB door commands after pairing | yes | yes | yes | no | no | no |
| Load the model at boot | yes | no | yes | yes | yes | no |
| R.E.V learner | yes | no | yes | yes | no | no |

Safe still allows a format, the clock, the door and Wi-Fi, so a board in safe mode can always be repaired.

### Model admission by response-time analysis

A model is a real-time job. It has to answer once per second while the OBD poll and the learner share its core. When a model loads at boot or on a switch, the OS takes its self-test run time, doubles it (one measured run is not a worst case), and uses that as the model's worst-case CPU time C. Then it runs the response-time analysis of Joseph and Pandya<sup>[[5]](#ref-5)</sup> for every job on that core:

$$
R_i = C_i + \sum_{j \in hp(i)} \left\lceil \frac{R_i}{T_j} \right\rceil C_j
$$

R<sub>i</sub> is job i's worst-case response time, the longest it can take from its release to its finish. C<sub>i</sub> is its worst-case CPU time. The sum runs over hp(i), the jobs on the same core with a higher priority. T<sub>j</sub> is the period of such a job j, so ⌈R<sub>i</sub> / T<sub>j</sub>⌉ counts how many times j can start, and take its C<sub>j</sub>, while job i waits. R<sub>i</sub> appears on both sides, so the OS starts from R<sub>i</sub> = C<sub>i</sub> and repeats the sum until the value stops changing. Job i meets its deadline when R<sub>i</sub> ≤ D<sub>i</sub>. If any job on the core would miss, a switch is undone like a failed load and the screen says, for example, "Not switched: CarMind E.V refused: ml would miss its deadline." (`ml` is the models' job.) A boot load that fails leaves the model off, with an error event.

On the board, the first boot of a 2.0.0 build before the rename (09ec8f5) logged `admission CarMind R.E.V: C 4472 us (self-test 2236 us x 2.0) on core 1, R: ml 4.5/1000 ms, margin 995.5 ms (ml): admitted`. The log lists only the `ml` job: the other jobs on core 1 had no measured run yet at 4.3 s, so this first check held the model alone. Host tests pass for the textbook three-job set (C 3, 3, 5 and T 7, 12, 20 give R 3, 6, 20) and for 90,013 random job sets compared with the scheduler monitor's own loop.

### Scheduler budgets per core

<p align="center"><img src="docs/images/edgeos-scheduler.png" width="900" alt="Scheduler budgets per core, drawn as Gantt lanes. Core 1, one 1,000 ms window on an axis broken at 40 ms: obd_poll, priority 5, period 1,000 ms, deadline 30,000 ms, not run on the bench. The ml job, priority 3, period and deadline 1,000 ms, self-test 2.1 ms doubled to a budget of 4.2 ms, admitted with R 4.2 ms on 4aeff43. The rev job, the R.E.V learner, priority 2, period 1,000 ms, deadline 30,000 ms, a 2 % share of 20 ms per second with bursts of at most 30 ms, no run on the bench. Core 0, one 100 ms window: the ui screen job, priority 2, period and deadline 50 ms, worst response 7.8 ms and worst CPU 4.98 ms over 9,597 runs in the 10-minute soak on 06ddf1d, 0 misses. The LVGL refresh every 16 ms. Slow jobs over the first 120 s: boot on 4aeff43 with Wi-Fi held from 4.3 s, the health verdict due at 65 s and good at 68.0 s, the Wi-Fi driver up from 88.1 to 106.5 s and the clock synced at 98.0 s. The learner's 30 s deadline windows and 60 s stall limit. The model's 10 s stall limit. The link_search, obd_poll and ecu_pick deadlines are of 90, 30 and 10 s. The board task every 2 s and the scheduler summary every 60 s."></p>

Every timed job registers a period T and a deadline D with the scheduler monitor. The task it runs in sets its core and priority. The chart takes those from the code and puts the board's measurements inside them. The model gets 1,000 ms and its admission budget is 4.2 ms, so the first 40 ms of the core 1 axis are drawn 23 times wider, or the bar would be thinner than a pixel. No car was connected on the bench, so the OBD jobs, the model and the learner never ran as periodic jobs there. Their bars are the code's budgets. The measured ones are the model's self-test at admission and the screen job over the soak.

| Job | Task, core, priority | T | D | Budget in the code | On the board |
|---|---|---|---|---|---|
| `obd_poll` | OBD, core 1, 5 | 1,000 ms | 30,000 ms | none | not run, no car |
| `link_search`, `ecu_pick` | OBD, core 1, 5 | 90 s, 10 s | 90 s, 10 s | one run per search or connect | not run |
| `ml` | model, core 1, 3 | 1,000 ms | 1,000 ms | C = 2 × self-test, stall 10 s | self-test 2.1 ms, C 4.2 ms, R 4.2 ms, admitted (4aeff43) |
| `rev` | learner, core 1, 2 | 1,000 ms | 30,000 ms | 20 per mille of a core in 30 ms bursts, stall 60 s | contract logged, no runs (4aeff43) |
| `ui` | screen, core 0, 2 | 50 ms | 50 ms | none | 9,597 runs, worst response 7.8 ms, worst CPU 4.98 ms, 0 misses (06ddf1d) |

The screen job's 7.8 ms is wall time from the start of a run to its end, and 4.98 ms is the CPU the screen task used in it. The difference is time the task waited for other tasks on core 0. The response-time analysis counts only registered jobs, so it reports 4.98 ms for `ui`. The board task (every 2 s), the Wi-Fi task and the scheduler monitor run on core 0 outside the job table.

<a id="ai-services-and-the-battery-estimator"></a>

### AI services in EdgeOS

Edge OS runs its small AI models as services. The first is a battery estimator that gives the cell's state of charge (SoC) and state of health (SoH). A network intrusion detector for Bluetooth and Wi-Fi comes next. All services share one worker task at FreeRTOS priority 1, below the screen, the R.E.V learner, the model and the OBD poll, so any of those can preempt a service at any instruction. The worker is admitted by the same response-time analysis as the models. On build 13757fd the board admitted it on core 1 with a worst-case response of 2.00 ms against a 1,000 ms deadline, and the battery estimator's self-test took 570 µs of its 2 ms budget.

A service reads copies of its inputs and writes its results without a lock, so it cannot hold up the jobs above it. No service acts on power, and none cuts the car link during a drive. When the model misses deadlines, the AI services are the first thing the board slows. On 13757fd the boot health check rated the board good at 68.9 s and released Wi-Fi with 130,623 B of internal RAM free. The estimates themselves, the new ladder rung and the pairing rules are built and being checked on the board now.

<p align="center"><img src="docs/images/edgeos-ai-services.png" width="900" alt="Where the AI services run, hand-drawn. Step 1, who preempts whom, FreeRTOS fixed priorities: obd_poll 5, the ml model 3, the ui screen 2 on core 0, the R.E.V learner 2, the AI worker 1, on core 1 on build 13757fd with R 2.00 ms, then idle. Step 2, the worker's release table, deadline-monotonic: bat_est for battery charge and health, T 1 s, D 1 s, budget 2 ms, self-test 570 microseconds, on the board. ids, the network intrusion detector, T 5 s, D 1 s, about 60 integer ops, shadow mode first, validated on the Mac, firmware service next. Future services plug in as one table entry each, with no new task. Admission treats the worker as one job with C the sum of the budgets, T the shortest period and D the shortest deadline. Step 3, in: snapshots of the gauge's V, I and T and the radio counters, one writer and a seqlock, so a reader never waits. One service step takes no mutex, waits on no queue, does no I/O and allocates nothing. Step 4, out: a lock-free result struct the screen reads, and log rows into a ring the board task writes to the NAND. The ladder when the model misses deadlines: 1, AI services slowed to a tenth of their rate with learned heads off, new in this build and being checked on the board. 2, UI slowed to 33 ms and AI services paused. 3, learner paused. 4, model paused, while the drive log keeps running. An overrun finishes and skips its next release. Board, build 13757fd: boot health good at 68.9 s, worker admitted on core 1 with R 2.00 ms and a 998 ms margin, battery self-test 570 microseconds, Wi-Fi released with 130,623 B free."></p>

| Model | What it does | Cost per step | Status |
|---|---|---|---|
| Battery charge (SoC): extended Kalman filter on a one-RC cell model | counts the charge and corrects it with the voltage, once a second | 111 flops and 1 exp, 324 B | on the board with placeholder parameters. Self-test checked on 13757fd, estimates being checked |
| Battery health (SoH): counted capacity and resistance trend | capacity between two charge anchors, and a DC resistance flag | a few ops, inside the charge estimator | on the board, being checked |
| Battery health: partial-charge regressor | SoH from an ordinary charge, with no full cycle | 8 multiply-adds and 16 ops, 100 B | validated on the Mac, not in the firmware yet |
| Battery anomaly detector | an alarm when 60 s windows stop looking normal for this cell | under 400 ops a window, 920 B | validated on the Mac, not in the firmware yet |
| Network intrusion detector: rotation detector | scores each 5 s of Bluetooth and Wi-Fi traffic for address-rotation floods | about 60 integer ops, 552 B per head | validated on the Mac, firmware service next |
| Pairing rules R1, R2 and R6 | adapter fingerprint, no silent re-pair, clock jump guard ([Pairing safety](#pairing-safety-built-not-yet-checked-on-the-board)) | one compare per link or clock sync, 48 B in NVS | on the board, being checked |
| Residual MLP, 3-state EKF, neural ECM, KalmanNet-lite, pure MLP, general IDS learners | learned replacements or corrections | | tested, not shipped (see the last dropdown) |

<details>
<summary><b>For experts: how the battery charge estimator works</b> (figure, equations, test results)</summary>

<p align="center"><img src="docs/images/edgeos-ai-soc.png" width="900" alt="How the battery charge estimator works, hand-drawn. On the board with placeholder parameters. The gauge gives voltage V, current I and temperature T from the sample the board task already reads, with no new I2C traffic, and the gauge is never written. The cell model is one RC pair: an open-circuit voltage source OCV(z), a series resistance R0, then R1 in parallel with C1, so V equals OCV(z) plus v1 plus R0 times I. OCV is a 41-point table over the charge z from 0 to 1, and tau equals R1 times C1. The filter runs one step per second on the state z and v1 with covariance P. Predict: z minus equals z plus eta I delta t over 3600 Q SoH, v1 minus equals a v1 plus 1 minus a times R1 I with a equal to exp of minus delta t over tau, P minus equals F P F transpose plus W with F equal to diag 1, a. Expect: V hat equals OCV of z minus plus v1 minus plus R0 I, with H equal to dOCV/dz and 1. Correct: K equals P minus H transpose over H P minus H transpose plus R, x equals x minus plus K times V minus V hat, P equals P minus minus K H P minus. Anchors: after 300 s at rest z equals the inverse OCV of V, and at the end of a CV charge z equals 1. Tried on top of it and not shipped: a residual MLP with 11 inputs, 12 tanh units and one clamped output, and an EKF with a third state for the current offset. Results on the Mac: on the 18650 NMC cycling set RMSE 0.33 to 0.53 percent and worst 2.4 to 4.0 percent. Start 20 percent wrong: EKF 0.75 to 2.0 percent, coulomb counting 8.6 to 10.5 percent. NASA cells with the 18650 OCV curve: 7.9 to 21 percent. Cost 111 flops and 1 exp, 324 B."></p>

The cell is modelled as a voltage source that depends on the charge, OCV(z), in series with a resistance R₀ and one resistor-capacitor pair R₁ ∥ C₁. R₀ gives the instant drop when current flows. The RC pair gives the slow part, with time constant τ = R₁C₁. The filter keeps two states, the charge z (0 to 1) and the voltage v₁ on the RC pair, and a 2 × 2 covariance P that says how sure it is of them. Plett laid out this use of the extended Kalman filter for battery management systems<sup>[[21]](#ref-21)</sup>.

Every second it predicts. It counts the charge and lets the RC voltage decay towards its new level:

$$z_k^- = z_{k-1} + \frac{\eta\, I_k\, \Delta t}{3600\, Q\, \mathrm{SoH}}, \qquad v_{1,k}^- = a\, v_{1,k-1} + (1 - a)\, R_1 I_k, \qquad a = e^{-\Delta t/\tau}$$

Here I is the current in amps (positive while charging), Δt the seconds since the last step, Q the capacity in Ah, η the charge efficiency, and SoH the share of the capacity the cell still has. The covariance grows by the process noise W: P⁻ = F P Fᵀ + W, with F = diag(1, a).

Then it predicts the terminal voltage and compares it with the gauge:

$$\hat V_k = \mathrm{OCV}(z_k^-) + v_{1,k}^- + R_0 I_k, \qquad H_k = \left[\, \frac{d\,\mathrm{OCV}}{dz}\bigg|_{z_k^-} ,\quad 1 \,\right]$$

The gap V − V̂ is the innovation. The Kalman gain K decides how much of it goes into each state:

$$K_k = \frac{P_k^- H_k^{\top}}{H_k P_k^- H_k^{\top} + R}, \qquad x_k = x_k^- + K_k \left(V_k - \hat V_k\right), \qquad P_k = P_k^- - K_k H_k P_k^-$$

R is the variance of the voltage reading. Where the OCV curve is steep, the voltage says a lot about z, and the filter follows it. Where the curve is flat, dOCV/dz is small, K shrinks, and the filter trusts the charge count. That is how a wrong start fixes itself: with the start 20 % off, the EKF's error stayed at 0.75 to 2.0 % RMSE, where plain coulomb counting stayed at 8.6 to 10.5 %.

Two anchors reset the charge outright. After 300 s at rest the voltage is the OCV, so z = OCV⁻¹(V). At the end of a CV charge, z = 1. Until the first anchor the Board page says "learning". Two anchors at least 30 % of charge apart give a capacity sample for the health estimate.

In the firmware (`main/bat_est.c`, math in `main/bat_est_core.c`, model in `main/bat_model.h`) the step matches the Mac reference to within 1.1e-6 over 370 test steps (`make -C tests/host bat BAT_MODEL=<fitted header>`). It reads the gauge sample the board task already takes every 2 s, so there is no new I2C traffic, and the gauge is never written. The learnt values and the last SoC go to NVS at most every 10 minutes and on the safe-shutdown path. Every 10 s a training row (V, I, average I, T, the gauge's SoC and remaining capacity, the estimator's SoC, SoH and mode) goes to `obd/bat/bNNNNNN.csv` on the NAND, at most 2 MB in all, oldest file first out. Those rows are what the cell's own OCV curve will be fitted from. Until then the firmware runs placeholder parameters for a 4.2 V cell, and the nominal capacity is `CONFIG_OCM_BAT_NOMINAL_MAH`, 65 mAh from the cell's label (UNVERIFIED).

Tested on the Mac, held out from fitting:

| Test | 2-state EKF (shipped) | For comparison |
|---|---|---|
| An 18650 NMC cycling data set, held-out early, middle and late life | RMSE 0.33 to 0.53 %, worst 2.4 to 4.0 % | |
| Start 20 % wrong | RMSE 0.75 to 2.0 % | coulomb counting 8.6 to 10.5 % |
| NASA cells<sup>[[26]](#ref-26)</sup>, with the 18650 cell's OCV curve | RMSE 7.9 to 21 % | the OCV curve is specific to the cell, so it has to come from this board's logs |
| Current offset of +0.01C (0.65 mA on 65 mAh) | RMSE 5.3 % | the 3-state EKF: 0.27 to 0.79 % |
| Cost per step | 111 flops and 1 exp, 324 B | |

I checked where the EKF's remaining error comes from. It is a slow drift of about ±0.3 % from counting charge, and it does not sit at the low-charge knee, in the CV taper or with heat. An unscented Kalman filter<sup>[[22]](#ref-22)</sup> with one or two RC pairs gave the same RMSE. Neither the linearisation nor the model order is the limit. The biggest risk is a current offset, which the 3-state EKF in the last dropdown is kept ready for.

</details>

<details>
<summary><b>For experts: how the battery health estimate works</b> (figure, features, test results)</summary>

<p align="center"><img src="docs/images/edgeos-ai-soh.png" width="900" alt="How the battery health estimate works, hand-drawn. State of health is the capacity the cell still holds as a share of its label capacity. Card 1, counted capacity, on the board: two anchors at least 30 percent of charge apart give one capacity sample, and SoH equals the learnt capacity over the label capacity. A full charge counted to the cutoff: 0.12 percent error on the 18650 set and 1.2 percent on the NASA cells. Card 2, DC resistance trend, on the board: a current step gives the DC resistance over 120 s, with r of minus 0.88 against capacity, used as a flag, and 6.42 percent off as an SoH estimate on its own. Card 3, partial-charge regressor, validated on the Mac: features from an ordinary charge relative to the cell's first 10 cycles, the charge added between 3.9 and 4.1 V, dQ/dV peak height and position, CV duration and taper constant, DC resistance over 120 s and mean temperature. Ridge regression, 8 multiply-adds and 16 ops, 100 B, picked by forward cross-validation over a GBDT and an 8-unit MLP. Held-out cycles: on the 18650 set the ridge 1.45 percent, the DCIR trend 6.42 percent, the full-charge count 0.08 percent with a full cycle. On the NASA discharge-only cells the ridge 13.9 to 25.0 percent and the DCIR trend 8.5 to 19.2 percent."></p>

SoH is the capacity the cell still holds divided by its label capacity. The board gets it two ways.

The firmware counts it. Two anchors (a rest, or the end of a CV charge) at least 30 % of charge apart give one sample: the charge counted between them divided by the change in z. A full charge counted down to the cutoff was off by 0.12 % on the 18650 set and by 1.2 % on the NASA cells. Current steps also give a DC resistance (DCIR) over 120 s. It rises as the cell fades (r = −0.88 against capacity on the 18650 set), but as an SoH estimate on its own it is off by 6.42 %, so the board uses it as a flag.

A count from full to the cutoff needs the board to run its cell flat, which a board on the car's USB rarely does. So I trained a regressor on what an ordinary charge shows. Its features are taken relative to the cell's first 10 cycles: the charge added between 3.9 and 4.1 V, the height and position of the dQ/dV peak (the charge added per volt of rise), the CV duration and its taper constant, the DCIR over 120 s and the mean temperature. Ridge regression won the forward cross-validation (0.83 % against 1.96 % for a tree ensemble (GBDT) and 20 % for an 8-unit MLP). It costs 8 multiply-adds and 16 ops, in 100 B.

| Held-out cycles, forward in time (MAPE) | Partial-charge ridge | DCIR trend | Full-charge count |
|---|---|---|---|
| 18650 NMC cycling set | 1.45 % | 6.42 % | 0.08 %, needs a full cycle |
| NASA cells<sup>[[26]](#ref-26)</sup>, discharge only | 13.9 to 25.0 % | 8.5 to 19.2 % | not compared |

The regressor passes on the cell it learned and does not transfer to cells it never saw charge. It needs the board's own first charges as its reference. The plan is the count when there is a full cycle and the regressor otherwise. The regressor is trained and tested on the Mac, not in the firmware yet.

</details>

<details>
<summary><b>For experts: how the battery anomaly detector works</b> (figure, features, fault tests)</summary>

<p align="center"><img src="docs/images/edgeos-ai-anomaly.png" width="900" alt="How the battery anomaly detector works, hand-drawn. Validated on the Mac, not in the firmware yet. Card 1, every 60 s, 9 features from the EKF and the gauge: innovation mean and RMS as slow averages, voltage drift and current at rest, temperature against a one-node heat model, voltage sag per C-rate against R0 at that temperature, the DC resistance ratio at a current step, load level and gauge read glitches. Card 2, three detectors: a 9-4-9 tanh autoencoder raises the alarm from its reconstruction error, a z-score with a frugal median and MAD and an online Gaussian are diagnostics only. False alarms on 388 h of held-out normal data: autoencoder 0.0026 per hour with a 95 percent upper bound of 0.012, z-score 0.245 and Gaussian 0.126, which drift with ageing. Card 3: a 24 h warm-up, learning only from clearly normal windows, an alarm after 3 windows in a row, a banner and a log line, and it never acts on power. Under 400 ops per window, 280 B of state and 640 B of parameters. Injected faults caught and median time: resistance plus 100 percent 92 percent in 72 min, temperature plus 15 C 83 percent in 164 min, soft short 5 percent per hour 75 percent in 176 min, resistance plus 30 percent 25 percent, soft short 0.5 percent per hour 8 percent, current offset 8 percent, intermittent reads 0 to 8 percent, temperature plus 5 C, current gain error and extra drain 0 percent."></p>

The detector learns what normal looks like for this cell and raises an alarm when 60 s windows stop looking like it. Nobody labels a fault for it. Each window has 9 features from the EKF and the gauge: the innovation's mean and RMS as slow averages, the voltage drift and the current at rest, the temperature against a one-node heat model, the voltage sag per C-rate against R₀ at that temperature, the DC resistance ratio at a current step, the load level and the gauge's read glitches.

Three detectors ran side by side. One is a z-score against a frugal streaming median and MAD<sup>[[25]](#ref-25)</sup>, one an online Gaussian, and one a 9 → 4 → 9 tanh autoencoder whose score is its reconstruction error. All three warm up for 24 h, learn only from clearly normal windows, and alarm after 3 windows in a row. On 388 h of held-out normal data the autoencoder gave 0.0026 false alarms per hour (95 % upper bound 0.012). The z-score gave 0.245 per hour and the Gaussian 0.126, because the cell's ageing moved them. So only the autoencoder raises alarms, and the other two only log.

| Injected fault | Caught | Median time to the alarm |
|---|---|---|
| Resistance +100 % | 92 % | 72 min |
| Temperature +15 °C | 83 % | 164 min |
| Soft short, 5 % of capacity per hour | 75 % | 176 min |
| Resistance +30 % | 25 % | |
| Soft short, 0.5 % per hour | 8 % | |
| Current offset | 8 % | |
| Intermittent reads | 0 to 8 % | |
| Temperature +5 °C, current gain error, extra drain | 0 % | |

It catches large faults only. The thresholds were learned on public cell data, so they have to be learned again on the board's own cell before an alarm means anything there. An alarm shows a banner and writes a log line. It never acts on power: the fixed battery protection above stays in charge of that. The cost is under 400 ops a window, with 280 B of state and 640 B of parameters. It is trained and tested on the Mac, not in the firmware yet.

</details>

<details>
<summary><b>For experts: how the network intrusion detector works</b> (figure, rules, attack tests)</summary>

<p align="center"><img src="docs/images/edgeos-ai-ids.png" width="900" alt="How the network intrusion detector works, hand-drawn. Rotation detector validated on the Mac, rules R1, R2 and R6 on the board. Card 1, radio callbacks: BLE adverts and link events, Wi-Fi round events, counters and three 1,024-bit bitmaps per head, no locks and no allocation. Card 2, the scan head every 5 s window: three features that measure rotation, not crowd size: single-use new transmitters, churn and adverts per address. Score: a frugal streaming median m and MAD per feature, m moves by eta times s times the sign of x minus m, z equals x minus m over 1.4826 MAD, one-sided, plus a fixed prior of 15 or more single-use new transmitters in one window. Block when 3 of the last 6 windows exceed, learning only from clean windows with clamped medians, after a 2 h shadow. Below it, the link head for relay timing is advisory only, the Wi-Fi head looks for floods of rotating access-point addresses with the fixed prior only, and the fixed rules on the board are R1 adapter fingerprint, R2 no silent re-pair and R6 clock jump guard. Step 6, a block does only safe, reversible steps: no pairing offers or re-pair requests, the scan narrowed to the paired adapter, the Wi-Fi round aborted with the current time kept, rows kept out of the learner, lifted 10 min after the last bad window or from the banner. It never cuts the paired adapter while driving, transmits, or pairs on its own. On the Mac: 0 false blocks in 446 h of BLE adverts, 2 times 120 h of a crowd shift and 769 h of Wi-Fi traffic. Advert floods at 100 ms and 20 ms 80 of 80, the stealthy 500 ms flood 71 of 80, a slow poisoning ramp blocked at 4.9 adverts per second, Wi-Fi floods with rotating access-point addresses 15 of 15. Cost 40 B of state and 512 B of bitmaps per head, about 60 integer ops a window. A stealthy flood inside a crowd of 150 or more devices was not caught."></p>

The detector looks for a transmitter that changes its address on almost every advert, which is how Bluetooth spam floods and prompt-spam tools look. Each radio callback only sets bits and adds to counters. Every 5 s the IDS service turns three 1,024-bit bitmaps (seen this window, seen twice, seen last window) into three features:

- single-use new: transmitters seen once that were not there in the last window,
- churn: the share of transmitters that are new,
- adverts per address, which falls towards 1 in a flood.

A crowd of real phones repeats inside a window and keeps each address for minutes, so these features stay low in a busy car park. A raw count of adverts would not.

For each feature the board keeps a frugal streaming median m and median absolute deviation (MAD)<sup>[[25]](#ref-25)</sup>. After each window the median moves one small step towards the new value, m ← m + η·s·sign(x − m), where η is the step and s the feature's scale. Over time m settles where half the values lie above it, and it costs one number per feature. The MAD follows the same rule on |x − m|. Scoring is one-sided, z = (x − m) / (1.4826·MAD), so only more rotation counts. With the factor 1.4826 the MAD equals the standard deviation for Gaussian data. A fixed prior adds that 15 or more single-use new transmitters in one window is a flood whatever the board has learned. A block starts when 3 of the last 6 windows exceed. The first 2 h are a shadow that learns and never blocks. After that the baseline learns only from clean windows, and its medians are clamped, so a slow poisoner cannot drag them into flood territory.

A block only stops what the board would do on its own. It offers no new adapter and asks for no re-pair, narrows the scan to the paired adapter, aborts the Wi-Fi clock round with the current time kept, and keeps the rows out of the learner. The block lifts 10 minutes after the last bad window, and you can clear it from the banner. It never cuts the paired adapter while you drive, never transmits, and never pairs on its own. Cloned adapters, silent re-pairs and clock jumps are left to the fixed rules R1, R2 and R6 in [Pairing safety](#pairing-safety-built-not-yet-checked-on-the-board). A link head that watches for relay delay is advisory only: it marks rows unverified and blocks nothing.

The data: a public BLE scanner data set, used for local testing only (nothing fitted on it ships), the UJI Probes<sup>[[27]](#ref-27)</sup> and IPIN2021<sup>[[28]](#ref-28)</sup> Wi-Fi probe-request captures, and a synthetic crowd of 5 to 300 devices for the busy car park. Attacks were injected into them.

| Test on the Mac | Result |
|---|---|
| False blocks on normal traffic | 0 in 446 h of BLE adverts, 0 in 2 × 120 h of a crowd shift, 0 in 769 h of Wi-Fi management traffic. Target ≤ 0.05 per hour |
| Advert floods at 100 ms and 20 ms | 80 / 80 each, blocked 10 to 15 s after the start |
| Stealthy flood at 500 ms | 71 / 80. Inside a synthetic crowd of 150 or more devices: 0 / 15 |
| Slow poisoning, 0 to 50 adverts per second over 6 h | blocked at 4.9 adverts per second, and the block held at full rate |
| Wi-Fi floods of fake access points with rotating addresses | 15 / 15 on both captures |
| Cost | 40 B of state and 512 B of bitmaps per head, about 60 integer ops a window |

On the Mac the detector is bit-exact against its Python reference on 21,601 test windows. It is not in the firmware yet. Its service row is ready (T 5 s, D 1 s), and it will start in shadow mode, logging and never blocking, until a week of logs from real car parks shows its false-block rate on the board.

</details>

<details>
<summary><b>For experts: why the AI services meet their deadlines and cannot deadlock</b> (the scheduling argument)</summary>

The AI services are soft real-time jobs. A late result is worth less, never dangerous, so a miss is counted and fed to the ladder, never fatal.

Each service is one table entry with a period T, a relative deadline D ≤ T, a CPU budget C and four hooks (`main/ai_svc.c`, rules in `main/ai_svc_core.c`). Inside the worker, a release table picks the due service with the shortest deadline first (deadline-monotonic order), and a step runs to its end. One worker instead of one task per service saves a stack and a scheduler slot per service. The cost is that one service can wait for one step of another, and the admission covers that. The worker is analysed as one job:

$$C_w = \sum_{s} C_s, \qquad T_w = \min_{s} T_s, \qquad D_w = \min_{s} D_s$$

and its response R<sub>w</sub> comes from the same response-time analysis as the models<sup>[[5]](#ref-5)</sup>, with every measured job above it on that core. If R<sub>w</sub> ≤ D<sub>w</sub>, every service is released at most once inside any window of length R<sub>w</sub>, so the sum of the budgets bounds all the work the worker can face, waiting for another service included. On 13757fd that gave R 2.00 ms on core 1, a margin of 998 ms. Admission runs when the boot health verdict is formed, about 65 s after a start, once the scheduler monitor has measured the other jobs, on the core with the lower measured load first. A service whose self-test takes longer than its budget is refused on its own. If both cores refuse the worker, the board runs without AI services and the Board page says why. In safe mode they stay off.

Overruns do not pile up. A step that runs past its budget may finish, and that service's next release is skipped. A worker that comes back late runs only the newest release and counts the older ones as skipped. No job ever waits in a queue behind another, so overload cannot build a backlog. `make -C tests/host ai` simulates the release table under overload (a step 3.5 periods long, budget overruns, a blocked worker) and checks that no service ever has more than one job outstanding, and that the ladder sheds in the order AI services, screen, learner, model, and comes back in reverse.

The worker cannot deadlock either. A deadlock needs a cycle of tasks, each holding something the next one waits for. A service step takes no mutex, waits on no queue, does no I/O and allocates nothing. Its input is a copy of the latest sample in a single-writer seqlock: the reader retries a torn copy at most four times, then keeps the last good one, and never waits. Its result goes out the same way, and the screen reads it without a lock. Rows for storage go into a single-producer, single-consumer ring that drops and counts when full. The board task drains it, since it already owns NVS and the file writes. The only lock on a step's path is the scheduler monitor's spinlock, held for a few field copies and never nested. So the worker never holds one resource while it waits for another, and the cycle cannot form through it.

On the Board page, the AI SERVICES card shows each service's period, deadline, budget, runs, misses, skips and worst response.

</details>

<details>
<summary><b>For experts: the models we tested and did not ship</b> (what each was for, and why it lost)</summary>

| Model | What it tried | Result on the Mac | Why it is not shipped |
|---|---|---|---|
| Residual MLP, phase 2 (10 → 12 tanh → 1, 375 multiply-add equivalents, 984 B) | learn the EKF's remaining SoC error | RMSE 0.49 % middle and 0.21 % late life against the EKF's 0.53 and 0.33 %, but a worst error of 6.49 % on the forward test against 3.96 %, and NASA 12.1 % against 12.9 % | better in some places, worse in others. The gate was to beat the EKF everywhere |
| Residual MLP, phase 3 (11 → 12 tanh → 1, trained with 120 simulated cells of random OCV shape, capacity, resistance and offset, with a confidence gate) | make the correction hold on other cells | the gate switched it off, or it raised the worst error | it did not pass |
| EKF with a third state for the current offset | cancel a current-sensor offset | under +0.01C offset, RMSE 5.3 % down to 0.27 to 0.79 %. On clean data the early-life worst error rose from 4.0 % to 7.5 % | kept for boards whose measured offset is above about 0.003C (0.2 mA on 65 mAh), not the default |
| UKF with one or two RC pairs<sup>[[22]](#ref-22)</sup> | a better nonlinear filter | the same RMSE as the EKF, a higher worst error (5.8 % against 2.5 % in middle life), 360 against 90 multiply-add equivalents | linearisation was not the limit |
| Neural ECM | a network in place of the cell's voltage map | 62 % RMSE on the NASA cells | it learned one cell's voltage map |
| Pure MLP | SoC straight from V, I and T | 32 % on the NASA cells, worst 10.8 % early in life, and no recovery from a wrong start | no physics to fall back on |
| KalmanNet-lite, a small KalmanNet<sup>[[23]](#ref-23)</sup> | a learned Kalman gain | worse than the EKF everywhere, for example 3.51 % RMSE early in life against 0.45 %, after a short training run | it lost on every test |
| SoH: GBDT and an 8-unit MLP | other SoH regressors | 1.96 % and 20 % MAPE in forward cross-validation, against 0.83 % for ridge | ridge was better and smaller |
| Battery anomaly: z-score and online Gaussian as alarms | simple baselines | 0.245 and 0.126 false alarms per hour on normal data, against a target of 0.05 | ageing moved them. They only log |
| IDS: z-score on all six features, diagonal Gaussian, Mahalanobis, online k-means, Half-Space Trees<sup>[[24]](#ref-24)</sup>, a tiny autoencoder | learn normal traffic, block anything unusual | 0.10 to 0.16 false blocks per hour on stable data (the autoencoder 0.009). After a move from a quiet place to a busy one, stuck blocking 33 to 100 % of the time | they learned the crowd's size, and the guard against poisoning froze them in the new place |
| IDS: paired-adapter signal strength as a clone detector | spot a second transmitter on the paired address | 0 / 60 clones found at z = 6 | the public data has no fixed transmitter, so it could not answer. Clones stay with rule R1 |

The two studies came out opposite ways. For the intrusion detector, the general learners failed and a baseline built for one pattern, address rotation, passed. For the battery anomaly detector, the simple baselines failed on ageing drift and the autoencoder passed.

</details>

### What it costs

Measured with `idf.py size` on the S31 build of the release (920318e, the firmware in main), against mossREV 1.2.0 rc3 (8b0b96f):

| | Edge OS 2.0.0 | mossREV 1.2.0 rc3 | Change |
|---|---|---|---|
| App image | 4,778,560 B, 76.0 % of the 6 MB app slot (24 % free) | 4,752,720 B | +25,840 B |
| Static internal RAM (DIRAM) | 194,198 B of 503,744 B (38.6 %) | 192,124 B | +2,074 B: PPA and 2D-DMA +2,380 B (mostly IRAM code), the pairing change +184 B, the OS parts and fixes +100 B, the feature gate −590 B |
| Static PSRAM | 291,200 B | 289,664 B | +1,536 B, mostly the admission's buffers (1,352 B) |
| Tasks | the same tasks as 1.2.0 | | none added: the OS layer runs inside the existing tasks |

At 09ec8f5 the graphics work had added 37,312 B to the app image and the OS parts together about 9 KB. The 16 px text floor later saved 24,304 B by dropping the 12 and 14 px fonts, so the release is 25,840 B larger overall. With the UPS option switched on, the deep-sleep path alone would add about 12 KB of IRAM, which is one reason that option stays off.

The target for internal RAM at run time is more than 60 KB free in every phase. This was measured on the board, EdgeOS 2.0.0 4aeff43, with a debug build of that code that adds a RAM monitor. Each figure is the lowest free internal RAM in that phase, all internal heaps added up:

| Phase | Lowest free internal RAM |
|---|---|
| The first minute after a start, before Wi-Fi | 127,407 B |
| The first Wi-Fi round, with the automatic adapter search | 82,907 B |
| An adapter search alone | 112,935 B |
| An adapter search and Sync clock now together | 82,883 B |
| Sync clock now alone | 82,907 B |
| Wi-Fi search | 88,791 B |
| A theme switch | 112,799 B |
| A switch to E.V | 102,383 B |
| A launcher slide | 112,599 B |

The lowest is 82,883 B, about 81 KB, with an adapter search and a clock sync running at once. Every phase stays above 60 KB. Two parts of that total were never used in any phase: the 32 KB internal reserve (32,031 B free) and the LP RAM (31,948 B free). The main heap alone went down to 18,900 B.

Before 4aeff43, the Board page's "lowest" read 32 KB too low. A photo showed 49 KB. ESP-IDF sets the minimum of a heap added after start-up to 0 (`heap_caps_init.c`, lines 325 to 332, in ESP-IDF 6.1). The 32 KB internal reserve is added that way, so the summed minimum always counted that pool as empty. 4aeff43 restarts the minimum count early in the boot. RAM use stays the same. Only the figure changes.

### What you see on the board

- The firmware reads "OpenCarMind EdgeOS 2.0.0" on the boot line, in Settings and on the Board page. The desktop app sees `fw=EdgeOS-2.0.0`.
- The Board page's header shows the runlevel and the health at the top right, for example "Parked, health good" in green. Degraded is amber and bad is red. The events list gets "Boot health: good" or the reasons, such as "Boot health: degraded: NAND not mounted".
- Wi-Fi stays off for about the first minute after a start. Then the automatic clock sync runs, and on a board whose clock is not set the first sync follows about 20 s later.
- A refused change says why. During a drive: "A drive is active. End the drive first, then switch models." For other actions during a drive: "Not now: a drive is running. Try again later." When the car's computer answers but no drive is recorded yet: "Not now: the car link is up. Try again later." The USB door answers "ERR busy: a drive is running, try again when parked".
- The ladder and the battery write events such as "Model deadlines: UI slowed", with the miss count, or "Battery hot (45.0 C on USB): load shed".
- The Board page's battery card shows the current, "on USB" or "on battery", the gauge temperature, and "uncalibrated" with the cycle count until the gauge is set up for the 65 mAh cell.

The [user manual](docs/manual/edgeos-2.0.0.md#os) lists every message with what to do about it.

### Changes after the board check

Four changes were merged after the board check, at 06ddf1d. Each is built and host-checked, not yet seen on the board.

- **Pairing asks you first, and only at a strong signal.** On the bench, the board kept trying a neighbour's adapter at -88 to -90 dBm: 35 connect attempts in about 16 minutes, all failed. With no saved adapter, the old rule took any device that looked like an OBD adapter at -90 dBm or better and saved the first one that answered. Now a new adapter is offered only at -70 dBm or better (`CONFIG_OBD_BLE_PAIR_MIN_RSSI`), and nothing is saved until you say yes. The Mosaico shows "OBD adapter found: <name>, signal strong. Use this adapter?" with "Not this one" and "Use this adapter", and counts down 30 s. The automatic search never replaces a saved adapter. The paired adapter still connects down to -90 dBm, so it works at the edge of range. All of this sits behind one Kconfig switch, `CONFIG_OBD_BLE_PAIR_CONFIRM`. It is on by default for the Mosaico and off for the ESP32-S3, which keeps its tested rule until the owner tests the new prompt on an S3. With the switch off, the pairing code is compiled out, and the S3 image equals the 4aeff43 S3 image except the build time, the ELF SHA-256 and the image's checksum and hash (host-checked on the Mac by a section-by-section compare, 68 bytes of the .bin differ, all in those fields). For the S3 the prompt is planned for a later release: "Use <name>?" and "Strong. Tap = use it", with a BOOT tap within 5 s as the yes. To try it, add `CONFIG_OBD_BLE_PAIR_CONFIRM=y` to `sdkconfig.defaults.esp32s3`. `make -C tests/host pair` checks the rule with the switch off and on.
- **Driving starts only when the car's computer answers.** The same neighbour's adapter kept putting the board into Driving for short spells, a median of about 4 s and at most 14 s, because connect attempts counted as Driving. Now the board is Driving only once the car's computer answers and rows are logged. Searching, connecting and an adapter in a parked car with the engine off keep it Parked.
- **Model names on the screen.** The model's file shows as "ev-frozen" for E.V and "active-estimator" for R.E.V, where the screen showed a 12-character hash before. The hash stays in the logs, the session files and the USB door.
- **No text smaller than 16 px.** The screen used 12 px and 14 px text in places. NHTSA's driver-interface guidance (DOT HS 812 360)<sup>[[14]](#ref-14)</sup>, following ISO 15008, asks for capital letters of at least 16 arcminutes for urgent text and 12 arcminutes for the rest, with 20 arcminutes as the best size. On this 2.16 inch, 480 × 480 screen, 16 px text has capitals about 1 mm tall. At about 65 cm from your eyes, that is about 5 arcminutes. So 16 px is the smallest size the screen uses, and even that is small for a driver. That is why the Drive screen and the alarm banner keep a few big items. The alarm banner's headline is now 32 px. The Board page now scrolls.

### Pairing safety (built, not yet checked on the board)

The saved adapter used to be known by its Bluetooth address alone. A device that copies that address could connect with no question, and the Bluetooth stack paired it again by itself when it claimed to have lost its key. Three rules close that on the Mosaico. The S3 build does not change.

- **Adapter fingerprint.** On the first link after you say yes to an adapter, the board saves a fingerprint in NVS. It is one 32-bit hash each for the address type, the advertised name, service UUIDs and maker id, the serial line it uses, and the ATI and AT@1 replies. The car's protocol is kept too, as a soft field. The VIN is not part of it. No raw reply is stored or logged, only an 8-digit hash id. On every later link the board compares. A match changes nothing. A difference keeps the drive logged, marks it `"adapter_check": {"result": "changed"}` in its session file, and gives the R.E.V learner and the pattern model none of its rows. When parked, the board asks "This adapter looks different. Use it?" Use it saves the new fingerprint. Not this one drops the link and leaves the saved adapter alone for 10 minutes. While driving the link is never cut, and the question waits until you park. Fields that change on their own are left out or compared only when both sides have them: the signal, the advert flags, the bytes after the maker id, which packet kind carried a field (advert or scan response), case and spaces in the replies, and a missing AT@1 reply. A different protocol alone (the adapter moved to another car) is noted, not counted as a change.
- **No silent re-pair.** In ESP-IDF 6.1 the NimBLE stack starts a new pairing by itself when the adapter refuses the saved key (`BLE_RESTART_PAIR` is fixed at 1 in `esp_nimble_cfg.h`), and the `BLE_GAP_EVENT_REPEAT_PAIRING` event the old code handled never reaches a central. So the board copies the saved key when the link comes up and compares it after encryption. A new key is a re-pair. When parked, the board holds the link with no data and asks "The adapter asks to pair again. Allow?" Allow keeps the new key. Don't allow, or no answer in 30 s, puts the old key back, drops the link and waits 10 minutes. While driving, the board puts the old key back, keeps the link, marks the rows and logs "Re-pair refused (driving)". The event handler follows the same table. The session file counts the events.
- **Clock jump guard.** When the clock came from Wi-Fi, a new network time more than 24 hours ahead or more than 10 minutes behind is refused, and the events say "Clock sync refused: N min ahead" (or behind). A clock set by hand, an estimated clock and no clock at all are not guarded. The 2026 to 2099 range check stays.

`make -C tests/host guard` checks the fingerprint, both decision tables and the question, and audits the glue in the sources. `make -C tests/host clock` checks the jump guard. Pairing with a real LELink has not been checked on this board yet.

### Flash it, or go back to mossREV 1.2.0

Flashing needs no buttons. Open the board's USB port at 1200 baud, then write the merged image with esptool 5.6.0 or later. [docs/MOSAICO.md](docs/MOSAICO.md#flashing) has the commands. The merged image erases the NVS partition, so the pairing with the desktop app, the saved Wi-Fi networks and the settings go, and you pair again. The drive logs on the NAND and the flash FAT's state files are not written.

The board-checked image is `EdgeOS-2.0.0-esp32s31-merged.bin`, built from 4aeff43, with a SHA-256 that starts `a225125d27f3906c`. It is not published yet. The merged branch tip (06ddf1d) has no released image yet. The release image will be rebuilt from the branch tip, and it gets a new SHA-256.

To go back, flash the mossREV 1.2.0 merged image the same way. Going back is not yet verified on the board.

### Prepared but off: A/B rollback and the battery as a UPS

Two parts are built into 2.0.0 and switched off by default.

- A/B rollback (`CONFIG_OCM_APP_ROLLBACK`). The flash has two 6 MB app slots. With ESP-IDF's app rollback, a new image gets one boot to confirm itself. A good or degraded health verdict would confirm it, and a bad one would reset into the other slot through `sys_reset`. It stays off because today's flashing writes slot 0 with esptool and leaves otadata erased, so no image is ever in the state that arms the rollback. Turning it on needs a change to the flashing flow first, and that is the owner's decision.
- The battery as a UPS (`CONFIG_OCM_UPS`). Two gauge readings in a row at or below −20 mA would mean USB is gone: flush the logs, dim the screen, refuse changes, and wait 15 s, because cranking the engine drops the car's USB for a few seconds. Then quiesce the storage and pull GPIO57 low, the board's power-switch request. At 3.4 V on battery it would act at once. It stays off until the board's real draw and the gauge's current sign are measured. The power-off through GPIO57 for the battery protection (`CONFIG_OCM_BATSAFE_POWER_OFF`) waits for the same check.

### What is verified on the board

Two board passes ran. The first on EdgeOS 2.0.0 4aeff43 (SHA-256 `a225125d27f3906c...`), the second on 06ddf1d, which adds the pairing change, the 16 px text and the model names (merged image SHA-256 `4cc402a76a53edc2...`). The release image in this branch is rebuilt from its tip, whose Mosaico code equals 06ddf1d apart from the Home card fix in ce58ca7. VERIFIED means a board log line shows it. Some rows ran on a scratch build of the 2.0.0 code with one test switch on, and the row says so. A scratch build is never shipped. Host tests pass for every part: `make os` (41,635 checks at 4aeff43, 41,719 with the pairing change), `make pair`, `make ppa`, `make uimetrics` and the source audits in `tests/host/os_audit.sh`.

Of the 28 rows from those two passes, 21 are VERIFIED (6 of those on a scratch build), 3 PARTLY and 4 NOT: not run yet, or switched off in this release. The three figures below show the main results. A third pass is running on build 13757fd, which adds the AI services and pairing safety. Its 8 rows are at the end of the table: 4 VERIFIED so far, 3 being checked, and 1 for the intrusion detector, which is not in the firmware yet. The full table is folded under the figures.

<p align="center"><img src="docs/images/edgeos-boot.png" width="900" alt="Boot and runlevels on the board. The release boot on 4aeff43, in seconds after power-on: running at 4.3 s with Wi-Fi held, boot health good at 68.0 s with Wi-Fi released and 128.9 KB of internal RAM free, the Wi-Fi driver up at 88.1 s, the clock synced at 98.0 s, the driver down at 106.5 s. If health is bad, on a scratch build forced bad: safe mode, Wi-Fi held, and Sync clock now still starts it by hand. The runlevels seen in the tests: Parked. Driving on a forced drive. Saving when the battery was hot at 34.2 °C, back to Parked when it cooled to 33.7 °C. Safe mode on a forced bad health check. Refused while Driving: the USB door answered ERR busy, and the screen said Not now: a drive is running for the clock sync and Format."></p>

The release boot holds Wi-Fi until the health check says the board is good, then joins, syncs the clock and stops the radio again. The runlevels in the lower half come from forced tests on scratch builds.

<p align="center"><img src="docs/images/edgeos-ladder-battery.png" width="900" alt="Degradation ladder and battery protection, both forced tests on scratch builds. Ladder with extra model load, in seconds after power-on: UI slowed at 110.6 s after 2 misses, learner paused at 122.6 s after 6, model paused at 134.6 s after 10, then one rung back at a time: model back at 214.6 s, learner back at 278.6 s, normal at 348.6 s. Battery protection at test limits: at boot on USB, 34.2 °C sheds load into Saving with Wi-Fi off, the learner paused and the screen dimmed, and 33.7 °C returns to Parked. 4,188 mV from Parked stops the board as a charger fault with the logs flushed and the storage stopped. With the limit lowered to 26 °C, 28 °C stops it as critical, with no power-off. The real limits of 45 °C, 60 °C and 4.25 V were not reached."></p>

The ladder drops the screen rate first and the model last, and climbs back one rung per minute without misses. The battery figures are test limits. The real 45 °C, 60 °C and 4.25 V were not reached.

<p align="center"><img src="docs/images/edgeos-ram.png" width="900" alt="Internal RAM per phase, the lowest free internal RAM in each of 9 phases on a debug build of 4aeff43, in KB: first minute before Wi-Fi 124.4, first Wi-Fi round with the adapter search 81.0, adapter search alone 110.3, adapter search and Sync clock now together 80.9, Sync clock now alone 81.0, Wi-Fi search 86.7, theme switch 110.2, switch to E.V 100.0, launcher slide 110.0, all above the 60 KB target. Below it, the 28 rows of the board table: 21 verified, 3 partly, 4 not run or off in this release."></p>

Every phase stays above the 60 KB target. The lowest is 80.9 KB, with an adapter search and a clock sync running at once.

<details><summary><b>Full board results table</b> (all 36 rows, with the build each one ran on)</summary>

| Part | Result | What the board showed | Build |
|---|---|---|---|
| Boot and name | VERIFIED | `OpenCarMind EdgeOS 2.0.0 running` at 4.3 s after a power-on, 0 USB drops in 185 s | 4aeff43 |
| Boot health, good | VERIFIED | `boot health good \| model self-test 1, flash FAT 1 (0 repaired), NAND 1, first UI tick 1365 ms, touch ok, 0 misses, 0 stalls, schedulable, runlevel Parked` at 68.0 s | 4aeff43 |
| Boot health, bad | VERIFIED | `boot health bad: safe mode`, then `Wi-Fi held: boot health bad`. Sync clock now still started Wi-Fi by hand | scratch, forced bad |
| Wi-Fi boot hold | VERIFIED | held at 4.3 s with no driver, released at 68.0 s with 131,975 B of internal RAM free, driver up at 88.1 s and down again at 106.5 s | 4aeff43 |
| No addresses in the log | VERIFIED | no access-point address and no board address in the boot and Wi-Fi log | 4aeff43 |
| Model admission, admitted | VERIFIED | `C 4248 us (self-test 2124 us x 2.0) on core 1, R: ml 4.2/1000 ms, margin 995.8 ms (ml): admitted` | 4aeff43 |
| Model admission, refused | VERIFIED | the boot load logged `REFUSED`, and a switch to E.V got `ERR CarMind E.V refused: ml would miss its deadline` while R.E.V stayed | scratch, safety factor raised from 2 to 2,000 |
| Service contract | VERIFIED | `service R.E.V learner: job rev, T 1000 ms, D 30000 ms, stall 60000 ms, CPU 20 per mille in 30 ms bursts, prio 2 core 1` | 4aeff43 |
| Runlevels and refusals during a drive | VERIFIED | `runlevel Driving (active 0x08)` and back to `runlevel Parked`. The USB door answered `ERR busy: a drive is running, try again when parked`. The screen showed "Not now: a drive is running. Try again later." for the clock sync and Format | scratch, forced drive |
| Degradation ladder | VERIFIED | events "UI slowed (2 misses)" at 110.6 s, "learner paused (6 misses)" at 122.6 s, "model paused (10 misses)" at 134.6 s, then one step back at a time to "normal" at 348.6 s. One model load in the whole run | scratch, extra model load |
| Battery protection | VERIFIED, test limits | "Battery hot (34.2 C on USB): load shed" with Wi-Fi off, the learner paused and the screen dimmed, runlevel Saving, then "Battery cooled (33.7 C)" and Parked, and "Battery over-voltage (4188 mV): charger fault, stopped". The real limits (45 °C, 60 °C, 4.25 V) were not reached | scratch, test limits |
| Desktop app | VERIFIED | connect with `fw=EdgeOS-2.0.0`, a wrong token refused, a model switch both ways, two files read at their listed sizes | 4aeff43 |
| Internal RAM above 60 KB | VERIFIED | lowest 82,883 B over 9 phases (see [What it costs](#what-it-costs)) | debug build of 4aeff43 |
| PPA and 2D-DMA | PARTLY | on at boot, `failures dma 0 ppa 0` in all 18 reports. A clean A/B on the same build: composing one slide frame takes 5.5 ms with the accelerator and 8.4 to 9.0 ms without, but a whole slide runs at 8.4 to 9.0 fps with it and 11.7 to 12.5 fps without, and its slowest frame is about 45 ms longer. Under investigation | 06ddf1d |
| Resets through `sys_reset` | VERIFIED | `the last run ended with sys_reset(REBOOT: door REBOOT)` after a door REBOOT, and `the last run ended with sys_reset(door: USB door)` after a download-door entry | 4aeff43, 06ddf1d |
| Feature gate | PARTLY | the gate starts and Smart swipe's warm-up runs. No verdict yet | 4aeff43 |
| Version on screen | VERIFIED | `OpenCarMind EdgeOS 2.0.0` in the boot line, on the Board page and in Settings, and `fw=EdgeOS-2.0.0` over USB | 4aeff43, 06ddf1d |
| 10-minute soak | VERIFIED | 10 min 02 s on the release: 0 restarts, 0 USB drops, 0 UI misses, every job within its deadline. The Wi-Fi round ran on the next boot: clock synced, internal RAM 90,959 B free at its low | 06ddf1d |
| Critical battery stop | VERIFIED, test limit | with the limit lowered to 26 °C: `Battery too hot (28 C): stopped` and `logs flushed, storage stopped`, no power-off. The real 60 °C was not reached | scratch, lowered limit |
| Where the gauge temperature comes from | VERIFIED | the gauge chip's own sensor: `Temperature()` equals `InternalTemperature()` in all 25 readings (28.1 to 31.4 °C). The board has no thermistor on the cell, so this is the temperature next to the cell, not of the cell | debug builds |
| Board page "lowest" after the fix | PARTLY | the Board page reads 108 KB, the all-internal figure (110,631 B) and no longer 32 KB low. Not shown after a Wi-Fi round | debug build of 06ddf1d |
| Wi-Fi with no known network in reach | NOT | not run | |
| Going back to mossREV 1.2.0 | NOT | not tried | |
| A/B rollback, UPS, GPIO57 power-off | NOT | off in this release | |
| Pairing: a weak adapter is ignored | VERIFIED | over the soak and the next boot: 0 connection tries, 0 pairing questions and 0 Driving spells, `automatic adapter search: not paired, strong signal only` | 06ddf1d |
| Pairing: offering and saving your adapter | NOT | needs the owner's adapter next to the board | |
| Model names and 16 px text on screen | VERIFIED | 37 board captures: "active-estimator" and "ev-frozen" on the model pages, no text under 16 px, nothing in a 60 px corner. One overlap on Home (fixed in ce58ca7, host-checked) | 06ddf1d |
| The download door in every state | VERIFIED | the 1200-baud door entered the ROM download mode on the release, in Saving, during a forced drive and after a battery safety stop | 4aeff43, 06ddf1d, scratch |
| Boot health, with the AI services | VERIFIED | rated good at 68.9 s | 13757fd |
| Wi-Fi boot hold, with the AI services | VERIFIED | Wi-Fi released after the good verdict with 130,623 B of internal RAM free | 13757fd |
| AI worker admission | VERIFIED | admitted on core 1 with R 2.00 ms against its 1,000 ms deadline, a margin of 998 ms | 13757fd |
| Battery estimator self-test | VERIFIED | 570 µs against its 2 ms budget | 13757fd |
| Battery SoC and SoH on the Board page | BEING CHECKED | built, being checked on the board. The parameters are placeholders until the cell's curve is fitted | 13757fd |
| Ladder rung "AI services slowed" | BEING CHECKED | built, being checked on the board | 13757fd |
| Pairing safety: fingerprint, no silent re-pair, clock jump guard | BEING CHECKED | built, being checked on the board | 13757fd |
| Network intrusion detector | NOT | not in the firmware yet. Validated on the Mac (see [AI services in EdgeOS](#ai-services-in-edgeos)) | |

</details>

## Limitations and future work

- **Torque is a physics estimate, not a measurement**, checked against one public car and for petrol engines only. R.E.V learns the engine size it uses, about 2.1 L from my car's last two drives, but not the torque itself. Cars that report torque (PIDs 0x62 and 0x63) could give a learned torque read-out its label, the way the fuel read-out learns now, and then the physics would not be needed. That is planned, not built or tested. My Sentra and most pre-2020 cars report no torque, so there is no label, and physics, or the ESP-Mosaico's accelerometer as a label, stays needed there.
- **The classifier and brake pressure do not learn in the car yet.** Accelerometer labels on the ESP-Mosaico and a self-check that forecasts the next moment are the routes being worked on.
- **No Wi-Fi upload yet.** It is being reworked so a lost packet on weak home Wi-Fi does not stall every file, as in QUIC<sup>[[13]](#ref-13)</sup>. Until then drives come over the USB cable.
- **The clock is only as exact as one second**, and it needs a known Wi-Fi network within reach.

## Planned

- A build without a display.
- Code-signed installers.
- A test drive of the corrected R.E.V, and learning each car's closed-throttle position on the board.
- A manoeuvre forecast as a self-check, so the classifier can learn in the car.
- More regularised models, to fit more cars.

## References

<a id="ref-1"></a>[1] J. Lin, W.-M. Chen, Y. Lin, J. Cohn, C. Gan and S. Han, "MCUNet: Tiny deep learning on IoT devices," in *NeurIPS*, 2020. [arXiv:2007.10319](https://arxiv.org/abs/2007.10319)

<a id="ref-2"></a>[2] R. David *et al.*, "TensorFlow Lite Micro: Embedded machine learning on TinyML systems," in *Proc. MLSys*, 2021. [arXiv:2010.08678](https://arxiv.org/abs/2010.08678)

<a id="ref-3"></a>[3] C. Banbury *et al.*, "MLPerf Tiny benchmark," in *Proc. NeurIPS Datasets and Benchmarks Track*, 2021. [arXiv:2106.07597](https://arxiv.org/abs/2106.07597)

<a id="ref-4"></a>[4] C. L. Liu and J. W. Layland, "Scheduling algorithms for multiprogramming in a hard-real-time environment," *Journal of the ACM*, vol. 20, no. 1, pp. 46–61, 1973. [doi:10.1145/321738.321743](https://doi.org/10.1145/321738.321743)

<a id="ref-5"></a>[5] M. Joseph and P. Pandya, "Finding response times in a real-time system," *The Computer Journal*, vol. 29, no. 5, pp. 390–395, 1986. [doi:10.1093/comjnl/29.5.390](https://doi.org/10.1093/comjnl/29.5.390)

<a id="ref-6"></a>[6] Espressif Systems, "ESP-DL: Espressif deep-learning library for AIoT applications." [github.com/espressif/esp-dl](https://github.com/espressif/esp-dl)

<a id="ref-7"></a>[7] B. I. Kwak, J. Woo and H. K. Kim, "Know your master: Driver profiling-based anti-theft method," in *Proc. PST*, 2016, pp. 211–218. [arXiv:1704.05223](https://arxiv.org/abs/1704.05223)

<a id="ref-8"></a>[8] G. Oh, D. J. LeBlanc and H. Peng, "Vehicle Energy Dataset (VED), a large-scale dataset for vehicle energy consumption research," *IEEE Transactions on Intelligent Transportation Systems*, 2020. [arXiv:1905.02081](https://arxiv.org/abs/1905.02081), data and licence (Apache-2.0) at [github.com/gsoh/VED](https://github.com/gsoh/VED)

<a id="ref-9"></a>[9] J. Kirkpatrick *et al.*, "Overcoming catastrophic forgetting in neural networks," *PNAS*, vol. 114, no. 13, pp. 3521–3526, 2017. [arXiv:1612.00796](https://arxiv.org/abs/1612.00796)

<a id="ref-10"></a>[10] B. Kovačević, Z. Banjac and I. Kostić Kovačević, "Robust adaptive filtering using recursive weighted least squares with combined scale and variable forgetting factors," *EURASIP Journal on Advances in Signal Processing*, 2016. [doi:10.1186/s13634-016-0341-3](https://doi.org/10.1186/s13634-016-0341-3)

<a id="ref-11"></a>[11] R. R. Selvaraju, M. Cogswell, A. Das, R. Vedantam, D. Parikh and D. Batra, "Grad-CAM: Visual explanations from deep networks via gradient-based localization," in *Proc. ICCV*, 2017. [arXiv:1610.02391](https://arxiv.org/abs/1610.02391)

<a id="ref-12"></a>[12] Espressif Systems, "ESP-Mosaico," esp-dev-kits documentation. [docs.espressif.com](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/index.html)

<a id="ref-13"></a>[13] J. Iyengar and M. Thomson, Eds., "QUIC: A UDP-based multiplexed and secure transport," RFC 9000, IETF, 2021. [rfc-editor.org](https://www.rfc-editor.org/rfc/rfc9000)

<a id="ref-14"></a>[14] J. L. Campbell *et al.*, "Human factors design guidance for driver-vehicle interfaces," Report DOT HS 812 360, National Highway Traffic Safety Administration, 2016. [nhtsa.gov](https://www.nhtsa.gov/sites/nhtsa.gov/files/documents/812360_humanfactorsdesignguidance.pdf)

<a id="ref-15"></a>[15] J. Zhang, Z. Qu, C. Chen, H. Wang, Y. Zhan, B. Ye and S. Guo, "Edge learning: The enabling technology for distributed big data analytics in the edge," *ACM Computing Surveys*, vol. 54, no. 7, art. 151, 2021. [doi:10.1145/3464419](https://doi.org/10.1145/3464419)

<a id="ref-16"></a>[16] T. Scheffer, C. Decomain and S. Wrobel, "Active hidden Markov models for information extraction," in *Proc. Intelligent Data Analysis (IDA)*, LNCS 2189, 2001. [doi:10.1007/3-540-44816-0_31](https://doi.org/10.1007/3-540-44816-0_31)

<a id="ref-17"></a>[17] H. Robbins and S. Monro, "A stochastic approximation method," *The Annals of Mathematical Statistics*, vol. 22, no. 3, pp. 400–407, 1951. [doi:10.1214/aoms/1177729586](https://doi.org/10.1214/aoms/1177729586)

<a id="ref-18"></a>[18] I. Žliobaitė, A. Bifet, B. Pfahringer and G. Holmes, "Active learning with drifting streaming data," *IEEE Transactions on Neural Networks and Learning Systems*, 2014. [doi:10.1109/TNNLS.2012.2236570](https://doi.org/10.1109/TNNLS.2012.2236570)

<a id="ref-19"></a>[19] D. Yoo and I. S. Kweon, "Learning loss for active learning," in *Proc. CVPR*, 2019. [arXiv:1905.03677](https://arxiv.org/abs/1905.03677)

<a id="ref-20"></a>[20] S. Dasgupta, "Two faces of active learning," *Theoretical Computer Science*, 2011. [cseweb.ucsd.edu](https://cseweb.ucsd.edu/~dasgupta/papers/twoface.pdf)

<a id="ref-21"></a>[21] G. L. Plett, "Extended Kalman filtering for battery management systems of LiPB-based HEV battery packs: Part 3. State and parameter estimation," *Journal of Power Sources*, vol. 134, no. 2, pp. 277–292, 2004. [doi:10.1016/j.jpowsour.2004.02.033](https://doi.org/10.1016/j.jpowsour.2004.02.033)

<a id="ref-22"></a>[22] S. J. Julier and J. K. Uhlmann, "Unscented filtering and nonlinear estimation," *Proceedings of the IEEE*, vol. 92, no. 3, pp. 401–422, 2004. [doi:10.1109/JPROC.2003.823141](https://doi.org/10.1109/JPROC.2003.823141)

<a id="ref-23"></a>[23] G. Revach, N. Shlezinger, X. Ni, A. L. Escoriza, R. J. G. van Sloun and Y. C. Eldar, "KalmanNet: Neural network aided Kalman filtering for partially known dynamics," *IEEE Transactions on Signal Processing*, vol. 70, pp. 1532–1547, 2022. [arXiv:2107.10043](https://arxiv.org/abs/2107.10043)

<a id="ref-24"></a>[24] S. C. Tan, K. M. Ting and T. F. Liu, "Fast anomaly detection for streaming data," in *Proc. IJCAI*, 2011, pp. 1511–1516. [ijcai.org](https://www.ijcai.org/proceedings/2011/0254.pdf)

<a id="ref-25"></a>[25] Q. Ma, S. Muthukrishnan and M. Sandler, "Frugal streaming for estimating quantiles: One (or two) memory suffices," 2014. [arXiv:1407.1121](https://arxiv.org/abs/1407.1121)

<a id="ref-26"></a>[26] B. Saha and K. Goebel, "Battery data set," NASA Prognostics Data Repository, NASA Ames Research Center, Moffett Field, CA, 2007. [nasa.gov](https://www.nasa.gov/intelligent-systems-division/discovery-and-systems-health/pcoe/pcoe-data-set-repository/)

<a id="ref-27"></a>[27] T. Bravenec, J. Torres-Sospedra, M. Gould and T. Fryza, Supplementary materials for "UJI Probes: Dataset of Wi-Fi probe requests," Zenodo, 2023. Licence CC BY 4.0. [doi:10.5281/zenodo.7801798](https://doi.org/10.5281/zenodo.7801798)

<a id="ref-28"></a>[28] T. Bravenec, J. Torres-Sospedra, M. Gould and T. Fryza, Supplementary materials for "What your wearable devices revealed about you and possibilities of non-cooperative 802.11 presence detection during your last IPIN visit," Zenodo, 2022. Licence CC BY 4.0. [doi:10.5281/zenodo.6798302](https://doi.org/10.5281/zenodo.6798302)

The UJI Probes and IPIN2021 probe-request captures are © T. Bravenec, J. Torres-Sospedra, M. Gould and T. Fryza, used under CC BY 4.0. Only windowed counts derived from them were used, on the Mac, and nothing from them is in this repository.

## Repository layout

| Path | What it holds |
|---|---|
| `main/` | firmware source |
| `ml/onboard/` | the shipped models (CarMind E.V, and CarMind R.E.V in `rev/`) |
| `samples/replay/` | two replay sessions from a public dataset, in the board's file format |
| `schema/` | JSON schema of the session files |
| `tests/host/` | unit tests that run on a computer, including R.E.V's learners |
| `tools/` | `ocm_report.py` and the model header generator |
| `desktop/` | the desktop app (Rust and Tauri) |
| `dashboard/` | Docker Compose file and Dashy configuration |
| `docs/` | configuration, model notes, field checks, screen designs and figures |
| `docs/manual/` | the user manuals, their sources and the script that builds them |

## Credits and licence

OpenCarMind is released under the MIT licence (see `LICENSE`). ESP-IDF, ESP-DL, LVGL and NimBLE keep their own licences. The training data of the shipped models is not part of this repository. The replay samples also use the carOBD dataset, <https://github.com/eron93br/carOBD>. See `samples/replay/README.md` for its terms.
