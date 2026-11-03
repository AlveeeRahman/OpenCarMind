# The on-board model

One small convolutional network runs on the ESP32-S3 and answers two questions every time an OBD row arrives. What is the car doing, and what are three things the OBD port does not report?

## Inputs

The input is a window of 30 one-second bins. Ten channels are defined, in this order: rpm, speed, engine load, MAF, throttle, short-term fuel trim, long-term fuel trim, coolant temperature, intake temperature and MAP. Only seven were seen during training: rpm, speed, load, throttle, both fuel trims and coolant temperature. MAF, intake temperature and MAP are always fed as missing, because many cars do not report them (a car without PID 0x0B has no MAP).

The network sees 30 input features per step: the 10 standardised values, 10 masks that say whether a value is present, and 10 differences between neighbouring steps. Standardisation uses the means and standard deviations in `ml/onboard/norm.json`.

The firmware builds the window when a row arrives. The newest bin ends when the row arrives, each earlier bin ends one second before the next, and each takes the latest sample in its second. If a channel has no sample in time, its last value is held for up to 3 seconds (`CARMIND_ML_HOLD_S`), and after that the bin is marked missing.

## Outputs

Manoeuvre, four classes in this order: stopped, braking, accelerating, cruising.

Three estimates, each standardised by the model and converted back with `ml/onboard/targets.json`:

| Estimate | Unit | Mean | Standard deviation |
|---|---|---|---|
| brake pressure | bar | 6.55 | 9.54 |
| flywheel torque | N m | 22.35 | 24.59 |
| fuel rate | L/h | 2.67 | 2.12 |

## How the labels were made

For each 10 second window of a training drive:

- stopped, if the mean speed is below 2 km/h
- braking, if not stopped and the brake switch was on for at least 30 % of the window
- accelerating, if neither of the above and the mean longitudinal acceleration is at least 0.5
- cruising, otherwise

Braking therefore means the driver's foot on the pedal, not slowing down. Each estimate target is the mean of that signal over the same 10 seconds. The model reads the 30 seconds that end with the window, so an estimate describes the last 10 seconds, not all 30.

## Architecture

A 1×1 convolution lifts the 30 features to 48 channels. Six residual blocks follow. Each block has two depthwise-separable convolutions (a kernel-3 filter on every channel, then a 1×1 mix across channels) and a skip connection. The network takes the mean over all 30 steps and the mean over the last 10, joins the two, and passes them through a 64-wide dense layer shared by both heads. The architecture came from an Optuna search over 150 designs, chosen on validation.

It has 38,679 parameters and needs 0.93 million multiply-adds per window. The exported file is 93,088 bytes. Every layer is a standard operation that ESP-DL runs on the chip, so the firmware has no custom kernels.

Training on one head per task scored the same as the shared trunk within noise (manoeuvre 0.896 against 0.895, estimator 0.768 against 0.775 on the earlier test split), so the heads share the trunk.

## Scores

All scores are on held-out data from the same source as the training data: the last 20 % of each drive. The test split was read once.

| Test windows | Manoeuvre macro-F1 | Estimator mean R² |
|---|---|---|
| Complete 1 s windows | 0.911 | 0.830 |
| Windows thinned to mimic a slow adapter | 0.874 | 0.792 |

The version on the chip (8-bit weights, 16-bit activations, exported with ESP-PPQ) scores 0.912 and 0.830 on the 1,878-window split, against 0.911 and 0.830 for the float network. Per target, brake pressure scores R² 0.56, flywheel torque 0.94 and fuel rate 0.99.

How fast the answer follows a true change: on held-out drives, with an answer every second and 618 true transitions, the median lag was 0 seconds and 89 % of transitions were followed within 3 seconds. The labels are defined over the last 10 seconds, so a real change takes at least about 3 seconds to show. Braking needs the pedal down for 30 % of the window.

## Cost on the chip

On the author's car and adapter, one run took about 9 ms on average, with a worst case of 180 ms. On the bench it took about 6 ms. That is about 0.6 % of one core. The numbers come from one setup.

## Limits

- One training source. Another make shifts the input distributions, and no one has measured accuracy on a car outside it. Two Toyota replay drives in `samples/replay` showed the chip agreeing with the float model on 98 % of windows, which checks the conversion and says nothing about accuracy.
- Brake pressure is the weakest estimate.
- The "thinned" windows are a degraded view of the same data. They are not measurements from a slower car.

## Files

| File | What it is |
|---|---|
| `ml/onboard/onboard.espdl` | the model, 8-bit weights and 16-bit activations |
| `ml/onboard/norm.json` | channel order, means, standard deviations and which channels were trained |
| `ml/onboard/targets.json` | class names, estimate names, means and standard deviations |
| `ml/onboard/export.json` | float and quantised scores and file hashes |
| `ml/onboard/bench_summary.json` | on-chip check against the simulator on two replay sessions |
| `main/ml_params.h` | generated from the files above by `tools/gen_ml_params.py` |

## Later models

This project will release more regularised models to fit more cars. They will arrive as new releases with their own `ml/onboard/` files and `main/ml_params.h`. The training data and code are not part of this repository.
