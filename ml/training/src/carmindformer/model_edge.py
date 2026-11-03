"""CarMind-Edge: a small temporal CNN autoencoder for the ten on-device signals.

Built only from operators that ESP-DL quantises well (Conv with 1xk kernels, ReLU, average pooling,
fully connected), so the ONNX export runs on the ESP32-S3 (and S31) at int8.
Outputs
  z        [B, 16]  regime embedding (replaces the k-means centroid id; nearest stored prototype = pattern id)
  recon    [B,10,T] reconstruction; per-window error = novelty score
  change   [B, T/8] change score per 8-step patch (same target as the big model)
Input is a [B, 10, T] window of instance-normalised channels plus a [B, 10, T] observed mask, concatenated
to 20 input channels so missing PIDs are explicit.
"""
from __future__ import annotations
from dataclasses import dataclass

import torch
import torch.nn as nn
import torch.nn.functional as F


@dataclass
class EdgeConfig:
    window: int = 64
    n_channels: int = 10
    width: int = 24
    embed: int = 16
    patch: int = 8


class EdgeAE(nn.Module):
    def __init__(self, cfg: EdgeConfig):
        super().__init__()
        self.cfg = cfg
        c, w = cfg.n_channels, cfg.width
        # 2D convs with 1xk kernels over [B, C, 1, T] keep the graph inside ESP-DL's Conv2D support
        self.enc = nn.Sequential(
            nn.Conv2d(2 * c, w, (1, 5), padding=(0, 2)), nn.ReLU(),
            nn.Conv2d(w, w, (1, 5), stride=(1, 2), padding=(0, 2)), nn.ReLU(),       # T/2
            nn.Conv2d(w, 2 * w, (1, 5), stride=(1, 2), padding=(0, 2)), nn.ReLU(),   # T/4
            nn.Conv2d(2 * w, 2 * w, (1, 3), stride=(1, 2), padding=(0, 1)), nn.ReLU(),  # T/8
        )
        t8 = cfg.window // 8
        self.to_embed = nn.Linear(2 * w * t8, cfg.embed)
        self.from_embed = nn.Linear(cfg.embed, 2 * w * t8)
        self.dec = nn.Sequential(
            nn.ConvTranspose2d(2 * w, 2 * w, (1, 4), stride=(1, 2), padding=(0, 1)), nn.ReLU(),
            nn.ConvTranspose2d(2 * w, w, (1, 4), stride=(1, 2), padding=(0, 1)), nn.ReLU(),
            nn.ConvTranspose2d(w, w, (1, 4), stride=(1, 2), padding=(0, 1)), nn.ReLU(),
            nn.Conv2d(w, c, (1, 3), padding=(0, 1)),
        )
        self.change = nn.Conv2d(2 * w, 1, (1, 1))     # per T/8 step -> change score per patch

    def forward(self, x: torch.Tensor, m: torch.Tensor):
        B, C, T = x.shape
        h = torch.cat([x, m], 1).unsqueeze(2)         # [B, 2C, 1, T]
        f = self.enc(h)                                # [B, 2w, 1, T/8]
        z = self.to_embed(f.flatten(1))
        g = self.from_embed(z).reshape_as(f)
        recon = self.dec(g).squeeze(2)                 # [B, C, T]
        change = F.relu(self.change(f)).squeeze(2).squeeze(1)   # [B, T/8]
        return {"z": z, "recon": recon, "change": change}


def edge_losses(model: EdgeAE, batch: dict) -> tuple[torch.Tensor, dict]:
    out = model(batch["x"], batch["m"])
    m = batch["m"]
    recon = (((out["recon"] - batch["x"]) ** 2) * m).sum() / m.sum().clamp(min=1)
    # change target: mean over channels of |patch-mean delta|
    p = model.cfg.patch
    pm = batch["x"].reshape(batch["x"].shape[0], batch["x"].shape[1], -1, p).mean(-1)
    d = (pm[:, :, 1:] - pm[:, :, :-1]).abs().mean(1)
    tgt = torch.cat([torch.zeros_like(d[:, :1]), d], 1)
    change = F.smooth_l1_loss(out["change"], tgt)
    # keep the embedding bounded so prototypes stay comparable across devices
    reg = out["z"].pow(2).mean() * 1e-3
    total = recon + 0.5 * change + reg
    return total, {"recon": recon.item(), "change": change.item(), "total": total.item()}
