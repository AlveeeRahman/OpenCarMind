# Desktop app

Status: design only. Nothing in this folder runs yet. The app is planned for version 2.9.0.

The plan is a Rust and Tauri app for Windows, macOS and Linux that does what the board's screen and SD card cannot: read your drives, show them in one place, and manage the board's Wi-Fi from your computer. It talks to the board over your own Wi-Fi network only, with TLS and a one-time pairing code, and it never writes to the SD card. A command-line tool, `ocm`, does the same things for PowerShell, Terminal and scripts.

## Contents

- `icons/`: the app icon in the sizes Tauri expects (`32x32.png`, `128x128.png`, `128x128@2x.png`, `icon.png`, `icon.icns`, `icon.ico`), the 1024 px master and its SVG source, and the tray glyphs (`tray-template` is black on transparent so macOS can tint it, `tray-light` is white for dark panels).
- `design/`: five preview screens. They are mockups. The numbers on the overview come from the replay sessions in `samples/replay`, and the statuses on the health screen illustrate the possible states. They are not results from any car.

## Design previews

![Brand sheet](design/brand-sheet.png)

![Overview](design/overview.png)

![Subsystem health](design/subsystem-health.png)

![Board and Wi-Fi](design/board-and-wifi.png)

The same overview in a half-screen tile, as a tiling window manager or Windows snap layout would give it:

![Half-screen tile](design/half-screen-tile.png)

## The health states

The health screen uses six states so that a missing signal is never shown as a healthy one: OK, Watch, Alert, Learning (fewer than three baseline drives), No data (too few events to judge) and Not observable (standard OBD-II has no signal for it). A system the car does not report, such as the steering-wheel buttons, stays "Not observable" until you map a custom PID or add a CAN tap.

## Window management and tiling

One Tauri codebase builds the Windows, macOS and Linux apps, so the same rules apply to all three. These are requirements. Once the app exists, CI will test what it can.

- Use the operating system's own window frame, with no custom title bar. Windows snap layouts, macOS full screen and split view, and Linux tiling window managers (i3, sway, Hyprland, and the tiling modes of GNOME and KDE) then treat it as an ordinary window.
- Make it resizable with a small minimum size (480 by 360 pixels), and reflow the layout. Below about 900 pixels wide the sidebar becomes tabs, the cards wrap two by two and the tables become lists.
- Remember the window's size and whether it was maximised, but not its position. Wayland clients cannot place their own windows, and a tiling window manager decides position anyway.
- Give the app a stable identifier so tiling rules can target it.
- Closing the window quits the app. The tray icon is optional, because some Linux desktops have no tray and a hidden window would be lost there.
- Allow one instance. Starting the app again, or running a command that needs the window, focuses the one that is open.
- Make everything reachable from the keyboard.

Test plan: CI starts the Linux build under a virtual display with a tiling window manager and checks that it opens, fills its tile and resizes down to the minimum. On the macOS and Windows runners it checks that the app builds and starts. Tiling on every desktop still needs a person with that desktop, so this page will list which combinations have been checked.

## USB serial and drivers

The dashboard talks to the board over Wi-Fi. Changes to the board itself use the USB cable: flashing, choosing the Bluetooth adapter, and entering Wi-Fi credentials.

What each system needs, from Espressif's documentation and from this board on macOS:

| System | Serial over the board's built-in USB | JTAG and debugging | Boards with a USB-to-UART chip |
|---|---|---|---|
| macOS | No driver needed (checked with this board) | Works with no extra steps according to Espressif | Drivers are normally part of the OS. If no `/dev/cu.*` port appears, install the vendor driver |
| Windows | Not verified. Espressif's page on it does not say | Install Espressif's driver with `eim install-drivers` or `idf-env driver install --espressif` | Normally part of Windows |
| Linux | Not verified. The user must be in the `dialout` group (`uucp` on Arch) and log in again | Needs Espressif's `60-openocd.rules` udev rule | Normally part of the kernel, with the same group rule |

The app will include a first-run USB check. It lists the serial devices, says what is missing and offers to fix it with an explicit prompt. Windows gets Espressif's own driver installer, downloaded at that moment so the driver is current, with its checksum verified. Linux gets the udev rule and the serial group, added with `pkexec`. macOS only gets a pointer to the vendor driver, and only if a port is missing. It never installs a driver the connected device does not need. The Windows installer offers the same step as an optional page, the Linux `.deb` installs the udev rule, and the AppImage uses the first-run check.

Flashing uses `espflash` or `esptool`. The full ESP-IDF is only needed to rebuild the firmware, not to flash a prebuilt release.

## Not built yet

The pairing and Wi-Fi management need a TLS service on the board, which the firmware does not have. Until then, the supported way to see your drives is the report tool and Dashy described in [docs/DASHBOARD.md](../docs/DASHBOARD.md).
