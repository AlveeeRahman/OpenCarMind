"""Export CarMind-Edge-S (supervised, QAT) for ESP-PPQ and ESP-DL on the ESP32-S31.

  python -m carmindformer.export_edge_sup --ckpt runs/supervised/edge_sup_holdout_qat.pt --data data --out export/edge_sup
  python -m carmindformer.export_edge_sup --mlx-weights runs/supervised_mlx/edge_sup_holdout_qat.safetensors --data data --out export/edge_sup

Writes edge_sup.onnx (opset 13, static [1, 2C, 1, T] input, outputs embed/logits/recon), calib.npz
(calibration windows in the same layout), gallery.json (per-driver mean embeddings for cosine matching,
so new drivers can be enrolled on device without retraining, as in TEMPEST-Raw), and norm.json (the
per-channel mean/std the firmware must apply before inference).

Then, on the CUDA box (esp-ppq installed from https://github.com/espressif/esp-ppq):

  from esp_ppq.api import espdl_quantize_onnx
  espdl_quantize_onnx("export/edge_sup/edge_sup.onnx", "export/edge_sup/edge_sup.espdl", calib_dataloader,
                      calib_steps=32, input_shape=[1, 2*C, 1, T], target="esp32s31", num_of_bits=8, device="cuda")
"""
from __future__ import annotations
import argparse
import json
import os

import numpy as np
import torch

from .model_edge_sup import EdgeSupConfig, EdgeSupNet
from .qat import QConv2d, QLinear, fake_quant, int8_size_bytes, pow2_scale, set_quant
from .train_supervised import DriverWindows, load_runs


def load_mlx(path: str):
    """Read an MLX safetensors file plus its sidecar json and build the same checkpoint dict torch.save wrote."""
    from safetensors.numpy import load_file
    w = load_file(path); meta = json.load(open(path[:-len(".safetensors")] + ".json"))
    sd = {}
    for k, v in w.items():
        t = torch.from_numpy(np.array(v))
        if k.endswith(".weight") and t.ndim == 3:            # conv [out, k, in] -> [out, in, 1, k]
            t = t.permute(0, 2, 1).unsqueeze(2).contiguous()
        sd[k] = t
    return {"model": sd, "config": meta["config"], "qat": meta["qat"], "protocol": meta["protocol"], "norm": meta["norm"],
            "test_acc": meta.get("test_acc")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt"); ap.add_argument("--mlx-weights"); ap.add_argument("--data", default="data"); ap.add_argument("--out", default="export/edge_sup"); ap.add_argument("--calib", type=int, default=256)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    ck = load_mlx(a.mlx_weights) if a.mlx_weights else torch.load(a.ckpt, map_location="cpu")
    cfg = EdgeSupConfig(**{k: v for k, v in ck["config"].items() if k in EdgeSupConfig.__dataclass_fields__})
    model = EdgeSupNet(cfg).eval(); model.load_state_dict(ck["model"], strict=False)
    C, T = cfg.n_channels, cfg.window
    if ck.get("qat", False):
        # bake the trained weights onto their int8 power-of-two grid, then export a plain float graph:
        # ESP-PPQ re-derives the same scales from calib.npz and the weight rounding is then exact.
        with torch.no_grad():
            for mod in model.modules():
                if isinstance(mod, (QConv2d, QLinear)):
                    mod.weight.copy_(fake_quant(mod.weight, pow2_scale(mod.weight.abs().max(), mod.bits), mod.bits))
    set_quant(model, False)
    Wn = torch.nn.functional.normalize(model.head.W.detach())

    class Wrapped(torch.nn.Module):
        """Graph for ESP-DL: Conv/Relu/Add/ReduceMean/Gemm only. `embed` is NOT L2-normalised here (ReduceL2
        is not an ESP-DL op); the firmware divides by its norm. `scores` = embed . Wn^T is the cosine times
        |embed|, so argmax is the same as on normalised cosines."""
        def __init__(self, m): super().__init__(); self.m = m; self.register_buffer("Wn", Wn)
        def forward(self, xm):
            h = torch.cat([xm[:, :C, 0], xm[:, C:, 0]], 1).unsqueeze(2)
            h = torch.relu(self.m.stem(h))
            for b in self.m.blocks:
                h = b(h)
            e = self.m.proj(h.mean(dim=(2, 3)))
            return e, e @ self.Wn.t(), self.m.recon(h).squeeze(2)
    torch.onnx.export(Wrapped(model), torch.zeros(1, 2 * C, 1, T), os.path.join(a.out, "edge_sup.onnx"), opset_version=13,
                      input_names=["xm"], output_names=["embed", "scores", "recon"], dynamo=False)

    runs = load_runs(a.data)
    norm = ck.get("norm")
    ds = DriverWindows(runs, T, "train", ck.get("protocol", "session"), samples=a.calib, norm=norm)
    xs, embs, ys = [], [], []
    with torch.no_grad():
        for i in range(len(ds)):
            b = ds[i]
            xs.append(torch.cat([b["x"], b["m"]], 0).unsqueeze(1).numpy())
            embs.append(model(b["x"][None], b["m"][None])["embed"][0].numpy()); ys.append(int(b["y"]))
    np.savez_compressed(os.path.join(a.out, "calib.npz"), xm=np.stack(xs))
    E, Y = np.stack(embs), np.array(ys)
    gallery = {int(k): (E[Y == k].mean(0) / np.linalg.norm(E[Y == k].mean(0))).round(5).tolist() for k in np.unique(Y)}
    json.dump({"embed": cfg.embed, "window": T, "channels": C, "gallery": gallery, "class_centres": Wn.numpy().round(5).tolist(),
               "note": "normalise the embed output on device before cosine against gallery or class_centres"}, open(os.path.join(a.out, "gallery.json"), "w"))
    json.dump(norm or {}, open(os.path.join(a.out, "norm.json"), "w"))
    json.dump({"source": a.mlx_weights or a.ckpt, "protocol": ck.get("protocol"), "qat": ck.get("qat", False),
               "float_test_acc": ck.get("test_acc"), "window": T, "channels": C, "data": "Kind 1 (HCRL Driving Dataset)"},
              open(os.path.join(a.out, "export.json"), "w"), indent=1)
    print(f"edge_sup.onnx {os.path.getsize(os.path.join(a.out, 'edge_sup.onnx')) / 1024:.0f} KB fp32; int8 weights {int8_size_bytes(model) / 1024:.1f} KB; gallery {len(gallery)} drivers")


if __name__ == "__main__":
    main()
