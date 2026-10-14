// SPDX-License-Identifier: BSD-3-Clause
//! The kernel's side of SMS.
//!
//! The modem keeps a received SMS in its own storage until it is fetched.
//! The network coordinator fetches whenever the cellular driver says one may
//! be waiting (`sms_due`), on its own thread, whether or not an app is open.
//! Each message is recorded in the message history (msg_log) and queued
//! here for apps, as mesh_manager queues MeshCore's; only then does the modem
//! delete it.
//!
//! A long message arrives as parts, each its own SMS, in any order. They are
//! put back together here and delivered once whole. Parts still missing an
//! hour after the first arrived are given up on, and what came is delivered
//! with "[...]" where the rest would be. A restart loses parts held here.

use std::collections::VecDeque;
use std::os::raw::{c_char, c_void};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use crate::msg_log::{self, Direction, Entry, Transport};
use crate::net_manager::{CHalSms, HalNetDriver};

const ESP_OK: i32 = 0;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_ERR_NOT_SUPPORTED: i32 = 0x106;

const INBOX_SIZE: usize = 32;
/// Long messages being put back together at once; the oldest is delivered
/// as it stands to make room.
const ASSEMBLING_MAX: usize = 8;
const PART_TIMEOUT: Duration = Duration::from_secs(3600);
/// Room for the text of a message in sms_message_t: 6 parts of 153
/// characters, most of them one byte each.
pub const SMS_TEXT_MAX: usize = 1024;
pub const SMS_SENDER_MAX: usize = 33;

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

macro_rules! sms_log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {{
        #[cfg(not(test))]
        unsafe {
            esp_log_write($level, b"sms\0".as_ptr(), $fmt.as_ptr() $(, $arg)*);
        }
        #[cfg(test)]
        {
            let _ = ($level, $fmt $(, $arg)*);
        }
    }};
}

const ESP_LOG_WARN: i32 = 2;
const ESP_LOG_INFO: i32 = 3;

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

/// A whole message, from one SMS or several.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Sms {
    pub sender: String,
    /// When the service centre took it, in seconds since 1970.
    pub sent: Option<u64>,
    pub text: String,
}

/// One SMS as the driver handed it over.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Part {
    pub sender: String,
    pub sent: Option<u64>,
    pub text: String,
    pub concat_ref: u16,
    /// Counted from 1; `parts` is 0 for a message that is whole.
    pub part: u8,
    pub parts: u8,
}

fn c_str(bytes: &[u8]) -> String {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}

impl Part {
    fn from_hal(sms: &CHalSms) -> Part {
        Part {
            sender: c_str(&sms.sender),
            sent: msg_log::scts_to_unix(&c_str(&sms.sent_at)),
            text: c_str(&sms.text),
            concat_ref: sms.concat_ref,
            part: sms.part,
            parts: sms.parts,
        }
    }
}

// ---------------------------------------------------------------------------
// Putting long messages back together
// ---------------------------------------------------------------------------

struct Assembly {
    sender: String,
    concat_ref: u16,
    texts: Vec<Option<String>>,
    sent: Option<u64>,
    started: Instant,
}

impl Assembly {
    fn whole(&self) -> bool {
        self.texts.iter().all(Option::is_some)
    }

    fn into_sms(self) -> Sms {
        let text = self
            .texts
            .into_iter()
            .map(|t| t.unwrap_or_else(|| "[...]".to_string()))
            .collect::<String>();
        Sms { sender: self.sender, sent: self.sent, text }
    }
}

#[derive(Default)]
pub(crate) struct Assembler {
    pending: Vec<Assembly>,
}

impl Assembler {
    /// Take a part: the message it completes, or the part itself if it is
    /// whole, or, for a new long message when ASSEMBLING_MAX are already
    /// waiting, the oldest of those as it stands.
    pub(crate) fn add(&mut self, part: Part, now: Instant) -> Vec<Sms> {
        if part.parts < 2 || part.part == 0 || part.part > part.parts {
            return vec![Sms { sender: part.sender, sent: part.sent, text: part.text }];
        }
        let mut done = Vec::new();
        let at = match self.pending.iter().position(|a| {
            a.sender == part.sender
                && a.concat_ref == part.concat_ref
                && a.texts.len() == part.parts as usize
        }) {
            Some(at) => at,
            None => {
                if self.pending.len() >= ASSEMBLING_MAX {
                    done.push(self.pending.remove(0).into_sms());
                }
                self.pending.push(Assembly {
                    sender: part.sender.clone(),
                    concat_ref: part.concat_ref,
                    texts: vec![None; part.parts as usize],
                    sent: None,
                    started: now,
                });
                self.pending.len() - 1
            }
        };
        let a = &mut self.pending[at];
        // The first part's time stamp stands for the message.
        if part.part == 1 || a.sent.is_none() {
            a.sent = part.sent.or(a.sent);
        }
        a.texts[part.part as usize - 1] = Some(part.text);
        if a.whole() {
            done.push(self.pending.remove(at).into_sms());
        }
        done
    }

    /// Messages whose missing parts have been waited for long enough.
    pub(crate) fn expired(&mut self, now: Instant) -> Vec<Sms> {
        let mut done = Vec::new();
        let mut i = 0;
        while i < self.pending.len() {
            if now.duration_since(self.pending[i].started) >= PART_TIMEOUT {
                done.push(self.pending.remove(i).into_sms());
            } else {
                i += 1;
            }
        }
        done
    }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct SmsState {
    inbox: VecDeque<Sms>,
    /// Messages an app has taken from the inbox and not yet shown the user,
    /// as it last reported.
    held_unread: u32,
    assembler: Assembler,
}

static STATE: Mutex<SmsState> = Mutex::new(SmsState {
    inbox: VecDeque::new(),
    held_unread: 0,
    assembler: Assembler { pending: Vec::new() },
});

fn deliver(messages: Vec<Sms>) {
    for sms in messages {
        msg_log::record(&Entry {
            transport: Transport::Sms,
            conv: &sms.sender,
            dir: Direction::In,
            peer: &sms.sender,
            text: &sms.text,
            sent: sms.sent,
        });
        crate::msg_alert::sms_arrived(&sms.sender, &sms.text);
        if let Ok(mut state) = STATE.lock() {
            if state.inbox.len() == INBOX_SIZE {
                state.inbox.pop_front();
                sms_log!(ESP_LOG_WARN, b"inbox full; the oldest message is dropped\0");
            }
            state.inbox.push_back(sms);
        }
    }
}

/// hal_sms_rx_cb_t, called by the driver with the modem held. Every message
/// is kept: from here on it is the kernel's to lose, not the modem's.
unsafe extern "C" fn on_sms(sms: *const CHalSms, _user_data: *mut c_void) -> bool {
    if sms.is_null() {
        return false;
    }
    let part = Part::from_hal(&*sms);
    let done = match STATE.lock() {
        Ok(mut state) => state.assembler.add(part, Instant::now()),
        Err(_) => return false,
    };
    deliver(done);
    true
}

// ---------------------------------------------------------------------------
// For the network coordinator
// ---------------------------------------------------------------------------

/// Whether the modem may hold SMS not yet fetched.
pub(crate) fn due(drv: &HalNetDriver) -> bool {
    drv.sms_due.map_or(false, |due| unsafe { due() })
}

/// Fetch what the modem holds. On the coordinator's thread.
pub(crate) fn fetch(drv: &HalNetDriver) {
    if let Some(fetch) = drv.sms_fetch {
        let mut kept = 0;
        let ret = unsafe { fetch(Some(on_sms), std::ptr::null_mut(), &mut kept) };
        if ret != ESP_OK {
            sms_log!(ESP_LOG_WARN, b"fetching SMS failed (0x%x)\0", ret);
        }
    }
    let late = match STATE.lock() {
        Ok(mut state) => state.assembler.expired(Instant::now()),
        Err(_) => Vec::new(),
    };
    if !late.is_empty() {
        sms_log!(ESP_LOG_INFO, b"a long message is delivered with parts missing\0");
    }
    deliver(late);
}

// ---------------------------------------------------------------------------
// FFI — thistle/sms.h
// ---------------------------------------------------------------------------

/// sms_message_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CSmsMessage {
    /// "+4917612345678", or a name such as "DHL"; NUL-terminated.
    pub sender: [u8; SMS_SENDER_MAX],
    /// UTF-8, NUL-terminated, cut at a character to fit.
    pub text: [u8; SMS_TEXT_MAX],
    /// When the service centre took it, in seconds since 1970; 0 if unknown.
    pub sent: u32,
}

/// `s` into `out`, cut at a character boundary, NUL-terminated.
pub(crate) fn copy_c(out: &mut [u8], s: &str) {
    let mut n = s.len().min(out.len() - 1);
    while !s.is_char_boundary(n) {
        n -= 1;
    }
    out[..n].copy_from_slice(&s.as_bytes()[..n]);
    out[n] = 0;
}

/// Whether an SMS can be sent: the board has a modem that sends them, and
/// it is on with a SIM ready.
#[no_mangle]
pub extern "C" fn rs_sms_ready() -> bool {
    match crate::net_coordinator::cellular() {
        Some(drv) if drv.sms_send.is_some() => crate::net_coordinator::modem_ready(drv),
        _ => false,
    }
}

/// Send `text` to `number`, and record it in the history. Blocks for the
/// exchange with the modem, seconds with a data session to interrupt.
///
/// # Safety
/// Both must be NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn rs_sms_send(number: *const c_char, text: *const c_char) -> i32 {
    if number.is_null() || text.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let Some(send) = crate::net_coordinator::cellular().and_then(|d| d.sms_send) else {
        return ESP_ERR_NOT_SUPPORTED;
    };
    if !rs_sms_ready() {
        return ESP_ERR_INVALID_STATE;
    }
    let ret = send(number, text);
    if ret == ESP_OK {
        let number = std::ffi::CStr::from_ptr(number).to_string_lossy();
        let text = std::ffi::CStr::from_ptr(text).to_string_lossy();
        msg_log::record(&Entry {
            transport: Transport::Sms,
            conv: &number,
            dir: Direction::Out,
            peer: &number,
            text: &text,
            sent: None,
        });
    }
    ret
}

/// Take the oldest received SMS. False when there is none.
///
/// # Safety
/// `out` must point to a writable sms_message_t.
#[no_mangle]
pub unsafe extern "C" fn rs_sms_inbox_pop(out: *mut CSmsMessage) -> bool {
    if out.is_null() {
        return false;
    }
    let sms = match STATE.lock() {
        Ok(mut state) => state.inbox.pop_front(),
        Err(_) => None,
    };
    let Some(sms) = sms else {
        return false;
    };
    let out = &mut *out;
    copy_c(&mut out.sender, &sms.sender);
    copy_c(&mut out.text, &sms.text);
    out.sent = sms.sent.and_then(|s| u32::try_from(s).ok()).unwrap_or(0);
    true
}

/// How many received SMS wait for an app.
#[no_mangle]
pub extern "C" fn rs_sms_inbox_count() -> u32 {
    STATE.lock().map(|s| s.inbox.len() as u32).unwrap_or(0)
}

/// SMS received that the user has not read: those still in the inbox, which
/// nothing has shown, and those an app took and has not shown yet. The status
/// bar shows it, with the mesh's.
#[no_mangle]
pub extern "C" fn rs_sms_unread_count() -> u32 {
    STATE
        .lock()
        .map(|s| s.inbox.len() as u32 + s.held_unread)
        .unwrap_or(0)
}

/// How many of the SMS it has taken from the inbox an app has still to show
/// the user. It reports again whenever that changes.
#[no_mangle]
pub extern "C" fn rs_sms_set_held_unread(count: u32) {
    if let Ok(mut s) = STATE.lock() {
        s.held_unread = count;
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn part(sender: &str, concat_ref: u16, part: u8, parts: u8, text: &str) -> Part {
        Part {
            sender: sender.into(),
            sent: Some(1_000 + part as u64),
            text: text.into(),
            concat_ref,
            part,
            parts,
        }
    }

    #[test]
    fn a_whole_message_is_delivered_at_once() {
        let mut a = Assembler::default();
        let done = a.add(part("+491701", 0, 0, 0, "short"), Instant::now());
        assert_eq!(done, vec![Sms { sender: "+491701".into(), sent: Some(1_000), text: "short".into() }]);
    }

    #[test]
    fn parts_in_any_order_make_one_message() {
        let mut a = Assembler::default();
        let now = Instant::now();
        assert!(a.add(part("+491701", 42, 3, 3, "three"), now).is_empty());
        assert!(a.add(part("+491701", 42, 1, 3, "one "), now).is_empty());
        // Another sender's message with the same reference is another message.
        assert!(a.add(part("+491702", 42, 1, 2, "x"), now).is_empty());
        let done = a.add(part("+491701", 42, 2, 3, "two "), now);
        assert_eq!(done.len(), 1);
        assert_eq!(done[0].text, "one two three");
        // The first part's time stamp stands for the whole.
        assert_eq!(done[0].sent, Some(1_001));
    }

    #[test]
    fn parts_never_completed_are_delivered_after_an_hour() {
        let mut a = Assembler::default();
        let t0 = Instant::now();
        a.add(part("+491701", 7, 1, 3, "start "), t0);
        a.add(part("+491701", 7, 3, 3, " end"), t0);
        assert!(a.expired(t0 + Duration::from_secs(3599)).is_empty());
        let done = a.expired(t0 + PART_TIMEOUT);
        assert_eq!(done.len(), 1);
        assert_eq!(done[0].text, "start [...] end");
        assert!(a.expired(t0 + PART_TIMEOUT * 2).is_empty());
    }

    #[test]
    fn too_many_unfinished_messages_push_out_the_oldest() {
        let mut a = Assembler::default();
        let now = Instant::now();
        for r in 0..ASSEMBLING_MAX as u16 {
            assert!(a.add(part("+491701", r, 1, 2, "a"), now).is_empty());
        }
        let done = a.add(part("+491701", 99, 1, 2, "b"), now);
        assert_eq!(done.len(), 1);
        assert_eq!(done[0].text, "a[...]");
    }

    #[test]
    fn a_part_numbered_out_of_range_is_taken_as_whole() {
        let mut a = Assembler::default();
        let done = a.add(part("+491701", 1, 4, 3, "odd"), Instant::now());
        assert_eq!(done[0].text, "odd");
    }

    fn sms(sender: &str, text: &str) -> Sms {
        Sms { sender: sender.into(), sent: None, text: text.into() }
    }

    fn empty_inbox() {
        let mut state = STATE.lock().unwrap();
        state.inbox.clear();
        state.held_unread = 0;
    }

    #[test]
    fn messages_nothing_has_shown_are_unread() {
        empty_inbox();
        assert_eq!(rs_sms_unread_count(), 0);
        deliver(vec![sms("+491701", "hello"), sms("+491702", "hi")]);
        assert_eq!(rs_sms_unread_count(), 2);
    }

    #[test]
    fn messages_an_app_took_stay_unread_until_it_shows_them() {
        empty_inbox();
        deliver(vec![sms("+491701", "hello"), sms("+491702", "hi")]);
        let mut out: CSmsMessage = unsafe { std::mem::zeroed() };
        assert!(unsafe { rs_sms_inbox_pop(&mut out) });
        rs_sms_set_held_unread(1);
        assert_eq!(rs_sms_unread_count(), 2);
        rs_sms_set_held_unread(0);
        assert_eq!(rs_sms_unread_count(), 1);
        empty_inbox();
    }

    #[test]
    fn text_is_cut_at_a_character() {
        let mut buf = [0xffu8; 6];
        copy_c(&mut buf, "aä€");
        // "a" (1) + "ä" (2) fit in 5; "€" (3) does not.
        assert_eq!(&buf[..4], b"a\xc3\xa4\0");
    }

    #[test]
    fn the_message_struct_matches_the_c_layout() {
        assert_eq!(std::mem::offset_of!(CSmsMessage, text), SMS_SENDER_MAX);
        assert_eq!(std::mem::offset_of!(CSmsMessage, sent), 1060);
        assert_eq!(std::mem::size_of::<CSmsMessage>(), 1064);
    }

    #[test]
    fn the_hal_struct_matches_the_c_layout() {
        assert_eq!(std::mem::offset_of!(CHalSms, sent_at), 33);
        assert_eq!(std::mem::offset_of!(CHalSms, text), 57);
        assert_eq!(std::mem::offset_of!(CHalSms, concat_ref), 538);
        assert_eq!(std::mem::offset_of!(CHalSms, part), 540);
        assert_eq!(std::mem::offset_of!(CHalSms, parts), 541);
        assert_eq!(std::mem::size_of::<CHalSms>(), 542);
    }
}
