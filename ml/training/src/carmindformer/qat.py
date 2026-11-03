"""Quantization-aware training that matches ESP-DL's scheme: symmetric int8, per-tensor,
power-of-two scales (ESP-PPQ: "symmetric quantization + POWER OF TWO"). Weights and activations are
fake-quantised in the forward pass with a straight-through estimator, so the float model that reaches
ESP-PPQ already lives on the int8 grid and post-training calibration loses almost nothing.
"""
from __future__ import annotations
import math

import torch
import torch.nn as nn


class _RoundSTE(torch.autograd.Function):
    @staticmethod
    def forward(ctx, x):
        return torch.round(x)

    @staticmethod
    def backward(ctx, g):
        return g


def pow2_scale(max_abs: torch.Tensor, bits: int = 8) -> torch.Tensor:
    """Smallest power-of-two scale that covers max_abs with a signed `bits` integer."""
    qmax = 2 ** (bits - 1) - 1
    s = (max_abs.clamp(min=1e-8) / qmax)
    return torch.pow(2.0, torch.ceil(torch.log2(s)))


def fake_quant(x: torch.Tensor, scale: torch.Tensor, bits: int = 8) -> torch.Tensor:
    qmax = 2 ** (bits - 1) - 1
    return torch.clamp(_RoundSTE.apply(x / scale), -qmax - 1, qmax) * scale


class ActQuant(nn.Module):
    """Activation fake-quant with an EMA of the observed max; frozen after warm-up."""

    def __init__(self, bits: int = 8, momentum: float = 0.99, warmup_steps: int = 200):
        super().__init__()
        self.bits, self.momentum, self.warmup = bits, momentum, warmup_steps
        self.register_buffer("running_max", torch.tensor(0.0))
        self.register_buffer("steps", torch.tensor(0))
        self.enabled = True

    def forward(self, x):
        if not self.enabled:
            return x
        if self.training and self.steps < self.warmup:
            m = x.detach().abs().max()
            self.running_max = m if self.steps == 0 else self.momentum * self.running_max + (1 - self.momentum) * m
            self.steps += 1
            return x
        if self.training:
            m = x.detach().abs().max()
            self.running_max = self.momentum * self.running_max + (1 - self.momentum) * m
        return fake_quant(x, pow2_scale(self.running_max, self.bits), self.bits)


class QConv2d(nn.Conv2d):
    def __init__(self, *a, bits: int = 8, **k):
        super().__init__(*a, **k)
        self.bits = bits
        self.act = ActQuant(bits)
        self.quant_enabled = True

    def forward(self, x):
        w = self.weight
        if self.quant_enabled:
            w = fake_quant(w, pow2_scale(w.detach().abs().max(), self.bits), self.bits)
        y = nn.functional.conv2d(x, w, self.bias, self.stride, self.padding, self.dilation, self.groups)
        return self.act(y) if self.quant_enabled else y


class QLinear(nn.Linear):
    def __init__(self, *a, bits: int = 8, **k):
        super().__init__(*a, **k)
        self.bits = bits
        self.act = ActQuant(bits)
        self.quant_enabled = True

    def forward(self, x):
        w = self.weight
        if self.quant_enabled:
            w = fake_quant(w, pow2_scale(w.detach().abs().max(), self.bits), self.bits)
        y = nn.functional.linear(x, w, self.bias)
        return self.act(y) if self.quant_enabled else y


def set_quant(model: nn.Module, enabled: bool):
    for m in model.modules():
        if isinstance(m, (QConv2d, QLinear)):
            m.quant_enabled = enabled
        if isinstance(m, ActQuant):
            m.enabled = enabled


def int8_size_bytes(model: nn.Module) -> int:
    """Weight bytes at int8 plus int32 biases, the number that matters for the chip."""
    n = 0
    for m in model.modules():
        if isinstance(m, (nn.Conv2d, nn.Linear)):
            n += m.weight.numel() + (m.bias.numel() * 4 if m.bias is not None else 0)
    return n
