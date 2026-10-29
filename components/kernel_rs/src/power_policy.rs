// SPDX-License-Identifier: BSD-3-Clause
//! When to sleep, and how deeply: the decisions behind the power manager,
//! kept apart from the hardware so a host can test them.
//!
//! The device is in one of three phases:
//!
//! - **Awake**: in use. It moves on when the power key or Settings asks for
//!   sleep, or when nobody has used it for the time set in Settings.
//! - **Settling**: on its way down. The screen is showing that the device is
//!   asleep, the window manager is stopping where it is safe to, and the
//!   network is resting. Someone using the device meanwhile calls it off.
//! - **Asleep**: the CPU is halted between wakes. A wake from someone at the
//!   device brings it back to Awake. A wake that brought news (the modem
//!   ringing in an SMS) brings it back too, but only for a short grace before
//!   it sleeps again. A LoRa packet keeps the CPU up just long enough for
//!   MeshCore to handle it, with the screen left as it is.
//!
//! Deep sleep is entered from any phase and is never left: waking from it
//! restarts the device. Powering off settles as deep sleep does, then cuts
//! the board off from its battery. The power menu asks for it, and so does
//! the deep sleep timer when `power_off_when_idle` is on and the power driver
//! can: from a light sleep it settles again first, so the screen says so.
//!
//! The settings live in system.json:
//!
//! ```json
//! "power": { "sleep_after_s": 300, "deep_sleep_after_s": 3600,
//!            "wake_on_modem": true, "stay_awake_on_usb": true,
//!            "power_off_when_idle": false }
//! ```
//!
//! A time of 0 is "never".

use std::time::{Duration, Instant};

use crate::system_json;

pub(crate) const SECTION: &str = "power";
pub(crate) const KEY_SLEEP_AFTER: &str = "sleep_after_s";
pub(crate) const KEY_DEEP_SLEEP_AFTER: &str = "deep_sleep_after_s";
pub(crate) const KEY_WAKE_ON_MODEM: &str = "wake_on_modem";
pub(crate) const KEY_STAY_AWAKE_ON_USB: &str = "stay_awake_on_usb";
pub(crate) const KEY_POWER_OFF_WHEN_IDLE: &str = "power_off_when_idle";

/// The shortest idle time accepted. Less would put the device to sleep
/// between two keystrokes of anyone who stops to think.
pub(crate) const MIN_SLEEP_AFTER: Duration = Duration::from_secs(15);
/// The shortest time to deep sleep accepted. Waking from it is a restart.
pub(crate) const MIN_DEEP_SLEEP_AFTER: Duration = Duration::from_secs(60);

/// Holding the power key this long opens the power menu instead of sleeping.
pub(crate) const LONG_PRESS: Duration = Duration::from_millis(1500);
/// A press shorter than this is contact bounce or noise, not a press.
pub(crate) const MIN_PRESS: Duration = Duration::from_millis(40);

/// After a wake nobody at the device asked for, it sleeps again this soon
/// unless someone uses it.
pub(crate) const UNASKED_WAKE_GRACE: Duration = Duration::from_secs(30);
/// How long a LoRa packet keeps the CPU up once it has woken it: MeshCore
/// reads it within one 10 ms pass, and answers an acknowledgement after a
/// short random delay.
pub(crate) const RADIO_DOZE: Duration = Duration::from_secs(2);
/// The longest the CPU stays up after a packet while MeshCore still has
/// something queued to send: a relay or retry that never empties the queue
/// must not keep the device awake.
pub(crate) const RADIO_DOZE_MAX: Duration = Duration::from_secs(10);
/// How long the screen and the network get to settle. Past this, sleep goes
/// ahead without them: a modem that never answers must not keep the device
/// awake all night.
pub(crate) const SETTLE_TIMEOUT: Duration = Duration::from_secs(15);

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Settings {
    /// None: never.
    pub(crate) sleep_after: Option<Duration>,
    /// None: never.
    pub(crate) deep_sleep_after: Option<Duration>,
    /// Keep the modem asleep but powered through deep sleep, so a call or an
    /// SMS can wake the device.
    pub(crate) wake_on_modem: bool,
    /// Do not sleep by the clock while on USB power. Light sleep turns off
    /// the S3's USB-Serial-JTAG, so a board left to sleep on a desk drops
    /// off the USB bus it is being flashed and watched over.
    pub(crate) stay_awake_on_usb: bool,
    /// When the deep sleep time comes, power off instead: nothing then
    /// wakes the device, not the modem, and the clock is lost.
    pub(crate) power_off_when_idle: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            sleep_after: Some(Duration::from_secs(5 * 60)),
            deep_sleep_after: Some(Duration::from_secs(60 * 60)),
            wake_on_modem: true,
            stay_awake_on_usb: true,
            power_off_when_idle: false,
        }
    }
}

/// `key` read as a time in seconds, with 0 for never. A missing or unreadable
/// value is the default; one below `min` is raised to it.
fn duration_in(
    json: &str,
    key: &str,
    default: Option<Duration>,
    min: Duration,
) -> Option<Duration> {
    match system_json::get(json, SECTION, key).and_then(|v| v.as_u64()) {
        Some(0) => None,
        Some(secs) => Some(Duration::from_secs(secs).max(min)),
        None => default,
    }
}

fn bool_in(json: &str, key: &str, default: bool) -> bool {
    system_json::get(json, SECTION, key)
        .and_then(|v| v.as_bool())
        .unwrap_or(default)
}

/// The settings saved in `json`, each one that is missing taking its default.
pub(crate) fn settings_in(json: Option<&str>) -> Settings {
    let d = Settings::default();
    let Some(json) = json else {
        return d;
    };
    Settings {
        sleep_after: duration_in(json, KEY_SLEEP_AFTER, d.sleep_after, MIN_SLEEP_AFTER),
        deep_sleep_after: duration_in(
            json,
            KEY_DEEP_SLEEP_AFTER,
            d.deep_sleep_after,
            MIN_DEEP_SLEEP_AFTER,
        ),
        wake_on_modem: bool_in(json, KEY_WAKE_ON_MODEM, d.wake_on_modem),
        stay_awake_on_usb: bool_in(json, KEY_STAY_AWAKE_ON_USB, d.stay_awake_on_usb),
        power_off_when_idle: bool_in(json, KEY_POWER_OFF_WHEN_IDLE, d.power_off_when_idle),
    }
}

/// A time as it is saved: whole seconds, 0 for never.
pub(crate) fn seconds(d: Option<Duration>) -> u64 {
    d.map_or(0, |d| d.as_secs())
}

// ---------------------------------------------------------------------------
// The power key
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum KeyPress {
    None,
    /// Pressed and let go before LONG_PRESS.
    Short,
    /// Held for LONG_PRESS; reported once, while still held.
    Long,
}

/// Turns samples of the power key's level into presses.
#[derive(Debug, Default)]
pub(crate) struct PowerKey {
    down_since: Option<Instant>,
    /// The press under way has already counted, as a long press or as the
    /// one that woke the device; letting go of it does nothing.
    spent: bool,
}

impl PowerKey {
    pub(crate) fn sample(&mut self, down: bool, now: Instant) -> KeyPress {
        match (down, self.down_since) {
            (true, None) => {
                self.down_since = Some(now);
                KeyPress::None
            }
            (true, Some(since)) => {
                if !self.spent && now.duration_since(since) >= LONG_PRESS {
                    self.spent = true;
                    KeyPress::Long
                } else {
                    KeyPress::None
                }
            }
            (false, Some(since)) => {
                let counts = !self.spent && now.duration_since(since) >= MIN_PRESS;
                self.down_since = None;
                self.spent = false;
                if counts {
                    KeyPress::Short
                } else {
                    KeyPress::None
                }
            }
            (false, None) => KeyPress::None,
        }
    }

    /// The device just woke. If the key is down it is the press that woke it,
    /// and letting go of it must not put the device straight back to sleep.
    pub(crate) fn woke(&mut self, down: bool, now: Instant) {
        if down {
            self.down_since.get_or_insert(now);
            self.spent = true;
        } else {
            *self = PowerKey::default();
        }
    }
}

// ---------------------------------------------------------------------------
// The phases
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Depth {
    Light,
    Deep,
    /// Powered off: the board cut off from its battery.
    Off,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Request {
    Sleep,
    DeepSleep,
    PowerOff,
}

/// Why the CPU came back from a light sleep.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Woke {
    /// The power key or the keyboard.
    User,
    /// The modem's RI line: an SMS, or a call.
    Modem,
    /// A LoRa packet.
    Radio,
    /// The timer set for deep sleep, or a slice of a long sleep.
    Timer,
    /// Anything else.
    Other,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Phase {
    Awake,
    Settling { depth: Depth, since: Instant },
    Asleep { doze_until: Option<Instant> },
}

/// What the power manager should do next.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Action {
    Nothing,
    OpenMenu,
    /// Sleep was asked for, but something holds the device awake.
    HeldOff,
    /// Put up the sleep screen, stop the window manager and rest the network.
    Settle(Depth),
    /// Settling was called off: undo it.
    Resume,
    /// The screen and the network are ready: put the drivers to sleep.
    FallAsleep,
    /// Halt the CPU, until `until` at the latest.
    LightSleep { until: Option<Instant> },
    DeepSleep,
    PowerOff,
    /// Bring the drivers, the network and the screen back.
    Wake,
}

/// What the board can do, from its profile.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Caps {
    /// Something other than a timer can wake it from light sleep.
    pub(crate) sleep: bool,
    /// Something can wake it from deep sleep.
    pub(crate) deep_sleep: bool,
    /// A window manager can show the power menu.
    pub(crate) menu: bool,
    /// The power driver can cut the board off from its battery.
    pub(crate) power_off: bool,
}

/// What the power manager saw since it last asked.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Facts {
    pub(crate) now: Instant,
    /// When someone last used the device.
    pub(crate) last_activity: Instant,
    pub(crate) key: KeyPress,
    pub(crate) request: Option<Request>,
    /// Something holds the device awake.
    pub(crate) held: bool,
    pub(crate) on_usb: bool,
    /// The window manager has stopped and the network is resting.
    pub(crate) settled: bool,
    /// The power key or a key on the keyboard is down while the CPU is up
    /// between sleeps.
    pub(crate) user_waiting: bool,
    pub(crate) battery_critical: bool,
    /// MeshCore has packets queued to go out.
    pub(crate) radio_busy: bool,
}

/// An early bedtime: after a wake nobody asked for, the device sleeps again
/// at `at`, unless someone uses it after `set_at`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Early {
    set_at: Instant,
    at: Instant,
    depth: Depth,
}

#[derive(Debug)]
pub(crate) struct Policy {
    pub(crate) settings: Settings,
    pub(crate) caps: Caps,
    phase: Phase,
    early: Option<Early>,
    /// After a packet woke the CPU, how long it may stay up for MeshCore.
    radio_limit: Option<Instant>,
}

impl Policy {
    pub(crate) fn new(settings: Settings, caps: Caps) -> Self {
        Policy {
            settings,
            caps,
            phase: Phase::Awake,
            early: None,
            radio_limit: None,
        }
    }

    pub(crate) fn phase(&self) -> Phase {
        self.phase
    }

    /// The device came up from deep sleep because the modem rang. Nobody is
    /// at it: after the grace, it goes straight back to deep sleep.
    pub(crate) fn booted_by_modem(&mut self, now: Instant) {
        self.early = Some(Early {
            set_at: now,
            at: now + UNASKED_WAKE_GRACE,
            depth: Depth::Deep,
        });
    }

    fn early(&self, depth: Depth) -> Option<Instant> {
        self.early
            .filter(|e| depth == Depth::Light || e.depth == Depth::Deep)
            .map(|e| e.at)
    }

    fn sleep_due(&self, now: Instant, last_activity: Instant) -> bool {
        let at = self
            .early(Depth::Light)
            .or_else(|| self.settings.sleep_after.map(|d| last_activity + d));
        at.is_some_and(|at| now >= at)
    }

    /// When deep sleep is due, if ever.
    pub(crate) fn deep_sleep_at(&self, last_activity: Instant) -> Option<Instant> {
        if !self.caps.deep_sleep {
            return None;
        }
        self.early(Depth::Deep)
            .or_else(|| self.settings.deep_sleep_after.map(|d| last_activity + d))
    }

    fn deep_due(&self, now: Instant, last_activity: Instant) -> bool {
        self.deep_sleep_at(last_activity).is_some_and(|at| now >= at)
    }

    /// Where the deep sleep timer takes the device: deep sleep, or off.
    fn idle_depth(&self) -> Depth {
        if self.settings.power_off_when_idle && self.caps.power_off {
            Depth::Off
        } else {
            Depth::Deep
        }
    }

    /// Whether the clock alone says it is time to sleep, whatever holds or
    /// USB power say: the power manager asks before reading the charger.
    pub(crate) fn idle_due(&self, now: Instant, last_activity: Instant) -> bool {
        self.caps.sleep
            && (self.deep_due(now, last_activity) || self.sleep_due(now, last_activity))
    }

    fn settle(&mut self, depth: Depth, now: Instant) -> Action {
        let depth = if depth == Depth::Deep && !self.caps.deep_sleep {
            Depth::Light
        } else {
            depth
        };
        self.phase = Phase::Settling { depth, since: now };
        Action::Settle(depth)
    }

    /// Sleep someone asked for, at `depth`.
    fn asked(&mut self, depth: Depth, f: &Facts) -> Action {
        let can = match depth {
            Depth::Off => self.caps.power_off,
            Depth::Light | Depth::Deep => self.caps.sleep,
        };
        if !can {
            return Action::Nothing;
        }
        if f.held {
            return Action::HeldOff;
        }
        self.settle(depth, f.now)
    }

    pub(crate) fn decide(&mut self, f: &Facts) -> Action {
        // Someone used the device since the early bedtime was set: it is
        // theirs again, on the usual clock.
        if self.early.is_some_and(|e| f.last_activity > e.set_at) {
            self.early = None;
        }

        match self.phase {
            Phase::Awake => {
                match f.key {
                    KeyPress::Long if self.caps.menu => return Action::OpenMenu,
                    KeyPress::Long => return self.asked(Depth::Deep, f),
                    KeyPress::Short => return self.asked(Depth::Light, f),
                    KeyPress::None => {}
                }
                match f.request {
                    Some(Request::Sleep) => return self.asked(Depth::Light, f),
                    Some(Request::DeepSleep) => return self.asked(Depth::Deep, f),
                    Some(Request::PowerOff) => return self.asked(Depth::Off, f),
                    None => {}
                }
                if !self.caps.sleep || f.held || (f.on_usb && self.settings.stay_awake_on_usb) {
                    return Action::Nothing;
                }
                if self.deep_due(f.now, f.last_activity) {
                    return self.settle(self.idle_depth(), f.now);
                }
                if self.sleep_due(f.now, f.last_activity) {
                    return self.settle(Depth::Light, f.now);
                }
                Action::Nothing
            }
            Phase::Settling { depth, since } => {
                if f.key != KeyPress::None || f.last_activity > since {
                    self.phase = Phase::Awake;
                    return Action::Resume;
                }
                if f.settled || f.now.duration_since(since) >= SETTLE_TIMEOUT {
                    return match depth {
                        Depth::Light => {
                            self.phase = Phase::Asleep { doze_until: None };
                            Action::FallAsleep
                        }
                        Depth::Deep => Action::DeepSleep,
                        Depth::Off => Action::PowerOff,
                    };
                }
                Action::Nothing
            }
            Phase::Asleep { doze_until } => {
                if f.user_waiting || f.key != KeyPress::None {
                    self.phase = Phase::Awake;
                    self.early = None;
                    return Action::Wake;
                }
                if self.deep_due(f.now, f.last_activity) && self.idle_depth() == Depth::Off {
                    // Settled again, for the screen to say it is off.
                    return self.settle(Depth::Off, f.now);
                }
                if self.deep_due(f.now, f.last_activity) || (f.battery_critical && self.caps.deep_sleep) {
                    return Action::DeepSleep;
                }
                let sending = f.radio_busy && self.radio_limit.is_some_and(|l| f.now < l);
                if doze_until.is_some_and(|t| f.now < t) || sending {
                    return Action::Nothing;
                }
                self.phase = Phase::Asleep { doze_until: None };
                self.radio_limit = None;
                Action::LightSleep {
                    until: self.deep_sleep_at(f.last_activity),
                }
            }
        }
    }

    /// The CPU came back from a light sleep, for `why`.
    pub(crate) fn woke(&mut self, why: Woke, now: Instant) -> Action {
        match why {
            Woke::User => {
                self.phase = Phase::Awake;
                self.early = None;
                Action::Wake
            }
            Woke::Modem => {
                self.phase = Phase::Awake;
                self.early = Some(Early {
                    set_at: now,
                    at: now + UNASKED_WAKE_GRACE,
                    depth: Depth::Light,
                });
                Action::Wake
            }
            Woke::Radio => {
                self.phase = Phase::Asleep {
                    doze_until: Some(now + RADIO_DOZE),
                };
                self.radio_limit = Some(now + RADIO_DOZE_MAX);
                Action::Nothing
            }
            Woke::Timer | Woke::Other => {
                self.phase = Phase::Asleep { doze_until: None };
                Action::Nothing
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const ALL: Caps = Caps {
        sleep: true,
        deep_sleep: true,
        menu: true,
        power_off: true,
    };

    fn secs(s: u64) -> Duration {
        Duration::from_secs(s)
    }

    fn settings(sleep: u64, deep: u64) -> Settings {
        Settings {
            sleep_after: (sleep > 0).then(|| secs(sleep)),
            deep_sleep_after: (deep > 0).then(|| secs(deep)),
            wake_on_modem: true,
            stay_awake_on_usb: true,
            power_off_when_idle: false,
        }
    }

    fn powering_off_when_idle(sleep: u64, deep: u64) -> Settings {
        Settings {
            power_off_when_idle: true,
            ..settings(sleep, deep)
        }
    }

    fn facts(t0: Instant, at: u64, last_activity: u64) -> Facts {
        Facts {
            now: t0 + secs(at),
            last_activity: t0 + secs(last_activity),
            key: KeyPress::None,
            request: None,
            held: false,
            on_usb: false,
            settled: false,
            user_waiting: false,
            battery_critical: false,
            radio_busy: false,
        }
    }

    /// A policy already in the given phase, reached the way the device would.
    fn asleep(t0: Instant) -> Policy {
        let mut p = Policy::new(settings(60, 600), ALL);
        assert_eq!(p.decide(&facts(t0, 60, 0)), Action::Settle(Depth::Light));
        let mut f = facts(t0, 61, 0);
        f.settled = true;
        assert_eq!(p.decide(&f), Action::FallAsleep);
        p
    }

    // ── Settings ────────────────────────────────────────────────────────

    #[test]
    fn with_nothing_saved_the_defaults_stand() {
        assert_eq!(settings_in(None), Settings::default());
        assert_eq!(settings_in(Some(r#"{ "thistle_os": {} }"#)), Settings::default());
    }

    #[test]
    fn saved_settings_read_back() {
        let json = r#"{ "thistle_os": { "power": { "sleep_after_s": 120,
            "deep_sleep_after_s": 7200, "wake_on_modem": false,
            "stay_awake_on_usb": false, "power_off_when_idle": true } } }"#;
        assert_eq!(
            settings_in(Some(json)),
            Settings {
                sleep_after: Some(secs(120)),
                deep_sleep_after: Some(secs(7200)),
                wake_on_modem: false,
                stay_awake_on_usb: false,
                power_off_when_idle: true,
            }
        );
    }

    #[test]
    fn zero_is_never() {
        let json = r#"{ "thistle_os": { "power": { "sleep_after_s": 0, "deep_sleep_after_s": 0 } } }"#;
        let s = settings_in(Some(json));
        assert_eq!(s.sleep_after, None);
        assert_eq!(s.deep_sleep_after, None);
        assert_eq!(seconds(s.sleep_after), 0);
    }

    #[test]
    fn a_time_too_short_to_use_the_device_in_is_raised() {
        let json = r#"{ "thistle_os": { "power": { "sleep_after_s": 1, "deep_sleep_after_s": 5 } } }"#;
        let s = settings_in(Some(json));
        assert_eq!(s.sleep_after, Some(MIN_SLEEP_AFTER));
        assert_eq!(s.deep_sleep_after, Some(MIN_DEEP_SLEEP_AFTER));
    }

    #[test]
    fn a_value_of_the_wrong_kind_is_the_default() {
        let json = r#"{ "thistle_os": { "power": { "sleep_after_s": "soon", "wake_on_modem": 1 } } }"#;
        let s = settings_in(Some(json));
        assert_eq!(s.sleep_after, Settings::default().sleep_after);
        assert!(s.wake_on_modem);
    }

    // ── The power key ───────────────────────────────────────────────────

    #[test]
    fn a_tap_is_a_short_press_on_release() {
        let t0 = Instant::now();
        let mut k = PowerKey::default();
        assert_eq!(k.sample(true, t0), KeyPress::None);
        assert_eq!(k.sample(true, t0 + Duration::from_millis(50)), KeyPress::None);
        assert_eq!(k.sample(false, t0 + Duration::from_millis(200)), KeyPress::Short);
        assert_eq!(k.sample(false, t0 + Duration::from_millis(250)), KeyPress::None);
    }

    #[test]
    fn a_glitch_shorter_than_a_press_is_ignored() {
        let t0 = Instant::now();
        let mut k = PowerKey::default();
        k.sample(true, t0);
        assert_eq!(k.sample(false, t0 + Duration::from_millis(10)), KeyPress::None);
    }

    #[test]
    fn holding_the_key_is_one_long_press_and_letting_go_is_nothing() {
        let t0 = Instant::now();
        let mut k = PowerKey::default();
        k.sample(true, t0);
        assert_eq!(k.sample(true, t0 + Duration::from_millis(1400)), KeyPress::None);
        assert_eq!(k.sample(true, t0 + LONG_PRESS), KeyPress::Long);
        assert_eq!(k.sample(true, t0 + secs(3)), KeyPress::None);
        assert_eq!(k.sample(false, t0 + secs(4)), KeyPress::None);
    }

    #[test]
    fn the_press_that_woke_the_device_does_not_put_it_back_to_sleep() {
        let t0 = Instant::now();
        let mut k = PowerKey::default();
        k.woke(true, t0);
        assert_eq!(k.sample(true, t0 + Duration::from_millis(50)), KeyPress::None);
        assert_eq!(k.sample(true, t0 + secs(2)), KeyPress::None);
        assert_eq!(k.sample(false, t0 + secs(3)), KeyPress::None);
        // The next press is a press again.
        k.sample(true, t0 + secs(4));
        assert_eq!(k.sample(false, t0 + secs(5)), KeyPress::Short);
    }

    #[test]
    fn a_key_already_let_go_at_wake_leaves_nothing_spent() {
        let t0 = Instant::now();
        let mut k = PowerKey::default();
        k.woke(false, t0);
        k.sample(true, t0 + secs(1));
        assert_eq!(k.sample(false, t0 + secs(2)), KeyPress::Short);
    }

    // ── Awake ───────────────────────────────────────────────────────────

    #[test]
    fn idle_for_the_set_time_it_settles_into_sleep() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(60, 600), ALL);
        assert_eq!(p.decide(&facts(t0, 59, 0)), Action::Nothing);
        assert_eq!(p.decide(&facts(t0, 60, 0)), Action::Settle(Depth::Light));
        assert!(matches!(p.phase(), Phase::Settling { depth: Depth::Light, .. }));
    }

    #[test]
    fn idle_due_follows_the_clock_alone() {
        let t0 = Instant::now();
        let p = Policy::new(settings(60, 600), ALL);
        assert!(!p.idle_due(t0 + secs(59), t0));
        assert!(p.idle_due(t0 + secs(60), t0));
        let p = Policy::new(settings(60, 600), Caps::default());
        assert!(!p.idle_due(t0 + secs(600), t0));
    }

    #[test]
    fn use_puts_sleep_off() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(60, 600), ALL);
        assert_eq!(p.decide(&facts(t0, 90, 40)), Action::Nothing);
        assert_eq!(p.decide(&facts(t0, 100, 40)), Action::Settle(Depth::Light));
    }

    #[test]
    fn never_means_never() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(0, 0), ALL);
        assert_eq!(p.decide(&facts(t0, 1_000_000, 0)), Action::Nothing);
    }

    #[test]
    fn deep_sleep_due_first_goes_straight_to_deep_sleep() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(600, 300), ALL);
        assert_eq!(p.decide(&facts(t0, 300, 0)), Action::Settle(Depth::Deep));
    }

    #[test]
    fn a_hold_keeps_it_awake_until_released() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(60, 600), ALL);
        let mut f = facts(t0, 3600, 0);
        f.held = true;
        assert_eq!(p.decide(&f), Action::Nothing);
        f.held = false;
        assert_eq!(p.decide(&f), Action::Settle(Depth::Deep));
    }

    #[test]
    fn on_usb_it_stays_awake_by_the_clock_if_set_to() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(60, 600), ALL);
        let mut f = facts(t0, 3600, 0);
        f.on_usb = true;
        assert_eq!(p.decide(&f), Action::Nothing);
        p.settings.stay_awake_on_usb = false;
        assert_eq!(p.decide(&f), Action::Settle(Depth::Deep));
    }

    #[test]
    fn on_usb_the_power_key_still_sleeps() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(60, 600), ALL);
        let mut f = facts(t0, 5, 4);
        f.on_usb = true;
        f.key = KeyPress::Short;
        assert_eq!(p.decide(&f), Action::Settle(Depth::Light));
    }

    #[test]
    fn a_board_that_cannot_wake_never_sleeps() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(60, 600), Caps::default());
        assert_eq!(p.decide(&facts(t0, 3600, 0)), Action::Nothing);
        let mut f = facts(t0, 3600, 0);
        f.request = Some(Request::Sleep);
        assert_eq!(p.decide(&f), Action::Nothing);
    }

    #[test]
    fn a_board_with_no_deep_sleep_sleeps_lightly_instead() {
        let t0 = Instant::now();
        let caps = Caps {
            sleep: true,
            deep_sleep: false,
            menu: true,
            power_off: false,
        };
        let mut p = Policy::new(settings(600, 60), caps);
        assert_eq!(p.decide(&facts(t0, 60, 0)), Action::Nothing);
        let mut f = facts(t0, 61, 60);
        f.request = Some(Request::DeepSleep);
        assert_eq!(p.decide(&f), Action::Settle(Depth::Light));
    }

    #[test]
    fn a_short_press_sleeps_and_a_long_one_opens_the_menu() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(600, 6000), ALL);
        let mut f = facts(t0, 5, 4);
        f.key = KeyPress::Long;
        assert_eq!(p.decide(&f), Action::OpenMenu);
        assert_eq!(p.phase(), Phase::Awake);
        f.key = KeyPress::Short;
        assert_eq!(p.decide(&f), Action::Settle(Depth::Light));
    }

    #[test]
    fn without_a_menu_a_long_press_is_deep_sleep() {
        let t0 = Instant::now();
        let mut p = Policy::new(
            settings(600, 6000),
            Caps {
                menu: false,
                ..ALL
            },
        );
        let mut f = facts(t0, 5, 4);
        f.key = KeyPress::Long;
        assert_eq!(p.decide(&f), Action::Settle(Depth::Deep));
    }

    #[test]
    fn asked_to_sleep_while_held_it_says_so_and_stays_up() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(600, 6000), ALL);
        let mut f = facts(t0, 5, 4);
        f.held = true;
        f.request = Some(Request::DeepSleep);
        assert_eq!(p.decide(&f), Action::HeldOff);
        assert_eq!(p.phase(), Phase::Awake);
    }

    #[test]
    fn powering_off_settles_then_powers_off() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(600, 6000), ALL);
        let mut f = facts(t0, 5, 4);
        f.request = Some(Request::PowerOff);
        assert_eq!(p.decide(&f), Action::Settle(Depth::Off));
        let mut f = facts(t0, 6, 4);
        f.settled = true;
        assert_eq!(p.decide(&f), Action::PowerOff);
    }

    #[test]
    fn powering_off_goes_ahead_after_the_settle_timeout() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(600, 6000), ALL);
        let mut f = facts(t0, 5, 4);
        f.request = Some(Request::PowerOff);
        p.decide(&f);
        assert_eq!(p.decide(&facts(t0, 10, 4)), Action::Nothing);
        assert_eq!(p.decide(&facts(t0, 5 + SETTLE_TIMEOUT.as_secs(), 4)), Action::PowerOff);
    }

    #[test]
    fn a_key_while_powering_off_calls_it_off() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(600, 6000), ALL);
        let mut f = facts(t0, 5, 4);
        f.request = Some(Request::PowerOff);
        p.decide(&f);
        assert_eq!(p.decide(&facts(t0, 6, 6)), Action::Resume);
        assert_eq!(p.phase(), Phase::Awake);
    }

    #[test]
    fn a_board_that_cannot_power_off_ignores_the_request() {
        let t0 = Instant::now();
        let mut p = Policy::new(
            settings(600, 6000),
            Caps {
                power_off: false,
                ..ALL
            },
        );
        let mut f = facts(t0, 5, 4);
        f.request = Some(Request::PowerOff);
        assert_eq!(p.decide(&f), Action::Nothing);
        assert_eq!(p.phase(), Phase::Awake);
    }

    #[test]
    fn a_board_that_cannot_sleep_may_still_power_off() {
        let t0 = Instant::now();
        let caps = Caps {
            power_off: true,
            ..Caps::default()
        };
        let mut p = Policy::new(settings(600, 6000), caps);
        let mut f = facts(t0, 5, 4);
        f.request = Some(Request::PowerOff);
        assert_eq!(p.decide(&f), Action::Settle(Depth::Off));
    }

    #[test]
    fn asked_to_power_off_while_held_it_says_so_and_stays_up() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(600, 6000), ALL);
        let mut f = facts(t0, 5, 4);
        f.held = true;
        f.request = Some(Request::PowerOff);
        assert_eq!(p.decide(&f), Action::HeldOff);
    }

    #[test]
    fn the_deep_sleep_timer_deep_sleeps_unless_told_to_power_off() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(0, 600), ALL);
        assert_eq!(p.decide(&facts(t0, 600, 0)), Action::Settle(Depth::Deep));
        let mut p = Policy::new(powering_off_when_idle(0, 600), ALL);
        assert_eq!(p.decide(&facts(t0, 600, 0)), Action::Settle(Depth::Off));
    }

    #[test]
    fn a_board_that_cannot_power_off_deep_sleeps_when_told_to_power_off() {
        let t0 = Instant::now();
        let caps = Caps {
            power_off: false,
            ..ALL
        };
        let mut p = Policy::new(powering_off_when_idle(0, 600), caps);
        assert_eq!(p.decide(&facts(t0, 600, 0)), Action::Settle(Depth::Deep));
    }

    #[test]
    fn asleep_when_the_deep_sleep_time_comes_it_settles_again_to_power_off() {
        let t0 = Instant::now();
        let mut p = Policy::new(powering_off_when_idle(60, 600), ALL);
        p.decide(&facts(t0, 60, 0));
        let mut f = facts(t0, 61, 0);
        f.settled = true;
        assert_eq!(p.decide(&f), Action::FallAsleep);
        assert_eq!(p.decide(&facts(t0, 600, 0)), Action::Settle(Depth::Off));
        assert!(matches!(p.phase(), Phase::Settling { depth: Depth::Off, .. }));
        let mut f = facts(t0, 601, 0);
        f.settled = true;
        assert_eq!(p.decide(&f), Action::PowerOff);
    }

    #[test]
    fn a_critical_battery_still_deep_sleeps_when_told_to_power_off() {
        let t0 = Instant::now();
        let mut p = Policy::new(powering_off_when_idle(60, 6000), ALL);
        p.decide(&facts(t0, 60, 0));
        let mut f = facts(t0, 61, 0);
        f.settled = true;
        p.decide(&f);
        let mut f = facts(t0, 100, 0);
        f.battery_critical = true;
        assert_eq!(p.decide(&f), Action::DeepSleep);
    }

    // ── Settling ────────────────────────────────────────────────────────

    #[test]
    fn settled_it_falls_asleep() {
        let t0 = Instant::now();
        let p = asleep(t0);
        assert_eq!(p.phase(), Phase::Asleep { doze_until: None });
    }

    #[test]
    fn settling_into_deep_sleep_ends_in_deep_sleep() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(600, 6000), ALL);
        let mut f = facts(t0, 5, 4);
        f.request = Some(Request::DeepSleep);
        assert_eq!(p.decide(&f), Action::Settle(Depth::Deep));
        let mut f = facts(t0, 6, 4);
        f.settled = true;
        assert_eq!(p.decide(&f), Action::DeepSleep);
    }

    #[test]
    fn a_network_that_never_rests_does_not_keep_it_awake() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(60, 600), ALL);
        p.decide(&facts(t0, 60, 0));
        assert_eq!(p.decide(&facts(t0, 70, 0)), Action::Nothing);
        assert_eq!(p.decide(&facts(t0, 75, 0)), Action::FallAsleep);
    }

    #[test]
    fn using_the_device_while_it_settles_calls_sleep_off() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(60, 600), ALL);
        p.decide(&facts(t0, 60, 0));
        assert_eq!(p.decide(&facts(t0, 61, 61)), Action::Resume);
        assert_eq!(p.phase(), Phase::Awake);
    }

    #[test]
    fn the_power_key_while_it_settles_calls_sleep_off() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(60, 600), ALL);
        p.decide(&facts(t0, 60, 0));
        let mut f = facts(t0, 61, 0);
        f.key = KeyPress::Short;
        assert_eq!(p.decide(&f), Action::Resume);
    }

    // ── Asleep ──────────────────────────────────────────────────────────

    #[test]
    fn asleep_it_sleeps_until_deep_sleep_is_due() {
        let t0 = Instant::now();
        let mut p = asleep(t0);
        assert_eq!(
            p.decide(&facts(t0, 62, 0)),
            Action::LightSleep {
                until: Some(t0 + secs(600))
            }
        );
        assert_eq!(p.woke(Woke::Timer, t0 + secs(600)), Action::Nothing);
        assert_eq!(p.decide(&facts(t0, 600, 0)), Action::DeepSleep);
    }

    #[test]
    fn with_deep_sleep_off_it_sleeps_with_no_timer() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(60, 0), ALL);
        p.decide(&facts(t0, 60, 0));
        let mut f = facts(t0, 61, 0);
        f.settled = true;
        p.decide(&f);
        assert_eq!(p.decide(&facts(t0, 62, 0)), Action::LightSleep { until: None });
    }

    #[test]
    fn the_user_wakes_it() {
        let t0 = Instant::now();
        let mut p = asleep(t0);
        p.decide(&facts(t0, 62, 0));
        assert_eq!(p.woke(Woke::User, t0 + secs(100)), Action::Wake);
        assert_eq!(p.phase(), Phase::Awake);
        // Back on the usual clock, from the press that woke it.
        assert_eq!(p.decide(&facts(t0, 150, 100)), Action::Nothing);
        assert_eq!(p.decide(&facts(t0, 160, 100)), Action::Settle(Depth::Light));
    }

    #[test]
    fn a_packet_keeps_it_up_briefly_then_it_sleeps_again() {
        let t0 = Instant::now();
        let mut p = asleep(t0);
        p.decide(&facts(t0, 62, 0));
        assert_eq!(p.woke(Woke::Radio, t0 + secs(100)), Action::Nothing);
        assert_eq!(p.decide(&facts(t0, 101, 0)), Action::Nothing);
        assert!(matches!(p.decide(&facts(t0, 102, 0)), Action::LightSleep { .. }));
    }

    #[test]
    fn an_answer_still_to_send_keeps_it_up_but_not_for_ever() {
        let t0 = Instant::now();
        let mut p = asleep(t0);
        p.woke(Woke::Radio, t0 + secs(100));
        let mut f = facts(t0, 105, 0);
        f.radio_busy = true;
        assert_eq!(p.decide(&f), Action::Nothing);
        let mut f = facts(t0, 110, 0);
        f.radio_busy = true;
        assert!(matches!(p.decide(&f), Action::LightSleep { .. }));
    }

    #[test]
    fn a_key_pressed_while_it_dozes_wakes_it() {
        let t0 = Instant::now();
        let mut p = asleep(t0);
        p.woke(Woke::Radio, t0 + secs(100));
        let mut f = facts(t0, 101, 0);
        f.user_waiting = true;
        assert_eq!(p.decide(&f), Action::Wake);
    }

    #[test]
    fn an_sms_wakes_it_for_a_moment_only() {
        let t0 = Instant::now();
        let mut p = asleep(t0);
        p.decide(&facts(t0, 62, 0));
        assert_eq!(p.woke(Woke::Modem, t0 + secs(100)), Action::Wake);
        assert_eq!(p.decide(&facts(t0, 129, 0)), Action::Nothing);
        assert_eq!(p.decide(&facts(t0, 130, 0)), Action::Settle(Depth::Light));
    }

    #[test]
    fn using_the_device_after_an_sms_puts_it_back_on_the_usual_clock() {
        let t0 = Instant::now();
        let mut p = asleep(t0);
        p.woke(Woke::Modem, t0 + secs(100));
        assert_eq!(p.decide(&facts(t0, 130, 110)), Action::Nothing);
        assert_eq!(p.decide(&facts(t0, 170, 110)), Action::Settle(Depth::Light));
    }

    #[test]
    fn a_dying_battery_ends_a_light_sleep_in_deep_sleep() {
        let t0 = Instant::now();
        let mut p = asleep(t0);
        let mut f = facts(t0, 62, 0);
        f.battery_critical = true;
        assert_eq!(p.decide(&f), Action::DeepSleep);
    }

    #[test]
    fn a_dying_battery_does_not_take_the_device_from_someone_using_it() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(60, 600), ALL);
        let mut f = facts(t0, 30, 29);
        f.battery_critical = true;
        assert_eq!(p.decide(&f), Action::Nothing);
    }

    // ── Booting from deep sleep ─────────────────────────────────────────

    #[test]
    fn rung_awake_from_deep_sleep_it_goes_back_to_deep_sleep_after_the_grace() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(60, 600), ALL);
        p.booted_by_modem(t0);
        assert_eq!(p.decide(&facts(t0, 29, 0)), Action::Nothing);
        assert_eq!(p.decide(&facts(t0, 30, 0)), Action::Settle(Depth::Deep));
    }

    #[test]
    fn rung_awake_and_then_used_it_keeps_the_usual_clock() {
        let t0 = Instant::now();
        let mut p = Policy::new(settings(60, 600), ALL);
        p.booted_by_modem(t0);
        assert_eq!(p.decide(&facts(t0, 30, 10)), Action::Nothing);
        assert_eq!(p.decide(&facts(t0, 70, 10)), Action::Settle(Depth::Light));
    }
}
