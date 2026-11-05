//! What the app checks before it sends settings to the board: Wi-Fi entries, the time zone, the time server and the
//! numeric settings, with the same rules as the firmware (main/system/harden.c, main/storage/remote_config.c).

const SETTINGS: [(&str, f64, f64); 7] = [
    ("poll_interval_ms", 50.0, 5000.0),
    ("slow_group_every", 1.0, 50.0),
    ("dtc_interval_s", 10.0, 3600.0),
    ("backlight_pct", 5.0, 100.0),
    ("theme_mode", 0.0, 2.0),
    ("day_start_hour", 0.0, 23.0),
    ("day_end_hour", 0.0, 23.0),
];

/// Validates a Wi-Fi entry the way the board does (main/system/harden.c), plus no comma in the SSID because wifi.csv splits
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

/// Same rule as the board (main/system/harden.c): a POSIX TZ string of 3 to 47 characters from letters, digits and <>+-,.:/
pub fn timezone_ok(tz: &str) -> bool {
    (3..=47).contains(&tz.len()) && tz.chars().all(|c| c.is_ascii_alphanumeric() || "<>+-,.:/".contains(c))
}

/// The board's rule for the one time server it may contact (main/system/harden.c harden_hostname_ok): a DNS name, 4 to 63
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
pub(crate) fn config_object(
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

#[cfg(test)]
mod tests {
    use super::*;

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
}
