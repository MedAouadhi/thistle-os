// SPDX-License-Identifier: BSD-3-Clause
//! Sleep, deep sleep and the power key, carried out. What to do and when is
//! decided in power_policy.rs; this does it.
//!
//! One kernel worker job, every 50 ms, samples the power key, reads what the
//! rest of the system reports (the last input, holds, requests from Settings
//! or the power menu) and acts on the policy's answer. Light sleep is entered
//! from that job: while it lasts nothing else runs anyway, and when the CPU
//! comes back the job returns, so MeshCore's job can handle the packet that
//! woke it.
//!
//! Going to sleep happens in stages, so that nothing is cut off halfway:
//!
//! 1. The window manager is asked to show the sleep screen, and every task
//!    that draws or reads input stops at its next checkpoint (power_ui_*),
//!    after the e-paper has finished drawing it.
//! 2. The network rests on its own thread (net_coordinator::rest): WiFi
//!    stops, the modem ends its data session and sleeps listening.
//! 3. The drivers sleep: lights out, the keyboard left able to wake the
//!    device, the touch panel, GPS and IMU at their lowest power.
//! 4. The CPU sleeps, woken by the pins the board profile names.
//!
//! Waking undoes it in reverse. Deep sleep goes further (the radio sleeps,
//! the panel sleeps, the rails the profile lists are cut) and never returns.

use std::os::raw::{c_char, c_void};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU8, Ordering};
use std::sync::{Condvar, Mutex, MutexGuard, OnceLock, PoisonError};
use std::time::{Duration, Instant};

use crate::hal_registry::{registry, HalDisplayType, HalPowerInfo, HalPowerState};
use crate::net_coordinator::{self, Rest};
use crate::power_policy::{
    self, Action, Caps, Depth, Facts, Phase, Policy, PowerKey, Request, Settings, Woke,
};
use crate::system_json;

const ESP_OK: i32 = 0;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_NOT_SUPPORTED: i32 = 0x106;

/// How often the power key is sampled and the clock looked at.
const TICK: Duration = Duration::from_millis(50);
/// The longest light sleep without a look at the battery.
const MAX_SLICE: Duration = Duration::from_secs(30 * 60);
/// How long a reading of USB power, or of the battery, stands.
const USB_CHECK: Duration = Duration::from_secs(10);
const BATTERY_CHECK: Duration = Duration::from_secs(10 * 60);
/// At or below this charge, on battery, a sleeping device goes into deep
/// sleep rather than drain to nothing.
const BATTERY_CRITICAL_PERCENT: u8 = 3;
/// How long deep sleep waits for the modem to be powered down.
const NET_OFF_TIMEOUT: Duration = Duration::from_secs(15);

// power_ui_request_t
const UI_NONE: u8 = 0;
const UI_SHOW_SLEEP_SCREEN: u8 = 1;
const UI_HIDE_SLEEP_SCREEN: u8 = 2;
const UI_SHOW_MENU: u8 = 3;
const UI_SHOW_HELD_OFF: u8 = 4;

// power_request_t
const REQUEST_SLEEP: u32 = 0;
const REQUEST_DEEP_SLEEP: u32 = 1;
const REQUEST_RESTART: u32 = 2;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

#[cfg(not(test))]
static TAG: &[u8] = b"power\0";

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
// The board
// ---------------------------------------------------------------------------

/// A pin, as the platform layer takes it. Mirrors power_pin_t in
/// components/kernel/src/power_platform.c.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct WakePin {
    gpio: i32,
    active_high: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Source {
    /// Someone at the device: a keyboard, a button.
    User,
    /// The modem's RI line.
    Modem,
    /// A LoRa radio's receive interrupt.
    Radio,
}

/// The profile's `power` section.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct Board {
    key: Option<WakePin>,
    wake: Vec<(Source, WakePin)>,
    /// Expander outputs deep sleep drives low: (expander, pin).
    deep_sleep_off: Vec<(usize, u8)>,
}

fn pin_in(v: &serde_json::Value) -> Option<WakePin> {
    let gpio = v.get("gpio")?.as_i64()?;
    if !(0..64).contains(&gpio) {
        return None;
    }
    let active_high = match v.get("active_level").map_or(Some(0), |l| l.as_u64())? {
        0 => false,
        1 => true,
        _ => return None,
    };
    Some(WakePin {
        gpio: gpio as i32,
        active_high,
    })
}

fn source_in(v: &serde_json::Value) -> Option<Source> {
    match v.get("source")?.as_str()? {
        "keyboard" | "button" | "touch" => Some(Source::User),
        "modem" => Some(Source::Modem),
        "radio" => Some(Source::Radio),
        _ => None,
    }
}

/// Read a profile's `power` section. An entry that does not make sense is
/// left out rather than guessed at.
pub(crate) fn board_in(section: Option<&serde_json::Value>) -> Board {
    let Some(section) = section else {
        return Board::default();
    };
    let wake = section
        .get("wake")
        .and_then(|w| w.as_array())
        .map(|entries| {
            entries
                .iter()
                .filter_map(|e| Some((source_in(e)?, pin_in(e)?)))
                .take(8)
                .collect()
        })
        .unwrap_or_default();
    let deep_sleep_off = section
        .get("deep_sleep_off")
        .and_then(|d| d.as_array())
        .map(|entries| {
            entries
                .iter()
                .filter_map(|e| {
                    let expander = e.get("expander")?.as_u64()? as usize;
                    let pin = e.get("pin")?.as_u64()?;
                    (pin < 16).then_some((expander, pin as u8))
                })
                .collect()
        })
        .unwrap_or_default();
    Board {
        key: section.get("key").and_then(pin_in),
        wake,
        deep_sleep_off,
    }
}

impl Board {
    fn pins(&self, source: Source) -> impl Iterator<Item = WakePin> + '_ {
        self.wake.iter().filter(move |(s, _)| *s == source).map(|(_, p)| *p)
    }

    /// Every pin the profile names, for handing back after deep sleep.
    fn all_pins(&self) -> Vec<WakePin> {
        self.key.iter().copied().chain(self.wake.iter().map(|(_, p)| *p)).collect()
    }

    /// The pins someone at the device wakes it with: the power key and the
    /// keyboard.
    fn user_pins(&self) -> Vec<WakePin> {
        self.key.iter().copied().chain(self.pins(Source::User)).collect()
    }

    /// The pins that can wake the device from deep sleep, of `from`.
    fn deep_pins(&self, from: impl Iterator<Item = WakePin>) -> Vec<WakePin> {
        from.filter(|p| !p.active_high && platform::can_wake_deep(p.gpio)).collect()
    }

    fn caps(&self) -> Caps {
        Caps {
            sleep: !self.user_pins().is_empty(),
            deep_sleep: !self.deep_pins(self.user_pins().into_iter()).is_empty(),
            menu: MENU_AVAILABLE.load(Ordering::Relaxed),
        }
    }
}

static BOARD: Mutex<Option<Board>> = Mutex::new(None);

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(PoisonError::into_inner)
}

/// The board profile's `power` section, from board_config at boot, before
/// any driver starts. Pins that woke the board from deep sleep are still
/// routed to the RTC domain then, and are handed back here.
pub(crate) fn declare_board(section: Option<&serde_json::Value>) {
    let board = board_in(section);
    platform::release_pins(&board.all_pins());
    *lock(&BOARD) = Some(board);
}

fn board() -> Board {
    lock(&BOARD).clone().unwrap_or_default()
}

// ---------------------------------------------------------------------------
// The SoC side (components/kernel/src/power_platform.c)
// ---------------------------------------------------------------------------

#[cfg(target_os = "espidf")]
mod platform {
    use super::WakePin;
    use std::time::Duration;

    #[repr(C)]
    #[derive(Default)]
    struct CWake {
        active: u32,
        timer: bool,
    }

    extern "C" {
        fn power_platform_light_sleep(
            pins: *const WakePin,
            count: usize,
            timeout_us: u64,
            out: *mut CWake,
        ) -> i32;
        fn power_platform_deep_sleep(pins: *const WakePin, count: usize) -> i32;
        fn power_platform_boot_wake(pins: *const WakePin, count: usize) -> u32;
        fn power_platform_release_pins(pins: *const WakePin, count: usize);
        fn power_platform_can_wake_deep(gpio: i32) -> bool;
        fn power_platform_setup_input(gpio: i32, active_high: bool) -> i32;
        fn power_platform_pin_active(gpio: i32, active_high: bool) -> bool;
        fn power_platform_restart();
    }

    /// Halt the CPU until a pin reaches its level or `timeout` passes. Returns
    /// the pins at their level on waking, by index, and whether the timer
    /// woke it.
    pub(super) fn light_sleep(pins: &[WakePin], timeout: Duration) -> Result<(u32, bool), i32> {
        let mut out = CWake::default();
        let us = timeout.as_micros().min(u64::MAX as u128) as u64;
        let ret = unsafe { power_platform_light_sleep(pins.as_ptr(), pins.len(), us, &mut out) };
        if ret == 0 {
            Ok((out.active, out.timer))
        } else {
            Err(ret)
        }
    }

    /// Returns only if deep sleep could not be entered.
    pub(super) fn deep_sleep(pins: &[WakePin]) -> i32 {
        unsafe { power_platform_deep_sleep(pins.as_ptr(), pins.len()) }
    }

    pub(super) fn boot_wake(pins: &[WakePin]) -> u32 {
        unsafe { power_platform_boot_wake(pins.as_ptr(), pins.len()) }
    }

    pub(super) fn release_pins(pins: &[WakePin]) {
        unsafe { power_platform_release_pins(pins.as_ptr(), pins.len()) }
    }

    pub(super) fn can_wake_deep(gpio: i32) -> bool {
        unsafe { power_platform_can_wake_deep(gpio) }
    }

    pub(super) fn setup_input(pin: WakePin) {
        unsafe { power_platform_setup_input(pin.gpio, pin.active_high) };
    }

    pub(super) fn active(pin: WakePin) -> bool {
        unsafe { power_platform_pin_active(pin.gpio, pin.active_high) }
    }

    pub(super) fn restart() {
        unsafe { power_platform_restart() }
    }
}

/// Off the device nothing sleeps: the simulators' boards declare no wake
/// pins, so the policy never asks.
#[cfg(not(target_os = "espidf"))]
mod platform {
    use super::WakePin;
    use std::time::Duration;

    pub(super) fn light_sleep(_pins: &[WakePin], timeout: Duration) -> Result<(u32, bool), i32> {
        std::thread::sleep(timeout.min(Duration::from_millis(100)));
        Ok((0, true))
    }
    pub(super) fn deep_sleep(_pins: &[WakePin]) -> i32 {
        super::ESP_ERR_NOT_SUPPORTED
    }
    pub(super) fn boot_wake(_pins: &[WakePin]) -> u32 {
        0
    }
    pub(super) fn release_pins(_pins: &[WakePin]) {}
    pub(super) fn can_wake_deep(_gpio: i32) -> bool {
        true
    }
    pub(super) fn setup_input(_pin: WakePin) {}
    pub(super) fn active(_pin: WakePin) -> bool {
        false
    }
    pub(super) fn restart() {
        std::process::exit(0);
    }
}

// ---------------------------------------------------------------------------
// What the rest of the system reports
// ---------------------------------------------------------------------------

static START: OnceLock<Instant> = OnceLock::new();
/// When someone last used the device, in tenths of a second from START: the
/// S3 has no 64-bit atomics, and this lasts thirteen years of uptime.
static LAST_ACTIVITY: AtomicU32 = AtomicU32::new(0);
/// A power_request_t + 1, or 0 for none.
static REQUEST: AtomicU8 = AtomicU8::new(0);
static MENU_AVAILABLE: AtomicBool = AtomicBool::new(false);
static UI_REQUEST: AtomicU8 = AtomicU8::new(UI_NONE);

fn start() -> Instant {
    *START.get_or_init(Instant::now)
}

fn note_activity_at(t: Instant) {
    let tenths = t.saturating_duration_since(start()).as_millis() / 100;
    LAST_ACTIVITY.fetch_max(tenths.min(u32::MAX as u128) as u32, Ordering::Relaxed);
}

fn last_activity() -> Instant {
    start() + Duration::from_millis(LAST_ACTIVITY.load(Ordering::Relaxed) as u64 * 100)
}

fn ui_request(request: u8) {
    UI_REQUEST.store(request, Ordering::SeqCst);
}

// ── Holds ───────────────────────────────────────────────────────────────

struct Hold {
    token: u32,
    /// The caller's `why`, a string literal, kept as an integer so the list
    /// is Send.
    why: usize,
}

static HOLDS: Mutex<Vec<Hold>> = Mutex::new(Vec::new());
static NEXT_HOLD: AtomicU32 = AtomicU32::new(1);

/// What holds the device awake, if anything does: the first hold taken.
fn held_by() -> Option<*const c_char> {
    lock(&HOLDS).first().map(|h| h.why as *const c_char)
}

/// A call ringing holds the modem's RI line low.
fn ringing() -> bool {
    board().pins(Source::Modem).any(platform::active)
}

/// What keeps the device awake, in words, for the window manager to say.
fn awake_because() -> Option<*const c_char> {
    held_by()
        .or_else(|| ringing().then_some(b"a call is ringing\0".as_ptr() as *const c_char))
        .or_else(|| {
            crate::ble_manager::running().then_some(b"Bluetooth is on\0".as_ptr() as *const c_char)
        })
}

// ── The window managers' checkpoints ────────────────────────────────────

#[derive(Default)]
struct Gate {
    /// Tasks that joined.
    members: u32,
    /// Tasks stopped at their checkpoint now.
    parked: u32,
    park: bool,
}

static GATE: Mutex<Gate> = Mutex::new(Gate {
    members: 0,
    parked: 0,
    park: false,
});
static GATE_MOVED: Condvar = Condvar::new();

fn gate_park(park: bool) {
    lock(&GATE).park = park;
    GATE_MOVED.notify_all();
}

fn gate_all_parked() -> bool {
    let g = lock(&GATE);
    g.park && g.parked >= g.members
}

// ---------------------------------------------------------------------------
// The job
// ---------------------------------------------------------------------------

struct Runtime {
    policy: Policy,
    board: Board,
    key: PowerKey,
    /// The rest asked of the network for the sleep under way.
    rest: Rest,
    /// The screen's light and the keys' are out, from the moment sleep began.
    dark: bool,
    drivers_asleep: bool,
    usb: Option<(Instant, bool)>,
    battery: Option<(Instant, bool)>,
}

static RUNTIME: Mutex<Option<Runtime>> = Mutex::new(None);

fn power_info() -> Option<HalPowerInfo> {
    let reg = registry();
    if reg.power.is_null() {
        return None;
    }
    let get = unsafe { (*reg.power).get_info }?;
    let mut info = HalPowerInfo {
        voltage_mv: 0,
        percent: 0,
        state: HalPowerState::NoBattery,
    };
    (unsafe { get(&mut info) } == ESP_OK).then_some(info)
}

impl Runtime {
    fn any_active(&self, pins: &[WakePin]) -> bool {
        pins.iter().any(|p| platform::active(*p))
    }

    /// The charger reports charging or charged only with a cable in: the
    /// nearest the HAL comes to saying the board is on USB power.
    fn on_usb(&mut self, now: Instant) -> bool {
        if let Some((at, on)) = self.usb {
            if now.duration_since(at) < USB_CHECK {
                return on;
            }
        }
        let on = power_info()
            .is_some_and(|i| matches!(i.state, HalPowerState::Charging | HalPowerState::Charged));
        self.usb = Some((now, on));
        on
    }

    fn battery_critical(&mut self, now: Instant) -> bool {
        if let Some((at, critical)) = self.battery {
            if now.duration_since(at) < BATTERY_CHECK {
                return critical;
            }
        }
        let critical = power_info().is_some_and(|i| {
            i.state == HalPowerState::Discharging
                && i.voltage_mv > 0
                && i.percent <= BATTERY_CRITICAL_PERCENT
        });
        self.battery = Some((now, critical));
        critical
    }

    /// Something keeps the device awake: a hold, a call ringing, which holds
    /// RI low, or Bluetooth, which nothing stops again once started.
    fn held(&self) -> bool {
        awake_because().is_some()
    }

    fn tick(&mut self) {
        let now = Instant::now();
        let key_down = self.board.key.is_some_and(platform::active);
        let key = self.key.sample(key_down, now);

        // A restart is not queued: power_manager_request() does it at once.
        let request = match REQUEST.swap(0, Ordering::SeqCst).checked_sub(1).map(u32::from) {
            Some(REQUEST_SLEEP) => Some(Request::Sleep),
            Some(REQUEST_DEEP_SLEEP) => Some(Request::DeepSleep),
            _ => None,
        };

        let phase = self.policy.phase();
        let asleep = matches!(phase, Phase::Asleep { .. });
        let last = last_activity();
        // The charger is read only when the clock says sleep is due: it is
        // an I2C exchange, and the status bar reads it too.
        let on_usb = phase == Phase::Awake
            && self.policy.settings.stay_awake_on_usb
            && self.policy.idle_due(now, last)
            && self.on_usb(now);
        let facts = Facts {
            now,
            last_activity: last,
            key,
            request,
            held: phase == Phase::Awake && self.held(),
            on_usb,
            settled: matches!(phase, Phase::Settling { .. })
                && gate_all_parked()
                && net_coordinator::rested(self.rest),
            user_waiting: asleep && (key_down || self.any_active(&self.board.user_pins())),
            battery_critical: asleep && self.battery_critical(now),
            radio_busy: asleep && crate::mesh_manager::outbound_pending(),
        };
        let action = self.policy.decide(&facts);
        self.act(action, now);
    }

    fn act(&mut self, action: Action, now: Instant) {
        match action {
            Action::Nothing => {}
            Action::OpenMenu => ui_request(UI_SHOW_MENU),
            Action::HeldOff => {
                log!(ESP_LOG_INFO, b"not sleeping: something holds the device awake\0");
                ui_request(UI_SHOW_HELD_OFF);
            }
            Action::Settle(depth) => self.settle(depth),
            Action::Resume => {
                log!(ESP_LOG_INFO, b"sleep called off\0");
                self.come_back();
            }
            Action::FallAsleep => self.fall_asleep(),
            Action::LightSleep { until } => self.light_sleep(until, now),
            Action::DeepSleep => self.deep_sleep(),
            Action::Wake => {
                log!(ESP_LOG_INFO, b"awake\0");
                self.come_back();
            }
        }
    }

    fn settle(&mut self, depth: Depth) {
        self.rest = match depth {
            Depth::Light => Rest::Sleep,
            Depth::Deep if self.policy.settings.wake_on_modem => Rest::Sleep,
            Depth::Deep => Rest::Off,
        };
        log!(
            ESP_LOG_INFO,
            b"going to %s\0",
            if depth == Depth::Deep {
                b"deep sleep\0".as_ptr()
            } else {
                b"sleep\0".as_ptr()
            }
        );
        // The lights go out with the sleep screen, not once settling is over:
        // the modem alone takes about 8 s to leave a data session.
        lights_out();
        self.dark = true;
        ui_request(UI_SHOW_SLEEP_SCREEN);
        gate_park(true);
        net_coordinator::rest(self.rest);
    }

    fn fall_asleep(&mut self) {
        if !gate_all_parked() || !net_coordinator::rested(self.rest) {
            log!(ESP_LOG_WARN, b"the screen or the network did not settle; sleeping anyway\0");
        }
        drivers_sleep();
        self.drivers_asleep = true;
        log!(ESP_LOG_INFO, b"asleep\0");
    }

    /// Undo going to sleep, from wherever it got to.
    fn come_back(&mut self) {
        if self.drivers_asleep {
            drivers_wake();
            self.drivers_asleep = false;
        }
        // After the drivers: an LCD has to be awake to be lit.
        if self.dark {
            crate::light_prefs::restore_after_sleep();
            self.dark = false;
        }
        self.rest = Rest::Normal;
        net_coordinator::rest(Rest::Normal);
        ui_request(UI_HIDE_SLEEP_SCREEN);
        gate_park(false);
    }

    fn woke(&mut self, why: Woke, key_down: bool, now: Instant) {
        self.key.woke(key_down, now);
        if why == Woke::User {
            note_activity_at(now);
        }
        let action = self.policy.woke(why, now);
        self.act(action, now);
    }

    fn light_sleep(&mut self, until: Option<Instant>, now: Instant) {
        let user = self.board.user_pins();
        let modem: Vec<WakePin> = self.board.pins(Source::Modem).collect();
        // Something is already waiting: no sleep would last.
        if self.any_active(&user) {
            let key_down = self.board.key.is_some_and(platform::active);
            return self.woke(Woke::User, key_down, now);
        }
        if self.any_active(&modem) {
            return self.woke(Woke::Modem, false, now);
        }
        // A radio interrupt still pending would end the sleep at once; the
        // packet is being read, and the next sleep can wait for the next one.
        let radio: Vec<WakePin> = self
            .board
            .pins(Source::Radio)
            .filter(|p| !platform::active(*p))
            .collect();
        let pins: Vec<WakePin> = user.iter().chain(&modem).chain(&radio).copied().collect();

        let slice = until
            .map_or(MAX_SLICE, |u| u.saturating_duration_since(now))
            .min(MAX_SLICE);
        if slice.is_zero() {
            return;
        }

        let (active, timer) = match platform::light_sleep(&pins, slice) {
            Ok(woke) => woke,
            Err(ret) => {
                log!(ESP_LOG_WARN, b"light sleep refused (0x%x)\0", ret);
                // Not a reason to spin: wait for the next tick.
                (0, false)
            }
        };

        let now = Instant::now();
        let woke_by = |from: &[WakePin], offset: usize| {
            (0..from.len()).any(|i| active & (1 << (offset + i)) != 0)
        };
        let why = if woke_by(&user, 0) {
            Woke::User
        } else if woke_by(&modem, user.len()) {
            Woke::Modem
        } else if woke_by(&radio, user.len() + modem.len()) {
            Woke::Radio
        } else if timer {
            Woke::Timer
        } else {
            Woke::Other
        };
        match why {
            Woke::User => log!(ESP_LOG_INFO, b"woken by a key\0"),
            Woke::Modem => log!(ESP_LOG_INFO, b"woken by the modem\0"),
            _ => {}
        }
        let key_down = self.board.key.is_some_and(platform::active);
        self.woke(why, key_down, now);
    }

    fn deep_sleep(&mut self) {
        let wake_on_modem = self.policy.settings.wake_on_modem;
        // From a light sleep the modem may be listening, and it should not be.
        let rest = if wake_on_modem { Rest::Sleep } else { Rest::Off };
        if self.rest != rest {
            self.rest = rest;
            net_coordinator::rest(rest);
        }
        let asked = Instant::now();
        while !net_coordinator::rested(rest) && asked.elapsed() < NET_OFF_TIMEOUT {
            std::thread::sleep(Duration::from_millis(100));
        }

        if !self.dark {
            lights_out();
            self.dark = true;
        }
        if !self.drivers_asleep {
            drivers_sleep();
            self.drivers_asleep = true;
        }
        drivers_deep_sleep();
        for &(expander, pin) in &self.board.deep_sleep_off {
            crate::drv_ioexp_xl9555::set_as_output(expander, pin);
            crate::drv_ioexp_xl9555::set_level(expander, pin, false);
        }

        let user = self.board.user_pins();
        let mut pins = self.board.deep_pins(user.into_iter());
        let modem_listening = wake_on_modem && net_coordinator::modem_listening();
        if modem_listening {
            pins.extend(self.board.deep_pins(self.board.pins(Source::Modem)));
        }
        log!(
            ESP_LOG_INFO,
            b"deep sleep; the modem %s\0",
            if modem_listening {
                b"listens\0".as_ptr()
            } else {
                b"is off\0".as_ptr()
            }
        );
        let ret = platform::deep_sleep(&pins);
        // Still here: the rails are already down, and only a restart brings
        // them and the drivers back as the profile has them.
        log!(ESP_LOG_WARN, b"deep sleep refused (0x%x); restarting\0", ret);
        platform::restart();
    }
}

/// Put out the screen's light and the keys'. light_prefs puts them back.
fn lights_out() {
    let reg = registry();
    for i in 0..(reg.input_count as usize) {
        if let Some(set) = unsafe { reg.inputs[i].as_ref() }.and_then(|d| d.set_backlight) {
            unsafe { set(0) };
        }
    }
    if let Some(set) = unsafe { reg.display.as_ref() }.and_then(|d| d.set_brightness) {
        unsafe { set(0) };
    }
}

fn drivers_sleep() {
    let reg = registry();
    for i in 0..(reg.input_count as usize) {
        if let Some(sleep) = unsafe { reg.inputs[i].as_ref() }.and_then(|d| d.sleep) {
            unsafe { sleep(true) };
        }
    }
    if let Some(display) = unsafe { reg.display.as_ref() } {
        unsafe {
            // E-paper keeps its picture with no power, and it powers itself
            // down after every refresh; an LCD has to be put out.
            if display.display_type == HalDisplayType::Lcd {
                if let Some(sleep) = display.sleep {
                    sleep(true);
                }
            }
        }
    }
    unsafe {
        if let Some(sleep) = reg.gps.as_ref().and_then(|g| g.sleep) {
            sleep(true);
        }
        if let Some(sleep) = reg.imu.as_ref().and_then(|i| i.sleep) {
            sleep(true);
        }
    }
}

fn drivers_wake() {
    let reg = registry();
    unsafe {
        if let Some(sleep) = reg.imu.as_ref().and_then(|i| i.sleep) {
            sleep(false);
        }
        if let Some(sleep) = reg.gps.as_ref().and_then(|g| g.sleep) {
            sleep(false);
        }
        if let Some(display) = reg.display.as_ref() {
            if display.display_type == HalDisplayType::Lcd {
                if let Some(sleep) = display.sleep {
                    sleep(false);
                }
            }
        }
    }
    for i in 0..(reg.input_count as usize) {
        if let Some(sleep) = unsafe { reg.inputs[i].as_ref() }.and_then(|d| d.sleep) {
            unsafe { sleep(false) };
        }
    }
}

/// What only deep sleep turns off: the radio, which light sleep leaves
/// listening, and the e-paper panel's controller.
fn drivers_deep_sleep() {
    let reg = registry();
    unsafe {
        if let Some(radio) = reg.radio.as_ref() {
            if let Some(stop) = radio.stop_receive {
                stop();
            }
            if let Some(sleep) = radio.sleep {
                sleep(true);
            }
        }
        if let Some(sleep) = reg.display.as_ref().and_then(|d| d.sleep) {
            sleep(true);
        }
    }
}

unsafe extern "C" fn tick(_arg: *mut c_void) {
    if let Some(rt) = lock(&RUNTIME).as_mut() {
        rt.tick();
    }
}

fn settings_from_file() -> Settings {
    power_policy::settings_in(std::fs::read_to_string(system_json::path()).ok().as_deref())
}

// ---------------------------------------------------------------------------
// FFI — thistle/power_manager.h
// ---------------------------------------------------------------------------

/// power_settings_t
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct CPowerSettings {
    pub sleep_after_s: u32,
    pub deep_sleep_after_s: u32,
    pub wake_on_modem: bool,
    pub stay_awake_on_usb: bool,
}

/// power_caps_t
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct CPowerCaps {
    pub has_key: bool,
    pub can_sleep: bool,
    pub can_deep_sleep: bool,
    pub can_wake_on_modem: bool,
}

#[no_mangle]
pub extern "C" fn power_manager_start() {
    if lock(&RUNTIME).is_some() {
        return;
    }
    let now = Instant::now();
    note_activity_at(now);

    let board = board();
    if let Some(key) = board.key {
        platform::setup_input(key);
    }
    for pin in board.pins(Source::Modem) {
        platform::setup_input(pin);
    }

    let settings = settings_from_file();
    let caps = board.caps();
    let mut policy = Policy::new(settings, caps);

    let pins = board.all_pins();
    let woke = platform::boot_wake(&pins);
    if woke != 0 {
        let by_user = board
            .user_pins()
            .iter()
            .any(|p| pins.iter().position(|q| q == p).is_some_and(|i| woke & (1 << i) != 0));
        if by_user {
            log!(ESP_LOG_INFO, b"woken from deep sleep by a key\0");
        } else {
            log!(ESP_LOG_INFO, b"woken from deep sleep by the modem\0");
            policy.booted_by_modem(now);
        }
    }

    if !caps.sleep {
        log!(ESP_LOG_INFO, b"no wake source on this board; it does not sleep\0");
    } else {
        log!(
            ESP_LOG_INFO,
            b"sleep after %us, deep sleep after %us (0: never)\0",
            power_policy::seconds(settings.sleep_after) as u32,
            power_policy::seconds(settings.deep_sleep_after) as u32
        );
    }

    *lock(&RUNTIME) = Some(Runtime {
        policy,
        board,
        key: PowerKey::default(),
        rest: Rest::Normal,
        dark: false,
        drivers_asleep: false,
        usb: None,
        battery: None,
    });
    if caps.sleep {
        crate::kworker::every(tick, std::ptr::null_mut(), TICK);
    }
}

/// # Safety
/// `out` must be valid for writes.
#[no_mangle]
pub unsafe extern "C" fn power_manager_get_settings(out: *mut CPowerSettings) -> i32 {
    if out.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let s = lock(&RUNTIME)
        .as_ref()
        .map(|rt| rt.policy.settings)
        .unwrap_or_else(settings_from_file);
    *out = CPowerSettings {
        sleep_after_s: power_policy::seconds(s.sleep_after) as u32,
        deep_sleep_after_s: power_policy::seconds(s.deep_sleep_after) as u32,
        wake_on_modem: s.wake_on_modem,
        stay_awake_on_usb: s.stay_awake_on_usb,
    };
    ESP_OK
}

/// # Safety
/// `out` must be valid for writes.
#[no_mangle]
pub unsafe extern "C" fn power_manager_get_caps(out: *mut CPowerCaps) {
    if out.is_null() {
        return;
    }
    let board = board();
    let caps = board.caps();
    *out = CPowerCaps {
        has_key: board.key.is_some(),
        can_sleep: caps.sleep,
        can_deep_sleep: caps.deep_sleep,
        can_wake_on_modem: caps.deep_sleep
            && !board.deep_pins(board.pins(Source::Modem)).is_empty(),
    };
}

/// Save one setting, and apply it to the running policy.
fn save(key: &str, value: serde_json::Value, apply: impl FnOnce(&mut Settings)) -> i32 {
    if let Some(rt) = lock(&RUNTIME).as_mut() {
        apply(&mut rt.policy.settings);
    }
    system_json::save(power_policy::SECTION, key, value)
}

#[no_mangle]
pub extern "C" fn power_manager_set_sleep_after(seconds: u32) -> i32 {
    let saved = power_policy::settings_in(Some(&format!(
        r#"{{"power": {{"{}": {seconds}}}}}"#,
        power_policy::KEY_SLEEP_AFTER
    )));
    save(power_policy::KEY_SLEEP_AFTER, seconds.into(), |s| {
        s.sleep_after = saved.sleep_after
    })
}

#[no_mangle]
pub extern "C" fn power_manager_set_deep_sleep_after(seconds: u32) -> i32 {
    let saved = power_policy::settings_in(Some(&format!(
        r#"{{"power": {{"{}": {seconds}}}}}"#,
        power_policy::KEY_DEEP_SLEEP_AFTER
    )));
    save(power_policy::KEY_DEEP_SLEEP_AFTER, seconds.into(), |s| {
        s.deep_sleep_after = saved.deep_sleep_after
    })
}

#[no_mangle]
pub extern "C" fn power_manager_set_wake_on_modem(on: bool) -> i32 {
    save(power_policy::KEY_WAKE_ON_MODEM, on.into(), |s| s.wake_on_modem = on)
}

#[no_mangle]
pub extern "C" fn power_manager_set_stay_awake_on_usb(on: bool) -> i32 {
    save(power_policy::KEY_STAY_AWAKE_ON_USB, on.into(), |s| s.stay_awake_on_usb = on)
}

#[no_mangle]
pub extern "C" fn power_manager_request(request: u32) -> i32 {
    match request {
        REQUEST_RESTART => {
            // Nothing to settle first: a restart is a restart.
            log!(ESP_LOG_INFO, b"restarting\0");
            platform::restart();
            ESP_OK
        }
        REQUEST_SLEEP | REQUEST_DEEP_SLEEP => {
            let caps = board().caps();
            let can = if request == REQUEST_SLEEP {
                caps.sleep
            } else {
                caps.deep_sleep
            };
            if !can || lock(&RUNTIME).is_none() {
                return ESP_ERR_NOT_SUPPORTED;
            }
            REQUEST.store(request as u8 + 1, Ordering::SeqCst);
            ESP_OK
        }
        _ => ESP_ERR_INVALID_ARG,
    }
}

#[no_mangle]
pub extern "C" fn power_manager_note_activity() {
    note_activity_at(Instant::now());
}

#[no_mangle]
pub extern "C" fn power_manager_hold(why: *const c_char) -> u32 {
    let token = NEXT_HOLD.fetch_add(1, Ordering::Relaxed).max(1);
    lock(&HOLDS).push(Hold {
        token,
        why: why as usize,
    });
    token
}

#[no_mangle]
pub extern "C" fn power_manager_release(token: u32) {
    lock(&HOLDS).retain(|h| h.token != token);
}

/// What keeps the device awake, in words, for the window manager to say;
/// NULL if nothing does.
#[no_mangle]
pub extern "C" fn power_manager_held_by() -> *const c_char {
    awake_because().unwrap_or(std::ptr::null())
}

/// Keeps the device awake while it lives. For Rust callers.
pub(crate) struct HoldGuard(u32);

impl HoldGuard {
    pub(crate) fn new(why: &'static [u8]) -> Self {
        debug_assert!(why.ends_with(b"\0"));
        HoldGuard(power_manager_hold(why.as_ptr() as *const c_char))
    }
}

impl Drop for HoldGuard {
    fn drop(&mut self) {
        power_manager_release(self.0);
    }
}

#[no_mangle]
pub extern "C" fn power_ui_join() {
    lock(&GATE).members += 1;
}

#[no_mangle]
pub extern "C" fn power_ui_checkpoint(ready: bool) {
    let mut g = lock(&GATE);
    if !g.park || !ready {
        return;
    }
    g.parked += 1;
    GATE_MOVED.notify_all();
    while g.park {
        g = GATE_MOVED.wait(g).unwrap_or_else(PoisonError::into_inner);
    }
    g.parked -= 1;
}

#[no_mangle]
pub extern "C" fn power_ui_take_request() -> u32 {
    UI_REQUEST.swap(UI_NONE, Ordering::SeqCst) as u32
}

#[no_mangle]
pub extern "C" fn power_ui_set_menu_available(available: bool) {
    MENU_AVAILABLE.store(available, Ordering::Relaxed);
    if let Some(rt) = lock(&RUNTIME).as_mut() {
        rt.policy.caps.menu = available;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn section(json: &str) -> serde_json::Value {
        serde_json::from_str(json).unwrap()
    }

    const MAX: &str = r#"{
        "key": {"gpio": 0, "active_level": 0},
        "wake": [
            {"source": "keyboard", "gpio": 15, "active_level": 0},
            {"source": "modem", "gpio": 7, "active_level": 0},
            {"source": "radio", "gpio": 5, "active_level": 1}
        ],
        "deep_sleep_off": [{"expander": 0, "pin": 1}, {"expander": 0, "pin": 2}]
    }"#;

    #[test]
    fn the_max_profile_reads_as_written() {
        let board = board_in(Some(&section(MAX)));
        assert_eq!(
            board.key,
            Some(WakePin {
                gpio: 0,
                active_high: false
            })
        );
        assert_eq!(board.user_pins().len(), 2);
        assert_eq!(board.pins(Source::Modem).count(), 1);
        assert_eq!(
            board.pins(Source::Radio).next(),
            Some(WakePin {
                gpio: 5,
                active_high: true
            })
        );
        assert_eq!(board.deep_sleep_off, vec![(0, 1), (0, 2)]);
    }

    #[test]
    fn a_board_with_no_power_section_cannot_sleep() {
        let board = board_in(None);
        assert_eq!(board, Board::default());
        assert!(!board.caps().sleep);
        assert!(!board.caps().deep_sleep);
    }

    #[test]
    fn a_radio_alone_does_not_make_a_board_able_to_sleep() {
        // Nobody could wake it.
        let board = board_in(Some(&section(
            r#"{"wake": [{"source": "radio", "gpio": 5, "active_level": 1}]}"#,
        )));
        assert!(!board.caps().sleep);
    }

    #[test]
    fn only_active_low_pins_wake_from_deep_sleep() {
        let board = board_in(Some(&section(
            r#"{"wake": [{"source": "keyboard", "gpio": 15, "active_level": 1}]}"#,
        )));
        assert!(board.caps().sleep);
        assert!(!board.caps().deep_sleep);
    }

    #[test]
    fn nonsense_entries_are_left_out() {
        let board = board_in(Some(&section(
            r#"{"key": {"gpio": 99},
                "wake": [{"source": "kettle", "gpio": 4}, {"source": "modem", "gpio": 7, "active_level": 3}],
                "deep_sleep_off": [{"expander": 0, "pin": 20}]}"#,
        )));
        assert_eq!(board, Board::default());
    }

    #[test]
    fn a_checkpoint_does_not_stop_a_task_while_the_device_is_awake() {
        power_ui_checkpoint(true);
    }

    #[test]
    fn holds_are_released_by_token() {
        let a = power_manager_hold(b"a\0".as_ptr() as *const c_char);
        let b = power_manager_hold(b"b\0".as_ptr() as *const c_char);
        assert_ne!(a, b);
        power_manager_release(a);
        assert_eq!(held_by(), Some(b"b\0".as_ptr() as *const c_char));
        power_manager_release(b);
        assert_eq!(held_by(), None);
        {
            let _guard = HoldGuard::new(b"guard\0");
            assert!(held_by().is_some());
        }
        assert_eq!(held_by(), None);
    }
}
