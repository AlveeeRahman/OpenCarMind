//! Reads OpenCarMind session files from a folder and returns summaries and chart data to the window.
//! The arithmetic follows tools/ocm_report.py so the desktop app and the report tool agree.

mod board;

use serde::Serialize;
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
}

const CHANNELS: [(&str, &str); 5] = [
    ("speed_kph", "km/h"),
    ("rpm", "rpm"),
    ("coolant_c", "°C"),
    ("engine_load_pct", "%"),
    ("throttle_pct", "%"),
];
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
    Some(SessionInfo {
        id: id.to_string(),
        date,
        rows: table.rows.len(),
        minutes: round1((last - first) / 60_000.0),
        row_interval_s: median(gaps).map(|g| (g * 100.0).round() / 100.0),
        max_speed_kph: max_speed,
        distance_km: round1(km),
        has_model_file,
    })
}

#[tauri::command]
async fn board_ports() -> Result<Vec<board::PortInfo>, String> {
    board::list_ports()
}

#[tauri::command]
async fn board_command(port: String, args: Vec<String>) -> Result<board::Answer, String> {
    tauri::async_runtime::spawn_blocking(move || board::send(&port, &args))
        .await
        .map_err(|e| format!("board task failed: {e}"))?
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
        if let Some(info) = summarise(&id, &table, has_model) {
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
    Ok(Series { minutes, channels })
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
        .invoke_handler(tauri::generate_handler![read_sessions, session_series, board_ports, board_command])
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
    }

    #[test]
    fn short_or_clockless_sessions() {
        assert!(summarise("s1", &table("uptime_ms,speed_kph\n0,1\n"), false).is_none());
        let s = summarise("s2", &table("uptime_ms,speed_kph\n0,1\n1000,1\n"), false).unwrap();
        assert_eq!(s.date, "clock not set");
    }
}
