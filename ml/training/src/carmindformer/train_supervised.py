"""Supervised training with the evaluation protocols the literature uses.

Driver identification (HCRL KIA Soul), CarMind-Edge-S with QAT:
  python -m carmindformer.train_supervised driver --data data --protocol session --qat
  protocols: "holdout" (default) = every run is cut chronologically 64/16/20 into train/val/test; the
             last 20 % of each drive is never seen until the one final test call (TEMPEST's chronological
             split reports 79-94 %); "session" = train on path 1 runs, test on path 2 runs (cross-session,
             TEMPEST 41-66 %); "random" = leaky random windows (~99.9 %, reported only to expose leakage).

CAN intrusion detection (HCRL Car-Hacking), CarMind-MLM:
  python -m carmindformer.train_supervised can --data data [--pretrain-steps N] [--max-rows N]
  29-frame windows, 5 classes, chronological 70/30 split per file. Reported per class: precision,
  recall, F1 (state of the art is 1.000 on all four attacks; Transformer 0.9998, KD-XVAE 1.000).
"""
from __future__ import annotations
import argparse
import glob
import json
import os
import random
import time

import numpy as np
import torch
import torch.nn.functional as F
from torch.utils.data import DataLoader, Dataset

from .can_frames import ATTACK_NAMES, CanFrameDataset, load_frames
from .data import EDGE_CHANNELS, Series, load_hcrl_driving
from .model_edge_sup import EdgeSupConfig, EdgeSupNet
from .model_mlm import CarMindMLM, MLMConfig, mask_tokens
from .qat import int8_size_bytes, set_quant
from .train import pick_device, to_device


# --------------------------------------------------------------------------- driver task
def load_runs(data_dir):
    runs = []
    for p in glob.glob(os.path.join(data_dir, "driving_dataset", "**", "*.csv"), recursive=True):
        runs += load_hcrl_driving(p)
    # CARMIND_DROP_CHANNELS="map_kpa,..." removes channels from every run, so training, the norm statistics and the
    # exporter's `trained` flags all agree that the model never saw them (a car that lacks the PID feeds them as missing).
    drop = {c.strip() for c in os.environ.get("CARMIND_DROP_CHANNELS", "").split(",") if c.strip()}
    if drop:
        for s in runs:
            keep = [i for i, c in enumerate(s.channels) if c not in drop]
            s.values = s.values[keep]
            s.channels = [s.channels[i] for i in keep]
    return runs


def split_bounds(T: int, part: str, protocol: str, name: str):
    """Row range of `part` in a run of length T under `protocol`; None when the run has no such part."""
    if protocol == "holdout":
        a, b = int(T * 0.64), int(T * 0.8)
        return {"train": (0, a), "val": (a, b), "test": (b, T)}[part]
    if protocol == "session":
        is_train_run = name.endswith("_1")
        if part == "train":
            return (0, int(T * 0.8)) if is_train_run else None
        if part == "val":
            return (int(T * 0.8), T) if is_train_run else None
        return None if is_train_run else (0, T)
    return (0, T)      # random: leaky on purpose


def channel_norm(series, protocol):
    """Per-channel mean/std over the training rows only (global standardisation keeps the window level,
    which per-window instance norm would throw away and which carries most of the driver signal)."""
    stats = {}
    for c in EDGE_CHANNELS:
        vals = []
        for s in series:
            if c not in s.channels:
                continue
            r = split_bounds(s.values.shape[1], "train", protocol, s.name)
            if r:
                vals.append(s.values[s.channels.index(c), r[0]:r[1]])
        v = np.concatenate(vals) if vals else np.zeros(1)
        v = v[~np.isnan(v)]
        stats[c] = [float(v.mean()) if len(v) else 0.0, float(v.std()) if len(v) and v.std() > 1e-6 else 1.0]
    return stats


class DriverWindows(Dataset):
    def __init__(self, series, window, part, protocol, samples=None, seed=0, norm=None):
        self.items = []
        self.norm = norm
        rng = random.Random(seed)
        for s in series:
            idx = [s.channels.index(c) if c in s.channels else -1 for c in EDGE_CHANNELS]
            r = split_bounds(s.values.shape[1], part, protocol, s.name)
            if r is None:
                continue
            lo, hi = r
            if hi - lo > window:
                self.items.append((s, idx, lo, hi))
        self.window, self.samples, self.rng = window, samples, rng
        self.eval_index = None if samples else [(k, st) for k, (s, idx, lo, hi) in enumerate(self.items) for st in range(lo, hi - window, window // 2)]

    def __len__(self):
        return self.samples if self.samples else len(self.eval_index)

    def _make(self, s, idx, st):
        v = np.zeros((len(EDGE_CHANNELS), self.window), dtype=np.float32); m = np.zeros_like(v)
        for k, j in enumerate(idx):
            if j >= 0:
                col = s.values[j, st:st + self.window]; ok = ~np.isnan(col); v[k, ok] = col[ok]; m[k] = ok
        if self.norm:
            mean = np.array([[self.norm[c][0]] for c in EDGE_CHANNELS], dtype=np.float32); std = np.array([[self.norm[c][1]] for c in EDGE_CHANNELS], dtype=np.float32)
        else:
            mean = (v * m).sum(1, keepdims=True) / np.maximum(m.sum(1, keepdims=True), 1)
            std = np.sqrt((((v - mean) ** 2) * m).sum(1, keepdims=True) / np.maximum(m.sum(1, keepdims=True), 1)); std = np.where(std < 1e-3, 1.0, std)
        return {"x": torch.from_numpy(np.where(m > 0, (v - mean) / std, 0.0).astype(np.float32)), "m": torch.from_numpy(m), "y": torch.tensor(s.driver)}

    def __getitem__(self, i):
        if self.eval_index is not None:
            k, st = self.eval_index[i]; s, idx, lo, hi = self.items[k]
        else:
            s, idx, lo, hi = self.rng.choice(self.items); st = self.rng.randint(lo, hi - self.window - 1)
        return self._make(s, idx, st)


def run_driver(args, device):
    runs = load_runs(args.data)
    norm = channel_norm(runs, args.protocol)
    cfg = EdgeSupConfig(window=args.window)
    model = EdgeSupNet(cfg).to(device)
    set_quant(model, args.qat)
    print(f"edge-sup: {sum(p.numel() for p in model.parameters()):,} params, int8 weights+bias {int8_size_bytes(model) / 1024:.1f} KB, QAT={args.qat}, protocol={args.protocol}")
    tr = DriverWindows(runs, args.window, "train", args.protocol, samples=args.steps * args.batch, norm=norm)
    va = DriverWindows(runs, args.window, "val" if args.protocol != "random" else "test", args.protocol, norm=norm)
    te = DriverWindows(runs, args.window, "test", args.protocol, norm=norm)
    dl = DataLoader(tr, batch_size=args.batch, num_workers=0)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=args.lr, total_steps=args.epochs * args.steps, pct_start=0.1)
    best = 0.0
    for ep in range(args.epochs):
        model.train(); tot = 0.0
        for i, b in enumerate(dl):
            if i >= args.steps:
                break
            b = to_device(b, device)
            out = model(b["x"], b["m"], b["y"])
            loss = F.cross_entropy(out["logits"], b["y"]) + 0.1 * (((out["recon"] - b["x"]) ** 2) * b["m"]).sum() / b["m"].sum().clamp(min=1)
            opt.zero_grad(); loss.backward(); torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0); opt.step(); sched.step(); tot += loss.item()
        acc = eval_driver(model, va, device)
        if acc >= best:
            best, best_state = acc, {k: v.detach().clone() for k, v in model.state_dict().items()}
        print(f"epoch {ep} loss {tot / max(1, i):.3f}  val acc ({args.protocol}) {acc:.3f}")
    model.load_state_dict(best_state)
    test_acc = eval_driver(model, te, device)
    os.makedirs(args.out, exist_ok=True)
    torch.save({"model": model.state_dict(), "config": cfg.__dict__, "qat": args.qat, "protocol": args.protocol, "val_acc": best, "test_acc": test_acc, "norm": norm}, os.path.join(args.out, f"edge_sup_{args.protocol}{'_qat' if args.qat else ''}.pt"))
    print(f"best val {best:.3f}; held-out TEST accuracy {test_acc:.3f} (chance {1 / cfg.n_classes:.3f}); windows val {len(va)} test {len(te)}")


@torch.no_grad()
def eval_driver(model, ds, device):
    model.eval(); ok = n = 0
    for b in DataLoader(ds, batch_size=256):
        b = to_device(b, device)
        pred = model(b["x"], b["m"])["logits"].argmax(1)
        ok += (pred == b["y"]).sum().item(); n += len(pred)
    return ok / max(1, n)


# --------------------------------------------------------------------------- CAN task
def run_can(args, device):
    files = sorted(glob.glob(os.path.join(args.data, "car_hacking", "**", "*_dataset.csv"), recursive=True))
    files += glob.glob(os.path.join(args.data, "car_hacking", "**", "normal_run_data.txt"), recursive=True)
    logs = []
    for f in files:
        t = time.time(); L = load_frames(f, max_rows=args.max_rows)
        logs.append(L); print(f"{L.name}: {len(L.ids):,} frames, {int(L.flag.sum()):,} injected, class {ATTACK_NAMES[L.attack_class]} ({time.time() - t:.0f}s)")
    cfg = MLMConfig(d_model=args.d_model, n_layers=args.layers, n_heads=max(1, args.d_model // 32), d_ff=args.d_model * 4, max_len=1 + 29 * 10)
    model = CarMindMLM(cfg).to(device)
    print(f"mlm: {sum(p.numel() for p in model.parameters()):,} params")
    tr = CanFrameDataset(logs, part="train", samples=args.steps * args.batch)
    va = CanFrameDataset(logs, part="val", stride_eval=args.eval_stride)
    te = CanFrameDataset(logs, part="test", stride_eval=args.eval_stride)
    dl = DataLoader(tr, batch_size=args.batch, num_workers=0)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=0.01, betas=(0.9, 0.98))
    total = args.pretrain_steps + args.epochs * args.steps
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=args.lr, total_steps=total, pct_start=0.1)
    step = 0
    # masked-token pretraining on the same training windows (labels unused)
    model.train()
    it = iter(dl)
    while step < args.pretrain_steps:
        try:
            b = next(it)
        except StopIteration:
            it = iter(dl); b = next(it)
        b = to_device(b, device)
        inp, tgt = mask_tokens(b["tokens"])
        out = model(inp)
        loss = F.cross_entropy(out["mlm"].reshape(-1, out["mlm"].shape[-1]), tgt.reshape(-1), ignore_index=-100)
        opt.zero_grad(); loss.backward(); torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0); opt.step(); sched.step(); step += 1
        if step % 50 == 0:
            print(f"pretrain step {step} mlm loss {loss.item():.3f}")
    for ep in range(args.epochs):
        model.train(); tot = 0.0
        for i, b in enumerate(dl):
            if i >= args.steps:
                break
            b = to_device(b, device)
            out = model(b["tokens"])
            loss = F.cross_entropy(out["window"], b["label"]) + F.binary_cross_entropy_with_logits(out["frame"], b["frame_flag"])
            opt.zero_grad(); loss.backward(); torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0); opt.step(); sched.step(); tot += loss.item()
        rep = eval_can(model, va, device)
        print(f"epoch {ep} loss {tot / max(1, i):.3f}  val: " + "  ".join(f"{k} F1 {v['f1']:.4f}" for k, v in rep.items()))
    rep = eval_can(model, te, device)
    os.makedirs(args.out, exist_ok=True)
    torch.save({"model": model.state_dict(), "config": cfg.__dict__, "test_report": rep}, os.path.join(args.out, "mlm_can.pt"))
    json.dump(rep, open(os.path.join(args.out, "mlm_can_report.json"), "w"), indent=2)
    print("held-out TEST (last 20 % of every file):"); print(json.dumps(rep, indent=1))


@torch.no_grad()
def eval_can(model, ds, device):
    model.eval(); preds, labels = [], []
    for b in DataLoader(ds, batch_size=256):
        b = to_device(b, device)
        preds.append(model(b["tokens"])["window"].argmax(1).cpu()); labels.append(b["label"].cpu())
    p, y = torch.cat(preds).numpy(), torch.cat(labels).numpy()
    rep = {}
    for c, name in enumerate(ATTACK_NAMES):
        tp = int(((p == c) & (y == c)).sum()); fp = int(((p == c) & (y != c)).sum()); fn = int(((p != c) & (y == c)).sum())
        if tp + fn == 0:
            continue
        prec = tp / max(1, tp + fp); rec = tp / max(1, tp + fn)
        rep[name] = {"precision": round(prec, 4), "recall": round(rec, 4), "f1": round(2 * prec * rec / max(1e-9, prec + rec), 4), "support": tp + fn}
    rep["accuracy"] = {"f1": round(float((p == y).mean()), 4), "precision": 0, "recall": 0, "support": int(len(y))}
    return rep


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("task", choices=["driver", "can"])
    ap.add_argument("--data", default="data"); ap.add_argument("--out", default="runs/supervised"); ap.add_argument("--device", default=None)
    ap.add_argument("--epochs", type=int, default=20); ap.add_argument("--steps", type=int, default=200); ap.add_argument("--batch", type=int, default=128); ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--window", type=int, default=60); ap.add_argument("--protocol", choices=["holdout", "session", "random"], default="holdout"); ap.add_argument("--qat", action="store_true")
    ap.add_argument("--max-rows", type=int, default=None); ap.add_argument("--pretrain-steps", type=int, default=0); ap.add_argument("--d-model", type=int, default=256); ap.add_argument("--layers", type=int, default=6); ap.add_argument("--eval-stride", type=int, default=29)
    args = ap.parse_args()
    torch.manual_seed(0); random.seed(0); np.random.seed(0)
    device = pick_device(args.device); print(f"device {device}")
    (run_driver if args.task == "driver" else run_can)(args, device)


if __name__ == "__main__":
    main()
