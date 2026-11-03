//! Talks to the board's USB settings console (docs/CONSOLE.md).
//!
//! The port is opened with DTR asserted and RTS released. On the ESP32-S3's USB Serial/JTAG port, opening with DTR
//! low resets the chip; with DTR high it does not (measured on a Waveshare ESP32-S3-LCD-1.47B, 2026-11-03).

use serde::Serialize;
use std::io::{Read, Write};
use std::time::{Duration, Instant};

const ESPRESSIF_VID: u16 = 0x303a;
const BAUD: u32 = 115_200;
const ANSWER_TIMEOUT: Duration = Duration::from_secs(5);
/// Commands the window may send, and how many arguments each takes after the command word.
const ALLOWED: [(&str, &str, usize, usize); 8] = [
    ("status", "", 0, 0),
    ("wifi", "list", 0, 0),
    ("wifi", "add", 1, 2),
    ("wifi", "forget", 1, 1),
    ("adapter", "forget", 0, 0),
    ("config", "show", 0, 0),
    ("config", "set", 2, 2),
    ("reboot", "", 0, 0),
];

#[derive(Serialize)]
pub struct PortInfo {
    pub name: String,
    pub label: String,
    pub likely_board: bool,
}

#[derive(Serialize, Debug, PartialEq)]
pub struct Answer {
    pub ok: bool,
    pub error: Option<String>,
    pub lines: Vec<String>,
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

fn quote(arg: &str) -> Result<String, String> {
    if arg.chars().any(|c| c.is_control()) {
        return Err("values cannot contain control characters".to_string());
    }
    Ok(format!("\"{}\"", arg.replace('\\', "\\\\").replace('"', "\\\"")))
}

/// Builds one console line from a command and its arguments, refusing anything not on the allowed list.
pub fn build_line(args: &[String]) -> Result<String, String> {
    let word = args.first().map(String::as_str).unwrap_or("");
    let sub = args.get(1).map(String::as_str).unwrap_or("");
    let rule = ALLOWED
        .iter()
        .find(|(w, s, _, _)| *w == word && (s.is_empty() || *s == sub))
        .ok_or_else(|| format!("command not allowed: {word} {sub}"))?;
    let skip = if rule.1.is_empty() { 1 } else { 2 };
    let extra = &args[skip.min(args.len())..];
    if extra.len() < rule.2 || extra.len() > rule.3 {
        return Err(format!("wrong number of values for {word} {sub}"));
    }
    let mut line = args[..skip].join(" ");
    for a in extra {
        line.push(' ');
        line.push_str(&quote(a)?);
    }
    Ok(line)
}

fn is_log_line(l: &str) -> bool {
    let b = l.as_bytes();
    b.len() > 3 && matches!(b[0], b'I' | b'W' | b'E' | b'D' | b'V') && b[1] == b' ' && b[2] == b'(' && b[3].is_ascii_digit()
}

fn strip_ansi(l: &str) -> String {
    let mut out = String::with_capacity(l.len());
    let mut chars = l.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\u{1b}' {
            if chars.peek() == Some(&'[') {
                chars.next();
                for d in chars.by_ref() {
                    if d.is_ascii_alphabetic() {
                        break;
                    }
                }
            }
        } else {
            out.push(c);
        }
    }
    out
}

/// Turns the console output for one command into an answer. Returns None until the closing line has arrived.
pub fn parse_answer(text: &str, sent: &str) -> Option<Answer> {
    let mut lines = Vec::new();
    for raw in text.split('\n') {
        let l = strip_ansi(raw).trim_end_matches('\r').trim().to_string();
        let l = l.strip_prefix("ocm> ").map(str::to_string).unwrap_or(l);
        if l.is_empty() || l == sent || l == "ocm>" || is_log_line(&l) {
            continue;
        }
        if l == "OK" {
            return Some(Answer { ok: true, error: None, lines });
        }
        if let Some(why) = l.strip_prefix("ERR ") {
            return Some(Answer { ok: false, error: Some(why.to_string()), lines });
        }
        if l == "Unrecognized command" {
            return Some(Answer { ok: false, error: Some("the board does not know this command; update its firmware".to_string()), lines });
        }
        lines.push(l);
    }
    None
}

pub fn send(port: &str, args: &[String]) -> Result<Answer, String> {
    let line = build_line(args)?;
    let mut p = serialport::new(port, BAUD)
        .timeout(Duration::from_millis(200))
        .dtr_on_open(true)
        .open()
        .map_err(|e| {
            let hint = if cfg!(target_os = "linux") { " On Linux, add yourself to the dialout group and log in again." } else { "" };
            format!("could not open {port}: {e}.{hint}")
        })?;
    let _ = p.write_request_to_send(false);
    let _ = p.clear(serialport::ClearBuffer::Input);
    p.write_all(format!("\n{line}\n").as_bytes()).map_err(|e| format!("could not write to {port}: {e}"))?;
    let start = Instant::now();
    let mut text = String::new();
    let mut buf = [0u8; 512];
    while start.elapsed() < ANSWER_TIMEOUT {
        match p.read(&mut buf) {
            Ok(n) if n > 0 => {
                text.push_str(&String::from_utf8_lossy(&buf[..n]));
                if let Some(a) = parse_answer(&text, &line) {
                    return Ok(a);
                }
            }
            Ok(_) => {}
            Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {}
            Err(e) => return Err(format!("could not read from {port}: {e}")),
        }
    }
    if args.first().map(String::as_str) == Some("reboot") {
        return Ok(Answer { ok: true, error: None, lines: vec![] });
    }
    Err("no answer from the board. Check the cable and that the board runs firmware 2.0.1 or later.".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v(a: &[&str]) -> Vec<String> {
        a.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn builds_allowed_lines_only() {
        assert_eq!(build_line(&v(&["status"])).unwrap(), "status");
        assert_eq!(build_line(&v(&["wifi", "add", "Guest net", "pa\"ss word"])).unwrap(), "wifi add \"Guest net\" \"pa\\\"ss word\"");
        assert_eq!(build_line(&v(&["config", "set", "backlight_pct", "60"])).unwrap(), "config set \"backlight_pct\" \"60\"");
        assert!(build_line(&v(&["rm", "-rf"])).is_err());
        assert!(build_line(&v(&["wifi", "add"])).is_err());
        assert!(build_line(&v(&["wifi", "add", "a\nreboot"])).is_err());
        assert!(build_line(&v(&["status", "extra"])).is_err());
    }

    #[test]
    fn parses_answers() {
        let t = "status\r\nI (1234) obd: something\r\nfirmware=2.0.1\r\nmodel=85d63ec33020\r\nOK\r\n";
        assert_eq!(parse_answer(t, "status").unwrap(), Answer { ok: true, error: None, lines: v(&["firmware=2.0.1", "model=85d63ec33020"]) });
        let e = parse_answer("ocm> wifi add x y\r\nERR rejected: too short\r\n", "wifi add x y").unwrap();
        assert!(!e.ok);
        assert_eq!(e.error.as_deref(), Some("rejected: too short"));
        assert!(parse_answer("firmware=2.0.1\r\n", "status").is_none());
        assert!(!parse_answer("\u{1b}[0mUnrecognized command\r\n", "x").unwrap().ok);
    }
}
