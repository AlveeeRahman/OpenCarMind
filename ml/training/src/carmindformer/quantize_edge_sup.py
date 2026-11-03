"""Quantise the exported CarMind-Edge-S ONNX with ESP-PPQ for the ESP32-S31 and measure the int8 accuracy.

  python -m carmindformer.quantize_edge_sup --export export/edge_sup --data data --target esp32s31

Reads edge_sup.onnx, calib.npz and norm.json written by export_edge_sup.py, writes edge_sup.espdl next to them, then
runs the quantised graph (ESP-PPQ's simulator) on the held-out test windows of the same protocol the model was trained
with and prints float vs int8 accuracy. Kind 1 data only (HCRL Driving Dataset).
"""
from __future__ import annotations
import argparse
import json
import os

import numpy as np
import torch
from torch.utils.data import DataLoader

from .train_supervised import DriverWindows, load_runs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--export", default="export/edge_sup"); ap.add_argument("--data", default="data")
    ap.add_argument("--target", default="esp32s31"); ap.add_argument("--quant-type", default="w8a8")
    ap.add_argument("--protocol", default="holdout"); ap.add_argument("--calib-steps", type=int, default=64)
    a = ap.parse_args()
    from esp_ppq.api import espdl_quantize_onnx          # imported late: it prints a banner and is Mac/CUDA optional
    from esp_ppq.executor import TorchExecutor

    xm = torch.from_numpy(np.load(os.path.join(a.export, "calib.npz"))["xm"]).float()   # [N, 2C, 1, T]
    C2, T = xm.shape[1], xm.shape[3]
    calib = DataLoader(xm, batch_size=1, shuffle=False)
    out = os.path.join(a.export, "edge_sup.espdl")
    graph = espdl_quantize_onnx(onnx_import_file=os.path.join(a.export, "edge_sup.onnx"), espdl_export_file=out,
                                calib_dataloader=calib, calib_steps=min(a.calib_steps, len(xm)),
                                input_shape=[1, C2, 1, T], target=a.target, quant_type=a.quant_type,
                                collate_fn=lambda b: b.float(), device="cpu", error_report=False, verbose=0)
    exe = TorchExecutor(graph=graph, device="cpu")

    norm = json.load(open(os.path.join(a.export, "norm.json")))
    te = DriverWindows(load_runs(a.data), T, "test", a.protocol, norm=norm or None)
    ok = n = 0
    for i in range(len(te)):
        b = te[i]
        x = torch.cat([b["x"], b["m"]], 0).unsqueeze(0).unsqueeze(2)                    # [1, 2C, 1, T]
        scores = exe.forward(inputs=x)[1]                                                 # outputs: embed, scores, recon
        ok += int(int(scores.argmax()) == int(b["y"])); n += 1
    acc = ok / max(1, n)
    meta_path = os.path.join(a.export, "export.json")
    meta = json.load(open(meta_path)) if os.path.exists(meta_path) else {}
    meta.update({"espdl": os.path.basename(out), "target": a.target, "quant_type": a.quant_type,
                 "int8_test_acc": round(acc, 4), "int8_test_windows": n,
                 "espdl_bytes": os.path.getsize(out) if os.path.exists(out) else None})
    json.dump(meta, open(meta_path, "w"), indent=1)
    print(f"{a.target} {a.quant_type}: int8 test accuracy {acc:.3f} on {n} windows; float {meta.get('float_test_acc')}; "
          f"{out} {meta['espdl_bytes']} bytes")


if __name__ == "__main__":
    main()
