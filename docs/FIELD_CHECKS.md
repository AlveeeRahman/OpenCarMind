# Field checks

These checks need a real car, a real adapter or a real card, so they cannot run in CI. The result tables are empty on purpose. Run a check, then report what you saw in an issue (use the "Field check" template) and the table gets a row.

## Input checks on the board

Put the file named in each row on the SD card at `obd/`, boot the board and read the log over USB.

| Check | Do this | Expected | Firmware | Board | Result |
|---|---|---|---|---|---|
| Short Wi-Fi password rejected | `wifi.csv` with the line `Home,abc` | log says `network entry rejected` | | | |
| Over-long line skipped | `wifi.csv` with one line over 160 characters, then a valid line | log says `skipping over-long line`, the valid line is imported | | | |
| Normal clock sync | a valid `wifi.csv` and a network in reach | log says `clock synced` | | | |
| Control character rejected | `wifi.csv` with a tab inside the SSID | the line is not imported | | | |

## Driving

| Check | Do this | Expected | Firmware | Car and adapter | Result |
|---|---|---|---|---|---|
| Power-off line | watch page 3 while driving, then switch the car off | red "Not safe: logging" while driving, green "Safe to turn off" about 10 s after the car stops | | | |
| Power off while driving | cut power mid-drive, pull the card | the last rows of `sNNNNNN.csv` are intact, at most the final 10 seconds are missing | | | |
| Another adapter | pair a different BLE OBD adapter | the board finds it, connects and logs rows | | | |
| Adapter name not on the list | an adapter whose name is not in `OBD_BLE_NAME_PREFIXES` | it is not found until its name prefix is added | | | |

## Clock and the desktop app

| Check | Do this | Expected | Firmware and app | Computer | Result |
|---|---|---|---|---|---|
| HTTPS clock | park near a known Wi-Fi network after a boot | page 2 shows Clock: synced, and the session JSON has `"clock": "https"` | | | |
| Your time server | set Time server on the Board page to a host you run, restart | the clock still syncs; a host without a valid certificate is refused | | | |
| R.E.V torque and tank | drive with R.E.V selected, then open the Fuel page | torque shows on page 5 in amber (idle when stopped); the Fuel page shows the tank learning progress | | | |

## Wi-Fi upload to the desktop app (not in 2.5.0)

| Check | Do this | Expected | Firmware and app | Computer | Result |
|---|---|---|---|---|---|
| Pair and receive | pair over USB from the Board page, add your Wi-Fi, then park the car near that network | the app lists the board within a minute and the Overview shows its drives | | | |
| Resume | close the app during an upload, open it again | the upload continues and no row is repeated | | | |
| Firewall prompt | first connection after installing | the system asks to allow incoming connections | | | |
| Flash from the app | Board page, Flash the firmware | the board restarts on the new version and still has its Wi-Fi networks and pairing | | | |
| Import over USB | Board page, Import drives over USB | the drives appear on the Overview, and a second import copies nothing new | | | |
| Window tiling | snap the app to half the screen (Windows), split view (macOS), a tiling window manager (Linux) | it fills the tile and stays usable down to 480 by 360 | | | |
| Clock after a power cut | let the clock sync, cut power, boot without Wi-Fi | the screen clock shows in amber and keeps running, and the session JSON says `resumed` | | | |

## How to report

Open a "Field check" issue with the firmware version (page 2 of the screen, or `status`), your board, your car and adapter, and what you saw. Do not paste Wi-Fi names or passwords from the log.
