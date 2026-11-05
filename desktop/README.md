# Desktop app

Status: preview, version 2.5.1, released together with the firmware of the same number. The app imports drives from the board over the USB cable and shows them. It can also open an SD card directly. In 2.5.x the board's Wi-Fi only sets its clock: the Wi-Fi upload is being reworked (see the main README, Limitations and future work), so the app opens no network port.

Built with Rust and Tauri for Windows, macOS (Apple silicon and Intel) and Linux. All the setup lives in the app: it writes the board's Wi-Fi networks, settings and pairing file to the SD card. The board itself has no console and accepts no incoming connections.

## Get the installers

Download them from the [Releases page](https://github.com/AlveeeRahman/OpenCarMind/releases/latest). `SHA256SUMS` lists a checksum for every file, and each file has build provenance from GitHub (`gh attestation verify <file> --repo AlveeeRahman/OpenCarMind`).

| System | File |
|---|---|
| Mac with Apple silicon | `OpenCarMind_<version>_aarch64.dmg` |
| Mac with an Intel chip | `OpenCarMind_<version>_x64.dmg` |
| Windows | `OpenCarMind_<version>_x64-setup.exe` or `OpenCarMind_<version>_x64_en-US.msi` |
| Debian, Ubuntu, Mint | `OpenCarMind_<version>_amd64.deb` |
| Fedora, RHEL | `OpenCarMind-<version>-1.x86_64.rpm` |
| Other Linux | `OpenCarMind_<version>_amd64.AppImage` (needs FUSE) |

The installers are not code-signed. macOS asks you to confirm the first launch: right-click the app and choose Open. Windows SmartScreen shows a warning: choose More info, then Run anyway. Builds of every change to `desktop/` are also kept for 90 days under the `desktop` workflow in the Actions tab.

Each build runs the Rust unit tests on its own system first. They check the session summary maths against `tools/ocm_report.py`, the receiver's resume-by-offset rules, and the Wi-Fi, time zone and time server checks.

## Use it

1. Install the app and connect the board to the computer with its USB cable. Open the Board page, pick the port (the app marks the one that looks like the board) and choose Connect.
2. The first time, choose Show a code on the board, type the 6-digit code from the board's screen, and choose Pair. Later connections need no code.
3. Add your Wi-Fi network, check the time zone the app filled in, optionally type a time server you trust (an HTTPS host name; "default" for the built-in one), and send them to the board. The board uses Wi-Fi only to set its clock.
4. After a drive, connect the board again and choose Import drives over USB.

More on the Board page, all over the cable:

- On-board model shows whether the board runs CarMind R.E.V (the default on a new board since 2.5.1), which keeps learning your car's fuel use and engine size, or the fixed CarMind E.V, and switches between them.
- Import drives over USB copies new drives from the card into the app's drives folder, at about 15 KB/s.
- Flash the firmware installs the firmware that ships with the app. The board keeps its Wi-Fi networks and pairing.

The Fuel page keeps your fill-ups (per board, in the app's data folder as `fillups.json`) and sends them to a connected board, which uses them to correct its fuel figures. It also shows fuel and economy per drive and in total, and what R.E.V has learned.

To read a card on its own, choose Open card folder on the Overview and pick the card's `obd` folder, its `sessions` folder, or any folder of `sNNNNNN.csv` files.

## How the board and the app trust each other

Over USB: the board accepts setup commands only after the app shows it the code from the board's screen, or, once paired, an HMAC-SHA256 of a fresh random challenge keyed with the app's token. The app does the work and the board only stores or applies what it receives (`main/usb_door.h`).

Over Wi-Fi (off in 2.5.x, kept for the reworked upload): the board is the client and the app is the server. The app makes a self-signed certificate and a random token on first run and keeps them in its data folder. Pairing hands both to the board over the cable, so they never cross the network in the clear. The board then trusts only that certificate (TLS 1.2), sends the token with every request, and appends each session file from the point the app already has. A pass that is cut off resumes where it stopped, and no byte is stored twice. Details and the protocol are in `main/uploader.h`.

With the upload back on, your firewall must let the app accept connections on port 8787. Allow it for private networks only. Windows normally asks the first time. macOS can block an app it has not approved without asking, so if no drives arrive, add OpenCarMind under System Settings, Network, Firewall, Options. On Linux, open the port if a firewall is active, for example `sudo ufw allow from 192.168.0.0/16 to any port 8787 proto tcp`, and add yourself to the `dialout` group (`uucp` on Arch) so the app can open the USB port: `sudo usermod -aG dialout $USER`, then log out and in.

If the computer's address changes, connect the board by USB and pair again.

## Flashing and esptool

The app carries the firmware image built from the same release and a copy of Espressif's `esptool` 5.6.0, the standalone build Espressif publishes for each system. You do not need ESP-IDF, Python or a driver: on macOS and Linux the board's USB port works as a serial port out of the box, and Windows 10 and 11 include the driver for it (not yet checked on Windows). The build scripts check each `esptool` download against a pinned SHA-256 (`app/scripts/fetch-esptool.sh`).

`esptool` is free software under the GNU GPL version 2. It runs as a separate program, its licence ships with the app as `esptool-LICENSE.txt`, and its source is at <https://github.com/espressif/esptool>.

## Window behaviour

One window, native frame, resizable down to 480 by 360. Below 640 px the margins shrink, cards and charts wrap to the width, and wide tables scroll sideways. A second launch focuses the running window instead of opening another. Closing the window quits the app. Every control is reachable from the keyboard.

## Contents

- `app/`: the Tauri app (`src-tauri` for Rust, `ui` for the window).
- `icons/`: the app icon in the sizes Tauri expects (`32x32.png`, `128x128.png`, `128x128@2x.png`, `icon.png`, `icon.icns`, `icon.ico`), the 1024 px master, and the tray glyphs (`tray-template` is black on transparent so macOS can tint it, `tray-light` is white for dark panels).

## The health states

The planned health screen uses six states so that a missing signal is never shown as a healthy one: OK, Watch, Alert, Learning (fewer than three baseline drives), No data (too few events to judge) and Not observable (standard OBD-II has no signal for it). A system the car does not report, such as the steering-wheel buttons, stays "Not observable" until you map a custom PID or add a CAN tap.

## Window management and tiling

One Tauri codebase builds the Windows, macOS and Linux apps, so the same rules apply to all three. The app follows these rules.

- Use the operating system's own window frame, with no custom title bar. Windows snap layouts, macOS full screen and split view, and Linux tiling window managers (i3, sway, Hyprland, and the tiling modes of GNOME and KDE) then treat it as an ordinary window.
- Make it resizable with a small minimum size (480 by 360 pixels), and reflow the layout. Pages are tabs at the top, cards and charts wrap to the width, and wide tables scroll.
- Do not remember the window's position. Wayland clients cannot place their own windows, and a tiling window manager decides position anyway.
- Give the app a stable identifier so tiling rules can target it.
- Closing the window quits the app. The tray icon is optional, because some Linux desktops have no tray and a hidden window would be lost there.
- Allow one instance. Starting the app again, or running a command that needs the window, focuses the one that is open.
- Make everything reachable from the keyboard.

Tested: CI builds the app and runs its unit tests on each system. No CI job opens the window, and tiling has not been tried on any desktop yet. That check is in [docs/FIELD_CHECKS.md](../docs/FIELD_CHECKS.md).

## Not built yet

- Code signing for the installers.
- Finding the computer again when its address changes. Today that needs a new pairing over USB.
- A health page with the state of each subsystem.
