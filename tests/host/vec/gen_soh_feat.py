"""Feature vectors for main/bat_soh.c: the reference functions q_between and ica_peak copied verbatim from carmind-ml
experiments/battery_soc/p3_soh.py, run on synthetic CC-charge curves sampled on a 10 mV grid (the board's Q(V) grid).
Usage: ~/Desktop/dev/metal_env/bin/python -I gen_soh_feat.py > soh_feat_vectors.csv
Columns: case, grid Q in units of Q_nom (56 values), exp_dq_win, exp_ica_h, exp_ica_v."""
import numpy as np


def q_between(V, Q, v1, v2):
    o = np.argsort(V)
    Vs, Qs = V[o], Q[o]
    if Vs[0] > min(v1, v2) or Vs[-1] < max(v1, v2):
        return np.nan
    return abs(np.interp(v2, Vs, Qs) - np.interp(v1, Vs, Qs))


def ica_peak(V, Q, lo, hi, step=0.01):
    o = np.argsort(V)
    Vs, Qs = V[o], Q[o]
    if Vs[0] > lo or Vs[-1] < hi:
        return np.nan, np.nan
    g = np.arange(lo, hi, step)
    q = np.interp(g, Vs, Qs)
    d = np.convolve(np.gradient(q, step), np.ones(5) / 5, "same")[2:-2]
    k = int(np.argmax(np.abs(d)))
    return float(abs(d[k])), float(g[2:-2][k])


rng = np.random.default_rng(7)
g = np.arange(3.6, 4.15, 0.01)
assert len(g) == 56
print("case," + ",".join(f"q{k}" for k in range(56)) + ",exp_dq_win,exp_ica_h,exp_ica_v")
for case in range(40):
    # a smooth charge curve with one or two dQ/dV peaks (logistic steps in Q(V)), then sampled at the grid voltages
    p1, w1, a1 = rng.uniform(3.65, 4.05), rng.uniform(0.02, 0.08), rng.uniform(0.2, 0.6)
    p2, w2, a2 = rng.uniform(3.7, 4.1), rng.uniform(0.02, 0.1), rng.uniform(0.0, 0.4)
    V = np.concatenate([g, [4.16]])
    Q = a1 / (1 + np.exp(-(V - p1) / w1)) + a2 / (1 + np.exp(-(V - p2) / w2)) + 0.05 * (V - 3.6)
    Q = (Q - Q[0]).astype(np.float32).astype(float)
    dq = q_between(V, Q, 3.9, 4.1)
    h, v = ica_peak(V, Q, 3.6, 4.15)
    print(f"{case}," + ",".join("%.9g" % x for x in Q[:56]) + f",{dq:.9g},{h:.9g},{v:.9g}")
