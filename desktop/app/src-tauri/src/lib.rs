//! Reads OpenCarMind session files from a folder and returns summaries and chart data to the window.
//! The arithmetic follows tools/ocm_report.py so the desktop app and the report tool agree.

mod board;
mod receiver;

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

/// Checks the settings and time zone the way the board does and turns them into a config.json object.
fn config_object(settings: &std::collections::HashMap<String, f64>, timezone: Option<String>) -> Result<Option<serde_json::Value>, String> {
    let mut map = serde_json::Map::new();
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
    let host = receiver::local_address().ok_or("this computer has no network address. Connect it to the Wi-Fi the board will use")?;
    if state.rx.token.is_empty() {
        return Err("the receiver could not start, so there is nothing to pair with".into());
    }
    let (token, cert) = (state.rx.token.clone(), state.rx.cert_pem.clone());
    let h = host.clone();
    blocking(move || board::pair(&port, &code, &h, receiver::PORT, &token, &cert)).await?;
    Ok(format!("Paired. The board will send its drives to {host}:{} when it is parked on Wi-Fi.", receiver::PORT))
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
) -> Result<String, String> {
    for (ssid, password) in &networks {
        wifi_entry_ok(ssid, password)?;
    }
    let config = config_object(&settings, timezone)?;
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
            board_flash
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
    fn short_or_clockless_sessions() {
        assert!(summarise("s1", &table("uptime_ms,speed_kph\n0,1\n"), false).is_none());
        let s = summarise("s2", &table("uptime_ms,speed_kph\n0,1\n1000,1\n"), false).unwrap();
        assert_eq!(s.date, "clock not set");
    }
}
