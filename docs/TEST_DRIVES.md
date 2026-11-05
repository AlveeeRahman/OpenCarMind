# Test drives

Two additional drives serve as a held-out test set for the models on `main`. No model saw them during training or tuning.

## Signals

![Timeline of both test drives](images/test-drives-timeline.png)

Speed, throttle, accelerator pedal and engine rpm over time. Red bands are stops at traffic lights, and the striped band is an extended stop for a road emergency. Sand bands are steady holds and blue bands are holds with the pedal released, as under cruise control. Triangles mark hard acceleration and hard braking.

![Test drives against routine drives](images/test-drives-distribution.png)

Distributions of speed, acceleration and throttle, and speed against throttle, for the test drives and six routine drives of the same vehicle.

## Why the set is a fair test

The drives move the inputs away from the routine data in five ways:

- **Extended idle.** One stop is about three times longer than the longest stop in the routine drives, so the classifier must hold "stopped" over a long stationary stretch with the engine running.
- **Stop and go.** Frequent stops at traffic lights, each followed by a launch.
- **Higher load and speed.** One drive spends more of its moving time above 20 % throttle than any routine drive, and reaches a higher top speed.
- **Cruise control.** With cruise control engaged, the throttle moves while the pedal reads zero. A model that relies on the pedal signal would misread these stretches.
- **Hard acceleration and braking.**

## Scoring

The drives are scored as data from an unseen vehicle. No per-vehicle settings carry over from earlier drives: the closed-throttle level used for the labels is estimated from each drive, and every learner starts from its factory state. The models run as their published int8 files.
