#!/usr/bin/env python3
"""Turn OpenCarMind session files from the SD card into a small local website and a summary for Dashy.

    python3 tools/ocm_report.py /Volumes/SDCARD/obd dashboard/site
    python3 tools/ocm_report.py /Volumes/SDCARD/obd dashboard/site --watch 30

The input is the card's `obd` folder, its `sessions` folder, or any folder that holds sNNNNNN.csv files. For each
session the tool writes sNNNNNN.html (charts as inline SVG, no external requests), plus index.html, latest.html and
summary.json. If a sNNNNNN.ml.csv file is next to the CSV, the model's manoeuvre classes are drawn under the charts.
Only the Python standard library is needed.
"""
import argparse
import csv
import html
import json
import re
import statistics
import time
from pathlib import Path

CLASSES = ["stopped", "braking", "accelerating", "cruising"]
CLASS_COLORS = {"stopped": "#8a94a6", "braking": "#dc2626", "accelerating": "#f59e0b", "cruising": "#0f766e"}
CHARTS = [("Speed", "speed_kph", "km/h", "#0f766e"), ("Engine speed", "rpm", "rpm", "#b45309"),
          ("Coolant", "coolant_c", "°C", "#2563eb"), ("Throttle", "throttle_pct", "%", "#7c3aed")]
PAGE = ("<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>{title}</title><style>body{{font:16px system-ui,sans-serif;margin:24px auto;max-width:960px;padding:0 16px;"
        "color:#1f2933}}h1{{font-size:24px}}table{{border-collapse:collapse;width:100%}}td,th{{padding:6px 10px;"
        "border-bottom:1px solid #d9dee5;text-align:left}}svg{{width:100%;height:auto;margin:4px 0}}.k{{color:#5b6470}}"
        "a{{color:#0f766e}}</style>{body}")


def number(text):
    try:
        value = float(text)
    except (TypeError, ValueError):
        return None
    return value if value == value else None


def read_rows(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def column(rows, name):
    return [number(r.get(name)) for r in rows]


def summarise(session_id, rows, model_rows):
    uptime = column(rows, "uptime_ms")
    speed = column(rows, "speed_kph")
    gaps = [(b - a) / 1000 for a, b in zip(uptime, uptime[1:]) if a is not None and b is not None]
    km = sum((s or 0) * g / 3600 for s, g in zip(speed, gaps))
    epoch = number(rows[0].get("epoch_ms")) or 0
    shares = {c: 0.0 for c in CLASSES}
    for r in model_rows:
        if r.get("manoeuvre") in shares:
            shares[r["manoeuvre"]] += 1
    n = sum(shares.values())
    result = {
        "id": session_id,
        "date": time.strftime("%Y-%m-%d %H:%M", time.gmtime(epoch / 1000)) + " UTC" if epoch else "clock not set",
        "rows": len(rows),
        "minutes": round((uptime[-1] - uptime[0]) / 60000, 1),
        "row_interval_s": round(statistics.median(gaps), 2) if gaps else None,
        "max_speed_kph": max((s for s in speed if s is not None), default=None),
        "distance_km": round(km, 1),
    }
    if n:
        result.update({f"{c}_pct": round(100 * shares[c] / n) for c in CLASSES})
        for key in ("brake_pressure_bar", "flywheel_torque_nm", "fuel_rate_lph"):
            values = [v for v in column(model_rows, f"est_{key}") if v is not None]
            result[f"mean_{key}"] = round(statistics.fmean(values), 1) if values else None
    return result


def line_chart(title, unit, color, xs, ys, width=900, height=130):
    points = [(x, y) for x, y in zip(xs, ys) if x is not None and y is not None]
    if len(points) < 2:
        return ""
    x0, x1 = points[0][0], points[-1][0]
    y0, y1 = min(y for _, y in points), max(y for _, y in points)
    y1 = y1 if y1 > y0 else y0 + 1
    px = lambda x: 50 + (x - x0) / max(x1 - x0, 1e-9) * (width - 60)
    py = lambda y: height - 22 - (y - y0) / (y1 - y0) * (height - 58)
    path = " ".join(f"{px(x):.1f},{py(y):.1f}" for x, y in points)
    return (f"<svg viewBox='0 0 {width} {height}' role=img aria-label='{html.escape(title)}'>"
            f"<text x=4 y=14 font-size=13 font-weight=600>{html.escape(title)} ({html.escape(unit)})</text>"
            f"<text x=46 y={py(y1) + 4:.0f} font-size=11 text-anchor=end fill='#5b6470'>{y1:.0f}</text>"
            f"<text x=46 y={py(y0) + 4:.0f} font-size=11 text-anchor=end fill='#5b6470'>{y0:.0f}</text>"
            f"<line x1=50 y1={height - 22} x2={width - 10} y2={height - 22} stroke='#d9dee5'/>"
            f"<text x=50 y={height - 6} font-size=11 fill='#5b6470'>{x0:.0f} min</text>"
            f"<text x={width - 10} y={height - 6} font-size=11 text-anchor=end fill='#5b6470'>{x1:.0f} min</text>"
            f"<polyline points='{path}' fill=none stroke='{color}' stroke-width=1.8 stroke-linejoin=round /></svg>")


def class_band(minutes, model_rows, first_uptime, width=900):
    cells = []
    for r in model_rows:
        end, kind = number(r.get("end_ms")), r.get("manoeuvre")
        if end is not None and kind in CLASS_COLORS:
            cells.append(((end - first_uptime) / 60000, kind))
    if len(cells) < 2:
        return ""
    px = lambda x: 50 + x / max(minutes, 1e-9) * (width - 60)
    step = max(px(b[0]) - px(a[0]) for a, b in zip(cells, cells[1:]))
    rects = "".join(f"<rect x={px(x) - step / 2:.1f} y=22 width={step + 0.5:.1f} height=24 fill='{CLASS_COLORS[k]}' />"
                    for x, k in cells)
    legend = "".join(f"<rect x={50 + i * 130} y=58 width=10 height=10 fill='{CLASS_COLORS[c]}' />"
                     f"<text x={66 + i * 130} y=68 font-size=12>{c}</text>" for i, c in enumerate(CLASSES))
    return (f"<svg viewBox='0 0 {width} 76' role=img aria-label='Manoeuvre classes'><text x=4 y=14 font-size=13 "
            f"font-weight=600>What the model saw</text>{rects}{legend}</svg>")


def session_page(info, rows, model_rows):
    uptime = column(rows, "uptime_ms")
    minutes = [(u - uptime[0]) / 60000 if u is not None else None for u in uptime]
    charts = "".join(line_chart(t, u, c, minutes, column(rows, name)) for t, name, u, c in CHARTS)
    band = class_band(info["minutes"], model_rows, uptime[0]) if model_rows else \
        "<p class=k>No model results file (.ml.csv) for this session.</p>"
    facts = "".join(f"<tr><td class=k>{html.escape(k.replace('_', ' '))}</td><td>{html.escape(str(v))}</td></tr>"
                    for k, v in info.items() if k != "id")
    body = (f"<h1>Session {info['id']}</h1><p><a href=index.html>All drives</a></p><table>{facts}</table>"
            f"{charts}{band}")
    return PAGE.format(title=f"Session {info['id']}", body=body)


def index_page(infos):
    rows = "".join(f"<tr><td><a href='{i['id']}.html'>{i['id']}</a></td><td>{i['date']}</td><td>{i['minutes']}</td>"
                   f"<td>{i['distance_km']}</td><td>{i['max_speed_kph']}</td></tr>" for i in reversed(infos))
    body = (f"<h1>OpenCarMind drives</h1><table><tr><th>Session<th>Start<th>Minutes<th>km (estimated)<th>Max km/h</tr>"
            f"{rows}</table>")
    return PAGE.format(title="OpenCarMind drives", body=body)


def find_sessions(source):
    source = Path(source)
    folder = source / "sessions" if (source / "sessions").is_dir() else source
    return sorted(p for p in folder.glob("s[0-9]*.csv") if re.fullmatch(r"s\d+\.csv", p.name))


def build(source, out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    infos = []
    for path in find_sessions(source):
        rows = read_rows(path)
        if len(rows) < 2:
            continue
        model_path = path.with_name(path.stem + ".ml.csv")
        model_rows = read_rows(model_path) if model_path.exists() else []
        info = summarise(path.stem, rows, model_rows)
        (out / f"{path.stem}.html").write_text(session_page(info, rows, model_rows))
        infos.append(info)
    if not infos:
        raise SystemExit(f"no sessions with data found under {source}")
    (out / "index.html").write_text(index_page(infos))
    (out / "latest.html").write_text((out / f"{infos[-1]['id']}.html").read_text())
    summary = {"generated": time.strftime("%Y-%m-%d %H:%M:%S"), "sessions": len(infos), "latest": infos[-1],
               "total_km": round(sum(i["distance_km"] for i in infos), 1),
               "total_minutes": round(sum(i["minutes"] for i in infos), 1)}
    (out / "summary.json").write_text(json.dumps(summary, indent=1))
    return infos


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("source", help="the card's obd folder, its sessions folder, or a folder of sNNNNNN.csv files")
    p.add_argument("out", help="output folder, for example dashboard/site")
    p.add_argument("--watch", type=int, metavar="SECONDS", help="rebuild every SECONDS while the source exists")
    args = p.parse_args()
    while True:
        if Path(args.source).exists():
            infos = build(args.source, args.out)
            print(f"{time.strftime('%H:%M:%S')}  {len(infos)} sessions, latest {infos[-1]['id']}", flush=True)
        elif not args.watch:
            raise SystemExit(f"{args.source} does not exist")
        if not args.watch:
            break
        time.sleep(args.watch)


if __name__ == "__main__":
    main()
