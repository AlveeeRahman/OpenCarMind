//! Receives session files from OpenCarMind boards on the same network (protocol in main/uploader.h).
//!
//! TLS with a self-signed certificate made on first run. The board trusts only that certificate, which reaches it over
//! the authenticated USB door (board.rs), together with a random token. Files are stored per board under the app data folder.

use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
use serde::Serialize;
use std::collections::HashMap;
use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream, UdpSocket};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

pub const PORT: u16 = 8787;
/// 2.5.x: the board sends nothing over Wi-Fi (CONFIG_OCM_WIFI_UPLOAD off), so the app opens no port. The upload is being
/// reworked into a transfer without head-of-line blocking; drives come over the USB cable meanwhile.
pub const WIFI_UPLOAD: bool = false;
const MAX_BODY: usize = 64 * 1024;
const MAX_CONNECTIONS: usize = 8;

#[derive(Serialize, Clone, Default)]
pub struct BoardSeen {
    pub id: String,
    pub firmware: String,
    pub last_seen_s: u64,
    pub bytes_received: u64,
}

#[derive(Serialize, Clone, Default)]
pub struct Status {
    pub running: bool,
    pub port: u16,
    pub address: Option<String>,
    pub error: Option<String>,
    pub folder: String,
    pub boards: Vec<BoardSeen>,
}

pub struct Receiver {
    pub dir: PathBuf,
    pub token: String,
    pub cert_pem: String,
    status: Mutex<Status>,
    seen: Mutex<HashMap<String, BoardSeen>>,
    active: AtomicUsize,
}

fn random_token() -> Result<String, String> {
    let mut b = [0u8; 24];
    getrandom::getrandom(&mut b).map_err(|e| format!("no random source: {e}"))?;
    Ok(b.iter().map(|x| format!("{x:02x}")).collect())
}

/// Loads the receiver's certificate, key and token from `dir`, making them on first run.
fn identity(dir: &Path) -> Result<(String, Vec<u8>, Vec<u8>, String), String> {
    fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let (cert_p, key_p, tok_p) = (dir.join("cert.pem"), dir.join("key.pk8"), dir.join("token.txt"));
    if !(cert_p.exists() && key_p.exists() && tok_p.exists()) {
        let mut params = rcgen::CertificateParams::new(vec!["opencarmind.local".to_string()]).map_err(|e| e.to_string())?;
        params.distinguished_name.push(rcgen::DnType::CommonName, "OpenCarMind desktop");
        params.not_before = rcgen::date_time_ymd(1970, 1, 1); // the board's clock may still be unset
        params.not_after = rcgen::date_time_ymd(2099, 12, 31);
        let key = rcgen::KeyPair::generate().map_err(|e| e.to_string())?;
        let cert = params.self_signed(&key).map_err(|e| e.to_string())?;
        fs::write(&cert_p, cert.pem()).map_err(|e| e.to_string())?;
        fs::write(&key_p, key.serialize_der()).map_err(|e| e.to_string())?;
        fs::write(&tok_p, random_token()?).map_err(|e| e.to_string())?;
    }
    let cert_pem = fs::read_to_string(&cert_p).map_err(|e| e.to_string())?;
    let cert_der = rustls_pemfile_der(&cert_pem)?;
    let key_der = fs::read(&key_p).map_err(|e| e.to_string())?;
    let token = fs::read_to_string(&tok_p).map_err(|e| e.to_string())?.trim().to_string();
    Ok((cert_pem, cert_der, key_der, token))
}

/// The DER bytes inside one PEM certificate.
fn rustls_pemfile_der(pem: &str) -> Result<Vec<u8>, String> {
    let b64: String = pem.lines().filter(|l| !l.starts_with("-----")).collect();
    base64_decode(&b64).ok_or_else(|| "certificate file is damaged".to_string())
}

fn base64_decode(s: &str) -> Option<Vec<u8>> {
    let val = |c: u8| -> Option<u32> {
        Some(match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            _ => return None,
        } as u32)
    };
    let bytes: Vec<u8> = s.bytes().filter(|c| *c != b'=' && !c.is_ascii_whitespace()).collect();
    let mut out = Vec::with_capacity(bytes.len() * 3 / 4);
    for chunk in bytes.chunks(4) {
        let mut n = 0u32;
        for (i, c) in chunk.iter().enumerate() {
            n |= val(*c)? << (18 - 6 * i);
        }
        out.push((n >> 16) as u8);
        if chunk.len() > 2 {
            out.push((n >> 8) as u8);
        }
        if chunk.len() > 3 {
            out.push(n as u8);
        }
    }
    Some(out)
}

/// The address other machines on the network reach this computer at. No packet is sent.
pub fn local_address() -> Option<String> {
    let s = UdpSocket::bind("0.0.0.0:0").ok()?;
    s.connect("192.0.2.1:9").ok()?;
    let ip = s.local_addr().ok()?.ip();
    if ip.is_loopback() || ip.is_unspecified() { None } else { Some(ip.to_string()) }
}

pub fn valid_name(n: &str) -> bool {
    let Some(rest) = n.strip_prefix('s') else { return false };
    if rest.len() < 10 || !rest.as_bytes()[..6].iter().all(|b| b.is_ascii_digit()) {
        return false;
    }
    matches!(&rest[6..], ".csv" | ".ml.csv" | ".pat" | ".raw" | ".json")
}

pub fn valid_board(b: &str) -> bool {
    b.len() == 12 && b.bytes().all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase())
}

fn query(path: &str, key: &str) -> Option<String> {
    let q = path.split_once('?')?.1;
    q.split('&').find_map(|kv| kv.strip_prefix(key).and_then(|r| r.strip_prefix('=')).map(str::to_string))
}

fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

impl Receiver {
    pub fn start(dir: PathBuf) -> Arc<Receiver> {
        let folder = dir.join("received");
        let mut status = Status { port: PORT, folder: folder.display().to_string(), ..Default::default() };
        let ident = identity(&dir.join("receiver"));
        let (cert_pem, token) = match &ident {
            Ok((c, _, _, t)) => (c.clone(), t.clone()),
            Err(e) => {
                status.error = Some(e.clone());
                (String::new(), String::new())
            }
        };
        let r = Arc::new(Receiver { dir: folder, token, cert_pem, status: Mutex::new(status), seen: Mutex::new(HashMap::new()), active: AtomicUsize::new(0) });
        if let Ok((_, cert_der, key_der, _)) = ident {
            if WIFI_UPLOAD {
                let r2 = r.clone();
                std::thread::spawn(move || r2.serve(cert_der, key_der));
            } else {
                r.status.lock().unwrap().error = Some("off in 2.5.x: drives come over the USB cable while the Wi-Fi upload is reworked".into());
            }
        }
        r
    }

    pub fn status(&self) -> Status {
        let mut s = self.status.lock().unwrap().clone();
        s.address = local_address();
        let mut boards: Vec<BoardSeen> = self.seen.lock().unwrap().values().cloned().collect();
        boards.sort_by(|a, b| b.last_seen_s.cmp(&a.last_seen_s));
        s.boards = boards;
        s
    }

    fn fail(&self, e: String) {
        let mut s = self.status.lock().unwrap();
        s.running = false;
        s.error = Some(e);
    }

    fn serve(self: Arc<Self>, cert_der: Vec<u8>, key_der: Vec<u8>) {
        let provider = Arc::new(rustls::crypto::ring::default_provider());
        let config = rustls::ServerConfig::builder_with_provider(provider)
            .with_safe_default_protocol_versions()
            .map_err(|e| e.to_string())
            .and_then(|b| {
                b.with_no_client_auth()
                    .with_single_cert(vec![CertificateDer::from(cert_der)], PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(key_der)))
                    .map_err(|e| e.to_string())
            });
        let config = match config {
            Ok(c) => Arc::new(c),
            Err(e) => return self.fail(format!("TLS setup failed: {e}")),
        };
        let listener = match TcpListener::bind(("0.0.0.0", PORT)) {
            Ok(l) => l,
            Err(e) => return self.fail(format!("could not listen on port {PORT}: {e}. Is another copy of the app running?")),
        };
        {
            let mut s = self.status.lock().unwrap();
            s.running = true;
            s.error = None;
        }
        for stream in listener.incoming().flatten() {
            if self.active.load(Ordering::SeqCst) >= MAX_CONNECTIONS {
                continue;
            }
            let me = self.clone();
            let cfg = config.clone();
            self.active.fetch_add(1, Ordering::SeqCst);
            std::thread::spawn(move || {
                let _ = me.connection(stream, cfg);
                me.active.fetch_sub(1, Ordering::SeqCst);
            });
        }
    }

    fn connection(&self, tcp: TcpStream, cfg: Arc<rustls::ServerConfig>) -> Result<(), String> {
        tcp.set_read_timeout(Some(Duration::from_secs(30))).map_err(|e| e.to_string())?;
        tcp.set_write_timeout(Some(Duration::from_secs(30))).map_err(|e| e.to_string())?;
        let conn = rustls::ServerConnection::new(cfg).map_err(|e| e.to_string())?;
        let mut io = BufReader::new(rustls::StreamOwned::new(conn, tcp));
        loop {
            let mut line = String::new();
            if io.read_line(&mut line).map_err(|e| e.to_string())? == 0 {
                return Ok(());
            }
            let mut parts = line.split_whitespace();
            let (method, path) = (parts.next().unwrap_or("").to_string(), parts.next().unwrap_or("").to_string());
            let mut headers: HashMap<String, String> = HashMap::new();
            loop {
                let mut h = String::new();
                if io.read_line(&mut h).map_err(|e| e.to_string())? == 0 {
                    return Ok(());
                }
                let h = h.trim_end();
                if h.is_empty() {
                    break;
                }
                if let Some((k, v)) = h.split_once(':') {
                    headers.insert(k.trim().to_ascii_lowercase(), v.trim().to_string());
                }
                if headers.len() > 32 {
                    return Err("too many headers".into());
                }
            }
            let len: usize = headers.get("content-length").and_then(|v| v.parse().ok()).unwrap_or(0);
            if len > MAX_BODY {
                return Err("body too large".into());
            }
            let mut body = vec![0u8; len];
            io.read_exact(&mut body).map_err(|e| e.to_string())?;
            let (code, text) = self.handle(&method, &path, &headers, &body);
            let reason = match code { 200 => "OK", 400 => "Bad Request", 401 => "Unauthorized", 404 => "Not Found", 409 => "Conflict", _ => "Error" };
            let reply = format!("HTTP/1.1 {code} {reason}\r\nContent-Type: text/plain\r\nContent-Length: {}\r\n\r\n{text}", text.len());
            io.get_mut().write_all(reply.as_bytes()).map_err(|e| e.to_string())?;
            io.get_mut().flush().map_err(|e| e.to_string())?;
        }
    }

    pub fn handle(&self, method: &str, path: &str, headers: &HashMap<String, String>, body: &[u8]) -> (u16, String) {
        let auth = headers.get("authorization").map(String::as_str).unwrap_or("");
        if self.token.is_empty() || !constant_time_eq(auth.as_bytes(), format!("Bearer {}", self.token).as_bytes()) {
            return (401, "pair the board again\n".into());
        }
        let board = headers.get("x-ocm-board").cloned().unwrap_or_default();
        if !valid_board(&board) {
            return (400, "bad board id\n".into());
        }
        let dir = self.dir.join(&board).join("sessions");
        if fs::create_dir_all(&dir).is_err() {
            return (500, "cannot store files\n".into());
        }
        {
            let now = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
            let mut seen = self.seen.lock().unwrap();
            let e = seen.entry(board.clone()).or_insert_with(|| BoardSeen { id: board.clone(), ..Default::default() });
            e.last_seen_s = now;
            e.firmware = headers.get("x-ocm-firmware").cloned().unwrap_or_default().chars().take(24).collect();
            e.bytes_received += body.len() as u64;
        }
        let route = path.split('?').next().unwrap_or("");
        match (method, route) {
            ("GET", "/api/v1/files") => {
                let mut out = String::new();
                if let Ok(entries) = fs::read_dir(&dir) {
                    for e in entries.flatten() {
                        let name = e.file_name().to_string_lossy().to_string();
                        if valid_name(&name) {
                            out.push_str(&format!("{name} {}\n", e.metadata().map(|m| m.len()).unwrap_or(0)));
                        }
                    }
                }
                (200, out)
            }
            ("POST", "/api/v1/append") => {
                let name = query(path, "name").unwrap_or_default();
                let Some(offset) = query(path, "offset").and_then(|o| o.parse::<u64>().ok()) else { return (400, "bad offset\n".into()) };
                if !valid_name(&name) || name.ends_with(".json") {
                    return (400, "bad name\n".into());
                }
                let p = dir.join(&name);
                let have = fs::metadata(&p).map(|m| m.len()).unwrap_or(0);
                if offset != have {
                    return (409, format!("{have}\n"));
                }
                let r = fs::OpenOptions::new().create(true).append(true).open(&p).and_then(|mut f| f.write_all(body));
                match r {
                    Ok(()) => (200, format!("{}\n", have + body.len() as u64)),
                    Err(_) => (500, "write failed\n".into()),
                }
            }
            ("PUT", "/api/v1/put") => {
                let name = query(path, "name").unwrap_or_default();
                if !valid_name(&name) || !name.ends_with(".json") {
                    return (400, "bad name\n".into());
                }
                let tmp = dir.join(format!("{name}.tmp"));
                match fs::write(&tmp, body).and_then(|_| fs::rename(&tmp, dir.join(&name))) {
                    Ok(()) => (200, format!("{}\n", body.len())),
                    Err(_) => (500, "write failed\n".into()),
                }
            }
            _ => (404, "unknown request\n".into()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rx(dir: &Path) -> Receiver {
        Receiver {
            dir: dir.to_path_buf(),
            token: "t".repeat(48),
            cert_pem: String::new(),
            status: Mutex::new(Status::default()),
            seen: Mutex::new(HashMap::new()),
            active: AtomicUsize::new(0),
        }
    }

    fn h(token: &str) -> HashMap<String, String> {
        HashMap::from([("authorization".into(), format!("Bearer {token}")), ("x-ocm-board".into(), "a1b2c3d4e5f6".into())])
    }

    #[test]
    fn append_resumes_by_offset() {
        let tmp = std::env::temp_dir().join(format!("ocm-rx-{}", std::process::id()));
        let r = rx(&tmp);
        let ok = h(&"t".repeat(48));
        assert_eq!(r.handle("GET", "/api/v1/files", &h("wrong"), b"").0, 401);
        assert_eq!(r.handle("POST", "/api/v1/append?name=s000001.csv&offset=0", &ok, b"abc"), (200, "3\n".into()));
        assert_eq!(r.handle("POST", "/api/v1/append?name=s000001.csv&offset=0", &ok, b"abc"), (409, "3\n".into()));
        assert_eq!(r.handle("POST", "/api/v1/append?name=s000001.csv&offset=3", &ok, b"de"), (200, "5\n".into()));
        assert_eq!(r.handle("POST", "/api/v1/append?name=../x.csv&offset=0", &ok, b"x").0, 400);
        assert_eq!(r.handle("PUT", "/api/v1/put?name=s000001.json", &ok, b"{}").0, 200);
        let list = r.handle("GET", "/api/v1/files", &ok, b"").1;
        assert!(list.contains("s000001.csv 5\n") && list.contains("s000001.json 2\n"));
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn names_and_base64() {
        assert!(valid_name("s000123.ml.csv") && valid_name("s000123.json") && !valid_name("s12.csv") && !valid_name("s000123.exe"));
        assert_eq!(base64_decode("aGVsbG8=").unwrap(), b"hello");
        assert_eq!(query("/a?name=s1.csv&offset=7", "offset").as_deref(), Some("7"));
    }
}
