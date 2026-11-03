"""Train CarMind-Former (big, CUDA) or CarMind-Edge (small, on-device).

  python -m carmindformer.train --model former --config configs/former.yaml --data data
  python -m carmindformer.train --model edge   --config configs/edge.yaml   --data data
  python -m carmindformer.train --model edge   --synthetic --smoke          # 1-minute CPU check
"""
from __future__ import annotations
import argparse
import csv
import json
import os
import time

import torch
import yaml
from torch.utils.data import DataLoader

from .data import EdgeWindowDataset, WindowDataset, discover, synthetic_series
from .model_edge import EdgeAE, EdgeConfig, edge_losses
from .model_former import CarMindFormer, FormerConfig, former_losses


def pick_device(name: str | None) -> torch.device:
    if name:
        return torch.device(name)
    if torch.cuda.is_available():
        return torch.device("cuda")
    if torch.backends.mps.is_available():
        return torch.device("mps")
    return torch.device("cpu")


def to_device(batch, device):
    if isinstance(batch, dict):
        return {k: to_device(v, device) for k, v in batch.items()}
    return batch.to(device, non_blocking=True)


def load_series(args):
    if args.synthetic:
        s = synthetic_series(n=12, length=4000)
        return {"driving": s, "can": [], "carmind": []}
    found = discover(args.data, max_can_rows=args.max_can_rows)
    for k, v in found.items():
        print(f"{k}: {len(v)} series, {sum(x.values.shape[1] for x in v):,} steps")
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", choices=["former", "edge"], default="former")
    ap.add_argument("--config", default=None)
    ap.add_argument("--data", default="data")
    ap.add_argument("--out", default="runs")
    ap.add_argument("--device", default=None)
    ap.add_argument("--synthetic", action="store_true")
    ap.add_argument("--smoke", action="store_true", help="tiny model, few steps")
    ap.add_argument("--max-can-rows", type=int, default=None)
    ap.add_argument("--resume", default=None)
    args = ap.parse_args()

    cfg = yaml.safe_load(open(args.config)) if args.config else {}
    train_cfg = cfg.get("train", {})
    if args.smoke:
        train_cfg.update({"epochs": 1, "steps_per_epoch": 30, "batch_size": 8})
    epochs = train_cfg.get("epochs", 30)
    steps = train_cfg.get("steps_per_epoch", 500)
    bs = train_cfg.get("batch_size", 64)
    lr = train_cfg.get("lr", 3e-4)
    device = pick_device(args.device)
    print(f"device: {device}")

    found = load_series(args)
    run_dir = os.path.join(args.out, f"{args.model}-{time.strftime('%Y%m%d-%H%M%S')}")
    os.makedirs(run_dir, exist_ok=True)

    if args.model == "former":
        mc = FormerConfig(**cfg.get("model", {}))
        if args.smoke:
            mc = FormerConfig(window=64, patch=8, d_model=32, n_heads=4, n_blocks=2, d_ff=64, max_channels=12)
        series = found["driving"] + found["can"] + found["carmind"]
        # CAN rate series run at 10 Hz: the same 256-step window is 25.6 s there and 256 s on OBD data,
        # which is intended: attacks are fast, driving regimes are slow
        ds = WindowDataset(series, window=mc.window, max_channels=mc.max_channels, samples_per_epoch=steps * bs, pairs=True)
        model = CarMindFormer(mc).to(device)
        weights = cfg.get("loss", {"recon": 1.0, "change": 1.0, "contrast": 0.5, "attack": 1.0, "driver": 0.2})
        loss_fn = lambda b: former_losses(model, b, weights)
        num_workers = train_cfg.get("workers", 4)
    else:
        mc = EdgeConfig(**cfg.get("model", {}))
        series = found["driving"] + found["carmind"]              # Kind 1 only
        ds = EdgeWindowDataset(series, window=mc.window, samples_per_epoch=steps * bs)
        model = EdgeAE(mc).to(device)
        loss_fn = lambda b: edge_losses(model, b)
        num_workers = train_cfg.get("workers", 2)

    n_params = sum(p.numel() for p in model.parameters())
    print(f"{args.model}: {n_params:,} parameters")
    json.dump({"model": args.model, "config": mc.__dict__, "params": n_params, "train": train_cfg}, open(os.path.join(run_dir, "config.json"), "w"), indent=2)

    dl = DataLoader(ds, batch_size=bs, num_workers=0 if args.smoke else num_workers, drop_last=True, persistent_workers=False)
    opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=train_cfg.get("weight_decay", 0.05), betas=(0.9, 0.95))
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=lr, total_steps=epochs * steps, pct_start=0.05)
    use_amp = device.type == "cuda"
    scaler = torch.amp.GradScaler("cuda", enabled=use_amp)
    start_epoch = 0
    if args.resume:
        ck = torch.load(args.resume, map_location=device)
        model.load_state_dict(ck["model"]); opt.load_state_dict(ck["opt"]); start_epoch = ck["epoch"] + 1

    log_path = os.path.join(run_dir, "log.csv")
    with open(log_path, "a", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["epoch", "step", "lr", "loss", "detail"])
        step_global = start_epoch * steps
        for epoch in range(start_epoch, epochs):
            model.train()
            t0 = time.time()
            running = 0.0
            for i, batch in enumerate(dl):
                if i >= steps:
                    break
                batch = to_device(batch, device)
                with torch.autocast(device_type=device.type, dtype=torch.bfloat16, enabled=use_amp):
                    loss, logs = loss_fn(batch)
                opt.zero_grad(set_to_none=True)
                scaler.scale(loss).backward()
                scaler.unscale_(opt)
                torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
                scaler.step(opt)
                scaler.update()
                if step_global < sched.total_steps - 1:
                    sched.step()
                step_global += 1
                running += loss.item()
                if i % 20 == 0:
                    wr.writerow([epoch, step_global, sched.get_last_lr()[0], loss.item(), json.dumps(logs)])
                    f.flush()
                    print(f"epoch {epoch} step {i}/{steps} loss {loss.item():.4f} {json.dumps({k: round(v, 4) for k, v in logs.items() if k != 'total'})}")
            print(f"epoch {epoch} mean loss {running / max(1, min(i + 1, steps)):.4f} ({time.time() - t0:.0f}s)")
            torch.save({"model": model.state_dict(), "opt": opt.state_dict(), "epoch": epoch, "config": mc.__dict__}, os.path.join(run_dir, "last.pt"))
    print(f"saved {run_dir}/last.pt")


if __name__ == "__main__":
    main()
