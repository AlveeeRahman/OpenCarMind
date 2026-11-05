# The on-board models: CarMind E.V and CarMind R.E.V

The firmware carries two models and runs one at a time. CarMind E.V is the default, and most of this page is about it. [CarMind R.E.V](#carmind-rev) is a smaller model that keeps learning your car's fuel use on the board.

One highly generalized and small convolutional network runs on the ESP32-S3 and answers two questions every time an OBD row arrives. What is the car doing right now, and what are three things the OBD port does not report? This page is for people who use the board. The model is a small quantised network run by ESP-DL. It takes about 89 KB on the chip and about 3.5 ms per run.

## What it reads

The model looks at the last 5 seconds, cut into five one-second bins. Ten channels are defined, in this order: rpm, speed, engine load, MAF, throttle, short-term fuel trim, long-term fuel trim, coolant temperature, intake temperature and MAP. The model reads the first nine. MAP is always fed as missing (`trained` is false for it in `ml/onboard/norm.json`).

Cars report different PIDs, so the model is told which values are present. A channel the car does not report is marked missing, and the model still answers. Rpm and speed matter most, so keep them in your adapter's PID list.

The firmware builds the window when a row arrives. The newest bin ends when the row arrives, each earlier bin ends one second before the next, and each takes the latest sample in its second. If a channel has no sample in time, its last value is held for up to 3 seconds (`CARMIND_ML_HOLD_S`), and after that the bin is marked missing. At 1.4 s per row, a 5 s window holds about three rows.

## What it answers

Manoeuvre, one of four classes in this order: stopped, braking, accelerating, cruising.

Three estimates:

| Estimate | Unit | `.ml.csv` column |
|---|---|---|
| brake pressure | bar | `est_brake_pressure_bar` |
| flywheel torque | N m | `est_flywheel_torque_nm` |
| fuel rate | L/h | `est_fuel_rate_lph` |

The firmware uses the fuel estimate only when the car reports neither a fuel-rate PID nor MAF. It is then shown in amber.

## Where the answers appear

- Page 5 of the board shows what the model sees and its three estimates for the newest window.
- Every row adds one line to the drive's `sNNNNNN.ml.csv` file on the SD card. The columns are `window_seq`, `start_ms`, `end_ms`, `model`, `manoeuvre`, `p_stopped`, `p_braking`, `p_accelerating`, `p_cruising`, `est_brake_pressure_bar`, `est_flywheel_torque_nm`, `est_fuel_rate_lph`, `latency_us`, `queue_ms` and `model_name` (`ev` or `rev`), followed by the R.E.V columns, empty with E.V. The `p_` columns are the class probabilities. The `model` column holds the first 12 hex digits of the model file's hash, so every answer can be traced to the exact weights. [CONFIGURATION.md](CONFIGURATION.md) has the rest of the file layout.

## Scores

Scores of the version on the chip, on data held out from training:

| Test set | Manoeuvre | Fuel R² |
|---|---|---|
| Public KIA drives | macro-F1 0.887 | 0.947 |
| Two drives of my car | 98.5 % in an allowed class | 0.884 |
| 37 VED cars the model never saw | 96.1 % in an allowed class | 0.940 (0.688 with MAF hidden) |

"In an allowed class" means the answer is one of the classes the labels allow for that window. Where there is no brake switch, "braking or cruising" counts as correct either way. The VED score with MAF partly measures the fuel formula, because the target is computed from MAF. The "MAF hidden" score is the one that matters, since that is when the board uses the estimate. Brake pressure (R² 0.46) is the weakest estimate.

## Limits

- Outside one data set there is no brake-switch signal, so braking and cruising are partly ambiguous.
- No diesel car is in the data.
- Brake pressure is the weakest estimate.
- The fuel estimate on MAF cars has not been checked against a measured fuel rate.
- The road numbers for this model are not measured yet.

## CarMind R.E.V

R.E.V reads the same inputs and gives the same answers as E.V, from a model about half the size (about 64 KB on the chip). What it adds is learning on the board, for fuel only:

- While the car reports its fuel rate or its airflow, R.E.V checks how its own fuel estimate would have done without the airflow reading, and slowly adjusts that estimate to your car. A change is kept only if it does better on the most recent part of the drive, and it goes back to the factory setting if it ever does worse.
- On cars that report engine load, it also learns how much air your engine takes in, which gives a second fuel estimate.
- When the airflow reading is missing, the board uses whichever estimate has been closest on your car. It is shown in amber, like every estimate.
- Fill-ups: you enter the fuel you pump in the desktop app's Fuel page (litres or gallons, filled to the click-off or not), or the car's trip-computer average. The board compares that with the fuel it counted between full tanks and corrects all its fuel figures by one factor. This works with E.V too.

Switch models on the desktop app's Board page, over the USB cable. The choice is kept after a restart. What R.E.V learned is saved on the SD card (`obd/rev_state.bin`) and kept when you switch back to E.V. In safe mode nothing learns, and the fill-up correction stays as it was.

R.E.V is new. It has not been checked on a car yet, so E.V stays the default. With R.E.V, every `.ml.csv` line also has the measured fuel over the last 5 s, the factory and learned estimates with the airflow reading hidden, and the learning counters; every main CSV row has the fuel rate before the fill-up correction (`fuel_lph_raw`), the factor (`fuel_cal`) and the uncorrected trip total (`trip_fuel_raw_l`). A forecast of the next moment's manoeuvre is planned for a later R.E.V.

## Files

| File | What it is |
|---|---|
| `ml/onboard/onboard.espdl` | the model |
| `ml/onboard/norm.json` | channel order, means, standard deviations and which channels were trained |
| `ml/onboard/targets.json` | class names, estimate names, means and standard deviations |
| `ml/onboard/export.json` | model name, file sizes and hashes, target chip and quantisation type |
| `main/model/ml_params.h` | generated from the files above by `tools/gen_ml_params.py` |
| `ml/onboard/rev/` | the same four files for CarMind R.E.V |
| `main/model/ml_params_rev.h` | generated from them by `tools/gen_ml_params.py ml/onboard/rev main/model/ml_params_rev.h MLR` |

## Data

Trained on public data, the HCRL Driving Dataset (Kwak, Woo and Kim, PST 2016) and the Vehicle Energy Dataset (Oh, LeBlanc and Peng, 2020, Apache-2.0), plus drives from my own car. The full model description is kept privately.
