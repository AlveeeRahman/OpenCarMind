"use strict";
const { invoke } = window.__TAURI__.core;
const { open } = window.__TAURI__.dialog;
const $ = (s) => document.querySelector(s);
const NS = "http://www.w3.org/2000/svg";
const state = { folder: null, sessions: [] };

function el(tag, props, ...kids) {
  const e = document.createElement(tag);
  Object.assign(e, props || {});
  for (const k of kids) e.append(k);
  return e;
}
function remember(key, value) { try { localStorage.setItem(key, value); } catch (_) { /* storage can be blocked */ } }
function recall(key) { try { return localStorage.getItem(key); } catch (_) { return null; } }

function show(page) {
  for (const t of document.querySelectorAll(".tab")) {
    if (t.dataset.page === page) t.setAttribute("aria-current", "page"); else t.removeAttribute("aria-current");
  }
  for (const s of document.querySelectorAll(".page")) s.hidden = s.id !== "page-" + page;
}
for (const t of document.querySelectorAll(".tab")) t.addEventListener("click", () => show(t.dataset.page));

function fmt(v, digits) { return v === null || v === undefined ? "n/a" : Number(v).toFixed(digits); }

function renderOverview() {
  const s = state.sessions;
  $("#empty").hidden = s.length > 0;
  $("#cards").hidden = $("#bars").hidden = s.length === 0;
  if (!s.length) return;
  const km = s.reduce((a, x) => a + x.distance_km, 0);
  const min = s.reduce((a, x) => a + x.minutes, 0);
  const top = Math.max(...s.map((x) => x.max_speed_kph ?? 0));
  const cards = [["Number of drives", String(s.length)], ["Km driven", km.toFixed(1)], ["Drive time (h)", (min / 60).toFixed(1)], ["Highest speed (km/h)", top.toFixed(0)]];
  $("#cards").replaceChildren(...cards.map(([k, v]) => el("div", { className: "card" }, el("div", { className: "k", textContent: k }), el("div", { className: "v", textContent: v }))));
  const bars = (title, pick, digits) => {
    const max = Math.max(...s.map(pick), 1e-9);
    return el("div", { className: "bar-card" }, el("h2", { textContent: title }),
      ...s.slice().reverse().slice(0, 8).map((x) => {
        const fill = el("div", { className: "fill" }); fill.style.width = (100 * pick(x) / max).toFixed(0) + "%";
        return el("div", { className: "bar" }, el("span", { textContent: x.id }), el("div", { className: "track" }, fill), el("span", { className: "num", textContent: pick(x).toFixed(digits) }));
      }));
  };
  $("#bars").replaceChildren(bars("Kilometres per drive", (x) => x.distance_km, 1), bars("Minutes per drive", (x) => x.minutes, 1), bars("Top speed per drive (km/h)", (x) => x.max_speed_kph ?? 0, 0));
}

function renderDrives() {
  const body = $("#drives tbody");
  body.replaceChildren(...state.sessions.slice().reverse().map((x) => {
    const tr = el("tr", { tabIndex: 0 },
      el("td", { textContent: x.id }), el("td", { textContent: x.date }),
      el("td", { className: "n", textContent: x.rows.toLocaleString() }), el("td", { className: "n", textContent: fmt(x.minutes, 1) }),
      el("td", { className: "n", textContent: fmt(x.max_speed_kph, 0) }), el("td", { className: "n", textContent: fmt(x.distance_km, 1) }));
    const choose = () => showDetail(x, tr);
    tr.addEventListener("click", choose);
    tr.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); choose(); } });
    return tr;
  }));
}

function chart(title, unit, xs, ys) {
  const pts = xs.map((x, i) => [x, ys[i]]).filter(([x, y]) => Number.isFinite(x) && y !== null && Number.isFinite(y));
  if (pts.length < 2) return null;
  const W = 900, H = 140, x0 = pts[0][0], x1 = pts[pts.length - 1][0];
  const lo = Math.min(...pts.map((p) => p[1])); let hi = Math.max(...pts.map((p) => p[1])); if (hi <= lo) hi = lo + 1;
  const px = (x) => 50 + (x - x0) / Math.max(x1 - x0, 1e-9) * (W - 60);
  const py = (y) => H - 22 - (y - lo) / (hi - lo) * (H - 58);
  const svg = document.createElementNS(NS, "svg");
  svg.setAttribute("viewBox", `0 0 ${W} ${H}`); svg.setAttribute("role", "img"); svg.setAttribute("aria-label", `${title} over time`);
  const add = (tag, attrs, text) => { const n = document.createElementNS(NS, tag); for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v); if (text !== undefined) n.textContent = text; svg.append(n); };
  add("text", { x: 4, y: 14, "font-size": 13, "font-weight": 600, fill: "currentColor" }, `${title} (${unit})`);
  add("text", { x: 46, y: py(hi) + 4, "font-size": 11, "text-anchor": "end", fill: "currentColor" }, hi.toFixed(0));
  add("text", { x: 46, y: py(lo) + 4, "font-size": 11, "text-anchor": "end", fill: "currentColor" }, lo.toFixed(0));
  add("line", { x1: 50, y1: H - 22, x2: W - 10, y2: H - 22, stroke: "currentColor", "stroke-opacity": 0.25 });
  add("text", { x: 50, y: H - 6, "font-size": 11, fill: "currentColor" }, `${x0.toFixed(0)} min`);
  add("text", { x: W - 10, y: H - 6, "font-size": 11, "text-anchor": "end", fill: "currentColor" }, `${x1.toFixed(0)} min`);
  add("polyline", { points: pts.map(([x, y]) => `${px(x).toFixed(1)},${py(y).toFixed(1)}`).join(" "), fill: "none", stroke: "#0f766e", "stroke-width": 1.8, "stroke-linejoin": "round" });
  return el("div", { className: "chart" }, svg);
}

async function showDetail(info, row) {
  for (const r of document.querySelectorAll("#drives tbody tr")) r.removeAttribute("aria-selected");
  row.setAttribute("aria-selected", "true");
  const box = $("#detail");
  box.replaceChildren(el("p", { className: "sub", textContent: `Reading ${info.id}...` }));
  try {
    const series = await invoke("session_series", { folder: state.folder, id: info.id });
    const charts = series.channels.map((c) => chart(c.name.replaceAll("_", " "), c.unit, series.minutes, c.values)).filter(Boolean);
    box.replaceChildren(el("h2", { textContent: `${info.id}, ${info.date}` }),
      ...(charts.length ? charts : [el("p", { className: "sub", textContent: "This drive has no chartable columns." })]),
      ...(info.has_model_file ? [] : [el("p", { className: "sub", textContent: "No model results file (.ml.csv) for this drive." })]));
  } catch (e) {
    box.replaceChildren(el("div", { className: "note warn", textContent: String(e) }));
  }
}

async function load(folder) {
  const error = $("#error"); error.hidden = true;
  try {
    state.sessions = await invoke("read_sessions", { folder });
    state.folder = folder;
    remember("folder", folder);
    $("#status").textContent = `${state.sessions.length} drives from ${folder}`;
    if (!state.sessions.length) { error.textContent = "No drives found in that folder. Look for files named like s000111.csv."; error.hidden = false; }
  } catch (e) {
    state.sessions = [];
    $("#status").textContent = "No card folder open";
    error.textContent = String(e); error.hidden = false;
  }
  renderOverview(); renderDrives(); $("#detail").replaceChildren();
}

$("#open-card").addEventListener("click", async () => {
  const picked = await open({ directory: true, multiple: false, title: "Choose the SD card's obd folder" });
  if (typeof picked === "string") load(picked);
});

const last = recall("folder");
if (last) load(last);
