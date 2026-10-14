// SPDX-License-Identifier: BSD-3-Clause
//! Voice calls, through a modem that makes them (thistle/call.h).
//!
//! The kernel follows a call by asking the modem for its calls (AT+CLCC,
//! `call_list`), not by parsing what the modem says unasked: esp_modem's URC
//! hook is compiled out, and a data session would hide the URCs anyway. A
//! call ringing here holds the modem's RI line low until it is answered or
//! given up, so a kernel worker job reads RI once a second (`call_ringing`,
//! a pin read that never waits) and starts the call thread when it is low.
//! Dialling starts the thread too. The thread lives only while there is a
//! call: it polls every POLL, runs what the UI asks of the modem in between,
//! and exits once the call is over and the modem has listed no call, which
//! is when the driver resumes a data session the call interrupted. Until
//! then the driver holds the line, so the thread keeps asking, however long
//! the modem takes to answer properly (up to GIVE_UP).
//!
//! What the user sees is a state machine (`Machine`), kept apart from the
//! thread so it can be tested on the host. It follows the first call the
//! modem lists and ignores the rest: call waiting, holding and conferences
//! are not done. How a call ended is read from what the user did and how
//! far the call got: hung up here, the other side hung up, declined, missed,
//! or failed (never connected, nobody hung up).
//!
//! While a call rings here, the thread plays a ringtone through the audio
//! driver at the system volume, unless the system is muted, and buzzes the
//! motor every two seconds. Once it connects, the speaker is handed to the
//! modem (`set_route`) and stays the modem's until the call ends. The modem
//! has a microphone of its own. The call's volume (`thistle_os.phone
//! .call_volume`, 0-100) and the speaker are sent to the modem only while
//! a call is connected: outside one each costs the data session seconds, and
//! the modem writes its gain to flash, so the volume is sent only when it
//! differs from what was last sent.
//!
//! While a call is up the network coordinator leaves the modem alone (it
//! asks `busy()`), the device is held awake, and anything that plays sound
//! pauses (`rs_call_in_progress`).

use std::ffi::{c_char, c_void, CString};
use std::sync::atomic::{AtomicBool, AtomicU16, AtomicU32, Ordering};
use std::sync::{Condvar, Mutex, MutexGuard, PoisonError};
use std::time::{Duration, Instant};

use crate::call_log::{self, Kind};
use crate::hal_registry::{
    registry, HalAudioConfig, HalAudioDriver, HAL_AUDIO_ROUTE_CODEC, HAL_AUDIO_ROUTE_VOICE, HAL_HAPTIC_MESSAGE,
    HAL_HAPTIC_RING,
};
use crate::net_manager::{CHalCall, HalNetDriver};
use crate::power_manager::HoldGuard;
use crate::sms_manager::copy_c;

const ESP_OK: i32 = 0;
const ESP_ERR_NO_MEM: i32 = 0x101;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_ERR_NOT_SUPPORTED: i32 = 0x106;

/// call.h
pub const CALL_NUMBER_MAX: usize = 33;
pub const CALL_NAME_MAX: usize = 64;
const ENDED_SHOW: Duration = Duration::from_millis(2500);
/// network.h: HAL_CALL_NUMBER_MAX, the longest dial string.
const DIAL_MAX: usize = 32;

/// hal_call_state_t
const STAT_ACTIVE: u32 = 0;
const STAT_HELD: u32 = 1;
const STAT_DIALING: u32 = 2;
const STAT_ALERTING: u32 = 3;
const STAT_INCOMING: u32 = 4;
const STAT_WAITING: u32 = 5;
const STAT_RELEASED: u32 = 6;

/// How often the modem is asked while a call is up.
#[cfg(not(test))]
const POLL: Duration = Duration::from_millis(500);
#[cfg(test)]
const POLL: Duration = Duration::from_millis(10);
/// How often RI is read between calls.
const WATCH_PERIOD: Duration = Duration::from_millis(1000);
/// How long a call just dialled may go unlisted before it counts as failed:
/// the modem takes a moment to list it.
const DIAL_LIST_WAIT: Duration = Duration::from_secs(6);
/// How long the modem may not answer before a call it has not listed yet
/// counts as gone: long enough for it to wake from sleep after RI.
const WAKE_GRACE: Duration = Duration::from_secs(10);
/// How long the modem may not answer about a call it did list before that
/// call counts as gone.
const LOST_GRACE: Duration = Duration::from_secs(30);
/// How long the thread goes on asking a modem that does not answer, for it
/// to list no call, before it gives up and exits.
const GIVE_UP: Duration = Duration::from_secs(60);
/// Calls asked for at a time: one is followed, the rest are listed only so
/// they can be told apart from it.
const LIST_MAX: usize = 4;

/// The call thread's stack, in internal RAM like every stack (a PSRAM one
/// cannot be read while flash is written, and this thread writes the call
/// log). Each driver call puts an answer buffer of about 512 bytes on it;
/// the ringtone's samples are on the heap. Writing the call log goes down
/// through FatFs and the SD card's SPI driver, which overflowed the
/// phonebook's 8 KB reader on the T-Deck Max, so this has room to spare: it
/// exists only while a call does.
const STACK_SIZE: usize = 12 * 1024;

const SECTION: &str = "phone";
const VOLUME_KEY: &str = "call_volume";
pub(crate) const DEFAULT_VOLUME: u8 = 70;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

macro_rules! log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {{
        #[cfg(not(test))]
        unsafe {
            esp_log_write($level, b"call\0".as_ptr(), $fmt.as_ptr() $(, $arg)*);
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
    fn uxTaskGetStackHighWaterMark(task: *mut c_void) -> u32;
}

fn c_string(s: &str) -> CString {
    CString::new(s).unwrap_or_default()
}

// ---------------------------------------------------------------------------
// The state machine
// ---------------------------------------------------------------------------

/// call_state_t
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum State {
    Idle = 0,
    Dialing = 1,
    Alerting = 2,
    Incoming = 3,
    Active = 4,
    Ended = 5,
}

/// call_end_t
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum End {
    None = 0,
    Local = 1,
    Remote = 2,
    Rejected = 3,
    Missed = 4,
    Failed = 5,
}

/// A call as the modem listed it.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Listed {
    pub id: u8,
    pub incoming: bool,
    /// hal_call_state_t
    pub stat: u32,
    pub number: String,
}

impl Listed {
    fn from_hal(c: &CHalCall) -> Listed {
        let end = c.number.iter().position(|&b| b == 0).unwrap_or(c.number.len());
        Listed {
            id: c.id,
            incoming: c.incoming,
            stat: c.state,
            number: String::from_utf8_lossy(&c.number[..end]).into_owned(),
        }
    }
}

/// How a call ended, for the call log.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Ending {
    pub end: End,
    pub incoming: bool,
    pub number: String,
    /// When it was placed or began ringing, by the clock, if set.
    pub began: Option<u64>,
    pub duration_s: u32,
}

impl Ending {
    fn kind(&self) -> Kind {
        match self.end {
            End::Missed => Kind::Missed,
            End::Rejected => Kind::Rejected,
            _ if self.incoming => Kind::Incoming,
            _ => Kind::Outgoing,
        }
    }
}

/// What the thread has to do about a change.
#[derive(Clone, Debug, PartialEq)]
pub(crate) enum Event {
    /// A call began ringing here, from this number ("" when withheld).
    Ringing(String),
    /// It stopped ringing here: answered, declined or gone.
    RingStopped,
    /// The call connected: the speaker goes over to the modem.
    Connected,
    Ended(Ending),
}

/// The modem's <stat> as what the user is shown.
fn state_for(stat: u32) -> State {
    match stat {
        STAT_DIALING => State::Dialing,
        STAT_ALERTING => State::Alerting,
        STAT_INCOMING | STAT_WAITING => State::Incoming,
        // A call on hold is still connected.
        STAT_ACTIVE | STAT_HELD => State::Active,
        _ => State::Active,
    }
}

fn secs(d: Duration) -> u32 {
    u32::try_from(d.as_secs()).unwrap_or(u32::MAX)
}

/// The call the user sees, and how the modem's lists move it on.
pub(crate) struct Machine {
    state: State,
    end: End,
    incoming: bool,
    number: String,
    name: String,
    muted: bool,
    speaker: bool,
    volume: u8,
    seq: u32,
    /// When the call was placed or began ringing.
    since: Option<Instant>,
    connected_at: Option<Instant>,
    /// Final once the call ended.
    duration_s: u32,
    ended_at: Option<Instant>,
    began: Option<u64>,
    /// The modem's index of the call followed, once it listed it.
    id: Option<u8>,
    /// The modem has listed the call at least once.
    listed: bool,
    /// The user ended it.
    hung_up: bool,
    /// The user answered; it is ACTIVE when the modem says so.
    answering: bool,
    silenced: bool,
    /// The modem's index of a call just ended, until when a listing of it is
    /// taken for the modem lagging rather than a new call.
    ignore: Option<(u8, Instant)>,
}

impl Machine {
    pub(crate) const fn new() -> Machine {
        Machine {
            state: State::Idle,
            end: End::None,
            incoming: false,
            number: String::new(),
            name: String::new(),
            muted: false,
            speaker: true,
            volume: DEFAULT_VOLUME,
            seq: 0,
            since: None,
            connected_at: None,
            duration_s: 0,
            ended_at: None,
            began: None,
            id: None,
            listed: false,
            hung_up: false,
            answering: false,
            silenced: false,
            ignore: None,
        }
    }

    fn bump(&mut self) {
        self.seq = self.seq.wrapping_add(1);
    }

    /// An ended call reads as idle once it has been shown long enough. Done
    /// whenever the machine is looked at, so it needs no thread.
    fn settle(&mut self, now: Instant) {
        if self.state == State::Ended && self.ended_at.is_some_and(|t| now.duration_since(t) >= ENDED_SHOW) {
            self.state = State::Idle;
            self.end = End::None;
            self.duration_s = 0;
            self.bump();
        }
    }

    /// Ringing here, being placed, or connected.
    pub(crate) fn in_progress(&self) -> bool {
        matches!(self.state, State::Dialing | State::Alerting | State::Incoming | State::Active)
    }

    /// Whether the ringtone and the buzzing should be going.
    fn audible(&self) -> bool {
        self.state == State::Incoming && !self.silenced && !self.answering
    }

    fn begin(&mut self, state: State, incoming: bool, number: String, name: String, now: Instant, wall: Option<u64>) {
        self.state = state;
        self.end = End::None;
        self.incoming = incoming;
        self.number = number;
        self.name = name;
        self.muted = false;
        self.speaker = true;
        self.since = Some(now);
        self.connected_at = None;
        self.duration_s = 0;
        self.ended_at = None;
        self.began = wall;
        self.id = None;
        self.listed = false;
        self.hung_up = false;
        self.answering = false;
        self.silenced = false;
        self.bump();
    }

    /// A call is placed: DIALING at once, before the modem has it.
    pub(crate) fn dial(&mut self, number: &str, name: String, now: Instant, wall: Option<u64>) -> Result<(), i32> {
        self.settle(now);
        if self.in_progress() {
            return Err(ESP_ERR_INVALID_STATE);
        }
        self.begin(State::Dialing, false, number.to_string(), name, now, wall);
        Ok(())
    }

    /// The call just dialled had no thread to place it.
    fn abandon(&mut self) {
        if self.state == State::Dialing && !self.listed {
            self.state = State::Idle;
            self.bump();
        }
    }

    /// The modem refused the dial.
    pub(crate) fn dial_failed(&mut self, now: Instant) -> Option<Event> {
        if self.state != State::Dialing || self.listed {
            return None;
        }
        let end = if self.hung_up { End::Local } else { End::Failed };
        Some(self.finish(end, now))
    }

    fn finish(&mut self, end: End, now: Instant) -> Event {
        self.duration_s = self.connected_at.map_or(0, |t| secs(now.duration_since(t)));
        self.state = State::Ended;
        self.end = end;
        self.ended_at = Some(now);
        self.ignore = self.id.map(|id| (id, now + ENDED_SHOW));
        self.id = None;
        self.answering = false;
        self.bump();
        Event::Ended(Ending {
            end,
            incoming: self.incoming,
            number: self.number.clone(),
            began: self.began,
            duration_s: self.duration_s,
        })
    }

    pub(crate) fn answer(&mut self, now: Instant) -> Result<(), i32> {
        self.settle(now);
        if self.state != State::Incoming {
            return Err(ESP_ERR_INVALID_STATE);
        }
        self.answering = true;
        Ok(())
    }

    /// The call ends when the modem stops listing it.
    pub(crate) fn hangup(&mut self, now: Instant) -> Result<(), i32> {
        self.settle(now);
        if !self.in_progress() {
            return Err(ESP_ERR_INVALID_STATE);
        }
        self.hung_up = true;
        Ok(())
    }

    /// True if this silenced a call ringing here.
    pub(crate) fn silence(&mut self) -> bool {
        if self.state != State::Incoming || self.silenced {
            return false;
        }
        self.silenced = true;
        true
    }

    pub(crate) fn set_mute(&mut self, mute: bool, now: Instant) -> Result<(), i32> {
        self.settle(now);
        if !self.in_progress() {
            return Err(ESP_ERR_INVALID_STATE);
        }
        if self.muted != mute {
            self.muted = mute;
            self.bump();
        }
        Ok(())
    }

    pub(crate) fn set_speaker(&mut self, on: bool, now: Instant) -> Result<(), i32> {
        self.settle(now);
        if !self.in_progress() {
            return Err(ESP_ERR_INVALID_STATE);
        }
        if self.speaker != on {
            self.speaker = on;
            self.bump();
        }
        Ok(())
    }

    pub(crate) fn set_volume(&mut self, percent: u8) {
        if self.volume != percent {
            self.volume = percent;
            self.bump();
        }
    }

    /// The phonebook's name for the call's number, found after the call
    /// began.
    pub(crate) fn set_name(&mut self, number: &str, name: String) {
        if self.number == number && self.name != name && (self.in_progress() || self.state == State::Ended) {
            self.name = name;
            self.bump();
        }
    }

    /// The modem's calls, as it listed them at `now`.
    pub(crate) fn listed(&mut self, calls: &[Listed], now: Instant, wall: Option<u64>) -> Vec<Event> {
        self.settle(now);
        if self.ignore.is_some_and(|(_, until)| now >= until) {
            self.ignore = None;
        }
        let ignored = self.ignore.map(|(id, _)| id);
        let ours = match self.id {
            Some(id) => calls.iter().find(|c| c.id == id),
            // Placed and not yet listed: the modem's outgoing call.
            None if self.state == State::Dialing => calls.iter().find(|c| !c.incoming && c.stat != STAT_RELEASED),
            // None followed: one ringing here, or one met already up.
            None if !self.in_progress() => calls.iter().find(|c| {
                matches!(c.stat, STAT_ACTIVE | STAT_DIALING | STAT_ALERTING | STAT_INCOMING) && Some(c.id) != ignored
            }),
            None => None,
        }
        .filter(|c| c.stat != STAT_RELEASED);

        let mut events = Vec::new();
        let Some(c) = ours else {
            match self.state {
                // The modem may take a moment to list a call just placed.
                State::Dialing
                    if !self.listed && !self.hung_up && self.since.is_some_and(|t| now.duration_since(t) < DIAL_LIST_WAIT) => {}
                State::Dialing | State::Alerting => {
                    let end = if self.hung_up { End::Local } else { End::Failed };
                    events.push(self.finish(end, now));
                }
                State::Incoming => {
                    events.push(Event::RingStopped);
                    let end = if self.hung_up { End::Rejected } else { End::Missed };
                    events.push(self.finish(end, now));
                }
                State::Active => {
                    let end = if self.hung_up { End::Local } else { End::Remote };
                    events.push(self.finish(end, now));
                }
                State::Idle | State::Ended => {}
            }
            return events;
        };

        let next = state_for(c.stat);
        if !self.in_progress() {
            self.begin(next, c.incoming, c.number.clone(), String::new(), now, wall);
            self.id = Some(c.id);
            self.listed = true;
            match next {
                State::Incoming => events.push(Event::Ringing(c.number.clone())),
                State::Active => {
                    self.connected_at = Some(now);
                    events.push(Event::Connected);
                }
                _ => {}
            }
            return events;
        }
        self.id = Some(c.id);
        self.listed = true;
        if self.number.is_empty() && !c.number.is_empty() {
            self.number = c.number.clone();
            self.bump();
        }
        // A connected call does not ring again.
        if next == self.state || (self.state == State::Active && next == State::Incoming) {
            return events;
        }
        if self.state == State::Incoming {
            events.push(Event::RingStopped);
        }
        if next == State::Active && self.connected_at.is_none() {
            self.connected_at = Some(now);
            self.answering = false;
            events.push(Event::Connected);
        }
        self.state = next;
        self.bump();
        events
    }

    pub(crate) fn status(&mut self, now: Instant) -> CCallStatus {
        self.settle(now);
        let mut s = CCallStatus {
            state: self.state as u32,
            end: self.end as u32,
            incoming: self.incoming,
            number: [0; CALL_NUMBER_MAX],
            name: [0; CALL_NAME_MAX],
            duration_s: match self.state {
                State::Active => self.connected_at.map_or(0, |t| secs(now.duration_since(t))),
                State::Ended => self.duration_s,
                _ => 0,
            },
            muted: self.muted,
            speaker: self.speaker,
            volume: self.volume,
            seq: self.seq,
        };
        if self.state != State::Idle {
            copy_c(&mut s.number, &self.number);
            copy_c(&mut s.name, &self.name);
        }
        s
    }
}

/// A number as the modem dials it: '+' first if at all, then digits, '*'
/// and '#'; spaces, dots, dashes and brackets dropped. None for one with no
/// digit, with anything else in it, or too long.
pub(crate) fn normalize(number: &str) -> Option<String> {
    let mut out = String::new();
    for c in number.chars() {
        match c {
            '.' | '-' | '(' | ')' => {}
            c if c.is_whitespace() => {}
            '+' if out.is_empty() => out.push('+'),
            '0'..='9' | '*' | '#' => out.push(c),
            _ => return None,
        }
    }
    (out.bytes().any(|b| b.is_ascii_digit()) && out.len() <= DIAL_MAX).then_some(out)
}

/// The saved call volume, or the default.
pub(crate) fn volume_in(json: Option<&str>) -> u8 {
    json.and_then(|j| crate::system_json::get(j, SECTION, VOLUME_KEY))
        .and_then(|v| v.as_u64())
        .filter(|&v| v <= 100)
        .map_or(DEFAULT_VOLUME, |v| v as u8)
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

/// What the UI asks of the modem, done on the call thread.
#[derive(Clone, Debug, PartialEq)]
enum Command {
    Dial(String),
    Answer,
    Hangup,
    Dtmf(u8),
}

struct Shared {
    m: Machine,
    commands: Vec<Command>,
    /// A call thread runs.
    thread: bool,
    /// Something changed for the thread to act on now.
    poke: bool,
    volume_loaded: bool,
}

static CALL: Mutex<Shared> = Mutex::new(Shared {
    m: Machine::new(),
    commands: Vec::new(),
    thread: false,
    poke: false,
    volume_loaded: false,
});
static WAKE: Condvar = Condvar::new();

/// The volume last sent to the modem this boot, u16::MAX for none: the
/// modem writes it to its flash, so it is not sent again unchanged.
static VOLUME_SENT: AtomicU16 = AtomicU16::new(u16::MAX);
/// "No audio switch" is said once.
static NO_SWITCH_SAID: AtomicBool = AtomicBool::new(false);
/// Call threads not yet finished, power hold released and all.
static LIVE: AtomicU32 = AtomicU32::new(0);

fn lock() -> MutexGuard<'static, Shared> {
    CALL.lock().unwrap_or_else(PoisonError::into_inner)
}

fn wake_thread(mut s: MutexGuard<'static, Shared>) {
    s.poke = true;
    drop(s);
    WAKE.notify_all();
}

/// The cellular driver, if it makes calls.
fn calls_driver() -> Option<&'static HalNetDriver> {
    crate::net_coordinator::cellular().filter(|d| {
        d.call_dial.is_some() && d.call_answer.is_some() && d.call_hangup.is_some() && d.call_list.is_some()
    })
}

/// Whether a call has the modem: one is up, or its thread still runs. The
/// network coordinator leaves the modem alone meanwhile.
pub(crate) fn busy() -> bool {
    let s = lock();
    s.thread || s.m.in_progress()
}

/// The saved call volume, read once.
fn load_volume() {
    if lock().volume_loaded {
        return;
    }
    let volume = if cfg!(test) {
        DEFAULT_VOLUME
    } else {
        volume_in(std::fs::read_to_string(crate::system_json::path()).ok().as_deref())
    };
    let mut s = lock();
    if !s.volume_loaded {
        s.volume_loaded = true;
        s.m.volume = volume;
    }
}

// ---------------------------------------------------------------------------
// The ringtone
// ---------------------------------------------------------------------------

const RING_RATE: u32 = 16_000;
/// Two short rings and a pause, two seconds in all: (tone, milliseconds).
/// The motor buzzes at the start of each.
const CADENCE: [(bool, u64); 4] = [(true, 400), (false, 200), (true, 400), (false, 1000)];
/// A pause up to this long is played as silence, so the second ring follows
/// the first on time; a longer one is waited out.
const SHORT_GAP_MS: u64 = 200;
/// Peak of each of the two sines, of full scale: together a little under
/// half, with the codec's volume on top.
const RING_LEVEL: f32 = 0.22;

/// 440 Hz and 480 Hz together, the ring of North American phones, with 10 ms
/// to fade in and out so it does not click: 16 kHz mono 16-bit samples.
fn ring_tone(ms: u64) -> Vec<u8> {
    let n = (RING_RATE as u64 * ms / 1000) as usize;
    let fade = (RING_RATE / 100) as usize;
    let mut out = Vec::with_capacity(n * 2);
    let tau = std::f32::consts::TAU;
    for i in 0..n {
        let t = i as f32 / RING_RATE as f32;
        let env = if i < fade {
            i as f32 / fade as f32
        } else if i + fade >= n {
            (n - 1 - i) as f32 / fade as f32
        } else {
            1.0
        };
        let v = ((tau * 440.0 * t).sin() + (tau * 480.0 * t).sin()) * RING_LEVEL * env;
        let sample = (v * i16::MAX as f32) as i16;
        out.extend_from_slice(&sample.to_le_bytes());
    }
    out
}

struct Ring {
    step: usize,
    due: Instant,
    /// Made the first time they are played; over 4 KB each, so in PSRAM.
    tone: Vec<u8>,
    gap: Vec<u8>,
    configured: bool,
    /// Written to the codec since it was last stopped.
    sounding: bool,
    /// The codec refused: the rest of the ring is timed, not played.
    quiet: bool,
}

impl Ring {
    fn new(now: Instant) -> Ring {
        Ring {
            step: 0,
            due: now,
            tone: Vec::new(),
            gap: Vec::new(),
            configured: false,
            sounding: false,
            quiet: false,
        }
    }
}

fn audio() -> Option<&'static HalAudioDriver> {
    unsafe { registry().audio.as_ref() }
}

/// Hand the speaker to `route`. False if the board has no audio switch,
/// which is said once.
fn route(route: u32, speaker: bool) -> bool {
    let ret = match audio().and_then(|a| a.set_route) {
        Some(set_route) => unsafe { set_route(route, speaker) },
        None => ESP_ERR_NOT_SUPPORTED,
    };
    if ret == ESP_ERR_NOT_SUPPORTED {
        if route == HAL_AUDIO_ROUTE_VOICE && !NO_SWITCH_SAID.swap(true, Ordering::Relaxed) {
            log!(ESP_LOG_WARN, b"no audio switch; the call is not routed to the speaker\0");
        }
        return false;
    }
    if ret != ESP_OK {
        log!(ESP_LOG_WARN, b"routing the speaker failed (0x%x)\0", ret);
    }
    ret == ESP_OK
}

/// The motor's ring, or its message buzz from a driver that has no ring.
fn buzz() {
    if crate::hal_registry::haptic_play(HAL_HAPTIC_RING) == ESP_ERR_INVALID_ARG {
        crate::hal_registry::haptic_play(HAL_HAPTIC_MESSAGE);
    }
}

// ---------------------------------------------------------------------------
// The call thread
// ---------------------------------------------------------------------------

struct CallThread {
    drv: &'static HalNetDriver,
    ring: Option<Ring>,
    /// The speaker is the modem's.
    voice: bool,
    /// What was sent to the modem for the call connected; None to send.
    sent_speaker: Option<bool>,
    sent_mute: Option<bool>,
    /// Since when call_list has failed, if it does.
    failing_since: Option<Instant>,
    /// A call command went to the modem, or a listing failed, and call_list
    /// has not since said there is no call: the driver still holds the line.
    dirty: bool,
    last_poll: Instant,
    /// The phonebook as last asked for the call's name.
    book_seq: Option<u32>,
}

impl CallThread {
    fn new(drv: &'static HalNetDriver) -> CallThread {
        CallThread {
            drv,
            ring: None,
            voice: false,
            sent_speaker: None,
            sent_mute: None,
            failing_since: None,
            dirty: false,
            last_poll: Instant::now(),
            book_seq: None,
        }
    }

    fn ringing(&self) -> bool {
        self.drv.call_ringing.is_some_and(|f| unsafe { f() })
    }

    fn run_commands(&mut self) {
        let commands = std::mem::take(&mut lock().commands);
        for command in commands {
            self.dirty = true;
            match command {
                Command::Dial(number) => {
                    let number = c_string(&number);
                    log!(ESP_LOG_INFO, b"dialing %s\0", number.as_ptr());
                    crate::net_coordinator::cancel_attempt();
                    let ret = self.drv.call_dial.map_or(ESP_ERR_NOT_SUPPORTED, |f| unsafe { f(number.as_ptr()) });
                    if ret != ESP_OK {
                        log!(ESP_LOG_WARN, b"the modem did not take the call (0x%x)\0", ret);
                        let event = lock().m.dial_failed(Instant::now());
                        if let Some(event) = event {
                            self.handle(event);
                        }
                    }
                }
                Command::Answer => {
                    let ret = self.drv.call_answer.map_or(ESP_ERR_NOT_SUPPORTED, |f| unsafe { f() });
                    if ret != ESP_OK {
                        log!(ESP_LOG_WARN, b"answering failed (0x%x)\0", ret);
                    }
                }
                Command::Hangup => {
                    let ret = self.drv.call_hangup.map_or(ESP_ERR_NOT_SUPPORTED, |f| unsafe { f() });
                    if ret != ESP_OK {
                        log!(ESP_LOG_WARN, b"hanging up failed (0x%x)\0", ret);
                    }
                }
                Command::Dtmf(digit) => {
                    if let Some(f) = self.drv.call_dtmf {
                        unsafe { f(digit as c_char) };
                    }
                }
            }
        }
    }

    /// Ask the modem for its calls, and move the machine on.
    fn poll(&mut self) {
        let Some(list) = self.drv.call_list else {
            return;
        };
        let mut buf = [CHalCall::EMPTY; LIST_MAX];
        let mut count: i32 = 0;
        let ret = unsafe { list(buf.as_mut_ptr(), LIST_MAX as i32, &mut count) };
        let now = Instant::now();
        self.last_poll = now;
        let calls = if ret == ESP_OK {
            self.failing_since = None;
            if count <= 0 {
                self.dirty = false;
            }
            let n = count.clamp(0, LIST_MAX as i32) as usize;
            Some(buf[..n].iter().map(Listed::from_hal).collect::<Vec<_>>())
        } else {
            let since = *self.failing_since.get_or_insert(now);
            // A modem asleep holds nothing; one that answered badly may.
            if ret != ESP_ERR_INVALID_STATE {
                self.dirty = true;
            }
            let (in_progress, listed) = {
                let s = lock();
                (s.m.in_progress(), s.m.listed)
            };
            let grace = if listed { LOST_GRACE } else { WAKE_GRACE };
            (in_progress && now.duration_since(since) >= grace).then(Vec::new)
        };
        let Some(calls) = calls else {
            return;
        };
        let events = lock().m.listed(&calls, now, crate::msg_log::now());
        for event in events {
            self.handle(event);
        }
        self.look_up_name();
    }

    /// The phonebook's name for a call that has none yet, asked again
    /// whenever the phonebook changes (it may still be reading the card).
    fn look_up_name(&mut self) {
        let number = {
            let s = lock();
            if !s.m.in_progress() || !s.m.name.is_empty() || s.m.number.is_empty() {
                return;
            }
            s.m.number.clone()
        };
        let seq = crate::phonebook::seq();
        if self.book_seq == Some(seq) {
            return;
        }
        self.book_seq = Some(seq);
        if let Some(name) = crate::phonebook::name_for(&number) {
            lock().m.set_name(&number, name);
        }
    }

    fn handle(&mut self, event: Event) {
        match event {
            Event::Ringing(number) => {
                if number.is_empty() {
                    log!(ESP_LOG_INFO, b"incoming call, number withheld\0");
                } else {
                    let number = c_string(&number);
                    log!(ESP_LOG_INFO, b"incoming call from %s\0", number.as_ptr());
                }
                self.book_seq = None;
                crate::net_coordinator::cancel_attempt();
            }
            Event::RingStopped => self.stop_ring(),
            Event::Connected => {
                log!(ESP_LOG_INFO, b"connected\0");
                self.stop_ring();
                if let Some(stop) = audio().and_then(|a| a.stop) {
                    unsafe { stop() };
                }
                // sync_settings() routes the speaker and sets the modem up.
                self.sent_speaker = None;
                self.sent_mute = None;
            }
            Event::Ended(ending) => {
                self.stop_ring();
                if self.voice {
                    route(HAL_AUDIO_ROUTE_CODEC, false);
                    self.voice = false;
                }
                self.sent_speaker = None;
                self.sent_mute = None;
                match ending.end {
                    End::Local => log!(ESP_LOG_INFO, b"ended: hung up after %u s\0", ending.duration_s),
                    End::Remote => {
                        log!(ESP_LOG_INFO, b"ended: remote hung up after %u s\0", ending.duration_s)
                    }
                    End::Rejected => log!(ESP_LOG_INFO, b"ended: declined\0"),
                    End::Missed => log!(ESP_LOG_INFO, b"ended: missed\0"),
                    End::Failed | End::None => log!(ESP_LOG_INFO, b"ended: failed\0"),
                }
                call_log::add(ending.kind(), &ending.number, ending.began, ending.duration_s);
            }
        }
    }

    /// The speaker, the microphone and the volume, to the modem and the
    /// audio switch, for a connected call: once when it connects, and again
    /// as the user changes them.
    fn sync_settings(&mut self) {
        let (state, speaker, muted, volume) = {
            let s = lock();
            (s.m.state, s.m.speaker, s.m.muted, s.m.volume)
        };
        if state != State::Active {
            return;
        }
        if self.sent_speaker != Some(speaker) {
            if let Some(f) = self.drv.call_set_speaker {
                unsafe { f(speaker) };
            }
            if route(HAL_AUDIO_ROUTE_VOICE, speaker) {
                self.voice = true;
            }
            self.sent_speaker = Some(speaker);
        }
        if self.sent_mute != Some(muted) {
            if let Some(f) = self.drv.call_set_mute {
                unsafe { f(muted) };
            }
            self.sent_mute = Some(muted);
        }
        if VOLUME_SENT.load(Ordering::Relaxed) != volume as u16 {
            if let Some(f) = self.drv.call_set_volume {
                unsafe { f(volume) };
            }
            // Not sent again until it changes, even if the modem refused.
            VOLUME_SENT.store(volume as u16, Ordering::Relaxed);
        }
    }

    /// One step of the ringtone and the buzzing, if a call rings here: plays
    /// at most 400 ms, so the modem is still asked often. When the next step
    /// is due, while it rings.
    fn ring(&mut self) -> Option<Instant> {
        let (audible, now) = (lock().m.audible(), Instant::now());
        if !audible {
            self.stop_ring();
            return None;
        }
        let ring = self.ring.get_or_insert_with(|| Ring::new(now));
        if now < ring.due {
            return Some(ring.due);
        }
        let (tone, ms) = CADENCE[ring.step];
        if ring.step == 0 {
            buzz();
        }
        let player = audio().filter(|a| a.play.is_some() && !ring.quiet);
        let play = player.is_some() && !crate::audio_volume::rs_audio_is_muted() && (tone || ms <= SHORT_GAP_MS);
        let mut played = false;
        if let (true, Some(a)) = (play, player) {
            if !ring.configured {
                ring.configured = true;
                let cfg = HalAudioConfig { sample_rate: RING_RATE, bits_per_sample: 16, channels: 1 };
                if let Some(configure) = a.configure {
                    unsafe { configure(&cfg) };
                }
            }
            let samples = if tone {
                if ring.tone.is_empty() {
                    ring.tone = ring_tone(ms);
                }
                &ring.tone
            } else {
                if ring.gap.is_empty() {
                    ring.gap = vec![0u8; (RING_RATE as u64 * ms / 1000 * 2) as usize];
                }
                &ring.gap
            };
            // Blocks while the samples are written: the step's length, less
            // what the I2S DMA buffers hold.
            let ret = a.play.map_or(ESP_ERR_NOT_SUPPORTED, |p| unsafe { p(samples.as_ptr(), samples.len()) });
            if ret == ESP_OK {
                ring.sounding = true;
                played = true;
            } else {
                ring.quiet = true;
            }
        }
        ring.due = if played { Instant::now() } else { now + Duration::from_millis(ms) };
        ring.step = (ring.step + 1) % CADENCE.len();
        Some(ring.due)
    }

    fn stop_ring(&mut self) {
        if let Some(ring) = self.ring.take() {
            if ring.sounding {
                if let Some(stop) = audio().and_then(|a| a.stop) {
                    unsafe { stop() };
                }
            }
        }
    }

    /// Whether the thread is done: no call up, nothing asked, and the modem
    /// has listed no call since the last command, or has not answered for
    /// GIVE_UP. Decided under the lock, so a call dialled meanwhile either
    /// finds this thread still running or starts another.
    fn done(&mut self) -> bool {
        let now = Instant::now();
        let failing = self.failing_since.map(|t| now.duration_since(t));
        // RI says a call rings but the modem is still waking up.
        let waking = failing.is_some_and(|d| d < WAKE_GRACE) && self.ringing();
        let gave_up = failing.is_some_and(|d| d >= GIVE_UP);
        let mut s = lock();
        if !s.commands.is_empty() || s.m.in_progress() {
            return false;
        }
        if (self.dirty || waking) && !gave_up {
            return false;
        }
        if gave_up && self.dirty {
            log!(ESP_LOG_WARN, b"the modem never said the call was over; giving up\0");
        }
        s.thread = false;
        true
    }

    /// Until `until`, or until the UI asks for something.
    fn wait(&self, until: Instant) {
        let mut s = lock();
        loop {
            if s.poke || !s.commands.is_empty() {
                s.poke = false;
                return;
            }
            let now = Instant::now();
            if now >= until {
                return;
            }
            s = WAKE.wait_timeout(s, until - now).unwrap_or_else(PoisonError::into_inner).0;
        }
    }

    fn leave(&mut self) {
        self.stop_ring();
        if self.voice {
            route(HAL_AUDIO_ROUTE_CODEC, false);
            self.voice = false;
        }
        #[cfg(target_os = "espidf")]
        {
            let left = unsafe { uxTaskGetStackHighWaterMark(std::ptr::null_mut()) };
            if left < 1536 {
                log!(ESP_LOG_WARN, b"the call thread left %u of %u bytes of stack unused\0", left, STACK_SIZE as u32);
            }
        }
        // The data session may be dialled again, and SMS fetched.
        crate::net_coordinator::poke();
    }
}

fn run(drv: &'static HalNetDriver) {
    {
        let _awake = HoldGuard::new(b"a call\0");
        load_volume();
        let mut t = CallThread::new(drv);
        loop {
            t.run_commands();
            t.poll();
            t.sync_settings();
            if t.done() {
                break;
            }
            let ring_due = t.ring();
            let next_poll = t.last_poll + POLL;
            t.wait(ring_due.map_or(next_poll, |due| due.min(next_poll)));
        }
        t.leave();
    }
    LIVE.fetch_sub(1, Ordering::SeqCst);
}

/// Start the call thread unless one runs.
fn start_thread(drv: &'static HalNetDriver) -> bool {
    {
        let mut s = lock();
        if s.thread {
            return true;
        }
        s.thread = true;
    }
    // From here the coordinator leaves the modem alone; an attempt it has
    // under way would hold the modem for up to two minutes.
    crate::net_coordinator::cancel_attempt();
    #[cfg(target_os = "espidf")]
    unsafe {
        thistle_pthread_name_next(b"call\0".as_ptr())
    };
    LIVE.fetch_add(1, Ordering::SeqCst);
    let spawned = std::thread::Builder::new()
        .name("call".to_string())
        .stack_size(STACK_SIZE)
        .spawn(move || run(drv))
        .is_ok();
    #[cfg(target_os = "espidf")]
    unsafe {
        thistle_pthread_name_next(std::ptr::null())
    };
    if !spawned {
        LIVE.fetch_sub(1, Ordering::SeqCst);
        lock().thread = false;
        log!(ESP_LOG_WARN, b"could not start the call thread\0");
    }
    spawned
}

/// The kernel worker's look at RI, once a second: a call ringing here starts
/// the thread. Never waits: RI is a pin.
unsafe extern "C" fn watch(_arg: *mut c_void) {
    let Some(drv) = calls_driver() else {
        return;
    };
    if lock().thread {
        return;
    }
    if drv.call_ringing.is_some_and(|f| f()) {
        start_thread(drv);
    }
}

/// Watch for calls, on a board whose modem makes them. kernel_init() calls
/// this once the drivers and the network coordinator have started.
pub(crate) fn start() {
    if calls_driver().is_none() {
        return;
    }
    crate::kworker::every(watch, std::ptr::null_mut(), WATCH_PERIOD);
}

fn dial_with(drv: &'static HalNetDriver, number: &str) -> i32 {
    if !crate::net_coordinator::modem_ready(drv) {
        return ESP_ERR_INVALID_STATE;
    }
    let name = crate::phonebook::name_for(number).unwrap_or_default();
    load_volume();
    {
        let mut s = lock();
        if let Err(e) = s.m.dial(number, name, Instant::now(), crate::msg_log::now()) {
            return e;
        }
        s.commands.push(Command::Dial(number.to_string()));
        wake_thread(s);
    }
    if !start_thread(drv) {
        let mut s = lock();
        s.commands.clear();
        s.m.abandon();
        return ESP_ERR_NO_MEM;
    }
    ESP_OK
}

// ---------------------------------------------------------------------------
// FFI — thistle/call.h
// ---------------------------------------------------------------------------

/// call_status_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CCallStatus {
    /// call_state_t
    pub state: u32,
    /// call_end_t
    pub end: u32,
    pub incoming: bool,
    pub number: [u8; CALL_NUMBER_MAX],
    pub name: [u8; CALL_NAME_MAX],
    pub duration_s: u32,
    pub muted: bool,
    pub speaker: bool,
    pub volume: u8,
    pub seq: u32,
}

/// Whether a call can be placed now: a modem that makes calls, on with a
/// SIM ready, and no call up.
#[no_mangle]
pub extern "C" fn rs_call_available() -> bool {
    match calls_driver() {
        Some(drv) => crate::net_coordinator::modem_ready(drv) && !busy(),
        None => false,
    }
}

#[no_mangle]
pub extern "C" fn rs_call_supported() -> bool {
    calls_driver().is_some()
}

/// Place a call. Returns at once, with the call DIALING.
///
/// # Safety
/// `number` must be NULL or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn rs_call_dial(number: *const c_char) -> i32 {
    if number.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let Some(number) = std::ffi::CStr::from_ptr(number).to_str().ok().and_then(normalize) else {
        return ESP_ERR_INVALID_ARG;
    };
    let Some(drv) = calls_driver() else {
        return ESP_ERR_NOT_SUPPORTED;
    };
    dial_with(drv, &number)
}

#[no_mangle]
pub extern "C" fn rs_call_answer() -> i32 {
    let mut s = lock();
    if let Err(e) = s.m.answer(Instant::now()) {
        return e;
    }
    s.commands.push(Command::Answer);
    wake_thread(s);
    ESP_OK
}

#[no_mangle]
pub extern "C" fn rs_call_hangup() -> i32 {
    let mut s = lock();
    if let Err(e) = s.m.hangup(Instant::now()) {
        return e;
    }
    s.commands.push(Command::Hangup);
    wake_thread(s);
    ESP_OK
}

#[no_mangle]
pub extern "C" fn rs_call_silence() {
    let mut s = lock();
    if s.m.silence() {
        wake_thread(s);
        log!(ESP_LOG_INFO, b"ringing silenced\0");
    }
}

#[no_mangle]
pub extern "C" fn rs_call_dtmf(digit: c_char) -> i32 {
    let digit = (digit as u8).to_ascii_uppercase();
    if !matches!(digit, b'0'..=b'9' | b'*' | b'#' | b'A'..=b'D') {
        return ESP_ERR_INVALID_ARG;
    }
    let mut s = lock();
    s.m.settle(Instant::now());
    if s.m.state != State::Active {
        return ESP_ERR_INVALID_STATE;
    }
    s.commands.push(Command::Dtmf(digit));
    wake_thread(s);
    ESP_OK
}

#[no_mangle]
pub extern "C" fn rs_call_set_mute(mute: bool) -> i32 {
    let mut s = lock();
    if let Err(e) = s.m.set_mute(mute, Instant::now()) {
        return e;
    }
    wake_thread(s);
    ESP_OK
}

#[no_mangle]
pub extern "C" fn rs_call_set_speaker(on: bool) -> i32 {
    let mut s = lock();
    if let Err(e) = s.m.set_speaker(on, Instant::now()) {
        return e;
    }
    wake_thread(s);
    ESP_OK
}

/// The call's volume, this call's and the next ones'. Saved; the return says
/// whether that worked.
#[no_mangle]
pub extern "C" fn rs_call_set_volume(percent: u8) -> i32 {
    if percent > 100 {
        return ESP_ERR_INVALID_ARG;
    }
    load_volume();
    let mut s = lock();
    s.m.set_volume(percent);
    wake_thread(s);
    if cfg!(test) {
        return ESP_OK;
    }
    crate::system_json::save(SECTION, VOLUME_KEY, serde_json::Value::from(percent))
}

/// # Safety
/// `out` must be NULL or point to a writable call_status_t.
#[no_mangle]
pub unsafe extern "C" fn rs_call_get_status(out: *mut CCallStatus) {
    if out.is_null() {
        return;
    }
    load_volume();
    *out = lock().m.status(Instant::now());
}

#[no_mangle]
pub extern "C" fn rs_call_in_progress() -> bool {
    lock().m.in_progress()
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn out(id: u8, stat: u32) -> Listed {
        Listed { id, incoming: false, stat, number: "+491701".into() }
    }

    fn inc(id: u8, stat: u32, number: &str) -> Listed {
        Listed { id, incoming: true, stat, number: number.into() }
    }

    fn ended(events: &[Event]) -> Option<&Ending> {
        events.iter().find_map(|e| match e {
            Event::Ended(e) => Some(e),
            _ => None,
        })
    }

    fn ms(n: u64) -> Duration {
        Duration::from_millis(n)
    }

    fn dialled(t0: Instant) -> Machine {
        let mut m = Machine::new();
        m.dial("+491701", "Anna".into(), t0, Some(1_760_000_000)).unwrap();
        m
    }

    #[test]
    fn numbers_are_normalised_for_dialling() {
        assert_eq!(normalize("+49 (176) 123-45.67").as_deref(), Some("+491761234567"));
        assert_eq!(normalize("*#06#").as_deref(), Some("*#06#"));
        assert_eq!(normalize("0049\t30 1234").as_deref(), Some("0049301234"));
        assert_eq!(normalize(""), None);
        assert_eq!(normalize("+ - ()"), None, "no digit");
        assert_eq!(normalize("*#"), None, "no digit");
        assert_eq!(normalize("0176 abc"), None);
        assert_eq!(normalize("49+176"), None, "'+' only first");
        assert_eq!(normalize("1234;ATH"), None);
        assert_eq!(normalize(&"1".repeat(32)).map(|s| s.len()), Some(32));
        assert_eq!(normalize(&"1".repeat(33)), None);
    }

    #[test]
    fn the_saved_volume_is_read_or_defaults() {
        assert_eq!(volume_in(None), DEFAULT_VOLUME);
        assert_eq!(volume_in(Some(r#"{"thistle_os":{"phone":{"call_volume":40}}}"#)), 40);
        assert_eq!(volume_in(Some(r#"{"thistle_os":{"phone":{"call_volume":140}}}"#)), DEFAULT_VOLUME);
        assert_eq!(volume_in(Some(r#"{"thistle_os":{"phone":{"call_volume":"loud"}}}"#)), DEFAULT_VOLUME);
    }

    #[test]
    fn a_dialled_call_shows_at_once_and_follows_the_modem_to_connected() {
        let t0 = Instant::now();
        let mut m = dialled(t0);
        let s = m.status(t0);
        assert_eq!((s.state, s.incoming, s.speaker, s.muted), (State::Dialing as u32, false, true, false));
        assert_eq!(&s.name[..5], b"Anna\0");
        let seq = s.seq;

        assert!(m.listed(&[out(1, STAT_DIALING)], t0 + ms(500), None).is_empty());
        assert_eq!(m.status(t0).seq, seq, "nothing changed");
        assert!(m.listed(&[out(1, STAT_ALERTING)], t0 + ms(1000), None).is_empty());
        assert_eq!(m.status(t0 + ms(1000)).state, State::Alerting as u32);
        assert_eq!(m.listed(&[out(1, STAT_ACTIVE)], t0 + ms(3000), None), [Event::Connected]);
        let a = m.status(t0 + ms(4500));
        let b = m.status(t0 + ms(5600));
        assert_eq!((a.state, a.duration_s, b.duration_s), (State::Active as u32, 1, 2));
        assert_eq!(a.seq, b.seq, "the connected time counting changes no seq");

        // The far end hangs up.
        let events = m.listed(&[], t0 + ms(66_000), None);
        let e = ended(&events).unwrap();
        assert_eq!((e.end, e.duration_s, e.incoming), (End::Remote, 63, false));
        assert_eq!(e.kind(), Kind::Outgoing);
        assert_eq!(e.began, Some(1_760_000_000));
        let s = m.status(t0 + ms(66_000));
        assert_eq!((s.state, s.end, s.duration_s), (State::Ended as u32, End::Remote as u32, 63));
    }

    #[test]
    fn a_dialled_call_not_yet_listed_is_waited_for_six_seconds() {
        let t0 = Instant::now();
        let mut m = dialled(t0);
        assert!(m.listed(&[], t0 + ms(5900), None).is_empty());
        assert_eq!(m.status(t0 + ms(5900)).state, State::Dialing as u32);
        let events = m.listed(&[], t0 + DIAL_LIST_WAIT, None);
        assert_eq!(ended(&events).unwrap().end, End::Failed);
    }

    #[test]
    fn a_dialled_call_that_was_listed_and_went_failed() {
        let t0 = Instant::now();
        let mut m = dialled(t0);
        m.listed(&[out(1, STAT_ALERTING)], t0 + ms(500), None);
        // Busy, or nobody answered: never connected, nobody here hung up.
        let events = m.listed(&[out(1, STAT_RELEASED)], t0 + ms(900), None);
        let e = ended(&events).unwrap();
        assert_eq!((e.end, e.duration_s, e.kind()), (End::Failed, 0, Kind::Outgoing));
    }

    #[test]
    fn a_call_the_modem_refused_failed_or_was_cancelled() {
        let t0 = Instant::now();
        let mut m = dialled(t0);
        assert_eq!(m.dial_failed(t0).map(|e| matches!(e, Event::Ended(Ending { end: End::Failed, .. }))), Some(true));
        let mut m = dialled(t0);
        m.hangup(t0).unwrap();
        assert!(matches!(m.dial_failed(t0), Some(Event::Ended(Ending { end: End::Local, .. }))));
    }

    #[test]
    fn hanging_up_ends_the_call_when_the_modem_drops_it() {
        let t0 = Instant::now();
        // Cancelled before the modem even listed it.
        let mut m = dialled(t0);
        m.hangup(t0 + ms(100)).unwrap();
        assert_eq!(m.status(t0 + ms(100)).state, State::Dialing as u32, "until the modem says so");
        assert_eq!(ended(&m.listed(&[], t0 + ms(400), None)).unwrap().end, End::Local);

        // Hung up after talking.
        let mut m = dialled(t0);
        m.listed(&[out(1, STAT_ACTIVE)], t0 + ms(1000), None);
        m.hangup(t0 + ms(11_000)).unwrap();
        let events = m.listed(&[], t0 + ms(11_500), None);
        let e = ended(&events).unwrap();
        assert_eq!((e.end, e.duration_s), (End::Local, 10));
    }

    #[test]
    fn an_incoming_call_rings_is_answered_and_ends() {
        let t0 = Instant::now();
        let mut m = Machine::new();
        assert_eq!(m.answer(t0), Err(ESP_ERR_INVALID_STATE));
        let events = m.listed(&[inc(1, STAT_INCOMING, "+491702")], t0, Some(1_760_000_100));
        assert_eq!(events, [Event::Ringing("+491702".into())]);
        let s = m.status(t0);
        assert_eq!((s.state, s.incoming), (State::Incoming as u32, true));
        assert_eq!(&s.number[..8], b"+491702\0");
        assert!(m.audible());

        m.set_name("+491702", "Bob".into());
        assert_eq!(&m.status(t0).name[..4], b"Bob\0");

        m.answer(t0 + ms(2000)).unwrap();
        assert!(!m.audible(), "the ring stops when the user answers");
        assert_eq!(m.status(t0 + ms(2000)).state, State::Incoming as u32);
        let events = m.listed(&[inc(1, STAT_ACTIVE, "+491702")], t0 + ms(2500), None);
        assert_eq!(events, [Event::RingStopped, Event::Connected]);
        let events = m.listed(&[], t0 + ms(32_500), None);
        let e = ended(&events).unwrap();
        assert_eq!((e.end, e.duration_s, e.kind()), (End::Remote, 30, Kind::Incoming));
    }

    #[test]
    fn an_incoming_call_declined_or_missed() {
        let t0 = Instant::now();
        let mut m = Machine::new();
        m.listed(&[inc(1, STAT_INCOMING, "+491702")], t0, None);
        m.hangup(t0 + ms(100)).unwrap();
        let events = m.listed(&[], t0 + ms(600), None);
        assert_eq!(events[0], Event::RingStopped);
        let e = ended(&events).unwrap();
        assert_eq!((e.end, e.kind()), (End::Rejected, Kind::Rejected));

        let mut m = Machine::new();
        m.listed(&[inc(1, STAT_INCOMING, "")], t0, None);
        let events = m.listed(&[], t0 + ms(20_000), None);
        let e = ended(&events).unwrap();
        assert_eq!((e.end, e.kind(), e.number.as_str()), (End::Missed, Kind::Missed, ""));
    }

    #[test]
    fn a_withheld_number_rings_with_no_number() {
        let mut m = Machine::new();
        assert_eq!(m.listed(&[inc(1, STAT_INCOMING, "")], Instant::now(), None), [Event::Ringing(String::new())]);
    }

    #[test]
    fn silencing_stops_the_ring_once_and_only_for_a_ringing_call() {
        let t0 = Instant::now();
        let mut m = Machine::new();
        assert!(!m.silence());
        m.listed(&[inc(1, STAT_INCOMING, "+491702")], t0, None);
        assert!(m.silence());
        assert!(!m.silence());
        assert!(!m.audible());
        assert_eq!(m.status(t0).state, State::Incoming as u32, "it still rings for the caller");
    }

    #[test]
    fn an_ended_call_reads_as_idle_after_it_has_been_shown() {
        let t0 = Instant::now();
        let mut m = Machine::new();
        m.listed(&[inc(1, STAT_INCOMING, "+491702")], t0, None);
        m.listed(&[], t0 + ms(1000), None);
        let shown = m.status(t0 + ms(1000) + ENDED_SHOW - ms(1));
        assert_eq!(shown.state, State::Ended as u32);
        let idle = m.status(t0 + ms(1000) + ENDED_SHOW);
        assert_eq!((idle.state, idle.end, idle.number[0]), (State::Idle as u32, End::None as u32, 0));
        assert_eq!(idle.seq, shown.seq.wrapping_add(1));
        assert_eq!(m.status(t0 + ms(60_000)).seq, idle.seq, "once");
    }

    #[test]
    fn nothing_to_answer_hang_up_or_mute_without_a_call() {
        let t0 = Instant::now();
        let mut m = Machine::new();
        assert_eq!(m.hangup(t0), Err(ESP_ERR_INVALID_STATE));
        assert_eq!(m.set_mute(true, t0), Err(ESP_ERR_INVALID_STATE));
        assert_eq!(m.set_speaker(false, t0), Err(ESP_ERR_INVALID_STATE));
        let mut m = dialled(t0);
        assert_eq!(m.answer(t0), Err(ESP_ERR_INVALID_STATE), "an outgoing call is not answered");
        assert_eq!(m.dial("+49", String::new(), t0, None), Err(ESP_ERR_INVALID_STATE));
        // A call that just ended is no call: the next may be placed.
        m.listed(&[], t0 + DIAL_LIST_WAIT, None);
        assert_eq!(m.dial("+491703", String::new(), t0 + DIAL_LIST_WAIT, None), Ok(()));
    }

    #[test]
    fn mute_speaker_and_volume_change_the_seq_only_when_they_change() {
        let t0 = Instant::now();
        let mut m = dialled(t0);
        let seq = m.status(t0).seq;
        m.set_mute(true, t0).unwrap();
        m.set_speaker(false, t0).unwrap();
        m.set_volume(30);
        let s = m.status(t0);
        assert_eq!((s.muted, s.speaker, s.volume, s.seq), (true, false, 30, seq.wrapping_add(3)));
        m.set_mute(true, t0).unwrap();
        m.set_volume(30);
        assert_eq!(m.status(t0).seq, s.seq);
        // The next call starts unmuted, on the speaker, at the volume set.
        m.listed(&[], t0 + DIAL_LIST_WAIT, None);
        m.dial("+491703", String::new(), t0 + DIAL_LIST_WAIT, None).unwrap();
        let s = m.status(t0 + DIAL_LIST_WAIT);
        assert_eq!((s.muted, s.speaker, s.volume), (false, true, 30));
    }

    #[test]
    fn only_the_first_call_is_followed() {
        let t0 = Instant::now();
        let mut m = dialled(t0);
        m.listed(&[out(1, STAT_ACTIVE)], t0, None);
        // A second caller waits; this call goes on as it was.
        assert!(m.listed(&[out(1, STAT_ACTIVE), inc(2, STAT_WAITING, "+491709")], t0 + ms(500), None).is_empty());
        assert_eq!(m.status(t0 + ms(500)).state, State::Active as u32);
        // A call placed does not take an incoming one for itself.
        let mut m = dialled(t0);
        assert!(m.listed(&[inc(1, STAT_INCOMING, "+491709")], t0 + ms(500), None).is_empty());
        assert_eq!(m.status(t0 + ms(500)).state, State::Dialing as u32);
    }

    #[test]
    fn a_call_just_declined_that_the_modem_still_lists_is_not_a_new_one() {
        let t0 = Instant::now();
        let mut m = Machine::new();
        m.listed(&[inc(1, STAT_INCOMING, "+491702")], t0, None);
        m.hangup(t0).unwrap();
        m.listed(&[], t0 + ms(500), None);
        assert!(m.listed(&[inc(1, STAT_INCOMING, "+491702")], t0 + ms(1000), None).is_empty());
        assert_eq!(m.status(t0 + ms(1000)).end, End::Rejected as u32);
        // Still ringing once that is over: a new call.
        let later = t0 + ms(500) + ENDED_SHOW;
        assert_eq!(m.listed(&[inc(1, STAT_INCOMING, "+491702")], later, None), [Event::Ringing("+491702".into())]);
        // Another index at once is a new call too.
        let mut m = Machine::new();
        m.listed(&[inc(1, STAT_INCOMING, "+491702")], t0, None);
        m.listed(&[], t0 + ms(500), None);
        assert_eq!(m.listed(&[inc(2, STAT_INCOMING, "+491704")], t0 + ms(600), None).len(), 1);
    }

    #[test]
    fn a_call_met_already_connected_is_taken_up() {
        let t0 = Instant::now();
        let mut m = Machine::new();
        assert_eq!(m.listed(&[inc(1, STAT_ACTIVE, "+491702")], t0, None), [Event::Connected]);
        assert_eq!(m.status(t0).state, State::Active as u32);
    }

    #[test]
    fn the_ringtone_is_400_ms_of_16_khz_samples_that_fade_in_and_out() {
        let tone = ring_tone(400);
        assert_eq!(tone.len(), 6400 * 2);
        let sample = |i: usize| i16::from_le_bytes([tone[2 * i], tone[2 * i + 1]]);
        assert_eq!(sample(0), 0);
        assert_eq!(sample(6399), 0);
        let peak = (0..6400).map(|i| sample(i).unsigned_abs()).max().unwrap();
        assert!(peak > 10_000 && peak as f32 <= 2.0 * RING_LEVEL * i16::MAX as f32 + 1.0, "{peak}");
        assert_eq!(CADENCE.iter().map(|&(_, ms)| ms).sum::<u64>(), 2000, "the motor buzzes every two seconds");
    }

    #[test]
    fn the_status_struct_matches_the_c_layout() {
        // Two enums, bool, char[33], char[64], pad to 108, uint32_t, bool,
        // bool, uint8_t, pad to 116, uint32_t.
        assert_eq!(std::mem::offset_of!(CCallStatus, incoming), 8);
        assert_eq!(std::mem::offset_of!(CCallStatus, number), 9);
        assert_eq!(std::mem::offset_of!(CCallStatus, name), 42);
        assert_eq!(std::mem::offset_of!(CCallStatus, duration_s), 108);
        assert_eq!(std::mem::offset_of!(CCallStatus, muted), 112);
        assert_eq!(std::mem::offset_of!(CCallStatus, speaker), 113);
        assert_eq!(std::mem::offset_of!(CCallStatus, volume), 114);
        assert_eq!(std::mem::offset_of!(CCallStatus, seq), 116);
        assert_eq!(std::mem::size_of::<CCallStatus>(), 120);
    }

    // ── The thread, against a fake modem ────────────────────────────────

    mod fake {
        use super::super::*;
        use crate::net_manager::{CCellInfo, HAL_NET_CELLULAR};
        use std::sync::atomic::AtomicI32;

        pub static CALLS: Mutex<Vec<CHalCall>> = Mutex::new(Vec::new());
        pub static SENT: Mutex<Vec<String>> = Mutex::new(Vec::new());
        pub static RINGING: AtomicBool = AtomicBool::new(false);
        pub static DIAL_RET: AtomicI32 = AtomicI32::new(0);
        pub static LIST_RET: AtomicI32 = AtomicI32::new(0);

        pub fn call(id: u8, incoming: bool, state: u32, number: &str) -> CHalCall {
            let mut c = CHalCall::EMPTY;
            c.id = id;
            c.incoming = incoming;
            c.state = state;
            copy_c(&mut c.number, number);
            c
        }

        pub fn set(calls: &[CHalCall]) {
            *CALLS.lock().unwrap() = calls.to_vec();
        }

        pub fn sent() -> Vec<String> {
            SENT.lock().unwrap().clone()
        }

        fn send(s: String) {
            SENT.lock().unwrap().push(s);
        }

        unsafe extern "C" fn get_cell_info(out: *mut CCellInfo) -> i32 {
            (*out).powered = true;
            (*out).sim = 1;
            0
        }
        unsafe extern "C" fn dial(number: *const c_char) -> i32 {
            send(format!("ATD{};", std::ffi::CStr::from_ptr(number).to_str().unwrap()));
            DIAL_RET.load(Ordering::SeqCst)
        }
        unsafe extern "C" fn answer() -> i32 {
            send("ATA".into());
            for c in CALLS.lock().unwrap().iter_mut() {
                c.state = STAT_ACTIVE;
            }
            0
        }
        unsafe extern "C" fn hangup() -> i32 {
            send("AT+CHUP".into());
            CALLS.lock().unwrap().clear();
            0
        }
        unsafe extern "C" fn list(calls: *mut CHalCall, max: i32, count: *mut i32) -> i32 {
            let ret = LIST_RET.load(Ordering::SeqCst);
            if ret != 0 {
                return ret;
            }
            let have = CALLS.lock().unwrap();
            let n = have.len().min(max as usize);
            for (i, c) in have.iter().take(n).enumerate() {
                *calls.add(i) = *c;
            }
            *count = n as i32;
            0
        }
        unsafe extern "C" fn ringing() -> bool {
            RINGING.load(Ordering::SeqCst)
        }
        unsafe extern "C" fn dtmf(d: c_char) -> i32 {
            send(format!("AT+VTS={}", d as u8 as char));
            0
        }
        unsafe extern "C" fn mute(m: bool) -> i32 {
            send(format!("AT+CMUT={}", m as u8));
            0
        }
        unsafe extern "C" fn volume(v: u8) -> i32 {
            send(format!("volume {v}"));
            0
        }
        unsafe extern "C" fn speaker(s: bool) -> i32 {
            send(format!("AT+CSDVC={}", if s { 3 } else { 1 }));
            0
        }

        pub static MODEM: HalNetDriver = HalNetDriver {
            transport_type: HAL_NET_CELLULAR,
            name: b"Fake modem\0".as_ptr() as *const c_char,
            init: None,
            connect: None,
            disconnect: None,
            get_state: None,
            get_ip: None,
            get_rssi: None,
            is_connected: None,
            get_cell_info: Some(get_cell_info),
            cancel: None,
            sleep: None,
            listen: None,
            sms_send: None,
            sms_due: None,
            sms_fetch: None,
            get_network_time: None,
            call_dial: Some(dial),
            call_answer: Some(answer),
            call_hangup: Some(hangup),
            call_list: Some(list),
            call_ringing: Some(ringing),
            call_dtmf: Some(dtmf),
            call_set_mute: Some(mute),
            call_set_volume: Some(volume),
            call_set_speaker: Some(speaker),
        };
    }

    /// Every call thread over, its hold on the power manager released.
    fn finished() {
        wait_for(|| !busy() && LIVE.load(Ordering::SeqCst) == 0, "the call thread to end");
    }

    fn fresh() {
        finished();
        let mut s = lock();
        s.m = Machine::new();
        s.commands.clear();
        s.poke = false;
        s.volume_loaded = true;
        drop(s);
        fake::set(&[]);
        fake::SENT.lock().unwrap().clear();
        fake::RINGING.store(false, Ordering::SeqCst);
        fake::DIAL_RET.store(0, Ordering::SeqCst);
        fake::LIST_RET.store(0, Ordering::SeqCst);
    }

    fn wait_for(f: impl Fn() -> bool, what: &str) {
        let t0 = Instant::now();
        while !f() {
            assert!(t0.elapsed() < Duration::from_secs(10), "waited too long for {what}");
            std::thread::sleep(ms(2));
        }
    }

    fn status() -> CCallStatus {
        let mut s: CCallStatus = unsafe { std::mem::zeroed() };
        unsafe { rs_call_get_status(&mut s) };
        s
    }

    fn state_is(state: State) -> impl Fn() -> bool {
        move || status().state == state as u32
    }

    fn sent(what: &'static str) -> impl Fn() -> bool {
        move || fake::sent().iter().any(|s| s == what)
    }

    #[test]
    fn a_call_placed_and_hung_up_through_the_thread() {
        fresh();
        VOLUME_SENT.store(u16::MAX, Ordering::Relaxed);
        assert_eq!(dial_with(&fake::MODEM, "+491701"), ESP_OK);
        assert_eq!(status().state, State::Dialing as u32);
        assert!(busy() && rs_call_in_progress());
        assert_eq!(dial_with(&fake::MODEM, "+491702"), ESP_ERR_INVALID_STATE);
        wait_for(sent("ATD+491701;"), "the dial");
        // Nothing for the speaker or the volume before the call connects.
        assert_eq!(fake::sent(), ["ATD+491701;"]);

        fake::set(&[fake::call(1, false, STAT_ALERTING, "+491701")]);
        wait_for(state_is(State::Alerting), "ALERTING");
        fake::set(&[fake::call(1, false, STAT_ACTIVE, "+491701")]);
        wait_for(state_is(State::Active), "ACTIVE");
        wait_for(sent("volume 70"), "the volume");
        assert!(fake::sent().contains(&"AT+CSDVC=3".to_string()), "speaker on by default");
        assert!(fake::sent().contains(&"AT+CMUT=0".to_string()));

        assert_eq!(rs_call_set_speaker(false), ESP_OK);
        wait_for(sent("AT+CSDVC=1"), "the speaker off");
        assert_eq!(rs_call_dtmf(b'5' as c_char), ESP_OK);
        wait_for(sent("AT+VTS=5"), "the tone");
        assert_eq!(rs_call_dtmf(b'x' as c_char), ESP_ERR_INVALID_ARG);
        assert_eq!(rs_call_set_volume(70), ESP_OK);
        assert_eq!(rs_call_set_volume(101), ESP_ERR_INVALID_ARG);

        assert_eq!(rs_call_hangup(), ESP_OK);
        wait_for(sent("AT+CHUP"), "the hang-up");
        wait_for(state_is(State::Ended), "ENDED");
        assert_eq!(status().end, End::Local as u32);
        finished();
        assert_eq!(fake::sent().iter().filter(|s| s.starts_with("volume")).count(), 1, "sent once");
        assert_eq!(rs_call_hangup(), ESP_ERR_INVALID_STATE);
        assert_eq!(rs_call_answer(), ESP_ERR_INVALID_STATE);

        let mut e = unsafe { std::mem::zeroed::<call_log::CCallLogEntry>() };
        assert!(unsafe { call_log::rs_call_log_get(0, &mut e) });
        assert_eq!((e.kind, &e.number[..8]), (Kind::Outgoing as u8, &b"+491701\0"[..]));
    }

    #[test]
    fn a_ringing_call_starts_the_thread_and_is_answered() {
        fresh();
        // No modem that makes calls is registered: the watcher starts nothing.
        unsafe { watch(std::ptr::null_mut()) };
        assert!(!lock().thread);

        fake::set(&[fake::call(1, true, STAT_INCOMING, "+491702")]);
        fake::RINGING.store(true, Ordering::SeqCst);
        assert!(start_thread(&fake::MODEM));
        wait_for(state_is(State::Incoming), "INCOMING");
        let s = status();
        assert!(s.incoming);
        assert_eq!(&s.number[..8], b"+491702\0");
        assert_eq!(rs_call_dtmf(b'1' as c_char), ESP_ERR_INVALID_STATE, "not connected yet");

        assert_eq!(rs_call_answer(), ESP_OK);
        fake::RINGING.store(false, Ordering::SeqCst);
        wait_for(sent("ATA"), "ATA");
        wait_for(state_is(State::Active), "ACTIVE");
        fake::set(&[]);
        wait_for(state_is(State::Ended), "ENDED");
        assert_eq!(status().end, End::Remote as u32);
        finished();
    }

    #[test]
    fn a_call_that_stops_ringing_is_missed() {
        fresh();
        call_log::rs_call_missed_seen();
        fake::set(&[fake::call(1, true, STAT_INCOMING, "")]);
        fake::RINGING.store(true, Ordering::SeqCst);
        assert!(start_thread(&fake::MODEM));
        wait_for(state_is(State::Incoming), "INCOMING");
        rs_call_silence();
        fake::set(&[]);
        fake::RINGING.store(false, Ordering::SeqCst);
        wait_for(state_is(State::Ended), "ENDED");
        assert_eq!(status().end, End::Missed as u32);
        finished();
        assert_eq!(call_log::rs_call_missed_count(), 1);
    }

    #[test]
    fn a_dial_the_modem_refuses_fails_and_the_thread_waits_for_an_empty_list() {
        fresh();
        fake::DIAL_RET.store(-1, Ordering::SeqCst);
        // The modem answers badly at first: the driver keeps the line held.
        fake::LIST_RET.store(0x108, Ordering::SeqCst);
        assert_eq!(dial_with(&fake::MODEM, "+491705"), ESP_OK);
        wait_for(state_is(State::Ended), "ENDED");
        assert_eq!(status().end, End::Failed as u32);
        std::thread::sleep(ms(100));
        assert!(lock().thread, "still asking the modem");
        fake::LIST_RET.store(0, Ordering::SeqCst);
        finished();
    }

    #[test]
    fn a_modem_still_waking_is_asked_again_while_ri_rings() {
        fresh();
        fake::LIST_RET.store(ESP_ERR_INVALID_STATE, Ordering::SeqCst);
        fake::RINGING.store(true, Ordering::SeqCst);
        fake::set(&[fake::call(1, true, STAT_INCOMING, "+491706")]);
        assert!(start_thread(&fake::MODEM));
        std::thread::sleep(ms(100));
        assert!(lock().thread);
        fake::LIST_RET.store(0, Ordering::SeqCst);
        wait_for(state_is(State::Incoming), "INCOMING once the modem woke");
        assert_eq!(rs_call_hangup(), ESP_OK);
        fake::RINGING.store(false, Ordering::SeqCst);
        wait_for(state_is(State::Ended), "ENDED");
        assert_eq!(status().end, End::Rejected as u32);
        finished();
    }
}
