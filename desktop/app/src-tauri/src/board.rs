//! Client for the board's authenticated USB door (protocol in main/usb_door.h).
//!
//! The app does the work and the board only stores or applies what it is sent: the pairing, Wi-Fi networks and
//! settings go down the cable ready to use, and session files come back in pieces the app reassembles.
//!
//! The port is opened with DTR asserted and RTS released. On the ESP32-S3's USB Serial/JTAG port, opening with DTR low
//! resets the chip; with DTR high it does not (measured on a Waveshare ESP32-S3-LCD-1.47B).

use base64::{engine::general_purpose::STANDARD as B64, Engine};
use serde::Serialize;
use std::fs;
use std::io::{Read, Write};
use std::path::Path;
use std::time::{Duration, Instant};

const ESPRESSIF_VID: u16 = 0x303a;
const BAUD: u32 = 115_200;
pub const GET_MAX: u64 = 3072;
const SHORT: Duration = Duration::from_secs(6);

#[derive(Serialize)]
pub struct PortInfo {
    pub name: String,
    pub label: String,
    pub likely_board: bool,
}

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct Hello {
    pub firmware: String,
    pub board: String,
    pub paired: bool,
    pub authed: bool,
}

pub fn list_ports() -> Result<Vec<PortInfo>, String> {
    let ports = serialport::available_ports().map_err(|e| format!("could not list serial ports: {e}"))?;
    let mut out: Vec<PortInfo> = ports
        .into_iter()
        // macOS lists every device twice; the tty.* side waits for a carrier signal the board never raises
        .filter(|p| !p.port_name.starts_with("/dev/tty."))
        .map(|p| {
            let (label, likely) = match &p.port_type {
                serialport::SerialPortType::UsbPort(u) => (
                    u.product.clone().unwrap_or_else(|| format!("USB {:04x}:{:04x}", u.vid, u.pid)),
                    u.vid == ESPRESSIF_VID,
                ),
                _ => ("serial port".to_string(), false),
            };
            PortInfo { name: p.port_name, label, likely_board: likely }
        })
        .collect();
    out.sort_by(|a, b| b.likely_board.cmp(&a.likely_board).then(a.name.cmp(&b.name)));
    Ok(out)
}

pub struct Door {
    port: Box<dyn serialport::SerialPort>,
    pending: String,
}

impl Door {
    pub fn open(name: &str) -> Result<Door, String> {
        let mut port = serialport::new(name, BAUD)
            .timeout(Duration::from_millis(200))
            .dtr_on_open(true)
            .open()
            .map_err(|e| {
                let hint = if cfg!(target_os = "linux") { " On Linux, add yourself to the dialout group and log in again." } else { "" };
                format!("could not open {name}: {e}.{hint}")
            })?;
        let _ = port.write_request_to_send(false);
        let _ = port.clear(serialport::ClearBuffer::Input);
        Ok(Door { port, pending: String::new() })
    }

    /// Sends one command and returns the board's "OCM1" lines for it, ending with the "OK ..." line.
    /// An "ERR ..." answer becomes Err with the board's reason. Log lines on the same cable are skipped.
    pub fn call(&mut self, line: &str, timeout: Duration) -> Result<Vec<String>, String> {
        if line.contains(['\n', '\r']) {
            return Err("a command cannot contain a line break".into());
        }
        self.port.write_all(format!("\nOCM1 {line}\n").as_bytes()).map_err(|e| format!("could not write to the board: {e}"))?;
        let start = Instant::now();
        let mut got = Vec::new();
        let mut chunk = [0u8; 4096];
        loop {
            while let Some(i) = self.pending.find('\n') {
                let l = self.pending[..i].trim().to_string();
                self.pending.drain(..=i);
                if let Some(rest) = l.strip_prefix("OCM1 ") {
                    if rest == "OK" || rest.starts_with("OK ") {
                        got.push(rest.to_string());
                        return Ok(got);
                    }
                    if let Some(why) = rest.strip_prefix("ERR ") {
                        // The board ends a session after 10 minutes without a command: say what to do, not just the board's word.
                        return Err(if why == "not authenticated" { "the board's session has ended (10 minutes idle). Choose Connect on the Board page again.".to_string() } else { why.to_string() });
                    }
                    got.push(rest.to_string());
                }
            }
            if start.elapsed() > timeout {
                return Err("no answer from the board. Check the cable, and that it runs firmware 2.2.0 or later.".into());
            }
            match self.port.read(&mut chunk) {
                Ok(n) if n > 0 => self.pending.push_str(&String::from_utf8_lossy(&chunk[..n])),
                Ok(_) => {}
                Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {}
                Err(e) => return Err(format!("could not read from the board: {e}")),
            }
        }
    }
}

pub fn field<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    line.split_whitespace().find_map(|w| w.strip_prefix(key).and_then(|r| r.strip_prefix('=')))
}

pub fn hmac_hex(token: &str, nonce: &str) -> String {
    let key = ring::hmac::Key::new(ring::hmac::HMAC_SHA256, token.as_bytes());
    ring::hmac::sign(&key, nonce.as_bytes()).as_ref().iter().map(|b| format!("{b:02x}")).collect()
}

/// HELLO, then the HMAC answer when the board is already paired and this app has a token.
pub fn connect(port: &str, token: &str) -> Result<Hello, String> {
    let mut door = Door::open(port)?;
    let hello = door.call("HELLO", SHORT)?.pop().unwrap_or_default();
    let board = field(&hello, "board").unwrap_or("").to_string();
    if !crate::receiver::valid_board(&board) {
        return Err("the board's answer was not understood".into());
    }
    let paired = field(&hello, "paired") == Some("1");
    let mut authed = false;
    if paired && !token.is_empty() {
        if let Some(nonce) = field(&hello, "nonce") {
            authed = door.call(&format!("AUTH-HMAC {}", hmac_hex(token, nonce)), SHORT).is_ok();
        }
    }
    Ok(Hello { firmware: field(&hello, "fw").unwrap_or("?").to_string(), board, paired, authed })
}

pub fn request_code(port: &str) -> Result<(), String> {
    Door::open(port)?.call("CODE", SHORT).map(|_| ())
}

/// The code from the board's screen, then the pairing: this computer's address and port, the token and the certificate.
pub fn pair(port: &str, code: &str, host: &str, rx_port: u16, token: &str, cert_pem: &str) -> Result<(), String> {
    if code.len() != 6 || !code.bytes().all(|b| b.is_ascii_digit()) {
        return Err("the code has 6 digits".into());
    }
    let mut door = Door::open(port)?;
    door.call(&format!("AUTH-CODE {code}"), SHORT)?;
    door.call(&format!("PAIR {host} {rx_port} {token} {}", B64.encode(cert_pem)), SHORT).map(|_| ())
}

pub fn wifi_list(port: &str) -> Result<Vec<String>, String> {
    let lines = Door::open(port)?.call("WIFI-LIST", SHORT)?;
    Ok(lines
        .iter()
        .filter_map(|l| l.strip_prefix("D "))
        .filter_map(|b| B64.decode(b).ok())
        .map(|v| String::from_utf8_lossy(&v).to_string())
        .collect())
}

pub fn wifi_forget(port: &str, ssid: &str) -> Result<(), String> {
    Door::open(port)?.call(&format!("WIFI-FORGET {}", B64.encode(ssid)), SHORT).map(|_| ())
}

/// Sends networks that were already checked by the caller, and a config.json object if there is one.
pub fn setup(port: &str, networks: &[(String, String)], config: Option<&serde_json::Value>) -> Result<String, String> {
    let mut door = Door::open(port)?;
    for (ssid, password) in networks {
        let line = if password.is_empty() {
            format!("WIFI-ADD {}", B64.encode(ssid))
        } else {
            format!("WIFI-ADD {} {}", B64.encode(ssid), B64.encode(password))
        };
        door.call(&line, SHORT).map_err(|e| format!("{ssid}: {e}"))?;
    }
    let mut applied = 0;
    if let Some(cfg) = config {
        let answer = door.call(&format!("CONFIG {}", B64.encode(cfg.to_string())), SHORT)?;
        applied = answer.last().and_then(|l| field(l, "applied")).and_then(|n| n.parse().ok()).unwrap_or(0);
    }
    Ok(format!("{} network(s) and {applied} setting(s) sent to the board.", networks.len()))
}

/// The on-board model, from "OK MODEL active=ev available=ev,rev loaded=1 learning=0 run_us=3500".
#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct ModelInfo {
    pub active: String,
    pub available: Vec<String>,
    pub loaded: bool,
    pub learning: bool,
    pub run_us: Option<u64>,
}

pub fn parse_model(line: &str) -> Result<ModelInfo, String> {
    let active = field(line, "active").ok_or("the board's model answer was not understood")?.to_string();
    let available = field(line, "available").unwrap_or("").split(',').filter(|k| *k == "ev" || *k == "rev").map(str::to_string).collect();
    Ok(ModelInfo {
        active,
        available,
        loaded: field(line, "loaded") == Some("1"),
        learning: field(line, "learning") == Some("1"),
        run_us: field(line, "run_us").and_then(|v| v.parse().ok()).filter(|v| *v > 0),
    })
}

/// Firmware before 2.5 has no MODEL command: it answers "unknown command", which means only CarMind E.V.
pub fn model_get(port: &str) -> Result<ModelInfo, String> {
    match Door::open(port)?.call("MODEL GET", SHORT) {
        Ok(lines) => parse_model(lines.last().map(String::as_str).unwrap_or("")),
        Err(e) if e.contains("unknown command") => Ok(ModelInfo { active: "ev".into(), available: vec!["ev".into()], loaded: true, learning: false, run_us: None }),
        Err(e) => Err(e),
    }
}

/// Unloading one model and loading the other takes a few seconds on the board.
pub fn model_set(port: &str, key: &str) -> Result<ModelInfo, String> {
    if key != "ev" && key != "rev" {
        return Err("the model is ev or rev".into());
    }
    let lines = Door::open(port)?.call(&format!("MODEL SET {key}"), Duration::from_secs(20))?;
    parse_model(lines.last().map(String::as_str).unwrap_or(""))
}

/// One "D <group> key=value ..." line of REV-STATUS.
#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct StatusGroup {
    pub group: String,
    pub fields: std::collections::BTreeMap<String, String>,
}

pub fn parse_rev_status(lines: &[String]) -> Vec<StatusGroup> {
    lines
        .iter()
        .filter_map(|l| l.strip_prefix("D "))
        .filter_map(|rest| {
            let mut words = rest.split_whitespace();
            let group = words.next()?.to_string();
            let fields = words.filter_map(|w| w.split_once('=')).map(|(k, v)| (k.to_string(), v.to_string())).collect();
            Some(StatusGroup { group, fields })
        })
        .collect()
}

pub fn rev_status(port: &str) -> Result<Vec<StatusGroup>, String> {
    Ok(parse_rev_status(&Door::open(port)?.call("REV-STATUS", SHORT)?))
}

/// Sends the whole fill-up list (already converted to litres and L/100 km); returns the board's answer line.
pub fn rev_fillups(port: &str, list: &serde_json::Value) -> Result<String, String> {
    let lines = Door::open(port)?.call(&format!("REV-FILLUPS {}", B64.encode(list.to_string())), Duration::from_secs(10))?;
    Ok(lines.last().cloned().unwrap_or_default())
}

/// The board answers OK and restarts about 300 ms later; it keeps its Wi-Fi networks, settings, pairing and learned state.
pub fn reboot(port: &str) -> Result<(), String> {
    Door::open(port)?.call("REBOOT", SHORT).map(|_| ())
}

pub fn rev_reset(port: &str, what: &str) -> Result<(), String> {
    if !["fuel", "phys", "cal", "tank", "all"].contains(&what) {
        return Err("reset fuel, phys, cal, tank or all".into());
    }
    Door::open(port)?.call(&format!("REV-RESET {what}"), Duration::from_secs(10)).map(|_| ())
}

/// The session files on the card, as (name, size).
pub fn list_files(door: &mut Door) -> Result<Vec<(String, u64)>, String> {
    let lines = door.call("LIST", Duration::from_secs(20))?;
    Ok(lines
        .iter()
        .filter_map(|l| l.strip_prefix("D "))
        .filter_map(|r| {
            let mut it = r.split_whitespace();
            let name = it.next()?.to_string();
            let size = it.next()?.parse().ok()?;
            crate::receiver::valid_name(&name).then_some((name, size))
        })
        .collect())
}

pub fn get_chunk(door: &mut Door, name: &str, offset: u64, len: u64) -> Result<Vec<u8>, String> {
    let lines = door.call(&format!("GET {name} {offset} {len}"), SHORT)?;
    let data = lines.iter().find_map(|l| l.strip_prefix("D ")).unwrap_or("");
    B64.decode(data).map_err(|_| format!("{name}: the board sent damaged data"))
}

/// Copies what is new on the card into `dest`, the same folder the Wi-Fi receiver fills. Session files only grow, so
/// each one continues from the size already here; the session JSON is rewritten whole when it changed.
pub fn import(port: &str, dest: &Path, mut progress: impl FnMut(&str, u64, u64)) -> Result<String, String> {
    fs::create_dir_all(dest).map_err(|e| format!("{}: {e}", dest.display()))?;
    let mut door = Door::open(port)?;
    let files = list_files(&mut door)?;
    let (mut copied, mut bytes) = (0, 0u64);
    for (name, size) in &files {
        let path = dest.join(name);
        let have = fs::metadata(&path).map(|m| m.len()).unwrap_or(0);
        let json = name.ends_with(".json");
        if (json && have == *size) || (!json && have >= *size) {
            continue;
        }
        let mut data = Vec::new();
        let mut offset = if json { 0 } else { have };
        while offset < *size {
            let chunk = get_chunk(&mut door, name, offset, GET_MAX.min(size - offset))?;
            if chunk.is_empty() {
                break;
            }
            offset += chunk.len() as u64;
            bytes += chunk.len() as u64;
            data.extend_from_slice(&chunk);
            progress(name, offset, *size);
            if !json && data.len() >= 256 * 1024 {
                append(&path, &data)?;
                data.clear();
            }
        }
        if json {
            fs::write(&path, &data).map_err(|e| e.to_string())?;
        } else {
            append(&path, &data)?;
        }
        copied += 1;
    }
    Ok(format!("{copied} file(s) updated, {:.1} MB copied from the card.", bytes as f64 / 1_048_576.0))
}

fn append(path: &Path, data: &[u8]) -> Result<(), String> {
    fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
        .and_then(|mut f| f.write_all(data))
        .map_err(|e| format!("{}: {e}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hello_fields() {
        let l = "OK HELLO fw=2.2.0 board=a1b2c3d4e5f6 paired=1 nonce=00112233445566778899aabbccddeeff";
        assert_eq!(field(l, "fw"), Some("2.2.0"));
        assert_eq!(field(l, "board"), Some("a1b2c3d4e5f6"));
        assert_eq!(field(l, "paired"), Some("1"));
        assert_eq!(field(l, "missing"), None);
    }

    #[test]
    fn model_answers() {
        let m = parse_model("OK MODEL active=rev available=ev,rev loaded=1 learning=1 run_us=2100").unwrap();
        assert_eq!(m.active, "rev");
        assert_eq!(m.available, vec!["ev", "rev"]);
        assert!(m.loaded && m.learning);
        assert_eq!(m.run_us, Some(2100));
        let m = parse_model("OK MODEL active=ev available=ev loaded=1 learning=0 run_us=0").unwrap();
        assert_eq!(m.available, vec!["ev"]);
        assert_eq!(m.run_us, None);
        assert!(parse_model("OK MODEL").is_err());
    }

    #[test]
    fn rev_status_lines() {
        let lines = vec!["D cal c=1.0430 fill_rev=7 used=2".to_string(), "D iv id=3 from=12 to=20 why=0".to_string(), "OK REV-STATUS".to_string()];
        let g = parse_rev_status(&lines);
        assert_eq!(g.len(), 2);
        assert_eq!(g[0].group, "cal");
        assert_eq!(g[0].fields["c"], "1.0430");
        assert_eq!(g[1].fields["to"], "20");
    }

    #[test]
    fn hmac_matches_rfc4231_case_2() {
        // RFC 4231 test case 2: key "Jefe", data "what do ya want for nothing?"
        assert_eq!(hmac_hex("Jefe", "what do ya want for nothing?"), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    }
}
