"""Parse every dataset once and cache the Series as .npz so training does not re-read gigabytes of CAN text.

  python -m carmindformer.prepare_cache --data data --out data/cache [--max-can-rows N]

train.py / eval.py pick up data/cache automatically when it exists (discover_cached).
"""
from __future__ import annotations
import argparse
import json
import os

import numpy as np

from .data import discover


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="data")
    ap.add_argument("--out", default="data/cache")
    ap.add_argument("--max-can-rows", type=int, default=None)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    found = discover(args.data, max_can_rows=args.max_can_rows)
    index = []
    for kind, series in found.items():
        for i, s in enumerate(series):
            fn = f"{kind}_{i:04d}.npz"
            np.savez_compressed(os.path.join(args.out, fn), values=s.values,
                                attack=s.attack if s.attack is not None else np.zeros(0, dtype=np.float32))
            index.append({"file": fn, "kind": kind, "name": s.name, "channels": s.channels, "hz": s.hz,
                          "driver": s.driver, "source": s.source, "steps": int(s.values.shape[1])})
            print(f"{kind:8s} {s.name[:50]:50s} {s.values.shape}")
    json.dump(index, open(os.path.join(args.out, "index.json"), "w"), indent=1)
    print(f"cached {len(index)} series to {args.out}")


if __name__ == "__main__":
    main()
