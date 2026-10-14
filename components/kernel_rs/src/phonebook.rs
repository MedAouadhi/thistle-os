// SPDX-License-Identifier: BSD-3-Clause
//! The phonebook: the contacts in every `contacts.vcf` on the SD card, for
//! the Phone app, the call screen, the Messenger and the call log alike.
//!
//! Phones and mail programs export contacts as vCard, and no two the same
//! way: Android writes 2.1 with names in quoted-printable UTF-8, iOS 3.0
//! with numbers grouped under custom labels (`item1.TEL` with
//! `item1.X-ABLabel`), Google 3.0 with `TYPE=CELL`, newer programs 4.0 with
//! `tel:` URIs, and old ones Latin-1. The parser here takes all of them. It
//! reads a line at a time, never the whole file, because a file with photos
//! in it runs to megabytes; a photo's lines are skipped as they are read.
//! It keeps only what a phone needs: a name, and up to PB_NUMBERS_MAX
//! numbers with a label each.
//!
//! The card is searched once, PB_SEARCH_DEPTH folders deep, the first time
//! anything asks for a contact, and again on rs_phonebook_rescan(). Reading
//! is done on a thread of its own, which exits when it is done; readers see
//! the book that was there before, empty the first time, until the new one
//! is swapped in. Between searches only the files found are looked at, and
//! not more than every RECHECK: a changed or vanished one has them all read
//! again.
//!
//! Internal RAM is what runs out on an ESP32-S3, and every allocation of
//! 4 KB or less lands there. So the book holds no string per contact: one
//! arena holds every name and number, and the contacts are fixed records of
//! offsets into it, each kept in one allocation of more than 4 KB, which
//! ESP-IDF places in PSRAM. Reading parses one card at a time into small
//! strings that are freed again as soon as the card is in the arena.
//!
//! Stars are this device's own, by name, in `phone/starred.json` on the
//! card: a .vcf is never written to.

use std::fs::File;
use std::io::{self, BufRead, BufReader};
use std::os::raw::c_char;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard, PoisonError};
use std::time::{Duration, Instant, SystemTime};

use crate::sms_manager::copy_c;

const ESP_OK: i32 = 0;
const ESP_FAIL: i32 = -1;
const ESP_ERR_NO_MEM: i32 = 0x101;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;

/// phonebook.h
pub const PB_NAME_MAX: usize = 64;
pub const PB_NUMBER_MAX: usize = 33;
pub const PB_LABEL_MAX: usize = 12;
pub const PB_NUMBERS_MAX: usize = 6;
pub const PB_CONTACTS_MAX: usize = 2000;
pub const PB_SEARCH_DEPTH: usize = 4;

/// The longest line read; the rest of a longer one is dropped. Nothing kept
/// comes near it: only photos and notes do.
const LINE_MAX: usize = 8 * 1024;
/// The longest property kept once its folded lines are joined.
const PROPERTY_MAX: usize = 4 * 1024;
/// contacts.vcf files read at most, in path order.
const FILES_MAX: usize = 16;
/// Cards read before the rest are given up on. Duplicates across files are
/// dropped only once all are read, so this is above PB_CONTACTS_MAX.
const PARSE_MAX: usize = 4 * PB_CONTACTS_MAX;
/// How often the files found may be looked at for changes.
const RECHECK: Duration = Duration::from_secs(10);
/// An allocation of more than this lands in PSRAM on the device
/// (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL).
const INTERNAL_MAX: usize = 4096;
/// The reading thread's stack, in internal RAM, as every stack must be: a
/// PSRAM stack cannot be read while flash is written. It exists only while
/// the card is read. 8 KB overflowed on the T-Deck Max the first time the
/// card was searched: every look at the card goes down through newlib, the
/// VFS, FatFs and the SD card's SPI driver.
const STACK_SIZE: usize = 16 * 1024;
/// Say so if a read leaves less than this unused.
#[cfg(target_os = "espidf")]
const STACK_LOW_WATER: u32 = 4 * 1024;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

macro_rules! pb_log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {{
        #[cfg(not(test))]
        unsafe {
            esp_log_write($level, b"phonebook\0".as_ptr(), $fmt.as_ptr() $(, $arg)*);
        }
        #[cfg(test)]
        {
            let _ = ($level, $fmt $(, $arg)*);
        }
    }};
}

const ESP_LOG_WARN: i32 = 2;
const ESP_LOG_INFO: i32 = 3;

#[cfg(target_os = "espidf")]
extern "C" {
    // components/kernel/src/link_deps.c
    fn thistle_pthread_name_next(name: *const u8) -> i32;
    fn uxTaskGetStackHighWaterMark(task: *mut std::ffi::c_void) -> u32;
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
    std::env::temp_dir().join(format!("thistle_pb_card_{}", std::process::id()))
}

fn stars_path(root: &Path) -> PathBuf {
    root.join("phone").join("starred.json")
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

/// `s` cut to at most `max` bytes, at a character.
fn cut(s: &str, max: usize) -> &str {
    if s.len() <= max {
        return s;
    }
    let mut n = max;
    while !s.is_char_boundary(n) {
        n -= 1;
    }
    &s[..n]
}

/// `s` on one line, its runs of white space one space, trimmed.
fn tidy(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for word in s.split(|c: char| c.is_whitespace() || c.is_control()) {
        if word.is_empty() {
            continue;
        }
        if !out.is_empty() {
            out.push(' ');
        }
        out.push_str(word);
    }
    out
}

/// The digits of a phone number, into `buf`, as many as fit.
fn digits_into<'a>(s: &str, buf: &'a mut [u8; PB_NUMBER_MAX]) -> &'a [u8] {
    let mut n = 0;
    for b in s.bytes().filter(u8::is_ascii_digit) {
        if n == buf.len() {
            break;
        }
        buf[n] = b;
        n += 1;
    }
    &buf[..n]
}

fn digit_string(s: &str) -> String {
    s.chars().filter(char::is_ascii_digit).collect()
}

/// A name as it is sorted and searched: lower case, with the accents of
/// Latin letters taken off, so "Émile" sorts among the e's and "muller"
/// finds "Müller". Other scripts are kept as they are.
pub(crate) fn fold(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars().flat_map(char::to_lowercase) {
        match c {
            'à' | 'á' | 'â' | 'ã' | 'ä' | 'å' => out.push('a'),
            'ç' => out.push('c'),
            'è' | 'é' | 'ê' | 'ë' => out.push('e'),
            'ì' | 'í' | 'î' | 'ï' => out.push('i'),
            'ñ' => out.push('n'),
            'ò' | 'ó' | 'ô' | 'õ' | 'ö' | 'ø' => out.push('o'),
            'ù' | 'ú' | 'û' | 'ü' => out.push('u'),
            'ý' | 'ÿ' => out.push('y'),
            'ß' => out.push_str("ss"),
            'æ' => out.push_str("ae"),
            'œ' => out.push_str("oe"),
            c => out.push(c),
        }
    }
    out
}

/// Windows-1252, which ISO-8859-1 files nearly always are in practice.
fn cp1252(b: u8) -> char {
    const HIGH: [u16; 32] = [
        0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039,
        0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178,
    ];
    match b {
        0x80..=0x9F => char::from_u32(HIGH[(b - 0x80) as usize] as u32).unwrap_or('?'),
        _ => b as char,
    }
}

/// A value's bytes as text: UTF-8 unless the card says a Latin-1 charset, and
/// Windows-1252 for bytes that are not UTF-8 whatever it says, which is what
/// old phones wrote without saying so.
fn decode_charset(bytes: Vec<u8>, charset: Option<&str>) -> String {
    let single_byte = matches!(
        charset,
        Some("ISO-8859-1" | "ISO8859-1" | "ISO-8859-15" | "LATIN1" | "LATIN-1" | "WINDOWS-1252" | "CP1252")
    );
    let bytes = if single_byte {
        bytes
    } else {
        match String::from_utf8(bytes) {
            Ok(s) => return s,
            Err(e) => e.into_bytes(),
        }
    };
    bytes.into_iter().map(cp1252).collect()
}

fn hex(b: u8) -> Option<u8> {
    (b as char).to_digit(16).map(|d| d as u8)
}

/// Quoted-printable (vCard 2.1): "=C3=BC" is the byte pair of "ü". Soft line
/// breaks are already joined.
fn qp_decode(raw: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(raw.len());
    let mut i = 0;
    while i < raw.len() {
        if raw[i] == b'=' && i + 2 < raw.len() {
            if let (Some(h), Some(l)) = (hex(raw[i + 1]), hex(raw[i + 2])) {
                out.push(h << 4 | l);
                i += 3;
                continue;
            }
        }
        out.push(raw[i]);
        i += 1;
    }
    out
}

/// `s` split at each `sep` that is not escaped with a backslash, the escapes
/// left in for unescape().
fn split_unescaped(s: &str, sep: char) -> Vec<String> {
    let mut out = vec![String::new()];
    let mut chars = s.chars();
    while let Some(c) = chars.next() {
        let last = out.last_mut().expect("never empty");
        if c == '\\' {
            last.push(c);
            if let Some(next) = chars.next() {
                last.push(next);
            }
        } else if c == sep {
            out.push(String::new());
        } else {
            last.push(c);
        }
    }
    out
}

/// The text of a value, its escapes undone (vCard 3.0 and 4.0, and 2.1's
/// "\;"): "\," ",", "\;" ";", "\n" a line break, "\\" "\".
fn unescape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars();
    while let Some(c) = chars.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        match chars.next() {
            Some('n' | 'N') => out.push('\n'),
            Some(c @ (',' | ';' | ':' | '\\')) => out.push(c),
            Some(c) => {
                out.push('\\');
                out.push(c);
            }
            None => out.push('\\'),
        }
    }
    out
}

// ---------------------------------------------------------------------------
// vCard
// ---------------------------------------------------------------------------

/// One contact as read from a card, before it goes into the book.
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct Card {
    pub name: String,
    /// (number as written, label)
    pub numbers: Vec<(String, String)>,
}

/// What a TEL's types say it is.
fn label_for(types: &[String]) -> &'static str {
    let has = |t: &str| types.iter().any(|x| x == t);
    if has("IPHONE") {
        "iPhone"
    } else if has("FAX") {
        "Fax"
    } else if has("PAGER") {
        "Pager"
    } else if has("CELL") || has("MOBILE") {
        "Mobile"
    } else if has("HOME") {
        "Home"
    } else if has("WORK") {
        "Work"
    } else if has("MAIN") || has("MAIN-NUMBER") {
        "Main"
    } else if has("OTHER") {
        "Other"
    } else if has("PREF") && types.iter().all(|t| t == "PREF" || t == "VOICE") {
        "Main"
    } else {
        "Phone"
    }
}

/// Apple's custom labels name its own as "_$!<Mobile>!$_".
fn apple_label(label: &str) -> &str {
    let label = label.trim();
    label
        .strip_prefix("_$!<")
        .and_then(|l| l.strip_suffix(">!$_"))
        .unwrap_or(label)
}

/// A property line, its parameters read and its value decoded.
struct Property {
    /// "item1" of "item1.TEL"; "" for none.
    group: String,
    /// Upper case.
    name: String,
    /// TYPE values, and 2.1's bare parameters, upper case.
    types: Vec<String>,
    /// VALUE=uri
    uri: bool,
    /// The value after quoted-printable and charset decoding, its escapes
    /// still in.
    text: String,
}

/// Where a line's value starts: its first ':' outside a quoted parameter.
fn colon(line: &[u8]) -> Option<usize> {
    let mut quoted = false;
    for (i, &b) in line.iter().enumerate() {
        match b {
            b'"' => quoted = !quoted,
            b':' if !quoted => return Some(i),
            _ => {}
        }
    }
    None
}

/// The head of a line split at each ';' outside quotes.
fn split_params(head: &[u8]) -> Vec<&[u8]> {
    let mut out = Vec::new();
    let mut quoted = false;
    let mut start = 0;
    for (i, &b) in head.iter().enumerate() {
        match b {
            b'"' => quoted = !quoted,
            b';' if !quoted => {
                out.push(&head[start..i]);
                start = i + 1;
            }
            _ => {}
        }
    }
    out.push(&head[start..]);
    out
}

/// The property name of a line, without its group.
fn property_name(head: &[u8]) -> &[u8] {
    let end = head.iter().position(|&b| b == b';').unwrap_or(head.len());
    let name = &head[..end];
    match name.iter().rposition(|&b| b == b'.') {
        Some(i) => &name[i + 1..],
        None => name,
    }
}

impl Property {
    fn parse(line: &[u8]) -> Option<Property> {
        let at = colon(line)?;
        let (head, raw) = (&line[..at], &line[at + 1..]);
        let params = split_params(head);
        let first = params[0];
        let (group, name) = match first.iter().rposition(|&b| b == b'.') {
            Some(i) => (&first[..i], &first[i + 1..]),
            None => (&first[..0], first),
        };
        let mut types = Vec::new();
        let mut qp = false;
        let mut charset = None;
        let mut uri = false;
        for p in &params[1..] {
            let p = String::from_utf8_lossy(p);
            match p.split_once('=') {
                Some((key, value)) => {
                    let value = value.trim().trim_matches('"');
                    match key.trim().to_ascii_uppercase().as_str() {
                        "TYPE" => types.extend(
                            value
                                .split(',')
                                .map(|t| t.trim().trim_matches('"').to_ascii_uppercase())
                                .filter(|t| !t.is_empty()),
                        ),
                        "ENCODING" => match value.to_ascii_uppercase().as_str() {
                            "QUOTED-PRINTABLE" => qp = true,
                            "B" | "BASE64" => return None,
                            _ => {}
                        },
                        "CHARSET" => charset = Some(value.to_ascii_uppercase()),
                        "VALUE" => uri = value.eq_ignore_ascii_case("uri"),
                        "PREF" => types.push("PREF".to_string()),
                        _ => {}
                    }
                }
                None => match p.trim().to_ascii_uppercase().as_str() {
                    "QUOTED-PRINTABLE" => qp = true,
                    "BASE64" => return None,
                    "" => {}
                    t => types.push(t.to_string()),
                },
            }
        }
        let bytes = if qp { qp_decode(raw) } else { raw.to_vec() };
        Some(Property {
            group: String::from_utf8_lossy(group).into_owned(),
            name: String::from_utf8_lossy(name).to_ascii_uppercase(),
            types,
            uri,
            text: decode_charset(bytes, charset.as_deref()),
        })
    }
}

/// Whether a line is one the parser reads, and whether its value is
/// quoted-printable, whose soft line breaks join the next line on.
fn classify(line: &[u8]) -> (bool, bool) {
    const READ: [&[u8]; 7] = [b"BEGIN", b"END", b"FN", b"N", b"ORG", b"TEL", b"X-ABLABEL"];
    let head = &line[..colon(line).unwrap_or(line.len())];
    let name = property_name(head);
    let upper = head.to_ascii_uppercase();
    let has = |needle: &[u8]| upper.windows(needle.len()).any(|w| w == needle);
    let qp = has(b"QUOTED-PRINTABLE");
    let binary = has(b"ENCODING=B") || has(b";BASE64");
    let read = READ.iter().any(|r| name.eq_ignore_ascii_case(r));
    (read && !binary, qp)
}

/// A card being read.
#[derive(Default)]
struct Draft {
    full: Option<String>,
    structured: Option<String>,
    org: Option<String>,
    /// (group, number, label from its types)
    tels: Vec<(String, String, &'static str)>,
    /// (group, label): Apple's labels, which name the TEL of their group.
    labels: Vec<(String, String)>,
}

/// N: "Family;Given;Middle;Prefix;Suffix", each perhaps a list, as
/// "Prefix Given Middle Family Suffix".
fn compose_n(text: &str) -> String {
    let parts = split_unescaped(text, ';');
    let mut out = String::new();
    for i in [3, 1, 2, 0, 4] {
        let Some(part) = parts.get(i) else {
            continue;
        };
        for value in split_unescaped(part, ',') {
            let value = tidy(&unescape(&value));
            if value.is_empty() {
                continue;
            }
            if !out.is_empty() {
                out.push(' ');
            }
            out.push_str(&value);
        }
    }
    out
}

/// A TEL's number: the text, or the number of a "tel:" URI without its
/// parameters.
fn tel_number(p: &Property) -> String {
    let text = p.text.trim();
    let scheme = text.get(..4).is_some_and(|s| s.eq_ignore_ascii_case("tel:"));
    let text = if p.uri || scheme {
        let text = if scheme { &text[4..] } else { text };
        text.split(';').next().unwrap_or("")
    } else {
        text
    };
    tidy(&unescape(text))
}

impl Draft {
    fn add(&mut self, p: Property) {
        match p.name.as_str() {
            "FN" if self.full.is_none() => self.full = Some(unescape(&p.text)),
            "N" if self.structured.is_none() => self.structured = Some(compose_n(&p.text)),
            "ORG" if self.org.is_none() => {
                self.org = split_unescaped(&p.text, ';').first().map(|o| unescape(o));
            }
            "TEL" => {
                let number = tel_number(&p);
                self.tels.push((p.group, number, label_for(&p.types)));
            }
            "X-ABLABEL" => self.labels.push((p.group, unescape(&p.text))),
            _ => {}
        }
    }

    fn finish(self) -> Option<Card> {
        let mut numbers: Vec<(String, String)> = Vec::new();
        let mut seen: Vec<String> = Vec::new();
        for (group, number, label) in &self.tels {
            let digits = digit_string(number);
            if digits.is_empty() || seen.contains(&digits) {
                continue;
            }
            if numbers.len() == PB_NUMBERS_MAX {
                break;
            }
            let custom = self
                .labels
                .iter()
                .find(|(g, _)| !group.is_empty() && g.eq_ignore_ascii_case(group))
                .map(|(_, l)| tidy(apple_label(l)))
                .filter(|l| !l.is_empty());
            let label = custom.unwrap_or_else(|| label.to_string());
            numbers.push((
                cut(number, PB_NUMBER_MAX - 1).to_string(),
                cut(&label, PB_LABEL_MAX - 1).to_string(),
            ));
            seen.push(digits);
        }
        let name = [self.full, self.structured, self.org]
            .into_iter()
            .flatten()
            .map(|n| tidy(&n))
            .find(|n| !n.is_empty())
            .or_else(|| numbers.first().map(|(n, _)| n.clone()))?;
        Some(Card {
            name: cut(&name, PB_NAME_MAX - 1).trim_end().to_string(),
            numbers,
        })
    }
}

/// The next physical line into `out`, without its line ending, cut to
/// LINE_MAX; false at the end of the input.
fn next_line<R: BufRead>(r: &mut R, out: &mut Vec<u8>) -> io::Result<bool> {
    out.clear();
    let mut any = false;
    loop {
        let (used, done) = {
            let buf = match r.fill_buf() {
                Ok(buf) => buf,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(e) => return Err(e),
            };
            if buf.is_empty() {
                break;
            }
            any = true;
            let room = LINE_MAX.saturating_sub(out.len());
            match buf.iter().position(|&b| b == b'\n') {
                Some(i) => {
                    out.extend_from_slice(&buf[..i.min(room)]);
                    (i + 1, true)
                }
                None => {
                    out.extend_from_slice(&buf[..buf.len().min(room)]);
                    (buf.len(), false)
                }
            }
        };
        r.consume(used);
        if done {
            break;
        }
    }
    if out.last() == Some(&b'\r') {
        out.pop();
    }
    Ok(any)
}

fn append_bounded(to: &mut Vec<u8>, bytes: &[u8]) {
    let room = PROPERTY_MAX.saturating_sub(to.len());
    to.extend_from_slice(&bytes[..bytes.len().min(room)]);
}

/// One whole (unfolded) line.
fn handle(line: &[u8], draft: &mut Option<Draft>, take: &mut dyn FnMut(Card) -> bool) -> bool {
    let Some(p) = Property::parse(line) else {
        return true;
    };
    let vcard = p.text.trim().eq_ignore_ascii_case("VCARD");
    match p.name.as_str() {
        "BEGIN" if vcard => {
            // A card left without its END is taken as it stands.
            if let Some(card) = draft.replace(Draft::default()).and_then(Draft::finish) {
                return take(card);
            }
        }
        "END" if vcard => {
            if let Some(card) = draft.take().and_then(Draft::finish) {
                return take(card);
            }
        }
        _ => {
            if let Some(d) = draft.as_mut() {
                d.add(p);
            }
        }
    }
    true
}

/// Read every contact in a vCard stream, handing each to `take`, which says
/// whether to go on. Folded lines (a line break and a space or tab) and
/// quoted-printable soft line breaks ('=' at the end) are joined; lines of
/// properties not kept, photos among them, are dropped as they are read.
pub(crate) fn read_cards<R: BufRead>(mut r: R, take: &mut dyn FnMut(Card) -> bool) -> io::Result<()> {
    let mut phys = Vec::new();
    let mut logical = Vec::new();
    let mut pending = false;
    let mut keep = false;
    let mut qp = false;
    let mut soft = false;
    let mut first = true;
    let mut draft: Option<Draft> = None;
    loop {
        let more = next_line(&mut r, &mut phys)?;
        if first {
            first = false;
            if phys.starts_with(b"\xEF\xBB\xBF") {
                phys.drain(..3);
            }
        }
        if more && pending {
            if soft {
                if keep {
                    if logical.last() == Some(&b'=') {
                        logical.pop();
                    }
                    append_bounded(&mut logical, &phys);
                }
                soft = phys.last() == Some(&b'=');
                continue;
            }
            if matches!(phys.first(), Some(b' ' | b'\t')) {
                if keep {
                    append_bounded(&mut logical, &phys[1..]);
                }
                soft = qp && phys.last() == Some(&b'=');
                continue;
            }
        }
        // The line before is whole: this one starts another, or the input
        // ended.
        if pending {
            if keep && !handle(&logical, &mut draft, take) {
                return Ok(());
            }
        }
        if !more {
            break;
        }
        logical.clear();
        (keep, qp) = classify(&phys);
        if keep {
            append_bounded(&mut logical, &phys);
        }
        soft = qp && phys.last() == Some(&b'=');
        pending = true;
    }
    if let Some(card) = draft.take().and_then(Draft::finish) {
        take(card);
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// The book
// ---------------------------------------------------------------------------

/// A string in the book's arena.
#[derive(Clone, Copy, Debug, Default)]
struct Span {
    off: u32,
    len: u16,
}

#[derive(Clone, Copy, Debug)]
struct Entry {
    name: Span,
    folded: Span,
    /// Its numbers: `count` from `first` in the book's numbers.
    first: u32,
    count: u8,
    starred: bool,
}

#[derive(Clone, Copy, Debug)]
struct Number {
    number: Span,
    label: Span,
}

/// Room for at least `n` of T in one allocation of more than INTERNAL_MAX
/// bytes, so that on the device it is in PSRAM.
fn psram_vec<T>(n: usize) -> Vec<T> {
    let size = std::mem::size_of::<T>().max(1);
    Vec::with_capacity(n.max((INTERNAL_MAX + 1).div_ceil(size)))
}

fn push_text(text: &mut String, s: &str) -> Span {
    let s = cut(s, u16::MAX as usize);
    let span = Span { off: text.len() as u32, len: s.len() as u16 };
    text.push_str(s);
    span
}

/// The contacts, sorted by name.
pub(crate) struct Book {
    text: String,
    entries: Vec<Entry>,
    numbers: Vec<Number>,
}

/// The query of a search.
enum Query {
    All,
    Name(String),
    Digits(Vec<u8>),
}

impl Query {
    /// Digits when the query is at least two digits and nothing but digits,
    /// '+', spaces and dashes; otherwise a name.
    fn new(q: &str) -> Query {
        let q = q.trim();
        if q.is_empty() {
            return Query::All;
        }
        let digits = q.bytes().filter(u8::is_ascii_digit).count();
        if digits >= 2 && q.bytes().all(|b| b.is_ascii_digit() || matches!(b, b'+' | b' ' | b'-')) {
            Query::Digits(q.bytes().filter(u8::is_ascii_digit).collect())
        } else {
            Query::Name(fold(q))
        }
    }
}

/// Whether two numbers are the same, by their digits: the last 9, which
/// leaves out a country code or trunk prefix, so "+49 176 12345678" is
/// "0176 12345678"; all of them when either has fewer; never with fewer
/// than 3.
fn same_number(a: &[u8], b: &[u8]) -> bool {
    if a.len() < 3 || b.len() < 3 {
        return false;
    }
    if a.len() >= 9 && b.len() >= 9 {
        a[a.len() - 9..] == b[b.len() - 9..]
    } else {
        a == b
    }
}

impl Book {
    pub(crate) const EMPTY: Book = Book { text: String::new(), entries: Vec::new(), numbers: Vec::new() };

    fn s(&self, span: Span) -> &str {
        let start = span.off as usize;
        self.text.get(start..start + span.len as usize).unwrap_or("")
    }

    pub(crate) fn len(&self) -> usize {
        self.entries.len()
    }

    pub(crate) fn name(&self, i: usize) -> &str {
        self.entries.get(i).map_or("", |e| self.s(e.name))
    }

    pub(crate) fn starred(&self, i: usize) -> bool {
        self.entries.get(i).is_some_and(|e| e.starred)
    }

    /// Contact `i`'s numbers and their labels.
    pub(crate) fn numbers(&self, i: usize) -> impl Iterator<Item = (&str, &str)> {
        let range = self.entries.get(i).map_or(0..0, |e| e.first as usize..e.first as usize + e.count as usize);
        self.numbers[range].iter().map(|n| (self.s(n.number), self.s(n.label)))
    }

    fn matches(&self, i: usize, q: &Query) -> bool {
        match q {
            Query::All => true,
            Query::Name(folded) => self.s(self.entries[i].folded).contains(folded.as_str()),
            Query::Digits(want) => self.numbers(i).any(|(n, _)| {
                let mut buf = [0u8; PB_NUMBER_MAX];
                digits_into(n, &mut buf).windows(want.len()).any(|w| w == want.as_slice())
            }),
        }
    }

    /// The contacts matching `query`, in name order, each to `found`; how
    /// many there were.
    pub(crate) fn search(&self, query: &str, found: &mut dyn FnMut(usize)) -> usize {
        let q = Query::new(query);
        let mut n = 0;
        for i in 0..self.len() {
            if self.matches(i, &q) {
                found(i);
                n += 1;
            }
        }
        n
    }

    /// The first contact, in name order, with `number` among its numbers.
    pub(crate) fn find_number(&self, number: &str) -> Option<usize> {
        let mut qbuf = [0u8; PB_NUMBER_MAX];
        let q = digits_into(number, &mut qbuf);
        if q.len() < 3 {
            return None;
        }
        (0..self.len()).find(|&i| {
            self.numbers(i).any(|(n, _)| {
                let mut buf = [0u8; PB_NUMBER_MAX];
                same_number(q, digits_into(n, &mut buf))
            })
        })
    }

    /// Mark the contacts named in `stars`.
    fn apply_stars(&mut self, stars: &[String]) {
        for i in 0..self.entries.len() {
            let on = stars.iter().any(|s| s == self.s(self.entries[i].name));
            self.entries[i].starred = on;
        }
    }

    /// Star or unstar every contact named `name`: stars go by name.
    fn set_starred_by_name(&mut self, name: &str, on: bool) {
        for i in 0..self.entries.len() {
            if self.s(self.entries[i].name) == name {
                self.entries[i].starred = on;
            }
        }
    }
}

/// The book as it is read, in the order the cards came, with room to grow
/// in PSRAM. finish() sorts it and makes the book.
pub(crate) struct Builder {
    book: Book,
}

impl Builder {
    pub(crate) fn new() -> Builder {
        Builder {
            book: Book {
                text: String::with_capacity(32 * 1024),
                entries: psram_vec(1024),
                numbers: psram_vec(2048),
            },
        }
    }

    pub(crate) fn len(&self) -> usize {
        self.book.len()
    }

    pub(crate) fn push(&mut self, card: &Card) {
        let b = &mut self.book;
        let first = b.numbers.len() as u32;
        let name = push_text(&mut b.text, &card.name);
        let folded = push_text(&mut b.text, &fold(&card.name));
        for (number, label) in card.numbers.iter().take(PB_NUMBERS_MAX) {
            let number = push_text(&mut b.text, number);
            let label = push_text(&mut b.text, label);
            b.numbers.push(Number { number, label });
        }
        b.entries.push(Entry {
            name,
            folded,
            first,
            count: card.numbers.len().min(PB_NUMBERS_MAX) as u8,
            starred: false,
        });
    }

    /// The digits of contact `i`'s numbers, as a set.
    fn digit_set(&self, i: usize) -> Vec<String> {
        let mut set: Vec<String> = self.book.numbers(i).map(|(n, _)| digit_string(n)).collect();
        set.sort();
        set.dedup();
        set
    }

    /// Sorted by name, ignoring case and accents; a contact with the same
    /// name and the same numbers as one before it dropped; cut to
    /// PB_CONTACTS_MAX; packed into allocations of their own size.
    pub(crate) fn finish(self) -> Book {
        let src = &self.book;
        let n = src.len();
        let mut order: Vec<u32> = psram_vec(n);
        order.extend(0..n as u32);
        let folded = |i: u32| src.s(src.entries[i as usize].folded);
        let name = |i: u32| src.s(src.entries[i as usize].name);
        order.sort_by(|&a, &b| folded(a).cmp(folded(b)).then_with(|| name(a).cmp(name(b))));

        let mut kept: Vec<u32> = psram_vec(n.min(PB_CONTACTS_MAX));
        let mut group = 0;
        for &i in &order {
            if kept.len() == PB_CONTACTS_MAX {
                break;
            }
            if kept.last().is_some_and(|&k| folded(k) != folded(i)) {
                group = kept.len();
            }
            let set = self.digit_set(i as usize);
            if kept[group..].iter().any(|&k| self.digit_set(k as usize) == set) {
                continue;
            }
            kept.push(i);
        }

        let mut text_len = 0;
        let mut number_count = 0;
        for &i in &kept {
            let e = &src.entries[i as usize];
            text_len += e.name.len as usize + e.folded.len as usize;
            for k in e.first as usize..e.first as usize + e.count as usize {
                text_len += src.numbers[k].number.len as usize + src.numbers[k].label.len as usize;
            }
            number_count += e.count as usize;
        }
        let mut book = Book {
            text: String::with_capacity(text_len.max(INTERNAL_MAX + 1)),
            entries: psram_vec(kept.len()),
            numbers: psram_vec(number_count),
        };
        for &i in &kept {
            let e = src.entries[i as usize];
            let first = book.numbers.len() as u32;
            let name = push_text(&mut book.text, src.s(e.name));
            let folded = push_text(&mut book.text, src.s(e.folded));
            for k in e.first as usize..e.first as usize + e.count as usize {
                let number = push_text(&mut book.text, src.s(src.numbers[k].number));
                let label = push_text(&mut book.text, src.s(src.numbers[k].label));
                book.numbers.push(Number { number, label });
            }
            book.entries.push(Entry { name, folded, first, count: e.count, starred: false });
        }
        book
    }
}

/// Read the cards of every file in `paths` into a builder.
pub(crate) fn read_files(paths: &[PathBuf]) -> Builder {
    let mut builder = Builder::new();
    for path in paths {
        if builder.len() >= PARSE_MAX {
            break;
        }
        let Ok(file) = File::open(path) else {
            continue;
        };
        let _ = read_cards(BufReader::new(file), &mut |card| {
            builder.push(&card);
            builder.len() < PARSE_MAX
        });
    }
    builder
}

// ---------------------------------------------------------------------------
// The card
// ---------------------------------------------------------------------------

/// Every file named contacts.vcf, in any case, under `root`, down to
/// PB_SEARCH_DEPTH folders deep, in path order. Hidden folders and Windows'
/// "System Volume Information" are not looked in.
pub(crate) fn find_files(root: &Path) -> Vec<PathBuf> {
    // Folder by folder from a list rather than by recursion, so only one
    // folder is open at a time and the stack does not grow with the depth:
    // a directory entry holds a 256-byte name on ESP-IDF.
    let mut found = Vec::new();
    let mut folders = vec![(root.to_path_buf(), 0usize)];
    while let Some((dir, depth)) = folders.pop() {
        let Ok(entries) = std::fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let name = entry.file_name();
            let Some(name) = name.to_str() else {
                continue;
            };
            if name.starts_with('.') || name.eq_ignore_ascii_case("System Volume Information") {
                continue;
            }
            let Ok(kind) = entry.file_type() else {
                continue;
            };
            if kind.is_dir() {
                if depth < PB_SEARCH_DEPTH {
                    folders.push((entry.path(), depth + 1));
                }
            } else if kind.is_file() && name.eq_ignore_ascii_case("contacts.vcf") && found.len() < 4 * FILES_MAX {
                found.push(entry.path());
            }
        }
    }
    found.sort();
    found.truncate(FILES_MAX);
    found
}

/// What a file was when it was read, to tell whether it changed since.
#[derive(Clone, Debug, PartialEq)]
struct Stamp {
    path: PathBuf,
    /// None when it is gone.
    modified: Option<SystemTime>,
    size: Option<u64>,
}

fn stamp(path: &Path) -> Stamp {
    match std::fs::metadata(path) {
        Ok(m) => Stamp { path: path.to_path_buf(), modified: m.modified().ok(), size: Some(m.len()) },
        Err(_) => Stamp { path: path.to_path_buf(), modified: None, size: None },
    }
}

/// `{"starred":["Anna Smith",…]}`; none for a file missing or unreadable.
pub(crate) fn read_stars(path: &Path) -> Vec<String> {
    std::fs::read_to_string(path)
        .ok()
        .and_then(|json| serde_json::from_str::<serde_json::Value>(&json).ok())
        .and_then(|v| {
            v.get("starred")?
                .as_array()
                .map(|a| a.iter().filter_map(|n| n.as_str().map(str::to_string)).collect())
        })
        .unwrap_or_default()
}

pub(crate) fn write_stars(path: &Path, stars: &[String]) -> io::Result<()> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    std::fs::write(path, serde_json::json!({ "starred": stars }).to_string())
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Phonebook {
    book: Book,
    seq: u32,
    /// The card has been searched, or is being.
    started: bool,
    loading: bool,
    /// The files the last search found, as they were read.
    files: Vec<Stamp>,
    /// When the files were last read or looked at.
    last_look: Option<Instant>,
    /// The stars, once read from the card; from then on this is what is
    /// written back.
    stars: Option<Vec<String>>,
}

static PB: Mutex<Phonebook> = Mutex::new(Phonebook {
    book: Book::EMPTY,
    seq: 0,
    started: false,
    loading: false,
    files: Vec::new(),
    last_look: None,
    stars: None,
});

fn lock() -> MutexGuard<'static, Phonebook> {
    PB.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Read the card on a thread of its own: searched for files if `search`,
/// otherwise the files found before. ESP_ERR_INVALID_STATE while a read is
/// under way.
fn start_load(search: bool) -> i32 {
    let known = {
        let mut pb = lock();
        if pb.loading {
            return ESP_ERR_INVALID_STATE;
        }
        pb.loading = true;
        pb.started = true;
        (!search).then(|| pb.files.iter().map(|s| s.path.clone()).collect::<Vec<_>>())
    };
    #[cfg(target_os = "espidf")]
    unsafe {
        thistle_pthread_name_next(b"phonebook\0".as_ptr())
    };
    let spawned = std::thread::Builder::new()
        .name("phonebook".to_string())
        .stack_size(STACK_SIZE)
        .spawn(move || load(known))
        .is_ok();
    #[cfg(target_os = "espidf")]
    unsafe {
        thistle_pthread_name_next(std::ptr::null())
    };
    if !spawned {
        let mut pb = lock();
        pb.loading = false;
        pb.started = false;
        drop(pb);
        pb_log!(ESP_LOG_WARN, b"could not start reading the contacts\0");
        return ESP_ERR_NO_MEM;
    }
    ESP_OK
}

/// On the reading thread.
fn load(known: Option<Vec<PathBuf>>) {
    let root = card_root();
    let paths = known.unwrap_or_else(|| find_files(&root));
    // Looked at before they are read, so a change while they are is seen
    // at the next look.
    let stamps: Vec<Stamp> = paths.iter().map(|p| stamp(p)).collect();
    let present = stamps.iter().filter(|s| s.size.is_some()).count();
    let mut book = read_files(&paths).finish();
    let file_stars = read_stars(&stars_path(&root));
    let count = book.len();

    let mut pb = lock();
    let stars = pb.stars.get_or_insert(file_stars);
    book.apply_stars(stars);
    pb.book = book;
    pb.files = stamps;
    pb.seq = pb.seq.wrapping_add(1);
    pb.loading = false;
    pb.last_look = Some(Instant::now());
    drop(pb);

    if present == 0 {
        pb_log!(ESP_LOG_INFO, b"no contacts.vcf on the card\0");
    } else {
        pb_log!(ESP_LOG_INFO, b"%u contacts from %u files\0", count as u32, present as u32);
    }
    #[cfg(target_os = "espidf")]
    {
        let left = unsafe { uxTaskGetStackHighWaterMark(std::ptr::null_mut()) };
        if left < STACK_LOW_WATER {
            pb_log!(ESP_LOG_WARN, b"reading left %u of %u bytes of stack unused\0", left, STACK_SIZE as u32);
        }
    }
}

/// The first search, if none has run.
fn ensure_started() {
    let started = lock().started;
    if !started {
        start_load(true);
    }
}

/// Read the files again if any changed or went since they were read; not
/// more than every RECHECK, and never a search of the card.
fn look_for_changes() {
    let files = {
        let mut pb = lock();
        if !pb.started || pb.loading || pb.files.is_empty() {
            return;
        }
        let now = Instant::now();
        if pb.last_look.is_some_and(|t| now.duration_since(t) < RECHECK) {
            return;
        }
        pb.last_look = Some(now);
        pb.files.clone()
    };
    if files.iter().any(|s| stamp(&s.path) != *s) {
        start_load(false);
    }
}

/// The phonebook's name for `number`, for the call screen and the call log.
pub(crate) fn name_for(number: &str) -> Option<String> {
    ensure_started();
    let pb = lock();
    pb.book.find_number(number).map(|i| pb.book.name(i).to_string())
}

/// Changes whenever the contacts or their stars do.
pub(crate) fn seq() -> u32 {
    rs_phonebook_seq()
}

// ---------------------------------------------------------------------------
// FFI — thistle/phonebook.h
// ---------------------------------------------------------------------------

/// pb_number_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CPbNumber {
    pub number: [u8; PB_NUMBER_MAX],
    pub label: [u8; PB_LABEL_MAX],
}

/// pb_contact_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CPbContact {
    pub name: [u8; PB_NAME_MAX],
    pub starred: bool,
    pub number_count: u8,
    pub numbers: [CPbNumber; PB_NUMBERS_MAX],
}

unsafe fn c_arg<'a>(s: *const c_char) -> Option<std::borrow::Cow<'a, str>> {
    (!s.is_null()).then(|| std::ffi::CStr::from_ptr(s).to_string_lossy())
}

/// Search the card again and read what it finds; returns at once.
#[no_mangle]
pub extern "C" fn rs_phonebook_rescan() -> i32 {
    start_load(true)
}

#[no_mangle]
pub extern "C" fn rs_phonebook_loading() -> bool {
    lock().loading
}

#[no_mangle]
pub extern "C" fn rs_phonebook_seq() -> u32 {
    ensure_started();
    look_for_changes();
    lock().seq
}

#[no_mangle]
pub extern "C" fn rs_phonebook_file_count() -> u32 {
    lock().files.len() as u32
}

/// # Safety
/// `path` must point to `size` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn rs_phonebook_first_file(path: *mut c_char, size: usize) -> bool {
    if path.is_null() || size == 0 {
        return false;
    }
    let pb = lock();
    let Some(first) = pb.files.first() else {
        return false;
    };
    copy_c(
        std::slice::from_raw_parts_mut(path as *mut u8, size),
        &first.path.to_string_lossy(),
    );
    true
}

#[no_mangle]
pub extern "C" fn rs_phonebook_count() -> u32 {
    ensure_started();
    lock().book.len() as u32
}

/// # Safety
/// `out` must be NULL or point to a writable pb_contact_t.
#[no_mangle]
pub unsafe extern "C" fn rs_phonebook_get(index: u32, out: *mut CPbContact) -> bool {
    if out.is_null() {
        return false;
    }
    ensure_started();
    let pb = lock();
    let i = index as usize;
    if i >= pb.book.len() {
        return false;
    }
    let out = &mut *out;
    copy_c(&mut out.name, pb.book.name(i));
    out.starred = pb.book.starred(i);
    out.number_count = 0;
    for (k, (number, label)) in pb.book.numbers(i).take(PB_NUMBERS_MAX).enumerate() {
        copy_c(&mut out.numbers[k].number, number);
        copy_c(&mut out.numbers[k].label, label);
        out.number_count = k as u8 + 1;
    }
    for k in out.number_count as usize..PB_NUMBERS_MAX {
        out.numbers[k].number[0] = 0;
        out.numbers[k].label[0] = 0;
    }
    true
}

/// # Safety
/// `query` must be NULL or a NUL-terminated string; `indices` NULL or room
/// for `max` indices.
#[no_mangle]
pub unsafe extern "C" fn rs_phonebook_search(query: *const c_char, indices: *mut u32, max: u32) -> u32 {
    ensure_started();
    let query = c_arg(query).unwrap_or_default();
    let pb = lock();
    let mut n = 0usize;
    pb.book.search(&query, &mut |i| {
        if !indices.is_null() && n < max as usize {
            *indices.add(n) = i as u32;
        }
        n += 1;
    }) as u32
}

/// # Safety
/// `indices` must be NULL or have room for `max` indices.
#[no_mangle]
pub unsafe extern "C" fn rs_phonebook_starred(indices: *mut u32, max: u32) -> u32 {
    ensure_started();
    let pb = lock();
    let mut n = 0u32;
    for i in 0..pb.book.len() {
        if pb.book.starred(i) {
            if !indices.is_null() && n < max {
                *indices.add(n as usize) = i as u32;
            }
            n += 1;
        }
    }
    n
}

/// Star or unstar a contact, and every other of the same name: stars go by
/// name. Saved to the card at once; the star holds even if that fails, which
/// the return says.
#[no_mangle]
pub extern "C" fn rs_phonebook_set_starred(index: u32, starred: bool) -> i32 {
    let stars = {
        let mut pb = lock();
        let i = index as usize;
        if i >= pb.book.len() {
            return ESP_ERR_INVALID_ARG;
        }
        if pb.book.starred(i) == starred {
            return ESP_OK;
        }
        let name = pb.book.name(i).to_string();
        pb.book.set_starred_by_name(&name, starred);
        let stars = pb.stars.get_or_insert_with(Vec::new);
        stars.retain(|s| *s != name);
        if starred {
            stars.push(name);
        }
        let stars = stars.clone();
        pb.seq = pb.seq.wrapping_add(1);
        stars
    };
    match write_stars(&stars_path(&card_root()), &stars) {
        Ok(()) => ESP_OK,
        Err(_) => {
            pb_log!(ESP_LOG_WARN, b"could not save the stars\0");
            ESP_FAIL
        }
    }
}

/// # Safety
/// `number` must be NULL or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn rs_phonebook_find_number(number: *const c_char) -> i32 {
    let Some(number) = c_arg(number) else {
        return -1;
    };
    ensure_started();
    lock().book.find_number(&number).map_or(-1, |i| i as i32)
}

/// # Safety
/// `number` must be NULL or a NUL-terminated string; `name` must point to
/// `size` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn rs_phonebook_name_for(number: *const c_char, name: *mut c_char, size: usize) -> bool {
    if name.is_null() || size == 0 {
        return false;
    }
    let Some(number) = c_arg(number) else {
        return false;
    };
    match name_for(&number) {
        Some(n) => {
            copy_c(std::slice::from_raw_parts_mut(name as *mut u8, size), &n);
            true
        }
        None => false,
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn cards(text: &str) -> Vec<Card> {
        let mut out = Vec::new();
        read_cards(text.as_bytes(), &mut |c| {
            out.push(c);
            true
        })
        .unwrap();
        out
    }

    fn one(text: &str) -> Card {
        let mut all = cards(text);
        assert_eq!(all.len(), 1, "{all:?}");
        all.remove(0)
    }

    fn nums(c: &Card) -> Vec<(&str, &str)> {
        c.numbers.iter().map(|(n, l)| (n.as_str(), l.as_str())).collect()
    }

    fn book(text: &str) -> Book {
        let mut b = Builder::new();
        read_cards(text.as_bytes(), &mut |c| {
            b.push(&c);
            true
        })
        .unwrap();
        b.finish()
    }

    fn names(b: &Book) -> Vec<&str> {
        (0..b.len()).map(|i| b.name(i)).collect()
    }

    fn search<'a>(b: &'a Book, q: &str) -> Vec<&'a str> {
        let mut found = Vec::new();
        b.search(q, &mut |i| found.push(i));
        found.into_iter().map(|i| b.name(i)).collect()
    }

    // ── Lines ────────────────────────────────────────────────────────────

    #[test]
    fn folded_lines_are_joined_crlf_or_lf() {
        let crlf = "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Anna Maria\r\n  Smith\r\nTEL;TYPE=CELL:+49 176\r\n\t1234567\r\nEND:VCARD\r\n";
        let c = one(crlf);
        assert_eq!(c.name, "Anna Maria Smith");
        assert_eq!(nums(&c), [("+49 1761234567", "Mobile")]);
        let lf = crlf.replace("\r\n", "\n");
        assert_eq!(one(&lf), c);
    }

    #[test]
    fn a_line_too_long_is_cut_not_held() {
        let mut r = std::io::Cursor::new(format!("{}\nnext\n", "x".repeat(100_000)));
        let mut line = Vec::new();
        assert!(next_line(&mut r, &mut line).unwrap());
        assert_eq!(line.len(), LINE_MAX);
        assert!(next_line(&mut r, &mut line).unwrap());
        assert_eq!(line, b"next");
        assert!(!next_line(&mut r, &mut line).unwrap());
    }

    // ── What phones export ───────────────────────────────────────────────

    #[test]
    fn android_2_1_quoted_printable_names() {
        let c = one(
            "BEGIN:VCARD\r\n\
             VERSION:2.1\r\n\
             N;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:=4D=C3=BC=6C=6C=65=72;=4A=C3=BC=72=67=65=6E;;;\r\n\
             FN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:=4A=C3=BC=72=67=65=6E\r\n\
             TEL;CELL;PREF:+491761234567\r\n\
             TEL;HOME:030 1234567\r\n\
             TEL;WORK;FAX:030 7654321\r\n\
             END:VCARD\r\n",
        );
        assert_eq!(c.name, "Jürgen");
        assert_eq!(
            nums(&c),
            [("+491761234567", "Mobile"), ("030 1234567", "Home"), ("030 7654321", "Fax")]
        );
    }

    #[test]
    fn quoted_printable_soft_line_breaks_continue_the_value() {
        let c = one(
            "BEGIN:VCARD\nVERSION:2.1\n\
             FN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:=4A=C3=BC=72=67=65=6E =\n\
             =4D=C3=BC=6C=6C=65=72\n\
             NOTE;ENCODING=QUOTED-PRINTABLE:a long note=\n\
             TEL:not a property\n\
             TEL:0301111\n\
             END:VCARD\n",
        );
        assert_eq!(c.name, "Jürgen Müller");
        // The note's continuation is part of the note, not a number.
        assert_eq!(nums(&c), [("0301111", "Phone")]);
    }

    #[test]
    fn latin1_cards_are_read_as_such() {
        let mut bytes = b"BEGIN:VCARD\nVERSION:2.1\nFN;CHARSET=ISO-8859-1:J".to_vec();
        bytes.extend_from_slice(&[0xFC]); // ü
        bytes.extend_from_slice(b"rgen\nTEL:1234\nEND:VCARD\n");
        // And one that says nothing, which is not UTF-8 either.
        bytes.extend_from_slice(b"BEGIN:VCARD\nFN:Ren");
        bytes.extend_from_slice(&[0xE9]); // é
        bytes.extend_from_slice(b"e \x80\nEND:VCARD\n");
        // Quoted-printable in Latin-1.
        bytes.extend_from_slice(b"BEGIN:VCARD\nFN;CHARSET=ISO-8859-1;ENCODING=QUOTED-PRINTABLE:Fran=E7ois\nEND:VCARD\n");
        let mut out = Vec::new();
        read_cards(&bytes[..], &mut |c| {
            out.push(c.name);
            true
        })
        .unwrap();
        assert_eq!(out, ["Jürgen", "Renée €", "François"]);
    }

    #[test]
    fn ios_3_0_with_item_groups_and_custom_labels() {
        let c = one(
            "BEGIN:VCARD\r\n\
             VERSION:3.0\r\n\
             PRODID:-//Apple Inc.//iPhone OS 17.0//EN\r\n\
             N:Smith;Anna;;;\r\n\
             FN:Anna Smith\r\n\
             ORG:Acme;\r\n\
             item1.TEL;type=pref:+49 176 1234567\r\n\
             item1.X-ABLabel:_$!<Mobile>!$_\r\n\
             TEL;type=IPHONE;type=CELL;type=VOICE:+49 151 7654321\r\n\
             item2.TEL:+49 30 555\r\n\
             item2.X-ABLabel:Oma's Festnetz\r\n\
             TEL;type=HOME;type=VOICE:+49 30 1234567\r\n\
             TEL;type=MAIN:+49 30 9999\r\n\
             item3.EMAIL;type=INTERNET:anna@example.com\r\n\
             item3.X-ABLabel:_$!<Other>!$_\r\n\
             END:VCARD\r\n",
        );
        assert_eq!(c.name, "Anna Smith");
        assert_eq!(
            nums(&c),
            [
                ("+49 176 1234567", "Mobile"),
                ("+49 151 7654321", "iPhone"),
                ("+49 30 555", "Oma's Festn"), // cut to PB_LABEL_MAX - 1 bytes
                ("+49 30 1234567", "Home"),
                ("+49 30 9999", "Main"),
            ]
        );
    }

    #[test]
    fn google_3_0_export() {
        let c = one(
            "BEGIN:VCARD\r\n\
             VERSION:3.0\r\n\
             FN:Bob O'Neil\r\n\
             N:O'Neil;Bob;;;\r\n\
             EMAIL;TYPE=INTERNET;TYPE=HOME:bob@example.com\r\n\
             TEL;TYPE=CELL:+1 555-123-4567\r\n\
             TEL;TYPE=WORK:+1 (555) 765-4321\r\n\
             TEL;TYPE=OTHER:555 0000\r\n\
             TEL:+1 555 999 0000\r\n\
             ADR;TYPE=HOME:;;1 Main St\\, Apt 2;Springfield;;;USA\r\n\
             NOTE:Line one\\nline two\\, still\r\n\
             END:VCARD\r\n",
        );
        assert_eq!(c.name, "Bob O'Neil");
        assert_eq!(
            nums(&c),
            [
                ("+1 555-123-4567", "Mobile"),
                ("+1 (555) 765-4321", "Work"),
                ("555 0000", "Other"),
                ("+1 555 999 0000", "Phone"),
            ]
        );
    }

    #[test]
    fn vcard_4_0_tel_uris_and_quoted_types() {
        let c = one(
            "BEGIN:VCARD\nVERSION:4.0\nFN:Carla\n\
             TEL;VALUE=uri;TYPE=\"voice,cell\";PREF=1:tel:+49-176-1234567;ext=12\n\
             TEL;VALUE=uri;TYPE=home:tel:+49-30-1234\n\
             TEL;TYPE=work:TEL:+49301111\n\
             END:VCARD\n",
        );
        assert_eq!(c.name, "Carla");
        assert_eq!(
            nums(&c),
            [("+49-176-1234567", "Mobile"), ("+49-30-1234", "Home"), ("+49301111", "Work")]
        );
    }

    #[test]
    fn every_form_of_parameters() {
        let c = one(
            "BEGIN:VCARD\nVERSION:3.0\nFN:P\n\
             TEL;TYPE=CELL,VOICE:111\n\
             TEL;HOME;PREF:222\n\
             TEL;type=WORK;type=pref:333\n\
             TEL;PREF:444\n\
             TEL;TYPE=PAGER:555\n\
             TEL;type=main-number:666\n\
             TEL:777\n\
             END:VCARD\n",
        );
        // At most PB_NUMBERS_MAX, in the order written.
        assert_eq!(
            nums(&c),
            [("111", "Mobile"), ("222", "Home"), ("333", "Work"), ("444", "Main"), ("555", "Pager"), ("666", "Main")]
        );
    }

    #[test]
    fn a_name_from_n_when_there_is_no_fn() {
        let c = one("BEGIN:VCARD\nVERSION:3.0\nN:Smith;John;Quincy;Dr.;Jr.\nTEL:123\nEND:VCARD\n");
        assert_eq!(c.name, "Dr. John Quincy Smith Jr.");
        let c = one("BEGIN:VCARD\nVERSION:3.0\nN:Doe;Jane;;;\nFN:\nEND:VCARD\n");
        assert_eq!(c.name, "Jane Doe");
        assert!(c.numbers.is_empty());
        // Escaped separators are part of the name.
        let c = one("BEGIN:VCARD\nN:Smith\\, Jr.;Ann\\;e;;;\nEND:VCARD\n");
        assert_eq!(c.name, "Ann;e Smith, Jr.");
    }

    #[test]
    fn a_name_from_org_then_from_the_number() {
        let c = one("BEGIN:VCARD\nVERSION:3.0\nORG:Pizza Roma;Delivery\nTEL;TYPE=WORK:030 1234\nEND:VCARD\n");
        assert_eq!(c.name, "Pizza Roma");
        let c = one("BEGIN:VCARD\nVERSION:3.0\nTEL;TYPE=CELL:+49 176 555\nEND:VCARD\n");
        assert_eq!(c.name, "+49 176 555");
        // Nothing to name it by: no contact.
        assert!(cards("BEGIN:VCARD\nVERSION:3.0\nEMAIL:x@y.z\nEND:VCARD\n").is_empty());
    }

    #[test]
    fn photos_are_skipped_whatever_their_form() {
        let mut text = String::from("BEGIN:VCARD\r\nVERSION:2.1\r\nFN:Pic\r\nPHOTO;ENCODING=BASE64;TYPE=JPEG:/9j/4AAQSkZJRgABAQ\r\n");
        for _ in 0..2000 {
            text.push_str(" AAAAjgESAAMAAAABAAEAAAEaAAUAAAABAAAAlAEbAAUAAAABAAAAnAEoAAMAAAABAAIAAAEx\r\n");
        }
        text.push_str("\r\nTEL;CELL:0176 1\r\nEND:VCARD\r\n");
        text.push_str("BEGIN:VCARD\nVERSION:3.0\nFN:Pic3\nPHOTO;ENCODING=b;TYPE=JPEG:/9j/4AAQ\n =ABC:DEF\nTEL:0176 2\nEND:VCARD\n");
        text.push_str(&format!(
            "BEGIN:VCARD\nVERSION:4.0\nFN:Pic4\nPHOTO:data:image/jpeg;base64,{}\nTEL:0176 3\nEND:VCARD\n",
            "QUJD".repeat(50_000)
        ));
        let all = cards(&text);
        let got: Vec<(&str, &str)> = all.iter().map(|c| (c.name.as_str(), c.numbers[0].0.as_str())).collect();
        assert_eq!(got, [("Pic", "0176 1"), ("Pic3", "0176 2"), ("Pic4", "0176 3")]);
    }

    #[test]
    fn case_does_not_matter_and_a_missing_end_is_forgiven() {
        let all = cards(
            "begin:vcard\nversion:3.0\nfn:lower\ntel;type=cell:1\nend:vcard\n\
             BEGIN:VCARD\nFN:No end\nTEL:2\n\
             BEGIN:VCARD\nFN:Last\nTEL:3\n",
        );
        let got: Vec<(&str, Vec<(&str, &str)>)> = all.iter().map(|c| (c.name.as_str(), nums(c))).collect();
        assert_eq!(
            got,
            [("lower", vec![("1", "Mobile")]), ("No end", vec![("2", "Phone")]), ("Last", vec![("3", "Phone")])]
        );
    }

    #[test]
    fn a_byte_order_mark_is_ignored() {
        assert_eq!(one("\u{feff}BEGIN:VCARD\nFN:Bom\nEND:VCARD\n").name, "Bom");
    }

    #[test]
    fn numbers_are_deduplicated_by_digits_and_capped() {
        let c = one(
            "BEGIN:VCARD\nFN:Many\n\
             TEL;CELL:+49 176 1\nTEL;HOME:+49-176-1\nTEL:no digits\n\
             TEL:2\nTEL:3\nTEL:4\nTEL:5\nTEL:6\nTEL:7\nEND:VCARD\n",
        );
        assert_eq!(c.numbers.len(), PB_NUMBERS_MAX);
        assert_eq!(c.numbers[0], ("+49 176 1".into(), "Mobile".into()));
        assert_eq!(c.numbers[1].0, "2");
    }

    #[test]
    fn long_strings_are_cut_at_a_character() {
        let c = one(&format!("BEGIN:VCARD\nFN:{}\nTEL:{}\nEND:VCARD\n", "ü".repeat(40), "1".repeat(50)));
        assert_eq!(c.name, "ü".repeat(31));
        assert_eq!(c.numbers[0].0.len(), PB_NUMBER_MAX - 1);
    }

    // ── The book ─────────────────────────────────────────────────────────

    const TWO: &str = "BEGIN:VCARD\nFN:Émile Zola\nTEL:+33 1 2345\nEND:VCARD\n\
                       BEGIN:VCARD\nFN:anna smith\nTEL;CELL:+49 176 12345678\nEND:VCARD\n\
                       BEGIN:VCARD\nFN:Zoë\nTEL:0300\nEND:VCARD\n\
                       BEGIN:VCARD\nFN:Ölaf\nTEL:0400\nEND:VCARD\n\
                       BEGIN:VCARD\nFN:Bob\nTEL:+1 555 0100\nTEL:+1 555 0199\nEND:VCARD\n";

    #[test]
    fn sorted_by_name_ignoring_case_and_accents() {
        let b = book(TWO);
        assert_eq!(names(&b), ["anna smith", "Bob", "Émile Zola", "Ölaf", "Zoë"]);
        assert_eq!(fold("ÀÉÎÕÜ çñý ß Æ Œ Ø"), "aeiou cny ss ae oe o");
        assert_eq!(fold("Ğ Москва"), "ğ москва");
    }

    #[test]
    fn duplicates_across_files_are_listed_once() {
        let dir = temp_dir("dupes");
        std::fs::write(
            dir.join("a.vcf"),
            "BEGIN:VCARD\nFN:Anna Smith\nTEL:+49 176 1\nEND:VCARD\nBEGIN:VCARD\nFN:Bob\nEND:VCARD\n",
        )
        .unwrap();
        std::fs::write(
            dir.join("b.vcf"),
            "BEGIN:VCARD\nFN:anna smith\nTEL;CELL:+49-176-1\nEND:VCARD\n\
             BEGIN:VCARD\nFN:Anna Smith\nTEL:+49 176 2\nEND:VCARD\n\
             BEGIN:VCARD\nFN:Bob\nEND:VCARD\n",
        )
        .unwrap();
        let b = read_files(&[dir.join("a.vcf"), dir.join("b.vcf")]).finish();
        // Same name, same numbers: one. Same name, other numbers: two.
        assert_eq!(names(&b), ["Anna Smith", "Anna Smith", "Bob"]);
        let numbers: Vec<Vec<(&str, &str)>> = (0..2).map(|i| b.numbers(i).collect()).collect();
        assert!(numbers.contains(&vec![("+49 176 1", "Phone")]));
        assert!(numbers.contains(&vec![("+49 176 2", "Phone")]));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_book_is_capped_and_lives_in_large_allocations() {
        let mut builder = Builder::new();
        for i in 0..PB_CONTACTS_MAX + 50 {
            builder.push(&Card { name: format!("C{i:05}"), numbers: vec![(format!("{i}"), "Phone".into())] });
        }
        let b = builder.finish();
        assert_eq!(b.len(), PB_CONTACTS_MAX);
        assert_eq!(b.name(0), "C00000");
        assert!(b.text.capacity() > INTERNAL_MAX);
        assert!(b.entries.capacity() * std::mem::size_of::<Entry>() > INTERNAL_MAX);
        assert!(b.numbers.capacity() * std::mem::size_of::<Number>() > INTERNAL_MAX);
        let small = book("BEGIN:VCARD\nFN:One\nEND:VCARD\n");
        assert!(small.entries.capacity() * std::mem::size_of::<Entry>() > INTERNAL_MAX);
    }

    #[test]
    fn search_by_name_or_by_digits() {
        let b = book(TWO);
        assert_eq!(search(&b, "emile"), ["Émile Zola"]);
        assert_eq!(search(&b, "ZO"), ["Émile Zola", "Zoë"]);
        assert_eq!(search(&b, " olaf "), ["Ölaf"]);
        assert_eq!(search(&b, "17612"), ["anna smith"]);
        assert_eq!(search(&b, "+1 555-01"), ["Bob"]);
        assert_eq!(search(&b, "0199"), ["Bob"]);
        assert_eq!(search(&b, "00"), ["Bob", "Ölaf", "Zoë"]);
        // One digit is a name search, which no name holds.
        assert!(search(&b, "5").is_empty());
        assert_eq!(search(&b, "").len(), 5);
        assert!(search(&b, "nobody").is_empty());
    }

    #[test]
    fn a_number_is_found_by_its_last_nine_digits() {
        let b = book(TWO);
        assert_eq!(b.find_number("017612345678").map(|i| b.name(i)), Some("anna smith"));
        assert_eq!(b.find_number("+4917612345678").map(|i| b.name(i)), Some("anna smith"));
        assert_eq!(b.find_number("0049 176 12345678").map(|i| b.name(i)), Some("anna smith"));
        // Fewer than 9 digits: all of them must agree.
        assert_eq!(b.find_number("0300").map(|i| b.name(i)), Some("Zoë"));
        assert_eq!(b.find_number("300"), None);
        assert_eq!(b.find_number("12"), None);
        assert_eq!(b.find_number(""), None);
        assert!(same_number(b"4917612345678", b"017612345678"));
        assert!(!same_number(b"4917612345678", b"4917612345679"));
    }

    // ── The card ─────────────────────────────────────────────────────────

    fn temp_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("thistle_pb_{}_{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn the_card_is_searched_four_folders_deep_skipping_hidden_ones() {
        let root = temp_dir("walk");
        let put = |rel: &str| {
            let p = root.join(rel);
            std::fs::create_dir_all(p.parent().unwrap()).unwrap();
            std::fs::write(&p, "BEGIN:VCARD\nFN:x\nEND:VCARD\n").unwrap();
        };
        put("contacts.vcf");
        put("Backup/Contacts.VCF");
        put("a/b/c/d/CONTACTS.vcf");
        put("a/b/c/d/e/contacts.vcf");
        put(".hidden/contacts.vcf");
        put("System Volume Information/contacts.vcf");
        put("other.vcf");
        std::fs::create_dir_all(root.join("contacts.vcf.d")).unwrap();
        let found: Vec<String> = find_files(&root)
            .iter()
            .map(|p| p.strip_prefix(&root).unwrap().to_string_lossy().into_owned())
            .collect();
        assert_eq!(found, ["Backup/Contacts.VCF", "a/b/c/d/CONTACTS.vcf", "contacts.vcf"]);
        assert!(find_files(&root.join("missing")).is_empty());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn stars_round_trip_through_their_file() {
        let dir = temp_dir("stars");
        let path = stars_path(&dir);
        assert!(read_stars(&path).is_empty());
        write_stars(&path, &["Anna Smith".into(), "Zoë \"Z\"".into()]).unwrap();
        assert_eq!(read_stars(&path), ["Anna Smith", "Zoë \"Z\""]);
        std::fs::write(&path, "{ not json").unwrap();
        assert!(read_stars(&path).is_empty());
        let _ = std::fs::remove_dir_all(&dir);
    }

    fn wait_loaded() {
        let t0 = Instant::now();
        while rs_phonebook_loading() {
            assert!(t0.elapsed() < Duration::from_secs(10), "the phonebook never finished reading");
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    fn reload() {
        wait_loaded();
        assert_eq!(rs_phonebook_rescan(), ESP_OK);
        wait_loaded();
    }

    fn get(i: u32) -> (String, bool, Vec<(String, String)>) {
        let mut c: CPbContact = unsafe { std::mem::zeroed() };
        assert!(unsafe { rs_phonebook_get(i, &mut c) });
        let s = |b: &[u8]| String::from_utf8(b[..b.iter().position(|&x| x == 0).unwrap()].to_vec()).unwrap();
        let numbers = (0..c.number_count as usize).map(|k| (s(&c.numbers[k].number), s(&c.numbers[k].label))).collect();
        (s(&c.name), c.starred, numbers)
    }

    #[test]
    fn the_phonebook_reads_the_card_keeps_stars_and_follows_changes() {
        let root = card_root();
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join("Contacts")).unwrap();
        let file = root.join("Contacts").join("contacts.vcf");
        std::fs::write(&file, TWO).unwrap();
        lock().stars = None;
        reload();
        assert_eq!(rs_phonebook_count(), 5);
        assert_eq!(rs_phonebook_file_count(), 1);
        let mut path = [0u8; 256];
        assert!(unsafe { rs_phonebook_first_file(path.as_mut_ptr() as *mut c_char, path.len()) });
        assert!(String::from_utf8_lossy(&path).starts_with(&*file.to_string_lossy()));
        assert_eq!(get(0), ("anna smith".into(), false, vec![("+49 176 12345678".into(), "Mobile".into())]));
        assert!(!unsafe { rs_phonebook_get(5, &mut std::mem::zeroed()) });

        // Search through the C interface: the count of all, the indices of
        // as many as there is room for.
        let mut idx = [u32::MAX; 2];
        let q = b"o\0".as_ptr() as *const c_char;
        assert_eq!(unsafe { rs_phonebook_search(q, idx.as_mut_ptr(), 2) }, 4);
        assert_eq!(idx, [1, 2]);
        assert_eq!(unsafe { rs_phonebook_search(std::ptr::null(), std::ptr::null_mut(), 0) }, 5);
        assert_eq!(unsafe { rs_phonebook_find_number(b"+4917612345678\0".as_ptr() as *const c_char) }, 0);
        assert_eq!(unsafe { rs_phonebook_find_number(b"999\0".as_ptr() as *const c_char) }, -1);
        let mut name = [0xffu8; 8];
        assert!(unsafe {
            rs_phonebook_name_for(b"0176 12345678\0".as_ptr() as *const c_char, name.as_mut_ptr() as *mut c_char, 8)
        });
        assert_eq!(&name, b"anna sm\0");

        // Stars, by name, saved to the card and read back after a restart.
        let seq = rs_phonebook_seq();
        assert_eq!(rs_phonebook_set_starred(4, true), ESP_OK);
        assert_eq!(rs_phonebook_set_starred(1, true), ESP_OK);
        assert_ne!(rs_phonebook_seq(), seq);
        let mut idx = [0u32; 4];
        assert_eq!(unsafe { rs_phonebook_starred(idx.as_mut_ptr(), 4) }, 2);
        assert_eq!(&idx[..2], [1, 4]);
        assert_eq!(read_stars(&stars_path(&root)), ["Zoë", "Bob"]);
        assert_eq!(rs_phonebook_set_starred(4, false), ESP_OK);
        assert_eq!(read_stars(&stars_path(&root)), ["Bob"]);
        assert_eq!(rs_phonebook_set_starred(99, true), ESP_ERR_INVALID_ARG);
        lock().stars = None;
        reload();
        assert!(get(1).1, "Bob's star is read back from the card");

        // A changed file is read again at the next look, without a search.
        let seq = rs_phonebook_seq();
        std::fs::write(&file, format!("{TWO}BEGIN:VCARD\nFN:Neu\nTEL:1234\nEND:VCARD\n")).unwrap();
        lock().last_look = Some(Instant::now() - RECHECK);
        rs_phonebook_seq();
        wait_loaded();
        assert_ne!(rs_phonebook_seq(), seq);
        assert_eq!(rs_phonebook_count(), 6);

        // A vanished one empties the book.
        std::fs::remove_file(&file).unwrap();
        lock().last_look = Some(Instant::now() - RECHECK);
        rs_phonebook_seq();
        wait_loaded();
        assert_eq!(rs_phonebook_count(), 0);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn the_c_structs_match_their_layout() {
        assert_eq!(std::mem::size_of::<CPbNumber>(), 45);
        assert_eq!(std::mem::offset_of!(CPbNumber, label), 33);
        assert_eq!(std::mem::offset_of!(CPbContact, starred), 64);
        assert_eq!(std::mem::offset_of!(CPbContact, number_count), 65);
        assert_eq!(std::mem::offset_of!(CPbContact, numbers), 66);
        assert_eq!(std::mem::size_of::<CPbContact>(), 336);
    }
}
