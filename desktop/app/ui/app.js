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

/* Units: each drive carries the units the board used ("metric", "us", "uk"); totals follow the most recent drive. */
const MI = 1.609344, USGAL = 3.785411784, UKGAL = 4.54609;
const U = {
  dist: (km, u) => u === "metric" ? km : km / MI,
  distLabel: (u) => u === "metric" ? "km" : "mi",
  speedLabel: (u) => u === "metric" ? "km/h" : "mph",
  econ: (kpl, u) => kpl == null ? null : u === "us" ? kpl * USGAL / MI : u === "uk" ? kpl * UKGAL / MI : kpl,
  econLabel: (u) => u === "metric" ? "km/L" : u === "uk" ? "mpg (UK)" : "mpg",
  fuel: (l, u) => l == null ? null : u === "us" ? l / USGAL : u === "uk" ? l / UKGAL : l,
  fuelLabel: (u) => u === "metric" ? "L" : "gal",
  l100: (kpl) => kpl == null || kpl <= 0 ? null : 100 / kpl,
  mpg: (kpl, u) => kpl == null ? null : kpl * (u === "uk" ? UKGAL : USGAL) / MI,
  mpgLabel: (u) => u === "uk" ? "mpg (UK)" : "mpg (US)",
};
const SOURCE = { pid: "from the car", maf: "from airflow", model: "model estimate", phys: "airflow physics estimate", learned: "learned estimate" };
const ESTIMATE = new Set(["model", "phys", "learned"]);
const MODEL_NAME = { ev: "CarMind E.V", rev: "CarMind R.E.V" };

function renderOverview() {
  const s = state.sessions;
  $("#empty").hidden = s.length > 0;
  $("#cards").hidden = $("#bars").hidden = s.length === 0;
  if (!s.length) return;
  const u = s[s.length - 1].units || "metric";
  const km = s.reduce((a, x) => a + x.distance_km, 0);
  const min = s.reduce((a, x) => a + x.minutes, 0);
  const top = Math.max(...s.map((x) => x.max_speed_kph ?? 0));
  const withFuel = s.filter((x) => x.fuel_l != null && x.economy_kpl != null);
  const fuelKm = withFuel.reduce((a, x) => a + x.economy_kpl * x.fuel_l, 0), fuelL = withFuel.reduce((a, x) => a + x.fuel_l, 0);
  const avg = fuelL >= 0.02 ? U.econ(fuelKm / fuelL, u) : null;
  const cards = [["Number of drives", String(s.length)], [`${U.distLabel(u) === "km" ? "Km" : "Miles"} driven`, U.dist(km, u).toFixed(1)],
    ["Drive time (h)", (min / 60).toFixed(1)], [`Highest speed (${U.speedLabel(u)})`, U.dist(top, u).toFixed(0)],
    [`Average economy (${U.econLabel(u)})`, avg == null ? "n/a" : avg.toFixed(1)]];
  $("#cards").replaceChildren(...cards.map(([k, v]) => el("div", { className: "card" }, el("div", { className: "k", textContent: k }), el("div", { className: "v", textContent: v }))));
  const bars = (title, pick, digits) => {
    const max = Math.max(...s.map(pick), 1e-9);
    return el("div", { className: "bar-card" }, el("h2", { textContent: title }),
      ...s.slice().reverse().slice(0, 8).map((x) => {
        const fill = el("div", { className: "fill" }); fill.style.width = (100 * pick(x) / max).toFixed(0) + "%";
        return el("div", { className: "bar" }, el("span", { textContent: x.id }), el("div", { className: "track" }, fill), el("span", { className: "num", textContent: pick(x).toFixed(digits) }));
      }));
  };
  $("#bars").replaceChildren(bars(`${U.distLabel(u) === "km" ? "Kilometres" : "Miles"} per drive`, (x) => U.dist(x.distance_km, u), 1), bars("Minutes per drive", (x) => x.minutes, 1),
    bars(`Top speed per drive (${U.speedLabel(u)})`, (x) => U.dist(x.max_speed_kph ?? 0, u), 0),
    bars(`Economy per drive (${U.econLabel(u)})`, (x) => U.econ(x.economy_kpl, u) ?? 0, 1));
}

function renderDrives() {
  const body = $("#drives tbody");
  body.replaceChildren(...state.sessions.slice().reverse().map((x) => {
    const tr = el("tr", { tabIndex: 0 },
      el("td", { textContent: x.id }), el("td", { textContent: x.date }),
      el("td", { className: "n", textContent: x.rows.toLocaleString() }), el("td", { className: "n", textContent: fmt(x.minutes, 1) }),
      el("td", { className: "n", textContent: x.max_speed_kph == null ? "n/a" : `${U.dist(x.max_speed_kph, x.units).toFixed(0)} ${U.speedLabel(x.units)}` }),
      el("td", { className: "n", textContent: `${U.dist(x.distance_km, x.units).toFixed(1)} ${U.distLabel(x.units)}` }),
      el("td", { className: "n", textContent: x.fuel_l == null ? "n/a" : `${U.fuel(x.fuel_l, x.units).toFixed(2)} ${U.fuelLabel(x.units)}` }),
      el("td", { className: "n", title: SOURCE[x.fuel_source] || "", textContent: x.economy_kpl == null ? "n/a" : `${U.econ(x.economy_kpl, x.units).toFixed(1)} ${U.econLabel(x.units)}${ESTIMATE.has(x.fuel_source) ? " (est.)" : ""}` }));
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
  add("polyline", { points: pts.map(([x, y]) => `${px(x).toFixed(1)},${py(y).toFixed(1)}`).join(" "), fill: "none", stroke: getComputedStyle(document.documentElement).getPropertyValue("--accent").trim() || "#2f5fd0", "stroke-width": 1.8, "stroke-linejoin": "round" });
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
    const stats = series.stats.length ? el("div", { className: "table-wrap" }, el("table", {},
      el("thead", {}, el("tr", {}, ...["What the car reported", "Unit", "Samples", "Min", "Mean", "Max", "Last"].map((h, i) => el("th", { className: i > 1 ? "n" : "", textContent: h })))),
      el("tbody", {}, ...series.stats.map((c) => el("tr", {}, el("td", { textContent: c.name.replaceAll("_", " ") }), el("td", { textContent: c.unit }),
        el("td", { className: "n", textContent: c.count.toLocaleString() }),
        ...[c.min, c.mean, c.max, c.last].map((v) => el("td", { className: "n", textContent: Math.abs(v) >= 100 ? v.toFixed(0) : v.toFixed(2) }))))))) : null;
    const econ = info.economy_kpl == null ? "Economy: n/a (this car reports no fuel rate or airflow)"
      : `Economy: ${U.econ(info.economy_kpl, info.units).toFixed(1)} ${U.econLabel(info.units)}, fuel ${SOURCE[info.fuel_source] || "unknown"}`;
    box.replaceChildren(el("h2", { textContent: `${info.id}, ${info.date}` }), el("p", { className: "sub", textContent: econ }),
      ...(charts.length ? charts : [el("p", { className: "sub", textContent: "This drive has no chartable columns." })]),
      ...(stats ? [el("h2", { textContent: "Everything this car reported" }),
        el("p", { className: "sub", textContent: "Columns the car never filled are left out. Older cars report fewer." }), stats] : []),
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
  renderOverview(); renderDrives(); renderFuel(); $("#detail").replaceChildren();
}

$("#open-card").addEventListener("click", async () => {
  const picked = await open({ directory: true, multiple: false, title: "Choose the SD card's obd folder" });
  if (typeof picked === "string") load(picked);
});

const last = recall("folder");
if (last) load(last);

/* ---- Board page: the authenticated USB door (main/usb_door.h). The app does the work, the board stores what it gets. ---- */
const SETTINGS = { poll_interval_ms: [50, 5000], slow_group_every: [1, 50], dtc_interval_s: [10, 3600], backlight_pct: [5, 100], theme_mode: [0, 2], day_start_hour: [0, 23], day_end_hour: [0, 23] };
let current = null;

function msg(id, text, bad) { const m = $(id); m.textContent = text; m.classList.toggle("bad", !!bad); }
function busy(on) { for (const b of document.querySelectorAll("#page-board button")) b.disabled = on; }
function port() { return $("#port").value; }
function ago(s) {
  const d = Math.max(0, Math.round(Date.now() / 1000 - s));
  return d < 60 ? `${d} s ago` : d < 3600 ? `${Math.round(d / 60)} min ago` : `${Math.round(d / 3600)} h ago`;
}
async function step(text, fn) {
  busy(true); msg("#board-msg", text);
  try { return await fn(); } catch (e) { msg("#board-msg", String(e), true); return undefined; } finally { busy(false); }
}

async function refreshPorts() {
  try {
    const ports = await invoke("board_ports");
    const sel = $("#port"), keep = sel.value || recall("port");
    sel.replaceChildren(...ports.map((p) => el("option", { value: p.name, textContent: `${p.name} (${p.label}${p.likely_board ? ", looks like the board" : ""})` })));
    if (keep && ports.some((p) => p.name === keep)) sel.value = keep;
    if (!ports.length) msg("#board-msg", "No serial ports found. Plug the board in and press Refresh.");
  } catch (e) { msg("#board-msg", String(e), true); }
}

async function loadWifi() {
  const nets = await invoke("board_wifi_list", { port: port() });
  $("#wifi-list").replaceChildren(...(nets.length ? nets.map((n) => el("li", {}, el("span", { textContent: n }),
    Object.assign(el("button", { textContent: "Forget" }), { onclick: () => step(`Forgetting ${n}...`, async () => { await invoke("board_wifi_forget", { port: port(), ssid: n }); await loadWifi(); msg("#board-msg", `Forgot ${n}.`); }) }))) : [el("li", { textContent: "No networks saved yet." })]));
}

function showTools(on) { $("#board-tools").hidden = !on; $("#pair-box").hidden = on; }

async function connect() {
  const h = await step("Connecting...", () => invoke("board_connect", { port: port() }));
  if (!h) return;
  current = h; remember("port", port());
  if (h.authed) {
    msg("#board-msg", `Connected to board ${h.board}, firmware ${h.firmware}, paired with this app.`);
    showTools(true); await step("Reading the board's networks...", loadWifi);
    await step("Reading the board's model...", loadModel);
    await loadFills();
    msg("#board-msg", `Connected to board ${h.board}, firmware ${h.firmware}, paired with this app.`);
  } else {
    showTools(false); $("#pair-box").hidden = false;
    msg("#board-msg", `Board ${h.board}, firmware ${h.firmware}. ${h.paired ? "It is paired with another app or an older install." : "It is not paired yet."} Pair it below.`);
  }
}

/* ---- On-board model: CarMind R.E.V (the default on a new board) or CarMind E.V, switched over the cable and kept by the board ---- */
let boardModel = null;
function showModel(m) {
  boardModel = m;
  const pick = $("#model-pick");
  for (const o of pick.options) {
    const ok = m.available.includes(o.value);
    o.disabled = !ok;
    o.textContent = MODEL_NAME[o.value] + (ok ? "" : " (not in this firmware)");
  }
  pick.value = m.active;
  const speed = m.run_us ? `, ${(m.run_us / 1000).toFixed(1)} ms per run` : "";
  const learn = m.active === "rev" ? (m.learning ? ", learning on" : ", learning off") : "";
  msg("#model-text", `The board runs ${MODEL_NAME[m.active] || m.active}${m.loaded ? "" : " (it did not load)"}${speed}${learn}.`);
}
async function loadModel() { showModel(await invoke("board_model", { port: port() })); }
$("#model-switch").addEventListener("click", () => {
  const want = $("#model-pick").value;
  if (!boardModel) return msg("#model-text", "Connect first.", true);
  if (want === boardModel.active) return msg("#model-text", `${MODEL_NAME[want]} is already running.`);
  $("#model-confirm-text").textContent = want === "rev"
    ? "Switch to CarMind R.E.V? It learns this car's fuel use and engine size while you drive, and from your fill-ups. You can switch back to E.V at any time; what R.E.V learned is kept."
    : "Switch back to CarMind E.V? R.E.V stops learning, and what it learned is kept on the card for later.";
  $("#model-confirm").hidden = false;
});
$("#model-no").addEventListener("click", () => { $("#model-confirm").hidden = true; });
$("#model-yes").addEventListener("click", () => {
  const want = $("#model-pick").value;
  $("#model-confirm").hidden = true;
  step(`Switching to ${MODEL_NAME[want]}...`, async () => {
    showModel(await invoke("board_model_set", { port: port(), model: want }));
    msg("#board-msg", `The board now runs ${MODEL_NAME[want]}. It keeps this choice after a restart.`);
  });
});

$("#refresh-ports").addEventListener("click", refreshPorts);
$("#connect").addEventListener("click", connect);
$("#show-code").addEventListener("click", () => step("Asking the board for a code...", async () => { await invoke("board_request_code", { port: port() }); msg("#board-msg", "Look at the board's screen and type the code."); $("#code").focus(); }));
$("#pair").addEventListener("click", () => step("Pairing...", async () => {
  const done = await invoke("board_pair", { port: port(), code: $("#code").value.trim() });
  $("#code").value = ""; msg("#board-msg", done); await connect();
}));
$("#wifi-add").addEventListener("click", () => {
  const ssid = $("#ssid").value, pass = $("#wpass").value;
  if (!ssid || ssid.length > 32 || ssid.includes(",")) return msg("#board-msg", "A network name has 1 to 32 characters and no comma.", true);
  if (pass && (pass.length < 8 || pass.length > 64)) return msg("#board-msg", "A Wi-Fi password has 8 to 64 characters. Leave it empty for an open network.", true);
  step(`Adding ${ssid}...`, async () => { await invoke("board_setup", { port: port(), networks: [[ssid, pass]], settings: {}, timezone: null, timeHost: null }); $("#ssid").value = ""; $("#wpass").value = ""; await loadWifi(); msg("#board-msg", `Saved ${ssid} on the board.`); });
});

$("#settings tbody").replaceChildren(...Object.entries(SETTINGS).map(([k, [lo, hi]]) => el("tr", {},
  el("td", {}, el("label", { htmlFor: "set-" + k, textContent: k.replaceAll("_", " ") })),
  el("td", {}, el("input", { type: "number", min: lo, max: hi, step: 1, id: "set-" + k, placeholder: "unchanged" })),
  el("td", { className: "sub", textContent: `${lo} to ${hi}` }))));
$("#settings-save").addEventListener("click", () => {
  const settings = {};
  for (const k of Object.keys(SETTINGS)) { const v = document.getElementById("set-" + k).value; if (v !== "") settings[k] = Number(v); }
  step("Sending...", async () => msg("#board-msg", await invoke("board_setup", { port: port(), networks: [], settings, timezone: $("#tz").value || null, timeHost: $("#thost").value || null })));
});

window.__TAURI__.event.listen("import-progress", (e) => { const p = e.payload; $("#import-progress").textContent = `${p.file}: ${Math.round(100 * p.done / Math.max(p.total, 1))} %`; });
$("#import").addEventListener("click", () => step("Importing drives over USB...", async () => {
  const done = await invoke("board_import", { port: port(), boardId: current.board });
  $("#import-progress").textContent = ""; msg("#board-msg", done); await refreshSources();
}));

window.__TAURI__.event.listen("flash-progress", (e) => { const log = $("#flash-log"); log.hidden = false; log.textContent = (log.textContent + "\n" + e.payload).split("\n").slice(-8).join("\n"); });
async function refreshFirmware() {
  const f = await invoke("firmware_info").catch(() => ({ available: false, version: "" }));
  $("#fw-text").textContent = f.available ? `This app carries firmware ${f.version || "(version unknown)"}. Flashing keeps the board's Wi-Fi networks and pairing.` : "This build of the app has no firmware image to flash.";
  $("#flash").hidden = !f.available;
}
$("#flash").addEventListener("click", () => {
  if (!confirm("Flash the firmware that came with this app? Keep the cable in until it finishes.")) return;
  $("#flash-log").textContent = "";
  step("Flashing... this takes about 30 seconds.", async () => msg("#board-msg", await invoke("board_flash", { port: port() })));
});
$("#reboot").addEventListener("click", () => {
  step("Restarting the board...", async () => { await invoke("board_reboot", { port: port() }); showTools(false); $("#pair-box").hidden = true; msg("#board-msg", "The board is restarting. It keeps its networks, settings, pairing and what it learned. Connect again in a few seconds."); });
});

async function refreshReceiver() {
  try {
    const s = await invoke("receiver_status");
    if (s.error) msg("#rx-status", `Not receiving: ${s.error}`, true);
    else msg("#rx-status", s.running ? `Listening at ${s.address || "this computer"}, port ${s.port}. Drives are stored in ${s.folder}.` : "Starting...");
    $("#rx-boards").replaceChildren(...s.boards.map((b) => el("li", {}, el("span", { textContent: `Board ${b.id} (firmware ${b.firmware || "?"})` }), el("span", { className: "sub", textContent: `last heard ${ago(b.last_seen_s)}` }))));
  } catch (e) { msg("#rx-status", String(e), true); }
}

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
document.querySelector('.tab[data-page="board"]').addEventListener("click", () => { refreshReceiver(); refreshFirmware(); if (!$("#port").options.length) refreshPorts(); });
refreshReceiver(); refreshSources();
setInterval(() => { refreshReceiver(); refreshSources(); if (!$("#page-overview").hidden && $("#source").value && state.folder === $("#source").value) load(state.folder); }, 15000);

/* ---- Fuel page: per-drive and lifetime fuel, the fill-up log (main/rev_cal.h) and R.E.V's learning status ---- */
const fuel = { log: { rev: 0, entries: [] }, editing: null, status: null, board: null };

function fuelBoard() {
  if (current && current.board) return current.board;
  const m = ($("#source").value || "").match(/([0-9a-f]{12})[\\/]*$/);
  return m ? m[1] : null;
}
function sessionNumber(id) { return parseInt(String(id).replace(/^s/, ""), 10); }
function boardConnected() { return !!(current && current.authed); }
function userUnits() { const s = state.sessions; return s.length ? s[s.length - 1].units || "metric" : "metric"; }
function fuelMsg(text, bad) { msg("#fuel-msg", text, bad); }
function group(name) { return (fuel.status || []).find((g) => g.group === name)?.fields || null; }

function driveOptions(sel, keep) {
  const opts = state.sessions.slice().reverse().map((x) => el("option", { value: String(sessionNumber(x.id)), textContent: `${x.id}, ${x.date}` }));
  sel.replaceChildren(...opts);
  if (keep != null && [...sel.options].some((o) => o.value === String(keep))) sel.value = String(keep);
}

/* The default drive for a fill-up is the last one that started before the entered time. */
function driveBefore(when) {
  const t = Date.parse(when);
  const before = state.sessions.filter((x) => { const d = Date.parse(String(x.date).replace(" UTC", "Z").replace(" ", "T")); return Number.isFinite(d) && Number.isFinite(t) && d <= t; });
  const pick = before.length ? before[before.length - 1] : state.sessions[state.sessions.length - 1];
  return pick ? sessionNumber(pick.id) : 0;
}

function localNow() { const d = new Date(); d.setMinutes(d.getMinutes() - d.getTimezoneOffset()); return d.toISOString().slice(0, 16); }

function resetForm() {
  fuel.editing = null;
  $("#fill-title").textContent = "Add a fill-up";
  $("#fill-cancel").hidden = true;
  $("#fill-date").value = localNow();
  $("#fill-amount").value = ""; $("#fill-figure").value = "";
  $("#fill-full").checked = true;
  const u = userUnits();
  $("#fill-unit").value = u === "us" ? "usgal" : u === "uk" ? "ukgal" : "L";
  $("#fill-econ-unit").value = u === "us" ? "mpgus" : u === "uk" ? "mpguk" : "l100";
  driveOptions($("#fill-after"), driveBefore($("#fill-date").value));
  driveOptions($("#fill-from")); driveOptions($("#fill-to"));
  showKind();
}
function showKind() { const trip = $("#fill-kind").value === "trip"; $("#fill-pump-row").hidden = trip; $("#fill-trip-row").hidden = !trip; }
$("#fill-kind").addEventListener("change", showKind);
$("#fill-date").addEventListener("change", () => driveOptions($("#fill-after"), driveBefore($("#fill-date").value)));
$("#fill-cancel").addEventListener("click", resetForm);

function editEntry(e) {
  fuel.editing = e.id;
  $("#fill-title").textContent = "Edit a fill-up";
  $("#fill-cancel").hidden = false;
  $("#fill-kind").value = e.kind; showKind();
  $("#fill-date").value = e.date || "";
  if (e.kind === "pump") { $("#fill-amount").value = e.amount; $("#fill-unit").value = e.unit; $("#fill-full").checked = e.full; driveOptions($("#fill-after"), e.after_session); }
  else { $("#fill-figure").value = e.amount; $("#fill-econ-unit").value = e.unit; driveOptions($("#fill-from"), e.from_session); driveOptions($("#fill-to"), e.after_session); }
  $("#fill-title").scrollIntoView({ block: "nearest" });
}

async function saveEntries(entries, done) {
  const board = fuelBoard();
  if (!board) return fuelMsg("Connect the board on the Board page, or choose its received drives on the Overview, so the log is kept for that board.", true);
  try {
    fuel.log = await invoke("fillups_save", { board, entries });
    fuel.board = board;
    if (boardConnected()) {
      const answer = await invoke("board_rev_fillups", { port: port(), board });
      const c = (answer.match(/cal=([0-9.]+)/) || [])[1];
      fuelMsg(`${done} Sent to the board${c ? `: correction x${Number(c).toFixed(3)}` : ""}.`);
      await refreshRev(false);
    } else fuelMsg(`${done} Connect the board on the Board page to send it.`);
  } catch (e) { fuelMsg(String(e), true); }
  renderFills();
}

$("#fill-save").addEventListener("click", () => {
  const kind = $("#fill-kind").value;
  const ids = fuel.log.entries.map((e) => e.id);
  const id = fuel.editing ?? (ids.length ? Math.max(...ids) + 1 : 1);
  const e = { id, date: $("#fill-date").value, kind, after_session: 0, from_session: 0, amount: 0, unit: "", full: false, price: null };
  if (kind === "pump") {
    e.amount = Number($("#fill-amount").value); e.unit = $("#fill-unit").value; e.full = $("#fill-full").checked;
    e.after_session = Number($("#fill-after").value);
  } else {
    e.amount = Number($("#fill-figure").value); e.unit = $("#fill-econ-unit").value;
    e.from_session = Number($("#fill-from").value); e.after_session = Number($("#fill-to").value);
  }
  if (!(e.amount > 0)) return fuelMsg("Enter the amount first.", true);
  if (!e.after_session) return fuelMsg("There are no drives to attach it to yet.", true);
  const entries = fuel.log.entries.filter((x) => x.id !== id).concat([e]);
  saveEntries(entries, fuel.editing ? "Fill-up changed." : "Fill-up saved.").then(resetForm);
});

$("#fill-send").addEventListener("click", async () => {
  const board = fuelBoard();
  if (!boardConnected() || !board) return fuelMsg("Connect the board on the Board page first.", true);
  try { const a = await invoke("board_rev_fillups", { port: port(), board }); fuelMsg(`Sent. ${a.replace(/^OK REV-FILLUPS /, "")}`); await refreshRev(false); }
  catch (e) { fuelMsg(String(e), true); }
});

function volume(l, u) { return l == null ? "n/a" : `${U.fuel(l, u).toFixed(2)} ${U.fuelLabel(u)}`; }

function renderFills() {
  const u = userUnits();
  const iv = (fuel.status || []).filter((g) => g.group === "iv").map((g) => g.fields);
  const why = ["used", "under 5 L counted", "fuel data missing over 5 % of the time", "drives no longer on the board", "ratio outside 0.5 to 2"];
  const body = $("#fill-table tbody");
  const rows = fuel.log.entries.slice().reverse().map((e) => {
    const r = iv.find((x) => Number(x.id) === e.id);
    const what = e.kind === "pump" ? `${e.amount} ${e.unit === "L" ? "L" : e.unit === "usgal" ? "US gal" : "UK gal"}${e.full ? ", full" : ", partial"}`
      : `trip computer ${e.amount} ${$(`#fill-econ-unit option[value="${e.unit}"]`)?.textContent || e.unit}`;
    const result = r ? (Number(r.why) === 0 ? `ratio ${(Number(r.pumped) / Number(r.board)).toFixed(3)}` : `not used: ${why[Number(r.why)] || "?"}`)
      : e.kind === "pump" && e.full ? "start of a tank" : "";
    const del = el("button", { textContent: "Delete" });
    del.addEventListener("click", () => saveEntries(fuel.log.entries.filter((x) => x.id !== e.id), "Fill-up deleted."));
    const edit = el("button", { textContent: "Edit" });
    edit.addEventListener("click", () => editEntry(e));
    return el("tr", {}, el("td", { textContent: (e.date || "").replace("T", " ") }), el("td", { textContent: what }),
      el("td", { textContent: e.kind === "trip" ? `s${String(e.from_session).padStart(6, "0")} to s${String(e.after_session).padStart(6, "0")}` : `s${String(e.after_session).padStart(6, "0")}` }),
      el("td", { className: "n", textContent: r ? volume(Number(r.pumped), u) : "" }), el("td", { className: "n", textContent: r ? volume(Number(r.board), u) : "" }),
      el("td", { textContent: result }), el("td", {}, el("div", { className: "row" }, edit, del)));
  });
  body.replaceChildren(...(rows.length ? rows : [el("tr", {}, el("td", { colSpan: 7, textContent: "No fill-ups yet." }))]));
  $("#fill-board").textContent = fuelBoard() ? `Board ${fuelBoard()}, list revision ${fuel.log.rev}` : "No board chosen";
}

function renderFuel() {
  const s = state.sessions, u = userUnits();
  const withFuel = s.filter((x) => x.fuel_l != null && x.economy_kpl != null);
  const L = withFuel.reduce((a, x) => a + x.fuel_l, 0), km = withFuel.reduce((a, x) => a + x.economy_kpl * x.fuel_l, 0);
  const kpl = L >= 0.02 && km >= 0.5 ? km / L : null;
  const cal = group("cal");
  const cards = [["Fuel used, all drives", volume(L, u)], ["Average km/L", kpl == null ? "n/a" : kpl.toFixed(1)],
    ["Average L/100 km", kpl == null ? "n/a" : U.l100(kpl).toFixed(1)], [`Average ${U.mpgLabel(u)}`, kpl == null ? "n/a" : U.mpg(kpl, u).toFixed(1)],
    ["Fill-up correction", cal ? `x${Number(cal.c).toFixed(3)}` : "connect to read"]];
  $("#fuel-cards").replaceChildren(...cards.map(([k, v]) => el("div", { className: "card" }, el("div", { className: "k", textContent: k }), el("div", { className: "v", textContent: v }))));
  $("#fuel-drives tbody").replaceChildren(...s.slice().reverse().map((x) => {
    const k = x.economy_kpl;
    return el("tr", {}, el("td", { textContent: x.id }), el("td", { textContent: x.date }), el("td", { textContent: MODEL_NAME[x.ml_model] || "" }),
      el("td", { className: "n", textContent: `${U.dist(x.distance_km, x.units).toFixed(1)} ${U.distLabel(x.units)}` }),
      el("td", { className: "n", title: x.fuel_raw_l != null ? `before the fill-up correction: ${volume(x.fuel_raw_l, x.units)}` : "", textContent: volume(x.fuel_l, x.units) }),
      el("td", { className: "n", textContent: k == null ? "n/a" : k.toFixed(1) }), el("td", { className: "n", textContent: k == null ? "n/a" : U.l100(k).toFixed(1) }),
      el("td", { className: "n", textContent: k == null ? "n/a" : `${U.mpg(k, x.units).toFixed(1)}${x.units === "uk" ? " UK" : ""}` }),
      el("td", { textContent: SOURCE[x.fuel_source] || x.fuel_source || "" }));
  }));
  renderFills();
}

function renderRev() {
  const st = group("state"), f = group("fuel"), er = group("error"), ph = group("phys"), cal = group("cal"), life = group("life");
  const tq = group("torque"), tk = group("tank");
  const fills = (fuel.status || []).filter((g) => g.group === "tankfill").map((g) => g.fields);
  if (!st) { $("#rev-status").replaceChildren(el("li", { textContent: "Connect the board on the Board page, then refresh." })); return; }
  const n = (v, d) => Number(v).toFixed(d);
  const items = [`Model on the board: ${MODEL_NAME[st.model] || st.model}. Learning ${st.learning === "1" ? "on" : "off"}${st.safe === "1" ? " (safe mode)" : ""}. Saved to the card: ${st.saved === "1" ? "yes" : "not yet"}.`];
  if (f) items.push(`Fuel estimate: ${f.commits} updates kept, ${f.resets} resets to factory, ${f.learned} labelled moments learned.`);
  if (er && Number(er.n_learned) >= 20 && Number(er.factory) > 0) {
    const gain = 1 - Number(er.learned) / Number(er.factory);
    items.push(`Without airflow data the learned estimate is ${Math.abs(gain * 100).toFixed(0)} % ${gain >= 0 ? "better" : "worse"} than the factory one on this car (running error ${n(er.learned, 3)} against ${n(er.factory, 3)}).`);
  }
  if (er) items.push(`If airflow data dropped out now, the board would use: ${SOURCE[er.prefer] || er.prefer}.`);
  if (ph) items.push(`Airflow constant k = ${Number(ph.k).toExponential(3)} g/s per %-rpm${ph.ready === "1" ? `, learned from ${ph.rows} rows` : " (factory value until 20 rows)"}${ph.path === "calc" ? ", from calculated load (experimental)" : ""}. It implies about ${(12000 * Number(ph.k) / 1.184).toFixed(1)} L of engine displacement.`);
  if (cal) items.push(`Fill-up correction x${n(cal.c, 3)}: ${cal.used} tank${cal.used === "1" ? "" : "s"} used, ${cal.skipped} skipped, ${cal.fills} entr${cal.fills === "1" ? "y" : "ies"} on the board.`);
  if (tq) {
    const src = { pid: "the car's own torque reading", rate: "physics, from the fuel rate", maf: "physics, from the airflow", k: "physics, from the airflow constant" };
    items.push(tq.ok === "1"
      ? `Engine torque now: ${tq.idle === "1" ? "idle" : `${Number(tq.nm).toFixed(0)} N m`} (${src[tq.src] || tq.src}). Engine size used: ${Number(tq.vd_l).toFixed(1)} L.`
      : `Engine torque: no value yet (it needs a running engine and an airflow or fuel-rate reading). Engine size used: ${Number(tq.vd_l).toFixed(1)} L.`);
  }
  if (tk) {
    const c = cal ? Number(cal.c) : 1, u = userUnits();
    if (tk.ready === "1") items.push(`Fuel tank: about ${volume(Number(tk.cap_raw_l) * c, u)}, ${volume(Number(tk.left_raw_l) * c, u)} left${Number(tk.range_km) >= 0 ? `, range ${U.dist(Number(tk.range_km), u).toFixed(0)} ${U.distLabel(u)}` : ""}.`);
    else items.push(`Fuel tank: learning its size, ${Number(tk.band_l).toFixed(1)} of 10 L of driving counted (needs the fuel level between 10 % and 95 %).`);
  }
  for (const s of fills) items.push(`The board saw a fill-up (fuel level ${Number(s.before).toFixed(0)} % to ${Number(s.after).toFixed(0)} %${s.ready === "1" ? `, about ${volume(Number(s.raw_l) * (cal ? Number(cal.c) : 1), userUnits())}` : ""}). Enter the litres from the pump receipt below; the board does not use its own guess for the correction.`);
  if (life && Number(life.litres) > 0.02) {
    const k = Number(life.km) / Number(life.litres), u = userUnits();
    items.push(`Counted by the board since it started: ${volume(Number(life.litres), u)} over ${U.dist(Number(life.km), u).toFixed(0)} ${U.distLabel(u)}, ${k.toFixed(1)} km/L, ${U.l100(k).toFixed(1)} L/100 km, ${U.mpg(k, u).toFixed(1)} ${U.mpgLabel(u)}.`);
  }
  $("#rev-status").replaceChildren(...items.map((t) => el("li", { textContent: t })));
}

async function refreshRev(say) {
  if (!boardConnected()) { if (say) fuelMsg("Connect the board on the Board page first.", true); return; }
  try {
    fuel.status = await invoke("board_rev_status", { port: port() });
    if (say) fuelMsg("Read from the board.");
  } catch (e) {
    fuel.status = null;
    if (say) fuelMsg(String(e).includes("unknown command") ? "This board's firmware has no fuel learning yet. Flash the firmware on the Board page." : String(e), true);
  }
  renderRev(); renderFuel();
}
$("#rev-refresh").addEventListener("click", () => refreshRev(true));

$("#rev-reset").addEventListener("click", () => {
  if (!boardConnected()) return fuelMsg("Connect the board on the Board page first.", true);
  const what = $("#rev-reset-what");
  $("#rev-confirm-text").textContent = `Reset the ${what.options[what.selectedIndex].textContent} to its factory value? What the board learned for it is lost.`;
  $("#rev-confirm").hidden = false;
});
$("#rev-reset-no").addEventListener("click", () => { $("#rev-confirm").hidden = true; });
$("#rev-reset-yes").addEventListener("click", async () => {
  $("#rev-confirm").hidden = true;
  try { await invoke("board_rev_reset", { port: port(), what: $("#rev-reset-what").value }); fuelMsg("Reset done."); await refreshRev(false); }
  catch (e) { fuelMsg(String(e), true); }
});

async function loadFills() {
  const board = fuelBoard();
  if (!board) { fuel.log = { rev: 0, entries: [] }; renderFills(); return; }
  try { fuel.log = await invoke("fillups_load", { board }); fuel.board = board; } catch (e) { fuelMsg(String(e), true); }
  if (boardConnected()) await refreshRev(false); else renderFills();
}
document.querySelector('.tab[data-page="fuel"]').addEventListener("click", () => { if (!fuel.editing) resetForm(); loadFills(); renderFuel(); });
resetForm();
