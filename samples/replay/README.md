# Replay sessions from a public driving dataset

Two sessions in the exact format an OpenCarMind board writes to its SD card, built from the public carOBD dataset. Use them
to try the file format, the training loaders and your own dashboards without driving.

| Session | Car | Source log | Driving | Rows | Rate | Pattern windows |
|---|---|---|---|---|---|---|
| `s900002` | Toyota Etios 2014, 1.5 L | carOBD `drive10.csv` | roads, up to 69 km/h | 5,454 | about 2 Hz | 264 |
| `s900003` | Toyota Etios 2014, 1.5 L | carOBD `ufpe12.csv` | university campus, up to 42 km/h | 3,075 | about 2 Hz | 149 |

Each session has three files, as on the card:

- `sNNNNNN.csv`: the board's 88-column session CSV. The header is byte-identical to `CSV_HEADER` in `main/storage/sdlog.c`.
- `sNNNNNN.pat`: one row per closed 10 s pattern window, written by a port of `main/analysis/pattern.c`.
- `sNNNNNN.json`: session metadata. Its `replay` block names the source, the vehicle, the citation and the column
  mapping, so a replay can never be mistaken for a drive this board recorded.

`SHA256SUMS` lists the checksums: `cd samples/replay && shasum -a 256 -c SHA256SUMS`.

## Using them

On a board: copy the six files into `obd/sessions/` on the SD card. Copy only these files. Never add a
`patterns.json`, because the board loads that file as its own pattern model. Session numbers from 900001 up stay clear
of the board's own numbering.

On a computer: the CSVs open in any spreadsheet or pandas.

## How they were made

| Script | Does |
|---|---|
| `make_replay_carobd.py` | carOBD CSV to session CSV and JSON |
| `make_patterns.py` | adds the `.pat` file and the board's `window_seq` column to a session CSV |

- Only signals with the same unit as the board's column are mapped. Neither car reports fuel rate or MAF. Those columns stay empty, as they would for a PID a car does not support.
- carOBD rows have no clock. The engine run time (PID 0x1F) repeats for consecutive rows, so rows are spread evenly
  inside each run-time second. That gives about two rows per second.
- The Toyota fuel level swings between 20 and 84 % within a drive. That is sensor slosh in the source data, kept
  as recorded.
- Timestamps were shifted to 2026-10-30 when the files were made. Pattern ids come from a fresh model per session,
  so they cannot be compared across sessions.
- The metadata carries a placeholder board id (`000000000000`), not a real device.

## Sources and terms

- carOBD, <https://github.com/eron93br/carOBD>. Described by its author as "free to everyone", with the request
  to cite "Detecting Anomalies in the Engine Coolant Sensor Using One-Class Classifiers",
  <https://ieeexplore.ieee.org/abstract/document/8891367>.

These files are derived from that dataset and carry the same terms.
