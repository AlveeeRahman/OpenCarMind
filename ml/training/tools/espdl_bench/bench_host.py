"""Run replay sessions through the on-board model on the ESP32 and compare with the Mac.

  python tools/espdl_bench/bench_host.py --export export/onboard_s3 --port /dev/cu.usbmodem21301 \
      --sessions ~/esp32-obd-logger/samples/replay/obd/sessions --out runs/bench

For every session sNNNNNN.csv with its .pat file (the board's own 10 s windows), this builds the model input exactly
as the firmware must: 1 s bins of the 10 OBD channels (mean of the rows in each second), the last W bins ending at
each window's end_ms, standardised with norm.json, plus masks and differences. Each window goes to the board
(bench app, USB-Serial/JTAG) and through ESP-PPQ's int-simulator and the float model on the Mac.

Writes <out>/sNNNNNN_ml.csv keyed by window_seq (joins to the session's .pat), and <out>/bench_summary.json with
device-vs-simulator agreement, device latency and the Iron Law terms measured on the chip.
"""
from __future__ import annotations
import argparse
import csv
import glob
import json
import os
import re
import struct
import sys
import time

import numpy as np
import serial
import torch

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))
from carmindformer.data import EDGE_CHANNELS                    # noqa: E402
from carmindformer.model_onboard import load_mlx, make_input   # noqa: E402
from carmindformer.qat import QConv2d, QLinear, fake_quant, pow2_scale  # noqa: E402
from carmindformer.systems_metrics import cost                 # noqa: E402

CLASSES = ["stopped", "braking", "accelerating", "cruising"]


def session_windows(csv_path, pat_path, W, mean, std, trained=None):
    rows = list(csv.DictReader(open(csv_path)))
    up = np.array([float(r["uptime_ms"]) for r in rows])
    vals = np.array([[float(r[c]) if r.get(c, "") not in ("", None) else np.nan for c in EDGE_CHANNELS] for r in rows])
    sec = np.floor(up / 1000).astype(int)
    s0, s1 = sec.min(), sec.max()
    bins = np.full((s1 - s0 + 1, len(EDGE_CHANNELS)), np.nan)
    for k in range(bins.shape[0]):
        sel = sec == s0 + k
        if sel.any():
            with np.errstate(all="ignore"):
                bins[k] = np.nanmean(vals[sel], axis=0)
    z = (bins - np.array(mean)) / np.array(std)
    if trained is not None:                      # channels the model never saw valid are fed as missing
        z[:, ~np.array(trained, dtype=bool)] = np.nan
    pats = list(csv.DictReader(open(pat_path)))
    X, M, meta, spd = [], [], [], []
    for p in pats:
        end = int(float(p["end_ms"]) // 1000) - s0 + 1           # bins up to and including the closing second
        a = max(0, end - W)
        seg = z[a:end].T
        x = np.zeros((len(EDGE_CHANNELS), W), np.float32); m = np.zeros_like(x)
        ok = ~np.isnan(seg)
        x[:, W - seg.shape[1]:] = np.where(ok, seg, 0); m[:, W - seg.shape[1]:] = ok
        X.append(x); M.append(m)
        last = bins[max(0, end - 10):end, EDGE_CHANNELS.index("speed_kph")]
        spd.append((np.nanmean(last), (last[-1] - last[0]) / 3.6 / max(1, len(last) - 1) if len(last) > 1 else 0.0))
        meta.append({k: p[k] for k in ("window_seq", "start_ms", "end_ms", "pattern_id", "novelty")})
    return np.stack(X), np.stack(M), meta, spd


def speed_label(mean_kph, accel_ms2):
    """The two classes that speed alone decides; None for braking / cruising, which need the pedal."""
    if mean_kph < 2:
        return 0
    if accel_ms2 >= 0.5:
        return 2
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--export", required=True); ap.add_argument("--port", default="/dev/cu.usbmodem21301")
    ap.add_argument("--sessions", required=True); ap.add_argument("--out", default="runs/bench")
    ap.add_argument("--weights"); ap.add_argument("--config")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    meta = json.load(open(os.path.join(a.export, "export.json")))
    arch = meta["arch"]; W = arch["W"]
    norm = json.load(open(os.path.join(a.export, "norm.json")))
    tg = json.load(open(os.path.join(a.export, "targets.json")))
    weights = a.weights or meta["source_weights"]

    net = load_mlx(weights, arch)
    with torch.no_grad():
        for mod in net.modules():
            if isinstance(mod, (QConv2d, QLinear)):
                mod.weight.copy_(fake_quant(mod.weight, pow2_scale(mod.weight.abs().max(), mod.bits), mod.bits))
    from esp_ppq.api import espdl_quantize_onnx
    from esp_ppq.executor import TorchExecutor
    calib = torch.from_numpy(np.load(os.path.join(a.export, "calib.npz"))["xm"])
    graph = espdl_quantize_onnx(onnx_import_file=os.path.join(a.export, "onboard.onnx"), espdl_export_file=os.path.join(a.out, "_sim.espdl"),
                                calib_dataloader=torch.utils.data.DataLoader(calib, batch_size=1), calib_steps=len(calib),
                                input_shape=[1, calib.shape[1], 1, W], target=meta["target"], quant_type=meta["quant_type"],
                                collate_fn=lambda b: b.float(), device="cpu", error_report=False, verbose=0)
    sim = TorchExecutor(graph=graph, device="cpu")

    port = serial.Serial(a.port, 115200, timeout=0.2); port.dtr = False; port.rts = False
    time.sleep(0.3); port.reset_input_buffer()

    def ask(xm):
        nhwc = np.ascontiguousarray(xm[0, :, 0, :].T).astype("<f4")      # [W, 30]
        port.reset_input_buffer()
        port.write(f"\nCMBENCH RUN {nhwc.size}\n".encode()); port.write(nhwc.tobytes())
        rx, t0 = b"", time.time()
        while time.time() - t0 < 10:
            rx += port.read(port.in_waiting or 1)
            mt = re.search(rb"CMBENCH OUT ([^\n]+)\n", rx)
            if mt:
                v = mt.group(1).split()
                return np.array([float(x) for x in v[:4]]), np.array([float(x) for x in v[4:7]]), int(v[7]), int(v[8])
            if b"CMBENCH ERR" in rx:
                raise RuntimeError(rx[-200:])
        raise TimeoutError(rx[-300:])

    summary = {"export": a.export, "target": meta["target"], "quant_type": meta["quant_type"], "sessions": {}}
    all_us, all_cyc = [], []
    for csv_path in sorted(glob.glob(os.path.join(os.path.expanduser(a.sessions), "s*.csv"))):
        sid = os.path.basename(csv_path)[:-4]
        pat = csv_path[:-4] + ".pat"
        if not os.path.exists(pat):
            continue
        vehicle = json.load(open(csv_path[:-4] + ".json")).get("replay", {}).get("vehicle", "?")
        X, M, wm, spd = session_windows(csv_path, pat, W, norm["mean"], norm["std"], norm.get("trained"))
        XM = make_input(X, M, arch["deltas"])
        with torch.no_grad():
            fm, fe = net(torch.from_numpy(XM))
        rows, agree_sim, agree_float, est_err, sl_hit, sl_n = [], 0, 0, [], 0, 0
        for i in range(len(XM)):
            dm, de, us, cyc = ask(XM[i:i + 1])
            o = sim.forward(inputs=torch.from_numpy(XM[i:i + 1]))
            sm, se = o[0].numpy()[0], o[1].numpy()[0]
            agree_sim += int(dm.argmax() == sm.argmax()); agree_float += int(dm.argmax() == fm[i].argmax().item())
            est_err.append(np.abs(de - se))
            lab = speed_label(*spd[i])
            if lab is not None:
                sl_n += 1; sl_hit += int(dm.argmax() == lab)
            units = de * np.array(tg["std"]) + np.array(tg["mean"])
            p = np.exp(dm - dm.max()); p /= p.sum()
            rows.append({**wm[i], "manoeuvre": CLASSES[int(dm.argmax())], **{f"p_{c}": round(float(v), 4) for c, v in zip(CLASSES, p)},
                         "est_brake_pressure_bar": round(float(units[0]), 2), "est_flywheel_torque_nm": round(float(units[1]), 2),
                         "est_fuel_rate_src_units": round(float(units[2]), 1), "device_us": us, "device_cycles": cyc,
                         "sim_manoeuvre": CLASSES[int(sm.argmax())]})
            all_us.append(us); all_cyc.append(cyc)
        out_csv = os.path.join(a.out, f"{sid}_ml.csv")
        with open(out_csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)
        n = len(rows); e = np.stack(est_err)
        summary["sessions"][sid] = {"vehicle": vehicle, "windows": n,
                                    "device_vs_sim_manoeuvre_agreement": round(agree_sim / n, 4),
                                    "device_vs_float_manoeuvre_agreement": round(agree_float / n, 4),
                                    "device_vs_sim_estimate_mean_abs_diff_z": [round(float(v), 4) for v in e.mean(0)],
                                    "speed_decidable_windows": sl_n, "speed_decidable_accuracy": round(sl_hit / max(1, sl_n), 4),
                                    "class_counts": {c: sum(r["manoeuvre"] == c for r in rows) for c in CLASSES}}
        print(sid, json.dumps(summary["sessions"][sid]))
    us = np.array(all_us); c = cost({"embed": 0, **arch})      # two-head model: no driver head
    mhz = float(np.median(np.array(all_cyc) / np.maximum(us, 1)))
    summary["latency_us"] = {"median": float(np.median(us)), "p95": float(np.percentile(us, 95)), "max": int(us.max())}
    summary["iron_law"] = {"macs": c["macs"], "ops": 2 * c["macs"], "weights_bytes": c["weights_bytes"], "act_bytes_int8": c["act_bytes"],
                           "cpu_mhz_measured": round(mhz, 1),
                           "effective_gops": round(2 * c["macs"] / (np.median(us) * 1e-6) / 1e9, 3),
                           "predicted_compute_ms_s31_model": round(c["t_compute_ms"], 3), "measured_ms": round(float(np.median(us)) / 1000, 3),
                           "deadline_ms": 10000, "share_of_window_budget": round(float(np.median(us)) / 1e7, 6)}
    json.dump(summary, open(os.path.join(a.out, "bench_summary.json"), "w"), indent=1)
    print(json.dumps({k: summary[k] for k in ("latency_us", "iron_law")}, indent=1))


if __name__ == "__main__":
    main()
