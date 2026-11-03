"""CarMind-Edge-S trained with Apple MLX (Metal) instead of PyTorch. Same architecture, same data split,
same power-of-two symmetric int8 fake quantisation as qat.py, so the weights load straight into the
PyTorch definition for ONNX export (export_edge_sup.py --mlx-weights).

  python -m carmindformer.train_edge_mlx --data data --protocol holdout --qat --epochs 40

MLX layout notes: Conv1d takes [N, L, C] and weights [out, k, in]; PyTorch's Conv2d(1xk) weight is
[out, in, 1, k]. The straight-through estimator is x + stop_gradient(q(x) - x).
"""
from __future__ import annotations
import argparse
import json
import math
import os
import random
import time

import mlx.core as mx
import mlx.nn as nn
import mlx.optimizers as optim
import numpy as np
from mlx.utils import tree_flatten

from .data import EDGE_CHANNELS
from .train_supervised import DriverWindows, channel_norm, load_runs


# ----------------------------------------------------------------------------- fake quant
def pow2_scale(max_abs, bits=8):
    qmax = 2 ** (bits - 1) - 1
    return mx.power(2.0, mx.ceil(mx.log2(mx.maximum(max_abs, 1e-8) / qmax)))


def fake_quant(x, bits=8):
    qmax = 2 ** (bits - 1) - 1
    s = pow2_scale(mx.stop_gradient(mx.max(mx.abs(x))), bits)
    q = mx.clip(mx.round(x / s), -qmax - 1, qmax) * s
    return x + mx.stop_gradient(q - x)


class QConv1d(nn.Module):
    def __init__(self, cin, cout, k, dilation=1, padding=0, quant=False):
        super().__init__()
        scale = math.sqrt(1 / (cin * k))
        self.weight = mx.random.uniform(-scale, scale, (cout, k, cin))
        self.bias = mx.zeros((cout,))
        self.k, self.dilation, self.padding, self.quant = k, dilation, padding, quant

    def __call__(self, x):
        w = fake_quant(self.weight) if self.quant else self.weight
        y = mx.conv1d(x, w, stride=1, padding=self.padding, dilation=self.dilation) + self.bias
        return fake_quant(y) if self.quant else y


class QLinear(nn.Module):
    def __init__(self, cin, cout, quant=False):
        super().__init__()
        scale = math.sqrt(1 / cin)
        self.weight = mx.random.uniform(-scale, scale, (cout, cin))
        self.bias = mx.zeros((cout,))
        self.quant = quant

    def __call__(self, x):
        w = fake_quant(self.weight) if self.quant else self.weight
        y = x @ w.T + self.bias
        return fake_quant(y) if self.quant else y


class TCNBlock(nn.Module):
    def __init__(self, c, k, d, p, quant):
        super().__init__()
        pad = (k - 1) * d // 2
        self.c1 = QConv1d(c, c, k, d, pad, quant); self.c2 = QConv1d(c, c, k, d, pad, quant)
        self.drop = nn.Dropout(p)

    def __call__(self, x):
        y = self.drop(nn.relu(self.c1(x)))
        return nn.relu(x + self.c2(y))


class ArcFace(nn.Module):
    def __init__(self, embed, n_classes, s=20.0, m=0.21):
        super().__init__()
        self.W = mx.random.normal((n_classes, embed)) * 0.01
        self.s, self.cos_m, self.sin_m = s, math.cos(m), math.sin(m)
        self.th, self.mm = math.cos(math.pi - m), math.sin(math.pi - m) * m

    def __call__(self, e, y=None):
        Wn = self.W / mx.maximum(mx.linalg.norm(self.W, axis=1, keepdims=True), 1e-6)
        cos = e @ Wn.T
        if y is None:
            return self.s * cos
        sin = mx.sqrt(mx.maximum(1.0 - cos * cos, 1e-6))
        phi = mx.where(cos > self.th, cos * self.cos_m - sin * self.sin_m, cos - self.mm)
        onehot = mx.zeros_like(cos); onehot[mx.arange(cos.shape[0]), y] = 1.0
        return self.s * mx.where(onehot > 0, phi, cos)


class EdgeSupNet(nn.Module):
    def __init__(self, n_channels=10, width=32, layers=5, kernel=3, embed=48, n_classes=10, dropout=0.1, quant=False):
        super().__init__()
        self.n_channels = n_channels
        self.stem = QConv1d(2 * n_channels, width, 1, quant=quant)
        self.blocks = [TCNBlock(width, kernel, 2 ** i, dropout, quant) for i in range(layers)]
        self.proj = QLinear(width, embed, quant)
        self.head = ArcFace(embed, n_classes)
        self.recon = QConv1d(width, n_channels, 1, quant=quant)

    def __call__(self, x, m, y=None):
        """x, m: [B, C, T] as in PyTorch; converted to [B, T, 2C] for MLX convs."""
        h = mx.concatenate([x, m], axis=1).transpose(0, 2, 1)
        h = nn.relu(self.stem(h))
        for b in self.blocks:
            h = b(h)
        e = self.proj(mx.mean(h, axis=1))
        e = e / mx.maximum(mx.linalg.norm(e, axis=1, keepdims=True), 1e-6)
        return e, self.head(e, y), self.recon(h).transpose(0, 2, 1)


# ----------------------------------------------------------------------------- data
def materialise(ds, n=None):
    """Pull windows out of the torch-based dataset into numpy once; MLX then streams from RAM."""
    N = len(ds) if n is None else n
    X = np.zeros((N, len(EDGE_CHANNELS), ds.window), np.float32); M = np.zeros_like(X); Y = np.zeros(N, np.int32)
    for i in range(N):
        b = ds[i]; X[i] = b["x"].numpy(); M[i] = b["m"].numpy(); Y[i] = int(b["y"])
    return X, M, Y


def accuracy(model, X, M, Y, bs=512):
    ok = 0
    for i in range(0, len(Y), bs):
        _, logits, _ = model(mx.array(X[i:i + bs]), mx.array(M[i:i + bs]))
        ok += int(mx.sum(mx.argmax(logits, axis=1) == mx.array(Y[i:i + bs])).item())
    return ok / max(1, len(Y))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="data"); ap.add_argument("--out", default="runs/supervised_mlx"); ap.add_argument("--protocol", choices=["holdout", "session", "random"], default="holdout")
    ap.add_argument("--qat", action="store_true"); ap.add_argument("--epochs", type=int, default=40); ap.add_argument("--steps", type=int, default=200); ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--lr", type=float, default=2e-3); ap.add_argument("--window", type=int, default=60); ap.add_argument("--train-pool", type=int, default=40000)
    ap.add_argument("--width", type=int, default=32); ap.add_argument("--layers", type=int, default=5); ap.add_argument("--embed", type=int, default=48); ap.add_argument("--tag", default="")
    a = ap.parse_args()
    mx.random.seed(0); random.seed(0); np.random.seed(0)
    runs = load_runs(a.data); norm = channel_norm(runs, a.protocol)
    t0 = time.time()
    Xtr, Mtr, Ytr = materialise(DriverWindows(runs, a.window, "train", a.protocol, samples=a.train_pool, norm=norm))
    Xva, Mva, Yva = materialise(DriverWindows(runs, a.window, "val" if a.protocol != "random" else "test", a.protocol, norm=norm))
    Xte, Mte, Yte = materialise(DriverWindows(runs, a.window, "test", a.protocol, norm=norm))
    print(f"windows: train pool {len(Ytr)}, val {len(Yva)}, test {len(Yte)} ({time.time() - t0:.0f}s); device {mx.default_device()}")
    model = EdgeSupNet(width=a.width, layers=a.layers, embed=a.embed, quant=a.qat)
    n_params = sum(v.size for _, v in tree_flatten(model.parameters()))
    print(f"edge-sup (MLX): {n_params:,} params, QAT={a.qat}, protocol={a.protocol}")
    total = a.epochs * a.steps
    sched = optim.join_schedules([optim.linear_schedule(a.lr / 25, a.lr, total // 10), optim.cosine_decay(a.lr, total - total // 10, a.lr / 100)], [total // 10])
    opt = optim.AdamW(learning_rate=sched, weight_decay=1e-4)

    def loss_fn(model, x, m, y):
        _, logits, recon = model(x, m, y)
        ce = mx.mean(nn.losses.cross_entropy(logits, y))
        rec = mx.sum(((recon - x) ** 2) * m) / mx.maximum(mx.sum(m), 1.0)
        return ce + 0.1 * rec

    step_fn = nn.value_and_grad(model, loss_fn)
    best, best_w = -1.0, None
    for ep in range(a.epochs):
        model.train(); tot = 0.0
        for _ in range(a.steps):
            idx = np.random.randint(0, len(Ytr), a.batch)
            loss, grads = step_fn(model, mx.array(Xtr[idx]), mx.array(Mtr[idx]), mx.array(Ytr[idx]))
            opt.update(model, grads); mx.eval(model.parameters(), opt.state); tot += loss.item()
        model.eval(); acc = accuracy(model, Xva, Mva, Yva)
        if acc >= best:
            best, best_w = acc, dict(tree_flatten(model.parameters()))
        print(f"epoch {ep} loss {tot / a.steps:.3f}  val acc ({a.protocol}) {acc:.3f}")
    model.load_weights(list(best_w.items())); model.eval()
    test_acc = accuracy(model, Xte, Mte, Yte)
    os.makedirs(a.out, exist_ok=True)
    tag = f"edge_sup_{a.protocol}{'_qat' if a.qat else ''}{a.tag}"
    mx.save_safetensors(os.path.join(a.out, tag + ".safetensors"), best_w)
    json.dump({"protocol": a.protocol, "qat": a.qat, "val_acc": best, "test_acc": test_acc, "norm": norm, "params": n_params,
               "config": {"window": a.window, "n_channels": len(EDGE_CHANNELS), "width": a.width, "layers": a.layers, "kernel": 3, "embed": a.embed, "n_classes": 10}},
              open(os.path.join(a.out, tag + ".json"), "w"), indent=1)
    print(f"best val {best:.3f}; held-out TEST accuracy {test_acc:.3f} (chance 0.100); saved {tag}.safetensors")


if __name__ == "__main__":
    main()
