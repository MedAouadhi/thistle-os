// SPDX-License-Identifier: BSD-3-Clause
//! The call log: every voice call this device placed or took, one JSON line
//! a call, appended to a file on the SD card, oldest first:
//!
//!   {"duration":63,"kind":"in","number":"+4917612345678","time":1760351234}
//!   {"duration":0,"kind":"missed","number":"","time":null}
//!
//! `kind` is `out`, `in` (answered), `missed` or `rejected`; an outgoing call
//! that never connected is `out` with a duration of 0. `time` is when the
//! call was placed or began ringing, by this device's clock, and null while
//! that clock has not been set, by the message history's rule (msg_log).
//! Keys are in alphabetical order and each line is written in one piece, so
//! a line cut short by a pulled card spoils only itself.
//!
//! The Phone app's Recents shows the newest CALL_LOG_KEEP calls. Those are
//! kept in memory, newest first, read from the end of the file the first
//! time anything asks: at most TAIL_BYTES of it, so a file years long costs
//! no more to open than a new one. The memory is one allocation of a little
//! over 4 KB, which puts it in PSRAM.
//!
//! Calls missed since the user last looked at Recents are counted here too,
//! for the status bar. The count is not saved: a restart forgets it.

use std::fs::OpenOptions;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, MutexGuard, PoisonError};

use crate::sms_manager::copy_c;

const ESP_OK: i32 = 0;
const ESP_FAIL: i32 = -1;

/// call.h: CALL_NUMBER_MAX, CALL_LOG_KEEP.
pub const CALL_NUMBER_MAX: usize = 33;
pub const CALL_LOG_KEEP: usize = 100;

/// How much of the end of the file is read for the calls kept in memory.
/// A line is about 70 bytes, so this holds more than CALL_LOG_KEEP.
const TAIL_BYTES: u64 = 16 * 1024;

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

#[cfg(not(test))]
macro_rules! warn {
    ($fmt:expr $(, $arg:expr)*) => {
        unsafe { esp_log_write(2, b"call_log\0".as_ptr(), $fmt.as_ptr() $(, $arg)*) }
    };
}
#[cfg(test)]
macro_rules! warn {
    ($fmt:expr $(, $arg:expr)*) => {{
        let _ = ($fmt $(, $arg)*);
    }};
}

/// The SD card's root. Tests have a directory of their own, so they leave
/// the simulator's card alone.
#[cfg(all(target_os = "espidf", not(test)))]
fn card_root() -> PathBuf {
    PathBuf::from("/sdcard")
}
#[cfg(all(not(target_os = "espidf"), not(test)))]
fn card_root() -> PathBuf {
    PathBuf::from("/tmp/thistle_sdcard")
}
#[cfg(test)]
fn card_root() -> PathBuf {
    std::env::temp_dir().join(format!("thistle_call_log_card_{}", std::process::id()))
}

fn file_path() -> PathBuf {
    card_root().join("phone").join("calls.jsonl")
}

// ---------------------------------------------------------------------------
// Entries
// ---------------------------------------------------------------------------

/// call_log_kind_t
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum Kind {
    Outgoing = 0,
    Incoming = 1,
    Missed = 2,
    Rejected = 3,
}

impl Kind {
    fn as_str(self) -> &'static str {
        match self {
            Kind::Outgoing => "out",
            Kind::Incoming => "in",
            Kind::Missed => "missed",
            Kind::Rejected => "rejected",
        }
    }

    fn parse(s: &str) -> Option<Kind> {
        match s {
            "out" => Some(Kind::Outgoing),
            "in" => Some(Kind::Incoming),
            "missed" => Some(Kind::Missed),
            "rejected" => Some(Kind::Rejected),
            _ => None,
        }
    }
}

/// call_log_entry_t, which is also how calls are kept in memory: 44 bytes
/// each, with no allocation of their own.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CCallLogEntry {
    /// NUL-terminated; "" when withheld.
    pub number: [u8; CALL_NUMBER_MAX],
    /// call_log_kind_t
    pub kind: u8,
    /// When it began, seconds since 1970, UTC; 0 if the clock was not set.
    pub time: u32,
    /// Connected time; 0 if it never was.
    pub duration_s: u32,
}

fn entry(kind: Kind, number: &str, time: Option<u64>, duration_s: u32) -> CCallLogEntry {
    let mut e = CCallLogEntry {
        number: [0; CALL_NUMBER_MAX],
        kind: kind as u8,
        time: time.and_then(|t| u32::try_from(t).ok()).unwrap_or(0),
        duration_s,
    };
    copy_c(&mut e.number, number);
    e
}

/// The line a call is written as, newline included.
pub(crate) fn line(kind: Kind, number: &str, time: Option<u64>, duration_s: u32) -> String {
    // serde_json's map keeps its keys sorted.
    let mut obj = serde_json::Map::new();
    obj.insert("duration".into(), duration_s.into());
    obj.insert("kind".into(), kind.as_str().into());
    obj.insert("number".into(), number.into());
    obj.insert("time".into(), time.into());
    let mut line = serde_json::Value::Object(obj).to_string();
    line.push('\n');
    line
}

/// A line read back; None for one that is cut short, not JSON, or of a kind
/// this does not know.
pub(crate) fn parse_line(line: &str) -> Option<CCallLogEntry> {
    let v: serde_json::Value = serde_json::from_str(line.trim()).ok()?;
    let kind = Kind::parse(v.get("kind")?.as_str()?)?;
    let number = v.get("number").and_then(|n| n.as_str()).unwrap_or("");
    let time = v.get("time").and_then(|t| t.as_u64());
    let duration = v
        .get("duration")
        .and_then(|d| d.as_u64())
        .map_or(0, |d| u32::try_from(d).unwrap_or(u32::MAX));
    Some(entry(kind, number, time, duration))
}

/// Room for CALL_LOG_KEEP calls and the one being added: one allocation
/// over 4 KB, which ESP-IDF places in PSRAM.
fn new_list() -> Vec<CCallLogEntry> {
    Vec::with_capacity(CALL_LOG_KEEP + 1)
}

/// The newest CALL_LOG_KEEP calls in the file at `path`, newest first, from
/// its last TAIL_BYTES. A line cut by where the read began, and any line
/// that does not parse, is skipped.
pub(crate) fn read_tail(path: &Path) -> Vec<CCallLogEntry> {
    let mut list = new_list();
    let Ok(mut file) = std::fs::File::open(path) else {
        return list;
    };
    let len = file.metadata().map(|m| m.len()).unwrap_or(0);
    let start = len.saturating_sub(TAIL_BYTES);
    if file.seek(SeekFrom::Start(start)).is_err() {
        return list;
    }
    let mut bytes = Vec::with_capacity((len - start) as usize + 1);
    if file.take(TAIL_BYTES).read_to_end(&mut bytes).is_err() {
        return list;
    }
    let mut text = &bytes[..];
    if start > 0 {
        // The read began inside a line, most likely.
        match text.iter().position(|&b| b == b'\n') {
            Some(i) => text = &text[i + 1..],
            None => return list,
        }
    }
    for raw in text.rsplit(|&b| b == b'\n') {
        if list.len() == CALL_LOG_KEEP {
            break;
        }
        let Ok(line) = std::str::from_utf8(raw) else {
            continue;
        };
        if let Some(e) = parse_line(line) {
            list.push(e);
        }
    }
    list
}

// ---------------------------------------------------------------------------
// The log in memory
// ---------------------------------------------------------------------------

struct Log {
    /// The file's tail has been read.
    loaded: bool,
    /// Newest first, at most CALL_LOG_KEEP.
    entries: Vec<CCallLogEntry>,
    seq: u32,
    missed: u32,
}

static LOG: Mutex<Log> = Mutex::new(Log {
    loaded: false,
    entries: Vec::new(),
    seq: 0,
    missed: 0,
});

/// A missing card is reported once, not at every call.
static WARNED: AtomicBool = AtomicBool::new(false);

fn lock() -> MutexGuard<'static, Log> {
    LOG.lock().unwrap_or_else(PoisonError::into_inner)
}

/// The log, with the file's calls read in if they were not yet.
fn loaded() -> MutexGuard<'static, Log> {
    let mut log = lock();
    if !log.loaded {
        log.entries = read_tail(&file_path());
        log.loaded = true;
    }
    log
}

/// Newest first, cut to CALL_LOG_KEEP.
fn push_front(entries: &mut Vec<CCallLogEntry>, e: CCallLogEntry) {
    if entries.capacity() == 0 {
        *entries = new_list();
    }
    entries.insert(0, e);
    entries.truncate(CALL_LOG_KEEP);
}

fn append(path: &Path, text: &str) -> std::io::Result<()> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let mut file = OpenOptions::new().create(true).append(true).open(path)?;
    // One write, so a line is never split.
    file.write_all(text.as_bytes())
}

/// Record a finished call. On the call thread.
pub(crate) fn add(kind: Kind, number: &str, time: Option<u64>, duration_s: u32) {
    let text = line(kind, number, time, duration_s);
    let mut log = loaded();
    push_front(&mut log.entries, entry(kind, number, time, duration_s));
    log.seq = log.seq.wrapping_add(1);
    if kind == Kind::Missed {
        log.missed = log.missed.saturating_add(1);
    }
    // Under the lock, so a clear cannot come between the two.
    match append(&file_path(), &text) {
        Ok(()) => WARNED.store(false, Ordering::Relaxed),
        Err(_) => {
            if !WARNED.swap(true, Ordering::Relaxed) {
                warn!(b"cannot write the call log; is there an SD card?\0");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// FFI — thistle/call.h
// ---------------------------------------------------------------------------

/// How many calls the log holds, up to CALL_LOG_KEEP.
#[no_mangle]
pub extern "C" fn rs_call_log_count() -> u32 {
    loaded().entries.len() as u32
}

/// The call at `index`, 0 being the latest.
///
/// # Safety
/// `out` must be NULL or point to a writable call_log_entry_t.
#[no_mangle]
pub unsafe extern "C" fn rs_call_log_get(index: u32, out: *mut CCallLogEntry) -> bool {
    if out.is_null() {
        return false;
    }
    match loaded().entries.get(index as usize) {
        Some(e) => {
            *out = *e;
            true
        }
        None => false,
    }
}

/// Forget every call, the file's too.
#[no_mangle]
pub extern "C" fn rs_call_log_clear() -> i32 {
    let mut log = lock();
    log.entries.clear();
    log.loaded = true;
    log.missed = 0;
    log.seq = log.seq.wrapping_add(1);
    let path = file_path();
    let ret = match OpenOptions::new().write(true).truncate(true).open(&path) {
        Ok(_) => ESP_OK,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => ESP_OK,
        Err(_) => ESP_FAIL,
    };
    ret
}

/// Changes whenever a call is added or the log cleared.
#[no_mangle]
pub extern "C" fn rs_call_log_seq() -> u32 {
    loaded().seq
}

/// Calls missed since the user last looked at Recents.
#[no_mangle]
pub extern "C" fn rs_call_missed_count() -> u32 {
    lock().missed
}

/// The user has seen the missed calls.
#[no_mangle]
pub extern "C" fn rs_call_missed_seen() {
    lock().missed = 0;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_file(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("thistle_call_log_{}_{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir.join("calls.jsonl")
    }

    fn number(e: &CCallLogEntry) -> String {
        let end = e.number.iter().position(|&b| b == 0).unwrap();
        String::from_utf8(e.number[..end].to_vec()).unwrap()
    }

    #[test]
    fn a_line_has_its_keys_in_order_and_a_null_time_while_the_clock_is_unset() {
        assert_eq!(
            line(Kind::Incoming, "+4917612345678", Some(1_760_351_234), 63),
            "{\"duration\":63,\"kind\":\"in\",\"number\":\"+4917612345678\",\"time\":1760351234}\n"
        );
        assert_eq!(
            line(Kind::Missed, "", None, 0),
            "{\"duration\":0,\"kind\":\"missed\",\"number\":\"\",\"time\":null}\n"
        );
        assert!(line(Kind::Outgoing, "1", None, 0).contains("\"kind\":\"out\""));
        assert!(line(Kind::Rejected, "1", None, 0).contains("\"kind\":\"rejected\""));
    }

    #[test]
    fn a_line_reads_back_as_it_was_written() {
        for kind in [Kind::Outgoing, Kind::Incoming, Kind::Missed, Kind::Rejected] {
            let e = parse_line(&line(kind, "+491701", Some(1_760_000_000), 7)).unwrap();
            assert_eq!(e.kind, kind as u8);
            assert_eq!(number(&e), "+491701");
            assert_eq!(e.time, 1_760_000_000);
            assert_eq!(e.duration_s, 7);
        }
        let unset = parse_line(&line(Kind::Missed, "", None, 0)).unwrap();
        assert_eq!(unset.time, 0);
        assert_eq!(number(&unset), "");
    }

    #[test]
    fn malformed_lines_are_none() {
        assert_eq!(parse_line(""), None);
        assert_eq!(parse_line("{\"duration\":3,\"kind\":\"in\",\"num"), None);
        assert_eq!(parse_line("{\"kind\":\"forwarded\",\"number\":\"1\"}"), None);
        assert_eq!(parse_line("[1,2,3]"), None);
        assert_eq!(parse_line("not json"), None);
        // A number too long for the struct is cut, not refused.
        let long = parse_line(&line(Kind::Outgoing, &"9".repeat(60), None, 0)).unwrap();
        assert_eq!(number(&long).len(), CALL_NUMBER_MAX - 1);
    }

    #[test]
    fn the_tail_reads_newest_first_and_skips_what_does_not_parse() {
        let path = temp_file("tail");
        let mut text = String::new();
        text.push_str(&line(Kind::Outgoing, "+491", Some(1_760_000_001), 10));
        text.push_str("garbage\n");
        text.push_str(&line(Kind::Missed, "+492", None, 0));
        text.push_str("{\"duration\":1,\"kind\":\"in\",\"number\":\"+49"); // cut short
        text.push('\n');
        text.push_str(&line(Kind::Incoming, "+493", Some(1_760_000_003), 30));
        std::fs::write(&path, text).unwrap();
        let list = read_tail(&path);
        let numbers: Vec<String> = list.iter().map(number).collect();
        assert_eq!(numbers, ["+493", "+492", "+491"]);
        assert_eq!(list[0].kind, Kind::Incoming as u8);
        assert!(read_tail(&path.with_extension("missing")).is_empty());
    }

    #[test]
    fn the_tail_keeps_the_newest_hundred_and_skips_the_line_it_began_inside() {
        let path = temp_file("cap");
        let mut text = String::new();
        // Far more than TAIL_BYTES, so the read begins mid-file.
        for i in 0..1000u32 {
            text.push_str(&line(Kind::Outgoing, &format!("+49{i:06}"), Some(1_760_000_000 + i as u64), i));
        }
        std::fs::write(&path, &text).unwrap();
        assert!(text.len() as u64 > TAIL_BYTES);
        let list = read_tail(&path);
        assert_eq!(list.len(), CALL_LOG_KEEP);
        assert_eq!(number(&list[0]), "+49000999");
        assert_eq!(number(&list[CALL_LOG_KEEP - 1]), "+49000900");
    }

    #[test]
    fn new_calls_go_on_top_and_the_list_stays_at_a_hundred() {
        let mut list = new_list();
        for i in 0..150u32 {
            push_front(&mut list, entry(Kind::Outgoing, &i.to_string(), None, i));
        }
        assert_eq!(list.len(), CALL_LOG_KEEP);
        assert_eq!(number(&list[0]), "149");
        assert_eq!(number(&list[99]), "50");
        // One allocation of over 4 KB: in PSRAM on the device.
        assert!(list.capacity() * std::mem::size_of::<CCallLogEntry>() > 4096);
    }

    #[test]
    fn calls_are_logged_counted_and_cleared() {
        let _ = std::fs::remove_dir_all(card_root());
        {
            let mut log = lock();
            log.loaded = false;
            log.entries = Vec::new();
            log.missed = 0;
        }
        assert_eq!(rs_call_log_count(), 0);
        let seq = rs_call_log_seq();
        add(Kind::Outgoing, "+491701", Some(1_760_000_000), 12);
        add(Kind::Missed, "+491702", None, 0);
        add(Kind::Missed, "", None, 0);
        assert_eq!(rs_call_log_seq(), seq.wrapping_add(3));
        assert_eq!(rs_call_log_count(), 3);
        assert_eq!(rs_call_missed_count(), 2);
        let mut e = unsafe { std::mem::zeroed::<CCallLogEntry>() };
        assert!(unsafe { rs_call_log_get(1, &mut e) });
        assert_eq!(number(&e), "+491702");
        assert!(!unsafe { rs_call_log_get(3, &mut e) });
        rs_call_missed_seen();
        assert_eq!(rs_call_missed_count(), 0);

        // What was written reads back the same after a restart.
        lock().loaded = false;
        assert_eq!(rs_call_log_count(), 3);
        assert!(unsafe { rs_call_log_get(2, &mut e) });
        assert_eq!((number(&e).as_str(), e.duration_s, e.time), ("+491701", 12, 1_760_000_000));

        assert_eq!(rs_call_log_clear(), ESP_OK);
        assert_eq!(rs_call_log_count(), 0);
        lock().loaded = false;
        assert_eq!(rs_call_log_count(), 0, "the file is emptied too");
        let _ = std::fs::remove_dir_all(card_root());
    }

    #[test]
    fn the_entry_struct_matches_the_c_layout() {
        // char[33], uint8_t, then two uint32_t from 36.
        assert_eq!(std::mem::offset_of!(CCallLogEntry, kind), 33);
        assert_eq!(std::mem::offset_of!(CCallLogEntry, time), 36);
        assert_eq!(std::mem::offset_of!(CCallLogEntry, duration_s), 40);
        assert_eq!(std::mem::size_of::<CCallLogEntry>(), 44);
    }
}
