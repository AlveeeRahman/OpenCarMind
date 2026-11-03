"""Logistic-regression baseline for driver identification under each protocol, on either the 10 OBD
channels the device can see (EDGE_CHANNELS) or all shared channels. Features: per-window mean and std.
  python -m carmindformer.baseline_driver --data data --protocol session --channels edge
"""
from __future__ import annotations
import argparse
import glob
import os

import numpy as np
from sklearn.linear_model import LogisticRegression
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler

from .data import EDGE_CHANNELS, load_hcrl_driving
from .train_supervised import split_bounds


def windows(s, chans, W, lo, hi, stride):
    idx = [s.channels.index(c) for c in chans]
    X = []
    for st in range(lo, hi - W, stride):
        v = s.values[idx, st:st + W]
        v = np.where(np.isnan(v), np.nanmean(v, axis=1, keepdims=True), v)
        X.append(np.concatenate([v.mean(1), v.std(1), np.abs(np.diff(v, axis=1)).mean(1)]))
    return X


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="data"); ap.add_argument("--protocol", default="holdout"); ap.add_argument("--channels", default="edge"); ap.add_argument("--window", type=int, default=60)
    a = ap.parse_args()
    runs = []
    for p in glob.glob(os.path.join(a.data, "driving_dataset", "**", "*.csv"), recursive=True):
        runs += load_hcrl_driving(p)
    shared = set.intersection(*[set(s.channels) for s in runs])
    chans = [c for c in EDGE_CHANNELS if c in shared] if a.channels == "edge" else sorted(shared)
    print(f"{len(chans)} channels: {chans if len(chans) < 15 else len(chans)}")
    Xtr, ytr, Xte, yte = [], [], [], []
    for s in runs:
        T = s.values.shape[1]
        if a.protocol == "random":
            parts = [("both", 0, T)]
        else:
            parts = [(p, *r) for p in ("train", "test") if (r := split_bounds(T, p, a.protocol, s.name))]
        for part, lo, hi in parts:
            X = windows(s, chans, a.window, lo, hi, a.window // 2)
            if part == "both":
                rng = np.random.RandomState(0); sel = rng.rand(len(X)) < 0.8
                Xtr += [x for x, k in zip(X, sel) if k]; ytr += [s.driver] * int(sel.sum()); Xte += [x for x, k in zip(X, sel) if not k]; yte += [s.driver] * int((~sel).sum())
            elif part == "train":
                Xtr += X; ytr += [s.driver] * len(X)
            else:
                Xte += X; yte += [s.driver] * len(X)
    clf = make_pipeline(StandardScaler(), LogisticRegression(max_iter=3000, C=0.5)).fit(np.array(Xtr), ytr)
    print(f"protocol={a.protocol} channels={a.channels} window={a.window}: train {len(ytr)} test {len(yte)} windows, accuracy {clf.score(np.array(Xte), yte):.3f} (chance 0.100)")


if __name__ == "__main__":
    main()
