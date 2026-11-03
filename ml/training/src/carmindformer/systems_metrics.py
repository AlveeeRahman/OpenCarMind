"""ML-systems cost model for the on-board network (nas_edge.Net configs), following the Iron Law and the energy tax:

    T = D_tot / BW + O / (R_peak * eta) + L_lat          E ~= D_tot * E_move + MACs * E_mac

D_tot is bytes moved per inference (int8 weights read once + every layer's input and output activations),
O = 2 * MACs. peak_act is the largest single layer's input + output, the SRAM the activations need at once.

Machine numbers for the ESP32-S31: 320 MHz RISC-V core, 512 KB SRAM and 250 MHz 8-bit DDR PSRAM (0.5 GB/s) are from
Espressif's S31 page. R_PEAK assumes 16 int8 MACs per cycle on the 128-bit SIMD unit; that figure is not verified.
Energy constants are Horowitz (ISSCC 2014) 45 nm values, used for ratios, not absolute joules.
"""
from __future__ import annotations

R_PEAK = 2 * 16 * 320e6          # ops/s (1 MAC = 2 ops), ASSUMED SIMD width
ETA = 0.3
BW_PSRAM = 0.5e9                 # bytes/s
SRAM_BYTES = 512 * 1024
E_MOVE = 160e-12                 # J per DRAM-class byte
E_MAC = 0.2e-12                  # J per int8 MAC


def cost(a: dict, n_classes: int = 10, n_man: int = 4, n_est: int = 3) -> dict:
    W, C, L, k = a["W"], a["width"], a["layers"], a["kernel"]
    cin = 30 if a.get("deltas") else 20
    macs = W * cin * C
    wts = cin * C + C
    acts = W * cin + W * C
    peak = W * cin + W * C
    for _ in range(L):
        if a["block"] == "std":
            macs += 2 * W * C * C * k; wts += 2 * (C * C * k + C); acts += 2 * W * C
        else:
            macs += 2 * (W * C * k + W * C * C); wts += 2 * (C * k + C * C + 2 * C); acts += 4 * W * C
        peak = max(peak, 2 * W * C)
    head = C * a["embed"] + a["embed"] * n_classes + 2 * C * a["local"] + a["local"] * (n_man + n_est)
    macs += head; wts += head
    D = wts + acts
    O = 2 * macs
    return {"macs": macs, "weights_bytes": wts, "act_bytes": acts, "peak_act_bytes": peak, "D_tot": D,
            "intensity": O / D, "t_data_ms": 1e3 * D / BW_PSRAM, "t_compute_ms": 1e3 * O / (R_PEAK * ETA),
            "energy_uJ": 1e6 * (D * E_MOVE + macs * E_MAC), "data_energy_share": D * E_MOVE / (D * E_MOVE + macs * E_MAC),
            "fits_sram": wts + peak < SRAM_BYTES // 4}      # keep the model to a quarter of SRAM; firmware needs the rest
