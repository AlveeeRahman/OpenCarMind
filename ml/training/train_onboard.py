"""Train the on-board model with quantisation-aware training (QAT).

The network is a 1-D residual CNN over a 30 s window of ten OBD channels. Two heads share the trunk: a manoeuvre
classifier (stopped, braking, accelerating, cruising) and a regressor for brake pressure, flywheel torque and fuel rate.
Weights and activations are rounded to the int8 grid in every forward pass, with the power-of-two scales that ESP-DL
uses, so the exported model keeps the accuracy measured here.

    PYTHONPATH=src python3 train_onboard.py --data data --name onboard_v1
    PYTHONPATH=src python3 train_onboard.py --init shipped/edge_kind1_me_v1_winner_qat.safetensors --lr-scale 0.5
    PYTHONPATH=src python3 train_onboard.py --eval-only --init runs/onboard/onboard_v1/onboard_v1_winner_qat.safetensors

Set CARMIND_DROP_CHANNELS=map_kpa to train without a channel. The exporter reads the same variable, so the firmware
feeds that channel as missing.
"""
import argparse
import json
import math
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import mlx.core as mx
import mlx.nn as nn
import mlx.optimizers as optim
import numpy as np
from mlx.utils import tree_flatten

from carmindformer.data import EDGE_CHANNELS
from carmindformer.tasks import EST, MANOEUVRES, _targets
from carmindformer.train_supervised import channel_norm, load_runs, split_bounds

N_CHANNELS = len(EDGE_CHANNELS)
TARGET_SECONDS = 10
SLOW_CHANNELS = ("stft_b1_pct", "ltft_b1_pct", "coolant_c", "intake_temp_c", "map_kpa")
IS_SLOW = np.array([name in SLOW_CHANNELS for name in EDGE_CHANNELS])


@dataclass
class Hyperparameters:
    """Field names match the exporter's config file, so the saved config can be exported unchanged."""

    W: int                # window length in one-second steps
    block: str            # residual block type, only "dws" (depthwise separable) is implemented
    layers: int           # residual blocks
    width: int            # channels in the trunk
    kernel: int
    dil_growth: int       # dilation of block i is dil_growth ** i
    deltas: bool          # feed step-to-step differences as extra input channels
    local: int            # width of the dense layer shared by both heads
    dropout: float
    lr: float
    wd: float
    w_man: float          # loss weight of the manoeuvre head
    w_est: float          # loss weight of the estimator head
    class_pow: float      # manoeuvre class weights are (mean count / class count) ** class_pow


# ------------------------------------------------------------------------------------------------ model
def pow2_scale(max_abs, bits=8):
    qmax = 2 ** (bits - 1) - 1
    return mx.power(2.0, mx.ceil(mx.log2(mx.maximum(max_abs, 1e-8) / qmax)))


def fake_quant(x, bits=8):
    """Round x to a symmetric per-tensor int8 grid with a power-of-two step. The gradient passes straight through."""
    qmax = 2 ** (bits - 1) - 1
    step = pow2_scale(mx.stop_gradient(mx.max(mx.abs(x))), bits)
    rounded = mx.clip(mx.round(x / step), -qmax - 1, qmax) * step
    return x + mx.stop_gradient(rounded - x)


class QuantConv(nn.Module):
    """1-D convolution whose weights and outputs are fake-quantised."""

    def __init__(self, c_in, c_out, kernel, dilation=1, groups=1):
        super().__init__()
        bound = math.sqrt(groups / (c_in * kernel))
        self.weight = mx.random.uniform(-bound, bound, (c_out, kernel, c_in // groups))
        self.bias = mx.zeros((c_out,))
        self.padding = (kernel - 1) * dilation // 2
        self.dilation = dilation
        self.groups = groups

    def __call__(self, x):
        y = mx.conv1d(x, fake_quant(self.weight), padding=self.padding, dilation=self.dilation, groups=self.groups)
        return fake_quant(y + self.bias)


class ResidualBlock(nn.Module):
    """Two depthwise-separable convolutions with a skip connection."""

    def __init__(self, channels, kernel, dilation, dropout):
        super().__init__()
        # The attribute name `l` is part of the saved weight names that the exporter expects.
        self.l = [
            QuantConv(channels, channels, kernel, dilation, groups=channels),
            QuantConv(channels, channels, 1),
            QuantConv(channels, channels, kernel, dilation, groups=channels),
            QuantConv(channels, channels, 1),
        ]
        self.dropout = nn.Dropout(dropout)

    def __call__(self, x):
        depthwise_1, pointwise_1, depthwise_2, pointwise_2 = self.l
        y = self.dropout(nn.relu(pointwise_1(depthwise_1(x))))
        y = pointwise_2(depthwise_2(y))
        return nn.relu(x + y)


class OnboardNet(nn.Module):
    def __init__(self, hp: Hyperparameters):
        super().__init__()
        if hp.block != "dws" or not hp.deltas:
            raise ValueError("only the depthwise-separable block with delta inputs is implemented")
        self.stem = QuantConv(3 * N_CHANNELS, hp.width, 1)
        self.blocks = [ResidualBlock(hp.width, hp.kernel, hp.dil_growth ** i, hp.dropout) for i in range(hp.layers)]
        self.local = nn.Linear(2 * hp.width, hp.local)
        self.man = nn.Linear(hp.local, len(MANOEUVRES))
        self.est = nn.Linear(hp.local, len(EST))

    def __call__(self, x, mask):
        """x, mask: [batch, channels, steps]. Returns manoeuvre logits and standardised estimates."""
        first_step = mx.zeros_like(x[:, :, :1])
        delta = mx.concatenate([first_step, x[:, :, 1:] - x[:, :, :-1]], axis=2) * mask
        features = mx.concatenate([x, mask, delta], axis=1).transpose(0, 2, 1)
        h = nn.relu(self.stem(features))
        for block in self.blocks:
            h = block(h)
        whole_window = mx.mean(h, axis=1)
        last_seconds = mx.mean(h[:, -TARGET_SECONDS:, :], axis=1)
        z = nn.relu(self.local(mx.concatenate([last_seconds, whole_window], axis=1)))
        return {"man": self.man(z), "est": self.est(z)}


# ------------------------------------------------------------------------------------------------ data
def standardised_signals(run, norm):
    """The run's channels as [N_CHANNELS, rows], standardised, NaN where the car did not report a channel."""
    signals = np.full((N_CHANNELS, run.values.shape[1]), np.nan, np.float32)
    for i, name in enumerate(EDGE_CHANNELS):
        if name in run.channels:
            mean, std = norm[name]
            signals[i] = (run.values[run.channels.index(name)] - mean) / std
    return signals


def label_window(run, start, stop):
    window = {"series": run, "rows": (start, stop)}
    _targets(window)
    return window["label"], window["est"]


def make_windows(runs, norm, part, hp, protocol="holdout", stride=1, limit=0):
    """One window per `stride` rows of the chosen split. Each window ends at row `end` and is labelled from the last
    10 rows. Windows never reach back before the start of their split."""
    xs, masks, labels, estimates = [], [], [], []
    for run in runs:
        bounds = split_bounds(run.values.shape[1], part, protocol, run.name)
        if bounds is None:
            continue
        lo, hi = bounds
        signals = standardised_signals(run, norm)
        for end in range(lo + TARGET_SECONDS, hi + 1, stride):
            segment = signals[:, max(lo, end - hp.W):end]
            present = ~np.isnan(segment)
            x = np.zeros((N_CHANNELS, hp.W), np.float32)
            mask = np.zeros((N_CHANNELS, hp.W), np.float32)
            x[:, hp.W - segment.shape[1]:] = np.where(present, segment, 0)
            mask[:, hp.W - segment.shape[1]:] = present
            label, estimate = label_window(run, end - TARGET_SECONDS, end)
            xs.append(x)
            masks.append(mask)
            labels.append(label)
            estimates.append(estimate)
            if limit and len(xs) >= limit:
                break
    return {"x": np.stack(xs), "mask": np.stack(masks), "man": np.array(labels, np.int32),
            "est": np.stack(estimates).astype(np.float32)}


def estimate_stats(path, train_estimates):
    """Mean and std used to standardise the estimator targets. The shipped values keep the firmware constants valid."""
    if path and Path(path).exists():
        stats = json.loads(Path(path).read_text())
        return np.array(stats["mean"], np.float32), np.array(stats["std"], np.float32)
    return np.nanmean(train_estimates, 0), np.nanstd(train_estimates, 0)


def thin_and_hold(x, mask, fast_rate, slow_rate, hold, rng):
    """Resample windows as a slow adapter would deliver them. Fast channels arrive together with probability
    `fast_rate` per second, each slow channel on its own with `slow_rate`. A value is held for up to `hold` seconds."""
    batch, _, steps = x.shape
    fast_fresh = rng.random((batch, 1, steps)) < fast_rate[:, None, None]
    slow_fresh = rng.random((batch, N_CHANNELS, steps)) < slow_rate[:, None, None]
    fresh = np.where(IS_SLOW[None, :, None], slow_fresh, fast_fresh) & (mask > 0)
    out = np.zeros_like(x)
    out_mask = np.zeros_like(mask)
    last = np.zeros((batch, N_CHANNELS), np.float32)
    age = np.full((batch, N_CHANNELS), 10 ** 6, np.int32)
    for t in range(steps):
        new = fresh[:, :, t]
        last = np.where(new, x[:, :, t], last)
        age = np.where(new, 0, age + 1)
        held = age <= hold
        out[:, :, t] = np.where(held, last, 0.0)
        out_mask[:, :, t] = held
    return out, out_mask


def augment(x, mask, rng, hold, p_sparse=0.5, p_drop=0.04):
    """Training augmentation: thin half the windows like a slow adapter and drop whole channels now and then."""
    batch = x.shape[0]
    thin_x, thin_mask = thin_and_hold(x, mask, rng.uniform(0.4, 1.0, batch), rng.uniform(0.1, 0.6, batch), hold, rng)
    use_thin = (rng.random(batch) < p_sparse)[:, None, None]
    x = np.where(use_thin, thin_x, x)
    mask = np.where(use_thin, thin_mask, mask)
    keep = rng.random((batch, N_CHANNELS, 1)) >= p_drop
    return (x * keep).astype(np.float32), (mask * keep).astype(np.float32)


def slow_adapter_view(data, hold, seed=123):
    """A fixed thinned copy of a split: fast channels at 0.6 per second, slow channels at 0.15."""
    rng = np.random.default_rng(seed)
    n = len(data["man"])
    x, mask = thin_and_hold(data["x"], data["mask"], np.full(n, 0.6), np.full(n, 0.15), hold, rng)
    return dict(data, x=x.astype(np.float32), mask=mask.astype(np.float32))


# ------------------------------------------------------------------------------------------------ scoring
def macro_f1(truth, predicted, n_classes):
    scores = []
    for c in range(n_classes):
        tp = np.sum((predicted == c) & (truth == c))
        fp = np.sum((predicted == c) & (truth != c))
        fn = np.sum((predicted != c) & (truth == c))
        if tp + fn == 0:
            continue
        precision, recall = tp / max(1, tp + fp), tp / max(1, tp + fn)
        scores.append(0.0 if precision + recall == 0 else 2 * precision * recall / (precision + recall))
    return float(np.mean(scores)) if scores else 0.0


def r_squared(truth, predicted):
    valid = ~np.isnan(truth)
    residual = np.sum((predicted[valid] - truth[valid]) ** 2)
    total = np.sum((truth[valid] - truth[valid].mean()) ** 2)
    return 1 - residual / max(1e-9, total)


def evaluate(model, data, batch=1024):
    logits, estimates = [], []
    for i in range(0, len(data["man"]), batch):
        out = model(mx.array(data["x"][i:i + batch]), mx.array(data["mask"][i:i + batch]))
        logits.append(np.array(out["man"]))
        estimates.append(np.array(out["est"]))
    logits, estimates = np.concatenate(logits), np.concatenate(estimates)
    r2 = [float(r_squared(data["est_z"][:, j], estimates[:, j])) for j in range(len(EST))]
    f1 = macro_f1(data["man"], logits.argmax(1), len(MANOEUVRES))
    return {"man": f1, "est": float(np.mean(r2)), "est_each": [round(v, 4) for v in r2],
            "mean": (f1 + float(np.mean(r2))) / 2}


# ------------------------------------------------------------------------------------------------ training
def class_weights(labels, power):
    counts = np.bincount(labels, minlength=len(MANOEUVRES)).astype(np.float32)
    return mx.array((counts.sum() / np.maximum(counts, 1) / len(counts)) ** power)


def learning_rate_schedule(peak, total_steps):
    warmup = max(1, total_steps // 10)
    return optim.join_schedules(
        [optim.linear_schedule(peak / 25, peak, warmup), optim.cosine_decay(peak, total_steps - warmup, peak / 100)],
        [warmup])


def fit(hp, train, validation, args, seed, log):
    """Train one model. After every epoch it is scored on the clean and the thinned validation split, and the weights
    with the best average are kept."""
    mx.random.seed(seed)
    rng = np.random.default_rng(seed)
    model = OnboardNet(hp)
    if args.init:
        model.load_weights(args.init, strict=False)
    n = len(train["man"])
    steps_per_epoch = max(1, n // args.batch)
    optimiser = optim.AdamW(
        learning_rate=learning_rate_schedule(hp.lr * args.lr_scale, args.epochs * steps_per_epoch),
        weight_decay=hp.wd)
    weights = class_weights(train["man"], hp.class_pow)

    def loss_fn(model, x, mask, man_target, est_target):
        out = model(x, mask)
        sample_weight = weights[man_target]
        ce = nn.losses.cross_entropy(out["man"], man_target, reduction="none")
        man_loss = mx.sum(ce * sample_weight) / mx.sum(sample_weight)
        valid = ~mx.isnan(est_target)
        squared = mx.where(valid, (out["est"] - mx.where(valid, est_target, 0)) ** 2, 0)
        est_loss = mx.sum(squared) / mx.maximum(mx.sum(valid), 1)
        return hp.w_man * man_loss + hp.w_est * est_loss

    loss_and_grad = nn.value_and_grad(model, loss_fn)
    best_score, best_weights, best_scores = -math.inf, None, None
    for epoch in range(args.epochs):
        started = time.time()
        model.train()
        order = rng.permutation(n)
        for step in range(steps_per_epoch):
            idx = order[step * args.batch:(step + 1) * args.batch]
            x, mask = augment(train["x"][idx], train["mask"][idx], rng, args.hold)
            _, grads = loss_and_grad(model, mx.array(x), mx.array(mask), mx.array(train["man"][idx]),
                                     mx.array(train["est_z"][idx]))
            optimiser.update(model, grads)
            mx.eval(model.parameters(), optimiser.state)
        model.eval()
        scores = {name: evaluate(model, data) for name, data in validation.items()}
        score = float(np.mean([s["mean"] for s in scores.values()]))
        if score > best_score:
            best_score, best_weights, best_scores = score, dict(tree_flatten(model.parameters())), scores
        log(f"seed {seed} epoch {epoch + 1}/{args.epochs}  clean F1 {scores['clean']['man']:.3f} R2 "
            f"{scores['clean']['est']:.3f}  thinned F1 {scores['thinned']['man']:.3f} R2 {scores['thinned']['est']:.3f}"
            f"  best {best_score:.3f}  {time.time() - started:.0f}s")
    model.load_weights(list(best_weights.items()))
    model.eval()
    return model, best_score, best_scores


# ------------------------------------------------------------------------------------------------ main
def parse_args():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--data", default="data", help="folder with the downloaded HCRL data")
    p.add_argument("--config", default="configs/onboard.json")
    p.add_argument("--targets", default="../onboard/targets.json", help="estimate mean and std; computed if missing")
    p.add_argument("--init", help="weights to start from, and to score with --eval-only")
    p.add_argument("--name", default="onboard_v1")
    p.add_argument("--out", default="runs/onboard")
    p.add_argument("--epochs", type=int, default=60)
    p.add_argument("--seeds", type=int, default=3)
    p.add_argument("--batch", type=int, default=128)
    p.add_argument("--stride", type=int, default=1, help="rows between windows")
    p.add_argument("--hold", type=int, default=3, help="seconds a value is held when the adapter is slow")
    p.add_argument("--lr-scale", type=float, default=1.0, help="0.5 is a good choice when starting from --init")
    p.add_argument("--limit", type=int, default=0, help="windows per split, for a quick run")
    p.add_argument("--eval-only", action="store_true", help="score --init on the test split and stop")
    return p.parse_args()


def main():
    args = parse_args()
    hp = Hyperparameters(**json.loads(Path(args.config).read_text()))
    out_dir = Path(args.out) / args.name
    out_dir.mkdir(parents=True, exist_ok=True)
    log_file = open(out_dir / "train.log", "a")

    def log(message):
        print(message, flush=True)
        log_file.write(message + "\n")
        log_file.flush()

    runs = load_runs(args.data)
    norm = channel_norm(runs, "holdout")
    data = {part: make_windows(runs, norm, part, hp, stride=args.stride, limit=args.limit)
            for part in ("train", "val", "test")}
    mean, std = estimate_stats(args.targets, data["train"]["est"])
    for split in data.values():
        split["est_z"] = ((split["est"] - mean) / std).astype(np.float32)
    log(f"{args.name}: windows " + ", ".join(f"{k} {len(v['man'])}" for k, v in data.items()))

    views = {part: {"clean": data[part], "thinned": slow_adapter_view(data[part], args.hold)} for part in ("val", "test")}

    if args.eval_only:
        model = OnboardNet(hp)
        model.load_weights(args.init, strict=False)
        model.eval()
        for name, split in views["test"].items():
            log(f"test {name}: {evaluate(model, split)}")
        return

    results = [fit(hp, data["train"], views["val"], args, seed, log) for seed in range(args.seeds)]
    best = max(range(args.seeds), key=lambda i: results[i][1])
    model, _, val_scores = results[best]
    test_scores = {name: evaluate(model, split) for name, split in views["test"].items()}
    for name, scores in test_scores.items():
        log(f"test {name}: {scores}")

    mx.save_safetensors(str(out_dir / f"{args.name}_winner_qat.safetensors"), dict(tree_flatten(model.parameters())))
    config = {"winner": {"params": asdict(hp)}, "training": {"args": vars(args), "seed": best, "val": val_scores,
                                                              "test": test_scores}}
    (out_dir / f"{args.name}_final_qat.json").write_text(json.dumps(config, indent=1))
    log(f"saved to {out_dir}")


if __name__ == "__main__":
    main()
