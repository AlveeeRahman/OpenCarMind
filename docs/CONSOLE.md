# USB settings console

The board's USB port takes a few settings commands. Open a serial terminal on it at 115200 baud and press Enter. The prompt is `ocm> `.

Anyone who can plug a cable into the board can use the console, so treat the USB port like the board itself. The console never shows saved Wi-Fi passwords. `wifi list` shows network names only.

## Commands

| Command | What it does |
|---|---|
| `status` | firmware, model hash, uptime, mode, card state, session number, clock, number of saved networks, adapter |
| `wifi list` | saved network names, one per line, with an index |
| `wifi add <ssid> [password]` | add or update a network. Quote names with spaces: `wifi add "Guest net" "pass word 1"` |
| `wifi forget <ssid>` | remove a network |
| `adapter forget` | forget the saved adapter and search again |
| `config show` | the settings below |
| `config set <key> <number>` | change one setting. A restart applies it, so follow with `reboot` |
| `reboot` | restart the board |
| `help` | list the commands |

Settings keys and ranges: `poll_interval_ms` 50 to 5000, `slow_group_every` 1 to 50, `dtc_interval_s` 10 to 3600, `backlight_pct` 5 to 100, `theme_mode` 0 (auto), 1 (dark) or 2 (light), `day_start_hour` and `day_end_hour` 0 to 23.

A Wi-Fi entry needs an SSID of 1 to 32 bytes and a password that is empty or 8 to 64 bytes, with no control characters. The board holds up to 8 networks.

## While driving

Commands that change something (`wifi add`, `wifi forget`, `adapter forget`, `config set`) are refused while the car link is up. `status`, `wifi list` and `config show` always work.

## For programs

Each command prints its answer, then one last line: `OK`, or `ERR <reason>`. The firmware's own log lines start with `I (`, `W (` or `E (` followed by a number, and a program should skip them. An unknown command prints `Unrecognized command` with no `OK` or `ERR`.

## Checked

Run on a real board with firmware 2.0.1: `status`, `wifi list`, `wifi add` (accepted and rejected), `wifi forget`, `config show`, and the rejection of a bad setting and an unknown key. Not checked: the refusal while driving, `adapter forget`, `config set` with a valid value, and `reboot`.
