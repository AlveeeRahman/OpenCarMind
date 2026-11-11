<p align="center"><img src="docs/images/banner.png" width="900" alt="OpenCarMind: an open-source OBD2 logger for the ESP32, with TinyML on the board"></p>

<p align="center">
<a href="https://github.com/AlveeeRahman/OpenCarMind/actions/workflows/ci.yml"><img src="https://github.com/AlveeeRahman/OpenCarMind/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI"></a>
<a href="https://scorecard.dev/viewer/?uri=github.com/AlveeeRahman/OpenCarMind"><img src="https://api.scorecard.dev/projects/github.com/AlveeeRahman/OpenCarMind/badge" alt="OpenSSF Scorecard"></a>
<a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License: MIT"></a>
<img src="https://img.shields.io/badge/ESP--IDF-v6.1-red.svg" alt="ESP-IDF v6.1">
<img src="https://img.shields.io/badge/chip-ESP32--S3-lightgrey.svg" alt="ESP32-S3">
</p>

OpenCarMind is an open-source OBD2 logger and dashboard for the ESP32, with TinyML on the board. It turns an ESP32-S3 board and a Bluetooth OBD-II adapter into a car data logger that runs a neural network on the chip. It needs no phone, no account and no internet connection. Everything it records goes to an SD card in the car and, if you pair the board, to the desktop app on your own computer.

It ships two on-board models, and you choose between them in the desktop app. CarMind E.V is fixed and tested. CarMind R.E.V is my first model that keeps learning in your car: the board itself updates part of the model from what your car reports while you drive, with no computer or cloud in the loop.

Version 2.5.1. Licence: MIT. Website: <https://alveeerahman.github.io/OpenCarMind/>. Downloads for the firmware and the desktop app are on the [Releases page](https://github.com/AlveeeRahman/OpenCarMind/releases/latest).

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
<li><a href="#the-models">The models</a>
<ol type="A">
<li><a href="#the-test-drives">The test drives</a></li>
<li><a href="#carmind-rev-results-so-far">CarMind R.E.V results so far</a></li>
<li><a href="#how-the-models-are-made">How the models are made</a></li>
<li><a href="#what-rev-learns">What R.E.V learns</a></li>
<li><a href="#how-rev-learns-on-the-board">How R.E.V learns on the board</a></li>
<li><a href="#looking-inside-rev-with-explainable-ai">Looking inside R.E.V with explainable AI</a></li>
<li><a href="#how-rev-got-better-step-by-step">How R.E.V got better, step by step</a></li>
</ol>
</li>
<li><a href="#status">Status</a></li>
<li><a href="#next-board-esp-mosaico-with-the-esp32-s31">Next board: ESP-Mosaico with the ESP32-S31</a>
<ol type="A">
<li><a href="#how-the-s3-and-the-s31-are-compatible">How the S3 and the S31 are compatible</a></li>
<li><a href="#continual-learning-on-the-s31">Continual learning on the S31</a></li>
<li><a href="#a-release-for-the-mosaico">A release for the Mosaico</a></li>
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
| Status | released in 2.4.0, the default | new in 2.5.0, improved in 2.5.1 |
| Size on the chip | about 89 KB | about 66 KB |
| Parameters | 38,679 | 19,815 |
| Time per run on the ESP32-S3 | about 3.5 ms in the car, 2.9 ms on the bench | about 2.9 ms in the car (median), 2.2 ms on the bench |
| Learns in your car | the model is fixed. The tank size and fill-up calibration learn with either model | its fuel estimate and an airflow constant that also gives the engine size, so its torque estimate learns your engine too. Plus the tank size and fill-up calibration, like E.V |
| Steady cruising with your foot on the gas | can be shown as braking | corrected in 2.5.1 |
| Fill-up calibration | yes | yes |
| Best for | a fixed, tested model | older cars, roughly pre-2020, that have no fuel-rate PID and can lose the airflow (MAF) reading |

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

Wi-Fi is optional and does one thing: while the car is parked on a known network, the board asks one HTTPS time server for the time. It checks the server's certificate, sends nothing else, and listens on no port. You can compile Wi-Fi out with `OCM_WIFI_CLOCK_SYNC`.

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

The first build downloads ESP-DL, LVGL, led_strip and cJSON through the IDF component manager. If you built an earlier version in the same folder, delete `sdkconfig` first. If the board stays in download mode after flashing, reset it with `python -m esptool --chip esp32s3 -p <port> --after watchdog-reset chip-id`.

On first boot the board connects to the strongest nearby adapter that looks like an OBD adapter and remembers it. If yours is not found, add its name to `OBD_BLE_NAME_PREFIXES` in `idf.py menuconfig` under OBD Logger Configuration. Settings, SD card files, the Wi-Fi list and the CSV columns are in [docs/CONFIGURATION.md](docs/CONFIGURATION.md).

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

## Status

Checked on the bench or in code: the build, the host unit tests (including R.E.V's learners with faulty readings), the switch to miles and mpg, the 10-second save, "Safe to turn off" and the time server rules. CI builds the desktop app on Windows, macOS and Linux. I have driven the board with the drive-end, power-off, fuel economy and units features, and R.E.V on two drives in my car.

Not checked yet: R.E.V over many drives and other cars, the corrected R.E.V on the road, the HTTPS clock on the board, other boards, and any adapter other than the LELink. The checks that need a car are in [docs/FIELD_CHECKS.md](docs/FIELD_CHECKS.md), with tables for you to fill in. Known security limits are in [SECURITY.md](SECURITY.md#known-limits).

## Next board: ESP-Mosaico with the ESP32-S31

<p align="center"><img src="docs/images/mosaico-rev.png" width="900" alt="Concept: OpenCarMind with CarMind R.E.V on ESP-Mosaico, three 480 by 480 screens in the dark, light and dark themes: drive gauges, what R.E.V sees and estimates, and what it has learned"></p>

The next board is Espressif's ESP-Mosaico<sup>[[12]](#ref-12)</sup>, built on the ESP32-S31, with a 480 × 480 touch screen, Bluetooth Classic, a motion sensor and a small battery. mossREV 1.0.0 below runs on it, and the concept above (a render with example values) shows CarMind R.E.V on it. The full board comparison is in the [detailed README](https://github.com/AlveeeRahman/OpenCarMind/blob/rev-c2/README.md#next-board-esp-mosaico-with-the-esp32-s31).

### How the S3 and the S31 are compatible

The two chips do not share machine code, but OpenCarMind builds for both from the same source through ESP-IDF. A test build for the S31 compiled with only the board's pins and one wake-up call changed. The models need to be exported again for the S31, with the same trained weights.

### Continual learning on the S31

The Mosaico's accelerometer can label braking and acceleration in the car, so the manoeuvre classifier could finally learn there, not only the fuel estimate. Its larger memory can keep past drives so new ones do not erase what the model knew, and its battery lets an update finish after the ignition is off.

### A release for the Mosaico

**mossREV 1.0.0** is the ESP-Mosaico build of OpenCarMind. It comes from the same source tree as the S3 version and runs CarMind R.E.V only: there is no E.V file in it and no model switch. The desktop app's `MODEL` command answers R.E.V and refuses a switch the same way an E.V-only S3 build refuses R.E.V. Every model mark and word on the screen is R.E.V's. The version, `mossREV 1.0.0`, is in the boot log and on the Board page (the USB door reports it as `fw=mossREV-1.0.0`, because a key=value field has no spaces). The S3 build is unchanged: E.V by default, R.E.V from the desktop app.

```sh
. $IDF_PATH/export.sh
idf.py --preview set-target esp32s31
idf.py build
```

The S31 is a preview target in ESP-IDF 6.1, hence `--preview`. The board's settings are in `sdkconfig.defaults.esp32s31` and its partition table in `partitions_mosaico.csv`: two 6 MB app slots and 3.75 MB of flash for the logs, since the board has no SD card slot.

- **The model.** `ml/onboard_s31/rev/onboard.espdl` is the public R.E.V, re-quantized by ESP-PPQ for the esp32s31 (8-bit weights, 16-bit activations) from the same trained weights and calibration windows as the S3 file. The S31 stores one weight exponent per output channel, and the classifier head dequantises to exactly the S3 values. It is exported without ESP-PPQ's int16 lookup table (`int16_lut_step=-1`): ESP-DL reads a stepped int16 table by nearest neighbour while ESP-PPQ computes the exact ReLU, and that was the whole gap between the chip and the PC. Without the table the board's self-test is bit-exact (0 of 64, 0 of 3 and 0 of 4 values off), and the quantised model picks the same manoeuvre class as the float model on 255 of 255 calibration windows and 16 of 16 test windows. `main/ml_params_rev_s31.h` is generated from that folder with `tools/gen_ml_params.py`.
- **The screens.** A launcher with touch on the 480 × 480 AMOLED: Drive, AI, Learning, R.E.V (the model in use, its self-test and the fuel-learning counts), Summary, Live, Fuel, Brakes, Trip, Engine, Faults, Board, Files and Settings. Swiping between pages runs at 17.9 frames per second over a whole run (median slide frame 29 ms).
- **Smart swipe.** A small learner on the board (`main/swipe_intent.c`, 1.3 KB: an online extreme learning machine with 16 hidden units and a recursive-least-squares output layer) predicts 40 ms after your finger lands whether a launcher swipe goes to the next page, the previous page or stays. It never moves a page itself. When it may act, it keeps the Home picture current while you rest on a page you usually leave for Home, and starts the snap at the lift. It learns on your device and turns itself off if it does not help: after a 30-swipe warm-up it runs an interleaved A/B test, and it stays on only if the 95th-percentile time from lift to a settled page drops and the predicted page was ready on more than 80 % of swipes (at least 50 swipes in each arm). The verdict is kept in NVS and re-tested after a firmware update; Settings shows it under SMART SWIPE. `CONFIG_OCM_SWIPE_INTENT` (on by default) is S31-only; the S3 build does not contain it.
- **Flashing without buttons.** The Type-C port is the S31's USB OTG port, so the console and the USB door run on TinyUSB there. When a computer opens that port at 1200 baud (the Arduino upload convention), the board sets the chip's force-download flag and restarts into its ROM download mode. No BOOT or RESET press is needed: open the port at 1200 baud, for example with pyserial, `python -c "import serial; serial.Serial('/dev/cu.usbmodemXXXX', 1200).close()"`, then flash to the ROM's port when it appears with `idf.py -p <that port> flash`.
- **Log storage that survives resets.** The logs live in a wear-levelled FAT in the 16 MB flash. ESP-IDF writes each 4 KB sector as an erase followed by a write, so a reset between the two leaves an erased sector, and FatFs then copies a torn first FAT over the good second one. This lost the whole volume twice on the bench. Now the logger flushes and holds the file system's lock before every software reset, the two FAT copies are compared before mounting and a torn copy is rewritten from the good one, lost clusters are freed, and a volume that holds data but does not mount is never formatted on its own (Settings has Format storage for that). In a reset-injection bench test (`CONFIG_OCM_STORAGE_RESET_TEST`, off by default, never in a car build) the volume was lost 8 times in 90 resets before the fix and 0 times in 200 after; with the screen frozen until the stall guard resets the board, 2 in 40 before and 0 in 60 after. A panic can still damage the files whose directory sector it was writing: 21 of the 200 cycles damaged a file, almost all after a panic. There is no journal yet.

## Limitations and future work

- **Torque is a physics estimate, not a measurement**, checked against one public car and for petrol engines only. R.E.V learns the engine size it uses, about 2.1 L from my car's last two drives, but not the torque itself. Cars that report torque (PIDs 0x62 and 0x63) could give a learned torque read-out its label, the way the fuel read-out learns now, and then the physics would not be needed. That is planned, not built or tested. My Sentra and most pre-2020 cars report no torque, so there is no label, and physics, or the ESP-Mosaico's accelerometer as a label, stays needed there.
- **The classifier and brake pressure do not learn in the car yet.** Accelerometer labels on the ESP-Mosaico and a self-check that forecasts the next moment are the routes being worked on.
- **No Wi-Fi upload yet.** It is being reworked so a lost packet on weak home Wi-Fi does not stall every file, as in QUIC<sup>[[13]](#ref-13)</sup>. Until then drives come over the USB cable.
- **The clock is only as exact as one second**, and it needs a known Wi-Fi network within reach.

## Planned

- The ESP-Mosaico port, and a build without a display.
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

## Credits and licence

OpenCarMind is released under the MIT licence (see `LICENSE`). ESP-IDF, ESP-DL, LVGL and NimBLE keep their own licences. The training data of the shipped models is not part of this repository. The replay samples also use the carOBD dataset, <https://github.com/eron93br/carOBD>. See `samples/replay/README.md` for its terms.
