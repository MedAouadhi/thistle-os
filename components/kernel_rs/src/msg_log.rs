// SPDX-License-Identifier: BSD-3-Clause
//! The message history: every SMS and MeshCore message this device receives
//! or sends, appended to a file per conversation on the SD card.
//!
//!   /sdcard/messages/sms/+4917612345678.jsonl
//!   /sdcard/messages/mesh/public.jsonl
//!   /sdcard/messages/mesh/1a2b3c4d5e6f.jsonl    a contact, by the start of its key
//!
//! One JSON object per line, keys in alphabetical order, so a line cut short
//! by a pulled card spoils only itself:
//!
//!   {"dir":"in","from":"+4917612345678","sent":1760351230,"text":"Hallo","time":1760351234}
//!   {"dir":"out","text":"Hi!","time":1760351290,"to":"+4917612345678"}
//!
//! `time` is when this device logged it, by its own clock, and null while
//! that clock has not been set. `sent` is when the sender sent it, by the
//! sender's clock (MeshCore) or the SMS service centre's, when known.
//!
//! Messages are recorded where they enter and leave the kernel, so nothing
//! depends on an app being open. The Messenger switches recording off and on:
//! `thistle_os.messenger.save_history` in system.json, on unless switched
//! off. The files are plain text: anyone holding the card can read them.

use std::fs::OpenOptions;
use std::io::Write;
use std::os::raw::c_char;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::Mutex;

const SECTION: &str = "messenger";
const KEY: &str = "save_history";

/// The longest file name a conversation gets, before ".jsonl".
const NAME_MAX: usize = 40;

/// Before this (2024-01-01), the device's clock has not been set.
const CLOCK_SET_AFTER: u64 = 1_704_067_200;

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

#[cfg(not(test))]
macro_rules! warn {
    ($fmt:expr $(, $arg:expr)*) => {
        unsafe { esp_log_write(2, b"msg_log\0".as_ptr(), $fmt.as_ptr() $(, $arg)*) }
    };
}
#[cfg(test)]
macro_rules! warn {
    ($fmt:expr $(, $arg:expr)*) => {{
        let _ = ($fmt $(, $arg)*);
    }};
}

/// Where the history goes.
#[cfg(target_os = "espidf")]
const ROOT: &str = "/sdcard/messages";
#[cfg(not(target_os = "espidf"))]
const ROOT: &str = "/tmp/thistle_sdcard/messages";

/// The root as a C string, for apps to show.
#[cfg(target_os = "espidf")]
const ROOT_C: &[u8] = b"/sdcard/messages\0";
#[cfg(not(target_os = "espidf"))]
const ROOT_C: &[u8] = b"/tmp/thistle_sdcard/messages\0";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Transport {
    Sms,
    Mesh,
}

impl Transport {
    fn dir(self) -> &'static str {
        match self {
            Transport::Sms => "sms",
            Transport::Mesh => "mesh",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Direction {
    In,
    Out,
}

/// One message, as it goes into the history.
#[derive(Debug)]
pub(crate) struct Entry<'a> {
    pub transport: Transport,
    /// The conversation: a phone number, "public", or a contact's key prefix.
    pub conv: &'a str,
    pub dir: Direction,
    /// Who sent it, for one received; who it went to, for one sent.
    pub peer: &'a str,
    pub text: &'a str,
    /// When the sender sent it, by its clock, in seconds since 1970.
    pub sent: Option<u64>,
}

// ---------------------------------------------------------------------------
// The setting
// ---------------------------------------------------------------------------

const UNKNOWN: u8 = 0;
const ON: u8 = 1;
const OFF: u8 = 2;

/// The setting, read from system.json once and kept here after.
static ENABLED: AtomicU8 = AtomicU8::new(UNKNOWN);

/// History is kept unless switched off.
pub(crate) fn enabled_in(json: Option<&str>) -> bool {
    json.and_then(|j| crate::system_json::get(j, SECTION, KEY))
        .and_then(|v| v.as_bool())
        .unwrap_or(true)
}

pub(crate) fn enabled() -> bool {
    match ENABLED.load(Ordering::Relaxed) {
        ON => true,
        OFF => false,
        _ => {
            let json = std::fs::read_to_string(crate::system_json::path()).ok();
            let on = enabled_in(json.as_deref());
            ENABLED.store(if on { ON } else { OFF }, Ordering::Relaxed);
            on
        }
    }
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

/// Appends are one at a time: the mesh, the modem and apps all record.
static WRITING: Mutex<()> = Mutex::new(());
/// A missing card is reported once, not at every message.
static WARNED: AtomicBool = AtomicBool::new(false);

/// A conversation's file name: letters, digits, '+', '-' and '_' only, so a
/// sender's name cannot reach outside the directory.
pub(crate) fn file_name(conv: &str) -> String {
    let mut name: String = conv
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || matches!(c, '+' | '-' | '_') {
                c
            } else {
                '_'
            }
        })
        .take(NAME_MAX)
        .collect();
    if name.is_empty() {
        name.push_str("unknown");
    }
    name.push_str(".jsonl");
    name
}

/// The device's clock, or None while it has not been set. The call log
/// (call_log.rs) dates calls by the same rule.
pub(crate) fn now() -> Option<u64> {
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .ok()?
        .as_secs();
    (secs >= CLOCK_SET_AFTER).then_some(secs)
}

/// The line `entry` is written as, newline included.
pub(crate) fn line(entry: &Entry, time: Option<u64>) -> String {
    let mut obj = serde_json::Map::new();
    obj.insert("time".into(), time.into());
    let (dir, who) = match entry.dir {
        Direction::In => ("in", "from"),
        Direction::Out => ("out", "to"),
    };
    obj.insert("dir".into(), dir.into());
    obj.insert(who.into(), entry.peer.into());
    if let Some(sent) = entry.sent {
        obj.insert("sent".into(), sent.into());
    }
    obj.insert("text".into(), entry.text.into());
    let mut line = serde_json::Value::Object(obj).to_string();
    line.push('\n');
    line
}

/// Append `entry` under `root`, creating its directory if need be.
pub(crate) fn append(root: &Path, entry: &Entry, time: Option<u64>) -> std::io::Result<()> {
    let dir: PathBuf = root.join(entry.transport.dir());
    std::fs::create_dir_all(&dir)?;
    let mut file = OpenOptions::new()
        .create(true)
        .append(true)
        .open(dir.join(file_name(entry.conv)))?;
    // One write, so a line is never split by another.
    file.write_all(line(entry, time).as_bytes())
}

/// Record a message, if history is kept. A card that is missing or full
/// loses the line, with a warning; nothing else waits on it.
pub(crate) fn record(entry: &Entry) {
    // Tests exercise append(); they leave the simulator's card alone.
    if cfg!(test) || !enabled() {
        return;
    }
    let _one_at_a_time = WRITING.lock();
    match append(Path::new(ROOT), entry, now()) {
        Ok(()) => WARNED.store(false, Ordering::Relaxed),
        Err(_) => {
            if !WARNED.swap(true, Ordering::Relaxed) {
                warn!(b"cannot write the message history to %s; is there an SD card?\0",
                      ROOT_C.as_ptr());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Times
// ---------------------------------------------------------------------------

/// Days from 1970-01-01 to a date in the proleptic Gregorian calendar
/// (Howard Hinnant's days_from_civil).
fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let mp = (m + 9) % 12;
    let doy = (153 * mp + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

/// An SMS service centre time stamp, "yy/MM/dd,hh:mm:ss+zz" with the zone in
/// quarters of an hour (3GPP TS 27.005, <scts>), as seconds since 1970.
pub(crate) fn scts_to_unix(scts: &str) -> Option<u64> {
    let b = scts.as_bytes();
    if b.len() < 17 || b[2] != b'/' || b[5] != b'/' || b[8] != b',' || b[11] != b':' || b[14] != b':' {
        return None;
    }
    let num = |at: usize| -> Option<i64> {
        let s = scts.get(at..at + 2)?;
        s.bytes().all(|c| c.is_ascii_digit()).then(|| s.parse().ok())?
    };
    let (yy, mo, dd, hh, mi, ss) = (num(0)?, num(3)?, num(6)?, num(9)?, num(12)?, num(15)?);
    if !(1..=12).contains(&mo) || !(1..=31).contains(&dd) || hh > 23 || mi > 59 || ss > 60 {
        return None;
    }
    let zone_quarters = match b.get(17) {
        Some(&sign @ (b'+' | b'-')) => {
            let digits = scts.get(18..)?;
            let q: i64 = digits.parse().ok()?;
            if sign == b'-' { -q } else { q }
        }
        None => 0,
        _ => return None,
    };
    let local = days_from_civil(2000 + yy, mo, dd) * 86_400 + hh * 3600 + mi * 60 + ss;
    u64::try_from(local - zone_quarters * 15 * 60).ok()
}

// ---------------------------------------------------------------------------
// FFI — thistle/msg_log.h
// ---------------------------------------------------------------------------

/// Whether messages are being recorded.
#[no_mangle]
pub extern "C" fn rs_msg_log_enabled() -> bool {
    enabled()
}

/// Switch recording on or off, and remember it. The choice applies even if
/// it could not be saved, which the return says.
#[no_mangle]
pub extern "C" fn rs_msg_log_set_enabled(on: bool) -> i32 {
    ENABLED.store(if on { ON } else { OFF }, Ordering::Relaxed);
    crate::system_json::save(SECTION, KEY, serde_json::Value::Bool(on))
}

/// Whether the history's directory can be written: false without an SD card.
#[no_mangle]
pub extern "C" fn rs_msg_log_writable() -> bool {
    std::fs::create_dir_all(ROOT).is_ok()
}

/// Where the history goes, for apps to show.
#[no_mangle]
pub extern "C" fn rs_msg_log_dir() -> *const c_char {
    ROOT_C.as_ptr() as *const c_char
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_root(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("thistle_msg_log_{}_{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        dir
    }

    fn sms_in<'a>(from: &'a str, text: &'a str) -> Entry<'a> {
        Entry {
            transport: Transport::Sms,
            conv: from,
            dir: Direction::In,
            peer: from,
            text,
            sent: Some(1_760_351_230),
        }
    }

    #[test]
    fn history_is_kept_unless_switched_off() {
        assert!(enabled_in(None));
        assert!(enabled_in(Some(r#"{ "thistle_os": {} }"#)));
        assert!(enabled_in(Some(r#"{ "thistle_os": { "messenger": { "save_history": "no" } } }"#)));
        assert!(!enabled_in(Some(r#"{ "thistle_os": { "messenger": { "save_history": false } } }"#)));
    }

    #[test]
    fn a_line_is_one_json_object() {
        let l = line(&sms_in("+4917612345678", "Hallo \"du\"\nzweite Zeile ä€"), Some(1_760_351_234));
        assert!(l.ends_with('\n'));
        assert_eq!(l.matches('\n').count(), 1, "{l}");
        let v: serde_json::Value = serde_json::from_str(l.trim_end()).unwrap();
        assert_eq!(v["time"], 1_760_351_234u64);
        assert_eq!(v["dir"], "in");
        assert_eq!(v["from"], "+4917612345678");
        assert_eq!(v["sent"], 1_760_351_230u64);
        assert_eq!(v["text"], "Hallo \"du\"\nzweite Zeile ä€");
    }

    #[test]
    fn a_sent_message_names_who_it_went_to_and_an_unset_clock_is_null() {
        let e = Entry {
            transport: Transport::Mesh,
            conv: "public",
            dir: Direction::Out,
            peer: "Public",
            text: "hi all",
            sent: None,
        };
        let v: serde_json::Value = serde_json::from_str(line(&e, None).trim_end()).unwrap();
        assert!(v["time"].is_null());
        assert_eq!(v["dir"], "out");
        assert_eq!(v["to"], "Public");
        assert!(v.get("sent").is_none());
        assert!(v.get("from").is_none());
    }

    #[test]
    fn file_names_stay_inside_the_directory() {
        assert_eq!(file_name("+4917612345678"), "+4917612345678.jsonl");
        assert_eq!(file_name("1a2b3c4d5e6f"), "1a2b3c4d5e6f.jsonl");
        assert_eq!(file_name("../../config/system"), "______config_system.jsonl");
        assert_eq!(file_name("DHL Paket"), "DHL_Paket.jsonl");
        assert_eq!(file_name(""), "unknown.jsonl");
        assert_eq!(file_name(&"9".repeat(80)).len(), NAME_MAX + ".jsonl".len());
    }

    #[test]
    fn messages_append_to_their_conversation_file() {
        let root = temp_root("append");
        append(&root, &sms_in("+491701", "one"), Some(1_760_000_000)).unwrap();
        append(&root, &sms_in("+491701", "two"), Some(1_760_000_001)).unwrap();
        append(&root, &sms_in("+491702", "other"), None).unwrap();
        let mesh = Entry {
            transport: Transport::Mesh,
            conv: "1a2b3c4d5e6f",
            dir: Direction::In,
            peer: "Heltec V3",
            text: "hello",
            sent: None,
        };
        append(&root, &mesh, None).unwrap();

        let first = std::fs::read_to_string(root.join("sms/+491701.jsonl")).unwrap();
        let texts: Vec<String> = first
            .lines()
            .map(|l| serde_json::from_str::<serde_json::Value>(l).unwrap()["text"].as_str().unwrap().to_string())
            .collect();
        assert_eq!(texts, ["one", "two"]);
        assert!(root.join("sms/+491702.jsonl").exists());
        assert!(root.join("mesh/1a2b3c4d5e6f.jsonl").exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_service_centre_time_stamp_is_read_with_its_zone() {
        // 13 Oct 2025, 12:34:56 at UTC+2 (8 quarters) is 10:34:56 UTC.
        assert_eq!(scts_to_unix("25/10/13,12:34:56+08"), Some(1_760_351_696));
        assert_eq!(scts_to_unix("25/10/13,10:34:56+00"), Some(1_760_351_696));
        assert_eq!(scts_to_unix("25/10/13,06:34:56-16"), Some(1_760_351_696));
        // Leap day.
        assert_eq!(scts_to_unix("24/02/29,00:00:00+00"), Some(1_709_164_800));
        assert_eq!(scts_to_unix(""), None);
        assert_eq!(scts_to_unix("25/13/13,12:34:56+08"), None);
        assert_eq!(scts_to_unix("not a time stamp"), None);
    }
}
