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

/* ---- Board page: the USB settings console (docs/CONSOLE.md) ---- */
const CONFIG_RANGES = { poll_interval_ms: [50, 5000], slow_group_every: [1, 50], dtc_interval_s: [10, 3600], backlight_pct: [5, 100], theme_mode: [0, 2], day_start_hour: [0, 23], day_end_hour: [0, 23] };
const LABELS = { firmware: "Firmware", model: "Model", uptime_s: "Running for (s)", mode: "Mode", sd: "SD card", session: "Session", clock: "Clock", wifi_networks: "Saved networks", adapter_saved: "Adapter saved", adapter_name: "Adapter" };
let boardConfig = {};

function say(text, bad) { const m = $("#board-msg"); m.textContent = text; m.classList.toggle("bad", !!bad); }
function busy(on) { for (const b of document.querySelectorAll("#page-board button")) b.disabled = on; }
function kv(lines) { const o = {}; for (const l of lines) { const i = l.indexOf("="); if (i > 0) o[l.slice(0, i)] = l.slice(i + 1); } return o; }

async function board(...args) {
  const port = $("#port").value;
  if (!port) throw new Error("Choose the board's USB port first.");
  const a = await invoke("board_command", { port, args });
  if (!a.ok) throw new Error(a.error || "the board refused");
  return a.lines;
}

async function refreshPorts() {
  try {
    const ports = await invoke("board_ports");
    const sel = $("#port"), keep = sel.value || recall("port");
    sel.replaceChildren(...ports.map((p) => el("option", { value: p.name, textContent: `${p.name} (${p.label}${p.likely_board ? ", looks like the board" : ""})` })));
    if (keep && ports.some((p) => p.name === keep)) sel.value = keep;
    say(ports.length ? "" : "No serial ports found. Plug the board in and press Refresh.");
  } catch (e) { say(String(e), true); }
}

async function readBoard() {
  busy(true); say("Reading the board...");
  try {
    const status = kv(await board("status"));
    remember("port", $("#port").value);
    $("#board-status tbody").replaceChildren(...Object.entries(status).map(([k, v]) => el("tr", {}, el("td", { textContent: LABELS[k] || k }), el("td", { textContent: v }))));
    $("#board-status").hidden = false;
    $("#adapter-text").textContent = status.adapter_saved === "yes" ? `The board connects to ${status.adapter_name} automatically. Forget it to pair a different adapter.` : "No adapter is saved. The board pairs with the first OBD adapter it finds.";
    const nets = (await board("wifi", "list")).map((l) => l.replace(/^\d+ /, ""));
    $("#wifi-list").replaceChildren(...(nets.length ? nets : [null]).map((n) => n === null ? el("li", { textContent: "No networks saved." }) :
      el("li", {}, el("span", { textContent: n }), Object.assign(el("button", { textContent: "Forget" }), { onclick: () => change(["wifi", "forget", n], `Forgot ${n}.`) }))));
    boardConfig = kv(await board("config", "show"));
    $("#config tbody").replaceChildren(...Object.keys(CONFIG_RANGES).filter((k) => k in boardConfig).map((k) => {
      const [lo, hi] = CONFIG_RANGES[k];
      const input = el("input", { type: "number", min: lo, max: hi, step: 1, value: boardConfig[k], id: "cfg-" + k });
      return el("tr", {}, el("td", {}, el("label", { htmlFor: "cfg-" + k, textContent: k.replaceAll("_", " ") })), el("td", {}, input), el("td", { className: "sub", textContent: `${lo} to ${hi}` }));
    }));
    $("#board-tools").hidden = false;
    say(`Connected. Mode: ${status.mode}.`);
  } catch (e) { say(String(e.message || e), true); }
  busy(false);
}

async function change(args, done) {
  busy(true);
  try { await board(...args); say(done); busy(false); await readBoard(); }
  catch (e) { say(String(e.message || e), true); busy(false); }
}

$("#refresh-ports").addEventListener("click", refreshPorts);
$("#read-board").addEventListener("click", readBoard);
$("#wifi-add").addEventListener("click", () => {
  const ssid = $("#ssid").value, pass = $("#wpass").value;
  if (!ssid) return say("Type the network name.", true);
  if (pass && (pass.length < 8 || pass.length > 64)) return say("A Wi-Fi password has 8 to 64 characters. Leave it empty for an open network.", true);
  change(pass ? ["wifi", "add", ssid, pass] : ["wifi", "add", ssid], `Saved ${ssid}.`).then(() => { $("#wpass").value = ""; });
});
$("#adapter-forget").addEventListener("click", () => {
  if (confirm("Forget the saved adapter? The board will search for an adapter again.")) change(["adapter", "forget"], "Adapter forgotten. The board is searching.");
});
$("#config-save").addEventListener("click", async () => {
  busy(true);
  try {
    let n = 0;
    for (const k of Object.keys(CONFIG_RANGES)) {
      const input = document.getElementById("cfg-" + k);
      if (input && input.value !== boardConfig[k]) { await board("config", "set", k, input.value); n++; }
    }
    if (!n) { say("Nothing changed."); busy(false); return; }
    await board("reboot");
    say(`Saved ${n} setting${n > 1 ? "s" : ""}. The board is restarting. Read it again in about 15 seconds.`);
  } catch (e) { say(String(e.message || e), true); }
  busy(false);
});
document.querySelector('.tab[data-page="board"]').addEventListener("click", () => { if (!$("#port").options.length) refreshPorts(); });
