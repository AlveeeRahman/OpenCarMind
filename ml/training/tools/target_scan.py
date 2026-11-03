"""Which extra estimator targets can be learned from the channels that almost every OBD-II car provides?
Inputs: 30 s windows of rpm, speed, load, throttle, stft, ltft, coolant (no MAP, no pedal).
Targets: value at the window end. Grouped CV by (driver, path). Run from the repo root: PYTHONPATH=src python tools/target_scan.py [path-to-HCRL-driving-csv]"""
import sys, warnings
warnings.filterwarnings("ignore")
import numpy as np
from sklearn.ensemble import HistGradientBoostingRegressor, HistGradientBoostingClassifier
from sklearn.model_selection import GroupKFold
from sklearn.metrics import r2_score, roc_auc_score
from carmindformer.data import load_hcrl_driving

PATH = sys.argv[1] if len(sys.argv) > 1 else "data/driving_dataset/extracted/Driving Data(KIA SOUL)_(150728-160714)_(10 Drivers_A-J).csv"
INPUTS = ["rpm", "speed_kph", "engine_load_pct", "throttle_pct", "stft_b1_pct", "ltft_b1_pct", "coolant_c"]
W, STRIDE = 30, 5
REG = ["brake_pressure_bar", "flywheel_torque_nm", "fuel_rate_lph", "accel_long_g", "road_gradient_pct", "torque_actual_pct",
       "friction_torque_pct", "pedal_d_pct", "map_kpa", "steering_angle_deg", "accel_lat_g", "yaw_rate_dps", "gear"]
BIN = ["fuel_cutoff", "brake_switch", "ac_compressor"]

series = load_hcrl_driving(PATH)
print("series (driver,path):", len(series), "| channels:", len(series[0].channels))

def feats(win):                              # win [C, W]
    f = []
    for c in win:
        t = np.arange(W)
        ok = ~np.isnan(c)
        if ok.sum() < W // 2:
            f += [np.nan] * 7; continue
        cc = np.where(ok, c, np.nanmean(c))
        f += [cc[-1], cc.mean(), cc.std(), cc.min(), cc.max(), np.polyfit(t, cc, 1)[0], cc[-10:].mean()]
    return f

X, G = [], []
Y = {k: [] for k in REG + BIN}
for s in series:
    idx = {n: i for i, n in enumerate(s.channels)}
    if not all(n in idx for n in INPUTS): continue
    v = s.values
    for e in range(W, v.shape[1], STRIDE):
        X.append(feats(v[[idx[n] for n in INPUTS], e - W:e])); G.append(s.name)
        for k in Y:
            Y[k].append(v[idx[k], e - 1] if k in idx else np.nan)
X = np.array(X, dtype=np.float32); G = np.array(G)
print("windows:", len(X), "| groups:", len(set(G)))
gkf = GroupKFold(n_splits=5)

def cv(y, clf):
    y = np.array(y, dtype=np.float32); m = ~np.isnan(y)
    if m.sum() < 500 or np.nanstd(y[m]) == 0: return None
    pred = np.full(len(y), np.nan)
    for tr, te in gkf.split(X[m], y[m], G[m]):
        mod = HistGradientBoostingClassifier(max_iter=120, learning_rate=0.1) if clf else HistGradientBoostingRegressor(max_iter=120, learning_rate=0.1)
        yy = (y[m] > 0.5).astype(int) if clf else y[m]
        mod.fit(X[m][tr], yy[tr])
        pred[np.where(m)[0][te]] = mod.predict_proba(X[m][te])[:, 1] if clf else mod.predict(X[m][te])
    ok = ~np.isnan(pred)
    return (roc_auc_score((y[ok] > 0.5).astype(int), pred[ok]) if clf else r2_score(y[ok], pred[ok])), int(m.sum())

print("\n--- regression targets (R2 on held-out driver/paths; 1.0 perfect, 0 = no better than the mean)")
for k in REG:
    r = cv(Y[k], False)
    print(f"{k:22}", "not in dataset/constant" if r is None else f"R2 {r[0]:6.3f}   n={r[1]}")
print("\n--- binary targets (AUC; 0.5 = chance)")
for k in BIN:
    r = cv(Y[k], True)
    print(f"{k:22}", "not in dataset/constant" if r is None else f"AUC {r[0]:5.3f}   n={r[1]}")
