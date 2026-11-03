# CarMind ML as a system: the metrics we hold it to

The on-board model is judged as part of a system, not only by its accuracy. This file applies the ML-systems
frameworks (Iron Law, data/algorithm/machine bottleneck, stage contracts, data-centric checks) to CarMind and records
the numbers we measured. `src/carmindformer/systems_metrics.py` computes the cost side for any candidate model.

## 1. Where the bottleneck is (Iron Law on the ESP32-S31)

T = D_tot / BW + O / (R_peak · η) + L_lat, and the energy tax E ≈ D_tot · E_move + MACs · E_mac.

| Per 10 s window | Edge-S today | Larger search candidate (W 90, C 96, depthwise) |
|---|---|---|
| Multiply-adds | 1.89 M | 5.42 M |
| Bytes moved, D_tot | 58 KB | 186 KB |
| Arithmetic intensity | 65 ops/byte | 58 ops/byte |
| Data term (weights and activations in PSRAM) | 0.12 ms | 0.37 ms |
| Compute term | 1.23 ms | 3.53 ms |
| Energy, data vs compute | 9.3 µJ vs 0.38 µJ | 29.7 µJ vs 1.08 µJ |

- Machine balance is about 6 ops/byte (assumed 10.2 GOPS peak, η = 0.3, 0.5 GB/s PSRAM), so time is compute-bound.
- Energy is data-bound: moving a byte costs about 25 times the arithmetic. Keep weights in internal SRAM, loaded
  once at boot, and never reload models per window.
- The board runs a stream, one window per 10 s, and OBD polling over BLE takes far longer than the model. By Amdahl's
  law a faster model buys nothing, so latency is not an objective. The constraints that matter are bytes (energy) and
  peak activation memory (SRAM fit).
- Latency budget: tiny-ML class, under 10 ms per window, deadline 10 s. Both models above meet it with a wide margin.

The S31 clock, SRAM and PSRAM figures come from Espressif's S31 page. The 16 int8 MACs per cycle is an assumption about
its 128-bit SIMD unit and must be measured on the chip.

## 2. Stage contracts

Hardware constraints propagate backward: 512 KB SRAM and ESP-DL's operator set decided the int8 quantization, the
operator list in the search, and the normalisation contract.

| Stage | Deliverable | Invariant we check |
|---|---|---|
| Problem | three heads with measurable scores: driver accuracy, manoeuvre macro-F1, estimator mean R² | scores fixed before the search; test split read once |
| Data | Kind 1 windows from the board's own pattern layer (`tasks.windows_for`), chronological 64/16/20 split | normalisation and target scaling fitted on training rows only |
| Model | `.espdl` plus `norm.json`, window length, channel order, mask meaning, `export.json` | float vs int8 accuracy recorded on the same test windows |
| Deployment | firmware applies `norm.json` and runs one inference task fed by a message buffer | the exported preprocessing matches the training preprocessing |

The model artifact is four coupled pieces: weights, inference code including preprocessing, environment (MLX 0.32,
ESP-PPQ 1.3.13, ESP-IDF 6.1) and configuration. `export.json` records the source weights and accuracies. It should also
record the library versions and a hash of `norm.json`.

## 3. Data checks, measured on the KIA Soul windows

**Sampling frame.** The data is a convenience sample: one car, ten drivers, Seoul routes, 2015 and 2016. The
deployment population is other cars (other makes and models). Expect covariate shift (different
rpm, speed and load relationships per car) and prior shift (a different mix of braking and stopping). Per-car
normalisation addresses the first; adjusting the manoeuvre head's output by the car's class priors addresses the second.

**Class imbalance.** Absolute counts per split:

| Split | stopped | braking | accelerating | cruising |
|---|---|---|---|---|
| train (6,028) | 1,474 | 1,624 | 370 | 2,560 |
| val (1,500) | 217 | 358 | 99 | 826 |
| test (1,878) | 275 | 591 | 72 | 940 |

Accuracy hides the rare class, so the manoeuvre score is macro-F1, and the search can weight the loss by class
frequency (cost-sensitive learning). With 72 test windows, the accelerating class's F1 moves by several points between
runs, so report per-class precision and recall with the headline number.

**Leakage.** All preprocessing (channel normalisation, estimator target scaling) is fitted on training rows only. No
resampling is done. A window's context never crosses into the previous split part.

**Outliers in estimator targets** (Tukey fences on training windows): brake pressure 12.3 % outside (a long tail of hard
braking up to 83 bar), flywheel torque 4.5 %, fuel rate 4.7 %. These are real events, not errors, so they stay, but R²
is sensitive to them.

**Fuel-rate unit (resolved 2026-10-30).** The KIA column mapped to `fuel_rate_lph` (median 527, peak near 7,565 per
window) is in µL/s, not L/h. `tools/check_fuel_unit.py` compares it with a speed-density estimate from MAP and rpm:
r = 0.93, median ratio 0.83 over 64,517 firing rows, idle 320 (1.15 L/h), fuel cut-off 0. `channels.HCRL_UNIT_SCALE`
now converts it on load (x 0.0036). The exported `targets.json` of the deployed model was scaled by the same factor,
which is exact because the estimate is de-standardised linearly.

**Distribution shift and monitoring.** Train rows vs test rows, two-sample Kolmogorov-Smirnov:

| Channel | D | Train median | Test median |
|---|---|---|---|
| speed_kph | 0.126 | 20 | 31 |
| engine_load_pct | 0.094 | 42.4 | 38.4 |
| map_kpa | 0.091 | 39 | 33 |
| rpm | 0.087 | 1,391 | 1,585 |
| coolant_c | 0.087 | 85 | 84 |
| ltft_b1_pct | 0.067 | 3.1 | 3.1 |
| throttle_pct | 0.042 | 7.5 | 8.0 |
| stft_b1_pct | 0.041 | 0.0 | 0.0 |

Every p-value is below 1e-20 because there are tens of thousands of rows, so the textbook rule "alert when p < 0.05"
would fire on everything. Monitor the effect size D instead, with a threshold set from these within-car numbers (for
example alert above D = 0.2 on any channel). The chronological test split is itself shifted (the last fifth of each drive
is faster), which makes it a harder and more honest test than a random split.

**Degradation.** Accuracy(t) ≈ Accuracy₀ − λ · divergence(P_t ‖ P₀). The board's novelty score and the per-channel D
give the divergence signal; a sustained rise is the retraining trigger.

## 4. Return on compute

The search spends minutes on a laptop, not GPU days, so its return on compute is high when it beats the cheap
baselines on the same split: logistic regression reaches 0.840 manoeuvre macro-F1 and ridge reaches R² 0.878 (torque)
and 0.935 (fuel). A candidate that does not beat those on validation is not worth its bytes on the chip.
