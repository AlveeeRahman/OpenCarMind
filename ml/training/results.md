# Results so far (Mac, Apple MPS; the full CUDA runs are still to come)

## Baselines, real data

| Task | Baseline | Score | Split |
|---|---|---|---|
| Driver identity, HCRL Driving (10 drivers) | logistic regression on per-window mean and std of 40 shared channels, 128 s windows | accuracy 0.567 (chance 0.100) | train on path 1 runs (700 windows), test on path 2 runs (742 windows) |
| CAN attack, DoS | AUROC of total message rate per 100 ms bin | 0.065, i.e. 0.935 with the sign flipped: bins under attack carry fewer captured frames | first 1.5 M frames of each file |
| CAN attack, Fuzzy | AUROC of payload byte entropy | 0.998 | same |
| CAN attack, RPM spoofing | best of total rate, unique ids, entropy | 0.495 / 0.228 / 0.330: the three summary channels do not see it; the per-ID rate channels must | same |
| CAN attack, gear spoofing | same | 0.482 / 0.221 / 0.070 | same |

The spoofing rows are the interesting ones: a flood is trivial to detect from totals, a spoof of one ID is not,
and that is where a learned model has to earn its place.

## CarMind-Edge on the HCRL Driving Dataset

| Item | Value |
|---|---|
| Parameters | 47,787 |
| Training | 20 epochs x 300 steps, batch 128, 64 s windows, 8 of 10 edge channels present (no MAF, no intake temp) |
| Reconstruction loss | 1.03 -> 0.409 (standardised units; 1.0 = predicting the window mean) |
| Change loss | 0.08 -> 0.002 |
| Time | 14 s per epoch on Apple MPS |
| Export | `edge.onnx` 197 KB fp32 (opset 13), 24 embedding prototypes with counts 10 to 40 over 512 calibration windows |

Not yet measured: whether the 16-d embedding separates idle, cruise and acceleration better than the on-device
k-means, and the int8 accuracy drop after ESP-PPQ.

## CarMind-Former, reduced config on real data (pipeline check only)

`configs/former_dev.yaml`: 431 k parameters, window 128, 3 blocks, 4 epochs x 150 steps, batch 32, on Apple MPS,
trained on the 20 Driving runs plus 45 CAN series (first 400 k frames of each Car-Hacking file and the 40 candump logs).
Total loss 4.09 -> 2.89; reconstruction 0.58 to 0.65, attack head 0.055, contrast 0.19, driver 2.1 (barely below ln 10).

Probes from `eval.py` on that checkpoint:

```
driver-id linear probe accuracy: 0.320 (10 drivers in train, 10 in test; chance 0.100)
change head correlation with |patch delta|: -0.384
CAN attack AUROC per patch, attack head: 0.683
CAN attack AUROC per patch, change head (unsupervised): 0.517  (attacked patches: 0.69)
```

CAN metrics are computed only on the four labelled Car-Hacking files, so negatives are normal bins of the same car;
an earlier version that counted the unlabelled candump logs as negatives scored 1.000 on the change head, which was the
model telling two cars apart, not attacks from traffic. Read against the baselines above: the attack head beats the
summary-feature baseline on spoofing after four short epochs; the driver probe (0.32) is above chance but below the
0.567 logistic baseline, as expected for an untrained-to-convergence 400 k model with contrast collapsing early; the
change head has not learned anything useful yet. These are the numbers the full CUDA run must move.

## Supervised track (2026-10-29, Mac; smoke runs only, full MLX run was in progress at the usage cut-off)

Protocol: every run/file cut chronologically 64/16/20 train/val/test; test scored once at the end.

| Task | Model | Run | Val | Held-out test |
|---|---|---|---|---|
| Driver id, 8 OBD channels | logistic regression (mean/std/abs-diff per 60 s window) | holdout | | 0.408 |
| Driver id, 8 OBD channels | logistic regression | session (path 1 -> path 2) | | 0.508 |
| Driver id, all 43 channels | logistic regression | session | | 0.591 |
| Driver id | Edge-S QAT, PyTorch MPS, 3 epochs x 100 steps | holdout | 0.402 | 0.267 |
| Driver id | Edge-S QAT, MLX Metal, 3 epochs x 100 steps | holdout | 0.333 | 0.313 |
| Driver id | Edge-S QAT, 4 epochs x 150 steps | random (leaky) | | 0.679 |
| CAN attack type | MLM d=128, 4 layers, 50 pretrain + 2 x 100 steps, 200 k frames per file | holdout | acc 0.968 | F1 normal 0.999, DoS 0.989, fuzzy 0.955, gear 0.988, RPM 0.994; acc 0.990 |

Literature targets: TEMPEST (arXiv:2610.06855) on the same KIA data, 15 CAN-internal signals, 720 k params: 79-94 %
chronological, 41-66 % cross-session. KD-XVAE (arXiv:2410.09043) on Car-Hacking: F1 1.000 all four attacks, 1,669 params.

Full MLX runs, 40 epochs x 200 steps, batch 128, 60 s windows, Apple Metal:

| Run | Best val | Held-out test | Baseline (logistic, same channels) |
|---|---|---|---|
| Edge-S QAT, holdout | 0.554 | 0.487 | 0.408 |
| Edge-S float twin, holdout | 0.543 | 0.488 | 0.408 |
| Edge-S QAT, cross-session (path 1 -> path 2) | 0.346 | 0.534 | 0.508 |

QAT costs nothing (0.487 vs 0.488 float), and the 34 KB model beats the logistic baseline by 8 points on holdout
and 2.6 on cross-session, with only 8 OBD channels; TEMPEST's 62-66 % cross-session used 15 CAN-internal signals
(steering, brake and so on) that OBD does not expose. Closing that gap is the next experiment: longer windows
(TEMPEST's are not stated in the passages read), wider model, and the same run on the CUDA box with 60 x 300 steps.
Weights: `runs/supervised_mlx/*.safetensors` (git-ignored); export with `export_edge_sup.py --mlx-weights`.

## Small model on the Mac, end to end (2026-10-30, `sh scripts/train_mac.sh`, 1 min 52 s on Apple Metal)

Kind 1 only. Same seed as the runs above, so the float numbers reproduce exactly.

| Step | Holdout test (600 windows) | Cross-session test |
|---|---|---|
| Logistic regression baseline | 0.408 | 0.508 |
| Edge-S, QAT, float | 0.487 | 0.534 |
| Edge-S after ESP-PPQ, `esp32s31`, w8a8, quantised graph simulated | 0.485 | not exported |

The int8 model loses 0.2 points against float: quantization-aware training with ESP-PPQ's power-of-two scheme did
its job. `export/edge_sup/edge_sup.espdl` is 67,568 bytes. `export.json` in the same folder records these numbers.
