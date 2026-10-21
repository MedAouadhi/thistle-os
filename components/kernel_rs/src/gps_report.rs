// SPDX-License-Identifier: BSD-3-Clause
//! A report of each GPS start, on the card, for working out afterwards why
//! one was slow: the console is not kept, and a start worth reporting
//! happens away from a computer.
//!
//! One file a start, `gps/starts/NNNN-YYYYMMDD-HHMMSS.jsonl` on the card,
//! the time UTC (`NNNN.jsonl` while the clock is not set), one JSON object
//! a line, `t` in seconds since the receiver started:
//!
//! - `start`: when, and the orbits kept;
//! - `handed`: what the receiver was handed, each time (`gps_assist.rs`);
//! - `sats`: every 5 s, every satellite the receiver knows of, from the
//!   driver's `get_satellites`: its signal, where it is, whether it is used,
//!   and which orbits the receiver has for it, in the columns the `start`
//!   line names;
//! - `fix`: the first fix, with the satellites in use;
//! - `end`: why it ended, and of each constellation what was heard and
//!   used, which the log says too, as it does of the first `sats`. It
//!   ends 30 s after the fix, when the receiver starts again (after a
//!   sleep, or GPS switched off and on), after 30 minutes without a fix,
//!   or at 2 MB.
//!
//! No position is written. The last 20 are kept. `"report": false` in the
//! card's config/assistnow.json stops them.
//!
//! The decisions and the lines are the pure functions at the top, tested on
//! the host. `look` runs in AssistNow's once-a-second look, on the kernel
//! worker, as the driver's reader does.

use std::fmt::Write as _;
use std::io::Write as _;
use std::path::PathBuf;
use std::sync::{Mutex, PoisonError};
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::hal_registry::{
    HalGpsAssist, HalGpsSat, HalGpsSats, HAL_GPS_HAS_ALMANAC, HAL_GPS_HAS_AUTONOMOUS,
    HAL_GPS_HAS_EPHEMERIS, HAL_GPS_HAS_PREDICTED, HAL_GPS_ORBIT_ALMANAC, HAL_GPS_ORBIT_AUTONOMOUS,
    HAL_GPS_ORBIT_EPHEMERIS, HAL_GPS_ORBIT_NONE, HAL_GPS_ORBIT_PREDICTED,
};

const ESP_OK: i32 = 0;
const ESP_ERR_NOT_FOUND: i32 = 0x105;

/// How often the satellites are written.
const SAMPLE_EVERY: Duration = Duration::from_secs(5);
/// How long a report goes on after the fix: the satellites the receiver
/// takes up once it has one.
const AFTER_FIX: Duration = Duration::from_secs(30);
/// The longest a report goes on without a fix.
const LONGEST: Duration = Duration::from_secs(30 * 60);
const MAX_BYTES: u64 = 2 * 1024 * 1024;
/// Reports kept on the card.
const KEEP: usize = 20;

/// A satellite's columns in a `sats` line.
const COLUMNS: [&str; 9] = ["sv", "dBHz", "elev", "azim", "quality", "used", "health", "orbit", "has"];

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

#[cfg(not(test))]
static TAG: &[u8] = b"gps_report\0";

#[cfg(not(test))]
macro_rules! log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {
        unsafe { esp_log_write($level, TAG.as_ptr(), $fmt.as_ptr() $(, $arg)*) }
    };
}
#[cfg(test)]
macro_rules! log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {{
        let _ = ($level, $fmt $(, $arg)*);
    }};
}

const ESP_LOG_WARN: i32 = 2;
const ESP_LOG_INFO: i32 = 3;

// ---------------------------------------------------------------------------
// The lines
// ---------------------------------------------------------------------------

/// A duration as the report's `t`: seconds, to a tenth.
fn secs(d: Duration) -> f64 {
    (d.as_secs_f64() * 10.0).round() / 10.0
}

/// `secs` since 1970 as "2026-10-21T19:45:03Z".
pub(crate) fn iso(secs: i64) -> String {
    let (year, month, day) = crate::gps_assist::civil_from_days(secs.div_euclid(86_400));
    let of_day = secs.rem_euclid(86_400);
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}Z",
        of_day / 3600,
        of_day / 60 % 60,
        of_day % 60
    )
}

fn orbit_code(orbit: u8) -> &'static str {
    match orbit {
        HAL_GPS_ORBIT_NONE => "-",
        HAL_GPS_ORBIT_EPHEMERIS => "eph",
        HAL_GPS_ORBIT_ALMANAC => "alm",
        HAL_GPS_ORBIT_PREDICTED => "pred",
        HAL_GPS_ORBIT_AUTONOMOUS => "auto",
        _ => "other",
    }
}

/// The orbits held, "eph+pred"; "" for none.
fn held(bits: u8) -> String {
    [
        (HAL_GPS_HAS_EPHEMERIS, "eph"),
        (HAL_GPS_HAS_ALMANAC, "alm"),
        (HAL_GPS_HAS_PREDICTED, "pred"),
        (HAL_GPS_HAS_AUTONOMOUS, "auto"),
    ]
    .iter()
    .filter(|(bit, _)| bits & bit != 0)
    .map(|(_, name)| *name)
    .collect::<Vec<_>>()
    .join("+")
}

/// One satellite, in `COLUMNS`' order: `["G12",38,45,123,7,1,1,"pred","eph+pred"]`.
fn push_row(out: &mut String, s: &HalGpsSat) {
    let elev = if s.elev == -128 { "null".to_string() } else { s.elev.to_string() };
    let azim = if s.azim < 0 { "null".to_string() } else { s.azim.to_string() };
    let _ = write!(
        out,
        "[\"{}{:02}\",{},{},{},{},{},{},\"{}\",\"{}\"]",
        s.gnss as char,
        s.sv,
        s.cno,
        elev,
        azim,
        s.quality,
        s.used as u8,
        s.health,
        orbit_code(s.orbit),
        held(s.orbits)
    );
}

pub(crate) fn start_line(number: u32, utc_s: Option<i64>, orbits: Option<(i64, usize)>) -> String {
    let line = json!({
        "type": "start",
        "number": number,
        "utc": utc_s.map(iso),
        "orbits_fetched": orbits.filter(|(at, _)| *at > 0).map(|(at, _)| iso(at)),
        "orbit_bytes": orbits.map_or(0, |(_, n)| n),
        "columns": COLUMNS,
        "orbit": {
            "-": "none",
            "eph": "decoded off the satellite",
            "alm": "almanac",
            "pred": "AssistNow's prediction",
            "auto": "the receiver's own prediction (AssistNow Autonomous)",
            "other": "other",
        },
        "quality": "0 no signal, 1 searching, 2 acquired, 3 unusable, 4 code locked, 5-7 code and carrier locked",
        "health": "0 not known, 1 healthy, 2 unhealthy",
    });
    format!("{line}\n")
}

pub(crate) fn handed_line(t: Duration, aid: &HalGpsAssist) -> String {
    let line = json!({
        "type": "handed",
        "t": secs(t),
        "time_acc_ms": (aid.utc_us > 0).then_some(aid.utc_acc_ms),
        "position_acc_m": aid.has_position.then_some(aid.position_acc_m),
        "orbit_bytes": aid.data_len,
    });
    format!("{line}\n")
}

pub(crate) fn sats_line(t: Duration, sats: &[HalGpsSat]) -> String {
    // Over 4 KB, so from PSRAM; 60 satellites take about 2.7 KB.
    let mut line = String::with_capacity(8192);
    let used = sats.iter().filter(|s| s.used).count();
    let heard = sats.iter().filter(|s| s.cno > 0).count();
    let _ = write!(line, "{{\"type\":\"sats\",\"t\":{},\"heard\":{heard},\"used\":{used},\"sats\":[", secs(t));
    for (i, s) in sats.iter().enumerate() {
        if i > 0 {
            line.push(',');
        }
        push_row(&mut line, s);
    }
    line.push_str("]}\n");
    line
}

pub(crate) fn fix_line(t: Duration, in_use: u8) -> String {
    format!("{}\n", json!({ "type": "fix", "t": secs(t), "in_use": in_use }))
}

pub(crate) fn end_line(t: Duration, why: &str, fix_at: Option<Duration>, summary: &str) -> String {
    let line = json!({
        "type": "end",
        "t": secs(t),
        "why": why,
        "fix_at": fix_at.map(secs),
        "summary": summary,
    });
    format!("{line}\n")
}

/// Of each constellation listed, what was heard, used, and had orbits:
/// "GPS 9 heard, 6 used, orbits: 3 decoded, 8 predicted, 0 autonomous".
pub(crate) fn summary(sats: &[HalGpsSat]) -> String {
    const NAMES: [(u8, &str); 8] = [
        (b'G', "GPS"),
        (b'E', "Galileo"),
        (b'R', "GLONASS"),
        (b'C', "BeiDou"),
        (b'J', "QZSS"),
        (b'S', "SBAS"),
        (b'I', "NavIC"),
        (b'?', "other"),
    ];
    let parts: Vec<String> = NAMES
        .iter()
        .filter_map(|&(letter, name)| {
            let of: Vec<&HalGpsSat> = sats.iter().filter(|s| s.gnss == letter).collect();
            if of.is_empty() {
                return None;
            }
            let count = |f: &dyn Fn(&HalGpsSat) -> bool| of.iter().filter(|s| f(s)).count();
            Some(format!(
                "{name} {} heard, {} used, orbits: {} decoded, {} predicted, {} autonomous",
                count(&|s| s.cno > 0),
                count(&|s| s.used),
                count(&|s| s.orbits & HAL_GPS_HAS_EPHEMERIS != 0),
                count(&|s| s.orbits & HAL_GPS_HAS_PREDICTED != 0),
                count(&|s| s.orbits & HAL_GPS_HAS_AUTONOMOUS != 0),
            ))
        })
        .collect();
    if parts.is_empty() {
        "no satellites listed".to_string()
    } else {
        parts.join("; ")
    }
}

// ---------------------------------------------------------------------------
// The files
// ---------------------------------------------------------------------------

pub(crate) fn file_name(number: u32, utc_s: Option<i64>) -> String {
    match utc_s {
        Some(t) => {
            let at = iso(t);
            // "2026-10-21T19:45:03Z" to "20261021-194503".
            let digits: String = at.chars().filter(char::is_ascii_digit).collect();
            format!("{number:04}-{}-{}.jsonl", &digits[..8], &digits[8..14])
        }
        None => format!("{number:04}.jsonl"),
    }
}

/// A report's number, from its name; None for anything else.
fn number_of(name: &str) -> Option<u32> {
    let stem = name.strip_suffix(".jsonl")?;
    let digits = stem.split('-').next()?;
    if digits.is_empty() || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    digits.parse().ok()
}

pub(crate) fn next_number(names: &[String]) -> u32 {
    names.iter().filter_map(|n| number_of(n)).max().map_or(1, |n| n.saturating_add(1))
}

/// The reports to remove so that `keep` are left: the lowest numbered.
/// Anything else in the folder is left alone.
pub(crate) fn pruned(names: &[String], keep: usize) -> Vec<String> {
    let mut reports: Vec<(u32, &String)> =
        names.iter().filter_map(|n| number_of(n).map(|num| (num, n))).collect();
    reports.sort();
    let over = reports.len().saturating_sub(keep);
    reports.into_iter().take(over).map(|(_, n)| n.clone()).collect()
}

fn dir() -> PathBuf {
    crate::appstore_client::card_root().join("gps/starts")
}

// ---------------------------------------------------------------------------
// The report
// ---------------------------------------------------------------------------

/// What AssistNow's look saw this second.
pub(crate) struct Look<'a> {
    pub now: Instant,
    pub utc_s: Option<i64>,
    /// The receiver started since the last look.
    pub started: bool,
    pub gps_on: bool,
    /// The satellites in use, with a fix.
    pub fix: Option<u8>,
    /// What the receiver was handed this second.
    pub handed: Option<&'a HalGpsAssist>,
    /// `report` in the card's config/assistnow.json.
    pub enabled: bool,
    /// When the orbits kept were fetched, and their bytes.
    pub orbits: Option<(i64, usize)>,
    /// The power manager's time asleep, µs, which a sleep changes.
    pub asleep_us: u64,
}

struct Report {
    number: u32,
    path: PathBuf,
    began: Instant,
    /// The last look, which a report ended by a sleep ends at.
    seen: Instant,
    asleep_us: u64,
    next_sample: Duration,
    /// When the last satellites written were the receiver's.
    sampled: Option<Duration>,
    fix_at: Option<Duration>,
    summary: String,
    bytes: u64,
    failed: bool,
}

impl Report {
    fn write(&mut self, line: &str) {
        let wrote = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(&self.path)
            // One write, so a line is whole or missing.
            .and_then(|mut f| f.write_all(line.as_bytes()));
        match wrote {
            Ok(()) => self.bytes += line.len() as u64,
            Err(_) if !self.failed => {
                self.failed = true;
                log!(ESP_LOG_WARN, b"could not write GPS start %u to the card\0", self.number);
            }
            Err(_) => {}
        }
    }

    fn end(mut self, at: Instant, why: &str) {
        let t = at.saturating_duration_since(self.began);
        let line = end_line(t, why, self.fix_at, &self.summary);
        self.write(&line);
        let fix = match self.fix_at {
            Some(f) => format!("fix at {} s", f.as_secs()),
            None => format!("no fix in {} s", t.as_secs()),
        };
        let text = std::ffi::CString::new(format!("{fix}, ended {why}; {}", self.summary)).unwrap_or_default();
        log!(ESP_LOG_INFO, b"GPS start %u: %s\0", self.number, text.as_ptr());
    }
}

static REPORT: Mutex<Option<Report>> = Mutex::new(None);

/// Why a report running ends at this look, if it does.
fn ends_now(r: &Report, l: &Look) -> Option<&'static str> {
    if l.started {
        Some(if l.asleep_us != r.asleep_us { "by a sleep" } else { "by GPS switched on again" })
    } else if !l.gps_on {
        Some("by GPS switched off")
    } else if !l.enabled {
        Some("by config/assistnow.json")
    } else {
        None
    }
}

/// Start a report: its folder, the oldest removed to make room, its first
/// line. None without a card.
fn begin(l: &Look) -> Option<Report> {
    let dir = dir();
    if std::fs::create_dir_all(&dir).is_err() {
        static SAID: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
        if !SAID.swap(true, std::sync::atomic::Ordering::Relaxed) {
            log!(ESP_LOG_WARN, b"no card: GPS starts are not reported\0");
        }
        return None;
    }
    let names: Vec<String> = std::fs::read_dir(&dir)
        .map(|entries| entries.flatten().filter_map(|e| e.file_name().into_string().ok()).collect())
        .unwrap_or_default();
    for name in pruned(&names, KEEP - 1) {
        let _ = std::fs::remove_file(dir.join(name));
    }
    let number = next_number(&names);
    let name = file_name(number, l.utc_s);
    let mut r = Report {
        number,
        path: dir.join(&name),
        began: l.now,
        seen: l.now,
        asleep_us: l.asleep_us,
        // The first ask polls the receiver; the next reads its answer.
        next_sample: Duration::ZERO,
        sampled: None,
        fix_at: None,
        summary: "no satellites listed".to_string(),
        bytes: 0,
        failed: false,
    };
    r.write(&start_line(number, l.utc_s, l.orbits));
    let name = std::ffi::CString::new(name).unwrap_or_default();
    log!(ESP_LOG_INFO, b"reporting this GPS start in gps/starts/%s\0", name.as_ptr());
    Some(r)
}

/// Once a second: start, write and end the report.
pub(crate) fn look(l: &Look, get_satellites: Option<unsafe extern "C" fn(*mut HalGpsSats) -> i32>) {
    let Some(get) = get_satellites else {
        return;
    };
    let mut guard = REPORT.lock().unwrap_or_else(PoisonError::into_inner);
    if let Some(why) = guard.as_ref().and_then(|r| ends_now(r, l)) {
        if let Some(r) = guard.take() {
            // Ended by a restart, at the last look before it: a sleep's
            // time is no time spent looking for a fix.
            let at = if l.started { r.seen } else { l.now };
            r.end(at, why);
        }
    }
    if l.started && l.gps_on && l.enabled {
        *guard = begin(l);
    }
    let Some(r) = guard.as_mut() else {
        return;
    };
    r.seen = l.now;
    let t = l.now.saturating_duration_since(r.began);

    if let Some(aid) = l.handed {
        r.write(&handed_line(t, aid));
    }
    if let (Some(in_use), None) = (l.fix, r.fix_at) {
        r.fix_at = Some(t);
        r.write(&fix_line(t, in_use));
    }

    let mut why = None;
    if t >= r.next_sample {
        r.next_sample = t + SAMPLE_EVERY;
        let mut sats = HalGpsSats::EMPTY;
        match unsafe { get(&mut sats) } {
            ESP_OK => {
                let at = t.saturating_sub(Duration::from_millis(sats.age_ms as u64));
                // The same answer twice is a poll that went unanswered.
                if r.sampled.is_none_or(|before| at > before + Duration::from_millis(500)) {
                    if r.sampled.is_none() {
                        let text = std::ffi::CString::new(summary(sats.listed())).unwrap_or_default();
                        log!(ESP_LOG_INFO, b"GPS start %u, at %u s: %s\0", r.number, at.as_secs() as u32, text.as_ptr());
                    }
                    r.sampled = Some(at);
                    r.summary = summary(sats.listed());
                    r.write(&sats_line(at, sats.listed()));
                }
            }
            ESP_ERR_NOT_FOUND => {}
            _ => why = Some("as nothing reads the receiver"),
        }
    }
    let why = why.or_else(|| {
        if r.fix_at.is_some_and(|f| t >= f + AFTER_FIX) {
            Some("30 s after the fix")
        } else if r.fix_at.is_none() && t >= LONGEST {
            Some("after 30 minutes without one")
        } else if r.bytes >= MAX_BYTES {
            Some("at 2 MB")
        } else {
            None
        }
    });
    if let Some(why) = why {
        if let Some(r) = guard.take() {
            r.end(l.now, why);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sat(gnss: u8, sv: u8, cno: u8, used: bool, orbits: u8) -> HalGpsSat {
        HalGpsSat {
            gnss,
            sv,
            cno,
            elev: 45,
            azim: 123,
            quality: if cno > 0 { 4 } else { 1 },
            orbit: if used { HAL_GPS_ORBIT_PREDICTED } else { HAL_GPS_ORBIT_ALMANAC },
            orbits,
            health: 1,
            used,
            reserved: 0,
        }
    }

    const EP: u8 = HAL_GPS_HAS_EPHEMERIS | HAL_GPS_HAS_PREDICTED;

    #[test]
    fn a_satellite_is_a_row_of_the_columns() {
        let mut s = sat(b'G', 7, 38, true, EP | HAL_GPS_HAS_ALMANAC);
        let line = sats_line(Duration::from_millis(5049), &[s]);
        let v: Value = serde_json::from_str(&line).unwrap();
        assert_eq!(v["t"], 5.0);
        assert_eq!((v["heard"].as_u64(), v["used"].as_u64()), (Some(1), Some(1)));
        assert_eq!(v["sats"][0], json!(["G07", 38, 45, 123, 4, 1, 1, "pred", "eph+alm+pred"]));
        assert_eq!(v["sats"][0].as_array().unwrap().len(), COLUMNS.len());

        (s.elev, s.azim, s.orbits, s.orbit) = (-128, -1, 0, HAL_GPS_ORBIT_NONE);
        let v: Value = serde_json::from_str(&sats_line(Duration::ZERO, &[s, s])).unwrap();
        assert_eq!(v["sats"][1], json!(["G07", 38, null, null, 4, 1, 1, "-", ""]));
    }

    #[test]
    fn the_start_names_what_the_columns_and_codes_mean() {
        let v: Value = serde_json::from_str(&start_line(42, Some(1_792_582_503), Some((1_792_540_800, 50_456)))).unwrap();
        assert_eq!(v["utc"], "2026-10-21T11:35:03Z");
        assert_eq!(v["orbits_fetched"], "2026-10-21T00:00:00Z");
        assert_eq!(v["orbit_bytes"], 50_456);
        assert_eq!(v["columns"][0], "sv");
        assert!(v["orbit"]["auto"].as_str().unwrap().contains("Autonomous"));
        let v: Value = serde_json::from_str(&start_line(1, None, None)).unwrap();
        assert!(v["utc"].is_null() && v["orbits_fetched"].is_null());
    }

    #[test]
    fn what_was_handed_is_written_as_null_where_it_was_not() {
        let mut aid = HalGpsAssist {
            utc_us: 1,
            utc_acc_ms: 1200,
            has_position: false,
            latitude: 59.3,
            longitude: 18.1,
            altitude_m: 20.0,
            position_acc_m: 32_000,
            data: std::ptr::null(),
            data_len: 50_456,
        };
        let v: Value = serde_json::from_str(&handed_line(Duration::from_secs(1), &aid)).unwrap();
        assert_eq!((v["time_acc_ms"].as_u64(), v["orbit_bytes"].as_u64()), (Some(1200), Some(50_456)));
        assert!(v["position_acc_m"].is_null());
        assert!(!handed_line(Duration::ZERO, &aid).contains("59.3"), "no position");
        (aid.utc_us, aid.has_position) = (0, true);
        let v: Value = serde_json::from_str(&handed_line(Duration::ZERO, &aid)).unwrap();
        assert!(v["time_acc_ms"].is_null());
        assert_eq!(v["position_acc_m"], 32_000);
        assert!(!handed_line(Duration::ZERO, &aid).contains("59.3"), "the accuracy, not the place");
    }

    #[test]
    fn the_summary_says_each_constellation_heard_used_and_its_orbits() {
        let sats = [
            sat(b'G', 1, 40, true, EP),
            sat(b'G', 2, 30, false, HAL_GPS_HAS_PREDICTED),
            sat(b'G', 3, 0, false, HAL_GPS_HAS_ALMANAC),
            sat(b'E', 5, 35, false, HAL_GPS_HAS_ALMANAC | HAL_GPS_HAS_AUTONOMOUS),
        ];
        assert_eq!(
            summary(&sats),
            "GPS 2 heard, 1 used, orbits: 1 decoded, 2 predicted, 0 autonomous; \
             Galileo 1 heard, 0 used, orbits: 0 decoded, 0 predicted, 1 autonomous"
        );
        assert_eq!(summary(&[]), "no satellites listed");
    }

    #[test]
    fn files_are_numbered_and_named_by_their_start() {
        assert_eq!(file_name(42, Some(1_792_582_503)), "0042-20261021-113503.jsonl");
        assert_eq!(file_name(7, None), "0007.jsonl");
        let names: Vec<String> =
            ["0003-20261021-113503.jsonl", "0012.jsonl", "notes.txt", "x-1.jsonl", "0001.jsonl"]
                .map(String::from)
                .to_vec();
        assert_eq!(next_number(&names), 13);
        assert_eq!(next_number(&[]), 1);
        assert_eq!(pruned(&names, 1), ["0001.jsonl", "0003-20261021-113503.jsonl"]);
        assert!(pruned(&names, 20).is_empty());
    }

    #[test]
    fn the_end_says_why_and_when_the_fix_came() {
        let v: Value = serde_json::from_str(&end_line(
            Duration::from_secs(125),
            "30 s after the fix",
            Some(Duration::from_secs(95)),
            "GPS 9 heard",
        ))
        .unwrap();
        assert_eq!((v["t"].as_f64(), v["fix_at"].as_f64()), (Some(125.0), Some(95.0)));
        assert_eq!(v["summary"], "GPS 9 heard");
        let v: Value = serde_json::from_str(&end_line(Duration::ZERO, "by a sleep", None, "")).unwrap();
        assert!(v["fix_at"].is_null());
    }

    fn report(began: Instant) -> Report {
        Report {
            number: 1,
            path: PathBuf::new(),
            began,
            seen: began,
            asleep_us: 3,
            next_sample: Duration::ZERO,
            sampled: None,
            fix_at: None,
            summary: String::new(),
            bytes: 0,
            failed: false,
        }
    }

    fn looking(now: Instant) -> Look<'static> {
        Look {
            now,
            utc_s: None,
            started: false,
            gps_on: true,
            fix: None,
            handed: None,
            enabled: true,
            orbits: None,
            asleep_us: 3,
        }
    }

    #[test]
    fn a_restart_ends_a_report_saying_whether_it_was_a_sleep() {
        let now = Instant::now();
        let r = report(now);
        assert_eq!(ends_now(&r, &looking(now)), None);
        assert_eq!(ends_now(&r, &Look { started: true, ..looking(now) }), Some("by GPS switched on again"));
        assert_eq!(ends_now(&r, &Look { started: true, asleep_us: 4, ..looking(now) }), Some("by a sleep"));
        assert_eq!(ends_now(&r, &Look { gps_on: false, ..looking(now) }), Some("by GPS switched off"));
        assert_eq!(ends_now(&r, &Look { enabled: false, ..looking(now) }), Some("by config/assistnow.json"));
    }
}
