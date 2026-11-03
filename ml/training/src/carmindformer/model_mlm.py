"""CarMind-MLM: a BERT-style encoder over discrete vehicle tokens (CAN frames and, through tokens.py,
binned OBD signals). Pretraining = masked token prediction (Devlin et al. 2019); fine-tuning adds a window
head (attack type or driver) and a per-frame head (injected or not). Rotary or learned positions are both
fine at these lengths; learned positions are used for ESP-friendliness of a possible small variant.
"""
from __future__ import annotations
from dataclasses import dataclass

import torch
import torch.nn as nn
import torch.nn.functional as F

from .can_frames import CLS, MASK, OBD_BASE, PAD


@dataclass
class MLMConfig:
    vocab: int = OBD_BASE + 48 * 64   # CAN tokens + 48 channels x 64 bins
    max_len: int = 1 + 29 * 10        # CLS + frames
    d_model: int = 256
    n_heads: int = 8
    n_layers: int = 6
    d_ff: int = 1024
    dropout: float = 0.1
    n_window_classes: int = 5
    tokens_per_frame: int = 10


class CarMindMLM(nn.Module):
    def __init__(self, cfg: MLMConfig):
        super().__init__()
        self.cfg = cfg
        self.tok = nn.Embedding(cfg.vocab, cfg.d_model, padding_idx=PAD)
        self.pos = nn.Embedding(cfg.max_len, cfg.d_model)
        self.field = nn.Embedding(cfg.tokens_per_frame + 1, cfg.d_model)   # position inside a frame: 0 CLS, 1 ID, 2 DLC, 3..10 bytes
        layer = nn.TransformerEncoderLayer(cfg.d_model, cfg.n_heads, cfg.d_ff, cfg.dropout, activation="gelu", batch_first=True, norm_first=True)
        self.enc = nn.TransformerEncoder(layer, cfg.n_layers)
        self.norm = nn.LayerNorm(cfg.d_model)
        self.mlm_head = nn.Linear(cfg.d_model, cfg.vocab)
        self.win_head = nn.Linear(cfg.d_model, cfg.n_window_classes)
        self.frame_head = nn.Linear(cfg.d_model, 1)

    def forward(self, tokens: torch.Tensor):
        """tokens: [B, L] without CLS; CLS is prepended here."""
        B, L = tokens.shape
        x = torch.cat([torch.full((B, 1), CLS, dtype=tokens.dtype, device=tokens.device), tokens], 1)
        pos = torch.arange(x.shape[1], device=x.device)
        field = torch.cat([torch.zeros(1, dtype=torch.long, device=x.device), (torch.arange(L, device=x.device) % self.cfg.tokens_per_frame) + 1])
        h = self.tok(x) + self.pos(pos)[None] + self.field(field)[None]
        h = self.enc(h, src_key_padding_mask=(x == PAD))
        h = self.norm(h)
        cls = h[:, 0]
        body = h[:, 1:]
        frames = body.reshape(B, L // self.cfg.tokens_per_frame, self.cfg.tokens_per_frame, -1)[:, :, 0]   # ID token of each frame
        return {"hidden": body, "cls": cls, "mlm": self.mlm_head(body), "window": self.win_head(cls), "frame": self.frame_head(frames).squeeze(-1)}


def mask_tokens(tokens: torch.Tensor, ratio: float = 0.15):
    """BERT masking on non-pad tokens: 80 % MASK, 10 % random, 10 % keep. Returns inputs and targets (-100 where not masked)."""
    sel = (torch.rand_like(tokens, dtype=torch.float) < ratio) & (tokens != PAD)
    targets = torch.where(sel, tokens, torch.full_like(tokens, -100))
    r = torch.rand_like(tokens, dtype=torch.float)
    inp = tokens.clone()
    inp[sel & (r < 0.8)] = MASK
    rnd = sel & (r >= 0.8) & (r < 0.9)
    inp[rnd] = torch.randint(4, int(tokens.max().item()) + 1, (int(rnd.sum()),), device=tokens.device)
    return inp, targets
