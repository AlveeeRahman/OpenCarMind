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

/* ---- Card and Wi-Fi page ---- */
const SETTINGS = { poll_interval_ms: [50, 5000], slow_group_every: [1, 50], dtc_interval_s: [10, 3600], backlight_pct: [5, 100], theme_mode: [0, 2], day_start_hour: [0, 23], day_end_hour: [0, 23] };
const networks = [];
let cardFolder = null;

function msg(id, text, bad) { const m = $(id); m.textContent = text; m.classList.toggle("bad", !!bad); }

function ago(s) {
  const d = Math.max(0, Math.round(Date.now() / 1000 - s));
  return d < 60 ? `${d} s ago` : d < 3600 ? `${Math.round(d / 60)} min ago` : `${Math.round(d / 3600)} h ago`;
}

async function refreshReceiver() {
  try {
    const s = await invoke("receiver_status");
    if (s.error) msg("#rx-status", `Not receiving: ${s.error}`, true);
    else msg("#rx-status", s.running ? `Listening at ${s.address || "this computer"}, port ${s.port}. Drives are stored in ${s.folder}.` : "Starting...");
    $("#rx-boards").replaceChildren(...s.boards.map((b) => el("li", {}, el("span", { textContent: `Board ${b.id} (firmware ${b.firmware || "?"})` }), el("span", { className: "sub", textContent: `last heard ${ago(b.last_seen_s)}` }))));
  } catch (e) { msg("#rx-status", String(e), true); }
}

function drawNetworks() {
  $("#net-list").replaceChildren(...networks.map(([ssid], i) => el("li", {}, el("span", { textContent: ssid }),
    Object.assign(el("button", { textContent: "Remove" }), { onclick: () => { networks.splice(i, 1); drawNetworks(); } }))));
}

$("#settings tbody").replaceChildren(...Object.entries(SETTINGS).map(([k, [lo, hi]]) => el("tr", {},
  el("td", {}, el("label", { htmlFor: "set-" + k, textContent: k.replaceAll("_", " ") })),
  el("td", {}, el("input", { type: "number", min: lo, max: hi, step: 1, id: "set-" + k, placeholder: "unchanged" })),
  el("td", { className: "sub", textContent: `${lo} to ${hi}` }))));

$("#net-add").addEventListener("click", () => {
  const ssid = $("#ssid").value, pass = $("#wpass").value;
  if (!ssid || ssid.length > 32 || ssid.includes(",")) return msg("#card-msg", "A network name has 1 to 32 characters and no comma.", true);
  if (pass && (pass.length < 8 || pass.length > 64)) return msg("#card-msg", "A Wi-Fi password has 8 to 64 characters. Leave it empty for an open network.", true);
  networks.push([ssid, pass]); $("#ssid").value = ""; $("#wpass").value = ""; msg("#card-msg", ""); drawNetworks();
});

$("#card-pick").addEventListener("click", async () => {
  const picked = await open({ directory: true, multiple: false, title: "Choose the SD card" });
  if (typeof picked === "string") { cardFolder = picked; $("#card-path").textContent = picked; }
});

$("#card-write").addEventListener("click", async () => {
  if (!cardFolder) return msg("#card-msg", "Choose the SD card first.", true);
  const settings = {};
  for (const k of Object.keys(SETTINGS)) { const v = document.getElementById("set-" + k).value; if (v !== "") settings[k] = Number(v); }
  try {
    const done = await invoke("write_card", { folder: cardFolder, networks, settings, timezone: $("#tz").value || null, pair: $("#pair").checked });
    msg("#card-msg", done); networks.length = 0; drawNetworks();
  } catch (e) { msg("#card-msg", String(e), true); }
});

/* The board's C library wants a POSIX TZ string, which browsers do not give. Common zones are listed with their daylight
   saving rules; any other zone gets its current fixed offset. */
const POSIX_TZ = {
  "America/New_York": "EST5EDT,M3.2.0,M11.1.0", "America/Detroit": "EST5EDT,M3.2.0,M11.1.0", "America/Toronto": "EST5EDT,M3.2.0,M11.1.0",
  "America/Chicago": "CST6CDT,M3.2.0,M11.1.0", "America/Winnipeg": "CST6CDT,M3.2.0,M11.1.0", "America/Mexico_City": "CST6",
  "America/Denver": "MST7MDT,M3.2.0,M11.1.0", "America/Edmonton": "MST7MDT,M3.2.0,M11.1.0", "America/Phoenix": "MST7",
  "America/Los_Angeles": "PST8PDT,M3.2.0,M11.1.0", "America/Vancouver": "PST8PDT,M3.2.0,M11.1.0", "America/Anchorage": "AKST9AKDT,M3.2.0,M11.1.0",
  "Pacific/Honolulu": "HST10", "Europe/London": "GMT0BST,M3.5.0/1,M10.5.0", "Europe/Dublin": "IST-1GMT0,M10.5.0,M3.5.0/1",
  "Europe/Lisbon": "WET0WEST,M3.5.0/1,M10.5.0", "Europe/Berlin": "CET-1CEST,M3.5.0,M10.5.0/3", "Europe/Paris": "CET-1CEST,M3.5.0,M10.5.0/3",
  "Europe/Madrid": "CET-1CEST,M3.5.0,M10.5.0/3", "Europe/Rome": "CET-1CEST,M3.5.0,M10.5.0/3", "Europe/Amsterdam": "CET-1CEST,M3.5.0,M10.5.0/3",
  "Europe/Athens": "EET-2EEST,M3.5.0/3,M10.5.0/4", "Asia/Dhaka": "<+06>-6", "Asia/Kolkata": "IST-5:30", "Asia/Dubai": "<+04>-4",
  "Asia/Singapore": "<+08>-8", "Asia/Shanghai": "CST-8", "Asia/Tokyo": "JST-9", "Asia/Seoul": "KST-9",
  "Australia/Sydney": "AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia/Melbourne": "AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia/Brisbane": "AEST-10",
  "Pacific/Auckland": "NZST-12NZDT,M9.5.0,M4.1.0/3", "UTC": "UTC0", "Etc/UTC": "UTC0",
};
function guessTimezone() {
  try { const z = Intl.DateTimeFormat().resolvedOptions().timeZone; if (POSIX_TZ[z]) return POSIX_TZ[z]; } catch (_) { /* fall through */ }
  const east = -new Date().getTimezoneOffset(), a = Math.abs(east), hh = String(Math.floor(a / 60)).padStart(2, "0"), mm = a % 60;
  const name = `<${east >= 0 ? "+" : "-"}${hh}${mm ? String(mm).padStart(2, "0") : ""}>`;
  return `${name}${east >= 0 ? "-" : ""}${Math.floor(a / 60)}${mm ? ":" + String(mm).padStart(2, "0") : ""}`;
}
$("#tz").value = guessTimezone();

/* ---- Where the Overview reads drives from ---- */
async function refreshSources() {
  const sel = $("#source"), keep = sel.value || recall("source");
  const sources = await invoke("received_sources").catch(() => []);
  sel.replaceChildren(el("option", { value: "", textContent: "Card folder" }), ...sources.map((s) => el("option", { value: s.path, textContent: `Received over Wi-Fi: board ${s.board}` })));
  if (keep && [...sel.options].some((o) => o.value === keep)) sel.value = keep;
  sel.hidden = sources.length === 0;
}
$("#source").addEventListener("change", () => { const v = $("#source").value; remember("source", v); if (v) load(v); });
document.querySelector('.tab[data-page="board"]').addEventListener("click", refreshReceiver);
refreshReceiver(); refreshSources();
setInterval(() => { refreshReceiver(); refreshSources(); if (!$("#page-overview").hidden && $("#source").value && state.folder === $("#source").value) load(state.folder); }, 15000);
