"""Exact Python port of the CarMind board's pattern layer (main/pattern.c): 10 s windows, 30 features,
running standardisation, online k-means (K=24), SAX words. float32 where the C code uses float.
Copied from CarMind samples/replay/make_patterns.py so the board heuristic can be scored as a baseline."""
import math
import os
import sys

import numpy as np

f32 = np.float32
K, SIGNALS, FEATURES, SAX_LEN, WINDOW_MS, MAX_SAMPLES = 24, 10, 30, 5, 10000, 128
# main/elm327.c pattern_step(): F_rpm, F_speed, F_load, F_maf, F_throttle, F_stft1, F_ltft1, F_coolant, F_iat, F_map
SIGNAL_COLS = ["rpm", "speed_kph", "engine_load_pct", "maf_gps", "throttle_pct", "stft_b1_pct", "ltft_b1_pct",
               "coolant_c", "intake_temp_c", "map_kpa"]
PAT_HEADER = ("window_seq,start_ms,end_ms,samples,pattern_id,dist,novelty,spawned,sax_rpm,sax_speed"
              + "".join(f",f{i}" for i in range(FEATURES)) + "\n")


def sax(x):
    n = len(x)
    if n < 2:
        return "-----"
    mean = sum(float(v) for v in x) / n
    var = sum((float(v) - mean) * (float(v) - mean) for v in x)
    sd = math.sqrt(var / n)
    out = ""
    for s in range(SAX_LEN):
        a, b = s * n // SAX_LEN, (s + 1) * n // SAX_LEN
        if b <= a:
            b = a + 1
        seg = sum(float(x[i]) for i in range(a, min(b, n))) / (b - a)
        z = (seg - mean) / sd if sd > 1e-6 else 0
        out += "a" if z < -0.67 else "b" if z < 0 else "c" if z < 0.67 else "d"
    return out


class PatternC:
    """pattern_init / pattern_feed from main/pattern.c."""

    def __init__(self):
        self.centroid = np.zeros((K, FEATURES), dtype=np.float32)
        self.count = [0] * K
        self.used = 0
        self.mean = [0.0] * FEATURES
        self.m2 = [0.0] * FEATURES
        self.n_windows = 0
        self.mean_dist = 0.0
        self.spawn_threshold = f32(2.5)
        self.seq = 0
        self._reset(0)

    def _reset(self, now_ms):
        self.start_ms, self.n = now_ms, 0
        self.sum = [0.0] * SIGNALS
        self.sum2 = [0.0] * SIGNALS
        self.first = [f32(0)] * SIGNALS
        self.last = [f32(0)] * SIGNALS
        self.first_ms = [0] * SIGNALS
        self.last_ms = [0] * SIGNALS
        self.cnt = [0] * SIGNALS
        self.rpm, self.speed = [], []

    def _std(self, f, raw):
        sd = math.sqrt(self.m2[f] / (self.n_windows - 1)) if self.n_windows > 1 else 0
        if sd < 1e-6:
            return f32(0)
        return f32((float(raw) - self.mean[f]) / sd)

    def feed(self, now_ms, sig, valid):
        if self.start_ms == 0:
            self.start_ms = now_ms
        for i in range(SIGNALS):
            if not valid[i]:
                continue
            v = sig[i]
            if self.cnt[i] == 0:
                self.first[i], self.first_ms[i] = v, now_ms
            self.last[i], self.last_ms[i] = v, now_ms
            self.sum[i] += float(v)
            self.sum2[i] += float(v) * float(v)
            self.cnt[i] += 1
        if valid[0] and len(self.rpm) < MAX_SAMPLES:
            self.rpm.append(sig[0])
        if valid[1] and len(self.speed) < MAX_SAMPLES:
            self.speed.append(sig[1])
        self.n += 1

        if now_ms - self.start_ms < WINDOW_MS:
            return None
        if self.n < 3:
            self._reset(now_ms)
            return None

        raw = np.zeros(FEATURES, dtype=np.float32)
        for i in range(SIGNALS):
            mean = sd = slope = f32(0)
            if self.cnt[i] > 0:
                mean = f32(self.sum[i] / self.cnt[i])
                var = self.sum2[i] / self.cnt[i] - float(mean) * float(mean)
                sd = f32(math.sqrt(var)) if var > 0 else f32(0)
                dt = (self.last_ms[i] - self.first_ms[i]) / 1000.0
                slope = f32(float(f32(self.last[i] - self.first[i])) / dt) if dt > 0.5 else f32(0)
            raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2] = mean, sd, slope

        self.n_windows += 1
        z = np.zeros(FEATURES, dtype=np.float32)
        for f in range(FEATURES):
            delta = float(raw[f]) - self.mean[f]
            self.mean[f] += delta / self.n_windows
            self.m2[f] += delta * (float(raw[f]) - self.mean[f])
            z[f] = self._std(f, raw[f])

        best, best_d = -1, f32(np.inf)
        for k in range(self.used):
            d = f32(0)
            for f in range(FEATURES):
                e = f32(z[f] - self.centroid[k][f])
                d = f32(d + f32(e * e))
            d = f32(math.sqrt(float(d)))
            if d < best_d:
                best_d, best = d, k
        spawned = False
        if best < 0 or (best_d > self.spawn_threshold and self.used < K and self.n_windows > 5):
            best = self.used
            self.used += 1
            self.centroid[best] = z
            self.count[best] = 1
            best_d = f32(0)
            spawned = True
        else:
            self.count[best] += 1
            lr = f32(f32(1.0) / f32(self.count[best]))
            if lr < f32(0.05):
                lr = f32(0.05)
            for f in range(FEATURES):
                c = self.centroid[best][f]
                self.centroid[best][f] = f32(c + f32(lr * f32(z[f] - c)))
        self.mean_dist += (float(best_d) - self.mean_dist) / (self.n_windows if self.n_windows < 50 else 50)

        self.seq += 1
        novelty = (f32(best_d / f32(self.mean_dist)) if self.mean_dist > 1e-3
                   else f32(2.0 if spawned else 1.0))
        res = dict(window_seq=self.seq, start_ms=self.start_ms, end_ms=now_ms, samples=self.n, pattern_id=best,
                   dist=best_d, novelty=novelty, spawned=spawned, sax_rpm=sax(self.rpm), sax_speed=sax(self.speed),
                   features=raw.copy())
        self._reset(now_ms)
        return res
