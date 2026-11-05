//! Entry point of the desktop app: the Tauri commands the window calls and the application state. The logic lives in
//! `sessions` (reading drives), `settings` (checks on what goes to the board), `fuel` (fill-ups), `board` (the USB door
//! protocol) and `receiver` (the TLS receiver for the reworked Wi-Fi upload).

mod board;
mod fuel;
mod receiver;
mod sessions;
mod settings;

use fuel::{board_fill_list, read_fill_log, FillEntry, FillLog};
use serde::Serialize;
use sessions::{is_session_file, read_table, session_files, sessions_dir, summarise, unit_of, Channel, ChannelStat, Series, SessionInfo, CHANNELS, MAX_POINTS, NOT_DATA};
use settings::{config_object, wifi_entry_ok};
use std::path::PathBuf;

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

fn fillups_path(state: &AppState, board: &str) -> Result<PathBuf, String> {
    if !receiver::valid_board(board) {
        return Err("unknown board".into());
    }
    Ok(state.rx.dir.join(board).join("fillups.json"))
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

#[tauri::command]
async fn board_reboot(port: String) -> Result<(), String> {
    blocking(move || board::reboot(&port)).await
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
    // is off in 2.5.x, so a computer without a network address pairs with the loopback address.
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
            board_reboot,
            fillups_load,
            fillups_save
        ])
        .run(tauri::generate_context!())
        .expect("OpenCarMind could not start");
}
