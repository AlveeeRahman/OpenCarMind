"""Neural architecture search for the on-board model: one shared trunk, three heads, Kind 1 data only, MLX on the Mac.

  python -m carmindformer.nas_edge --trials 150                 # search (validation only)
  python -m carmindformer.nas_edge --final                      # re-train the best candidates, test the winner once
  python -m carmindformer.nas_edge --final --single-task        # same winner, three single-head models, for comparison

Windows are the board's own 10 s pattern windows (tasks.windows_for, an exact port of main/pattern.c). For each window
the model reads the last W samples of the 10 OBD channels the board polls (plus validity masks), never crossing the
start of its split part. Three heads share the trunk:

  driver     10 drivers, from the mean over the whole context            score: accuracy
  manoeuvre  stopped / braking / accelerating / cruising, from the last  score: macro-F1
             10 samples and the whole context
  estimator  brake pressure, flywheel torque, fuel rate (standardised),  score: mean R2 over the three targets
             from the same pooled features

Optuna 5 (multi-objective TPE) maximises the three validation scores; the final pick is the Pareto candidate with the
best mean after a 3-seed re-train. Operators are ESP-DL-friendly only: 1-D convolutions (standard or depthwise-
separable), ReLU, residual Add, average pooling, Concat and Linear. The test part is read only by --final.
"""
from __future__ import annotations
import argparse
import json
import math
import os
import time

import mlx.core as mx
import mlx.nn as nn
import mlx.optimizers as optim
import numpy as np
import optuna
from mlx.utils import tree_flatten

from .data import EDGE_CHANNELS
from .tasks import EST, MANOEUVRES, windows_for
from .train_edge_mlx import ArcFace, fake_quant
from .train_supervised import channel_norm, load_runs, split_bounds

PARAM_BUDGET = 120_000
LAST = 10                         # samples in one board window (10 s at 1 Hz)


# ----------------------------------------------------------------------------- data
def build(runs, windows, W, norm, protocol):
    """Arrays for every board window: x, m [N, 10, W]; driver, manoeuvre labels; estimator targets [N, 3]."""
    sig = {}
    for s in runs:
        v = np.full((len(EDGE_CHANNELS), s.values.shape[1]), np.nan, np.float32)
        for k, c in enumerate(EDGE_CHANNELS):
            if c in s.channels:
                v[k] = (s.values[s.channels.index(c)] - norm[c][0]) / norm[c][1]
        sig[s.name] = v
    out = {p: {"x": [], "m": [], "drv": [], "man": [], "est": [], "run": [], "row": []} for p in ("train", "val", "test")}
    for w in windows:
        s = w["series"]; lo, _ = split_bounds(s.values.shape[1], w["part"], protocol, s.name)
        b = w["rows"][1]; a = max(lo, b - W)
        seg = sig[s.name][:, a:b]
        x = np.zeros((len(EDGE_CHANNELS), W), np.float32); m = np.zeros_like(x)
        ok = ~np.isnan(seg)
        x[:, W - seg.shape[1]:] = np.where(ok, seg, 0); m[:, W - seg.shape[1]:] = ok
        d = out[w["part"]]
        d["x"].append(x); d["m"].append(m); d["drv"].append(w["driver"]); d["man"].append(w["label"]); d["est"].append(w["est"])
        d["run"].append(s.name); d["row"].append(b)
    for d in out.values():
        for k in d:
            d[k] = np.stack(d[k]) if d[k] else np.zeros((0,))
        d["drv"] = d["drv"].astype(np.int32); d["man"] = d["man"].astype(np.int32)
    mu = np.nanmean(out["train"]["est"], 0); sd = np.nanstd(out["train"]["est"], 0)
    for d in out.values():
        d["est_z"] = ((d["est"] - mu) / sd).astype(np.float32)
    return out


_DATA: dict = {}


def get_data(runs, windows, W, norm, protocol):
    if W not in _DATA:
        _DATA[W] = build(runs, windows, W, norm, protocol)
    return _DATA[W]


# ----------------------------------------------------------------------------- model
class Conv(nn.Module):
    def __init__(self, cin, cout, k, d=1, groups=1, quant=False):
        super().__init__()
        s = math.sqrt(groups / (cin * k))
        self.weight = mx.random.uniform(-s, s, (cout, k, cin // groups))
        self.bias = mx.zeros((cout,))
        self.pad, self.d, self.g, self.quant = (k - 1) * d // 2, d, groups, quant

    def __call__(self, x):
        w = fake_quant(self.weight) if self.quant else self.weight
        y = mx.conv1d(x, w, padding=self.pad, dilation=self.d, groups=self.g) + self.bias
        return fake_quant(y) if self.quant else y


class Block(nn.Module):
    """Residual block. 'std': two k-convs. 'dws': (depthwise k, pointwise 1) twice."""

    def __init__(self, c, k, d, kind, p, quant):
        super().__init__()
        self.kind = kind
        if kind == "std":
            self.l = [Conv(c, c, k, d, quant=quant), Conv(c, c, k, d, quant=quant)]
        else:
            self.l = [Conv(c, c, k, d, c, quant), Conv(c, c, 1, quant=quant), Conv(c, c, k, d, c, quant), Conv(c, c, 1, quant=quant)]
        self.drop = nn.Dropout(p)

    def __call__(self, x):
        y = x
        for i, conv in enumerate(self.l):
            y = conv(y)
            if i < len(self.l) - 1 and (self.kind == "std" or i % 2 == 1):
                y = self.drop(nn.relu(y))
        return nn.relu(x + y)


class Net(nn.Module):
    def __init__(self, a: dict, quant: bool = False, heads=("drv", "man", "est")):
        super().__init__()
        self.a, self.heads = a, heads
        c, n_in = a["width"], 30 if a["deltas"] else 20
        self.stem = Conv(n_in, c, 1, quant=quant)
        self.blocks = [Block(c, a["kernel"], a["dil_growth"] ** i, a["block"], a["dropout"], quant) for i in range(a["layers"])]
        if "drv" in heads:
            self.drv_proj = nn.Linear(c, a["embed"])
            self.drv = ArcFace(a["embed"], 10, a["s"], a["m"]) if a["drv_head"] == "arcface" else nn.Linear(a["embed"], 10)
        if "man" in heads or "est" in heads:
            self.local = nn.Linear(2 * c, a["local"])
        if "man" in heads:
            self.man = nn.Linear(a["local"], len(MANOEUVRES))
        if "est" in heads:
            self.est = nn.Linear(a["local"], len(EST))

    def __call__(self, x, m, y_drv=None):
        parts = [x, m]
        if self.a["deltas"]:
            parts.append(mx.concatenate([mx.zeros_like(x[:, :, :1]), x[:, :, 1:] - x[:, :, :-1]], axis=2) * m)
        h = nn.relu(self.stem(mx.concatenate(parts, axis=1).transpose(0, 2, 1)))     # [B, T, C]
        for b in self.blocks:
            h = b(h)
        g = mx.mean(h, axis=1)                         # whole context
        out = {}
        if "drv" in self.heads:
            e = self.drv_proj(g)
            if self.a["drv_head"] == "arcface":
                e = e / mx.maximum(mx.linalg.norm(e, axis=1, keepdims=True), 1e-6)
                out["drv"] = self.drv(e, y_drv)
            else:
                out["drv"] = self.drv(e)
        if "man" in self.heads or "est" in self.heads:
            z = nn.relu(self.local(mx.concatenate([mx.mean(h[:, -LAST:, :], axis=1), g], axis=1)))
            if "man" in self.heads:
                out["man"] = self.man(z)
            if "est" in self.heads:
                out["est"] = self.est(z)
        return out


def n_params(model) -> int:
    return sum(v.size for _, v in tree_flatten(model.parameters()))


# ----------------------------------------------------------------------------- train / score
def macro_f1(y, p, k):
    f = []
    for c in range(k):
        tp = np.sum((p == c) & (y == c)); fp = np.sum((p == c) & (y != c)); fn = np.sum((p != c) & (y == c))
        if tp + fn == 0:
            continue
        pr, rc = tp / max(1, tp + fp), tp / max(1, tp + fn)
        f.append(0.0 if pr + rc == 0 else 2 * pr * rc / (pr + rc))
    return float(np.mean(f)) if f else 0.0


def score(model, d, bs=512):
    P = {"drv": [], "man": [], "est": []}
    for i in range(0, len(d["drv"]), bs):
        o = model(mx.array(d["x"][i:i + bs]), mx.array(d["m"][i:i + bs]))
        for k in o:
            P[k].append(np.array(o[k]))
    s = {}
    if P["drv"]:
        s["drv"] = float(np.mean(np.concatenate(P["drv"]).argmax(1) == d["drv"]))
    if P["man"]:
        s["man"] = macro_f1(d["man"], np.concatenate(P["man"]).argmax(1), len(MANOEUVRES))
    if P["est"]:
        pe, t = np.concatenate(P["est"]), d["est_z"]
        r2 = []
        for j in range(t.shape[1]):
            ok = ~np.isnan(t[:, j])
            r2.append(1 - np.sum((pe[ok, j] - t[ok, j]) ** 2) / max(1e-9, np.sum((t[ok, j] - t[ok, j].mean()) ** 2)))
        s["est"] = float(np.mean(r2)); s["est_each"] = [round(float(v), 4) for v in r2]
    return s


def train(a, D, epochs, batch=128, seed=0, quant=False, heads=("drv", "man", "est")):
    mx.random.seed(seed); rng = np.random.default_rng(seed)
    tr, va = D["train"], D["val"]
    model = Net(a, quant=quant, heads=heads)
    n = len(tr["drv"]); steps = max(1, n // batch); total = epochs * steps
    sched = optim.join_schedules([optim.linear_schedule(a["lr"] / 25, a["lr"], max(1, total // 10)),
                                  optim.cosine_decay(a["lr"], total - total // 10, a["lr"] / 100)], [max(1, total // 10)])
    opt = optim.AdamW(learning_rate=sched, weight_decay=a["wd"])
    cw = np.bincount(tr["man"], minlength=len(MANOEUVRES)).astype(np.float32)
    cw = mx.array((cw.sum() / np.maximum(cw, 1) / len(cw)) ** a["class_pow"])     # 0 = no weighting, 1 = balanced

    def loss_fn(model, x, m, yd, ym, ye):
        o = model(x, m, yd)
        L = 0.0
        if "drv" in o:
            L = L + mx.mean(nn.losses.cross_entropy(o["drv"], yd, label_smoothing=a["smooth"]))
        if "man" in o:
            L = L + a["w_man"] * mx.sum(nn.losses.cross_entropy(o["man"], ym, reduction="none") * cw[ym]) / mx.sum(cw[ym])
        if "est" in o:
            ok = ~mx.isnan(ye)
            L = L + a["w_est"] * mx.sum(mx.where(ok, (o["est"] - mx.where(ok, ye, 0)) ** 2, 0)) / mx.maximum(mx.sum(ok), 1)
        return L

    step = nn.value_and_grad(model, loss_fn)
    best, best_w, best_s = -1e9, None, None
    for ep in range(epochs):
        model.train()
        order = rng.permutation(n)
        for i in range(steps):
            idx = order[i * batch:(i + 1) * batch]
            _, g = step(model, *(mx.array(tr[k][idx]) for k in ("x", "m", "drv", "man", "est_z")))
            opt.update(model, g); mx.eval(model.parameters(), opt.state)
        model.eval()
        s = score(model, va)
        mean = float(np.mean([s[k] for k in ("drv", "man", "est") if k in s]))
        if mean > best:
            best, best_w, best_s = mean, dict(tree_flatten(model.parameters())), s
    model.load_weights(list(best_w.items())); model.eval()
    return model, best_s


def space(t: optuna.Trial, heads=("drv", "man", "est")) -> dict:
    a = {"W": t.suggest_categorical("W", [30, 60, 90, 120]),
         "block": t.suggest_categorical("block", ["std", "dws"]),
         "layers": t.suggest_int("layers", 2, 6),
         "width": t.suggest_categorical("width", [16, 24, 32, 48, 64, 96]),
         "kernel": t.suggest_categorical("kernel", [3, 5, 7]),
         "dil_growth": t.suggest_categorical("dil_growth", [1, 2]),
         "deltas": t.suggest_categorical("deltas", [False, True]),
         "local": t.suggest_categorical("local", [16, 32, 64]),
         "dropout": t.suggest_float("dropout", 0.0, 0.3),
         "lr": t.suggest_float("lr", 3e-4, 5e-3, log=True),
         "wd": t.suggest_float("wd", 1e-5, 1e-2, log=True),
         "w_man": t.suggest_float("w_man", 0.3, 3.0, log=True),
         "w_est": t.suggest_float("w_est", 0.3, 3.0, log=True),
         "class_pow": t.suggest_categorical("class_pow", [0.0, 0.5, 1.0])}
    if "drv" in heads:
        a.update({"embed": t.suggest_categorical("embed", [16, 32, 48, 64]),
                  "drv_head": t.suggest_categorical("drv_head", ["arcface", "softmax"]),
                  "s": t.suggest_float("s", 10.0, 32.0), "m": t.suggest_float("m", 0.1, 0.4),
                  "smooth": t.suggest_float("smooth", 0.0, 0.2)})
    return a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="data"); ap.add_argument("--study", default="runs/nas/edge_nas.db")
    ap.add_argument("--name", default="edge_kind1_mt_v1"); ap.add_argument("--trials", type=int, default=150)
    ap.add_argument("--epochs", type=int, default=25); ap.add_argument("--final-epochs", type=int, default=60)
    ap.add_argument("--protocol", default="holdout"); ap.add_argument("--final", action="store_true")
    ap.add_argument("--single-task", action="store_true"); ap.add_argument("--top", type=int, default=5)
    ap.add_argument("--heads", default="drv,man,est", help="comma list from drv, man, est")
    ap.add_argument("--qat", action="store_true", help="quantization-aware final training (int8, power-of-two)")
    a = ap.parse_args()
    H = tuple(h for h in ("drv", "man", "est") if h in a.heads.split(","))
    NAME = {"drv": "driver", "man": "manoeuvre", "est": "estimator"}
    os.makedirs(os.path.dirname(a.study), exist_ok=True)
    runs = load_runs(a.data); norm = channel_norm(runs, a.protocol); windows = windows_for(runs, a.protocol)
    study = optuna.create_study(storage=f"sqlite:///{a.study}", study_name=a.name, load_if_exists=True,
                                directions=["maximize"] * len(H),
                                sampler=optuna.samplers.TPESampler(seed=0, multivariate=True, n_startup_trials=25))
    if not a.final:
        def objective(t):
            cfg = space(t, H)
            p = n_params(Net(cfg, heads=H)); t.set_user_attr("params", p)
            if p > PARAM_BUDGET:
                raise optuna.TrialPruned(f"{p} params over budget")
            t0 = time.time()
            _, s = train(cfg, get_data(runs, windows, cfg["W"], norm, a.protocol), a.epochs, heads=H)
            t.set_user_attr("seconds", round(time.time() - t0, 1)); t.set_user_attr("est_each", s["est_each"])
            return tuple(s[h] for h in H)
        study.optimize(objective, n_trials=a.trials, gc_after_trial=True)
        done = [t for t in study.trials if t.state == optuna.trial.TrialState.COMPLETE]
        front = sorted(study.best_trials, key=lambda t: -np.mean(t.values))
        print(f"trials {len(study.trials)}: complete {len(done)}, over budget {len(study.trials) - len(done)}; Pareto front {len(front)}")
        for t in front[:a.top]:
            print(f"  #{t.number} " + " ".join(f"{h} {v:.3f}" for h, v in zip(H, t.values)) + f" mean {np.mean(t.values):.3f} "
                  f"params {t.user_attrs['params']:,} W {t.params['W']} {t.params['block']} L{t.params['layers']} C{t.params['width']} k{t.params['kernel']}")
        return

    front = sorted(study.best_trials, key=lambda t: -np.mean(t.values))[:a.top]
    rows = []
    for t in front:
        D = get_data(runs, windows, t.params["W"], norm, a.protocol)
        S = [train(t.params, D, a.final_epochs, seed=s, quant=a.qat, heads=H)[1] for s in (0, 1, 2)]
        means = [np.mean([s[h] for h in H]) for s in S]
        rows.append({"trial": t.number, "val_mean": float(np.mean(means)), "val_std": float(np.std(means)),
                     "val_heads": {k: float(np.mean([s[k] for s in S])) for k in H}, "params": t.params})
        print(f"#{t.number}: 3-seed val mean {np.mean(means):.3f} ± {np.std(means):.3f}  {rows[-1]['val_heads']}")
    rows.sort(key=lambda r: -r["val_mean"]); win = rows[0]
    D = get_data(runs, windows, win["params"]["W"], norm, a.protocol)
    model, _ = train(win["params"], D, a.final_epochs, seed=0, quant=a.qat, heads=H)
    test = score(model, D["test"])
    res = {"winner": win, "test": test, "test_mean": float(np.mean([test[h] for h in H])), "heads": list(H),
           "params": n_params(model), "qat": a.qat}
    print(f"WINNER #{win['trial']} ({res['params']:,} params, qat={a.qat}): TEST " + ", ".join(f"{NAME[h]} {test[h]:.3f}" for h in H)
          + (f" {test['est_each']}" if "est" in H else "") + f", mean {res['test_mean']:.3f}")
    if a.single_task:                                  # same architecture, one head each, same budget and seed
        single = {}
        for h in H:
            m1, _ = train(win["params"], D, a.final_epochs, seed=0, quant=a.qat, heads=(h,))
            single[h] = score(m1, D["test"])[h]
        res["single_task_test"] = single
        print("single-task TEST: " + ", ".join(f"{NAME[h]} {single[h]:.3f}" for h in H))
    mx.save_safetensors(os.path.join(os.path.dirname(a.study), f"{a.name}_winner{'_qat' if a.qat else ''}.safetensors"),
                        dict(tree_flatten(model.parameters())))
    json.dump(res, open(os.path.join(os.path.dirname(a.study), f"{a.name}_final{'_qat' if a.qat else ''}.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
