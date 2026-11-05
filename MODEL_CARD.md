# Model card: CarMind E.V and CarMind R.E.V

This card shows how the on-board model went from CarMind E.V to the final CarMind R.E.V, one checked step at a time. Every number here is also in the [CHANGELOG](CHANGELOG.md) or the [detailed README on `rev-c2`](https://github.com/AlveeeRahman/OpenCarMind/blob/rev-c2/README.md), which has the methods behind each one.

## Key

How to read the tables.

| Term | Meaning | Better |
|---|---|---|
| R² | Coefficient of determination: the share of the variation in the true value an estimate explains. 1 is perfect, 0 is no better than always guessing the average. | higher ↑ |
| Accuracy | Share of labelled windows the classifier got right. | higher ↑ |
| Recall | Share of the windows of one class that were called that class. | higher ↑ |
| Macro-F1 | The F1 score (balance of precision and recall) averaged over classes with equal weight. | higher ↑ |
| "In an allowed class" | The class given is one of the classes the raw signals allow for that window. | higher ↑ |
| Factory | The model or read-out as shipped, before any learning in the car. | |
| Δ | Change from the step before. | |
| Window | Five seconds of driving, the unit the model reads. | |

Evidence level, shown in the "Where" column:

| Tag | Where the number comes from |
|---|---|
| **C** | Measured on the ESP32-S3 itself |
| **Q** | The quantised model, or its integer classifier head on features the board logged, run on a computer |
| **R** | Replay: recorded drives played through the learners on a computer, float network, airflow hidden |
| **D** | Drives of my own car (Nissan Sentra), labelled by a rule on the raw OBD signals |

## The two models at a glance

| | CarMind E.V | CarMind R.E.V (first, 2.5.0) | CarMind R.E.V (final, 2.5.1) |
|---|---|---|---|
| Model hash | `dfa9af416dfb` | `f70bdcd3151b` | `6e6d698c44c5` |
| Parameters | 38,679 | 19,815 | 19,815 |
| Size on the chip | 89,152 bytes | 65,648 bytes | 65,648 bytes |
| Arithmetic | int8 weights, int16 activations | int8 weights, int16 activations | int8 weights, int16 activations |
| Time per run on the ESP32-S3 (**C**) | about 3.5 ms in the car, 2.9 ms on the bench | about 2.9 ms in the car (median), 2.2 ms on the bench | the same |
| Outputs | manoeuvre class (stopped, braking, accelerating, cruising), brake pressure, flywheel torque, fuel rate | same | same |
| Learns in the car | tank size and fill-up only (both models) | fuel read-out, airflow constant (engine size, which sizes the torque estimate), tank size | the same, outlier-robust |
| Held-out windows, 2 drives of my car (**Q, D**) | 78.9 % | 78.3 % | 95.1 % |
| Steady cruising, foot on the gas (**Q, D**) | 13.7 % right | 10.7 % right | 87.5 % right |

## From E.V to the final R.E.V, step by step

| Step | What changed | Metric | Before | After | Δ | Where |
|---|---|---|---|---|---|---|
| 0. E.V, the baseline (2.4.0) | quantisation-aware training, tested on held-out drives and unseen cars | manoeuvre macro-F1, public KIA drives | | 0.887 | | Q |
| | | fuel R², public KIA drives | | 0.947 | | Q |
| | | in an allowed class, 37 VED cars it never saw | | 96.1 % | | Q |
| | | fuel R², 37 unseen VED cars, airflow hidden | | 0.688 | | Q |
| 1. Compression (2.5.0) | structured pruning to about half the parameters, knowledge distillation from E.V, int8 | parameters | 38,679 | 19,815 | −48.8 % | |
| | | size on the chip | 89,152 B | 65,648 B | −26.4 % | |
| | | time per run | 3.5 ms | 2.9 ms | −17 % | C |
| 2. Learning on the board (2.5.0) | the fuel read-out (65 weights) is refit while you drive, and goes live only if it beats the live one on held-back rows | fuel R², 31 unseen VED cars, airflow hidden | 0.709 factory | 0.867 | +0.158 | R |
| | | cars that got better | | 29 of 31 | | R |
| | an airflow constant per car | median R² across 24 cars | 0.578 factory | 0.877 | +0.299 | R |
| | | cars that got better | | 24 of 24 | | R |
| | engine torque from physics, sized to the engine | R² against measured torque, one public car, moving | | 0.81 to 0.85 | | R |
| 3. First drives in my car | the learners in a real car | fuel read-out refits kept, resets | | 4 kept, 0 reset | | C, D |
| | | learned read-out running error against factory | | 39 % lower | | C, D |
| | | cruising windows called braking (14 drives) | | 78 % | | D |
| 4. Explainable AI | Grad-CAM, Guided Grad-CAM and a linear probe on the 64-value embedding | probe: foot on the gas against closed throttle, balanced accuracy | | 93 % | | D |
| | | angle between the head's braking-against-cruising direction and that concept (cosine) | | 0.002 | | D |
| | | braking evidence on wrong windows: speed / throttle | | 62 % / 17 % | | D |
| 5. Classifier head corrected (2.5.1) | 108 of 256 head weights and 3 of 4 biases, 126 bytes of the model. Trunk and estimate head unchanged | all labelled windows, two held-out drives | 78.3 % | 95.1 % | +16.8 pts | Q, D |
| | | cruising, foot on the gas | 10.7 % | 87.5 % | +76.8 pts | Q, D |
| | | public KIA test set, accuracy | 88.9 % | 87.1 % | −1.8 pts | Q |
| 6. Outlier-robust learner (2.5.1) | Huber weights on a running error scale, pairs over 10 times that scale dropped, labels above 120 L/h ignored | fuel R², 31 cars, 1 % bad labels | 0.696 | 0.867 | +0.171 | R |
| | | cars more than 10 % worse than no learning, 1 % bad labels | 15 | 0 | −15 | R |
| | | bad labels rejected, one of my drives with 8 injected | | 8 of 8 | | R, D |
| 7. Checked against meta-learning | ANIL and Meta-SGD on the same frozen trunk and the same 31 cars | fuel R², clean labels | 0.778 / 0.197 | 0.867 (kept) | | R |

## Classifier, all three models on the same windows

Two drives of my car from 4 November 2026 that took no part in the correction (**Q, D**), and the public KIA test set (**Q**).

| Class or set | Windows | CarMind E.V | CarMind R.E.V (old) | CarMind R.E.V | Δ R.E.V (old) → R.E.V |
|---|---|---|---|---|---|
| Cruising, foot on the gas | 168 | 13.7 % | 10.7 % | 87.5 % | +76.8 pts |
| Braking | 95 | 97.9 % | 98.9 % | 96.8 % | −2.1 pts |
| Accelerating | 100 | 86.0 % | 88.0 % | 91.0 % | +3.0 pts |
| Stopped | 433 | 98.4 % | 97.7 % | 98.6 % | +0.9 pts |
| **All labelled windows** | 796 | **78.9 %** | **78.3 %** | **95.1 %** | **+16.8 pts** |
| KIA test set, accuracy | | 89.4 % (export test) | 88.9 % | 87.1 % | −1.8 pts |
| KIA test set, braking recall | | not measured | 80.5 % | 83.7 % | +3.2 pts |
| KIA test set, cruising recall | | not measured | 90.3 % | 84.7 % | −5.6 pts |

E.V was scored on the same windows and the same raw-signal labels as R.E.V, run as its float ONNX graph (its int8 version agrees on 99.9 % of KIA windows). Its KIA accuracy comes from its export test, which gives the first R.E.V 88.75 %.

The correction costs 5.6 points of cruising recall on the KIA car, and real braking now wins by a smaller margin.

## Fuel learner benchmark

31 VED test cars that took no part in training, airflow hidden (**R**).

| Fuel learner | Fuel R², clean labels | Fuel R², 1 % bad labels | Cars more than 10 % worse than no learning (clean / bad) |
|---|---|---|---|
| No learning (factory read-out) | 0.709 | 0.709 | |
| Plain RLS, CarMind R.E.V (old) | 0.867 | 0.696 | 0 / 15 |
| ANIL (learn2learn) | 0.778 | 0.774 | 2 / 3 |
| Meta-SGD (learn2learn) | 0.197 | 0.151 | 20 / 22 |
| **Huber RLS, CarMind R.E.V (shipped)** | **0.867** | **0.867** | **0 / 0** |

## Intended use

- An OBD2 data logger for older cars, roughly pre-2020, many of which report no fuel-rate PID and can lose the airflow reading.
- The class and the three estimates are information for the driver and the logs. They are not a safety system.
- Everything runs on the board. Drives stay on the SD card and, if you pair the board, on your own computer.

## Data

Public data, the HCRL Driving Dataset (Kwak, Woo and Kim, PST 2016) and the Vehicle Energy Dataset (Oh, LeBlanc and Peng, 2020, Apache-2.0), plus drives of my own car. Training happens off the board, once per release, in a private repository. This repository holds the trained files in [ml/onboard](ml/onboard).

## Limitations

- The classifier gains are measured on one car, mine, with labels from a rule on the raw signals, which cannot see the brake pedal.
- Outside one data set there is no brake-switch signal, so braking and cruising stay partly ambiguous. Brake pressure (R² 0.46 for E.V) is the weakest estimate.
- No diesel car is in the data. Physics torque is for petrol engines and was checked on one public car. R.E.V learns the engine size the physics uses (about 2.1 L on my car's last two drives), not the torque itself. A learned torque read-out from cars that report torque (PIDs 0x62 and 0x63) is planned, not built. Cars without those PIDs, like my Sentra, have no torque label, so physics or an accelerometer label stays needed there.
- CarMind E.V still calls steady cruising "braking". Only R.E.V got the corrected head.
