// SPDX-License-Identifier: BSD-3-Clause
//! AssistNow: what lets a u-blox receiver find itself in seconds, where a
//! cold start reads the satellites' orbits off them first, half a minute
//! under an open sky and never under a poor one.
//!
//! u-blox's free service, Predictive Orbits, hands a registered receiver the
//! almanacs and a week of predicted orbits. With those, the time and where it
//! last was, the receiver starts as if it had been running. Live Orbits, the
//! paid service, is not used.
//!
//! - **Registration** is once per receiver: its UBX-SEC-UNIQID and
//!   UBX-MON-VER replies (the driver's `get_identity`), with the token of a
//!   device profile in the user's Thingstream account, posted to
//!   Thingstream, which answers a chipcode. The token is `ASSISTNOW_ZTP_TOKEN`
//!   in the repository's .env, built in, or `ztp_token` in the card's
//!   config/assistnow.json, which wins; `"enabled": false` there leaves
//!   the service alone, its data unused. Without a token nothing is asked
//!   of anyone. The time and the last fix are handed over either way.
//! - **The data** is fetched with the chipcode about once a day, while GPS is
//!   on and there is a network: the almanacs and seven days of orbits for
//!   GPS, Galileo and GLONASS, about 50 KB. Thingstream allows a device on a commercial
//!   profile two requests a day, on an evaluation profile 300 a month. No
//!   position goes with it: Predictive Orbits needs none.
//! - **Kept on SPIFFS** with the chipcode and the last fix (assistnow.json,
//!   assistnow.ubx), so a week away from any network still starts quickly.
//! - **Handed to the receiver** (the driver's `assist`) whenever it starts:
//!   at boot, when GPS is switched on, and after a sleep, unless it had a fix
//!   in the last two hours and so still holds what it needs; and again when
//!   new data comes while it has no fix, or a much better time. The driver
//!   sends what the receiver takes of it: the almanacs and today's GPS and
//!   GLONASS orbits.
//!
//! The decisions are the pure functions at the top, tested on the host. A
//! kernel worker job looks once a second, and has `gps_report.rs` report
//! each start on the card; the requests, which wait on the network, run on
//! a task of their own.

use std::ffi::{c_char, c_void};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, OnceLock, PoisonError};
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::hal_registry::{registry, HalGpsAssist, HalGpsIdentity, HalGpsPosition};
use crate::time_policy::{Source, Sync};

const ESP_OK: i32 = 0;

const ZTP_URL: &str = "https://api.thingstream.io/ztp/assistnow/credentials";
const SERVICE_URL: &str = "https://assistnow.services.u-blox.com/GetAssistNowData.ashx";
/// The constellations asked for. The driver has the receiver track GPS,
/// Galileo, GLONASS and BeiDou; it takes orbit predictions for GPS and
/// GLONASS (and needs GLONASS's almanac to use its), and almanacs for all.
/// BeiDou's are left out: its predictions are 23 KB a week the receiver
/// would not take.
pub(crate) const GNSS: &str = "gps,gal,glo";
/// The prediction asked for, in days, of those a profile allows.
const DAYS_WANTED: [u32; 4] = [7, 14, 3, 1];
/// How old the data may be before it is fetched again. A little under a
/// day, so the hour of the fetch does not creep later each day.
const REFRESH_S: i64 = 20 * 3600;
/// The largest answer taken: fourteen days of every constellation is 135 KB.
const MAX_DATA: usize = 256 * 1024;
const MAX_REPLY: usize = 4096;
/// The longest a failure is waited out: to the next UTC day, at most.
const MAX_WAIT_S: i64 = 86_400 + 300;
const TIMEOUT_MS: i32 = 20_000;

/// A receiver that had a fix this recently still holds its orbits.
const FRESH_FIX: Duration = Duration::from_secs(2 * 3600);
/// How long a receiver's start waits for the clock before it is handed what
/// there is without it.
const CLOCK_WAIT: Duration = Duration::from_secs(60);
/// How often the last fix is written while fixes keep coming.
const SAVE_FIX_EVERY_S: i64 = 30 * 60;
/// How often the card's config is read again while there is no token, so
/// one put there needs no restart.
const CONFIG_AGAIN: Duration = Duration::from_secs(10 * 60);
/// How good the time handed over is. A receiver told the time is better
/// than it is starts worse than one told nothing (the interface
/// description, of UBX-MGA-INI-TIME_UTC), so each part is the most it may
/// be: what the source is good to (`source_acc_us`), the time between the
/// clock being read and the receiver taking it, and the drift since the
/// source last confirmed the clock: 50 ppm awake, on the crystal, and 1 %
/// asleep, on the RC oscillator, which a 20 s deep sleep once left 0.7 %
/// out (ticket 17). Past an hour the time is left out.
const HANDOVER_US: i64 = 500_000;
const AWAKE_DRIFT_PER: i64 = 20_000;
const ASLEEP_DRIFT_PER: i64 = 100;
const TIME_MAX_ACC_MS: i64 = 3_600_000;
/// Where the device may have gone since its last fix: as far as a car on a
/// motorway, 100 km an hour. Past 3000 km the position is left out.
const POSITION_BASE_ACC_M: i64 = 1000;
const POSITION_MAX_ACC_M: i64 = 3_000_000;

/// The task that registers and fetches: TLS needs most of it.
const STACK_SIZE: usize = 8192;

const US: i64 = 1_000_000;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

#[cfg(not(test))]
static TAG: &[u8] = b"assistnow\0";

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
// Decisions
// ---------------------------------------------------------------------------

/// What a source's time is good to, µs; None for one not known.
fn source_acc_us(source: Source) -> Option<i64> {
    match source {
        Source::Ntp => Some(100_000),
        // Whole seconds, as time_policy reckons them.
        Source::Gnss | Source::Network | Source::Rtc => Some(2 * US),
        // Typed, perhaps to the minute.
        Source::Manual => Some(60 * US),
        Source::None | Source::Kept => None,
    }
}

/// How far off the clock may be, in ms, by what last confirmed it and how
/// much of the time since was spent asleep (all of it when that is not
/// known); None when the source is unknown or too long ago to help. The
/// clock is taken with what is still to be slewed in added.
pub(crate) fn time_accuracy_ms(now_us: i64, last: Option<Sync>, asleep_us: Option<i64>) -> Option<u32> {
    let last = last?;
    let age = (now_us - last.at_us).max(0);
    let asleep = asleep_us.map_or(age, |a| a.clamp(0, age));
    let acc_us = source_acc_us(last.source)?
        + HANDOVER_US
        + (age - asleep) / AWAKE_DRIFT_PER
        + asleep / ASLEEP_DRIFT_PER;
    let acc_ms = (acc_us + 999) / 1000;
    (acc_ms <= TIME_MAX_ACC_MS).then_some(acc_ms as u32)
}

/// How far from a fix `age_s` old the device may be, in m; None past use.
pub(crate) fn position_accuracy_m(age_s: i64) -> Option<u32> {
    let acc = POSITION_BASE_ACC_M + age_s.max(0) * 1000 / 36;
    (acc <= POSITION_MAX_ACC_M).then_some(acc as u32)
}

/// Seconds to wait after the `failures`th failure in a row: 15 minutes,
/// then four times as long each time, up to 12 hours.
pub(crate) fn backoff_s(failures: u32) -> i64 {
    let quarter_hours = 4i64.saturating_pow(failures.saturating_sub(1).min(8));
    (quarter_hours * 15 * 60).min(12 * 3600)
}

/// A few minutes into the next UTC day, when Thingstream's daily count
/// starts again.
pub(crate) fn next_utc_day_s(now_s: i64) -> i64 {
    (now_s.div_euclid(86_400) + 1) * 86_400 + 300
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Inputs {
    /// The time, if the clock is set: TLS checks certificates by it.
    pub now_s: Option<i64>,
    pub gps_on: bool,
    pub network: bool,
    pub busy: bool,
    pub token: bool,
    pub registered: bool,
    /// Whether the receiver's identity is at hand, for a registration.
    pub identity: bool,
    pub fetched_s: i64,
    /// The data kept is for other constellations than are asked for now.
    pub other_gnss: bool,
    pub retry_at_s: i64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Action {
    None,
    /// Ask the driver for the receiver's identity.
    Identify,
    Register,
    Fetch,
}

pub(crate) fn decide(i: &Inputs) -> Action {
    let Some(now) = i.now_s else {
        return Action::None;
    };
    // No wait is longer than a day; one further off was set by a clock
    // that was wrong then.
    let waiting = now < i.retry_at_s && i.retry_at_s - now <= MAX_WAIT_S;
    if !i.gps_on || !i.network || i.busy || !i.token || waiting {
        return Action::None;
    }
    if !i.registered {
        return if i.identity { Action::Register } else { Action::Identify };
    }
    // A clock set back since is no reason to wait.
    if i.fetched_s == 0 || i.other_gnss || now - i.fetched_s >= REFRESH_S || now < i.fetched_s {
        Action::Fetch
    } else {
        Action::None
    }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02X}")).collect()
}

/// `s` made safe in a URL's query: a chipcode is Base64, whose + / and =
/// mean other things there.
fn query_escape(s: &str) -> String {
    s.bytes()
        .map(|b| {
            if b.is_ascii_alphanumeric() || b"-_.~".contains(&b) {
                (b as char).to_string()
            } else {
                format!("%{b:02X}")
            }
        })
        .collect()
}

/// The registration's body.
pub(crate) fn ztp_body(token: &str, id: &HalGpsIdentity) -> String {
    let unique = &id.unique_id[..(id.unique_id_len as usize).min(id.unique_id.len())];
    let version = &id.version[..(id.version_len as usize).min(id.version.len())];
    json!({
        "token": token,
        "messages": { "UBX-SEC-UNIQID": hex(unique), "UBX-MON-VER": hex(version) },
    })
    .to_string()
}

/// What a registration gives, as kept.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Registration {
    pub chipcode: String,
    pub service_url: String,
    pub days: u32,
    pub almanac: bool,
}

/// Thingstream's answer to a registration: the chipcode, where to ask, and
/// of what the profile allows, the prediction to ask for. Err says why not.
pub(crate) fn ztp_answer(body: &str) -> Result<Registration, &'static str> {
    let v: Value = serde_json::from_str(body).map_err(|_| "the answer is not JSON")?;
    let chipcode = v["chipcode"].as_str().filter(|c| !c.is_empty()).ok_or("no chipcode")?;
    // A list in u-blox's guide; one string, "ualm, uporb_1, ...", from
    // the service itself (2026-10-21).
    let allowed: Vec<&str> = match &v["allowedData"] {
        Value::Array(items) => items.iter().filter_map(Value::as_str).map(str::trim).collect(),
        Value::String(list) => list.split(',').map(str::trim).collect(),
        _ => Vec::new(),
    };
    let days = DAYS_WANTED
        .iter()
        .copied()
        .find(|d| allowed.contains(&format!("uporb_{d}").as_str()))
        .ok_or("the profile allows no Predictive Orbits")?;
    Ok(Registration {
        chipcode: chipcode.to_string(),
        service_url: v["serviceUrl"].as_str().unwrap_or("").to_string(),
        days,
        almanac: allowed.contains(&"ualm"),
    })
}

/// An answer for the log, every credential in it blanked: what a profile
/// allows is worth seeing, its chipcode is not.
pub(crate) fn redacted(body: &str) -> String {
    fn blank(v: &mut Value) {
        match v {
            Value::Object(map) => {
                for (key, value) in map.iter_mut() {
                    let key = key.to_ascii_lowercase();
                    let secret = ["chip", "code", "token", "secret", "key", "password"]
                        .iter()
                        .any(|word| key.contains(word));
                    if secret && !value.is_array() && !value.is_object() {
                        *value = Value::from("...");
                    } else {
                        blank(value);
                    }
                }
            }
            Value::Array(items) => items.iter_mut().for_each(blank),
            _ => {}
        }
    }
    let text = match serde_json::from_str::<Value>(body) {
        Ok(mut v) => {
            blank(&mut v);
            v.to_string()
        }
        Err(_) => "(not JSON)".to_string(),
    };
    text.chars().filter(|c| !c.is_control()).take(400).collect()
}

/// The data's URL.
pub(crate) fn data_url(service: &str, chipcode: &str, days: u32, almanac: bool) -> String {
    format!(
        "{service}?chipcode={}&gnss={GNSS}&data=uporb_{days}{}",
        query_escape(chipcode),
        if almanac { ",ualm" } else { "" }
    )
}

/// How many assistance messages (class UBX-MGA) `data` holds whole.
pub(crate) fn mga_messages(data: &[u8]) -> usize {
    let mut at = 0;
    let mut count = 0;
    while at + 8 <= data.len() && data[at] == 0xB5 && data[at + 1] == 0x62 {
        let end = at + 8 + u16::from_le_bytes([data[at + 4], data[at + 5]]) as usize;
        if end > data.len() {
            break;
        }
        count += (data[at + 2] == 0x13) as usize;
        at = end;
    }
    count
}

/// What the receiver was handed, for the log: "the time to 10 s, its last
/// fix to 2 km, 45056 bytes of orbits and almanacs".
pub(crate) fn handed_line(aid: &HalGpsAssist) -> String {
    let time = if aid.utc_us > 0 {
        format!("the time to {} s", aid.utc_acc_ms.div_ceil(1000))
    } else {
        "no time".to_string()
    };
    let position = if aid.has_position {
        format!("its last fix to {} km", aid.position_acc_m.div_ceil(1000))
    } else {
        "no position".to_string()
    };
    let data = match aid.data_len {
        0 => "no orbits".to_string(),
        n => format!("{n} bytes of orbits and almanacs"),
    };
    format!("{time}, {position}, {data}")
}

/// The UTC date of `secs`, "27 Oct".
fn day_month(secs: i64) -> String {
    const MONTHS: [&str; 12] =
        ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    let (_, month, day) = civil_from_days(secs.div_euclid(86_400));
    format!("{day} {}", MONTHS[(month as usize - 1) % 12])
}

/// Days since 1970-01-01 to (year, month, day). Howard Hinnant's algorithm.
pub(crate) fn civil_from_days(days: i64) -> (i64, u32, u32) {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let month = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (yoe + era * 400 + if month <= 2 { 1 } else { 0 }, month, day)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Doing {
    Idle,
    Registering,
    Fetching,
}

/// What Settings shows, "Orbits: until 27 Oct".
pub(crate) fn describe(
    supported: bool,
    token: bool,
    doing: Doing,
    fetched_s: i64,
    days: u32,
    now_s: Option<i64>,
    failure: &str,
) -> String {
    if !supported {
        return "Orbits: --".to_string();
    }
    if !token {
        return "Orbits: no AssistNow token".to_string();
    }
    match doing {
        Doing::Registering => return "Orbits: registering".to_string(),
        Doing::Fetching => return "Orbits: downloading".to_string(),
        Doing::Idle => {}
    }
    if fetched_s > 0 {
        let last_day = fetched_s + (days.max(1) as i64 - 1) * 86_400;
        let expired = now_s.is_some_and(|now| now.div_euclid(86_400) > last_day.div_euclid(86_400));
        let word = if expired { "expired" } else { "until" };
        return format!("Orbits: {word} {}", day_month(last_day));
    }
    if !failure.is_empty() {
        return format!("Orbits: {failure}");
    }
    "Orbits: none yet".to_string()
}

// ---------------------------------------------------------------------------
// What is kept
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct Fix {
    pub lat: f64,
    pub lon: f64,
    pub alt: f32,
    pub at_s: i64,
}

/// assistnow.json.
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct Kept {
    /// "" until registered.
    pub chipcode: String,
    pub service_url: String,
    pub days: u32,
    pub almanac: bool,
    /// When assistnow.ubx was fetched; 0 for never.
    pub fetched_s: i64,
    /// The constellations it was fetched for, as GNSS has them.
    pub gnss: String,
    /// When a request may be made again after a failure: kept, so a device
    /// restarted again and again does not spend the day's two at once.
    pub retry_at_s: i64,
    pub fix: Option<Fix>,
}

pub(crate) fn kept_from(json: &str) -> Kept {
    let v: Value = serde_json::from_str(json).unwrap_or(Value::Null);
    let fix = &v["fix"];
    Kept {
        chipcode: v["chipcode"].as_str().unwrap_or("").to_string(),
        service_url: v["service_url"].as_str().unwrap_or("").to_string(),
        days: v["days"].as_u64().unwrap_or(1).clamp(1, 14) as u32,
        almanac: v["almanac"].as_bool().unwrap_or(true),
        fetched_s: v["fetched"].as_i64().unwrap_or(0),
        gnss: v["gnss"].as_str().unwrap_or("").to_string(),
        retry_at_s: v["retry_at"].as_i64().unwrap_or(0),
        fix: match (fix["lat"].as_f64(), fix["lon"].as_f64(), fix["at"].as_i64()) {
            (Some(lat), Some(lon), Some(at_s)) => Some(Fix {
                lat,
                lon,
                alt: fix["alt"].as_f64().unwrap_or(0.0) as f32,
                at_s,
            }),
            _ => None,
        },
    }
}

pub(crate) fn kept_json(k: &Kept) -> String {
    let mut v = json!({
        "chipcode": k.chipcode,
        "service_url": k.service_url,
        "days": k.days,
        "almanac": k.almanac,
        "fetched": k.fetched_s,
        "gnss": k.gnss,
        "retry_at": k.retry_at_s,
    });
    if let Some(f) = k.fix {
        v["fix"] = json!({ "lat": f.lat, "lon": f.lon, "alt": f.alt, "at": f.at_s });
    }
    serde_json::to_string_pretty(&v).unwrap_or_default()
}

/// The card's config/assistnow.json, which overrides what is built in.
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct Config {
    pub enabled: bool,
    /// Whether each start is reported on the card (`gps_report.rs`).
    pub report: bool,
    /// "" for none.
    pub token: String,
    pub ztp_url: String,
    pub service_url: String,
}

pub(crate) fn config_from(json: Option<&str>, built_in_token: Option<&str>) -> Config {
    let v: Value = json.and_then(|j| serde_json::from_str(j).ok()).unwrap_or(Value::Null);
    let text = |key: &str| v[key].as_str().map(str::trim).filter(|s| !s.is_empty()).map(String::from);
    Config {
        enabled: v["enabled"].as_bool().unwrap_or(true),
        report: v["report"].as_bool().unwrap_or(true),
        token: text("ztp_token")
            .or_else(|| built_in_token.map(str::trim).filter(|t| !t.is_empty()).map(String::from))
            .unwrap_or_default(),
        ztp_url: text("ztp_url").unwrap_or_else(|| ZTP_URL.to_string()),
        service_url: text("service_url").unwrap_or_default(),
    }
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

fn spiffs_root() -> PathBuf {
    if cfg!(target_os = "espidf") {
        PathBuf::from("/spiffs")
    } else {
        std::env::var("THISTLE_SIM_SPIFFS")
            .ok()
            .filter(|s| !s.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("/tmp/thistle_spiffs"))
    }
}

fn state_path() -> PathBuf {
    spiffs_root().join("assistnow.json")
}

fn data_path() -> PathBuf {
    spiffs_root().join("assistnow.ubx")
}

fn read_config() -> Config {
    let path = crate::appstore_client::card_root().join("config/assistnow.json");
    let json = std::fs::read_to_string(path).ok();
    config_from(json.as_deref(), option_env!("THISTLE_ASSISTNOW_TOKEN"))
}

/// SPIFFS has no folders; the simulator's is a real one.
fn make_root() {
    if !cfg!(target_os = "espidf") {
        let _ = std::fs::create_dir_all(spiffs_root());
    }
}

fn save_kept(k: &Kept) {
    make_root();
    if std::fs::write(state_path(), kept_json(k)).is_err() {
        log!(ESP_LOG_WARN, b"could not write assistnow.json\0");
    }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct State {
    config: Config,
    config_read: Instant,
    kept: Kept,
    data: Option<Arc<Vec<u8>>>,
    doing: Doing,
    failures: u32,
    /// Why the last request failed, for Settings: "HTTP 403".
    failure: String,
    identity: Option<Box<HalGpsIdentity>>,
    /// A receiver's start waiting for its assistance, since when.
    want_since: Option<Instant>,
    /// New data came: a receiver without a fix gets it at once.
    new_data: bool,
    /// When the receiver last had a fix, this boot.
    fix_seen: Option<Instant>,
    fix_saved_s: i64,
    /// The last sync of the clock seen, and the power manager's sleep count
    /// then; None for the count when there has been a sleep or a restart
    /// since that this boot did not see the start of.
    sync_mark: Option<(i64, Option<u64>)>,
    /// The time this module started: a sync before it was a boot ago.
    started_us: i64,
    /// What the time handed over last was good to, ms (u32::MAX for no
    /// time), until the receiver has a fix: a much better time, from NTP
    /// coming after the boot's hand-over, is handed over again.
    handed_time_ms: Option<u32>,
}

static STATE: Mutex<Option<State>> = Mutex::new(None);
/// Set by `receiver_started` from wherever the receiver was started.
static STARTED: AtomicBool = AtomicBool::new(false);

fn with<R>(f: impl FnOnce(&mut State) -> R) -> Option<R> {
    STATE.lock().unwrap_or_else(PoisonError::into_inner).as_mut().map(f)
}

fn wall_us() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_micros().min(i64::MAX as u128) as i64)
        .unwrap_or(0)
}

/// The receiver was started: at boot, switched on, or woken after a sleep.
/// Its assistance is handed over at the next look.
pub(crate) fn receiver_started() {
    STARTED.store(true, Ordering::Relaxed);
}

// ---------------------------------------------------------------------------
// Requests, on a task of their own
// ---------------------------------------------------------------------------

enum Job {
    Register { url: String, token: String, identity: Box<HalGpsIdentity> },
    Fetch { url: String, days: u32 },
}

#[cfg(not(test))]
mod http {
    use std::ffi::{c_char, c_int, c_void, CString};

    type DataCb = unsafe extern "C" fn(*const u8, c_int, *mut c_void) -> i32;

    extern "C" {
        fn thistle_http_client_init(
            url: *const c_char,
            timeout_ms: c_int,
            data_cb: Option<DataCb>,
            user_data: *mut c_void,
        ) -> *mut c_void;
        fn thistle_http_client_set_post(
            client: *mut c_void,
            content_type: *const c_char,
            body: *const c_char,
            length: c_int,
        ) -> i32;
        fn thistle_http_client_perform(client: *mut c_void) -> i32;
        fn thistle_http_client_get_status_code(client: *mut c_void) -> c_int;
        fn thistle_http_client_cleanup(client: *mut c_void) -> i32;
    }

    pub(super) struct Reply {
        /// 0 when no answer came.
        pub status: i32,
        pub body: Vec<u8>,
        /// The answer was longer than asked for.
        pub too_long: bool,
    }

    struct Body {
        data: Vec<u8>,
        max: usize,
        too_long: bool,
    }

    unsafe extern "C" fn take(data: *const u8, len: c_int, user: *mut c_void) -> i32 {
        let body = &mut *(user as *mut Body);
        if !data.is_null() && len > 0 {
            let bytes = std::slice::from_raw_parts(data, len as usize);
            if body.data.len() + bytes.len() <= body.max {
                body.data.extend_from_slice(bytes);
            } else {
                body.too_long = true;
            }
        }
        0
    }

    /// GET `url`, or POST `json` to it.
    pub(super) fn request(url: &str, json: Option<&str>, max: usize, timeout_ms: i32) -> Reply {
        let mut body = Box::new(Body { data: Vec::new(), max, too_long: false });
        let mut reply = Reply { status: 0, body: Vec::new(), too_long: false };
        let Ok(c_url) = CString::new(url) else {
            return reply;
        };
        unsafe {
            let client = thistle_http_client_init(
                c_url.as_ptr(),
                timeout_ms,
                Some(take),
                &mut *body as *mut Body as *mut c_void,
            );
            if client.is_null() {
                return reply;
            }
            let posted = match json {
                Some(json) => {
                    thistle_http_client_set_post(
                        client,
                        b"application/json\0".as_ptr() as *const c_char,
                        json.as_ptr() as *const c_char,
                        json.len() as c_int,
                    ) == 0
                }
                None => true,
            };
            if posted && thistle_http_client_perform(client) == 0 {
                reply.status = thistle_http_client_get_status_code(client);
            }
            thistle_http_client_cleanup(client);
        }
        reply.body = std::mem::take(&mut body.data);
        reply.too_long = body.too_long;
        reply
    }
}

#[cfg(test)]
mod http {
    pub(super) struct Reply {
        pub status: i32,
        pub body: Vec<u8>,
        pub too_long: bool,
    }

    pub(super) fn request(_url: &str, _json: Option<&str>, _max: usize, _timeout_ms: i32) -> Reply {
        Reply { status: 0, body: Vec::new(), too_long: false }
    }
}

/// An answer's text, for the log: the first line, printable, short.
fn reply_text(body: &[u8]) -> std::ffi::CString {
    let text: String = String::from_utf8_lossy(body)
        .lines()
        .next()
        .unwrap_or("")
        .chars()
        .filter(|c| !c.is_control())
        .take(100)
        .collect();
    std::ffi::CString::new(text).unwrap_or_default()
}

fn spawn(job: Job) {
    if !start_task(job) {
        log!(ESP_LOG_WARN, b"could not start a task for the request\0");
        with(|s| {
            s.doing = Doing::Idle;
            s.failures += 1;
            s.kept.retry_at_s = wall_us() / US + backoff_s(s.failures);
        });
    }
}

/// Run `job` on a task of its own, gone when done (kernel_task).
fn start_task(job: Job) -> bool {
    const PRIORITY: u32 = 4;
    // The simulator's HTTP client (curl) wants more than a device's.
    let stack = if cfg!(target_os = "espidf") { STACK_SIZE } else { STACK_SIZE * 4 };
    crate::kernel_task::spawn(c"assistnow", stack, PRIORITY, move || run(job))
}

fn failed(status: i32, retry_at_s: i64) {
    with(|s| {
        s.failures += 1;
        s.kept.retry_at_s = retry_at_s.max(wall_us() / US + backoff_s(s.failures));
        s.failure = if status > 0 { format!("failed, HTTP {status}") } else { "no answer".to_string() };
        save_kept(&s.kept);
    });
}

fn run(job: Job) {
    let _awake = crate::power_manager::HoldGuard::new(b"AssistNow\0");
    match job {
        Job::Register { url, token, identity } => register(&url, &token, &identity),
        Job::Fetch { url, days } => fetch(&url, days),
    }
    with(|s| s.doing = Doing::Idle);
}

fn register(url: &str, token: &str, identity: &HalGpsIdentity) {
    let reply = http::request(url, Some(&ztp_body(token, identity)), MAX_REPLY, TIMEOUT_MS);
    let text = String::from_utf8_lossy(&reply.body);
    let answer = if reply.status == 200 || reply.status == 201 {
        ztp_answer(&text)
    } else {
        Err("")
    };
    match answer {
        Ok(reg) => {
            log!(ESP_LOG_INFO, b"this receiver is registered; orbits come %u days at a time\0", reg.days);
            with(|s| {
                s.kept.chipcode = reg.chipcode;
                s.kept.service_url = reg.service_url;
                s.kept.days = reg.days;
                s.kept.almanac = reg.almanac;
                s.kept.fetched_s = 0;
                s.kept.retry_at_s = 0;
                s.failures = 0;
                s.failure.clear();
                s.identity = None;
                save_kept(&s.kept);
            });
        }
        Err(why) => {
            if why.is_empty() {
                let text = reply_text(&reply.body);
                log!(ESP_LOG_WARN, b"registration refused, HTTP %d: %s\0", reply.status, text.as_ptr());
            } else {
                let why = std::ffi::CString::new(why).unwrap_or_default();
                let answer = std::ffi::CString::new(redacted(&text)).unwrap_or_default();
                log!(ESP_LOG_WARN, b"registration answer not understood: %s: %s\0", why.as_ptr(),
                     answer.as_ptr());
            }
            failed(reply.status, 0);
        }
    }
}

fn fetch(url: &str, days: u32) {
    let reply = http::request(url, None, MAX_DATA, TIMEOUT_MS);
    let now_s = wall_us() / US;
    let messages = mga_messages(&reply.body);
    if reply.status == 200 && !reply.too_long && messages > 0 {
        make_root();
        let written = std::fs::write(data_path(), &reply.body).is_ok();
        log!(ESP_LOG_INFO, b"%u days of orbits fetched: %u messages, %u bytes\0", days,
             messages as u32, reply.body.len() as u32);
        if !written {
            log!(ESP_LOG_WARN, b"could not write assistnow.ubx; kept only until a restart\0");
        }
        let data = Arc::new(reply.body);
        with(|s| {
            s.data = Some(data);
            s.kept.fetched_s = now_s;
            s.kept.gnss = GNSS.to_string();
            s.kept.retry_at_s = 0;
            s.new_data = true;
            s.failures = 0;
            s.failure.clear();
            save_kept(&s.kept);
        });
        return;
    }
    let text = reply_text(&reply.body);
    log!(ESP_LOG_WARN, b"fetch failed, HTTP %d: %s\0", reply.status, text.as_ptr());
    match reply.status {
        // The chipcode is not taken: register again.
        401 => {
            with(|s| {
                s.kept.chipcode.clear();
                save_kept(&s.kept);
            });
            failed(401, 0);
        }
        // Over the day's or the month's count: tomorrow, then.
        403 => failed(403, next_utc_day_s(now_s)),
        status => failed(status, 0),
    }
}

// ---------------------------------------------------------------------------
// The look, once a second
// ---------------------------------------------------------------------------

fn now_mono() -> Instant {
    static START: OnceLock<Instant> = OnceLock::new();
    START.get_or_init(Instant::now);
    Instant::now()
}

/// Whether a time good to `now_ms` is worth handing over again after one
/// good to `handed_ms`: one much better, where the first was seconds out.
pub(crate) fn better_time(handed_ms: u32, now_ms: Option<u32>) -> bool {
    now_ms.is_some_and(|now| handed_ms > 3000 && now < handed_ms / 2)
}

/// How much of the time since the clock's last sync was spent asleep, µs;
/// None when that is not known.
fn asleep_since_sync(s: &mut State, last: Option<Sync>) -> Option<i64> {
    let (slept, last_wake_us) = crate::power_manager::slept();
    let sync = last?;
    if s.sync_mark.map(|(at, _)| at) != Some(sync.at_us) {
        // A sync after the last wake has seen no sleep since; of one before
        // it, or before this boot, the sleep since is not known.
        let seen = sync.at_us >= s.started_us.max(last_wake_us);
        s.sync_mark = Some((sync.at_us, seen.then_some(slept)));
    }
    s.sync_mark.and_then(|(_, at)| at).map(|at| slept.saturating_sub(at) as i64)
}

/// The assistance for a receiver starting now.
fn aid_now(s: &State, now_us: i64, clock: bool, last: Option<Sync>, asleep_us: Option<i64>)
    -> (HalGpsAssist, Option<Arc<Vec<u8>>>) {
    let time_acc = if clock { time_accuracy_ms(now_us, last, asleep_us) } else { None };
    let fix = s.kept.fix.filter(|_| clock).and_then(|f| {
        position_accuracy_m(now_us / US - f.at_s).map(|acc| (f, acc))
    });
    // Switched off, the service's data is not used either; the time and the
    // last fix are the device's own.
    let data = s.data.clone().filter(|_| s.config.enabled);
    let aid = HalGpsAssist {
        utc_us: if time_acc.is_some() { now_us + crate::time_manager::slew_left_us() } else { 0 },
        utc_acc_ms: time_acc.unwrap_or(0),
        has_position: fix.is_some(),
        latitude: fix.map_or(0.0, |(f, _)| f.lat),
        longitude: fix.map_or(0.0, |(f, _)| f.lon),
        altitude_m: fix.map_or(0.0, |(f, _)| f.alt),
        position_acc_m: fix.map_or(0, |(_, acc)| acc),
        data: data.as_ref().map_or(std::ptr::null(), |d| d.as_ptr()),
        data_len: data.as_ref().map_or(0, |d| d.len()),
    };
    (aid, data)
}

unsafe extern "C" fn tick(_arg: *mut c_void) {
    let reg = registry();
    let Some(drv) = (unsafe { reg.gps.as_ref() }) else {
        return;
    };
    let Some(assist) = drv.assist else {
        return;
    };
    let now_us = wall_us();
    let clock = crate::time_policy::plausible(now_us);
    let now_s = clock.then_some(now_us / US);
    let mono = now_mono();
    let gps_on = crate::gps_manager::rs_gps_is_enabled();
    let started = STARTED.swap(false, Ordering::Relaxed);

    let mut pos = HalGpsPosition {
        latitude: 0.0,
        longitude: 0.0,
        altitude_m: 0.0,
        speed_kmh: 0.0,
        heading_deg: 0.0,
        satellites: 0,
        fix_valid: false,
        timestamp: 0,
    };
    let fix = gps_on
        && drv.get_position.is_some_and(|get| unsafe { get(&mut pos) } == ESP_OK)
        && pos.fix_valid;

    let mut guard = STATE.lock().unwrap_or_else(PoisonError::into_inner);
    let Some(s) = guard.as_mut() else {
        return;
    };

    if fix {
        s.fix_seen = Some(mono);
        if let Some(now) = now_s {
            s.kept.fix = Some(Fix { lat: pos.latitude, lon: pos.longitude, alt: pos.altitude_m, at_s: now });
            if now - s.fix_saved_s >= SAVE_FIX_EVERY_S {
                s.fix_saved_s = now;
                save_kept(&s.kept);
            }
        }
    }
    if !gps_on {
        s.want_since = None;
    } else {
        let holds_its_orbits = s.fix_seen.is_some_and(|t| mono.duration_since(t) < FRESH_FIX);
        if started && !holds_its_orbits {
            s.want_since.get_or_insert(mono);
        }
        if std::mem::take(&mut s.new_data) && !fix {
            s.want_since.get_or_insert(mono);
        }
    }

    // Each tick, so a sync is seen within a second of it, before any sleep.
    let last = crate::time_manager::last_sync();
    let asleep_us = asleep_since_sync(s, last);

    if fix {
        s.handed_time_ms = None;
    } else if let Some(handed) = s.handed_time_ms.filter(|_| gps_on && clock) {
        if better_time(handed, time_accuracy_ms(now_us, last, asleep_us)) {
            s.want_since.get_or_insert(mono);
        }
    }

    // A start waits a while for the clock, which the orbits need.
    let hand = s
        .want_since
        .is_some_and(|since| clock || mono.duration_since(since) >= CLOCK_WAIT);
    let handed = hand.then(|| {
        s.want_since = None;
        let (aid, data) = aid_now(s, now_us, clock, last, asleep_us);
        s.handed_time_ms = Some(if aid.utc_us > 0 { aid.utc_acc_ms } else { u32::MAX });
        (aid, data)
    });

    if s.config.token.is_empty() && mono.duration_since(s.config_read) >= CONFIG_AGAIN {
        s.config = read_config();
        s.config_read = mono;
    }

    let report_on = s.config.report;
    let orbits = s.data.as_ref().map(|d| (s.kept.fetched_s, d.len()));

    let mut job = None;
    let mut identify = false;
    if s.config.enabled && s.doing == Doing::Idle {
        let inputs = Inputs {
            now_s,
            gps_on,
            network: crate::net_manager::net_has_underlay_connection(),
            busy: false,
            token: !s.config.token.is_empty(),
            registered: !s.kept.chipcode.is_empty(),
            identity: s.identity.is_some(),
            fetched_s: s.kept.fetched_s,
            other_gnss: s.kept.fetched_s > 0 && s.kept.gnss != GNSS,
            retry_at_s: s.kept.retry_at_s,
        };
        match decide(&inputs) {
            Action::None => {}
            Action::Identify => identify = true,
            Action::Register => {
                s.config = read_config();
                if let Some(identity) = s.identity.clone().filter(|_| !s.config.token.is_empty()) {
                    s.doing = Doing::Registering;
                    job = Some(Job::Register {
                        url: s.config.ztp_url.clone(),
                        token: s.config.token.clone(),
                        identity,
                    });
                }
            }
            Action::Fetch => {
                let service = [&s.config.service_url, &s.kept.service_url]
                    .into_iter()
                    .find(|u| !u.is_empty())
                    .cloned()
                    .unwrap_or_else(|| SERVICE_URL.to_string());
                s.doing = Doing::Fetching;
                job = Some(Job::Fetch {
                    url: data_url(&service, &s.kept.chipcode, s.kept.days, s.kept.almanac),
                    days: s.kept.days,
                });
            }
        }
    }
    drop(guard);

    if let Some((aid, _)) = &handed {
        let ret = unsafe { assist(aid) };
        let line = std::ffi::CString::new(handed_line(aid)).unwrap_or_default();
        log!(ESP_LOG_INFO, b"handed to the receiver: %s (0x%x)\0", line.as_ptr(), ret);
    }
    crate::gps_report::look(
        &crate::gps_report::Look {
            now: mono,
            utc_s: now_s,
            started,
            gps_on,
            fix: fix.then_some(pos.satellites),
            handed: handed.as_ref().map(|(aid, _)| aid),
            enabled: report_on,
            orbits,
            asleep_us: crate::power_manager::slept().0,
        },
        drv.get_satellites,
    );
    drop(handed);
    if identify {
        if let Some(get) = drv.get_identity {
            let mut id = Box::new(HalGpsIdentity::EMPTY);
            if unsafe { get(&mut *id) } == ESP_OK {
                with(|s| s.identity = Some(id));
            }
        }
    }
    if let Some(job) = job {
        spawn(job);
    }
}

// ---------------------------------------------------------------------------
// Start, and Settings
// ---------------------------------------------------------------------------

/// Read what is kept and look once a second from now on. Called once by
/// kernel_init(), after the network coordinator has started.
pub(crate) fn start() {
    let reg = registry();
    let supported = unsafe { reg.gps.as_ref() }.is_some_and(|d| d.assist.is_some());
    let config = read_config();
    let kept = std::fs::read_to_string(state_path()).map(|j| kept_from(&j)).unwrap_or_default();
    let data = std::fs::read(data_path())
        .ok()
        .filter(|d| kept.fetched_s > 0 && mga_messages(d) > 0)
        .map(Arc::new);
    {
        let mut guard = STATE.lock().unwrap_or_else(PoisonError::into_inner);
        if guard.is_some() {
            return;
        }
        if supported {
            match (&data, config.token.is_empty(), config.enabled) {
                (_, _, false) => log!(ESP_LOG_INFO, b"switched off in config/assistnow.json\0"),
                (Some(d), _, true) => {
                    let day = std::ffi::CString::new(day_month(kept.fetched_s)).unwrap_or_default();
                    log!(ESP_LOG_INFO, b"orbits from %s, %u bytes\0", day.as_ptr(), d.len() as u32);
                }
                (None, true, true) => log!(ESP_LOG_INFO, b"no token: GPS starts without orbits\0"),
                (None, false, true) => {}
            }
        }
        *guard = Some(State {
            config,
            config_read: now_mono(),
            kept,
            data,
            doing: Doing::Idle,
            failures: 0,
            failure: String::new(),
            identity: None,
            want_since: None,
            new_data: false,
            fix_seen: None,
            fix_saved_s: 0,
            sync_mark: None,
            started_us: wall_us(),
            handed_time_ms: None,
        });
    }
    if supported {
        // The receiver started at boot, before this.
        if crate::gps_manager::rs_gps_is_enabled() {
            receiver_started();
        }
        crate::kworker::every(tick, std::ptr::null_mut(), Duration::from_secs(1));
    }
}

/// What Settings → GPS shows of it: "Orbits: until 27 Oct".
///
/// # Safety
/// `buf` must be null or hold `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn rs_gps_assist_describe(buf: *mut c_char, len: usize) {
    if buf.is_null() || len == 0 {
        return;
    }
    let supported = unsafe { registry().gps.as_ref() }.is_some_and(|d| d.assist.is_some());
    let now_us = wall_us();
    let now_s = crate::time_policy::plausible(now_us).then_some(now_us / US);
    let text = with(|s| {
        describe(
            supported,
            s.config.enabled && !s.config.token.is_empty(),
            s.doing,
            s.kept.fetched_s,
            s.kept.days,
            now_s,
            &s.failure,
        )
    })
    .unwrap_or_else(|| describe(false, false, Doing::Idle, 0, 0, None, ""));
    let n = text.len().min(len - 1);
    unsafe {
        std::ptr::copy_nonoverlapping(text.as_ptr(), buf as *mut u8, n);
        *buf.add(n) = 0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const NOW_S: i64 = 1_792_499_696; // 2026-10-20 12:34:56 UTC

    fn ready() -> Inputs {
        Inputs {
            now_s: Some(NOW_S),
            gps_on: true,
            network: true,
            busy: false,
            token: true,
            registered: true,
            identity: false,
            fetched_s: NOW_S - REFRESH_S,
            other_gnss: false,
            retry_at_s: 0,
        }
    }

    #[test]
    fn fetches_once_the_data_is_most_of_a_day_old() {
        assert_eq!(decide(&ready()), Action::Fetch);
        assert_eq!(decide(&Inputs { fetched_s: 0, ..ready() }), Action::Fetch);
        assert_eq!(decide(&Inputs { fetched_s: NOW_S - REFRESH_S + 1, ..ready() }), Action::None);
        assert_eq!(decide(&Inputs { fetched_s: NOW_S + 3600, ..ready() }), Action::Fetch, "clock set back");
        let fresh = Inputs { fetched_s: NOW_S - 60, ..ready() };
        assert_eq!(decide(&Inputs { other_gnss: true, ..fresh }), Action::Fetch, "for other constellations");
    }

    #[test]
    fn asks_nothing_without_a_token_a_network_the_clock_or_gps() {
        assert_eq!(decide(&Inputs { token: false, ..ready() }), Action::None);
        assert_eq!(decide(&Inputs { network: false, ..ready() }), Action::None);
        assert_eq!(decide(&Inputs { now_s: None, ..ready() }), Action::None);
        assert_eq!(decide(&Inputs { gps_on: false, ..ready() }), Action::None);
        assert_eq!(decide(&Inputs { busy: true, ..ready() }), Action::None);
        assert_eq!(decide(&Inputs { retry_at_s: NOW_S + 1, ..ready() }), Action::None);
    }

    /// A wait set while the clock was days ahead is no wait at all.
    #[test]
    fn a_wait_past_a_day_is_not_waited() {
        assert_eq!(decide(&Inputs { retry_at_s: NOW_S + MAX_WAIT_S, ..ready() }), Action::None);
        assert_eq!(decide(&Inputs { retry_at_s: NOW_S + 3 * 86_400, ..ready() }), Action::Fetch);
    }

    #[test]
    fn registers_first_once_the_receiver_has_said_who_it_is() {
        let fresh = Inputs { registered: false, fetched_s: 0, ..ready() };
        assert_eq!(decide(&fresh), Action::Identify);
        assert_eq!(decide(&Inputs { identity: true, ..fresh }), Action::Register);
    }

    #[test]
    fn failures_wait_longer_each_time() {
        assert_eq!(backoff_s(1), 15 * 60);
        assert_eq!(backoff_s(2), 3600);
        assert_eq!(backoff_s(3), 4 * 3600);
        assert_eq!(backoff_s(4), 12 * 3600);
        assert_eq!(backoff_s(40), 12 * 3600);
        assert_eq!(next_utc_day_s(NOW_S), 1_792_540_800 + 300);
    }

    fn synced(source: Source, age_s: i64) -> Option<Sync> {
        Some(Sync { source, at_us: (NOW_S - age_s) * US })
    }

    /// Awake since NTP, the clock is good to well under a second for hours.
    #[test]
    fn the_time_awake_since_ntp_is_good_to_a_second() {
        let now = NOW_S * US;
        assert_eq!(time_accuracy_ms(now, synced(Source::Ntp, 0), Some(0)), Some(600));
        assert_eq!(time_accuracy_ms(now, synced(Source::Ntp, 3600), Some(0)), Some(780));
        assert_eq!(time_accuracy_ms(now, synced(Source::Ntp, 86_400), Some(0)), Some(4920));
    }

    /// Asleep, it drifts on the RC oscillator, and a sleep not counted is
    /// taken for the whole time.
    #[test]
    fn the_time_asleep_counts_for_much_more() {
        let now = NOW_S * US;
        assert_eq!(time_accuracy_ms(now, synced(Source::Ntp, 3600), Some(1800 * US)), Some(18_690));
        assert_eq!(time_accuracy_ms(now, synced(Source::Ntp, 3600), None), Some(36_600));
        assert_eq!(time_accuracy_ms(now, synced(Source::Ntp, 3600), Some(7200 * US)), Some(36_600));
        assert_eq!(time_accuracy_ms(now, synced(Source::Ntp, 5 * 86_400), None), None, "past an hour off");
    }

    /// The boot's hand-over comes before NTP, with the time kept through
    /// the restart: NTP's is handed over again, a time good to a second
    /// where the first was not, but not one a little better.
    #[test]
    fn a_much_better_time_is_handed_over_again() {
        assert!(better_time(15_000, Some(700)));
        assert!(better_time(u32::MAX, Some(700)), "after none at all");
        assert!(!better_time(15_000, Some(9_000)));
        assert!(!better_time(2_500, Some(600)), "the first was good enough");
        assert!(!better_time(15_000, None));
    }

    #[test]
    fn the_time_is_as_good_as_its_source() {
        let now = NOW_S * US;
        assert_eq!(time_accuracy_ms(now, synced(Source::Gnss, 0), Some(0)), Some(2500));
        assert_eq!(time_accuracy_ms(now, synced(Source::Network, 0), Some(0)), Some(2500));
        assert_eq!(time_accuracy_ms(now, synced(Source::Manual, 0), Some(0)), Some(60_500));
        assert_eq!(time_accuracy_ms(now, synced(Source::Kept, 0), Some(0)), None);
        assert_eq!(time_accuracy_ms(now, None, Some(0)), None);
    }

    #[test]
    fn the_last_fix_counts_for_less_the_older_it_is() {
        assert_eq!(position_accuracy_m(0), Some(1000));
        assert_eq!(position_accuracy_m(3600), Some(101_000));
        assert_eq!(position_accuracy_m(29 * 3600), Some(2_901_000));
        assert_eq!(position_accuracy_m(31 * 3600), None);
    }

    fn identity() -> HalGpsIdentity {
        let mut id = HalGpsIdentity::EMPTY;
        id.unique_id[..4].copy_from_slice(&[0xB5, 0x62, 0x27, 0x03]);
        id.unique_id_len = 4;
        id.version[..2].copy_from_slice(&[0x0A, 0xFF]);
        id.version_len = 2;
        id
    }

    #[test]
    fn the_registration_sends_the_token_and_the_receivers_replies_as_hex() {
        let v: Value = serde_json::from_str(&ztp_body("tok", &identity())).unwrap();
        assert_eq!(v["token"], "tok");
        assert_eq!(v["messages"]["UBX-SEC-UNIQID"], "B5622703");
        assert_eq!(v["messages"]["UBX-MON-VER"], "0AFF");
    }

    #[test]
    fn the_registrations_answer_gives_the_chipcode_and_the_prediction() {
        let answer = r#"{"chipcode":"QkRC+/=","serviceUrl":"https://assistnow.services.u-blox.com/GetAssistNowData.ashx",
            "allowedData":["ualm","uporb_1","uporb_3","uporb_7","uporb_14","ulorb_l1"]}"#;
        assert_eq!(
            ztp_answer(answer),
            Ok(Registration {
                chipcode: "QkRC+/=".into(),
                service_url: SERVICE_URL.into(),
                days: 7,
                almanac: true,
            })
        );
        // As the service sends it.
        let real = r#"{"allowedData":"ualm, uporb_1, uporb_3, uporb_7, uporb_14, ulorb_l1, utime, ukion, usvht",
            "chipcode":"c","serviceUrl":"https://assistnow.services.u-blox.com/GetAssistNowData.ashx"}"#;
        assert_eq!(ztp_answer(real).map(|r| (r.days, r.almanac)), Ok((7, true)));
        let short = r#"{"chipcode":"c","allowedData":["uporb_1","uporb_3"]}"#;
        assert_eq!(ztp_answer(short).map(|r| (r.days, r.almanac)), Ok((3, false)));
        assert!(ztp_answer(r#"{"chipcode":"c","allowedData":["ulorb_l1"]}"#).is_err());
        assert!(ztp_answer(r#"{"allowedData":["uporb_7"]}"#).is_err());
        assert!(ztp_answer("<html>").is_err());
    }

    #[test]
    fn an_answer_is_logged_without_its_credentials() {
        let answer = r#"{"chipcode":"SECRET1","allowedData":["uporb_7"],"x":{"apiKey":"SECRET2","n":1}}"#;
        let logged = redacted(answer);
        assert!(!logged.contains("SECRET"), "{logged}");
        assert!(logged.contains("uporb_7") && logged.contains("\"n\":1"), "{logged}");
        assert_eq!(redacted("chipcode=SECRET"), "(not JSON)");
    }

    #[test]
    fn the_data_url_escapes_the_chipcode() {
        assert_eq!(
            data_url(SERVICE_URL, "Qk+R/C=", 7, true),
            format!("{SERVICE_URL}?chipcode=Qk%2BR%2FC%3D&gnss=gps,gal,glo&data=uporb_7,ualm")
        );
        assert!(data_url(SERVICE_URL, "c", 3, false).ends_with("&data=uporb_3"));
    }

    #[test]
    fn only_whole_mga_messages_count_as_data() {
        let mut data = vec![0xB5, 0x62, 0x13, 0x20, 2, 0, 1, 2, 0, 0];
        data.extend_from_slice(&[0xB5, 0x62, 0x05, 0x01, 0, 0, 0, 0]);
        assert_eq!(mga_messages(&data), 1);
        assert_eq!(mga_messages(b"Access is blocked"), 0);
        assert_eq!(mga_messages(&data[..8]), 0);
    }

    #[test]
    fn what_is_kept_reads_back() {
        let k = Kept {
            chipcode: "abc".into(),
            service_url: "https://x".into(),
            days: 7,
            almanac: true,
            fetched_s: NOW_S,
            gnss: GNSS.into(),
            retry_at_s: NOW_S + 900,
            fix: Some(Fix { lat: 59.5, lon: 18.25, alt: 30.0, at_s: NOW_S - 60 }),
        };
        assert_eq!(kept_from(&kept_json(&k)), k);
        assert_eq!(kept_from("not json"), Kept { days: 1, almanac: true, ..Kept::default() });
    }

    #[test]
    fn the_cards_config_wins_over_what_is_built_in() {
        let c = config_from(Some(r#"{"ztp_token": " card "}"#), Some("built"));
        assert_eq!((c.enabled, c.token.as_str(), c.ztp_url.as_str()), (true, "card", ZTP_URL));
        assert_eq!(config_from(None, Some("built")).token, "built");
        assert_eq!(config_from(None, Some("")).token, "");
        assert!(!config_from(Some(r#"{"enabled": false}"#), Some("built")).enabled);
        assert!(config_from(None, None).report, "starts are reported unless switched off");
        assert!(!config_from(Some(r#"{"report": false}"#), None).report);
    }

    #[test]
    fn the_log_says_what_the_receiver_was_handed() {
        let mut aid = HalGpsAssist {
            utc_us: NOW_S * US,
            utc_acc_ms: 10_500,
            has_position: true,
            latitude: 0.0,
            longitude: 0.0,
            altitude_m: 0.0,
            position_acc_m: 1500,
            data: std::ptr::null(),
            data_len: 45_056,
        };
        assert_eq!(handed_line(&aid), "the time to 11 s, its last fix to 2 km, 45056 bytes of orbits and almanacs");
        (aid.utc_us, aid.has_position, aid.data_len) = (0, false, 0);
        assert_eq!(handed_line(&aid), "no time, no position, no orbits");
    }

    #[test]
    fn settings_says_how_long_the_orbits_last() {
        let d = |token, doing, fetched, failure| describe(true, token, doing, fetched, 7, Some(NOW_S), failure);
        assert_eq!(d(false, Doing::Idle, 0, ""), "Orbits: no AssistNow token");
        assert_eq!(d(true, Doing::Registering, 0, ""), "Orbits: registering");
        assert_eq!(d(true, Doing::Idle, NOW_S, ""), "Orbits: until 26 Oct");
        assert_eq!(d(true, Doing::Idle, NOW_S - 7 * 86_400, ""), "Orbits: expired 19 Oct");
        assert_eq!(d(true, Doing::Idle, 0, "failed, HTTP 403"), "Orbits: failed, HTTP 403");
        assert_eq!(d(true, Doing::Idle, 0, ""), "Orbits: none yet");
        assert_eq!(describe(false, true, Doing::Idle, 0, 7, None, ""), "Orbits: --");
    }
}
