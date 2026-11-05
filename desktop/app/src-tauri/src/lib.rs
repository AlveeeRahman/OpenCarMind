//! Reads OpenCarMind session files from a folder and returns summaries and chart data to the window.
//! The arithmetic follows tools/ocm_report.py so the desktop app and the report tool agree.

mod board;
mod receiver;

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct SessionInfo {
    pub id: String,
    pub date: String,
    pub rows: usize,
    pub minutes: f64,
    pub row_interval_s: Option<f64>,
    pub max_speed_kph: Option<f64>,
    pub distance_km: f64,
    pub has_model_file: bool,
    /// Fuel used, from the board's trip columns (or its per-row fuel rate). None when the car gave nothing to work it out.
    pub fuel_l: Option<f64>,
    /// km per litre over the drive; None until 0.5 km and 0.02 L are counted.
    pub economy_kpl: Option<f64>,
    /// Where the fuel figure came from: "pid", "maf", "model", or None.
    pub fuel_source: Option<String>,
    /// How the board showed this drive: "metric", "us" or "uk" (from the session JSON; "metric" when absent).
    pub units: String,
    pub odometer_km: Option<f64>,
    /// Fuel before the fill-up calibration (firmware 2.5 and later), None for older logs.
    pub fuel_raw_l: Option<f64>,
    /// Which on-board model ran the drive: "ev" or "rev" (session JSON), None for older logs.
    pub ml_model: Option<String>,
}

#[derive(Serialize)]
pub struct ChannelStat {
    pub name: String,
    pub unit: String,
    pub count: usize,
    pub min: f64,
    pub mean: f64,
    pub max: f64,
    pub last: f64,
}

#[derive(Serialize)]
pub struct Channel {
    pub name: String,
    pub unit: String,
    pub values: Vec<Option<f64>>,
}

#[derive(Serialize)]
pub struct Series {
    pub minutes: Vec<f64>,
    pub channels: Vec<Channel>,
    /// Every numeric column this car reported in this drive, with its range. Columns the car never filled are left out.
    pub stats: Vec<ChannelStat>,
}

const CHANNELS: [(&str, &str); 6] = [
    ("speed_kph", "km/h"),
    ("rpm", "rpm"),
    ("fuel_lph_calc", "L/h"),
    ("coolant_c", "°C"),
    ("engine_load_pct", "%"),
    ("throttle_pct", "%"),
];
/// Columns that are bookkeeping, not car data.
const NOT_DATA: [&str; 4] = ["seq", "uptime_ms", "epoch_ms", "window_seq"];
const KNOWN_UNITS: [(&str, &str); 74] = [
    ("engine_load_pct", "%"),
    ("coolant_c", "°C"),
    ("stft_b1_pct", "%"),
    ("ltft_b1_pct", "%"),
    ("stft_b2_pct", "%"),
    ("ltft_b2_pct", "%"),
    ("fuel_pressure_kpa", "kPa"),
    ("map_kpa", "kPa"),
    ("rpm", "rpm"),
    ("speed_kph", "km/h"),
    ("timing_adv_deg", "°"),
    ("intake_temp_c", "°C"),
    ("maf_gps", "g/s"),
    ("throttle_pct", "%"),
    ("o2_s1_v", "V"),
    ("o2_s2_v", "V"),
    ("o2_s3_v", "V"),
    ("o2_s4_v", "V"),
    ("o2_s5_v", "V"),
    ("o2_s6_v", "V"),
    ("o2_s7_v", "V"),
    ("o2_s8_v", "V"),
    ("run_time_s", "s"),
    ("dist_mil_km", "km"),
    ("fuel_rail_rel_kpa", "kPa"),
    ("fuel_rail_kpa", "kPa"),
    ("lambda_s1", ""),
    ("lambda_s2", ""),
    ("lambda_s3", ""),
    ("lambda_s4", ""),
    ("egr_commanded_pct", "%"),
    ("egr_error_pct", "%"),
    ("evap_purge_pct", "%"),
    ("fuel_level_pct", "%"),
    ("warmups_since_clear", ""),
    ("dist_since_clear_km", "km"),
    ("evap_vapor_pa", "Pa"),
    ("baro_kpa", "kPa"),
    ("cat_temp_b1s1_c", "°C"),
    ("cat_temp_b2s1_c", "°C"),
    ("cat_temp_b1s2_c", "°C"),
    ("cat_temp_b2s2_c", "°C"),
    ("module_voltage_v", "V"),
    ("abs_load_pct", "%"),
    ("equiv_ratio_cmd", ""),
    ("rel_throttle_pct", "%"),
    ("ambient_c", "°C"),
    ("abs_throttle_b_pct", "%"),
    ("abs_throttle_c_pct", "%"),
    ("pedal_d_pct", "%"),
    ("pedal_e_pct", "%"),
    ("throttle_actuator_pct", "%"),
    ("time_mil_min", "min"),
    ("time_since_clear_min", "min"),
    ("ethanol_pct", "%"),
    ("evap_abs_kpa", "kPa"),
    ("fuel_rail_abs_kpa", "kPa"),
    ("rel_pedal_pct", "%"),
    ("hybrid_batt_pct", "%"),
    ("oil_temp_c", "°C"),
    ("fuel_inj_timing_deg", "°"),
    ("fuel_rate_lph", "L/h"),
    ("driver_torque_pct", "%"),
    ("actual_torque_pct", "%"),
    ("ref_torque_nm", "Nm"),
    ("odometer_km", "km"),
    ("adapter_voltage_v", "V"),
    ("dtc_count", ""),
    ("fuel_lph_calc", "L/h"),
    ("trip_km", "km"),
    ("trip_fuel_l", "L"),
    ("fuel_lph_raw", "L/h"),
    ("fuel_cal", ""),
    ("trip_fuel_raw_l", "L"),
    ("torque_nm", "N m"),
    ("tank_left_l", "L"),
    ("tank_range_km", "km"),
];

fn unit_of(name: &str) -> &'static str {
    KNOWN_UNITS.iter().find(|(n, _)| *n == name).map_or("", |(_, u)| u)
}
const MAX_POINTS: usize = 800;

fn number(s: &str) -> Option<f64> {
    s.trim().parse::<f64>().ok().filter(|v| v.is_finite())
}

/// Seconds since 1970 to "YYYY-MM-DD HH:MM UTC" (civil-from-days, no time zone database needed).
pub fn format_utc(epoch_s: i64) -> String {
    let days = epoch_s.div_euclid(86_400);
    let secs = epoch_s.rem_euclid(86_400);
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + if m <= 2 { 1 } else { 0 };
    format!("{y:04}-{m:02}-{d:02} {:02}:{:02} UTC", secs / 3600, (secs % 3600) / 60)
}

/// The card's obd folder, its sessions folder, or a folder that holds sNNNNNN.csv directly.
fn sessions_dir(folder: &Path) -> PathBuf {
    for candidate in [folder.join("obd").join("sessions"), folder.join("sessions")] {
        if candidate.is_dir() {
            return candidate;
        }
    }
    folder.to_path_buf()
}

fn is_session_file(name: &str) -> bool {
    let Some(stem) = name.strip_suffix(".csv") else { return false };
    let Some(digits) = stem.strip_prefix('s') else { return false };
    !digits.is_empty() && digits.bytes().all(|b| b.is_ascii_digit())
}

fn session_files(folder: &Path) -> Vec<PathBuf> {
    let dir = sessions_dir(folder);
    let mut files: Vec<PathBuf> = match std::fs::read_dir(&dir) {
        Ok(entries) => entries
            .filter_map(|e| e.ok())
            .map(|e| e.path())
            .filter(|p| p.file_name().and_then(|n| n.to_str()).map_or(false, is_session_file))
            .collect(),
        Err(_) => Vec::new(),
    };
    files.sort();
    files
}

struct Table {
    headers: Vec<String>,
    rows: Vec<Vec<String>>,
}

fn read_table(path: &Path) -> Result<Table, String> {
    let mut reader = csv::ReaderBuilder::new()
        .flexible(true)
        .from_path(path)
        .map_err(|e| format!("{}: {e}", path.display()))?;
    let headers = reader
        .headers()
        .map_err(|e| format!("{}: {e}", path.display()))?
        .iter()
        .map(|h| h.to_string())
        .collect();
    let rows = reader
        .records()
        .filter_map(|r| r.ok())
        .map(|r| r.iter().map(|c| c.to_string()).collect())
        .collect();
    Ok(Table { headers, rows })
}

impl Table {
    fn column(&self, name: &str) -> Vec<Option<f64>> {
        match self.headers.iter().position(|h| h == name) {
            Some(i) => self.rows.iter().map(|r| r.get(i).and_then(|c| number(c))).collect(),
            None => vec![None; self.rows.len()],
        }
    }
}

fn median(mut v: Vec<f64>) -> Option<f64> {
    if v.is_empty() {
        return None;
    }
    v.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    let n = v.len();
    Some(if n % 2 == 1 { v[n / 2] } else { (v[n / 2 - 1] + v[n / 2]) / 2.0 })
}

fn round1(v: f64) -> f64 {
    (v * 10.0).round() / 10.0
}

fn summarise(id: &str, table: &Table, has_model_file: bool) -> Option<SessionInfo> {
    if table.rows.len() < 2 {
        return None;
    }
    let uptime = table.column("uptime_ms");
    let speed = table.column("speed_kph");
    let first = uptime.iter().flatten().next().copied()?;
    let last = uptime.iter().flatten().last().copied()?;
    let mut gaps = Vec::new();
    let mut km = 0.0;
    for i in 0..uptime.len() - 1 {
        if let (Some(a), Some(b)) = (uptime[i], uptime[i + 1]) {
            let gap = (b - a) / 1000.0;
            gaps.push(gap);
            km += speed[i].unwrap_or(0.0) * gap / 3600.0;
        }
    }
    let epoch_ms = table.column("epoch_ms").first().copied().flatten().unwrap_or(0.0);
    let date = if epoch_ms > 0.0 { format_utc((epoch_ms / 1000.0) as i64) } else { "clock not set".to_string() };
    let max_speed = speed.iter().flatten().copied().fold(None, |m: Option<f64>, v| Some(m.map_or(v, |x| x.max(v))));
    // Fuel: the board's running trip totals when present (firmware 2.3.1 and later), else integrate its fuel rate.
    let fuel_source = {
        let i = table.headers.iter().position(|h| h == "fuel_src");
        let mut counts: Vec<(String, usize)> = Vec::new();
        if let Some(i) = i {
            for r in &table.rows {
                if let Some(v) = r.get(i).filter(|v| !v.is_empty()) {
                    match counts.iter().position(|(k, _)| k == v) {
                        Some(j) => counts[j].1 += 1,
                        None => counts.push((v.clone(), 1)),
                    }
                }
            }
        }
        counts.into_iter().max_by_key(|c| c.1).map(|c| c.0)
    };
    let last_of = |name: &str| table.column(name).into_iter().flatten().last();
    let (mut fuel_l, mut fuel_km) = (last_of("trip_fuel_l"), last_of("trip_km"));
    // The board writes trip totals on every row, as 0 when it never had a fuel rate. A log with a source column but no
    // source in any row had no fuel data at all: unknown, not 0 L.
    if table.headers.iter().any(|h| h == "fuel_src") && fuel_source.is_none() {
        fuel_l = None;
        fuel_km = None;
    }
    if fuel_l.is_none() {
        let rate = table.column("fuel_lph_calc");
        let (mut l, mut k, mut any) = (0.0, 0.0, false);
        for i in 0..uptime.len().saturating_sub(1) {
            if let (Some(a), Some(b), Some(r)) = (uptime[i], uptime[i + 1], rate[i]) {
                let dt = (b - a) / 1000.0;
                if dt > 0.0 && dt <= 10.0 {
                    l += r * dt / 3600.0;
                    k += speed[i].unwrap_or(0.0) * dt / 3600.0;
                    any = true;
                }
            }
        }
        if any { fuel_l = Some(l); fuel_km = Some(k); }
    }
    let economy_kpl = match (fuel_km, fuel_l) {
        (Some(k), Some(l)) if k >= 0.5 && l >= 0.02 => Some(((k / l) * 10.0).round() / 10.0),
        _ => None,
    };
    Some(SessionInfo {
        id: id.to_string(),
        date,
        rows: table.rows.len(),
        minutes: round1((last - first) / 60_000.0),
        row_interval_s: median(gaps).map(|g| (g * 100.0).round() / 100.0),
        max_speed_kph: max_speed,
        distance_km: round1(km),
        has_model_file,
        fuel_l: fuel_l.map(|l| (l * 100.0).round() / 100.0),
        economy_kpl,
        fuel_source,
        units: "metric".to_string(),
        odometer_km: last_of("odometer_km"),
        fuel_raw_l: last_of("trip_fuel_raw_l").filter(|_| fuel_l.is_some()).map(|l| (l * 100.0).round() / 100.0),
        ml_model: None,
    })
}

struct AppState {
    rx: std::sync::Arc<receiver::Receiver>,
}

#[derive(Serialize)]
pub struct Source {
    pub board: String,
    pub path: String,
}

#[tauri::command]
fn receiver_status(state: tauri::State<AppState>) -> receiver::Status {
    state.rx.status()
}

/// Folders of drives received over Wi-Fi, one per board.
#[tauri::command]
fn received_sources(state: tauri::State<AppState>) -> Vec<Source> {
    let mut out: Vec<Source> = std::fs::read_dir(&state.rx.dir)
        .map(|it| {
            it.flatten()
                .filter(|e| e.path().join("sessions").is_dir())
                .map(|e| Source { board: e.file_name().to_string_lossy().to_string(), path: e.path().display().to_string() })
                .collect()
        })
        .unwrap_or_default();
    out.sort_by(|a, b| a.board.cmp(&b.board));
    out
}

const SETTINGS: [(&str, f64, f64); 7] = [
    ("poll_interval_ms", 50.0, 5000.0),
    ("slow_group_every", 1.0, 50.0),
    ("dtc_interval_s", 10.0, 3600.0),
    ("backlight_pct", 5.0, 100.0),
    ("theme_mode", 0.0, 2.0),
    ("day_start_hour", 0.0, 23.0),
    ("day_end_hour", 0.0, 23.0),
];

/// Validates a Wi-Fi entry the way the board does (main/harden.c), plus no comma in the SSID because wifi.csv splits
/// each line at the first comma.
pub fn wifi_entry_ok(ssid: &str, password: &str) -> Result<(), String> {
    let bad = |s: &str| s.chars().any(|c| c.is_control());
    if ssid.is_empty() || ssid.len() > 32 || bad(ssid) || ssid.contains(',') || ssid.starts_with('#') || ssid.starts_with('-') {
        return Err(format!("network name '{ssid}' needs 1 to 32 characters, no comma, and cannot start with # or -"));
    }
    if !password.is_empty() && (password.len() < 8 || password.len() > 64) || bad(password) {
        return Err(format!("the password for '{ssid}' needs 8 to 64 characters, or leave it empty for an open network"));
    }
    Ok(())
}

/// Same rule as the board (main/harden.c): a POSIX TZ string of 3 to 47 characters from letters, digits and <>+-,.:/
pub fn timezone_ok(tz: &str) -> bool {
    (3..=47).contains(&tz.len()) && tz.chars().all(|c| c.is_ascii_alphanumeric() || "<>+-,.:/".contains(c))
}

/// The board's rule for the one time server it may contact (main/harden.c harden_hostname_ok): a DNS name, 4 to 63
/// characters, letters, digits and hyphens in dot-separated labels, at least one dot and one letter, no IP, port or scheme.
pub fn hostname_ok(h: &str) -> bool {
    let b = h.as_bytes();
    if !(4..=63).contains(&b.len()) || !h.contains('.') || !b.iter().any(|c| c.is_ascii_alphabetic()) {
        return false;
    }
    h.split('.').all(|l| !l.is_empty() && !l.starts_with('-') && !l.ends_with('-') && l.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-'))
}

/// Checks the settings, time zone and time server the way the board does and turns them into a config.json object.
/// time_host "default" sends "", which puts the board back on its built-in time server.
fn config_object(
    settings: &std::collections::HashMap<String, f64>,
    timezone: Option<String>,
    time_host: Option<String>,
) -> Result<Option<serde_json::Value>, String> {
    let mut map = serde_json::Map::new();
    if let Some(h) = time_host.map(|t| t.trim().to_ascii_lowercase()).filter(|t| !t.is_empty()) {
        if h == "default" {
            map.insert("time_host".into(), serde_json::json!(""));
        } else if hostname_ok(&h) {
            map.insert("time_host".into(), serde_json::json!(h));
        } else {
            return Err(format!("time server '{h}' is not a host name such as www.google.com (no https://, port or IP address)"));
        }
    }
    if let Some(tz) = timezone.map(|t| t.trim().to_string()).filter(|t| !t.is_empty()) {
        if !timezone_ok(&tz) {
            return Err(format!("time zone '{tz}' is not a POSIX time zone such as CST6CDT,M3.2.0,M11.1.0"));
        }
        map.insert("timezone".into(), serde_json::json!(tz));
    }
    for (key, value) in settings {
        let (_, lo, hi) = SETTINGS.iter().find(|(k, _, _)| *k == key.as_str()).ok_or_else(|| format!("unknown setting {key}"))?;
        if !(lo..=hi).contains(&value) || value.fract() != 0.0 {
            return Err(format!("{key} must be a whole number from {lo} to {hi}"));
        }
        map.insert(key.clone(), serde_json::json!(*value as i64));
    }
    Ok((!map.is_empty()).then_some(serde_json::Value::Object(map)))
}

/* ---- Fill-ups (main/rev_cal.h). The app keeps the user's log and sends all of it to the board, which works out one
   correction factor for its fuel figures. The board takes litres and L/100 km; the app converts the user's units. ---- */

const L_PER_US_GAL: f64 = 3.785411784;
const L_PER_UK_GAL: f64 = 4.54609;
const KM_PER_MILE: f64 = 1.609344;
pub const FILLUPS_MAX: usize = 16;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
pub struct FillEntry {
    pub id: u32,
    /// When it happened, as the user entered it (ISO 8601 local time); shown only.
    #[serde(default)]
    pub date: String,
    /// "pump": litres pumped; "trip": the car's trip-computer average over a range of drives.
    pub kind: String,
    /// Pump: the last drive before the fill. Trip: the last drive of the range.
    pub after_session: u32,
    /// Trip: the first drive of the range.
    #[serde(default)]
    pub from_session: u32,
    /// Pump: volume in `unit` ("L", "usgal", "ukgal"). Trip: the figure in `unit` ("l100", "kpl", "mpgus", "mpguk").
    pub amount: f64,
    pub unit: String,
    /// Pump: filled to the automatic click-off.
    #[serde(default)]
    pub full: bool,
    #[serde(default)]
    pub price: Option<f64>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Default)]
pub struct FillLog {
    pub rev: u32,
    pub entries: Vec<FillEntry>,
}

pub fn to_litres(amount: f64, unit: &str) -> Result<f64, String> {
    match unit {
        "L" => Ok(amount),
        "usgal" => Ok(amount * L_PER_US_GAL),
        "ukgal" => Ok(amount * L_PER_UK_GAL),
        _ => Err(format!("unknown volume unit {unit}")),
    }
}

/// L/100 km from km/L or mpg: L/100 km = 100 / (km/L), km/L = mpg x km per mile / litres per gallon.
pub fn to_l_per_100km(value: f64, unit: &str) -> Result<f64, String> {
    if !(value > 0.0) {
        return Err("a fuel economy figure must be above 0".into());
    }
    match unit {
        "l100" => Ok(value),
        "kpl" => Ok(100.0 / value),
        "mpgus" => Ok(100.0 / (value * KM_PER_MILE / L_PER_US_GAL)),
        "mpguk" => Ok(100.0 / (value * KM_PER_MILE / L_PER_UK_GAL)),
        _ => Err(format!("unknown economy unit {unit}")),
    }
}

/// The list the board takes (main/usb_door.c REV-FILLUPS), with the board's own plausibility rules checked first.
pub fn board_fill_list(log: &FillLog) -> Result<serde_json::Value, String> {
    if log.entries.len() > FILLUPS_MAX {
        return Err(format!("the board keeps the last {FILLUPS_MAX} fill-ups; delete older ones first"));
    }
    let mut out = Vec::new();
    for e in &log.entries {
        match e.kind.as_str() {
            "pump" => {
                let l = to_litres(e.amount, &e.unit)?;
                if !(l > 0.0 && l <= 300.0) {
                    return Err(format!("{:.1} L is not a plausible fill-up", l));
                }
                out.push(serde_json::json!({"id": e.id, "after": e.after_session, "litres": (l * 1000.0).round() / 1000.0, "full": e.full}));
            }
            "trip" => {
                let l100 = to_l_per_100km(e.amount, &e.unit)?;
                if !(1.0..=60.0).contains(&l100) {
                    return Err(format!("{:.1} L/100 km is not a plausible average", l100));
                }
                if e.from_session > e.after_session {
                    return Err("the first drive of the range comes after the last one".into());
                }
                out.push(serde_json::json!({"id": e.id, "kind": "trip", "from": e.from_session, "after": e.after_session, "l100": (l100 * 1000.0).round() / 1000.0}));
            }
            k => return Err(format!("unknown fill-up kind {k}")),
        }
    }
    Ok(serde_json::json!({"rev": log.rev, "entries": out}))
}

fn fillups_path(state: &AppState, board: &str) -> Result<PathBuf, String> {
    if !receiver::valid_board(board) {
        return Err("unknown board".into());
    }
    Ok(state.rx.dir.join(board).join("fillups.json"))
}

fn read_fill_log(path: &Path) -> FillLog {
    std::fs::read_to_string(path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
}

#[tauri::command]
fn fillups_load(state: tauri::State<AppState>, board: String) -> Result<FillLog, String> {
    Ok(read_fill_log(&fillups_path(&state, &board)?))
}

/// Saves the edited list (checked as the board would check it) with the next revision number.
#[tauri::command]
fn fillups_save(state: tauri::State<AppState>, board: String, entries: Vec<FillEntry>) -> Result<FillLog, String> {
    let path = fillups_path(&state, &board)?;
    let mut log = FillLog { rev: read_fill_log(&path).rev + 1, entries };
    log.entries.sort_by_key(|e| (e.after_session, e.id));
    board_fill_list(&log)?;
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    }
    let tmp = path.with_extension("json.tmp");
    let text = serde_json::to_string_pretty(&log).map_err(|e| e.to_string())?;
    std::fs::write(&tmp, text).and_then(|_| std::fs::rename(&tmp, &path)).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(log)
}

#[tauri::command]
async fn board_rev_fillups(state: tauri::State<'_, AppState>, port: String, board: String) -> Result<String, String> {
    let list = board_fill_list(&read_fill_log(&fillups_path(&state, &board)?))?;
    blocking(move || board::rev_fillups(&port, &list)).await
}

#[tauri::command]
async fn board_model(port: String) -> Result<board::ModelInfo, String> {
    blocking(move || board::model_get(&port)).await
}

#[tauri::command]
async fn board_model_set(port: String, model: String) -> Result<board::ModelInfo, String> {
    blocking(move || board::model_set(&port, &model)).await
}

#[tauri::command]
async fn board_rev_status(port: String) -> Result<Vec<board::StatusGroup>, String> {
    blocking(move || board::rev_status(&port)).await
}

#[tauri::command]
async fn board_rev_reset(port: String, what: String) -> Result<(), String> {
    blocking(move || board::rev_reset(&port, &what)).await
}

async fn blocking<T: Send + 'static>(f: impl FnOnce() -> Result<T, String> + Send + 'static) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(f).await.map_err(|e| format!("background task failed: {e}"))?
}

#[tauri::command]
async fn board_ports() -> Result<Vec<board::PortInfo>, String> {
    blocking(board::list_ports).await
}

#[tauri::command]
async fn board_connect(state: tauri::State<'_, AppState>, port: String) -> Result<board::Hello, String> {
    let token = state.rx.token.clone();
    blocking(move || board::connect(&port, &token)).await
}

#[tauri::command]
async fn board_request_code(port: String) -> Result<(), String> {
    blocking(move || board::request_code(&port)).await
}

#[tauri::command]
async fn board_pair(state: tauri::State<'_, AppState>, port: String, code: String) -> Result<String, String> {
    // Pairing is what lets the board accept this app's USB commands. The address is stored for the Wi-Fi upload, which
    // is off in 2.5.0, so a computer without a network address pairs with the loopback address.
    let host = receiver::local_address().unwrap_or_else(|| "127.0.0.1".to_string());
    if state.rx.token.is_empty() {
        return Err("the receiver could not start, so there is nothing to pair with".into());
    }
    let (token, cert) = (state.rx.token.clone(), state.rx.cert_pem.clone());
    let h = host.clone();
    blocking(move || board::pair(&port, &code, &h, receiver::PORT, &token, &cert)).await?;
    if receiver::WIFI_UPLOAD {
        Ok(format!("Paired. The board will send its drives to {host}:{} when it is parked on Wi-Fi.", receiver::PORT))
    } else {
        Ok("Paired. The board now accepts this app's commands over the cable. Import drives over USB: the Wi-Fi upload is being reworked.".into())
    }
}

#[tauri::command]
async fn board_wifi_list(port: String) -> Result<Vec<String>, String> {
    blocking(move || board::wifi_list(&port)).await
}

#[tauri::command]
async fn board_wifi_forget(port: String, ssid: String) -> Result<(), String> {
    blocking(move || board::wifi_forget(&port, &ssid)).await
}

#[tauri::command]
async fn board_setup(
    port: String,
    networks: Vec<(String, String)>,
    settings: std::collections::HashMap<String, f64>,
    timezone: Option<String>,
    time_host: Option<String>,
) -> Result<String, String> {
    for (ssid, password) in &networks {
        wifi_entry_ok(ssid, password)?;
    }
    let config = config_object(&settings, timezone, time_host)?;
    blocking(move || board::setup(&port, &networks, config.as_ref())).await
}

#[derive(Clone, Serialize)]
struct Progress {
    file: String,
    done: u64,
    total: u64,
}

#[tauri::command]
async fn board_import(app: tauri::AppHandle, state: tauri::State<'_, AppState>, port: String, board_id: String) -> Result<String, String> {
    use tauri::Emitter;
    if !receiver::valid_board(&board_id) {
        return Err("unknown board".into());
    }
    let dest = state.rx.dir.join(&board_id).join("sessions");
    blocking(move || {
        board::import(&port, &dest, |file, done, total| {
            let _ = app.emit("import-progress", Progress { file: file.to_string(), done, total });
        })
    })
    .await
}

#[derive(Serialize)]
struct FirmwareInfo {
    available: bool,
    version: String,
}

fn firmware_path(app: &tauri::AppHandle) -> Option<PathBuf> {
    use tauri::Manager;
    let p = app.path().resolve("resources/opencarmind-firmware.bin", tauri::path::BaseDirectory::Resource).ok()?;
    p.is_file().then_some(p)
}

#[tauri::command]
fn firmware_info(app: tauri::AppHandle) -> FirmwareInfo {
    use tauri::Manager;
    let version = app
        .path()
        .resolve("resources/firmware-version.txt", tauri::path::BaseDirectory::Resource)
        .ok()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .map(|v| v.trim().to_string())
        .unwrap_or_default();
    FirmwareInfo { available: firmware_path(&app).is_some(), version }
}

/// Flashes the firmware that ships with this app, using the bundled esptool. The settings in the board's flash
/// (Wi-Fi networks, pairing) are kept, because the image covers the bootloader, partition table and app only.
#[tauri::command]
async fn board_flash(app: tauri::AppHandle, port: String) -> Result<String, String> {
    use tauri::Emitter;
    use tauri_plugin_shell::process::CommandEvent;
    use tauri_plugin_shell::ShellExt;
    let image = firmware_path(&app).ok_or("this build of the app has no firmware image")?;
    if port.is_empty() || port.contains(char::is_whitespace) {
        return Err("choose the board's USB port first".into());
    }
    let args = [
        "--chip", "esp32s3", "-p", port.as_str(), "-b", "460800", "--before", "default-reset", "--after", "watchdog-reset",
        "write-flash", "0x0",
    ];
    let (mut rx, _child) = app
        .shell()
        .sidecar("esptool")
        .map_err(|e| format!("esptool is missing from this build: {e}"))?
        .args(args)
        .arg(image.as_os_str())
        .spawn()
        .map_err(|e| format!("could not start esptool: {e}"))?;
    let mut tail: Vec<String> = Vec::new();
    let mut code = None;
    while let Some(event) = rx.recv().await {
        match event {
            CommandEvent::Stdout(b) | CommandEvent::Stderr(b) => {
                for line in String::from_utf8_lossy(&b).split(['\n', '\r']).map(str::trim).filter(|l| !l.is_empty()) {
                    let _ = app.emit("flash-progress", line.to_string());
                    tail.push(line.to_string());
                    if tail.len() > 6 {
                        tail.remove(0);
                    }
                }
            }
            CommandEvent::Terminated(p) => code = p.code,
            _ => {}
        }
    }
    if code == Some(0) {
        Ok("Flashed. The board restarts with the new firmware and keeps its Wi-Fi networks and pairing.".into())
    } else {
        Err(format!("flashing failed: {}", tail.join(" | ")))
    }
}

#[tauri::command]
async fn read_sessions(folder: String) -> Result<Vec<SessionInfo>, String> {
    let root = PathBuf::from(&folder);
    if !root.is_dir() {
        return Err(format!("{folder} is not a folder"));
    }
    let mut out = Vec::new();
    for path in session_files(&root) {
        let id = path.file_stem().and_then(|s| s.to_str()).unwrap_or("").to_string();
        let table = read_table(&path)?;
        let has_model = path.with_extension("ml.csv").exists();
        if let Some(mut info) = summarise(&id, &table, has_model) {
            let json = std::fs::read_to_string(path.with_extension("json")).ok().and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok());
            if let Some(u) = json.as_ref().and_then(|v| v.get("units").and_then(|u| u.as_str()).map(str::to_string)) {
                if ["metric", "us", "uk"].contains(&u.as_str()) { info.units = u; }
            }
            info.ml_model = json.as_ref().and_then(|v| v.get("ml_model")).and_then(|m| m.as_str()).filter(|m| *m == "ev" || *m == "rev").map(str::to_string);
            out.push(info);
        }
    }
    Ok(out)
}

#[tauri::command]
async fn session_series(folder: String, id: String) -> Result<Series, String> {
    if !is_session_file(&format!("{id}.csv")) {
        return Err("not a session id".to_string());
    }
    let path = sessions_dir(&PathBuf::from(&folder)).join(format!("{id}.csv"));
    let table = read_table(&path)?;
    let uptime = table.column("uptime_ms");
    let first = uptime.iter().flatten().next().copied().unwrap_or(0.0);
    let step = (table.rows.len() / MAX_POINTS).max(1);
    let pick = |i: usize| i % step == 0;
    let minutes: Vec<f64> = uptime
        .iter()
        .enumerate()
        .filter(|(i, _)| pick(*i))
        .map(|(_, u)| u.map_or(f64::NAN, |u| (u - first) / 60_000.0))
        .collect();
    let channels = CHANNELS
        .iter()
        .map(|(name, unit)| Channel {
            name: name.to_string(),
            unit: unit.to_string(),
            values: table.column(name).into_iter().enumerate().filter(|(i, _)| pick(*i)).map(|(_, v)| v).collect(),
        })
        .collect();
    let mut stats = Vec::new();
    for name in &table.headers {
        if NOT_DATA.contains(&name.as_str()) { continue; }
        let vals: Vec<f64> = table.column(name).into_iter().flatten().collect();
        if vals.is_empty() { continue; }                          // the car never reported it: not shown, not zero
        let sum: f64 = vals.iter().sum();
        stats.push(ChannelStat {
            name: name.clone(),
            unit: unit_of(name).to_string(),
            count: vals.len(),
            min: vals.iter().copied().fold(f64::INFINITY, f64::min),
            mean: sum / vals.len() as f64,
            max: vals.iter().copied().fold(f64::NEG_INFINITY, f64::max),
            last: *vals.last().unwrap(),
        });
    }
    Ok(Series { minutes, channels, stats })
}

pub fn run() {
    let builder = tauri::Builder::default();
    #[cfg(desktop)]
    let builder = builder.plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
        use tauri::Manager;
        if let Some(window) = app.get_webview_window("main") {
            let _ = window.unminimize();
            let _ = window.show();
            let _ = window.set_focus();
        }
    }));
    builder
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_shell::init())
        .setup(|app| {
            use tauri::Manager;
            let dir = app.path().app_data_dir()?;
            app.manage(AppState { rx: receiver::Receiver::start(dir) });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            read_sessions,
            session_series,
            receiver_status,
            received_sources,
            board_ports,
            board_connect,
            board_request_code,
            board_pair,
            board_wifi_list,
            board_wifi_forget,
            board_setup,
            board_import,
            firmware_info,
            board_flash,
            board_model,
            board_model_set,
            board_rev_status,
            board_rev_reset,
            board_rev_fillups,
            fillups_load,
            fillups_save
        ])
        .run(tauri::generate_context!())
        .expect("OpenCarMind could not start");
}

#[cfg(test)]
mod tests {
    use super::*;

    fn table(csv_text: &str) -> Table {
        let mut r = csv::ReaderBuilder::new().flexible(true).from_reader(csv_text.as_bytes());
        let headers = r.headers().unwrap().iter().map(|h| h.to_string()).collect();
        let rows = r.records().map(|x| x.unwrap().iter().map(|c| c.to_string()).collect()).collect();
        Table { headers, rows }
    }

    #[test]
    fn dates() {
        assert_eq!(format_utc(0), "1970-01-01 00:00 UTC");
        assert_eq!(format_utc(1_767_225_600), "2026-01-01 00:00 UTC");
        assert_eq!(format_utc(951_782_400 + 3_660), "2000-02-29 01:01 UTC");
        assert_eq!(format_utc(951_782_400 + 86_400), "2000-03-01 00:00 UTC");
    }

    #[test]
    fn session_names() {
        assert!(is_session_file("s000111.csv"));
        assert!(is_session_file("s900002.csv"));
        assert!(!is_session_file("s000111.ml.csv"));
        assert!(!is_session_file("wifi.csv"));
        assert!(!is_session_file("s.csv"));
    }

    #[test]
    fn summary_matches_report_tool_rules() {
        // 3 rows, 10 s apart, 36 km/h then 72 km/h: 36*10/3600 + 72*10/3600 = 0.3 km
        let t = table("seq,uptime_ms,epoch_ms,speed_kph\n1,0,1767225600000,36\n2,10000,,72\n3,20000,,0\n");
        let s = summarise("s000001", &t, false).unwrap();
        assert_eq!(s.rows, 3);
        assert_eq!(s.minutes, 0.3);
        assert_eq!(s.distance_km, 0.3);
        assert_eq!(s.max_speed_kph, Some(72.0));
        assert_eq!(s.row_interval_s, Some(10.0));
        assert_eq!(s.date, "2026-01-01 00:00 UTC");
        assert_eq!(s.fuel_l, None);                // an old log with no fuel columns: unknown, not zero
        assert_eq!(s.economy_kpl, None);
    }

    #[test]
    fn fuel_and_economy() {
        // trip totals from the board win: 12 km on 1 L = 12 km/L
        let t = table("uptime_ms,speed_kph,fuel_lph_calc,fuel_src,trip_km,trip_fuel_l\n0,30,2,maf,0,0\n1000,30,2,maf,6,0.5\n2000,30,,,12,1\n");
        let s = summarise("s3", &t, false).unwrap();
        assert_eq!(s.fuel_l, Some(1.0));
        assert_eq!(s.economy_kpl, Some(12.0));
        assert_eq!(s.fuel_source.as_deref(), Some("maf"));
        // no trip columns: integrate the rate; 36 km/h for 100 s at 3.6 L/h = 1 km on 0.1 L = 10 km/L
        let mut csv = String::from("uptime_ms,speed_kph,fuel_lph_calc\n");
        for i in 0..=100 { csv.push_str(&format!("{},36,3.6\n", i * 1000)); }
        let s = summarise("s4", &table(&csv), false).unwrap();
        assert_eq!(s.economy_kpl, Some(10.0));
        // a 2.4 log from a car with no fuel source: trip totals are written as 0, but fuel is unknown, not 0 L
        let s = summarise("s5", &table("uptime_ms,speed_kph,fuel_lph_calc,fuel_src,trip_km,trip_fuel_l\n0,30,,,0,0\n1000,30,,,0,0\n"), false).unwrap();
        assert_eq!(s.fuel_l, None);
        assert_eq!(s.economy_kpl, None);
        assert_eq!(s.fuel_source, None);
        assert_eq!(s.fuel_source, None);
        assert_eq!(unit_of("odometer_km"), "km");
        assert_eq!(unit_of("not_a_column"), "");
    }

    #[test]
    fn time_server_names() {
        assert!(hostname_ok("www.google.com") && hostname_ok("my-broker.example.workers.dev"));
        assert!(!hostname_ok("localhost") && !hostname_ok("192.168.1.10") && !hostname_ok("a.b") && !hostname_ok("https://x.com"));
        assert!(!hostname_ok("x.com:443") && !hostname_ok("-x.com") && !hostname_ok("x-.com") && !hostname_ok("x..com") && !hostname_ok(".x.com"));
        let none = std::collections::HashMap::new();
        assert_eq!(config_object(&none, None, Some("WWW.Google.com".into())).unwrap().unwrap()["time_host"], "www.google.com");
        assert_eq!(config_object(&none, None, Some("default".into())).unwrap().unwrap()["time_host"], "");
        assert!(config_object(&none, None, Some("http://x".into())).is_err());
        assert!(config_object(&none, None, Some("  ".into())).unwrap().is_none());
    }

    #[test]
    fn wifi_entries() {
        assert!(wifi_entry_ok("Home", "").is_ok());
        assert!(wifi_entry_ok("Home", "12345678").is_ok());
        assert!(wifi_entry_ok("Home", "1234567").is_err());
        assert!(wifi_entry_ok("Ho,me", "12345678").is_err());
        assert!(wifi_entry_ok("-Home", "12345678").is_err());
        assert!(wifi_entry_ok("Home", "pass\nword1").is_err());
        assert!(timezone_ok("CST6CDT,M3.2.0,M11.1.0") && timezone_ok("<+06>-6") && !timezone_ok("CST 6") && !timezone_ok("UT"));
    }

    #[test]
    fn fill_up_units() {
        assert!((to_litres(10.0, "usgal").unwrap() - 37.85411784).abs() < 1e-9);
        assert!((to_litres(10.0, "ukgal").unwrap() - 45.4609).abs() < 1e-9);
        assert!(to_litres(10.0, "pints").is_err());
        assert!((to_l_per_100km(12.5, "kpl").unwrap() - 8.0).abs() < 1e-9);
        // 30 mpg (US) is 12.754 km/L, 7.84 L/100 km; 30 mpg (UK) is 10.62 km/L, 9.42 L/100 km
        assert!((to_l_per_100km(30.0, "mpgus").unwrap() - 7.8404).abs() < 1e-3);
        assert!((to_l_per_100km(30.0, "mpguk").unwrap() - 9.4160).abs() < 1e-3);
        assert!(to_l_per_100km(0.0, "kpl").is_err());
    }

    #[test]
    fn fill_list_for_the_board() {
        let pump = |id, after, amount: f64, unit: &str| FillEntry { id, kind: "pump".into(), after_session: after, amount, unit: unit.into(), full: true, ..Default::default() };
        let log = FillLog { rev: 3, entries: vec![pump(1, 10, 40.0, "L"), pump(2, 14, 10.0, "usgal")] };
        let v = board_fill_list(&log).unwrap();
        assert_eq!(v["rev"], 3);
        assert_eq!(v["entries"][1]["litres"], 37.854);
        assert_eq!(v["entries"][0]["after"], 10);
        let trip = FillEntry { id: 3, kind: "trip".into(), from_session: 11, after_session: 14, amount: 30.0, unit: "mpgus".into(), ..Default::default() };
        let v = board_fill_list(&FillLog { rev: 1, entries: vec![trip.clone()] }).unwrap();
        assert_eq!(v["entries"][0]["kind"], "trip");
        assert_eq!(v["entries"][0]["l100"], 7.84);
        // the board's rules: a typo of 400 L, a range backwards, more than 16 entries
        assert!(board_fill_list(&FillLog { rev: 1, entries: vec![pump(1, 1, 400.0, "L")] }).is_err());
        assert!(board_fill_list(&FillLog { rev: 1, entries: vec![FillEntry { from_session: 20, ..trip }] }).is_err());
        assert!(board_fill_list(&FillLog { rev: 1, entries: (0..17).map(|i| pump(i, i, 30.0, "L")).collect() }).is_err());
        // the board reads its JSON with cJSON: the text must stay one line of plain JSON
        assert!(!board_fill_list(&log).unwrap().to_string().contains('\n'));
    }

    #[test]
    fn short_or_clockless_sessions() {
        assert!(summarise("s1", &table("uptime_ms,speed_kph\n0,1\n"), false).is_none());
        let s = summarise("s2", &table("uptime_ms,speed_kph\n0,1\n1000,1\n"), false).unwrap();
        assert_eq!(s.date, "clock not set");
    }
}
