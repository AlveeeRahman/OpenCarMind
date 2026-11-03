"""Probes for a trained CarMind-Former checkpoint.

  python -m carmindformer.eval --ckpt runs/former-.../last.pt --data data

1. Driver identity: logistic regression on frozen CLS embeddings of HCRL Driving windows (train/test by run).
2. CAN attack detection: AUROC of the attack head and of the change head on CAN windows.
3. Change detection: synthetic regime switches, precision/recall of change-head peaks.
"""
from __future__ import annotations
import argparse

import numpy as np
import torch
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import roc_auc_score


def rank_auroc(y, score):
    """AUROC from ranks (Mann-Whitney); robust to dtype quirks in sklearn."""
    y = np.asarray(y).astype(int).ravel(); score = np.asarray(score, dtype=np.float64).ravel()
    pos, neg = score[y == 1], score[y == 0]
    if len(pos) == 0 or len(neg) == 0:
        return float("nan")
    order = np.argsort(np.concatenate([pos, neg]))
    ranks = np.empty(len(order)); ranks[order] = np.arange(1, len(order) + 1)
    return float((ranks[: len(pos)].sum() - len(pos) * (len(pos) + 1) / 2) / (len(pos) * len(neg)))
from torch.utils.data import DataLoader

from .data import WindowDataset, discover, synthetic_series
from .model_former import CarMindFormer, FormerConfig, change_targets
from .train import pick_device, to_device


@torch.no_grad()
def embed(model, series, device, n=2000, max_channels=48):
    ds = WindowDataset(series, window=model.cfg.window, max_channels=max_channels, samples_per_epoch=n, channel_dropout=0.0)
    out = {"cls": [], "driver": [], "attack_logit": [], "attack": [], "change_pred": [], "change_tgt": [], "change_patch": []}
    for b in DataLoader(ds, batch_size=64):
        b = to_device(b, device)
        o = model(b["x"], b["m"], b["cid"], b["cmask"], b["stats"], mask_ratio=0.0)
        out["cls"].append(o["cls"].cpu()); out["driver"].append(b["driver"].cpu())
        out["attack_logit"].append(o["attack"].cpu()); out["attack"].append(b["attack"].reshape(b["attack"].shape[0], -1, model.cfg.patch).max(-1).values.cpu())
        out["change_patch"].append(o["change"].mean(1).cpu()); out["change_pred"].append(o["change"].mean((1, 2)).cpu()); out["change_tgt"].append(change_targets(b["x"], model.cfg.patch).mean((1, 2)).cpu())
    return {k: torch.cat(v).numpy() for k, v in out.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--data", default="data")
    ap.add_argument("--synthetic", action="store_true")
    ap.add_argument("--device", default=None)
    args = ap.parse_args()
    device = pick_device(args.device)
    ck = torch.load(args.ckpt, map_location=device)
    model = CarMindFormer(FormerConfig(**ck["config"])).to(device).eval()
    model.load_state_dict(ck["model"])
    found = {"driving": synthetic_series(n=12), "can": []} if args.synthetic else discover(args.data, max_can_rows=2_000_000)

    if found["driving"]:
        # split by recorded path, not by driver: path 1 runs train the probe, path 2 runs test it
        tr_s = [r for r in found["driving"] if r.name.endswith("_1")] or found["driving"][: len(found["driving"]) // 2]
        te_s = [r for r in found["driving"] if r.name.endswith("_2")] or found["driving"][len(found["driving"]) // 2:]
        tr, te = embed(model, tr_s, device), embed(model, te_s, device)
        ok_tr, ok_te = tr["driver"] >= 0, te["driver"] >= 0
        if ok_tr.any() and ok_te.any() and len(set(tr["driver"][ok_tr])) > 1:
            clf = LogisticRegression(max_iter=3000).fit(tr["cls"][ok_tr], tr["driver"][ok_tr])
            print(f"driver-id linear probe accuracy: {clf.score(te['cls'][ok_te], te['driver'][ok_te]):.3f} "
                  f"({len(set(tr['driver'][ok_tr]))} drivers in train, {len(set(te['driver'][ok_te]))} in test; chance {1/len(set(tr['driver'][ok_tr])):.3f})")
        corr = np.corrcoef(te["change_pred"], te["change_tgt"])[0, 1]
        print(f"change head correlation with |patch delta|: {corr:.3f}")
    labelled = [c for c in found["can"] if c.attack is not None and c.attack.sum() > 0]   # only files with attack flags:
    if labelled:                                                                           # negatives are normal bins of the same car
        e = embed(model, labelled, device)
        y_patch = (e["attack"].reshape(-1) > 0.5).astype(int)
        y_win = (e["attack"].max(1) > 0.5).astype(int)
        if 0 < y_patch.sum() < len(y_patch):
            print(f"CAN attack AUROC per patch, attack head: {rank_auroc(y_patch, e['attack_logit'].reshape(-1)):.3f}")
        if 0 < y_patch.sum() < len(y_patch):
            print(f"CAN attack AUROC per patch, change head (unsupervised): {rank_auroc(y_patch, e['change_patch'].reshape(-1)):.3f}  (attacked patches: {y_patch.mean():.2f})")
        if 0 < y_win.sum() < len(y_win):
            print(f"CAN attack AUROC per window, change head (unsupervised): {rank_auroc(y_win, e['change_pred']):.3f}  (attacked windows: {y_win.mean():.2f})")


if __name__ == "__main__":
    main()
