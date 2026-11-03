"""PyTorch twin of the on-board network found by nas_edge.py (manoeuvre and estimator heads), for ONNX / ESP-PPQ
export and for cloud fine-tuning.

Contract with the firmware (one inference per 10 s board window):
  input  xm [1, 30, 1, W] float: 10 standardised OBD channels (norm.json order = data.EDGE_CHANNELS), their 10 validity
         masks, and 10 sample-to-sample differences (x[t] - x[t-1], 0 at t = 0, times the mask). Oldest sample first.
         Channels marked trained=false in norm.json (MAF and intake air temperature for the KIA-trained model) are
         always fed as missing, even when the car reports them: the model never saw them valid.
  output man [1, 4] scores for stopped / braking / accelerating / cruising
         est [1, 3] standardised brake pressure, flywheel torque, fuel rate (de-standardise with targets.json)

Graph uses Conv (standard and depthwise), Relu, Add, ReduceMean, Concat and Gemm only. The mean over the last 10
samples is a depthwise convolution with fixed weights, so no Slice is needed on the chip.
"""
from __future__ import annotations

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

from .qat import QConv2d, QLinear, set_quant

LAST = 10


class Block(nn.Module):
    def __init__(self, c, k, d, kind):
        super().__init__()
        pad = (0, (k - 1) * d // 2)
        if kind == "std":
            self.l = nn.ModuleList([QConv2d(c, c, (1, k), padding=pad, dilation=(1, d)),
                                    QConv2d(c, c, (1, k), padding=pad, dilation=(1, d))])
        else:
            self.l = nn.ModuleList([QConv2d(c, c, (1, k), padding=pad, dilation=(1, d), groups=c), QConv2d(c, c, (1, 1)),
                                    QConv2d(c, c, (1, k), padding=pad, dilation=(1, d), groups=c), QConv2d(c, c, (1, 1))])
        self.kind = kind

    def forward(self, x):
        y = x
        for i, conv in enumerate(self.l):
            y = conv(y)
            if i < len(self.l) - 1 and (self.kind == "std" or i % 2 == 1):
                y = F.relu(y)
        return F.relu(x + y)


class OnboardNet(nn.Module):
    def __init__(self, a: dict, n_man: int = 4, n_est: int = 3):
        super().__init__()
        self.a = a
        c, W = a["width"], a["W"]
        n_in = 30 if a["deltas"] else 20
        self.stem = QConv2d(n_in, c, (1, 1))
        self.blocks = nn.ModuleList([Block(c, a["kernel"], a["dil_growth"] ** i, a["block"]) for i in range(a["layers"])])
        last = torch.zeros(c, 1, 1, W); last[..., W - LAST:] = 1.0 / LAST
        self.register_buffer("last_w", last)                 # fixed depthwise kernel: mean of the last 10 samples
        self.local = QLinear(2 * c, a["local"])
        self.man = QLinear(a["local"], n_man)
        self.est = QLinear(a["local"], n_est)

    def forward(self, xm):
        h = F.relu(self.stem(xm))
        for b in self.blocks:
            h = b(h)
        g = h.mean(dim=(2, 3))                                # whole context
        l10 = F.conv2d(h, self.last_w, groups=h.shape[1]).flatten(1)
        z = F.relu(self.local(torch.cat([l10, g], 1)))
        return self.man(z), self.est(z)


def make_input(x: np.ndarray, m: np.ndarray, deltas: bool = True) -> np.ndarray:
    """x, m [N, 10, W] -> xm [N, 30, 1, W] exactly as the firmware must build it."""
    parts = [x, m]
    if deltas:
        dx = np.concatenate([np.zeros_like(x[:, :, :1]), x[:, :, 1:] - x[:, :, :-1]], axis=2) * m
        parts.append(dx)
    return np.concatenate(parts, axis=1)[:, :, None, :].astype(np.float32)


def load_mlx(path: str, a: dict, quant: bool = True) -> OnboardNet:
    """Load nas_edge MLX weights (conv [out, k, in/groups]) into the PyTorch twin ([out, in/groups, 1, k])."""
    from safetensors.numpy import load_file
    w = load_file(path)
    net = OnboardNet(a)
    sd = {}
    for k, v in w.items():
        if k.startswith("drv"):
            continue                                          # driver head is not part of the on-board model
        t = torch.from_numpy(np.array(v))
        if k.endswith(".weight") and t.ndim == 3:
            t = t.permute(0, 2, 1).unsqueeze(2).contiguous()
        sd[k.replace(".l.", ".l.")] = t
    missing, unexpected = net.load_state_dict(sd, strict=False)
    missing = [m for m in missing if not m.endswith(("running_max", "steps", "last_w"))]
    assert not missing and not unexpected, (missing, unexpected)
    set_quant(net, False)
    net.eval()
    return net
