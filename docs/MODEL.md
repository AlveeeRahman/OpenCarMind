# The on-board model: CarMind E.V

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
- Every row adds one line to the drive's `sNNNNNN.ml.csv` file on the SD card. The columns are `window_seq`, `start_ms`, `end_ms`, `model`, `manoeuvre`, `p_stopped`, `p_braking`, `p_accelerating`, `p_cruising`, `est_brake_pressure_bar`, `est_flywheel_torque_nm`, `est_fuel_rate_lph`, `latency_us` and `queue_ms`. The `p_` columns are the class probabilities. The `model` column holds the first 12 hex digits of the model file's hash, so every answer can be traced to the exact weights. [CONFIGURATION.md](CONFIGURATION.md) has the rest of the file layout.

## Scores

Scores of the version on the chip, on data held out from training:

| Test set | Manoeuvre | Fuel R² |
|---|---|---|
| Public KIA drives | macro-F1 0.887 | 0.947 |
| Two drives of the author's car | 98.5 % in an allowed class | 0.884 |
| 37 VED cars the model never saw | 96.1 % in an allowed class | 0.940 (0.688 with MAF hidden) |

"In an allowed class" means the answer is one of the classes the labels allow for that window. Where there is no brake switch, "braking or cruising" counts as correct either way. The VED score with MAF partly measures the fuel formula, because the target is computed from MAF. The "MAF hidden" score is the one that matters, since that is when the board uses the estimate. Brake pressure (R² 0.46) is the weakest estimate.

## Limits

- Outside one data set there is no brake-switch signal, so braking and cruising are partly ambiguous.
- No diesel car is in the data.
- Brake pressure is the weakest estimate.
- The fuel estimate on MAF cars has not been checked against a measured fuel rate.
- The road numbers for this model are not measured yet.

## Files

| File | What it is |
|---|---|
| `ml/onboard/onboard.espdl` | the model |
| `ml/onboard/norm.json` | channel order, means, standard deviations and which channels were trained |
| `ml/onboard/targets.json` | class names, estimate names, means and standard deviations |
| `ml/onboard/export.json` | model name, file sizes and hashes, target chip and quantisation type |
| `main/ml_params.h` | generated from the files above by `tools/gen_ml_params.py` |

## Data

Trained on public data, the HCRL Driving Dataset (Kwak, Woo and Kim, PST 2016) and the Vehicle Energy Dataset (Oh, LeBlanc and Peng, 2020, Apache-2.0), plus drives from my own car. The full model description is kept privately.
