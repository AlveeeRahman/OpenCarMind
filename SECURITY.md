# Security

OpenCarMind runs on a board plugged into your car's diagnostic port, so this page says what the firmware does with the network and your data, what it does not do, and how to report a problem.

## Reporting a vulnerability

Please report privately through GitHub: open the repository's Security tab and choose "Report a vulnerability". Do not open a public issue for anything that could be misused. Include the firmware version (shown on page 2 of the screen), your board, and the steps to reproduce.

## What the firmware does and does not do

- Opens no listening sockets and runs no web server. It has two network clients, both optional: the clock sync (SNTP to `pool.ntp.org`) and the uploader, which sends session files to the desktop app you paired over TLS 1.2. The uploader trusts only the certificate that came with the pairing file, sends a token with each request, and sends only to the address in the pairing file, which the desktop app sets to the computer's own address on your network. The firmware has no update mechanism.
- Starts Wi-Fi only if you saved a network, and never scans or connects while the car's OBD link is up. Turn Wi-Fi off completely with `OCM_WIFI_CLOCK_SYNC=n`.
- Runs Wi-Fi as a station only. The soft access point and enterprise Wi-Fi are compiled out, and the stack protector is on (`sdkconfig.defaults`).
- Keeps saved Wi-Fi passwords in the board's flash (NVS) in plain text. Anyone with physical access to the board can read them. Use a phone hotspot or a guest network, not your main network.
- Writes drive data to the SD card as plain CSV, JSON and text. Anyone who holds the card can read it, including the VIN and timestamps. Treat the card like any file that records where and when you drove.
- Talks to the car only through the paired Bluetooth LE adapter, by its saved address. It sends standard OBD-II read requests (modes 01, 03, 07, 09, 0A) and the custom PIDs you list in `custom_pids.csv`. It never sends mode 04 (clear codes). A custom PID sends exactly the command you put in the file, so list read requests only.
- Keeps the model in the firmware image.

The USB port only prints logs. The firmware reads nothing from it, so there is no command to send. It is also how you flash the board, and flashing needs no authentication: whoever can plug a cable in can reflash it.

## Checks on outside input

Everything that arrives from outside is checked before it is used. `main/harden.c` holds the rules, and `tests/host` tests them.

- Clock: SNTP replies are not authenticated. A time outside 2026-01-01 to 2099-12-31 is refused, the previous clock is restored, and the board logs a warning.
- Wi-Fi entries, from `wifi.csv`, `config.json` or the build settings: the SSID must be 1 to 32 bytes, the password empty or 8 to 64 bytes, and neither may contain control characters. A `wifi.csv` line longer than the read buffer is dropped whole.
- Wi-Fi connection: the board asks for WPA2 or better whenever a password is set, and uses protected management frames when the access point offers them.
- Model input: a value the adapter reports as not-a-number or infinity is treated as a missing sample. Any other value is limited to 100 standard deviations.
- `config.json`: files over 4 KB are ignored, numbers must fall in their documented range, `rev` must fit in 32 bits, and `timezone` may hold only letters, digits and `<>+-,.:/`.
- `receiver.json`: the host, port, token and certificate are checked for length and characters before the board stores them, and the file is deleted afterwards.
- Build: stack protector on every function, a TLS client only with renegotiation off, and no soft-AP or enterprise Wi-Fi code.

These checks stop malformed or hostile data from corrupting logs, the clock or the model input. They do not make a stolen board or card safe.

## Known limits

These are accepted for this release and listed so nobody has to find them by testing.

- The model is not hidden. The board answers real OBD data, so anyone can feed it inputs and read the class and the three estimates. The weights ship inside the firmware image, and the image is not encrypted unless you encrypt your own board (next section).
- The chip protections (flash encryption, Secure Boot, JTAG and ROM download off) are off in the default build. They burn eFuses, which are permanent, can leave a board unable to boot or flash, and would stop you reflashing over USB. You choose them per board.
- Saved Wi-Fi passwords and the pairing token sit in flash as plain text until you turn on flash encryption and NVS encryption. The token also sits on the SD card from the moment the desktop app writes `receiver.json` until the board imports and deletes it at its next start.
- Anyone on your network who has the pairing token can send files to the desktop app as if they were the board. The app checks the token, the board ID format and every file name, stores files only in its own folder, and accepts at most 64 KB per request.
- Whoever holds the board can reflash it over USB, and whoever holds the SD card can read every session.
- The adapter is chosen by its Bluetooth name (`OBD_BLE_NAME_PREFIXES`). A device with a matching name is accepted, and ELM327-style adapters have no pairing security to check. The board reads from it and never sends mode 04.
- The clock check rejects impossible times only. A server that lies by a few hours or days is not detected, because SNTP has no authentication.
- Data from a bad adapter can still be wrong. The firmware limits the model input to finite values, but it cannot tell a lying sensor from a real one.

## Hardening a board you own (optional, permanent)

The default build leaves the board open so you can always reflash it. Espressif's chip protections are not on by default because most of them burn eFuses, which cannot be undone, and a wrong step can leave the board unable to boot or flash. The steps below follow Espressif's [Security Overview](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/security.html). Read it and the linked guides before you try any of this on a board you care about.

| Protection | What it stops | Cost |
|---|---|---|
| Flash encryption, release mode | Reading the firmware, the model and the saved Wi-Fi passwords out of the flash chip. Encrypt the NVS partition as well, or the passwords stay readable. | Permanent. Espressif advises a unique key per board. You can no longer flash a plain image over USB. |
| Secure Boot v2 | Running firmware that you did not sign. Needs your private signing key, which must stay secret. | Permanent. Lose the key and you cannot update the board. |
| JTAG off | Debugging and memory reads through the debug pins. Turns itself off once Secure Boot or flash encryption is on. | Permanent when burned. |
| ROM download mode off, or secure download mode | Loading code through the chip's built-in download mode. | Permanent. In secure download mode, `esptool` only works with `--no-stub`. |

They do not stop someone who has the board from querying it. See the first item under Known limits.

## Supply chain

Releases are built by the `release` workflow from a tagged commit and carry `SHA256SUMS`. Every GitHub Action used by the workflows is pinned to a commit, the ESP-IDF build image is pinned by digest, and the OpenSSF Scorecard workflow reviews the repository weekly. The firmware depends on ESP-IDF, ESP-DL, LVGL, esp_lvgl_port, led_strip and cJSON, fetched by the IDF component manager at build time. Their versions are in `main/idf_component.yml`.

The desktop app is built from pinned GitHub Actions and the npm lockfile for the Tauri command-line tool. Its Rust crates are resolved when it is built, because `Cargo.lock` is not committed yet.

## Supported versions

Only the latest release gets fixes.
