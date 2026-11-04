# Case study: making one small model work on every car

Edge Model 1 is the result of one question: how far can training alone take a 38,679-parameter network on an ESP32-S3, without touching its architecture? Here is everything we tried between the first model (30 s window, one car) and Edge Model 1 (5 s window, three sources, 384 extra cars), including what failed. The architecture stayed fixed the whole time. That is the next step, and we would like your help with it (see the end).

## The problem

The first model was trained on the public KIA Soul drives of the HCRL Driving Dataset and scored a macro-F1 of 0.912 on held-out KIA drives. On two held-out drives of the author's car it put 93.4 % of windows in an allowed class and estimated fuel with R² 0.837. Respectable, but it was trained on one car, and two things differ on every other car:

- Sensors. The KIA data has no MAF and no intake temperature. The author's car reports both. The firmware already feeds untrained channels as missing, so the shipped model was safe, but it also could not use them.
- Labels. Only the KIA data has a brake switch. Other cars give speed, and speed alone is a weak braking signal. Against the KIA brake switch, "deceleration of 0.3 m/s² or more" is right 92 % of the time it fires but catches only 37 % of brake presses (best F1 0.61 at any threshold).

So the work had two parts: give the model data from more cars, and label that data honestly.

## How every variant was tested

- Test sets, never used for training or model selection: the last 20 % of each public KIA drive, two whole drives of the author's car, and (from variant BV on) 37 whole VED vehicles.
- Selection on validation only: one score that weighs the KIA, the author's car and, when used, VED equally.
- Three seeds per variant. Tables show the mean, with the spread where it matters.
- "Set accuracy" is the share of windows where the model's class is one the label allows. A window labelled "braking or cruising" counts as right for either.
- "Fuel R², MAF hidden" scores the fuel estimate with MAF removed from the input, which is when the board actually uses it.
- "Missing sensors" repeats the author's test with whole channels removed: no MAF, no fuel trims, no temperatures, or rpm, speed, load and throttle only.

## Step 1: how long a window?

The labels describe the last 10 s, but the first model read 30 s. The architecture search had only offered 30, 60, 90 and 120 s. Same weights to start, same training, on the KIA test set:

| Window | Manoeuvre F1 | Estimator mean R² |
|---|---|---|
| 10 s | 0.894 ± 0.004 | 0.791 ± 0.007 |
| 15 s | 0.894 ± 0.002 | 0.784 ± 0.003 |
| 20 s | 0.895 ± 0.001 | 0.789 ± 0.006 |
| 30 s | 0.892 ± 0.003 | 0.786 ± 0.015 |

Every difference is inside the seed spread. A shorter window loses nothing, so we moved to 10 s.

## Step 2: training methods, at 10 s

| Variant | What changed | KIA F1 | Author's car, set acc. | Author's fuel R², MAF hidden | Author's car, missing sensors |
|---|---|---|---|---|---|
| A10 | KIA only | 0.891 | see note | see note | see note |
| A5 | KIA only, second mean over the last 5 s | 0.887 | see note | see note | see note |
| B | + the author's drives with set labels, + "other car" augmentation | 0.892 | 0.970 | 0.869 | 0.965 |
| C | B after contrastive pre-training (SimCLR-style NT-Xent) | diverged | | | |
| C2 | C with a 10× lower pre-training rate and gradient clipping | 0.890 | 0.970 | 0.845 | 0.966 |
| D2 | B + distillation from a 30 s teacher | 0.891 | 0.883 | 0.850 | 0.893 |

Notes, because the honest version matters more than the table:

- A10 and A5 on the author's car: our test script fed these KIA-only models the MAF and intake values they never trained on, which the firmware would never do. Their scores on the author's car (set accuracy 0.67 and 0.75) measure that mistake, not the models. With those channels hidden, as the firmware hides them, the KIA-only 10 s model from step 1 (one seed) scored 0.944 set accuracy and 0.841 fuel R². B beats that.
- "Other car" augmentation switches each sensor off 15 % of the time (rpm and speed always kept, since every car reports them), scales each channel by a random gain in raw units (±25 % for MAF and load, which covers engine size, ±8 % elsewhere) and thins the slow channels. B added it together with the author's drives, and the pair moved every number on the author's car. We did not separate the two.
- C, contrastive pre-training, moved the network's internal features while the output heads kept their old weights. The first fine-tuning step produced an estimator R² of −1,145 and the class head collapsed to one class. C2 fixed the instability and then matched B without beating it.
- D2's teacher had the same test-script mistake as A10 (it was fed channels it never saw), so its soft labels on the author's car were wrong. We fixed it before the VED round (DV below). Even fixed, distillation did not help.

## Step 3: 384 more cars (VED)

The Vehicle Energy Dataset (Oh, LeBlanc and Peng, IEEE T-ITS 2020, Apache-2.0) logs 384 petrol, hybrid, plug-in and electric vehicles. It has rpm, speed, MAF and fuel trims, about one sample every 0.9 s. We replayed each trip at the author's real row spacing, binned it as the firmware does, labelled it with the same speed rules, and split it by vehicle: 325 to train, 19 to validate, 37 to test. EVs were left out. MAF and intake temperature also got their own normalisation for the first time, since the KIA data has neither.

| Variant | KIA F1 | Author's car, set acc. | Author's fuel R², MAF hidden | Unseen VED cars, set acc. | Unseen VED fuel R², MAF hidden | Validation score |
|---|---|---|---|---|---|---|
| B (no VED, new normalisation) | 0.886 | 0.975 | 0.896 | 0.899 | 0.408 | 0.835 |
| BV (B + VED) | 0.893 | 0.979 | 0.888 | 0.965 | 0.693 | 0.882 |
| DV (BV + distillation, teacher fixed) | 0.890 | 0.962 | 0.875 | 0.881 | 0.674 | 0.861 |

VED cost nothing on the KIA or the author's car and gained a lot on cars the model had never seen. BV won on validation and on every test set built from unseen cars.

## Step 4: a faster answer, 5 s

At about 1.4 s per OBD row, 5 s still holds three rows, and a faster adapter only adds more. We retrained BV with a 5 s window and labels over the last 5 s, starting from the BV weights. The table compares each model against 5 s labels on 1,498 manoeuvre changes in the held-out KIA drives:

| | Median lag | Mean lag | Matched within 3 s | Missed (over 15 s) | Accuracy |
|---|---|---|---|---|---|
| BV, 10 s | 2.8 s | 4.5 s | 55 % | 16 % | 0.805 |
| BV5, 5 s (3 seeds) | 0 s | 1.5 to 1.6 s | 87 to 88 % | 5 % | 0.892 to 0.893 |

The 10 s model was built to describe a longer span, so some of that lag is by design. What the table shows is that the 5 s model answers "what is the car doing now". On its own 5 s labels, BV5 kept the BV scores: KIA F1 0.887, author's car set accuracy 0.983, unseen VED cars 0.958, fuel with MAF hidden 0.882 and 0.685. Brake pressure paid the price (KIA estimator mean R² 0.776, against 0.792), because 5 s averages are noisier. BV5 seed 1 shipped as Edge Model 1.

## What did not help

- Contrastive pre-training (C, C2): unstable at first, then no gain over plain augmentation.
- Distillation from a larger-context teacher (D2, DV): worse on new cars both times.
- A longer window (15 to 30 s): no gain.
- Generating synthetic drives (for example a time-series GAN) and an LLM as a labeller were discussed and not run. Neither adds information the three real sources lack, and the second would send drive data to an outside service.

## What we did not change

The architecture, all of it: six residual depthwise-separable blocks, 48 channels, two mean pools, a 64-wide shared layer, two heads. Every result above comes from data, labels, augmentation and window length. The remaining weak spots look like architecture limits more than data limits:

- Brake pressure stays near R² 0.46 to 0.5 whatever the data.
- Fuel without MAF on unseen cars tops out near 0.69.
- Every variant used one temporal design: kernel 3, no attention, no recurrence.

## Send us an architecture

If you think a different network does better, open an issue with the Model architecture template. We train it on the same data, splits, seeds and quantisation-aware recipe, score it on the same held-out KIA drives, the author's held-out drives and the 37 unseen VED cars, and post the results in this file with your name on them, whether it wins or not.

What a proposal needs to fit on the chip:

- Input `[1, 30, 1, W]`: 10 values, 10 masks and 10 differences per one-second step, oldest first, W = 5 today (any W from 5 to 30 is fine).
- Outputs: 4 manoeuvre logits and 3 standardised estimates.
- Operations ESP-DL runs: convolution (standard and depthwise), ReLU, Add, ReduceMean, Concat and Gemm are known to work. Others may, but check the ESP-DL operator list first.
- Budget: under about 150 KB as int8 and under about 10 ms per run on the ESP32-S3. Edge Model 1 uses 89 KB and 3.5 ms.
- Format: a PyTorch `nn.Module` or an ONNX graph, plus a sentence on why you expect it to help.

We do not publish the training data or code. The author's drives stay private, and training lives in a separate project. Every result we post will follow the test protocol above.
