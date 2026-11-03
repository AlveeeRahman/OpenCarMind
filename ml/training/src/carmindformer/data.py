"""Datasets: everything becomes (values[C, T], mask[C, T], channel_ids[C], labels) windows.

Sources
  CarMind CSV      sNNNNNN.csv from the logger (schema/obd_log_schema.json), resampled to 1 Hz
  HCRL Driving     one CSV, ~1 Hz rows, 10 drivers (column "Class" A..J), decoded OBD signals
  HCRL CAN logs    Timestamp, CAN ID, DLC, DATA[0..7], Flag -> per-ID message-rate series at 10 Hz
  Synthetic        regime-switching toy series for smoke tests

Windows are random crops. Values are instance-normalised per channel per window (RevIN style:
Kim et al., ICLR 2022); the per-channel mean and std are kept as two extra features so the model
still knows "this is a high-rpm window".
"""
from __future__ import annotations
import glob
import math
import os
import random
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np
import pandas as pd
import torch
from torch.utils.data import Dataset

from .channels import HCRL_UNIT_SCALE, canonical_name, channel_id, normalize_name

EDGE_CHANNELS = ["rpm", "speed_kph", "engine_load_pct", "maf_gps", "throttle_pct", "stft_b1_pct",
                 "ltft_b1_pct", "coolant_c", "intake_temp_c", "map_kpa"]   # same ten signals as main/pattern.c


@dataclass
class Series:
    """One continuous recording: values[C, T] float32 with NaN for missing, channel names, labels."""
    values: np.ndarray
    channels: List[str]
    hz: float
    name: str
    driver: Optional[int] = None            # HCRL Driving class 0..9
    attack: Optional[np.ndarray] = None     # per-time-step flag 0/1 (CAN datasets)
    source: str = "unknown"


# ----------------------------------------------------------------------------- loaders
def _numeric_frame(df: pd.DataFrame, drop: Sequence[str]) -> pd.DataFrame:
    keep = [c for c in df.columns if c not in drop]
    num = df[keep].apply(pd.to_numeric, errors="coerce")
    num = num.loc[:, num.notna().mean() > 0.2]          # drop columns that are mostly empty
    num = num.loc[:, num.std(skipna=True) > 0]           # drop constants
    return num


def load_carmind_csv(path: str) -> Series:
    df = pd.read_csv(path)
    t = (df["uptime_ms"].to_numpy(dtype=np.float64) / 1000.0)
    num = _numeric_frame(df, drop=["seq", "uptime_ms", "epoch_ms", "window_seq", "dtcs", "pending_dtcs", "permanent_dtcs"])
    # resample to 1 Hz by last-value-carried-forward on a regular grid
    grid = np.arange(math.floor(t.min()), math.ceil(t.max()) + 1, 1.0)
    out = np.full((num.shape[1], len(grid)), np.nan, dtype=np.float32)
    idx = np.searchsorted(t, grid, side="right") - 1
    valid = idx >= 0
    arr = num.to_numpy(dtype=np.float32)
    out[:, valid] = arr[idx[valid]].T
    return Series(out, [canonical_name(c) for c in num.columns], 1.0, os.path.basename(path), source="carmind")


def load_hcrl_driving(path: str) -> List[Series]:
    """The HCRL Driving Dataset CSV. Rows are ~1 Hz; 'Class' is the driver (A..J); 'PathOrder' and
    'Time(s)' exist in the released file. Each (driver, path) run becomes one Series."""
    df = pd.read_csv(path)
    cls_col = next((c for c in df.columns if c.lower() == "class"), None)
    path_col = next((c for c in df.columns if "pathorder" in c.lower().replace("_", "")), None)
    time_col = next((c for c in df.columns if c.lower().startswith("time")), None)
    drop = [c for c in (cls_col, path_col, time_col) if c]
    series: List[Series] = []
    groups = df.groupby([c for c in (cls_col, path_col) if c]) if drop else [((None,), df)]
    for key, g in groups:
        key = key if isinstance(key, tuple) else (key,)
        num = _numeric_frame(g, drop=drop)
        for c in num.columns:
            k = normalize_name(c)
            if k in HCRL_UNIT_SCALE:
                num[c] = num[c] * HCRL_UNIT_SCALE[k]
        drv = None
        if cls_col:
            label = str(key[0]).strip().upper()
            drv = ord(label[0]) - ord("A") if label and label[0].isalpha() else None
        series.append(Series(num.to_numpy(dtype=np.float32).T, [canonical_name(c) for c in num.columns], 1.0,
                             f"driving_{'_'.join(map(str, key))}", driver=drv, source="hcrl_driving"))
    return series


def load_hcrl_can(path: str, bin_s: float = 0.1, top_ids: int = 32, max_rows: Optional[int] = None) -> Series:
    """HCRL CAN capture (csv or txt) -> rate series at 1/bin_s Hz.
    Channels: can_total_rate, can_unique_ids, can_payload_entropy, and can_rate_<ID> for the most
    frequent IDs. attack[t] = 1 when any injected ('T') message falls in the bin."""
    if path.endswith(".txt"):
        import re
        # two text layouts: HCRL "Timestamp: t  ID: 0350  000  DLC: 8  d0 .. d7" and tab-separated candump with a header
        normal_re = re.compile(r"Timestamp:\s*(\S+)\s+ID:\s*(\S+)\s+\d{3}\s+DLC:\s*(\d)\s*(.*)")
        rows = []
        with open(path) as f:
            for i, line in enumerate(f):
                if max_rows and i >= max_rows:
                    break
                m = normal_re.match(line)
                if m:
                    ts, cid, dlc, data = float(m.group(1)), m.group(2), int(m.group(3)), m.group(4).split()[:int(m.group(3))]
                else:
                    parts = line.split()
                    try:
                        ts = float(parts[0]); cid = parts[1]; dlc = int(parts[2]); data = parts[3:3 + dlc]
                    except (ValueError, IndexError):
                        continue
                rows.append((ts, cid, dlc, " ".join(data), "R"))
        df = pd.DataFrame(rows, columns=["Timestamp", "CAN ID", "DLC", "DATA", "Flag"])
    else:
        df = pd.read_csv(path, header=None, nrows=max_rows, on_bad_lines="skip")
        # Car-Hacking layout: Timestamp, CAN ID, DLC, DATA[0..7], Flag (short DLC rows shift the flag left)
        df = df.rename(columns={0: "Timestamp", 1: "CAN ID", 2: "DLC"})
        df["DLC"] = pd.to_numeric(df["DLC"], errors="coerce").fillna(8).astype(int)
        flags = []
        datas = []
        for r in df.itertuples(index=False):
            dlc = int(r[2])
            cells = list(r[3:3 + dlc])
            flag = r[3 + dlc] if 3 + dlc < len(r) else "R"
            datas.append(" ".join(str(c) for c in cells))
            flags.append(flag if isinstance(flag, str) else "R")
        df["DATA"] = datas
        df["Flag"] = flags
    df["Timestamp"] = pd.to_numeric(df["Timestamp"], errors="coerce")
    df = df.dropna(subset=["Timestamp"]).sort_values("Timestamp")
    t0 = df["Timestamp"].iloc[0]
    bins = ((df["Timestamp"] - t0) / bin_s).astype(int).to_numpy()
    nb = int(bins.max()) + 1
    ids = df["CAN ID"].astype(str).str.upper().str.replace("0X", "", regex=False).str.zfill(4).to_numpy()
    top = pd.Series(ids).value_counts().index[:top_ids].tolist()
    chan = ["can_total_rate", "can_unique_ids", "can_payload_entropy"] + [f"can_rate_{i}" for i in top]
    out = np.zeros((len(chan), nb), dtype=np.float32)
    np.add.at(out[0], bins, 1.0)
    # unique ids per bin and payload entropy per bin
    frame = pd.DataFrame({"b": bins, "id": ids, "d": df["DATA"].astype(str).to_numpy()})
    uniq = frame.groupby("b")["id"].nunique()
    out[1, uniq.index.to_numpy()] = uniq.to_numpy(dtype=np.float32)

    def entropy(strings: pd.Series) -> float:
        joined = " ".join(strings.tolist()).split()
        if not joined:
            return 0.0
        _, counts = np.unique(joined, return_counts=True)
        p = counts / counts.sum()
        return float(-(p * np.log2(p)).sum())
    ent = frame.groupby("b")["d"].apply(entropy)
    out[2, ent.index.to_numpy()] = ent.to_numpy(dtype=np.float32)
    for k, cid in enumerate(top):
        np.add.at(out[3 + k], bins[ids == cid], 1.0)
    out /= bin_s                                           # counts -> messages per second
    attack = np.zeros(nb, dtype=np.float32)
    inj = (df["Flag"].astype(str).str.upper() == "T").to_numpy()
    if inj.any():
        np.maximum.at(attack, bins[inj], 1.0)
    return Series(out, chan, 1.0 / bin_s, os.path.basename(path), attack=attack, source="hcrl_can")


def synthetic_series(n: int = 8, length: int = 3000, seed: int = 0) -> List[Series]:
    """Regime-switching toy data: idle / city / highway with abrupt changes and one 'attack' channel."""
    rng = np.random.default_rng(seed)
    out = []
    for i in range(n):
        t = np.arange(length)
        regimes = np.zeros(length, dtype=int)
        pos = 0
        while pos < length:
            seg = rng.integers(60, 400)
            regimes[pos:pos + seg] = rng.integers(0, 3)
            pos += seg
        base_rpm = np.array([800, 2000, 2800])[regimes] + rng.normal(0, 60, length)
        speed = np.array([0, 40, 100])[regimes] + rng.normal(0, 3, length)
        load = np.array([12, 35, 55])[regimes] + rng.normal(0, 2, length)
        maf = load * 0.4 + rng.normal(0, 0.5, length)
        thr = load * 0.6 + rng.normal(0, 1, length)
        stft = rng.normal(0, 2, length) + (i % 3 - 1) * 3
        ltft = np.full(length, (i % 4 - 1.5) * 2.0) + rng.normal(0, 0.2, length)
        cool = np.minimum(90, 20 + t * 0.05) + rng.normal(0, 0.5, length)
        iat = 25 + rng.normal(0, 0.5, length)
        mapk = np.array([30, 60, 85])[regimes] + rng.normal(0, 1.5, length)
        vals = np.stack([base_rpm, speed, load, maf, thr, stft, ltft, cool, iat, mapk]).astype(np.float32)
        out.append(Series(vals, EDGE_CHANNELS, 1.0, f"synthetic_{i}", driver=i % 4, source="synthetic"))
    return out


# ----------------------------------------------------------------------------- windows
class WindowDataset(Dataset):
    """Random windows over a list of Series. Returns tensors:
        x      [C, T]   instance-normalised values (NaN -> 0)
        m      [C, T]   1 where a value existed
        cid    [C]      channel vocabulary ids
        stats  [C, 2]   per-channel mean and log-std before normalisation
        driver scalar (-1 if unknown), attack [T] (0 if unknown), has_attack scalar
    Two crops of the same series are returned when `pairs=True` (contrastive positives)."""

    def __init__(self, series: Sequence[Series], window: int, max_channels: int = 48, samples_per_epoch: int = 10000,
                 pairs: bool = False, channel_dropout: float = 0.15, seed: int = 0):
        self.series = [s for s in series if s.values.shape[1] >= window]
        if not self.series:
            raise ValueError("no series long enough for the window")
        self.window = window
        self.max_channels = max_channels
        self.n = samples_per_epoch
        self.pairs = pairs
        self.channel_dropout = channel_dropout
        self.rng = random.Random(seed)

    def __len__(self):
        return self.n

    def _crop(self, s: Series, start: int, keep: np.ndarray) -> Dict[str, torch.Tensor]:
        v = s.values[keep, start:start + self.window].astype(np.float32)
        m = ~np.isnan(v)
        mean = np.nanmean(np.where(m, v, np.nan), axis=1, keepdims=True)
        mean = np.where(np.isnan(mean), 0.0, mean)
        std = np.nanstd(np.where(m, v, np.nan), axis=1, keepdims=True)
        std = np.where(np.isnan(std) | (std < 1e-3), 1.0, std)
        x = np.where(m, (v - mean) / std, 0.0).astype(np.float32)
        cid = np.array([channel_id(s.channels[i]) for i in keep], dtype=np.int64)
        stats = np.concatenate([mean, np.log(std)], axis=1).astype(np.float32)
        C = len(keep)
        pad = self.max_channels - C
        pad_arr = lambda a, fill: np.concatenate([a, np.full((pad,) + a.shape[1:], fill, dtype=a.dtype)], axis=0) if pad > 0 else a
        out = {
            "x": torch.from_numpy(pad_arr(x, 0.0)),
            "m": torch.from_numpy(pad_arr(m.astype(np.float32), 0.0)),
            "cid": torch.from_numpy(pad_arr(cid, 0)),
            "cmask": torch.from_numpy(np.concatenate([np.ones(C, dtype=np.float32), np.zeros(pad, dtype=np.float32)])),
            "stats": torch.from_numpy(pad_arr(stats, 0.0)),
            "driver": torch.tensor(-1 if s.driver is None else s.driver, dtype=torch.long),
        }
        if s.attack is not None:
            out["attack"] = torch.from_numpy(s.attack[start:start + self.window].astype(np.float32))
            out["has_attack"] = torch.tensor(1.0)
        else:
            out["attack"] = torch.zeros(self.window)
            out["has_attack"] = torch.tensor(0.0)
        return out

    def __getitem__(self, i):
        s = self.rng.choice(self.series)
        C = min(s.values.shape[0], self.max_channels)
        keep = np.array(sorted(self.rng.sample(range(s.values.shape[0]), C)))
        if self.channel_dropout > 0 and C > 3:
            drop = [c for c in keep if self.rng.random() < self.channel_dropout]
            if len(drop) < C - 2:
                keep = np.array([c for c in keep if c not in drop])
        T = s.values.shape[1]
        start = self.rng.randint(0, T - self.window)
        a = self._crop(s, start, keep)
        if not self.pairs:
            return a
        start2 = min(max(0, start + self.rng.randint(-self.window // 2, self.window // 2)), T - self.window)
        b = self._crop(s, start2, keep)
        return {"a": a, "b": b}


class EdgeWindowDataset(Dataset):
    """Fixed ten-channel windows for the on-device model (missing channels -> 0 with mask)."""

    def __init__(self, series: Sequence[Series], window: int = 60, samples_per_epoch: int = 20000, seed: int = 0):
        self.items = []
        for s in series:
            idx = [s.channels.index(c) if c in s.channels else -1 for c in EDGE_CHANNELS]
            if sum(i >= 0 for i in idx) >= 4 and s.values.shape[1] >= window:
                self.items.append((s, idx))
        if not self.items:
            raise ValueError("no series with at least 4 of the edge channels")
        self.window = window
        self.n = samples_per_epoch
        self.rng = random.Random(seed)

    def __len__(self):
        return self.n

    def __getitem__(self, i):
        s, idx = self.rng.choice(self.items)
        start = self.rng.randint(0, s.values.shape[1] - self.window)
        v = np.zeros((len(EDGE_CHANNELS), self.window), dtype=np.float32)
        m = np.zeros_like(v)
        for k, j in enumerate(idx):
            if j >= 0:
                col = s.values[j, start:start + self.window]
                ok = ~np.isnan(col)
                v[k, ok] = col[ok]
                m[k] = ok
        mean = (v * m).sum(1, keepdims=True) / np.maximum(m.sum(1, keepdims=True), 1)
        var = (((v - mean) ** 2) * m).sum(1, keepdims=True) / np.maximum(m.sum(1, keepdims=True), 1)
        std = np.sqrt(var)
        std = np.where(std < 1e-3, 1.0, std)
        x = np.where(m > 0, (v - mean) / std, 0.0).astype(np.float32)
        return {"x": torch.from_numpy(x), "m": torch.from_numpy(m), "stats": torch.from_numpy(np.concatenate([mean, np.log(std)], 1).astype(np.float32)),
                "driver": torch.tensor(-1 if s.driver is None else s.driver, dtype=torch.long)}


# ----------------------------------------------------------------------------- discovery
def discover_cached(cache_dir: str) -> Dict[str, List[Series]]:
    import json
    index = json.load(open(os.path.join(cache_dir, "index.json")))
    found: Dict[str, List[Series]] = {"driving": [], "can": [], "carmind": []}
    for e in index:
        z = np.load(os.path.join(cache_dir, e["file"]))
        attack = z["attack"] if z["attack"].size else None
        found.setdefault(e["kind"], []).append(Series(z["values"], e["channels"], e["hz"], e["name"], driver=e["driver"], attack=attack, source=e["source"]))
    return found


def discover(data_root: str, max_can_rows: Optional[int] = None) -> Dict[str, List[Series]]:
    """Find whatever has been downloaded and extracted under data/ (or the npz cache if prepare_cache ran)."""
    cache = os.path.join(data_root, "cache", "index.json")
    if os.path.exists(cache):
        return discover_cached(os.path.dirname(cache))
    found: Dict[str, List[Series]] = {"driving": [], "can": [], "carmind": []}
    for p in glob.glob(os.path.join(data_root, "driving_dataset", "**", "*.csv"), recursive=True):
        try:
            found["driving"] += load_hcrl_driving(p)
        except Exception as e:  # noqa: BLE001
            print(f"skip {p}: {e}")
    can_files = glob.glob(os.path.join(data_root, "car_hacking", "**", "*"), recursive=True) + \
                glob.glob(os.path.join(data_root, "can_signal_extraction", "**", "*candump*.txt"), recursive=True)
    for p in can_files:
        if p.lower().endswith((".csv", ".txt")) and os.path.getsize(p) > 1000:
            try:
                found["can"].append(load_hcrl_can(p, max_rows=max_can_rows))
            except Exception as e:  # noqa: BLE001
                print(f"skip {p}: {e}")
    for p in glob.glob(os.path.join(data_root, "carmind", "**", "s*.csv"), recursive=True):
        try:
            found["carmind"].append(load_carmind_csv(p))
        except Exception as e:  # noqa: BLE001
            print(f"skip {p}: {e}")
    return found
