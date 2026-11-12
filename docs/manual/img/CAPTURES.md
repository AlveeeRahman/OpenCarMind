# Manual captures

Every screen capture the manuals use, which screen and theme it shows, and the build it came from. All Mosaico captures are 480 x 480 PNG files taken on a bench with no car connected. They were checked before publishing: no network names, VIN, MAC address, pairing code or clock time zone line.

To swap a capture, write the new PNG over the file with the same name and run `python3 docs/manual/build.py`. The file names stay the same, so neither manual source changes. A capture of a screen that has none yet (the Board page) needs a new file and an `<img>` in `src/edgeos-2.0.0.src.html` in place of its "Capture coming" frame.

## ESP-Mosaico (OpenCarMind EdgeOS 2.0.0 manual)

All 20 were taken on mossREV 1.2.0 builds, before the 2.0.0 rename and before the larger screen text. Retake every one on the final EdgeOS 2.0.0 image in the same theme.

| File | Screen | Theme | Taken on | Action |
|---|---|---|---|---|
| `mosaico-home-light.png` | Home | light | 1.2.0-line test build | retake on EdgeOS 2.0.0 |
| `mosaico-launcher-page1-light.png` | Launcher page 1 (Drive, AI, Learning, R.E.V) | light | mossREV 1.2.0 rc2 | retake on EdgeOS 2.0.0 |
| `mosaico-launcher-page2-dark.png` | Launcher page 2 (E.V, Summary, Live, Fuel) | dark | mossREV 1.2.0 rc2 | retake on EdgeOS 2.0.0 |
| `mosaico-drive-light.png` | Drive, E.V in use | light | mossREV 1.2.0 rc2 with a fix | retake on EdgeOS 2.0.0 |
| `mosaico-ai-dark.png` | AI, E.V in use | dark | mossREV 1.2.0 rc2 with a fix | retake on EdgeOS 2.0.0 |
| `mosaico-learning-rev-light.png` | Learning, R.E.V in use | light | mossREV 1.2.0 rc2 | retake on EdgeOS 2.0.0 |
| `mosaico-learning-ev-light.png` | Learning, E.V in use (learning off) | light | mossREV 1.2.0 rc2 | retake on EdgeOS 2.0.0 |
| `mosaico-rev-light.png` | R.E.V app, in use | light | mossREV 1.2.0 rc2 | retake on EdgeOS 2.0.0 |
| `mosaico-ev-dark.png` | E.V app, in use | dark | mossREV 1.2.0 rc2 | retake on EdgeOS 2.0.0 |
| `mosaico-summary-light.png` | Summary, no drive yet, recent events | light | 1.2.0-line test build | retake on EdgeOS 2.0.0, with no old "NAND damaged" event |
| `mosaico-live-light.png` | Live | light | 1.2.0-line test build | retake on EdgeOS 2.0.0 |
| `mosaico-fuel-dark.png` | Fuel, E.V in use | dark | mossREV 1.2.0 rc2 | retake on EdgeOS 2.0.0 |
| `mosaico-brakes-dark.png` | Brakes and torque, E.V in use | dark | mossREV 1.2.0 rc2 | retake on EdgeOS 2.0.0 |
| `mosaico-trip-dark.png` | Trip, between drives | dark | 1.2.0-line development build | retake on EdgeOS 2.0.0 |
| `mosaico-engine-dark.png` | Engine | dark | 1.2.0-line test build | retake on EdgeOS 2.0.0 |
| `mosaico-faults-light.png` | Faults, no codes | light | 1.2.0-line test build | retake on EdgeOS 2.0.0 |
| `mosaico-files-light.png` | Files, drives on the 3.7 MB flash store | light | 1.2.0-line test build | retake on EdgeOS 2.0.0, with the 128 MB NAND in the storage bar |
| `mosaico-wifi-dark.png` | Wi-Fi, no network saved | dark | 1.2.0-line test build | retake on EdgeOS 2.0.0, with no network names on screen |
| `mosaico-settings-dark.png` | Settings, Screen off after and Home apps order | dark | 1.2.0-line development build | retake on EdgeOS 2.0.0, never the clock time zone line |
| `mosaico-alarm-banner-dark.png` | Home with the bench test critical-fault banner | dark | mossREV 1.2.0 rc3 line, bench test alarm | retake on EdgeOS 2.0.0 |

Needed and not taken yet:

| File (to add) | Screen | Theme | Action |
|---|---|---|---|
| `mosaico-board-dark.png` | Board page with the bottom line "Parked, health good" and the battery card | dark | take on EdgeOS 2.0.0, then replace the "Capture coming" frame in the source |

## ESP32-S3 (OpenCarMind 2.5.1 manual)

The S3 manual uses the renders already in `docs/images/`, so no copy lives here. They are renders with example values, not photos, and do not change with Edge OS.

| File | Screen | Theme |
|---|---|---|
| `../../images/dash.png` | Page 1, Dashboard | dark |
| `../../images/dash-light.png` | Page 1, Dashboard | light |
| `../../images/faults.png` | Page 4, Faults | dark |
| `../../images/system-rev-light.png` | Page 5, System with R.E.V | light |

Pages 2 (Details) and 3 (Internals) show "Capture coming" frames: the manual waits for renders with no VIN and no network name on them.
