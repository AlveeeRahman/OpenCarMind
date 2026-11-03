"""On-board tasks on Kind 1 data, scored against the board's own heuristic.

The board's pattern layer (pattern_c.py, an exact port of main/pattern.c) is run over every drive in time order,
one online model for all drives as on the board. Each window it closes records the rows it covered. On those same
windows we derive ground truth from signals the board cannot read over OBD (the KIA Soul's CAN-internal channels):

  manoeuvre (4 classes, priority order): stopped (mean speed < 2 km/h); braking (moving, brake pedal pressed in
  >= 30 % of samples); accelerating (mean longitudinal accel >= 0.5 m/s2); cruising (everything else). Turning is
  left out by the owner's decision (1 % of windows).
  estimates: mean brake pressure (bar), mean flywheel torque (N m), mean fuel rate.

Notes on the source columns, checked against speed: `accel_long_g` and `accel_lat_g` are m/s2 despite the name
(slope 0.98 against d(speed)/dt); `brake_switch` is 1 released, 2 pressed.

  python -m carmindformer.tasks          # baselines: board heuristic vs learned classifiers on the board's features
"""
from __future__ import annotations
import argparse
import json

import numpy as np

from .pattern_c import SIGNAL_COLS, PatternC, f32
from .train_supervised import load_runs, split_bounds

MANOEUVRES = ["stopped", "braking", "accelerating", "cruising"]
EST = ["brake_pressure_bar", "flywheel_torque_nm", "fuel_rate_lph"]


def _col(s, c):
    return s.values[s.channels.index(c)] if c in s.channels else np.full(s.values.shape[1], np.nan)


def windows_for(runs, protocol="holdout", parts=("train", "val", "test")):
    """Run the board heuristic over every drive part in order; return one record per closed window."""
    pc = PatternC()
    out = []
    t_ms = 0
    for part in parts:                                   # the board's model sees train, then val, then test
        for s in runs:
            r = split_bounds(s.values.shape[1], part, protocol, s.name)
            if r is None:
                continue
            lo, hi = r
            sig = np.stack([_col(s, c) for c in SIGNAL_COLS])           # [10, T]
            pc._reset(0)                                              # a new drive starts a new window
            start_row = lo
            for i in range(lo, hi):
                t_ms += 1000
                v = sig[:, i]
                valid = [not np.isnan(x) for x in v]
                res = pc.feed(t_ms, [f32(0 if np.isnan(x) else x) for x in v], valid)
                if res is None:
                    continue
                out.append({"run": s.name, "part": part, "driver": s.driver, "rows": (start_row, i + 1),
                            "pattern_id": int(res["pattern_id"]), "novelty": float(res["novelty"]),
                            "features": np.asarray(res["features"], dtype=np.float32), "series": s})
                start_row = i + 1
    for w in out:
        _targets(w)
    return out


def _targets(w):
    s, (a, b) = w["series"], w["rows"]
    seg = lambda c: _col(s, c)[a:b]
    speed = np.nanmean(seg("speed_kph"))
    brake = np.nanmean(seg("brake_switch") == 2)
    lon = np.nanmean(seg("accel_long_g"))
    if speed < 2:
        lab = "stopped"
    elif brake >= 0.3:
        lab = "braking"
    elif lon >= 0.5:
        lab = "accelerating"
    else:
        lab = "cruising"
    w["label"] = MANOEUVRES.index(lab)
    w["est"] = np.array([np.nanmean(seg("brake_pressure_bar")), np.nanmean(seg("flywheel_torque_nm")),
                         np.nanmean(seg("fuel_rate_lph"))], dtype=np.float32)


def macro_f1(y, p, k=len(MANOEUVRES)):
    f = []
    for c in range(k):
        tp = np.sum((p == c) & (y == c)); fp = np.sum((p == c) & (y != c)); fn = np.sum((p != c) & (y == c))
        if tp + fn == 0:
            continue
        pr = tp / max(1, tp + fp); rc = tp / max(1, tp + fn)
        f.append(0 if pr + rc == 0 else 2 * pr * rc / (pr + rc))
    return float(np.mean(f))


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--data", default="data"); ap.add_argument("--protocol", default="holdout")
    a = ap.parse_args()
    from sklearn.linear_model import LogisticRegression, Ridge
    from sklearn.metrics import normalized_mutual_info_score, r2_score
    from sklearn.pipeline import make_pipeline
    from sklearn.preprocessing import StandardScaler

    W = windows_for(load_runs(a.data), a.protocol)
    tr = [w for w in W if w["part"] == "train"]; te = [w for w in W if w["part"] == "test"]
    ytr = np.array([w["label"] for w in tr]); yte = np.array([w["label"] for w in te])
    print(f"windows: train {len(tr)}, test {len(te)}")
    print("test label shares:", {MANOEUVRES[c]: round(float(np.mean(yte == c)), 3) for c in range(len(MANOEUVRES))})

    rep = {}
    maj = int(np.bincount(ytr).argmax())
    rep["majority class"] = (float(np.mean(yte == maj)), macro_f1(yte, np.full_like(yte, maj)))
    # board heuristic: each online k-means pattern id mapped to the label it most often carries in training
    pid_tr = np.array([w["pattern_id"] for w in tr]); pid_te = np.array([w["pattern_id"] for w in te])
    m = {p: int(np.bincount(ytr[pid_tr == p], minlength=len(MANOEUVRES)).argmax()) for p in np.unique(pid_tr)}
    pb = np.array([m.get(p, maj) for p in pid_te])
    rep["board patterns -> label"] = (float(np.mean(pb == yte)), macro_f1(yte, pb))
    nmi = normalized_mutual_info_score(yte, pid_te)
    # learned classifier on the board's own 30 features
    Xtr = np.stack([w["features"] for w in tr]); Xte = np.stack([w["features"] for w in te])
    lr = make_pipeline(StandardScaler(), LogisticRegression(max_iter=3000, class_weight="balanced")).fit(Xtr, ytr)
    pl = lr.predict(Xte); rep["logistic on board features"] = (float(np.mean(pl == yte)), macro_f1(yte, pl))
    for k, (acc, f1) in rep.items():
        print(f"  {k:30s} accuracy {acc:.3f}  macro-F1 {f1:.3f}")
    print(f"  board pattern ids vs labels: NMI {nmi:.3f}")

    Etr = np.stack([w["est"] for w in tr]); Ete = np.stack([w["est"] for w in te])
    print("estimation, ridge on board features (R2 on test; 0 = predicting the mean):")
    for j, name in enumerate(EST):
        ok_tr = ~np.isnan(Etr[:, j]); ok_te = ~np.isnan(Ete[:, j])
        rg = make_pipeline(StandardScaler(), Ridge(alpha=1.0)).fit(Xtr[ok_tr], Etr[ok_tr, j])
        print(f"  {name:22s} R2 {r2_score(Ete[ok_te, j], rg.predict(Xte[ok_te])):.3f}   (test std {np.std(Ete[ok_te, j]):.2f})")
    json.dump({k: {"accuracy": v[0], "macro_f1": v[1]} for k, v in rep.items()} | {"board_nmi": nmi},
              open("runs/tasks_baselines.json", "w"), indent=1)


if __name__ == "__main__":
    main()
