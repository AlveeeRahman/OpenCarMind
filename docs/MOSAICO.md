# OpenCarMind on the ESP-Mosaico (mossREV 1.2.0)

mossREV is the build of OpenCarMind for Espressif's ESP-Mosaico board. It comes from the same source tree as the ESP32-S3 version and does the same job: it reads your car over a Bluetooth OBD-II adapter, logs every drive, and runs a small neural network on the chip. The Mosaico adds a 480 x 480 touch screen, more memory and a 128 MB store for the drive logs.

The S3 build is not changed by any of this. For the Waveshare ESP32-S3 board, keep using the S3 releases and the main [README](../README.md).

## Contents

1. [The hardware](#the-hardware)
2. [Flashing](#flashing)
3. [Using the desktop app with the Mosaico](#using-the-desktop-app-with-the-mosaico)
4. [The apps](#the-apps)
5. [The two models](#the-two-models)
6. [Smart swipe and Smart UI](#smart-swipe-and-smart-ui)
7. [Clock, brightness and theme](#clock-brightness-and-theme)
8. [Storage that survives resets](#storage-that-survives-resets)
9. [The 128 MB NAND](#the-128-mb-nand)
10. [Swiping between pages](#swiping-between-pages)
11. [Known limits](#known-limits)
12. [Coming next](#coming-next)
13. [Licences](#licences)

## The hardware

| Part | On the ESP-Mosaico V1.2 |
|---|---|
| Chip | ESP32-S31 (ESP32-S31NRV16), dual core, run at 320 MHz |
| PSRAM | 32 MB in the package. The board's user guide says 16 MB. The chip reports 33,554,432 bytes (`esp_psram_get_size()`, measured on the board) |
| Flash | 16 MB NOR (GD25Q128): two 6 MB app slots, and 3.75 MB of FAT for the state files |
| Drive-log store | 128 MB SPI NAND (GD5F1GM7UEYIGR) on its own SPI bus, see [The 128 MB NAND](#the-128-mb-nand) |
| Screen | 2.16 inch CO5300 AMOLED, 480 x 480, over QSPI |
| Touch | CST9220 on I2C |
| Battery | a small cell with a BQ27220 fuel gauge |
| Radio | Wi-Fi and Bluetooth. OpenCarMind uses Bluetooth LE for the OBD adapter, as on the S3 |
| USB | one Type-C port, the S31's USB OTG port: console, the desktop app's USB door, and flashing |

There is no SD card slot. The drive logs live on the NAND, and the learned model state, the clock, the settings files and the Wi-Fi list live in the flash FAT.

**Unused pins.** At boot every GPIO the firmware does not use is put in a safe state: input and output off and no pull resistor, held (the strapping pins 37 to 40 are not held), and the speaker amplifier's enable (GPIO45) is driven low, so the amplifier stays off. The flash, USB, UART0 console and BOOT pins are never touched. This gives the same pin state at every boot. Its power saving is not measured.

## Flashing

You need the merged image `mossREV-1.2.0-esp32s31-merged.bin` (its SHA-256 is in the `.sha256` file next to it), a USB-C cable, Python with pyserial (`pip install pyserial`), and esptool 5.6.0 or later (`pip install "esptool>=5.6.0"`), the version this release was tested with.

**Before you flash, copy your drives off the board and expect to pair again.** The merged image starts at address 0x0 and covers the NVS partition (0x9000 to 0xF000). Flashing it erases the pairing with the desktop app, the saved Wi-Fi networks, the settings, the Smart swipe and Smart UI verdicts, the model choice and the session counter. The flash FAT (0xC40000) and the NAND are not written, and the next session number continues after the highest one found on the storage, so old drives are not overwritten. Import your drives first anyway. A flash that stops halfway is not covered by that.

1. Plug the board into the computer. It shows up as a USB serial port (`/dev/cu.usbmodem...` on macOS, `/dev/ttyACM...` on Linux, `COMx` on Windows).
2. Put it into download mode without pressing a button: open that port at 1200 baud and close it again. The board waits until its storage has started, flushes the logs, holds the file systems and restarts into the chip's ROM download mode.

   ```sh
   python -c "import serial; serial.Serial('/dev/cu.usbmodemXXXX', 1200).close()"
   ```

3. A new port appears, the ROM's. Write the image to it:

   ```sh
   esptool --chip esp32s31 --port <ROM port> --before no-reset --after hard-reset \
       write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 mossREV-1.2.0-esp32s31-merged.bin
   ```

4. The board restarts into mossREV. The console (115200 baud on the same port) prints `OpenCarMind mossREV 1.2.0 running`. Pair it again from the desktop app.

To build it yourself, use ESP-IDF 6.1. The S31 is a preview target there, hence `--preview`:

```sh
. $IDF_PATH/export.sh
idf.py --preview set-target esp32s31
idf.py build
```

The board's settings are in `sdkconfig.defaults.esp32s31` and its partition table in `partitions_mosaico.csv`.

## Using the desktop app with the Mosaico

The desktop app talks to the Mosaico over the same USB protocol as the S3: pairing, the drive list and import, the model switch and adding Wi-Fi networks all work the same way. Two things in the app are written for the S3. Its **Flash firmware** button is for the ESP32-S3 only (it runs esptool with `--chip esp32s3`), so do not use it for the Mosaico and follow [Flashing](#flashing) instead. And where the app says "SD card", on the Mosaico it means the board's own storage: the drive logs are on the NAND and the learned model state is in the flash, and nothing has to come out of the board.

## The apps

The launcher has a Home page and 16 apps on four more pages. Swipe left and right between pages, tap an app to open it, and tap Back (the round arrow at the top left) or swipe down to come home. Back is a 44 px circle with a touch area 104 px wide that reaches from the screen corner down to the first card, so a thumb from the corner always hits it.

- **Drive.** The live drive view (speed, rpm and more) and the End drive control (hold it for 1 second).
- **AI.** What the model in use sees: the manoeuvre class and the three estimates (brake pressure, flywheel torque, fuel rate).
- **Learning.** The model in use, its self-test, and what R.E.V has learned so far.
- **R.E.V and E.V.** One app per model, side by side on the launcher: its self-test, its own figures (R.E.V's fuel learning, what E.V answers) and a 64 px "Use R.E.V" or "Use E.V" button that reads "In use" for the model running. The button takes the Settings > MODEL path and is refused during a drive. While E.V runs, the Learning app dims its cards and says learning is off, and R.E.V keeps what it learned.
- **Summary.** The last drive: distance, time, economy, hard brakes, a speed and fuel chart, what the AI saw every 10 seconds, the history of the last drives, and the recent events (see below).
- **Live, Fuel, Brakes, Trip, Engine.** The live OBD values grouped by topic.
- **Faults.** Stored, pending and permanent trouble codes, ranked by severity and grouped by shared cause.
- **Board.** Firmware version, chip, memory, storage, the scheduler's job table.
- **Files.** The drive logs on the board: size, rows and date of each, its files, and delete (always behind a confirm sheet). The drive being written cannot be deleted.
- **Wi-Fi.** Search for networks and join one from the screen (see below), the saved networks with Forget, and Sync clock now.
- **Settings.** Brightness, theme, the model, the clock, units, screen off, the order of the apps on Home, the OBD adapter, Format storage, and the firmware version.

**Recent events (Summary).** The board's event list, newest first, in the same colours as the S3's events page: errors red, warnings amber. A tap opens the full list with filters (All, Warnings, Errors, Wi-Fi, Drive). A tap on a row opens its detail. Each drive also keeps its own events: the recorder copies them while the drive runs (the newest 24) and saves them with the drive's summary file, so the Drive filter shows them even after a long drive or a restart. After a restart the Summary shows the last drive again, read back from its files (the charts are not kept).

**Wi-Fi search and join.** Search for networks runs one active scan and lists up to 20 networks, strongest first, with the signal bars and a padlock for secured ones. Tap a secured network to type its password on the on-screen keyboard (letter keys 44 x 59 px, every printable character on four layouts), or tap an open one to join it with no password. The board saves the network the same way the desktop app's Wi-Fi add does (up to 8), connects once, and says what happened: connected, wrong password, or can't reach it. A wrong password for a new network is not kept. The network names show on the screen only. The serial log writes `<network>` in their place.

Wi-Fi still does one thing: it sets the board's clock from an HTTPS time server while the car is parked. It never runs while a drive is being logged. A board that booted with no saved network starts Wi-Fi the first time you search or join, and it then stays on until the next restart.

**Settings.** Every control is at least 64 x 64 px. A tap shows the new choice in the same frame. Saving waits until you stop tapping (500 ms after the last change, at most 3 seconds after the first), and a theme or Home-order change rebuilds the launcher once, after the taps. Before this redesign, a tap showed nothing new for 725 to 1,648 ms (measured on the board). App hiding is gone: every app is always on the launcher, and only the order can change. A board that had hidden apps shows them again after the update.

## The two models

mossREV carries both on-board models, each re-quantized by ESP-PPQ for the esp32s31 from the same trained weights and calibration windows as the S3 files (8-bit weights, 16-bit activations):

| | CarMind R.E.V | CarMind E.V |
|---|---|---|
| File | `ml/onboard_s31/rev/onboard.espdl`, version `bdcf1bb83f01` | `ml/onboard_s31/ev/onboard.espdl`, version `621f1ce3196e` |
| Self-test on the board | exact: 0 of 64 z values, 0 of 3 estimates, 0 of 4 class scores off | exact: 0 of 3 estimates, 0 of 4 class scores off |
| Same class as the float model | 255 of 255 calibration windows, 16 of 16 test windows | 255 of 255 calibration windows, 16 of 16 test windows |
| One inference on the board | 1,960 us (the timed run at boot) | not measured yet |

Both files are exported without ESP-PPQ's int16 lookup table (`int16_lut_step=-1`). ESP-DL reads a stepped int16 table by nearest neighbour while ESP-PPQ's simulator computes the exact ReLU, and that was the whole gap between the chip and the PC.

A new board starts on R.E.V. To switch, tap Use in the R.E.V or E.V app, open Settings > MODEL and tap R.E.V or E.V, or use the desktop app's model switch. A switch needs the car to be parked: during a drive the board says "A drive is active. End the drive first, then switch models." The board keeps the choice after a restart. Every model mark and name on the screen follows the model in use, and what R.E.V learned stays on the board while E.V runs. One model is loaded at a time.

## Smart swipe and Smart UI

**Smart swipe** is a small learner on the board (`main/swipe_intent.c`, 1,680 bytes of RAM). 40 ms after your finger lands on the launcher it predicts whether the swipe goes to the next page, the previous page or stays. It never moves a page itself. When it may act, it keeps the Home picture fresh while you rest on a page you usually leave for Home, and starts the snap at the lift.

**Smart UI** measures every touch interaction the same way, by class: app opens, Back, in-app controls and list scrolls. Its first helper is a press-time prefetch: it starts building the app you are opening, or the launcher you are going back to, when your finger goes down instead of when it lifts.

Both follow the owner's rule, and the rule is decided by your real use, not by a bench script (a learner trained on a script only learns the script). Per class, the board measures the time from the finger lifting to the page settling. A helper stays on only if the 95th-percentile time drops and the predicted page was ready more than 80 % of the time, with at least 50 interactions on each side of a random A/B split, a one-sided Mann-Whitney test with z above 1.645, and a Holm correction across the classes. Otherwise it switches itself off. The verdicts are kept in NVS and tested again after a firmware update. Until there is a verdict, a helper does not act.

Measured in the owner's real use before any helper acted (95th percentile, lift to settled): app opens 973 ms, Back 561 ms, launcher swipes 1,055 ms. A scripted bench gave the app prefetch no gain (828 ms with and without it), and that bench result was reset on purpose: real use decides. Smart swipe now measures from 0 to 3 seconds. Its first version stopped at 475 ms, below the owner's real 95th percentile, so it could never have passed.

## Clock, brightness and theme

The board has no battery-backed clock chip. The clock comes from Wi-Fi (an HTTPS time server), or you set it by hand in Settings > Set clock, and the chip's RTC keeps it through a soft reset. The Settings clock line says where the time came from: "Not set", "Estimated, not synced", "Set by hand" or "From Wi-Fi". The board saves the last known time to a file at most once a minute (on the NAND when it is mounted), so after a power cut it starts from that time and Wi-Fi corrects it later.

Brightness is a slider in Settings. The panel's brightness is a command to the AMOLED, not a backlight.

The theme is Auto (light by day, dark at night once the clock is known), Light or Dark, and it covers every app. On an AMOLED every lit pixel draws current and black pixels almost none, so Light costs more battery than Dark at the same brightness. How much has not been measured yet. Every text and ground pair in both palettes is checked by a host test for at least 4.5:1 contrast (graphics 3:1). The worst text pair is 4.58:1.

Screen off after a set time turns the panel's display off without its sleep mode. With the sleep mode, every one of 4 wakes stopped the touch controller from answering about 5 seconds later. Without it, 20 of 20 wakes were clean. If the touch controller ever stops answering anyway, the board opens it again after 30 bad reads, which took about 1 second each time it happened on the bench.

## Storage that survives resets

ESP-IDF writes each 4 KB flash sector as an erase followed by a write, so a reset between the two leaves an erased sector, and FatFs then copied a torn first FAT over the good second one. That lost the whole volume twice on the bench. Now:

- Before every software reset the logger flushes and holds the file system's lock.
- The two FAT copies are compared before mounting, and a torn copy is rewritten from the good one.
- Lost clusters are freed and torn directory entries are marked deleted.
- A volume that holds data but does not mount is never formatted on its own. Settings has Format storage for that, and it works right after boot too.
- Entering download mode through the USB port waits until the storage has finished starting.

| Bench test | Before the fix | After |
|---|---|---|
| Random software resets (restart, abort, ROM reset), volume lost | 8 of 90 | 0 of 200 |
| Screen frozen until the stall guard resets the board, volume lost | 2 of 40 | 0 of 60 |
| Entering download mode during storage start-up, sectors torn | 3 of 12 | 0 of 24 |

The reset test (`CONFIG_OCM_STORAGE_RESET_TEST`) is off in the release and never in a car build.

## The 128 MB NAND

The drive logs (`sNNNNNN.csv`, `.json`, `.pat`, `.ml.csv`, `.sum.json` and `drives.csv`) are written to the board's 128 MB SPI NAND, mounted at `/nand`. The flash FAT keeps the state files. NAND writes go over their own SPI bus and never turn the flash cache off, so a log write no longer stalls the screen or the other tasks the way a NOR flash write does.

- The NAND runs on SPI3 at 20 MHz with the Dhara flash translation layer (`espressif/spi_nand_flash` 1.4.4) and a FAT16 volume with two FAT copies.
- A blank NAND is formatted at the first boot. A NAND that holds data but will not mount is never formatted on its own.
- Drives logged before the update are moved from the flash to the NAND once: each file is copied, synced, read back and compared (size and CRC) before the flash copy is removed. A reset in the middle of the move leaves either the old copy or two identical copies, never none.
- The desktop app lists and imports drives from both stores while the move runs. A drive on both is shown once.
- Format storage formats the NAND (about 15 seconds) and removes the drive logs still on the flash. The state files are not touched.
- Without a working NAND the board logs to the flash FAT as before.

The NAND's speed, its power draw and a reset-injection run on it are part of the board check for this release and are not in this guide yet. The store rules, the move and the volume repair are covered by host tests (652 checks, the repair at 19 reset points).

## Swiping between pages

Pages slide as cached pictures sent straight to the panel while your finger moves.

| | Frames per second | Median frame while sliding |
|---|---|---|
| Before (plain LVGL redraw) | 4.4 | not logged (mean 210 to 270 ms) |
| mossREV | 17.9 over a whole run | 29 ms (compose 8 ms, send 5 ms) |

A full 480 x 480 frame takes 23.6 ms on the panel link (measured), so a full-screen redraw cannot get faster than that.

## Known limits

- **A panic in the middle of a write can still damage a file.** The volume survives, but 21 of 200 reset-test cycles damaged a file, almost all of them after a panic, which cannot flush first. There is no journal.
- **A true power cut is not tested.** The bench covered software resets, aborts and ROM resets. A setting changed less than 500 ms before a power cut is lost.
- **Flashing the merged image erases the NVS** (see [Flashing](#flashing)). That includes the NAND's "format in progress" flag, so do not flash while a format runs.
- **The download door needs no pairing.** Anyone with the cable can open the port at 1200 baud and put the board into download mode. That is how flashing works without buttons.
- **A theme switch pauses the screen once.** It rebuilds the launcher and the open app in one go. On the bench the launcher took 267 to 542 ms and the Settings page 249 to 714 ms. The switch waits while the Wi-Fi password page or the clock picker is open.
- **Wi-Fi.** Enhanced-open (OWE) networks are shown as secured and ask for a password. WEP and WPA-TKIP-only networks end with "Can't join: the board does not support this network's security type." Wi-Fi started from the screen stays on until the next restart. There is no Wi-Fi upload of drives: they come over the USB cable.
- **The events list keeps the newest 10 events since boot**, in RAM. Each drive saves its own newest 24.
- **LVGL is not pinned to one version** in the build, and two screen features depend on LVGL 9.5 internals.
- **The Wi-Fi fixes of this release are for the Mosaico only for now:** the cleanup that could forget the network that just connected, the network names in the serial log, and the lock around adding and forgetting networks over USB. The S3 copies come with a later S3 release, because the S3 build stays byte-identical to the current S3 release.

## Coming next

mossREV 2.0.0 will use the S31's pixel-processing accelerator and 2D-DMA for the screen, together with the internal-RAM work.

## Licences

- OpenCarMind: MIT.
- Tabler Icons v3.51.0, 14 of the launcher's app icons (the R.E.V and E.V apps show their own marks): MIT, Copyright (c) 2020-2026 Paweł Kuna.
- Fonts on the screen: LVGL's built-in Montserrat fonts, under the SIL Open Font License 1.1, with the Font Awesome 5 symbols LVGL merges into them, under the Font Awesome Free licence.
- No Inter font ships in the firmware. The desktop app's stylesheet names Inter first but uses it only if it is installed on the computer.

The full notices are in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
