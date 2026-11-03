"""CarMind-Former: channel-agnostic patch transformer with dual (temporal + variate) attention.

Tokens: each channel's window is cut into patches (PatchTST, Nie et al. ICLR 2023). A patch token =
linear(patch values) + channel embedding (shared vocabulary) + patch position embedding
+ a projection of the channel's window statistics (mean, log std) so instance normalisation does
not throw away absolute level. Blocks alternate attention along time within a channel and
attention across channels at the same patch index (inverted/variate attention, iTransformer,
Liu et al. ICLR 2024; two-stage attention, Crossformer, Zhang & Yan ICLR 2023).

Heads
  recon   : masked patch reconstruction (values of masked patches), MSE on observed points
  change  : per patch, predicted |mean(patch t) - mean(patch t-1)| in standardised units
  cls     : window embedding from a learned CLS token, used for contrastive (NT-Xent) training
            between two crops of the same recording and for linear probes (driver id)
  attack  : per patch logit, trained only on windows that carry CAN attack flags
"""
from __future__ import annotations
import math
from dataclasses import dataclass

import torch
import torch.nn as nn
import torch.nn.functional as F

from .channels import VOCAB_SIZE


@dataclass
class FormerConfig:
    window: int = 256          # time steps per window
    patch: int = 8             # steps per patch -> 32 patches
    d_model: int = 192
    n_heads: int = 6
    n_blocks: int = 6
    d_ff: int = 512
    dropout: float = 0.1
    max_channels: int = 48
    mask_ratio: float = 0.4
    n_drivers: int = 10


class Block(nn.Module):
    """One dual-attention block: temporal attention (per channel) then variate attention (per patch)."""

    def __init__(self, cfg: FormerConfig):
        super().__init__()
        self.t_norm = nn.LayerNorm(cfg.d_model)
        self.t_attn = nn.MultiheadAttention(cfg.d_model, cfg.n_heads, dropout=cfg.dropout, batch_first=True)
        self.v_norm = nn.LayerNorm(cfg.d_model)
        self.v_attn = nn.MultiheadAttention(cfg.d_model, cfg.n_heads, dropout=cfg.dropout, batch_first=True)
        self.f_norm = nn.LayerNorm(cfg.d_model)
        self.ff = nn.Sequential(nn.Linear(cfg.d_model, cfg.d_ff), nn.GELU(), nn.Dropout(cfg.dropout), nn.Linear(cfg.d_ff, cfg.d_model))
        self.drop = nn.Dropout(cfg.dropout)

    def forward(self, h: torch.Tensor, cmask: torch.Tensor) -> torch.Tensor:
        # h: [B, C, P, D]; cmask: [B, C] 1 for real channels
        B, C, P, D = h.shape
        x = h.reshape(B * C, P, D)
        y = self.t_norm(x)
        y, _ = self.t_attn(y, y, y, need_weights=False)
        x = x + self.drop(y)
        x = x.reshape(B, C, P, D).permute(0, 2, 1, 3).reshape(B * P, C, D)       # variate attention
        pad = (cmask < 0.5).repeat_interleave(P, dim=0)                           # [B*P, C] True = ignore
        y = self.v_norm(x)
        y, _ = self.v_attn(y, y, y, key_padding_mask=pad, need_weights=False)
        x = x + self.drop(y)
        x = x + self.drop(self.ff(self.f_norm(x)))
        return x.reshape(B, P, C, D).permute(0, 2, 1, 3)


class CarMindFormer(nn.Module):
    def __init__(self, cfg: FormerConfig):
        super().__init__()
        assert cfg.window % cfg.patch == 0
        self.cfg = cfg
        P = cfg.window // cfg.patch
        self.patch_proj = nn.Linear(cfg.patch * 2, cfg.d_model)        # values and their observed-mask, interleaved
        self.chan_emb = nn.Embedding(VOCAB_SIZE, cfg.d_model)
        self.pos_emb = nn.Parameter(torch.zeros(1, 1, P, cfg.d_model))
        self.stats_proj = nn.Linear(2, cfg.d_model)
        self.mask_token = nn.Parameter(torch.zeros(1, 1, 1, cfg.d_model))
        self.cls = nn.Parameter(torch.zeros(1, 1, 1, cfg.d_model))      # one CLS "channel" with P copies
        self.blocks = nn.ModuleList([Block(cfg) for _ in range(cfg.n_blocks)])
        self.norm = nn.LayerNorm(cfg.d_model)
        self.recon_head = nn.Linear(cfg.d_model, cfg.patch)
        self.change_head = nn.Sequential(nn.Linear(cfg.d_model, cfg.d_model // 2), nn.GELU(), nn.Linear(cfg.d_model // 2, 1))
        self.attack_head = nn.Linear(cfg.d_model, 1)
        self.proj_head = nn.Sequential(nn.Linear(cfg.d_model, cfg.d_model), nn.GELU(), nn.Linear(cfg.d_model, 128))
        self.driver_head = nn.Linear(cfg.d_model, cfg.n_drivers)
        nn.init.trunc_normal_(self.pos_emb, std=0.02)
        nn.init.trunc_normal_(self.mask_token, std=0.02)
        nn.init.trunc_normal_(self.cls, std=0.02)

    # ---- helpers
    def patchify(self, x: torch.Tensor) -> torch.Tensor:
        B, C, T = x.shape
        return x.reshape(B, C, T // self.cfg.patch, self.cfg.patch)

    def forward(self, x, m, cid, cmask, stats, mask_ratio: float | None = None):
        """x,m: [B,C,T]; cid: [B,C]; cmask: [B,C]; stats: [B,C,2].
        Returns dict with recon [B,C,P,patch], change [B,C,P], attack [B,P], cls [B,D], driver [B,n], patch_mask [B,C,P]."""
        cfg = self.cfg
        B, C, T = x.shape
        xp, mp = self.patchify(x), self.patchify(m)                      # [B,C,P,patch]
        P = xp.shape[2]
        tok = self.patch_proj(torch.stack([xp, mp], -1).flatten(-2))    # [B,C,P,D]
        tok = tok + self.chan_emb(cid)[:, :, None, :] + self.pos_emb + self.stats_proj(stats)[:, :, None, :]

        ratio = cfg.mask_ratio if mask_ratio is None else mask_ratio
        if ratio > 0:
            pm = (torch.rand(B, C, P, device=x.device) < ratio) & (cmask[:, :, None] > 0.5)
            tok = torch.where(pm[..., None], self.mask_token.expand(B, C, P, -1) + self.chan_emb(cid)[:, :, None, :] + self.pos_emb, tok)
        else:
            pm = torch.zeros(B, C, P, dtype=torch.bool, device=x.device)

        cls_tok = (self.cls + self.pos_emb).expand(B, 1, P, -1)
        h = torch.cat([cls_tok, tok], dim=1)                             # CLS channel first
        cm = torch.cat([torch.ones(B, 1, device=x.device), cmask], dim=1)
        for blk in self.blocks:
            h = blk(h, cm)
        h = self.norm(h)
        cls = h[:, 0].mean(dim=1)                                        # [B,D] pooled over patches
        body = h[:, 1:]
        return {
            "recon": self.recon_head(body),
            "change": self.change_head(body).squeeze(-1),
            "attack": self.attack_head(body).squeeze(-1).masked_fill(cmask[:, :, None] < 0.5, 0).sum(1) / cmask.sum(1, keepdim=True).clamp(min=1),
            "cls": cls,
            "proj": F.normalize(self.proj_head(cls), dim=-1),
            "driver": self.driver_head(cls),
            "patch_mask": pm,
        }


def change_targets(x: torch.Tensor, patch: int) -> torch.Tensor:
    """|mean(patch t) - mean(patch t-1)| per channel, 0 for the first patch. x: [B,C,T] standardised."""
    pm = x.reshape(x.shape[0], x.shape[1], -1, patch).mean(-1)
    d = (pm[:, :, 1:] - pm[:, :, :-1]).abs()
    return torch.cat([torch.zeros_like(d[:, :, :1]), d], dim=2)


def nt_xent(a: torch.Tensor, b: torch.Tensor, temperature: float = 0.1) -> torch.Tensor:
    """SimCLR loss between two L2-normalised views [B,D] (Chen et al. ICML 2020)."""
    B = a.shape[0]
    z = torch.cat([a, b], 0)
    sim = z @ z.t() / temperature
    sim.fill_diagonal_(-1e9)
    target = torch.cat([torch.arange(B, 2 * B), torch.arange(0, B)]).to(a.device)
    return F.cross_entropy(sim, target)


def former_losses(model: CarMindFormer, batch: dict, w: dict) -> tuple[torch.Tensor, dict]:
    """Pretraining losses on a paired batch {a, b}."""
    cfg = model.cfg
    out_a = model(batch["a"]["x"], batch["a"]["m"], batch["a"]["cid"], batch["a"]["cmask"], batch["a"]["stats"])
    out_b = model(batch["b"]["x"], batch["b"]["m"], batch["b"]["cid"], batch["b"]["cmask"], batch["b"]["stats"])
    logs = {}
    total = 0.0
    for name, out, side in (("a", out_a, batch["a"]), ("b", out_b, batch["b"])):
        xp = model.patchify(side["x"])
        mp = model.patchify(side["m"])
        sel = out["patch_mask"][..., None] & (mp > 0.5)
        recon = ((out["recon"] - xp) ** 2)[sel].mean() if sel.any() else out["recon"].sum() * 0
        tgt = change_targets(side["x"], cfg.patch)
        valid = side["cmask"][:, :, None].expand_as(tgt) > 0.5
        change = F.smooth_l1_loss(out["change"][valid], tgt[valid])
        total = total + w["recon"] * recon + w["change"] * change
        logs[f"recon_{name}"] = recon.item()
        logs[f"change_{name}"] = change.item()
        if w.get("attack", 0) > 0 and side["has_attack"].sum() > 0:
            at = side["attack"].reshape(side["attack"].shape[0], -1, cfg.patch).max(-1).values
            sel_b = side["has_attack"] > 0.5
            attack = F.binary_cross_entropy_with_logits(out["attack"][sel_b], at[sel_b])
            total = total + w["attack"] * attack
            logs[f"attack_{name}"] = attack.item()
        if w.get("driver", 0) > 0 and (side["driver"] >= 0).sum() > 0:
            sel_d = side["driver"] >= 0
            drv = F.cross_entropy(out["driver"][sel_d], side["driver"][sel_d])
            total = total + w["driver"] * drv
            logs[f"driver_{name}"] = drv.item()
    con = nt_xent(out_a["proj"], out_b["proj"])
    total = total + w["contrast"] * con
    logs["contrast"] = con.item()
    logs["total"] = float(total.detach())
    return total, logs
