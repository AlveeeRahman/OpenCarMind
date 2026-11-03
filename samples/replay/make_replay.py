"""Turn one HCRL KIA Soul drive into a CarMind session (CSV + JSON) in the board's exact on-card format,
so the board uploads it from /obd/sessions like any session it recorded itself."""
import json, re, sys, time
import pandas as pd

SRC = sys.argv[1]; OUT = sys.argv[2]; SID = int(sys.argv[3]); MAC = "000000000000"
fields = re.findall(r'^X\(\w+,\s*"([^"]+)"', open(sys.argv[4]).read(), re.M)
assert len(fields) == 70, len(fields)
header = ["seq", "uptime_ms", "epoch_ms"] + fields + ["adapter_voltage_v", "mil_on", "dtc_count", "dtcs", "pending_dtcs",
          "permanent_dtcs"] + [f"custom_{i}" for i in range(1, 9)] + ["window_seq"]

MAP = {  # CarMind CSV column <- KIA Soul column (same physical unit)
    "rpm": "Engine_speed", "speed_kph": "Vehicle_speed", "coolant_c": "Engine_coolant_temperature",
    "engine_load_pct": "Calculated_LOAD_value", "stft_b1_pct": "Short_Term_Fuel_Trim_Bank1",
    "ltft_b1_pct": "Long_Term_Fuel_Trim_Bank1", "map_kpa": "Intake_air_pressure",
    "throttle_pct": "Absolute_throttle_position", "timing_adv_deg": "Current_spark_timing",
    "rel_pedal_pct": "Accelerator_Pedal_value",
}
df = pd.read_csv(SRC)
r = df[(df.Class == "A") & (df.PathOrder == 1)].reset_index(drop=True)
t = r["Time(s)"].to_numpy()
cut = next((i for i in range(1, len(t)) if t[i] <= t[i - 1]), len(t))   # first drive only: time restarts after it
r = r.iloc[:cut]

def fmt(v):
    if pd.isna(v): return ""
    v = float(v)
    return str(int(v)) if v == int(v) and abs(v) < 1e7 else f"{v:.3f}"

start_ms = int(time.time() // 60 * 60 * 1000)          # whole minute "now"
lines = [",".join(header)]
t0 = float(r["Time(s)"].iloc[0])
for k, rd in enumerate(r.to_dict("records")):
    ts = float(rd["Time(s)"]) - t0
    vals = {"seq": k + 1, "uptime_ms": int(10000 + ts * 1000), "epoch_ms": int(start_ms + ts * 1000),
            "mil_on": 0, "dtc_count": 0, "window_seq": int(ts // 10)}
    for dst, src in MAP.items():
        vals[dst] = fmt(rd[src])
    lines.append(",".join(str(vals.get(c, "")) for c in header))
csv = "\n".join(lines) + "\n"
open(f"{OUT}/s{SID:06d}.csv", "w", newline="\n").write(csv)

meta = {"schema_version": 1, "session_id": SID, "device_mac": MAC, "firmware": "OpenCarMind replay",
        "adapter": {}, "vehicle": {"supported_pids": {}},
        "time_sync": {"epoch_ms": start_ms, "uptime_ms": 10000}, "started_epoch_ms": start_ms,
        "rows": len(lines) - 1, "meta_rev": 1, "dtc_history": [],
        "replay": {"source": "HCRL Driving Dataset, KIA Soul, driver A, path 1, first drive", "vehicle": "KIA Soul (HCRL Driving Dataset)",
                   "url": "https://ocslab.hksecurity.net/Datasets/driving-dataset",
                   "note": "replay data in the board's SD session format; not recorded by this board",
                   "mapped_columns": MAP}}
open(f"{OUT}/s{SID:06d}.json", "w").write(json.dumps(meta, separators=(",", ":")))
print(f"s{SID:06d}: {len(lines)-1} rows, {len(csv)/1024:.0f} KB, {ts:.0f} s of driving, starts {time.strftime('%Y-%m-%d %H:%M', time.localtime(start_ms/1000))}")
