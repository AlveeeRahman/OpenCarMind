"""Turn a carOBD log (Toyota Etios 2014, 1.5 L; https://github.com/eron93br/carOBD) into a CarMind session CSV + JSON in
the board's exact on-card format. Then run make_patterns.py on the CSV to add the .pat file and window_seq.

usage: python make_replay_carobd.py <carOBD csv> <out sessions dir> <session id> <obd_fields.h> <start epoch ms> <label>

The carOBD rows have no clock. ENGINE_RUN_TINE (PID 0x1F, whole seconds) repeats for consecutive rows, so each
row is placed evenly inside its run-time second; the logs come out at about two rows per second.
"""
import json, re, sys
import numpy as np

SRC, OUT, SID, FIELDS_H, START_MS, LABEL = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], int(sys.argv[5]), sys.argv[6]
MAC = "000000000000"
fields = re.findall(r'^X\(\w+,\s*"([^"]+)"', open(FIELDS_H).read(), re.M)
assert len(fields) == 70, len(fields)
header = ["seq", "uptime_ms", "epoch_ms"] + fields + ["adapter_voltage_v", "mil_on", "dtc_count", "dtcs", "pending_dtcs",
          "permanent_dtcs"] + [f"custom_{i}" for i in range(1, 9)] + ["window_seq"]

MAP = {  # CarMind CSV column <- carOBD column; both are standard SAE J1979 mode-01 values in the same units
    "run_time_s": "ENGINE_RUN_TINE", "rpm": "ENGINE_RPM", "speed_kph": "VEHICLE_SPEED", "throttle_pct": "THROTTLE",
    "engine_load_pct": "ENGINE_LOAD", "coolant_c": "COOLANT_TEMPERATURE", "ltft_b1_pct": "LONG_TERM_FUEL_TRIM_BANK_1",
    "stft_b1_pct": "SHORT_TERM_FUEL_TRIM_BANK_1", "map_kpa": "INTAKE_MANIFOLD_PRESSURE", "fuel_level_pct": "FUEL_TANK",
    "abs_throttle_b_pct": "ABSOLUTE_THROTTLE_B", "pedal_d_pct": "PEDAL_D", "pedal_e_pct": "PEDAL_E",
    "throttle_actuator_pct": "COMMANDED_THROTTLE_ACTUATOR", "equiv_ratio_cmd": "FUEL_AIR_COMMANDED_EQUIV_RATIO",
    "baro_kpa": "ABSOLUTE_BAROMETRIC_PRESSURE", "rel_throttle_pct": "RELATIVE_THROTTLE_POSITION",
    "intake_temp_c": "INTAKE_AIR_TEMP", "timing_adv_deg": "TIMING_ADVANCE", "cat_temp_b1s1_c": "CATALYST_TEMPERATURE_BANK1_SENSOR1",
    "cat_temp_b1s2_c": "CATALYST_TEMPERATURE_BANK1_SENSOR2", "module_voltage_v": "CONTROL_MODULE_VOLTAGE",
    "evap_purge_pct": "COMMANDED_EVAPORATIVE_PURGE", "time_mil_min": "TIME_RUN_WITH_MIL_ON",
    "time_since_clear_min": "TIME_SINCE_TROUBLE_CODES_CLEARED", "dist_mil_km": "DISTANCE_TRAVELED_WITH_MIL_ON",
    "warmups_since_clear": "WARM_UPS_SINCE_CODES_CLEARED",
}
assert all(k in fields for k in MAP), [k for k in MAP if k not in fields]

lines = open(SRC).read().splitlines()
src_cols = [h.split(" (")[0].strip() for h in lines[0].split(",")]
rows = []
for l in lines[1:]:
    v = [x.strip() for x in l.split(",") if x.strip() != ""]
    if len(v) == len(src_cols):            # skips the truncated first line
        rows.append([float(x) for x in v])
a = np.array(rows)
col = {h: a[:, i] for i, h in enumerate(src_cols)}

rt = col["ENGINE_RUN_TINE"]
t = np.empty(len(rt))                       # seconds since the first row
i = 0
while i < len(rt):
    j = i
    while j + 1 < len(rt) and rt[j + 1] == rt[i]: j += 1
    nxt = rt[j + 1] if j + 1 < len(rt) and rt[j + 1] > rt[i] else rt[i] + 1
    t[i:j + 1] = rt[i] + (np.arange(j - i + 1) / (j - i + 1)) * (nxt - rt[i])
    i = j + 1
t = t - t[0]
assert np.all(np.diff(t) > 0), "time must increase"

def fmt(v):                                 # same as sdlog.c fmt_num
    v = float(np.float32(v))
    return str(int(v)) if v == int(v) and abs(v) < 1e7 else f"{v:.3f}"

out = [",".join(header)]
for k in range(len(a)):
    vals = {"seq": k + 1, "uptime_ms": int(10000 + round(t[k] * 1000)), "epoch_ms": int(START_MS + round(t[k] * 1000)),
            "mil_on": 0, "dtc_count": 0, "window_seq": 0}
    for dst, src in MAP.items():
        vals[dst] = fmt(col[src][k])
    out.append(",".join(str(vals.get(c, "")) for c in header))
csv = "\n".join(out) + "\n"
open(f"{OUT}/s{SID:06d}.csv", "w", newline="\n").write(csv)

meta = {"schema_version": 1, "session_id": SID, "device_mac": MAC, "firmware": "OpenCarMind replay",
        "adapter": {}, "vehicle": {"supported_pids": {}},
        "time_sync": {"epoch_ms": START_MS, "uptime_ms": 10000}, "started_epoch_ms": START_MS,
        "rows": len(out) - 1, "meta_rev": 1, "dtc_history": [],
        "replay": {"source": f"carOBD dataset, Toyota Etios 2014 1.5 L, {LABEL}",
                   "vehicle": "Toyota Etios 2014 1.5 L",
                   "url": "https://github.com/eron93br/carOBD",
                   "cite": "Detecting Anomalies in the Engine Coolant Sensor Using One-Class Classifiers, https://ieeexplore.ieee.org/abstract/document/8891367",
                   "note": "replay data in the board's SD session format; not recorded by this board",
                   "timing": "carOBD rows have no clock; rows spread evenly inside each ENGINE_RUN_TINE second",
                   "mapped_columns": MAP}}
open(f"{OUT}/s{SID:06d}.json", "w").write(json.dumps(meta, separators=(",", ":")))
print(f"s{SID:06d}: {len(out)-1} rows, {len(csv)/1024:.0f} KB, {t[-1]/60:.1f} min, {(len(out)-1)/t[-1]:.2f} rows/s")
