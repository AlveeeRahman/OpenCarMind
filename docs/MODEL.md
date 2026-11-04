# The on-board model: Edge Model 1

One small convolutional network runs on the ESP32-S3 and answers two questions every time an OBD row arrives. What is the car doing right now, and what are three things the OBD port does not report? Edge Model 1 is the first OpenCarMind model trained on more than one car. [MODEL_CASE_STUDY.md](MODEL_CASE_STUDY.md) has every variant we trained on the way here and how each one scored.

## Inputs

The input is a window of 5 one-second bins. Ten channels are defined, in this order: rpm, speed, engine load, MAF, throttle, short-term fuel trim, long-term fuel trim, coolant temperature, intake temperature and MAP. The model was trained on the first nine. MAP is always fed as missing (`trained` is false for it in `ml/onboard/norm.json`).

No single training source has every channel. KIA drives (public) lack MAF and intake temperature, VED cars report only rpm, speed, MAF and fuel trims, and the author's car reports all nine. During training, whole channels were also switched off at random (15 % each, rpm and speed always kept), so a car that reports fewer PIDs gets a model that has seen that case.

The network sees 30 input features per step: the 10 standardised values, 10 masks that say whether a value is present, and 10 differences between neighbouring steps. Standardisation uses the means and standard deviations in `ml/onboard/norm.json`. MAF is scaled on the VED fleet and intake temperature on the author's car, since the KIA drives have neither.

The firmware builds the window when a row arrives. The newest bin ends when the row arrives, each earlier bin ends one second before the next, and each takes the latest sample in its second. If a channel has no sample in time, its last value is held for up to 3 seconds (`CARMIND_ML_HOLD_S`), and after that the bin is marked missing. At 1.4 s per row, a 5 s window holds about three rows.

## Outputs

Manoeuvre, four classes in this order: stopped, braking, accelerating, cruising.

Three estimates, each standardised by the model and converted back with `ml/onboard/targets.json`:

| Estimate | Unit | Mean | Standard deviation |
|---|---|---|---|
| brake pressure | bar | 6.55 | 9.54 |
| flywheel torque | N m | 22.35 | 24.59 |
| fuel rate | L/h | 2.67 | 2.12 |

The firmware uses the fuel estimate only when the car reports neither a fuel-rate PID nor MAF. It is then shown in amber.

## How the labels were made

Every label and every estimate target covers the last 5 seconds.

- stopped, if the mean speed is below 2 km/h
- braking, if not stopped and the brake switch was on for at least 30 % of the window
- accelerating, if neither of the above and the mean longitudinal acceleration is at least 0.5 m/s²
- cruising, otherwise

Only the KIA drives have a brake switch. On the VED fleet and the author's car, braking is labelled from speed: a deceleration of at least 0.3 m/s² over the window is braking, and a moving window that is neither braking nor accelerating carries the set label "braking or cruising". The loss then asks only for one of the two (partial-label learning). We measured the speed rule against the KIA brake switch before using it: at 0.3 m/s² it is right 92 % of the time it says braking, but it catches only a third of real brake presses, which is why it is not used as a hard label.

The fuel target is measured on the KIA. Elsewhere it is MAF / (14.7 × λ) × 3600 / 745 L/h, with λ the commanded equivalence ratio (PID 0x44) where the car reports it and 1 otherwise. No car in our data logs both MAF and a measured fuel rate, so this formula has not been checked against a measurement.

## Architecture

A 1×1 convolution lifts the 30 features to 48 channels. Six residual blocks follow. Each block has two depthwise-separable convolutions (a kernel-3 filter on every channel, then a 1×1 mix across channels) and a skip connection. The network takes the mean over all 5 steps and the mean over the last 3, joins the two, and passes them through a 64-wide dense layer shared by both heads. The layer sizes came from an Optuna search over 150 designs, chosen on validation. Because the network pools over time, the same weights work for any window length, which is how the window went from 30 s to 10 s to 5 s without a new search.

It has 38,679 parameters and needs about 0.16 million multiply-adds per window. The exported file is 89,152 bytes. Every layer is a standard operation that ESP-DL runs on the chip, so the firmware has no custom kernels. A temperature of 1.05, fitted on validation data, is folded into the class layer so the class probabilities are calibrated.

## Training

Quantisation-aware from the first step: every forward pass rounds weights and activations to the int8 grid with power-of-two scales, the scheme ESP-DL uses. Each batch mixes the three sources (about 55 % KIA, 30 % VED, 15 % the author's car). The KIA drives are replayed through the author's real row spacing (median 1.45 s, gaps over 3 s) and binned exactly as the firmware does. Augmentation imitates other cars: whole sensors switched off, a random gain per channel in raw units (±25 % for MAF and load, which covers engine size, ±8 % elsewhere), and thinned slow channels. Three seeds were trained, and the one with the best validation score ships.

## Scores

All test data was held out from training and model selection. The VED test set is 37 whole vehicles the model never saw. The author's test set is two whole drives.

| Test set | Manoeuvre | Fuel R² |
|---|---|---|
| Public KIA drives, last 20 % of each drive | macro-F1 0.887 | 0.947 (brake pressure R² 0.46, torque 0.92) |
| Author's car, two held-out drives | class in the allowed set 98.5 %, macro-F1 0.989 on unambiguous windows | 0.884 |
| 37 unseen VED cars (26 petrol, 10 hybrid, 1 plug-in) | class in the allowed set 96.1 %, macro-F1 0.966 on unambiguous windows | 0.940 with MAF, 0.688 with MAF hidden |

Scores are for the int8 file on the chip. It gives the same class as the float network on 99.9 % of KIA windows, 100 % of the author's and 99.9 % of VED's. The VED fuel score with MAF partly measures the formula above, since the target is computed from MAF. The "MAF hidden" score is the one that matters, because that is when the board uses the estimate.

How fast it reacts: on held-out KIA drives with 1,498 manoeuvre changes, the median time from a label change to the model's first matching answer is 0 s, the mean 1.5 s, and 88 % of changes are matched within 3 s. The previous 10 s model, on the same labels, had a median of 2.8 s and matched 55 % within 3 s. The label itself needs the pedal down for 30 % of 5 s, so a real brake press shows about 1.5 s after the foot goes down.

## Cost on the chip

One run takes about 3.5 ms on the ESP32-S3 (measured at the boot self-test). Loading takes 61 ms and 192 bytes of internal RAM. The road numbers for this model are not measured yet.

## Limits

- The labels outside the KIA drives are partial: without a brake switch, "braking or cruising" is often all the data can say.
- Brake pressure is the weakest estimate, and only the KIA drives teach it.
- The fuel formula on MAF cars is unchecked against a measured fuel rate.
- The VED test set has one plug-in hybrid. No diesel car is in any data set.
- ESP-DL's own boot test reports "output does not match" at one-step precision. The S3 drifts about 2 % inside the network, so the firmware checks the output to within that drift instead (`main/ml_infer.cpp`).

## Files

| File | What it is |
|---|---|
| `ml/onboard/onboard.espdl` | the model, 8-bit weights and 16-bit activations |
| `ml/onboard/norm.json` | channel order, means, standard deviations and which channels were trained |
| `ml/onboard/targets.json` | class names, estimate names, means and standard deviations |
| `ml/onboard/export.json` | float and int8 scores on all three test sets, the temperature, and file hashes |
| `main/ml_params.h` | generated from the files above by `tools/gen_ml_params.py` |

## Data

- HCRL Driving Dataset (KIA Soul drives, public), the source of the brake switch, brake pressure and torque labels.
- Vehicle Energy Dataset (VED), by Oh, LeBlanc and Peng, IEEE Transactions on Intelligent Transportation Systems, 2020, under Apache-2.0: 384 vehicles around Ann Arbor, of which 325 trained, 19 validated and 37 tested the model. EVs were left out.
- Twelve drives from the author's car, logged by this board. They are not published.

The training code is not part of this repository. If you want to try an architecture, see the call at the end of [MODEL_CASE_STUDY.md](MODEL_CASE_STUDY.md).
