"""Frame-level CAN data for the big model: each frame is tokenised as [ID][DLC][B0..B7] (10 tokens),
a sample is a sequence of N consecutive frames (N = 29 is the convention from Song et al. 2020 that the
Car-Hacking leaderboard uses), labelled by attack type if any injected frame is inside.

Vocabulary layout (shared with the OBD bin tokens in tokens.py):
  0 PAD, 1 MASK, 2 CLS, 3 SEP
  4 .. 4+2048       CAN ID (11-bit)
  2052 .. 2052+9    DLC 0..8 + unknown
  2062 .. 2062+256  byte value
  2318 ..           OBD bin tokens (channel_id * n_bins + bin), see tokens.py
"""
from __future__ import annotations
import os
import random
from dataclasses import dataclass
from typing import List, Optional

import numpy as np
import pandas as pd
import torch
from torch.utils.data import Dataset

PAD, MASK, CLS, SEP = 0, 1, 2, 3
ID_BASE, DLC_BASE, BYTE_BASE = 4, 4 + 2048, 4 + 2048 + 10
OBD_BASE = BYTE_BASE + 256
ATTACK_NAMES = ["normal", "dos", "fuzzy", "gear", "rpm"]


def attack_class_from_name(path: str) -> int:
    n = os.path.basename(path).lower()
    for i, k in enumerate(ATTACK_NAMES):
        if i and k in n:
            return i
    return 0


@dataclass
class FrameLog:
    ids: np.ndarray       # [N] int (0..2047)
    dlc: np.ndarray       # [N] int
    data: np.ndarray      # [N, 8] int, 256 = absent
    flag: np.ndarray      # [N] 1 injected
    attack_class: int
    name: str


def load_frames(path: str, max_rows: Optional[int] = None) -> FrameLog:
    """Car-Hacking CSV (no header: ts, id hex, dlc, bytes..., flag) or HCRL normal_run text."""
    cls = attack_class_from_name(path)
    if path.endswith(".txt"):
        import re
        rx = re.compile(r"Timestamp:\s*\S+\s+ID:\s*(\S+)\s+\d{3}\s+DLC:\s*(\d)\s*(.*)")
        ids, dlcs, datas = [], [], []
        with open(path) as f:
            for i, line in enumerate(f):
                if max_rows and i >= max_rows:
                    break
                m = rx.match(line)
                if not m:
                    continue
                d = int(m.group(2)); b = [int(x, 16) for x in m.group(3).split()[:d]]
                ids.append(int(m.group(1), 16)); dlcs.append(d); datas.append(b + [256] * (8 - len(b)))
        flag = np.zeros(len(ids), dtype=np.int8)
    else:
        df = pd.read_csv(path, header=None, nrows=max_rows, on_bad_lines="skip", dtype=str)
        ids, dlcs, datas, flags = [], [], [], []
        for r in df.itertuples(index=False):
            try:
                d = int(r[2])
                cells = [int(str(c), 16) for c in r[3:3 + d]]
            except (ValueError, TypeError):
                continue
            fl = r[3 + d] if 3 + d < len(r) else "R"
            ids.append(int(str(r[1]), 16) & 0x7FF); dlcs.append(d); datas.append(cells + [256] * (8 - d)); flags.append(1 if str(fl).strip().upper() == "T" else 0)
        flag = np.array(flags, dtype=np.int8)
    return FrameLog(np.array(ids, dtype=np.int32), np.array(dlcs, dtype=np.int8), np.array(datas, dtype=np.int16), flag, cls, os.path.basename(path))


def frames_to_tokens(log: FrameLog, start: int, n: int) -> np.ndarray:
    ids = log.ids[start:start + n]; dlc = log.dlc[start:start + n]; data = log.data[start:start + n]
    tok = np.empty((n, 10), dtype=np.int64)
    tok[:, 0] = ID_BASE + ids.astype(np.int64)
    tok[:, 1] = DLC_BASE + np.clip(dlc.astype(np.int64), 0, 9)
    tok[:, 2:] = np.where(data < 256, BYTE_BASE + data.astype(np.int64), PAD)
    return tok.reshape(-1)


class CanFrameDataset(Dataset):
    """Windows of `n_frames` consecutive frames. Chronological split per file: train = first 64 %,
    val = next 16 %, test = last 20 %, so no evaluation window overlaps training time."""

    def __init__(self, logs: List[FrameLog], n_frames: int = 29, part: str = "train",
                 samples: int = 20000, seed: int = 0, stride_eval: Optional[int] = None):
        self.logs, self.n, self.rng = logs, n_frames, random.Random(seed)
        self.ranges = []
        for L in logs:
            N = len(L.ids); a, b = int(N * 0.64), int(N * 0.8)
            lo, hi = {"train": (0, a), "val": (a, b), "test": (b, N)}[part]
            if hi - lo > n_frames:
                self.ranges.append((L, lo, hi))
        self.samples = samples
        self.eval_index = None
        if stride_eval:
            self.eval_index = [(L, s) for (L, lo, hi) in self.ranges for s in range(lo, hi - n_frames, stride_eval)]

    def __len__(self):
        return len(self.eval_index) if self.eval_index is not None else self.samples

    def __getitem__(self, i):
        if self.eval_index is not None:
            L, s = self.eval_index[i]
        else:
            L, lo, hi = self.rng.choice(self.ranges)
            s = self.rng.randint(lo, hi - self.n - 1)
        tok = frames_to_tokens(L, s, self.n)
        fl = L.flag[s:s + self.n]
        y = L.attack_class if fl.any() else 0
        return {"tokens": torch.from_numpy(tok), "frame_flag": torch.from_numpy(fl.astype(np.float32)),
                "label": torch.tensor(y), "has_label": torch.tensor(1.0)}
