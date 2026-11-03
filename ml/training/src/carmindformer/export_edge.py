"""Export CarMind-Edge to ONNX for ESP-DL quantisation, and build the pattern prototypes.

  python -m carmindformer.export_edge --ckpt runs/edge-.../last.pt --data data --out export/edge

Produces edge.onnx (opset 13, static shapes), prototypes.json (k-means over the embedding so the
device maps z to a pattern id exactly like today's centroids), and calib.npz (calibration windows
for esp-ppq). Quantise with ESP-PPQ following https://docs.espressif.com/projects/esp-dl/ .
"""
from __future__ import annotations
import argparse
import json
import os

import numpy as np
import torch
from sklearn.cluster import KMeans

from .data import EdgeWindowDataset, discover, synthetic_series
from .model_edge import EdgeAE, EdgeConfig


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--data", default="data")
    ap.add_argument("--out", default="export/edge")
    ap.add_argument("--synthetic", action="store_true")
    ap.add_argument("--k", type=int, default=24)
    ap.add_argument("--calib", type=int, default=512)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    ck = torch.load(args.ckpt, map_location="cpu")
    cfg = EdgeConfig(**ck["config"])
    model = EdgeAE(cfg).eval()
    model.load_state_dict(ck["model"])

    class Wrapped(torch.nn.Module):     # ONNX-friendly: one input [1, 20, 1, T], outputs z and change
        def __init__(self, m): super().__init__(); self.m = m
        def forward(self, xm):
            x, mk = xm[:, :cfg.n_channels, 0], xm[:, cfg.n_channels:, 0]
            o = self.m(x, mk)
            return o["z"], o["change"], o["recon"]
    dummy = torch.zeros(1, 2 * cfg.n_channels, 1, cfg.window)
    torch.onnx.export(Wrapped(model), dummy, os.path.join(args.out, "edge.onnx"), opset_version=13,
                      input_names=["xm"], output_names=["z", "change", "recon"], dynamo=False)

    series = synthetic_series() if args.synthetic else [s for v in discover(args.data).values() for s in v]
    ds = EdgeWindowDataset(series, window=cfg.window, samples_per_epoch=args.calib)
    xs, zs = [], []
    with torch.no_grad():
        for i in range(len(ds)):
            b = ds[i]
            xs.append(torch.cat([b["x"], b["m"]], 0).unsqueeze(1).numpy())
            zs.append(model(b["x"][None], b["m"][None])["z"][0].numpy())
    np.savez_compressed(os.path.join(args.out, "calib.npz"), xm=np.stack(xs))
    Z = np.stack(zs)
    km = KMeans(n_clusters=min(args.k, len(Z)), n_init=5, random_state=0).fit(Z)
    json.dump({"k": int(km.n_clusters), "embed": int(Z.shape[1]), "window": cfg.window, "channels": cfg.n_channels,
               "centroids": km.cluster_centers_.round(5).tolist(), "counts": np.bincount(km.labels_).tolist()},
              open(os.path.join(args.out, "prototypes.json"), "w"))
    print(f"exported {args.out}/edge.onnx, prototypes.json ({km.n_clusters} prototypes), calib.npz ({len(xs)} windows)")


if __name__ == "__main__":
    main()
