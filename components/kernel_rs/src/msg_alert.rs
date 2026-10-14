// SPDX-License-Identifier: BSD-3-Clause
//! Alerts for messages as they arrive: the motor buzzes, and the message
//! waits here for the status bar to show as a pop-up.
//!
//! Every SMS and MeshCore message the kernel receives comes through
//! `arrived()`, Messenger open or not. There is no alert when alerts are
//! switched off (`thistle_os.messenger.alerts`, on unless switched off, in
//! the Messenger's settings), nor for a message in the conversation the
//! Messenger has on screen, which the user is already reading.
//!
//! The buzz is played here, on whatever thread received the message, since
//! the motor's driver only queues it. Nothing here waits for the screen: a
//! LoRa packet wakes the CPU without waking the UI, and the device should
//! buzz all the same. The pop-up is for the UI to take: messages arriving
//! before it does make one pop-up, for the latest, with a count.

use std::ffi::{c_char, CStr};
use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use crate::sms_manager::copy_c;

const SECTION: &str = "messenger";
const KEY: &str = "alerts";

/// Buzzes closer together than this are one: a burst of messages, or a
/// modem handing over what it kept while off, buzzes once.
const BUZZ_GAP: Duration = Duration::from_secs(5);

/// The longest message text a pop-up shows, in bytes, before "...".
const PREVIEW_MAX: usize = 72;

/// msg_alert.h: MSG_ALERT_SMS, MSG_ALERT_MESH.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(u8)]
pub(crate) enum Transport {
    Sms = 0,
    Mesh = 1,
}

impl Transport {
    fn from_c(value: i32) -> Option<Self> {
        match value {
            0 => Some(Transport::Sms),
            1 => Some(Transport::Mesh),
            _ => None,
        }
    }
}

#[derive(Debug, PartialEq)]
struct Pending {
    transport: Transport,
    from: String,
    text: String,
    count: u32,
}

struct State {
    /// The conversation the Messenger has on screen, by transport and its
    /// address there.
    viewing: Option<(Transport, String)>,
    pending: Option<Pending>,
    last_buzz: Option<Instant>,
}

static STATE: Mutex<State> = Mutex::new(State {
    viewing: None,
    pending: None,
    last_buzz: None,
});

fn lock() -> std::sync::MutexGuard<'static, State> {
    STATE
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

// ---------------------------------------------------------------------------
// The setting
// ---------------------------------------------------------------------------

const UNKNOWN: u8 = 0;
const ON: u8 = 1;
const OFF: u8 = 2;

static ENABLED: AtomicU8 = AtomicU8::new(UNKNOWN);

/// Alerts are on unless switched off.
pub(crate) fn enabled_in(json: Option<&str>) -> bool {
    json.and_then(|j| crate::system_json::get(j, SECTION, KEY))
        .and_then(|v| v.as_bool())
        .unwrap_or(true)
}

fn enabled() -> bool {
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
// Arrivals
// ---------------------------------------------------------------------------

/// `text` as a pop-up shows it: on one line, and cut at a character to
/// PREVIEW_MAX bytes, with "..." where it was cut.
fn preview(text: &str) -> String {
    let line: String = text
        .chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .collect();
    let line = line.trim();
    if line.len() <= PREVIEW_MAX {
        return line.to_string();
    }
    let mut n = PREVIEW_MAX - 3;
    while !line.is_char_boundary(n) {
        n -= 1;
    }
    format!("{}...", line[..n].trim_end())
}

/// A message arrived in conversation `conv` on `transport`, from `from` (a
/// name to show). Buzzes and queues a pop-up unless alerts are off or the
/// conversation is on screen; says whether it did.
pub(crate) fn arrived(transport: Transport, conv: &str, from: &str, text: &str) -> bool {
    if !enabled() {
        return false;
    }
    arrived_at(transport, conv, from, text, Instant::now())
}

fn arrived_at(transport: Transport, conv: &str, from: &str, text: &str, now: Instant) -> bool {
    let buzz = {
        let mut state = lock();
        if state
            .viewing
            .as_ref()
            .is_some_and(|(t, c)| *t == transport && c == conv)
        {
            return false;
        }
        let count = state.pending.as_ref().map_or(0, |p| p.count) + 1;
        state.pending = Some(Pending {
            transport,
            from: from.to_string(),
            text: preview(text),
            count,
        });
        let buzz = state
            .last_buzz
            .is_none_or(|at| now.duration_since(at) >= BUZZ_GAP);
        if buzz {
            state.last_buzz = Some(now);
        }
        buzz
    };
    if buzz {
        crate::hal_registry::haptic_play(crate::hal_registry::HAL_HAPTIC_MESSAGE);
    }
    true
}

/// An SMS arrived from `sender`, a phone number or a name such as "DHL".
/// Shown by the contact's name when the number is one, and not shown at all
/// when the contact's messages are encrypted: the Messenger decrypts those.
pub(crate) fn sms_arrived(sender: &str, text: &str) {
    let contact = crate::contact_manager::contact_by_phone(sender);
    let encrypted = contact
        .as_ref()
        .is_some_and(|(id, _)| unsafe { crate::msg_crypto::rs_msg_crypto_is_active(*id) });
    let from = match &contact {
        Some((_, name)) if !name.is_empty() => name.as_str(),
        _ => sender,
    };
    let text = if encrypted { "Encrypted message" } else { text };
    arrived(Transport::Sms, sender, from, text);
}

// ---------------------------------------------------------------------------
// FFI — thistle/msg_alert.h
// ---------------------------------------------------------------------------

pub const FROM_MAX: usize = 33;
pub const TEXT_MAX: usize = 73;

/// msg_alert_t
#[repr(C)]
pub struct CMsgAlert {
    /// Messages since the last pop-up: this one and those it stands for.
    pub count: u32,
    /// MSG_ALERT_SMS or MSG_ALERT_MESH.
    pub transport: u8,
    pub from: [u8; FROM_MAX],
    pub text: [u8; TEXT_MAX],
}

/// Take the pop-up waiting for the latest message; false when none is.
///
/// # Safety
/// `out` must point to a writable msg_alert_t.
#[no_mangle]
pub unsafe extern "C" fn rs_msg_alert_take(out: *mut CMsgAlert) -> bool {
    if out.is_null() {
        return false;
    }
    let Some(p) = lock().pending.take() else {
        return false;
    };
    let out = unsafe { &mut *out };
    out.count = p.count;
    out.transport = p.transport as u8;
    copy_c(&mut out.from, &p.from);
    copy_c(&mut out.text, &p.text);
    true
}

/// The conversation the Messenger shows: `conv` on `transport`
/// (MSG_ALERT_*). A transport that is neither, or a null `conv`, is none.
///
/// # Safety
/// `conv` must be null or a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn rs_msg_alert_set_viewing(transport: i32, conv: *const c_char) {
    let viewing = match (Transport::from_c(transport), conv.is_null()) {
        (Some(t), false) => {
            let conv = unsafe { CStr::from_ptr(conv) }
                .to_string_lossy()
                .into_owned();
            Some((t, conv))
        }
        _ => None,
    };
    lock().viewing = viewing;
}

/// Whether messages buzz and pop up as they arrive.
#[no_mangle]
pub extern "C" fn rs_msg_alert_enabled() -> bool {
    enabled()
}

/// Switch alerts on or off, and remember it. The choice applies even if it
/// could not be saved, which the return says.
#[no_mangle]
pub extern "C" fn rs_msg_alert_set_enabled(on: bool) -> i32 {
    ENABLED.store(if on { ON } else { OFF }, Ordering::Relaxed);
    if !on {
        lock().pending = None;
    }
    crate::system_json::save(SECTION, KEY, serde_json::Value::Bool(on))
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn fresh() {
        let mut s = lock();
        s.viewing = None;
        s.pending = None;
        s.last_buzz = None;
    }

    fn take() -> Option<(u32, u8, String, String)> {
        let mut out: CMsgAlert = unsafe { std::mem::zeroed() };
        unsafe { rs_msg_alert_take(&mut out) }.then(|| {
            let s = |b: &[u8]| {
                String::from_utf8_lossy(&b[..b.iter().position(|&c| c == 0).unwrap()]).into_owned()
            };
            (out.count, out.transport, s(&out.from), s(&out.text))
        })
    }

    #[test]
    fn a_message_waits_for_the_status_bar_once() {
        fresh();
        let t0 = Instant::now();
        assert!(arrived_at(Transport::Sms, "+491701", "Anna", "Hallo", t0));
        assert_eq!(take(), Some((1, 0, "Anna".into(), "Hallo".into())));
        assert_eq!(take(), None);
    }

    #[test]
    fn messages_before_the_pop_up_make_one_for_the_latest() {
        fresh();
        let t0 = Instant::now();
        arrived_at(Transport::Sms, "+491701", "Anna", "one", t0);
        arrived_at(Transport::Mesh, "", "Bob", "two", t0);
        assert_eq!(take(), Some((2, 1, "Bob".into(), "two".into())));
    }

    #[test]
    fn a_message_in_the_conversation_on_screen_is_not_alerted() {
        fresh();
        let conv = std::ffi::CString::new("+491701").unwrap();
        unsafe { rs_msg_alert_set_viewing(0, conv.as_ptr()) };
        let t0 = Instant::now();
        assert!(!arrived_at(Transport::Sms, "+491701", "Anna", "hi", t0));
        // Another conversation, or the same address on another transport, is.
        assert!(arrived_at(Transport::Sms, "+491702", "Ben", "hi", t0));
        assert!(arrived_at(Transport::Mesh, "+491701", "?", "hi", t0));
        unsafe { rs_msg_alert_set_viewing(-1, std::ptr::null()) };
        assert!(arrived_at(Transport::Sms, "+491701", "Anna", "hi", t0));
        fresh();
    }

    #[test]
    fn a_burst_buzzes_once() {
        fresh();
        let t0 = Instant::now();
        arrived_at(Transport::Mesh, "", "Bob", "a", t0);
        assert_eq!(lock().last_buzz, Some(t0));
        arrived_at(Transport::Mesh, "", "Bob", "b", t0 + Duration::from_secs(2));
        assert_eq!(lock().last_buzz, Some(t0));
        let later = t0 + BUZZ_GAP;
        arrived_at(Transport::Mesh, "", "Bob", "c", later);
        assert_eq!(lock().last_buzz, Some(later));
        fresh();
    }

    #[test]
    fn a_long_message_is_cut_at_a_character_onto_one_line() {
        assert_eq!(preview("  Line one\nline two  "), "Line one line two");
        let long = "ü".repeat(60); // 120 bytes
        let p = preview(&long);
        assert!(p.len() <= PREVIEW_MAX, "{}", p.len());
        assert!(p.ends_with("..."));
        assert!(p.trim_end_matches("...").chars().all(|c| c == 'ü'));
    }

    #[test]
    fn alerts_are_on_unless_switched_off() {
        assert!(enabled_in(None));
        assert!(enabled_in(Some(r#"{ "thistle_os": {} }"#)));
        assert!(!enabled_in(Some(
            r#"{ "thistle_os": { "messenger": { "alerts": false } } }"#
        )));
    }

    #[test]
    fn the_alert_struct_matches_the_c_layout() {
        assert_eq!(std::mem::offset_of!(CMsgAlert, transport), 4);
        assert_eq!(std::mem::offset_of!(CMsgAlert, from), 5);
        assert_eq!(std::mem::offset_of!(CMsgAlert, text), 38);
        assert_eq!(std::mem::size_of::<CMsgAlert>(), 112);
    }
}
