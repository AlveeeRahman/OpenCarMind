"""OBD value tokeniser for the MLM: per-channel quantile bins fitted on training runs. A window of
[C, T] values becomes T frames of C tokens (one token per channel per step), so the same encoder reads
OBD windows and CAN frame windows. Missing values map to PAD."""
from __future__ import annotations
import json
from typing import Dict, List

import numpy as np

from .can_frames import OBD_BASE, PAD
from .channels import channel_id


class ObdTokenizer:
    def __init__(self, n_bins: int = 64, max_channels: int = 48):
        self.n_bins, self.max_channels = n_bins, max_channels
        self.edges: Dict[int, np.ndarray] = {}

    def fit(self, series) -> "ObdTokenizer":
        pool: Dict[int, List[np.ndarray]] = {}
        for s in series:
            for i, c in enumerate(s.channels):
                cid = channel_id(c)
                if cid < self.max_channels:
                    v = s.values[i]; v = v[~np.isnan(v)]
                    if len(v):
                        pool.setdefault(cid, []).append(v[:: max(1, len(v) // 20000)])
        for cid, vs in pool.items():
            v = np.concatenate(vs)
            self.edges[cid] = np.unique(np.quantile(v, np.linspace(0, 1, self.n_bins + 1)[1:-1]))
        return self

    def encode(self, values: np.ndarray, channels: List[str]) -> np.ndarray:
        """values [C, T] -> tokens [T, C] (frame-major)."""
        C, T = values.shape
        out = np.full((T, C), PAD, dtype=np.int64)
        for i, c in enumerate(channels):
            cid = channel_id(c)
            if cid not in self.edges:
                continue
            b = np.searchsorted(self.edges[cid], values[i], side="right")
            tok = OBD_BASE + cid * self.n_bins + np.clip(b, 0, self.n_bins - 1)
            out[:, i] = np.where(np.isnan(values[i]), PAD, tok)
        return out

    def save(self, path: str):
        json.dump({"n_bins": self.n_bins, "edges": {str(k): v.tolist() for k, v in self.edges.items()}}, open(path, "w"))

    @classmethod
    def load(cls, path: str) -> "ObdTokenizer":
        d = json.load(open(path)); t = cls(d["n_bins"]); t.edges = {int(k): np.array(v) for k, v in d["edges"].items()}; return t
