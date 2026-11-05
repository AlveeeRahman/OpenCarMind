//! Fill-ups (main/model/rev_cal.h). The app keeps the user's log and sends all of it to the board, which works out one
//! correction factor for its fuel figures. The board takes litres and L/100 km; the app converts the user's units.

use serde::{Deserialize, Serialize};
use std::path::Path;

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

pub(crate) fn read_fill_log(path: &Path) -> FillLog {
    std::fs::read_to_string(path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

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
}
