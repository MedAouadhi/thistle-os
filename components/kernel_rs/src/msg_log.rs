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
//!
//! The Messenger reads the history back the first time it opens after a
//! restart (`rs_msg_log_read_back`): the lines from before this boot, as
//! everything since reaches it from the kernel's inboxes or its own sending.

use std::collections::BTreeMap;
use std::ffi::{c_void, CString};
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::raw::c_char;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{Mutex, PoisonError};

const SECTION: &str = "messenger";
const KEY: &str = "save_history";

/// The longest file name a conversation gets, before ".jsonl".
const NAME_MAX: usize = 40;

/// A mesh contact's file name: the first 6 bytes of its key in hex, as the
/// Messenger addresses it (MSG_MESH_KEY_PREFIX).
const MESH_KEY_HEX: usize = 12;

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

/// Where each file ended before this boot first wrote to it. The lines
/// after that are this boot's: the Messenger is handed those as they come,
/// so reading them back too would show them twice.
pub(crate) type Marks = BTreeMap<PathBuf, u64>;

/// Appends are one at a time: the mesh, the modem and apps all record.
static WRITING: Mutex<Marks> = Mutex::new(BTreeMap::new());
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

/// Append `entry` under `root`, creating its directory if need be, and mark
/// where the file ended if this boot had not written to it yet.
pub(crate) fn append(root: &Path, entry: &Entry, time: Option<u64>, marks: &mut Marks) -> std::io::Result<()> {
    let dir: PathBuf = root.join(entry.transport.dir());
    std::fs::create_dir_all(&dir)?;
    let path = dir.join(file_name(entry.conv));
    let mut file = OpenOptions::new().create(true).append(true).open(&path)?;
    if !marks.contains_key(&path) {
        let end = file.metadata()?.len();
        marks.insert(path, end);
    }
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
    let mut marks = WRITING.lock().unwrap_or_else(PoisonError::into_inner);
    match append(root(), entry, now(), &mut marks) {
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
// Reading back
// ---------------------------------------------------------------------------

/// One message read back from the history.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct Saved {
    pub transport: Transport,
    /// The conversation as the Messenger addresses it: a phone number or a
    /// sender's name, a mesh contact's key prefix, or "" for the Public
    /// channel.
    pub conv: String,
    pub dir: Direction,
    pub peer: String,
    pub text: String,
    /// When this device logged it, else when it was sent.
    pub time: Option<u64>,
}

/// A conversation's file, as far as it is read back.
#[derive(Debug)]
pub(crate) struct Found {
    transport: Transport,
    path: PathBuf,
    /// Where this boot's lines begin, or the file's end.
    end: u64,
}

/// The conversation a mesh file holds, as the Messenger addresses it: "" for
/// the Public channel, a contact by its key prefix. None for another
/// channel, which the Messenger has no conversation for.
fn mesh_conv(stem: &str) -> Option<String> {
    if stem == "public" {
        return Some(String::new());
    }
    let is_key = stem.len() == MESH_KEY_HEX && stem.bytes().all(|b| b.is_ascii_hexdigit());
    is_key.then(|| stem.to_string())
}

/// A line of the history, or None for one that is not a message: cut short
/// by a pulled card, say. `mesh_conv` is the conversation a mesh file
/// holds; an SMS conversation is with whoever the message came from or
/// went to.
fn parse(transport: Transport, mesh_conv: Option<&str>, line: &str) -> Option<Saved> {
    let v: serde_json::Value = serde_json::from_str(line).ok()?;
    let (dir, who) = match v.get("dir")?.as_str()? {
        "in" => (Direction::In, "from"),
        "out" => (Direction::Out, "to"),
        _ => return None,
    };
    let peer = v.get(who)?.as_str()?.to_string();
    let text = v.get("text")?.as_str()?.to_string();
    let time = v.get("time").and_then(|t| t.as_u64()).or_else(|| v.get("sent").and_then(|t| t.as_u64()));
    let conv = match transport {
        Transport::Sms if peer.is_empty() => return None,
        Transport::Sms => peer.clone(),
        Transport::Mesh => mesh_conv?.to_string(),
    };
    Some(Saved { transport, conv, dir, peer, text, time })
}

/// The last `count` whole lines of `file` before `end`, read backwards a
/// block at a time, so a long history costs no more than a short one.
fn last_lines(file: &mut File, end: u64, count: usize) -> std::io::Result<String> {
    const BLOCK: u64 = 4096;
    let mut buf: Vec<u8> = Vec::new();
    let mut start = end;
    // A line is whole once the newline before it is in: count + 1 of them.
    while start > 0 && buf.iter().filter(|&&b| b == b'\n').count() <= count {
        let from = start.saturating_sub(BLOCK);
        let mut block = vec![0u8; (start - from) as usize];
        file.seek(SeekFrom::Start(from))?;
        file.read_exact(&mut block)?;
        block.extend_from_slice(&buf);
        buf = block;
        start = from;
    }
    let text = String::from_utf8_lossy(&buf);
    let mut lines: Vec<&str> = text.split('\n').collect();
    if start > 0 {
        // The first is the end of a line begun before what was read.
        lines.remove(0);
    }
    lines.retain(|l| !l.trim().is_empty());
    let from = lines.len().saturating_sub(count);
    Ok(lines[from..].join("\n"))
}

/// Every conversation's file under `root` that has lines from before this
/// boot. Called with the writing lock held, so no line can land between
/// reading a file's length and its mark.
pub(crate) fn conversations(root: &Path, marks: &Marks) -> Vec<Found> {
    let mut found = Vec::new();
    for transport in [Transport::Sms, Transport::Mesh] {
        let Ok(dir) = std::fs::read_dir(root.join(transport.dir())) else {
            continue;
        };
        for item in dir.flatten() {
            let path = item.path();
            if path.extension().and_then(|e| e.to_str()) != Some("jsonl") {
                continue;
            }
            let Ok(meta) = item.metadata() else { continue };
            let end = marks.get(&path).map_or(meta.len(), |&mark| mark.min(meta.len()));
            if meta.is_file() && end > 0 {
                found.push(Found { transport, path, end });
            }
        }
    }
    found
}

/// The history in `found`, for the Messenger to show again after a
/// restart: the `max_convs` conversations with the latest messages, the
/// latest last, each with its last `max_msgs` messages, oldest first. One
/// conversation's lines are in memory at a time.
pub(crate) fn read_back(found: Vec<Found>, max_convs: usize, max_msgs: usize, mut visit: impl FnMut(Saved)) {
    let stem = |f: &Found| f.path.file_stem().and_then(|s| s.to_str()).unwrap_or("").to_string();

    // Each by its last message's time; a file the clock never dated, first.
    let mut dated: Vec<(Option<u64>, Found)> = found
        .into_iter()
        .filter(|f| f.transport == Transport::Sms || mesh_conv(&stem(f)).is_some())
        .filter_map(|f| {
            let mut file = File::open(&f.path).ok()?;
            let last = last_lines(&mut file, f.end, 1).ok()?;
            let conv = mesh_conv(&stem(&f));
            let time = parse(f.transport, conv.as_deref(), &last).and_then(|m| m.time);
            Some((time, f))
        })
        .collect();
    dated.sort_by(|a, b| a.0.cmp(&b.0).then_with(|| a.1.path.cmp(&b.1.path)));
    let skip = dated.len().saturating_sub(max_convs);

    for (_, f) in dated.into_iter().skip(skip) {
        let conv = mesh_conv(&stem(&f));
        let Ok(mut file) = File::open(&f.path) else { continue };
        let Ok(lines) = last_lines(&mut file, f.end, max_msgs) else { continue };
        for line in lines.lines() {
            if let Some(m) = parse(f.transport, conv.as_deref(), line) {
                visit(m);
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
    std::fs::create_dir_all(root()).is_ok()
}

/// Where the history goes, for apps to show.
#[no_mangle]
pub extern "C" fn rs_msg_log_dir() -> *const c_char {
    ROOT_C.as_ptr() as *const c_char
}

/// What rs_msg_log_read_back() hands over for each message.
pub type SavedFn = unsafe extern "C" fn(
    ctx: *mut c_void,
    transport: i32,
    conv: *const c_char,
    sent: bool,
    peer: *const c_char,
    text: *const c_char,
    time: *const c_char,
);

/// MSG_LOG_SMS and MSG_LOG_MESH in thistle/msg_log.h.
const C_SMS: i32 = 0;
const C_MESH: i32 = 1;

/// A C string of `s`, any NUL in it dropped.
fn c_string(s: &str) -> CString {
    CString::new(s.replace('\0', "")).unwrap_or_default()
}

/// Hand `visit` the history from before this boot, as read_back() picks it,
/// with each message's time of day as the status bar shows the time ("--:--"
/// when it is not known). Returns how many messages it handed over.
///
/// # Safety
/// `visit` is called with `ctx` and strings that live for the call only.
#[no_mangle]
pub unsafe extern "C" fn rs_msg_log_read_back(
    max_convs: u32,
    max_msgs: u32,
    visit: Option<SavedFn>,
    ctx: *mut c_void,
) -> i32 {
    let Some(visit) = visit else { return 0 };
    let found = {
        let marks = WRITING.lock().unwrap_or_else(PoisonError::into_inner);
        conversations(root(), &marks)
    };
    let mut count = 0;
    read_back(found, max_convs as usize, max_msgs as usize, |m| {
        let transport = match m.transport {
            Transport::Sms => C_SMS,
            Transport::Mesh => C_MESH,
        };
        let time = m
            .time
            .and_then(|t| i64::try_from(t).ok())
            .and_then(crate::time_manager::clock_face_at)
            .unwrap_or_else(|| "--:--".to_string());
        let (conv, peer, text, time) = (c_string(&m.conv), c_string(&m.peer), c_string(&m.text), c_string(&time));
        visit(ctx, transport, conv.as_ptr(), m.dir == Direction::Out, peer.as_ptr(), text.as_ptr(), time.as_ptr());
        count += 1;
    });
    count
}

// ---------------------------------------------------------------------------
// Tailnet chat: the third kind
// ---------------------------------------------------------------------------
//
//   /sdcard/messages/tailnet/630dcd2966c433669112.jsonl            a contact
//   /sdcard/messages/tailnet/requests/630dcd2966c433669112.jsonl   a contact request
//
// One file per chat peer, named by its key's fingerprint. A contact
// request's messages are kept apart until it is accepted, when they join
// the contact's file; declined, they are deleted. Each line carries the
// peer's key and the name it came under, the message's id, when it was
// written (`ts`, ms, the sender's clock) and its delivery state.

/// The directory under the history's root.
pub(crate) const CHAT_DIR: &str = "tailnet";
const CHAT_REQUESTS: &str = "requests";

/// One Tailnet chat message, as it goes into the history.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ChatLine {
    pub dir: Direction,
    /// The chat peer's identity key, hex.
    pub key: String,
    /// The name it came under.
    pub name: String,
    pub id: String,
    /// When it was written, ms since 1970 by the writer's clock.
    pub ts: u64,
    pub text: String,
    pub reply_to: Option<String>,
    /// "received"; for one sent, "waiting", "delivered" or "read".
    pub state: String,
    /// A voice message: its file is `voice/<id>.opus` beside the history,
    /// and `text` is empty.
    pub voice: Option<ChatVoice>,
}

/// A voice message's length and size, as its line keeps them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ChatVoice {
    pub ms: u32,
    pub bytes: u32,
}

const CHAT_VOICE: &str = "voice";

/// Where voice message `id`'s Ogg/Opus file is: beside the history, in
/// `tailnet/voice/`, named by its id alone, so a contact's key changing
/// moves nothing.
pub(crate) fn chat_voice_file(root: &Path, id: &str) -> PathBuf {
    root.join(CHAT_DIR).join(CHAT_VOICE).join(format!("{id}.opus"))
}

/// Keep voice message `id`'s file: written beside as `.tmp` and renamed,
/// so a file that is there is whole.
pub(crate) fn chat_keep_voice(root: &Path, id: &str, data: &[u8]) -> std::io::Result<PathBuf> {
    if id.is_empty() || !id.chars().all(|c| c.is_ascii_hexdigit()) {
        return Err(std::io::Error::new(std::io::ErrorKind::InvalidInput, "not a message id"));
    }
    let path = chat_voice_file(root, id);
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let tmp = path.with_extension("tmp");
    std::fs::write(&tmp, data)?;
    std::fs::rename(&tmp, &path)?;
    Ok(path)
}

pub(crate) fn chat_line(l: &ChatLine, time: Option<u64>) -> String {
    let mut obj = serde_json::Map::new();
    let (dir, who) = match l.dir {
        Direction::In => ("in", "from"),
        Direction::Out => ("out", "to"),
    };
    obj.insert("dir".into(), dir.into());
    obj.insert(who.into(), l.key.as_str().into());
    obj.insert("name".into(), l.name.as_str().into());
    obj.insert("id".into(), l.id.as_str().into());
    obj.insert("ts".into(), l.ts.into());
    obj.insert("time".into(), time.into());
    obj.insert("text".into(), l.text.as_str().into());
    if let Some(r) = &l.reply_to {
        obj.insert("reply_to".into(), r.as_str().into());
    }
    obj.insert("state".into(), l.state.as_str().into());
    if let Some(v) = &l.voice {
        obj.insert("voice".into(), format!("{CHAT_VOICE}/{}.opus", l.id).into());
        obj.insert("ms".into(), v.ms.into());
        obj.insert("bytes".into(), v.bytes.into());
    }
    let mut line = serde_json::Value::Object(obj).to_string();
    line.push('\n');
    line
}

pub(crate) fn parse_chat_line(line: &str) -> Option<ChatLine> {
    let v: serde_json::Value = serde_json::from_str(line).ok()?;
    let (dir, who) = match v.get("dir")?.as_str()? {
        "in" => (Direction::In, "from"),
        "out" => (Direction::Out, "to"),
        _ => return None,
    };
    let s = |k: &str| v.get(k).and_then(|x| x.as_str()).map(str::to_string);
    Some(ChatLine {
        dir,
        key: s(who)?,
        name: s("name").unwrap_or_default(),
        id: s("id")?,
        ts: v.get("ts")?.as_u64()?,
        text: s("text")?,
        reply_to: s("reply_to"),
        state: s("state").unwrap_or_default(),
        voice: s("voice").map(|_| {
            let n = |k: &str| v.get(k).and_then(|x| x.as_u64()).unwrap_or(0) as u32;
            ChatVoice { ms: n("ms"), bytes: n("bytes") }
        }),
    })
}

/// A reaction, as it goes into the history: `dir` In for the peer's, Out
/// for this device's; `emoji` "" removes one.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ChatReact {
    pub dir: Direction,
    pub key: String,
    pub name: String,
    /// The reaction's own id.
    pub id: String,
    pub ts: u64,
    /// The message reacted to.
    pub to: String,
    pub emoji: String,
}

/// A line of a Tailnet chat file. Messages are never rewritten: what
/// happens to one later (delivered, read, a reaction) is a line of its own
/// after it, and reading the file folds them together.
///
///   {"dir":"out","to":K,"name":N,"id":X,"ts":..,"time":..,"text":"..","state":"waiting"}
///   {"dir":"out","to":K,"id":X,"state":"delivered","time":..}          a state change
///   {"dir":"in","from":K,"name":N,"id":R,"ts":..,"react":X,"emoji":"👍","time":..}
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum ChatEntry {
    Message(ChatLine),
    /// Message `id` became `state`: "delivered" or "read" for one sent,
    /// "read" for one received.
    State { dir: Direction, key: String, id: String, state: String },
    React(ChatReact),
}

fn dir_names(dir: Direction) -> (&'static str, &'static str) {
    match dir {
        Direction::In => ("in", "from"),
        Direction::Out => ("out", "to"),
    }
}

pub(crate) fn chat_entry_line(e: &ChatEntry, time: Option<u64>) -> String {
    let mut obj = serde_json::Map::new();
    match e {
        ChatEntry::Message(l) => return chat_line(l, time),
        ChatEntry::State { dir, key, id, state } => {
            let (d, who) = dir_names(*dir);
            obj.insert("dir".into(), d.into());
            obj.insert(who.into(), key.as_str().into());
            obj.insert("id".into(), id.as_str().into());
            obj.insert("state".into(), state.as_str().into());
        }
        ChatEntry::React(r) => {
            let (d, who) = dir_names(r.dir);
            obj.insert("dir".into(), d.into());
            obj.insert(who.into(), r.key.as_str().into());
            obj.insert("name".into(), r.name.as_str().into());
            obj.insert("id".into(), r.id.as_str().into());
            obj.insert("ts".into(), r.ts.into());
            obj.insert("react".into(), r.to.as_str().into());
            obj.insert("emoji".into(), r.emoji.as_str().into());
        }
    }
    obj.insert("time".into(), time.into());
    let mut line = serde_json::Value::Object(obj).to_string();
    line.push('\n');
    line
}

pub(crate) fn parse_chat_entry(line: &str) -> Option<ChatEntry> {
    if let Some(l) = parse_chat_line(line) {
        return Some(ChatEntry::Message(l));
    }
    let v: serde_json::Value = serde_json::from_str(line).ok()?;
    let (dir, who) = match v.get("dir")?.as_str()? {
        "in" => (Direction::In, "from"),
        "out" => (Direction::Out, "to"),
        _ => return None,
    };
    let s = |k: &str| v.get(k).and_then(|x| x.as_str()).map(str::to_string);
    if let Some(to) = s("react") {
        return Some(ChatEntry::React(ChatReact {
            dir,
            key: s(who)?,
            name: s("name").unwrap_or_default(),
            id: s("id")?,
            ts: v.get("ts")?.as_u64()?,
            to,
            emoji: s("emoji")?,
        }));
    }
    Some(ChatEntry::State { dir, key: s(who)?, id: s("id")?, state: s("state")? })
}

fn chat_path(root: &Path, request: bool, fp: &str) -> PathBuf {
    let mut dir = root.join(CHAT_DIR);
    if request {
        dir.push(CHAT_REQUESTS);
    }
    dir.join(file_name(fp))
}

/// Append `line` to the file of the chat peer whose fingerprint is `fp`:
/// its contact request's if `request`.
pub(crate) fn chat_append(root: &Path, request: bool, fp: &str, line: &ChatLine, time: Option<u64>,
                          marks: &mut Marks) -> std::io::Result<()> {
    chat_append_text(root, request, fp, &chat_line(line, time), marks)
}

/// Append a state change or a reaction, as chat_append() a message.
pub(crate) fn chat_append_entry(root: &Path, request: bool, fp: &str, e: &ChatEntry, time: Option<u64>,
                                marks: &mut Marks) -> std::io::Result<()> {
    chat_append_text(root, request, fp, &chat_entry_line(e, time), marks)
}

fn chat_append_text(root: &Path, request: bool, fp: &str, text: &str, marks: &mut Marks) -> std::io::Result<()> {
    let path = chat_path(root, request, fp);
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let mut file = OpenOptions::new().create(true).append(true).open(&path)?;
    if !marks.contains_key(&path) {
        let end = file.metadata()?.len();
        marks.insert(path, end);
    }
    file.write_all(text.as_bytes())
}

/// A contact request accepted: its messages join the contact's file.
pub(crate) fn chat_accept(root: &Path, fp: &str) -> std::io::Result<()> {
    let from = chat_path(root, true, fp);
    let held = match std::fs::read(&from) {
        Ok(h) => h,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(e) => return Err(e),
    };
    let to = chat_path(root, false, fp);
    if let Some(dir) = to.parent() {
        std::fs::create_dir_all(dir)?;
    }
    OpenOptions::new().create(true).append(true).open(&to)?.write_all(&held)?;
    std::fs::remove_file(&from)
}

/// A contact's file under another fingerprint: a contact added by address
/// whose key is pinned at last. Appended to whatever is there already.
pub(crate) fn chat_join(root: &Path, from_fp: &str, to_fp: &str) -> std::io::Result<()> {
    let from = chat_path(root, false, from_fp);
    let held = match std::fs::read(&from) {
        Ok(h) => h,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(e) => return Err(e),
    };
    let to = chat_path(root, false, to_fp);
    OpenOptions::new().create(true).append(true).open(&to)?.write_all(&held)?;
    std::fs::remove_file(&from)
}

/// A contact request declined: its messages go.
pub(crate) fn chat_drop_request(root: &Path, fp: &str) {
    let _ = std::fs::remove_file(chat_path(root, true, fp));
}

/// The last `count` messages of a file, oldest first.
pub(crate) fn chat_read(root: &Path, request: bool, fp: &str, count: usize) -> Vec<ChatLine> {
    let Ok(mut file) = File::open(chat_path(root, request, fp)) else {
        return Vec::new();
    };
    let Ok(end) = file.metadata().map(|m| m.len()) else {
        return Vec::new();
    };
    last_lines(&mut file, end, count)
        .map(|text| text.lines().filter_map(parse_chat_line).collect())
        .unwrap_or_default()
}

/// The last `count` lines of a file, messages, state changes and
/// reactions, oldest first.
pub(crate) fn chat_entries(root: &Path, request: bool, fp: &str, count: usize) -> Vec<ChatEntry> {
    let Ok(mut file) = File::open(chat_path(root, request, fp)) else {
        return Vec::new();
    };
    let Ok(end) = file.metadata().map(|m| m.len()) else {
        return Vec::new();
    };
    last_lines(&mut file, end, count)
        .map(|text| text.lines().filter_map(parse_chat_entry).collect())
        .unwrap_or_default()
}

/// The fingerprints of the contact requests on the card.
pub(crate) fn chat_requests(root: &Path) -> Vec<String> {
    let Ok(dir) = std::fs::read_dir(root.join(CHAT_DIR).join(CHAT_REQUESTS)) else {
        return Vec::new();
    };
    let mut fps: Vec<String> = dir
        .flatten()
        .filter_map(|e| {
            let p = e.path();
            if p.extension()? != "jsonl" {
                return None;
            }
            p.file_stem()?.to_str().map(str::to_string)
        })
        .collect();
    fps.sort();
    fps
}

/// Where the history is, on the card.
pub(crate) fn root() -> &'static Path {
    // The simulator's card is THISTLE_SIM_SDCARD when it names one, not the
    // /tmp/thistle_sdcard symlink, which every simulator on the machine
    // shares: tests run side by side would write each other's history.
    #[cfg(not(any(target_os = "espidf", test)))]
    {
        static SIM: std::sync::OnceLock<Option<PathBuf>> = std::sync::OnceLock::new();
        if let Some(p) = SIM
            .get_or_init(|| std::env::var_os("THISTLE_SIM_SDCARD").map(|c| PathBuf::from(c).join("messages")))
        {
            return p.as_path();
        }
    }
    Path::new(ROOT)
}

/// Record a Tailnet chat message, if history is kept.
pub(crate) fn chat_record(request: bool, fp: &str, line: &ChatLine) {
    if cfg!(test) || !enabled() {
        return;
    }
    let mut marks = WRITING.lock().unwrap_or_else(PoisonError::into_inner);
    match chat_append(root(), request, fp, line, now(), &mut marks) {
        Ok(()) => WARNED.store(false, Ordering::Relaxed),
        Err(_) => {
            if !WARNED.swap(true, Ordering::Relaxed) {
                warn!(b"cannot write the message history to %s; is there an SD card?\0", ROOT_C.as_ptr());
            }
        }
    }
}

/// Record a state change or a reaction, if history is kept.
pub(crate) fn chat_note(request: bool, fp: &str, e: &ChatEntry) {
    if cfg!(test) || !enabled() {
        return;
    }
    let mut marks = WRITING.lock().unwrap_or_else(PoisonError::into_inner);
    let _ = chat_append_entry(root(), request, fp, e, now(), &mut marks);
}

/// A contact's file moved to its pinned key's fingerprint, on the card.
pub(crate) fn chat_rekey(from_fp: &str, to_fp: &str) {
    if cfg!(test) {
        return;
    }
    let _guard = WRITING.lock().unwrap_or_else(PoisonError::into_inner);
    let _ = chat_join(root(), from_fp, to_fp);
}

/// A contact request accepted or declined, on the card.
pub(crate) fn chat_settle_request(fp: &str, accepted: bool) {
    if cfg!(test) {
        return;
    }
    let _guard = WRITING.lock().unwrap_or_else(PoisonError::into_inner);
    if accepted {
        if chat_accept(root(), fp).is_err() {
            warn!(b"could not move a contact request's messages into its conversation\0");
        }
    } else {
        chat_drop_request(root(), fp);
    }
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
        let marks = &mut Marks::new();
        append(&root, &sms_in("+491701", "one"), Some(1_760_000_000), marks).unwrap();
        append(&root, &sms_in("+491701", "two"), Some(1_760_000_001), marks).unwrap();
        append(&root, &sms_in("+491702", "other"), None, marks).unwrap();
        let mesh = Entry {
            transport: Transport::Mesh,
            conv: "1a2b3c4d5e6f",
            dir: Direction::In,
            peer: "Heltec V3",
            text: "hello",
            sent: None,
        };
        append(&root, &mesh, None, marks).unwrap();

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

    /// A file as an earlier boot left it.
    fn saved_file(root: &Path, name: &str, lines: &[String]) {
        let path = root.join(name);
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, lines.iter().map(|l| format!("{l}\n")).collect::<String>()).unwrap();
    }

    fn sms_line(from: &str, text: &str, time: u64) -> String {
        line(&sms_in(from, text), Some(time)).trim_end().to_string()
    }

    fn read_all(root: &Path, marks: &Marks, max_convs: usize, max_msgs: usize) -> Vec<Saved> {
        let mut out = Vec::new();
        read_back(conversations(root, marks), max_convs, max_msgs, |m| out.push(m));
        out
    }

    fn texts(saved: &[Saved]) -> Vec<&str> {
        saved.iter().map(|m| m.text.as_str()).collect()
    }

    #[test]
    fn the_history_is_read_back_oldest_first() {
        let root = temp_root("read_back");
        saved_file(&root, "sms/+491701.jsonl", &[
            sms_line("+491701", "one", 1_760_000_000),
            r#"{"dir":"out","text":"two","time":1760000001,"to":"+491701"}"#.to_string(),
        ]);
        let saved = read_all(&root, &Marks::new(), 12, 50);
        assert_eq!(saved, [
            Saved {
                transport: Transport::Sms,
                conv: "+491701".into(),
                dir: Direction::In,
                peer: "+491701".into(),
                text: "one".into(),
                time: Some(1_760_000_000),
            },
            Saved {
                transport: Transport::Sms,
                conv: "+491701".into(),
                dir: Direction::Out,
                peer: "+491701".into(),
                text: "two".into(),
                time: Some(1_760_000_001),
            },
        ]);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn what_this_boot_wrote_is_not_read_back() {
        // The kernel's inboxes hand those to the Messenger, or it sent them.
        let root = temp_root("this_boot");
        saved_file(&root, "sms/+491701.jsonl", &[sms_line("+491701", "before", 1_760_000_000)]);
        let marks = &mut Marks::new();
        append(&root, &sms_in("+491701", "since"), Some(1_760_000_100), marks).unwrap();
        append(&root, &sms_in("+491702", "new one"), Some(1_760_000_200), marks).unwrap();

        assert_eq!(texts(&read_all(&root, marks, 12, 50)), ["before"]);
        // After the next restart, all of it.
        assert_eq!(texts(&read_all(&root, &Marks::new(), 12, 50)), ["before", "since", "new one"]);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn the_conversations_with_the_latest_messages_are_read_back_the_latest_last() {
        let root = temp_root("latest");
        saved_file(&root, "sms/+491701.jsonl", &[sms_line("+491701", "a", 100), sms_line("+491701", "b", 2_000)]);
        saved_file(&root, "sms/+491702.jsonl", &[sms_line("+491702", "c", 3_000)]);
        saved_file(&root, "sms/+491703.jsonl", &[sms_line("+491703", "d", 1_000)]);
        // Never dated: the clock was not set.
        saved_file(&root, "mesh/public.jsonl", &[r#"{"dir":"in","from":"Bob","text":"e","time":null}"#.to_string()]);

        assert_eq!(texts(&read_all(&root, &Marks::new(), 2, 50)), ["a", "b", "c"]);
        assert_eq!(texts(&read_all(&root, &Marks::new(), 12, 50)), ["e", "d", "a", "b", "c"]);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn only_a_long_conversations_last_messages_are_read_back() {
        let root = temp_root("long");
        // Lines long enough that 50 of them span several blocks.
        let lines: Vec<String> = (0..300)
            .map(|i| sms_line("+491701", &format!("{i} {}", "x".repeat(150)), 1_760_000_000 + i))
            .collect();
        saved_file(&root, "sms/+491701.jsonl", &lines);

        let saved = read_all(&root, &Marks::new(), 12, 50);
        let numbers: Vec<u64> = saved.iter().map(|m| m.text.split(' ').next().unwrap().parse().unwrap()).collect();
        assert_eq!(numbers, (250..300).collect::<Vec<u64>>());
        // Fewer than asked for: all of them.
        assert_eq!(read_all(&root, &Marks::new(), 12, 500).len(), 300);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn files_are_read_back_as_the_messengers_conversations() {
        let root = temp_root("convs");
        saved_file(&root, "mesh/public.jsonl", &[r#"{"dir":"in","from":"Bob","text":"hi all","time":1760000001}"#.to_string()]);
        saved_file(&root, "mesh/1a2b3c4d5e6f.jsonl", &[r#"{"dir":"out","text":"hello","time":1760000002,"to":"Heltec V3"}"#.to_string()]);
        // The Messenger has no conversation for another channel.
        saved_file(&root, "mesh/channel3.jsonl", &[r#"{"dir":"in","from":"Eve","text":"elsewhere","time":1760000003}"#.to_string()]);
        // An SMS conversation is with its sender as the modem wrote it, not
        // as the file is named.
        saved_file(&root, "sms/DHL_Paket.jsonl", &[r#"{"dir":"in","from":"DHL Paket","text":"Ihr Paket","time":1760000004}"#.to_string()]);

        let saved = read_all(&root, &Marks::new(), 12, 50);
        let convs: Vec<(Transport, &str, &str)> =
            saved.iter().map(|m| (m.transport, m.conv.as_str(), m.peer.as_str())).collect();
        assert_eq!(convs, [
            (Transport::Mesh, "", "Bob"),
            (Transport::Mesh, "1a2b3c4d5e6f", "Heltec V3"),
            (Transport::Sms, "DHL Paket", "DHL Paket"),
        ]);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_line_that_is_not_a_message_is_passed_over() {
        let root = temp_root("cut");
        let path = root.join("sms/+491701.jsonl");
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        // A message dated only by its sender, a stray line, and one cut short
        // by a pulled card.
        std::fs::write(&path, concat!(
            r#"{"dir":"in","from":"+491701","sent":1760000000,"text":"kept","time":null}"#, "\n",
            "not json\n",
            r#"{"dir":"in","from":"+491701","te"#,
        )).unwrap();

        let saved = read_all(&root, &Marks::new(), 12, 50);
        assert_eq!(texts(&saved), ["kept"]);
        assert_eq!(saved[0].time, Some(1_760_000_000));
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
    fn chat(id: &str, text: &str, ts: u64) -> ChatLine {
        ChatLine {
            dir: Direction::In,
            key: "ab".repeat(32),
            name: "laptop".into(),
            id: id.into(),
            ts,
            text: text.into(),
            reply_to: None,
            state: "received".into(),
            voice: None,
        }
    }

    #[test]
    fn a_voice_message_is_a_line_that_points_to_its_file_beside_the_history() {
        let id = "3".repeat(32);
        let mut l = chat(&id, "", 1_793_541_600_000);
        l.voice = Some(ChatVoice { ms: 1500, bytes: 6129 });
        let text = chat_line(&l, Some(1));
        let v: serde_json::Value = serde_json::from_str(text.trim_end()).unwrap();
        assert_eq!(v["voice"], format!("voice/{id}.opus"));
        assert_eq!(v["ms"], 1500);
        assert_eq!(v["bytes"], 6129);
        assert_eq!(parse_chat_line(&text), Some(l));

        let root = temp_root("chat-voice");
        let path = chat_voice_file(&root, &id);
        assert_eq!(path, root.join("tailnet").join("voice").join(format!("{id}.opus")));
        chat_keep_voice(&root, &id, b"OggS whole").unwrap();
        assert_eq!(std::fs::read(&path).unwrap(), b"OggS whole");
        // Nothing half-written is left beside it.
        let names: Vec<_> = std::fs::read_dir(path.parent().unwrap()).unwrap().flatten().map(|e| e.file_name()).collect();
        assert_eq!(names.len(), 1, "{names:?}");
        assert!(chat_keep_voice(&root, "../escape", b"x").is_err());
    }

    #[test]
    fn a_chat_line_reads_back_as_it_was_written() {
        let mut l = chat(&"1".repeat(32), "Are you home? مرحبا\n", 1_793_541_600_000);
        l.reply_to = Some("2".repeat(32));
        let text = chat_line(&l, Some(1_793_541_601));
        assert_eq!(text.matches('\n').count(), 1);
        let v: serde_json::Value = serde_json::from_str(text.trim_end()).unwrap();
        assert_eq!(v["from"], "ab".repeat(32));
        assert_eq!(v["time"], 1_793_541_601u64);
        assert_eq!(parse_chat_line(&text), Some(l));
        assert_eq!(parse_chat_line("{\"dir\":\"in\""), None);
    }

    #[test]
    fn state_changes_and_reactions_follow_their_messages_in_the_file() {
        let root = temp_root("chat-entries");
        let mut marks = Marks::new();
        let fp = "630dcd2966c433669112";
        let mut mine = chat(&"1".repeat(32), "I passed!", 10);
        mine.dir = Direction::Out;
        mine.state = "waiting".into();
        let delivered = ChatEntry::State { dir: Direction::Out, key: "ab".repeat(32), id: "1".repeat(32),
                                           state: "delivered".into() };
        let thumbs = ChatEntry::React(ChatReact { dir: Direction::In, key: "ab".repeat(32), name: "laptop".into(),
                                                  id: "9".repeat(32), ts: 20, to: "1".repeat(32),
                                                  emoji: "👍".into() });
        chat_append(&root, false, fp, &mine, None, &mut marks).unwrap();
        for e in [&delivered, &thumbs] {
            let line = chat_entry_line(e, Some(5));
            assert_eq!(parse_chat_entry(&line).as_ref(), Some(e), "{line}");
            assert_eq!(parse_chat_line(&line), None, "not read as a message: {line}");
            chat_append_entry(&root, false, fp, e, None, &mut marks).unwrap();
        }
        let entries = chat_entries(&root, false, fp, 10);
        assert_eq!(entries, vec![ChatEntry::Message(mine.clone()), delivered, thumbs]);
        assert_eq!(chat_entries(&root, false, fp, 1).len(), 1, "the last lines only");
        assert_eq!(chat_read(&root, false, fp, 20), vec![mine], "chat_read keeps to the messages");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_request_is_kept_apart_until_accepted_or_declined() {
        let root = temp_root("chat");
        let mut marks = Marks::new();
        let fp = "630dcd2966c433669112";
        chat_append(&root, true, fp, &chat(&"1".repeat(32), "first", 1), None, &mut marks).unwrap();
        chat_append(&root, true, fp, &chat(&"2".repeat(32), "second", 2), None, &mut marks).unwrap();
        assert!(root.join("tailnet/requests/630dcd2966c433669112.jsonl").is_file());
        assert_eq!(chat_requests(&root), vec![fp.to_string()]);
        assert!(chat_read(&root, false, fp, 20).is_empty());
        chat_accept(&root, fp).unwrap();
        assert!(chat_requests(&root).is_empty());
        let texts: Vec<String> = chat_read(&root, false, fp, 20).into_iter().map(|l| l.text).collect();
        assert_eq!(texts, ["first", "second"]);
        assert_eq!(chat_read(&root, false, fp, 1)[0].text, "second");

        chat_append(&root, true, "other", &chat(&"3".repeat(32), "spam", 3), None, &mut marks).unwrap();
        chat_drop_request(&root, "other");
        assert!(chat_requests(&root).is_empty());
        chat_accept(&root, "nothing-held").unwrap();
        let _ = std::fs::remove_dir_all(&root);
    }

}
