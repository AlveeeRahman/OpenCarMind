"""CarMind-Edge-S: supervised, quantization-aware TCN embedding model for the ESP32-S31.

Recipe follows TEMPEST (arXiv:2610.06855): a temporal convolutional encoder with exponentially dilated
layers, global average pooling, an L2-normalised embedding, and an ArcFace (additive angular margin,
Deng et al. CVPR 2019) classification head. TEMPEST reports 62-66 % on the KIA Soul cross-session split
with 720 k parameters; this model aims at the same protocol with about 40 k int8 parameters so it fits the
S31's 512 KB SRAM with room for the firmware.

All layers are 1xk Conv2d / Linear built from the QAT wrappers in qat.py, so the exported ONNX is the
graph ESP-PPQ expects. Dilation is expressed as Conv2d dilation, which ESP-DL supports.
"""
from __future__ import annotations
from dataclasses import dataclass
import math

import torch
import torch.nn as nn
import torch.nn.functional as F

from .qat import QConv2d, QLinear


@dataclass
class EdgeSupConfig:
    window: int = 60
    n_channels: int = 10
    width: int = 32
    layers: int = 5           # dilations 1,2,4,8,16, two convs per block -> receptive field 125 steps
    kernel: int = 3
    embed: int = 48
    n_classes: int = 10
    arcface_s: float = 20.0
    arcface_m: float = 0.21
    dropout: float = 0.1


class TCNBlock(nn.Module):
    def __init__(self, c: int, k: int, d: int, p: float):
        super().__init__()
        pad = (k - 1) * d // 2
        self.c1 = QConv2d(c, c, (1, k), padding=(0, pad), dilation=(1, d))
        self.c2 = QConv2d(c, c, (1, k), padding=(0, pad), dilation=(1, d))
        self.drop = nn.Dropout(p)

    def forward(self, x):
        y = self.drop(F.relu(self.c1(x)))
        y = self.c2(y)
        return F.relu(x + y)


class ArcFace(nn.Module):
    """Additive angular margin (Deng et al. 2019). cos(theta + m) is only a penalty while theta < pi - m;
    past that it rewards the wrong direction and training collapses to theta = pi (loss ~1.9 for 10 classes).
    The InsightFace fix: beyond the threshold use cos(theta) - m*sin(m) instead."""

    def __init__(self, embed: int, n_classes: int, s: float, m: float):
        super().__init__()
        self.W = nn.Parameter(torch.randn(n_classes, embed) * 0.01)
        self.s, self.m = s, m
        self.cos_m, self.sin_m = math.cos(m), math.sin(m)
        self.th, self.mm = math.cos(math.pi - m), math.sin(math.pi - m) * m

    def forward(self, e: torch.Tensor, y: torch.Tensor | None = None):
        cos = F.normalize(e) @ F.normalize(self.W).t()
        if y is None:
            return self.s * cos
        sin = torch.sqrt((1.0 - cos * cos).clamp(min=1e-6))
        phi = cos * self.cos_m - sin * self.sin_m
        phi = torch.where(cos > self.th, phi, cos - self.mm)
        onehot = F.one_hot(y, cos.shape[1]).bool()
        return self.s * torch.where(onehot, phi, cos)


class EdgeSupNet(nn.Module):
    def __init__(self, cfg: EdgeSupConfig):
        super().__init__()
        self.cfg = cfg
        self.stem = QConv2d(2 * cfg.n_channels, cfg.width, (1, 1))
        self.blocks = nn.ModuleList([TCNBlock(cfg.width, cfg.kernel, 2 ** i, cfg.dropout) for i in range(cfg.layers)])
        self.proj = QLinear(cfg.width, cfg.embed)
        self.head = ArcFace(cfg.embed, cfg.n_classes, cfg.arcface_s, cfg.arcface_m)
        self.recon = QConv2d(cfg.width, cfg.n_channels, (1, 1))     # keeps the autoencoder novelty score

    def embed(self, x: torch.Tensor, m: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        h = torch.cat([x, m], 1).unsqueeze(2)      # [B, 2C, 1, T]
        h = F.relu(self.stem(h))
        for b in self.blocks:
            h = b(h)
        pooled = h.mean(dim=(2, 3))                # global average pool
        return F.normalize(self.proj(pooled), dim=-1), h

    def forward(self, x, m, y=None):
        e, h = self.embed(x, m)
        return {"embed": e, "logits": self.head(e, y), "recon": self.recon(h).squeeze(2)}
