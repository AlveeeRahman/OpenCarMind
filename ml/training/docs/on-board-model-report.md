# CarMind on-board model: data, architecture and results

One small network now does three jobs on the board. It recognises the manoeuvre in each 10 second window, estimates
three signals the OBD port does not give you, and identifies the driver. On held-out test data it beats the board's
built-in pattern heuristic by 33 macro-F1 points, as an int8 model of 55,025 parameters that needs about 2.4 ms per
window on the ESP32-S31.

| Head | Test metric | Board heuristic | Linear baseline | This model (int8) |
|---|---|---|---|---|
| Manoeuvre | macro-F1 | 0.536 | 0.840 | 0.867 |
| Estimator | mean R² | not applicable | 0.653 | 0.770 |
| Driver | accuracy (10 drivers) | not applicable | 0.331 | 0.525 |

All numbers come from the 1,878 windows in the last 20 % of every drive, scored once after the model was chosen on
validation data.

## Data

Only Kind 1 data goes in. These are decoded OBD-II values at about one sample per second, the same kind the board reads
through its LELink adapter. Kind 2 (raw CAN traffic) never reaches a board on an OBD adapter, so it stays with the cloud
models.

All of it comes from the HCRL Driving Dataset (Kwak, Woo and Kim, PST 2016), which covers one KIA Soul, ten drivers,
94,381 rows and 51 signals. The loader keeps 43 channels and maps them to CarMind's channel names.

Two kinds of channel play different roles.

- Inputs are the 10 channels the board polls: rpm, speed, engine load, MAF, throttle, both fuel trims, coolant, intake
  air temperature and manifold pressure. MAF and intake air temperature do not exist in this car's data, so they enter
  as missing values with a mask of zero, as an unsupported PID does on the board.
- Ground truth comes from CAN-internal signals the board cannot read over OBD: brake pedal state and pressure,
  longitudinal acceleration, flywheel torque and fuel rate.

The windows are the board's own. An exact Python port of `main/pattern.c` runs over every drive in time order, one
online model for all drives as on the board, and every 10 s window it closes becomes one example. That lets the board's
heuristic and the network be scored on identical windows.

Each window gets these labels from the hidden signals:

| Manoeuvre | Rule, in priority order |
|---|---|
| stopped | mean speed under 2 km/h |
| braking | moving, brake pedal pressed in at least 30 % of samples |
| accelerating | mean longitudinal acceleration at least 0.5 m/s² |
| cruising | everything else |

Turning was dropped, because it covered 1 % of windows. The estimator targets are the window means of brake pressure,
flywheel torque and fuel rate.

Two source columns needed checking against speed before use. `accel_long_g` is in m/s² despite its name (slope 0.98
against the change in speed per second), and `brake_switch` reads 1 when released and 2 when pressed.

Every drive is cut by time. Its first 64 % trains, the next 16 % picks the model, and the last 20 % tests. Channel scaling
and target scaling are fitted on training rows only. A window's input never reaches back across the start of its split.

| Split | Windows | stopped | braking | accelerating | cruising |
|---|---|---|---|---|---|
| train | 6,028 | 1,474 | 1,624 | 370 | 2,560 |
| validation | 1,500 | 217 | 358 | 99 | 826 |
| test | 1,878 | 275 | 591 | 72 | 940 |

Test data is genuinely different from training, because the last fifth of every drive is faster than the rest (median speed 31 km/h
against 20 km/h in training, Kolmogorov-Smirnov D = 0.126), so the test asks the model to work on driving it saw less
of.

## Architecture

For each window, the network reads the last 90 samples of the 10 input channels, plus their validity masks and their
sample-to-sample differences: 30 channels in, 90 time steps.

1. A 1×1 convolution mixes the 30 inputs into 96 features per time step.
2. Two residual blocks follow. Each one is a depthwise convolution with kernel 3 (each feature looks at its own
   neighbouring samples), a pointwise 1×1 convolution (features mix), then the same pair again, added back onto the
   block's input.
3. Two summaries come off the trunk: the mean over all 90 steps, and the mean over the last 10 steps, which are the
   window being labelled.
4. The driver head reads the 90-step summary through a 64-wide layer and a softmax over ten drivers.
5. The manoeuvre and estimator heads share a 32-wide layer on both summaries side by side. One output layer gives four
   manoeuvre scores, the other gives three standardised estimates.

The training loss adds the three tasks:

$$
\mathcal{L} = \mathrm{CE}_{\text{driver}} + 0.53\,\mathrm{CE}^{w}_{\text{manoeuvre}} + 1.79\,\mathrm{MSE}_{\text{estimates}}
$$

Each manoeuvre class is weighted by the square root of its inverse frequency, so the 370 accelerating windows
count more than their share. Label smoothing of 0.10 on the driver term and dropout of 0.19 limit overfitting to ten
people.

Training is quantization-aware from the first step, so every weight and activation is rounded to int8 with a
power-of-two step, exactly as the chip will round it:

$$
q = \operatorname{clamp}\!\left(\operatorname{round}\!\left(\frac{x}{\Delta}\right), -128, 127\right)\Delta,
\qquad \Delta = 2^{\left\lceil \log_2 \left( \max|x| / 127 \right) \right\rceil}
$$

ESP-PPQ uses power-of-two steps, so the chip can rescale with a bit shift instead of a multiply.
Rounding has no useful gradient, so the backward pass treats it as the identity (the straight-through estimator). The
reported test scores are from this rounded model.

### How the architecture was found

Optuna 5.0 searched 150 designs in 30 minutes on a MacBook, training each for 25 epochs in Apple MLX. It maximised the
three validation scores together and kept the trade-off front of 21 designs. The space allowed only operators ESP-DL
runs: convolutions (standard or depthwise-separable), ReLU, residual addition, average pooling, concatenation and
linear layers. Any design over 120,000 parameters was rejected before training.

| Searched | Range | Chosen |
|---|---|---|
| context length | 30, 60, 90, 120 s | 90 s |
| block type | standard, depthwise-separable | depthwise-separable |
| blocks | 2 to 6 | 2 |
| width | 16 to 96 | 96 |
| kernel | 3, 5, 7 | 3 |
| dilation | none, doubling | none |
| difference channels | off, on | on |
| driver head | ArcFace, softmax | softmax |

All five best designs used 90 s of context, depthwise-separable blocks and two blocks, so the search converged. The top
five were then retrained for 60 epochs with quantization, three seeds each. Their validation means landed between 0.768
and 0.772, with seed spread of 0.002 to 0.011, so the five are effectively tied. The winner was picked by mean
validation score and tested once.

## Results

Manoeuvre recognition, per class on the test windows:

| Class | Test windows | Precision | Recall |
|---|---|---|---|
| stopped | 275 | 0.932 | 0.993 |
| braking | 591 | 0.832 | 0.778 |
| accelerating | 72 | 0.768 | 0.875 |
| cruising | 940 | 0.878 | 0.887 |

The board's heuristic scores 0.536 macro-F1 on the same windows when each of its 24 patterns is mapped to the
manoeuvre it most often carries. Logistic regression on the heuristic's own 30 features reaches 0.840. The network
reaches 0.867. Most of the gain over the heuristic comes from learning the classes at all, and the network adds 2.7
points over the linear model. Braking is the hard class: telling a driver braking from one coasting needs the pedal,
which OBD does not report.

Estimation, test R² (1.0 is perfect, 0 is predicting the mean):

| Target | Ridge on the heuristic's features | This model |
|---|---|---|
| brake pressure | 0.146 | 0.387 |
| flywheel torque | 0.878 | 0.939 |
| fuel rate | 0.935 | 0.983 |

Brake pressure gains the most, from 0.146 to 0.387. The network reads the speed trace inside the window, where the
linear model saw only window summaries.

Driver identity reaches 0.525 against 0.331 for logistic regression on the same windows, ten drivers, chance 0.100.
The earlier Edge-S result (0.487) used 60 s windows and a different test set, so it is not directly comparable.

Sharing one trunk costs little. The same architecture trained as three single-head models scored driver 0.505,
manoeuvre 0.881 and estimator 0.788 on test, a mean of 0.725 against 0.720 for the shared model. Those single-head runs
used one seed, and the seed spread on validation was up to 0.011, so the difference is within noise. The shared model
moves a third of the bytes of three separate ones.

## Cost on the ESP32-S31

| Per window | Value |
|---|---|
| Parameters | 55,025 (about 55 KB at int8) |
| Multiply-adds | 3.69 million |
| Bytes moved, weights and activations | 135 KB |
| Peak activation memory | 17 KB |
| Compute time | 2.4 ms |
| Data time (from PSRAM at 0.5 GB/s) | 0.27 ms |
| Energy share spent moving data | 97 % |

The board closes one window every 10 s and polls OBD over BLE far slower than 2.4 ms, so the model's speed does not
limit the system. Its cost is bytes: keeping the weights in internal SRAM, loaded once at boot, saves most of the
energy. The compute time assumes 16 int8 multiply-adds per cycle at 320 MHz and 30 % efficiency on the S31's SIMD unit,
which still needs measuring on the chip.

## Limits

- One car. Every number here is for a KIA Soul. Another make or model will shift the input
  distributions, so the board should standardise inputs with that car's own statistics and adjust the manoeuvre head
  for that car's class mix.
- The fuel-rate column's unit is unresolved. Its median is 527 and its peak about 7,565, which cannot be litres per
  hour. The estimate needs the right unit before it is stored.
- Accelerating has 72 test windows, so its F1 moves by several points between runs.
- The new architecture is trained and scored in MLX with simulated int8. It still needs a PyTorch twin, ONNX export
  and an ESP-PPQ run to produce the `.espdl` file, as the earlier Edge-S has.

The model, search and scores are reproducible from the `carmind-ml` repository: `nas_edge.py` for the search and the
final stage, `tasks.py` for the windows and labels, `systems_metrics.py` for the cost figures. The winner's weights are
in `runs/nas/edge_kind1_mt_v1_winner_qat.safetensors` and its scores in `runs/nas/edge_kind1_mt_v1_final_qat.json`.

## Two-head model (2026-10-30)

The owner dropped the driver head, so the network was searched again with two heads only, manoeuvre and estimator.
This is the model that goes on the board. The three-head numbers above remain as the reference.

### Search and test

Optuna searched 150 designs (145 completed, 5 rejected over the parameter budget) in study `edge_kind1_me_v1`. The
top five were retrained with quantization-aware training, three seeds each, and picked on validation. Trial 94 won with
a 3-seed validation mean of 0.887 and a seed spread of 0.001, and the test split was then read once.

| Searched | Chosen |
|---|---|
| context length | 30 s |
| block type | depthwise-separable |
| blocks | 6 |
| width | 48 |
| kernel | 3 |
| dilation | none |
| difference channels | on |
| shared layer before the heads | 64 wide |
| parameters | 38,679 |

The two-head model reads 30 seconds of context against 90 for the three-head one, and it is deeper and narrower.

| Head | Test metric | Board heuristic | Linear baseline | Three-head (int8) | Two-head (int8 QAT) | Same architecture, single head |
|---|---|---|---|---|---|---|
| Manoeuvre | macro-F1 | 0.536 | 0.840 | 0.867 | 0.895 | 0.896 |
| Estimator | mean R² | not applicable | 0.653 | 0.770 | 0.775 | 0.768 |

Per estimator target the two-head model reaches brake pressure 0.410, flywheel torque 0.933 and fuel rate 0.982 on
test. Against the ridge baseline (0.146, 0.878, 0.935) brake pressure again gains the most. The model beats the board
heuristic by 36 macro-F1 points and logistic regression by 5.5 points on the same 1,878 test windows.

Sharing the trunk between the two heads costs nothing measurable. The single-head models score 0.896 and 0.768, against
0.895 and 0.775 for the shared model. They were trained with one seed each, and the seed spread on validation was up to
0.005, so these differences are inside the noise.

Source: `docs/session_logs/final_me_v1_qat.log` and `runs/nas/edge_kind1_me_v1_final_qat.json`, produced by
`nas_edge.py --heads man,est --name edge_kind1_me_v1 --final --qat --single-task --top 5`.

### Export for the ESP32-S3

The command was `python -m carmindformer.export_onboard --weights runs/nas/edge_kind1_me_v1_winner_qat.safetensors
--config runs/nas/edge_kind1_me_v1_final_qat.json --target esp32s3 --quant-type w8a16 --out export/onboard_s3`. It
writes `onboard.espdl` (92,432 bytes), `norm.json`, `targets.json`, `export.json` and test vectors.

`norm.json` carries a `trained` flag per input channel. MAF and intake air temperature are false, because the KIA data
never had them valid, so the board must feed them as missing (value 0, mask 0) whatever the car reports. The other
eight channels are true.

The export script scores the PyTorch twin and ESP-PPQ's quantised simulation on the same 1,878 test windows. This is a
fidelity check of the conversion and plays no part in choosing the model.

| | Manoeuvre macro-F1 | Estimator mean R² | brake, torque, fuel |
|---|---|---|---|
| PyTorch float twin | 0.890 | 0.773 | 0.412, 0.927, 0.981 |
| ESP-PPQ w8a16 simulation | 0.890 | 0.774 | 0.412, 0.928, 0.982 |

Quantising to w8a16 loses nothing against float. The float twin sits 0.005 below the MLX test score of 0.895. A likely
reason is that the twin runs without the fake-quantization rounding the MLX model was trained and scored with, but this
has not been checked.

### On the board

The bench app was built with the new `onboard.espdl` and flashed to the ESP32-S3 (240 MHz, 8 MB octal PSRAM), and
`tools/espdl_bench/bench_host.py` streamed 578 replay windows to it over USB. The boot log is in
`docs/session_logs/bench_s3_twohead_boot.log` and the summary in `docs/session_logs/bench_s3_twohead_summary.json`.

| Session | Car | Seen in training | Windows | Device = simulator (manoeuvre) | Device = float | Estimate diff vs simulator (z, brake, torque, fuel) |
|---|---|---|---|---|---|---|
| s900001 | KIA Soul | yes | 165 | 0.994 | 0.994 | 0.011, 0.014, 0.011 |
| s900002 | Toyota Etios | no | 264 | 0.981 | 0.981 | 0.007, 0.015, 0.011 |
| s900003 | Toyota Etios | no | 149 | 0.980 | 0.980 | 0.012, 0.014, 0.008 |

s900001 is listed for completeness only. Its rows are in the training split, so it does not count as a result. On the
two unseen Toyota sessions the board picks the same manoeuvre as the simulator and the float model on 98 % of windows.
The three-head trunk agreed with float on only 37 % and 60 % of those windows, before the `trained` flag fix. In
physical units the largest estimate difference is about 0.1 bar of brake pressure and 0.4 Nm of torque.

On windows whose label speed alone decides (stopped, accelerating), the board's manoeuvre is right on 94 % for the KIA
(47 windows, seen), 62 % for s900002 (53 windows) and 78 % for s900003 (18 windows). The three-head trunk scored 70 to 78 % on the Toyota before the trained-flag fix, so s900002 is worse with the new model. The Toyota sessions have no ground
truth beyond speed, so these are the only accuracy figures for them. Accelerating was predicted on 7 Toyota windows,
where the three-head model never predicted it.

| Cost on the ESP32-S3 | Value |
|---|---|
| Latency per window, median | 5.24 ms (p95 5.26 ms, max 5.27 ms) |
| Share of the 10 s window | 0.05 % |
| Multiply-adds | 0.93 million |
| Effective throughput | 0.36 GOPS |
| Model in flash | 90.25 KB (51.5 KB parameters) |
| Parameters copied to PSRAM at load | 51.5 KB |
| PSRAM in use after load | 73.25 KB by ESP-DL's summary, 75,008 bytes measured by the app |
| Internal RAM in use after load | 15.8 KB by ESP-DL's summary, 20,876 bytes measured by the app |
| Load time | 50 ms |

The per-layer profile totals 5.46 ms. Convolutions take 3.71 ms of it across 26 layers, the three linear layers 0.72
ms, ReLU 0.39 ms, the residual additions 0.24 ms and the time-mean 0.22 ms. The two-head model runs 2.9 times faster
than the three-head trunk (15.3 ms), which needed 3.69 million multiply-adds.

ESP-DL's built-in self-test fails again. The first manoeuvre score comes out as -1941 against -1875 expected in int16
units, about 3.5 %. This check compares one test input at 1 LSB tolerance, and it is not a success measure. The
replay agreement above is the measure that counts.

### Open points

- The fuel-rate unit of the KIA data is still unknown, so the fuel estimate stays in source units.
- Speed-decidable accuracy on the Toyota (62 % and 78 %) is well below the KIA's 94 %. Per-vehicle standardisation is
  the next thing to test (handoff, next step 3).

## Shared trunk versus one head per task

The same architecture trained as single-head models does not beat the shared two-head model. Test manoeuvre macro-F1
is 0.896 single-head against 0.895 shared, estimator mean R² 0.768 against 0.775. Seeds were not repeated and the
validation seed spread was 0.005 to 0.011, so every difference is inside the noise (the three-head comparison above
shows the same: mean 0.725 single against 0.720 shared). The shared trunk stays because one inference serves both
heads and it moves fewer bytes. A per-task head or a separate network has not been shown to help; it should be
re-tested with several seeds if more estimator targets are added.

## Estimator target scan (2026-11-02)

Which further signals could the estimator output on a car that reports only standard OBD-II? A gradient-boosted probe
(`tools/target_scan.py`) was fitted on 30 s windows of the seven channels almost every OBD-II car supplies (rpm, speed,
load, throttle, STFT, LTFT, coolant; MAP and pedal left out) from the HCRL KIA Soul data, 18,765 windows in 20
driver-path groups, 5-fold cross-validation grouped by driver and path. The score is held-out R² (regression) or AUC
(binary). This probe is not the network, so it shows learnability, not the number the network would reach.

| Target | Score | Reading |
|---|---|---|
| fuel rate | R² 0.972 | already an output |
| flywheel torque | R² 0.855 | already an output (network 0.933) |
| brake pressure | R² 0.433 | already an output (network 0.410) |
| engine torque (actual %) | R² 0.976 | strongly learnable |
| longitudinal acceleration | R² 0.707 | learnable; also helps the manoeuvre head |
| gear | R² 0.669 | KIA gearbox specific; a CVT has no discrete gears |
| pedal position | R² 0.536 | many cars report it directly (PID 0x49) |
| MAP | R² 0.478 | many cars do not support PID 0x0B |
| friction torque | R² 0.437 | weak |
| road gradient | R² 0.379 | weak |
| steering angle, lateral acceleration | about 0 | not observable from these channels |
| fuel cut-off (binary) | AUC 0.985 | strongly learnable; defines a "coasting" class |
| brake switch (binary) | AUC 0.889 | learnable |
| A/C compressor (binary) | AUC 0.813 | learnable, low value |

Yaw rate is not in the dataset. Candidates worth adding: longitudinal acceleration, engine torque and fuel cut-off.

## Field note: sparse polling

A BLE adapter answers one request at a time, about 100 ms each, so a car polled for 30 or more PIDs returns a full row only every
1 to 2 s, and some channels are slower than others. A model trained on dense 1 Hz data sees missing and stale values on the road.
That is why the refinement below trains on sparse-polling windows, why the firmware holds a channel's last value for up to 3 s
(the training loader also carries values forward), and why the model must tolerate a missing MAP channel.

## How the labels and targets are defined (training code)

From `src/carmindformer/tasks.py`, `_targets`, per 10 s pattern window of the HCRL Driving Dataset:

- Manoeuvre: `stopped` if the mean speed is below 2 km/h; else `braking` if the brake switch was on for at least 30 % of
  the window; else `accelerating` if the mean longitudinal acceleration is at least 0.5; else `cruising`. Braking is
  therefore the driver's brake pedal, not deceleration.
- Estimator targets: the mean of brake pressure, flywheel torque and fuel rate over that same 10 s window. The model
  reads the 30 s ending at the window's end, so each estimate describes the last 10 s, not the whole input.
- Any scoring of the estimates against a drive (a future dashboard, say) should compare with the last 10 s of that drive.

## Refinement v1 for a real car (2026-11-02, firmware 2.0.1)

The model on the board from 2.0.1 (hash `85d63ec33020`) is the shipped network (same architecture, 38,679 parameters,
93,088 B) fine-tuned on the Mac with `train_onboard.py` (MLX on the GPU, 15 minutes
for 3 seeds of 60 epochs). Same data as before, used differently:

- Dense windows: one per 1 s step instead of one per 10 s pattern window, 60,214 train / 14,921 val / 18,705 test windows
  (was 6,028 / 1,500 / 1,878), same 64/16/20 split of each drive, same targets (`tasks._targets`).
- MAP removed (`CARMIND_DROP_CHANNELS=map_kpa`) so the exporter marks it `trained=false`; many cars do not support PID 0x0B.
- Sparse-polling augmentation: half of the training windows are re-sampled as a slow poller would see them (fast channels
  together, slow channels on their own) with the last value held for 3 s, as the firmware does.
- Warm start from the shipped QAT winner, validation scored on clean and sparse-polling windows, test split read once.

| Test windows | Manoeuvre macro-F1, shipped to refined | Estimator mean R², shipped to refined |
|---|---|---|
| Clean (MAP removed) | 0.886 to 0.911 | 0.777 to 0.830 |
| Sparse polling (about 0.6 Hz fast channels, 3 s hold) | 0.800 to 0.874 | 0.645 to 0.792 |

Per target on clean windows: brake pressure 0.419 to 0.559, flywheel torque 0.933 to 0.944, fuel rate 0.978 to 0.987. The
w8a16 export scores 0.912 and 0.830. "Sparse polling" is a degraded view of the KIA data, not accuracy on a real car of another make, and the
test split is the last 20 % of the same drivers and route.

How fast the answer follows a true change (held-out drives, answers every second, 618 transitions): median lag 0 s and
89 % followed within 3 s for the refined model (87 % shipped), against a median of 4.0 s and 40 % within 3 s when the
model answers once per 10 s as the earlier 10 s design did. The lag is measured against the label, which is itself defined over the last
10 s, so a real change takes at least about 3 s to show (braking needs the pedal pressed for 30 % of the window).
Answering every second does not make the label flicker much: 3.8 flips per minute against 3.2 in the true labels, and a
spurious blip of 2 s or less about 0.6 times per minute. Smoothing the output cut blips but cost accuracy and lag, so the
logged output is not smoothed.

Not done: new estimator targets, consistency training on unlabelled cars, and any measurement on a real drive in another make of car.

