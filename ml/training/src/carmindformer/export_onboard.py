"""Export the on-board network (manoeuvre + estimator) to ONNX and ESP-DL int8, and score the int8 graph.

  python -m carmindformer.export_onboard --weights runs/nas/<name>_winner_qat.safetensors \
      --config runs/nas/<name>_final_qat.json --target esp32s3 --out export/onboard_s3

Writes to --out: onboard.onnx, onboard.espdl (ESP-PPQ, with embedded test values for ESP-DL's model->test()),
calib.npz, norm.json (per-channel mean and std, data.EDGE_CHANNELS order), targets.json (estimator mean and std, to
de-standardise), test_vectors.npz (inputs and the int8 simulator's outputs) and export.json (metrics, versions, hashes).
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os

import numpy as np
import torch

from .data import EDGE_CHANNELS
from .model_onboard import OnboardNet, load_mlx, make_input
from .nas_edge import get_data, macro_f1
from .qat import QConv2d, QLinear, fake_quant, pow2_scale
from .tasks import EST, MANOEUVRES, windows_for
from .train_supervised import channel_norm, load_runs


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def r2_mean(pred, t):
    r = []
    for j in range(t.shape[1]):
        ok = ~np.isnan(t[:, j])
        r.append(1 - np.sum((pred[ok, j] - t[ok, j]) ** 2) / np.sum((t[ok, j] - t[ok, j].mean()) ** 2))
    return float(np.mean(r)), [round(float(v), 4) for v in r]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--weights", required=True); ap.add_argument("--config", required=True)
    ap.add_argument("--data", default="data"); ap.add_argument("--out", default="export/onboard")
    ap.add_argument("--target", default="esp32s3"); ap.add_argument("--quant-type", default="w8a8")
    ap.add_argument("--protocol", default="holdout"); ap.add_argument("--calib", type=int, default=256)
    ap.add_argument("--test-vectors", type=int, default=16); ap.add_argument("--error-report", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    cfg = json.load(open(a.config))
    arch = cfg["winner"]["params"]
    net = load_mlx(a.weights, arch)
    with torch.no_grad():                               # bake weights onto the int8 power-of-two grid QAT trained for
        for mod in net.modules():
            if isinstance(mod, (QConv2d, QLinear)):
                mod.weight.copy_(fake_quant(mod.weight, pow2_scale(mod.weight.abs().max(), mod.bits), mod.bits))

    runs = load_runs(a.data); norm = channel_norm(runs, a.protocol)
    D = get_data(runs, windows_for(runs, a.protocol), arch["W"], norm, a.protocol)
    tr, te = D["train"], D["test"]
    Xte = make_input(te["x"], te["m"], arch["deltas"])
    rng = np.random.default_rng(0)
    calib = make_input(tr["x"], tr["m"], arch["deltas"])[rng.choice(len(tr["man"]), a.calib, replace=False)]
    np.savez_compressed(os.path.join(a.out, "calib.npz"), xm=calib)
    trained = [any(c in r.channels for r in runs) for c in EDGE_CHANNELS]     # channels the model ever saw valid
    json.dump({"channels": EDGE_CHANNELS, "mean": [norm[c][0] for c in EDGE_CHANNELS], "std": [norm[c][1] for c in EDGE_CHANNELS],
               "trained": trained,
               "rule": "a channel with trained=false must be fed as missing (value 0, mask 0) even when the car reports it",
               "window": arch["W"], "deltas": arch["deltas"], "layout": "[1, 30, 1, W]: values, masks, differences; oldest first"},
              open(os.path.join(a.out, "norm.json"), "w"), indent=1)
    mu = np.nanmean(tr["est"], 0); sd = np.nanstd(tr["est"], 0)
    json.dump({"targets": EST, "mean": mu.tolist(), "std": sd.tolist(), "units": ["bar", "N m", "L/h"], "classes": MANOEUVRES},
              open(os.path.join(a.out, "targets.json"), "w"), indent=1)

    with torch.no_grad():
        fm, fe = net(torch.from_numpy(Xte))
    float_man = macro_f1(te["man"], fm.numpy().argmax(1), len(MANOEUVRES)); float_est, float_each = r2_mean(fe.numpy(), te["est_z"])

    onnx_path = os.path.join(a.out, "onboard.onnx")
    torch.onnx.export(net, torch.zeros(1, Xte.shape[1], 1, arch["W"]), onnx_path, opset_version=13,
                      input_names=["xm"], output_names=["man", "est"], dynamo=False)

    from esp_ppq.api import espdl_quantize_onnx
    from esp_ppq.executor import TorchExecutor
    espdl = os.path.join(a.out, "onboard.espdl")
    graph = espdl_quantize_onnx(onnx_import_file=onnx_path, espdl_export_file=espdl,
                                calib_dataloader=torch.utils.data.DataLoader(torch.from_numpy(calib), batch_size=1),
                                calib_steps=a.calib, input_shape=[1, Xte.shape[1], 1, arch["W"]], target=a.target,
                                quant_type=a.quant_type, collate_fn=lambda b: b.float(), device="cpu",
                                error_report=a.error_report, export_test_values=True, test_output_names=["man", "est"], verbose=0)
    exe = TorchExecutor(graph=graph, device="cpu")
    qm, qe = [], []
    for i in range(len(Xte)):
        o = exe.forward(inputs=torch.from_numpy(Xte[i:i + 1]))
        qm.append(o[0].numpy()); qe.append(o[1].numpy())
    qm, qe = np.concatenate(qm), np.concatenate(qe)
    int8_man = macro_f1(te["man"], qm.argmax(1), len(MANOEUVRES)); int8_est, int8_each = r2_mean(qe, te["est_z"])
    k = a.test_vectors
    np.savez_compressed(os.path.join(a.out, "test_vectors.npz"), xm=Xte[:k], man=qm[:k], est=qe[:k], label=te["man"][:k])

    import esp_ppq
    meta = {"target": a.target, "quant_type": a.quant_type, "arch": arch, "params": sum(p.numel() for p in net.parameters()),
            "test_windows": int(len(Xte)), "float": {"manoeuvre_macro_f1": round(float_man, 4), "estimator_r2": round(float_est, 4), "estimator_r2_each": float_each},
            "int8": {"manoeuvre_macro_f1": round(int8_man, 4), "estimator_r2": round(int8_est, 4), "estimator_r2_each": int8_each},
            "files": {f: {"bytes": os.path.getsize(os.path.join(a.out, f)), "sha256": sha(os.path.join(a.out, f))}
                      for f in ("onboard.espdl", "onboard.onnx", "norm.json", "targets.json")},
            "versions": {"torch": torch.__version__, "esp_ppq": getattr(esp_ppq, "__version__", "?")},
            "source_weights": a.weights, "data": "Kind 1, HCRL Driving Dataset, protocol " + a.protocol}
    json.dump(meta, open(os.path.join(a.out, "export.json"), "w"), indent=1)
    print(f"{a.target} {a.quant_type}: manoeuvre macro-F1 float {float_man:.3f} int8 {int8_man:.3f}; estimator R2 float "
          f"{float_est:.3f} int8 {int8_est:.3f} {int8_each}; espdl {meta['files']['onboard.espdl']['bytes']:,} bytes")


if __name__ == "__main__":
    main()
