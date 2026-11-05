//! Session files (sNNNNNN.csv and friends) read from a folder: summaries for the Overview and Drives pages and the
//! chart series of one drive. The arithmetic follows tools/ocm_report.py so the desktop app and the report tool agree.

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

pub(crate) const CHANNELS: [(&str, &str); 6] = [
    ("speed_kph", "km/h"),
    ("rpm", "rpm"),
    ("fuel_lph_calc", "L/h"),
    ("coolant_c", "°C"),
    ("engine_load_pct", "%"),
    ("throttle_pct", "%"),
];
/// Columns that are bookkeeping, not car data.
pub(crate) const NOT_DATA: [&str; 4] = ["seq", "uptime_ms", "epoch_ms", "window_seq"];
const KNOWN_UNITS: [(&str, &str); 77] = [
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

pub(crate) fn unit_of(name: &str) -> &'static str {
    KNOWN_UNITS.iter().find(|(n, _)| *n == name).map_or("", |(_, u)| u)
}
pub(crate) const MAX_POINTS: usize = 800;

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
pub(crate) fn sessions_dir(folder: &Path) -> PathBuf {
    for candidate in [folder.join("obd").join("sessions"), folder.join("sessions")] {
        if candidate.is_dir() {
            return candidate;
        }
    }
    folder.to_path_buf()
}

pub(crate) fn is_session_file(name: &str) -> bool {
    let Some(stem) = name.strip_suffix(".csv") else { return false };
    let Some(digits) = stem.strip_prefix('s') else { return false };
    !digits.is_empty() && digits.bytes().all(|b| b.is_ascii_digit())
}

pub(crate) fn session_files(folder: &Path) -> Vec<PathBuf> {
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

pub(crate) struct Table {
    pub(crate) headers: Vec<String>,
    pub(crate) rows: Vec<Vec<String>>,
}

pub(crate) fn read_table(path: &Path) -> Result<Table, String> {
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
    pub(crate) fn column(&self, name: &str) -> Vec<Option<f64>> {
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

pub(crate) fn summarise(id: &str, table: &Table, has_model_file: bool) -> Option<SessionInfo> {
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
    fn short_or_clockless_sessions() {
        assert!(summarise("s1", &table("uptime_ms,speed_kph\n0,1\n"), false).is_none());
        let s = summarise("s2", &table("uptime_ms,speed_kph\n0,1\n1000,1\n"), false).unwrap();
        assert_eq!(s.date, "clock not set");
    }
}
