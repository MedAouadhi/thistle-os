// SPDX-License-Identifier: BSD-3-Clause
//! Keeps up the network transports the user wants, so that traffic always
//! has the best one available.
//!
//! Choosing the route is not done here. esp_netif sends everything through
//! the connected interface with the highest route priority, WiFi station
//! (100) above PPP (20), and with CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF
//! the DNS servers follow that choice. Whenever WiFi is connected it carries
//! the traffic. When it drops, a cellular link that is already up takes over
//! at once, without any socket being told. net_manager ranks transports the
//! same way, so it names the one in use.
//!
//! What this module decides is what to keep up:
//!
//! - **Mobile data on**: the cellular link is kept connected, WiFi or not, so
//!   it is ready the moment WiFi goes. A failed attempt is retried with
//!   backoff, and a session that drops is redialled at once. A SIM that is
//!   missing or locked will not fix itself, so the modem is powered down and
//!   left until mobile data is switched off and on again.
//! - **Mobile data off, SMS on**: the modem is powered and registered with a
//!   network, without a data session, so SMS arrive. Switching mobile data
//!   off ends its session by powering the modem down and up again.
//! - **Both off**: the modem is powered down, including at boot, for a modem
//!   left running by a reset of the SoC alone.
//! - **WiFi**: the WiFi manager joins a network only when asked, so a saved
//!   network that is enabled is rejoined, with backoff, after it drops,
//!   unless WiFi is switched off (`wifi_manager_set_enabled`).
//!
//! The settings are `thistle_os.cellular.enabled` (default off),
//! `thistle_os.cellular.sms` (default on) and `thistle_os.cellular.apn`
//! (default "", the APN the network assigns) in system.json.
//!
//! SMS wait in the modem until fetched. Whenever the driver says some may be
//! waiting, they are fetched here (sms_manager), before anything but a
//! power-down: before a data session is dialled, so it is not interrupted
//! straight after. During a session the driver says so only when the
//! modem's RI line signals an SMS, and fetching interrupts the session for
//! a few seconds, as sending one does. Each cellular attempt therefore first
//! registers without dialling (`listen`), fetches, and then dials.
//!
//! While the device sleeps the network **rests** (`rest`): WiFi stops, and a
//! modem that is on ends its data session and sleeps listening for calls and
//! SMS, raising RI for them. Before deep sleep without "wake on calls and
//! SMS", the modem is powered down instead. Back at `Rest::Normal`, the
//! modem is woken, WiFi is started, and both reconnect at once.
//!
//! All of it happens on the coordinator's one thread, started at boot with
//! its stack in internal RAM: powering the modem down, rejoining WiFi, and
//! cellular attempts, one at a time. So WiFi waits to be rejoined while an
//! attempt runs, up to two minutes; traffic has the cellular link meanwhile.
//! Mobile data switched off cancels an attempt at once, through the driver.

use std::os::raw::c_char;
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{Condvar, Mutex};
use std::time::{Duration, Instant};

use crate::net_manager::{find_transport, CCellInfo, HalNetDriver, HAL_NET_CELLULAR};

const ESP_OK: i32 = 0;
const ESP_ERR_NO_MEM: i32 = 0x101;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_ERR_NOT_FOUND: i32 = 0x105;
const ESP_ERR_NOT_SUPPORTED: i32 = 0x106;

// hal_net_state_t
const NET_DISCONNECTED: u32 = 0;
const NET_CONNECTING: u32 = 1;
const NET_CONNECTED: u32 = 2;

// wifi_state_t
const WIFI_DISCONNECTED: u32 = 0;
const WIFI_CONNECTING: u32 = 1;
const WIFI_CONNECTED: u32 = 2;
const WIFI_FAILED: u32 = 3;

const SECTION: &str = "cellular";

/// The longest APN accepted. 3GPP allows 100 octets; no carrier comes near.
pub(crate) const APN_MAX_LEN: usize = 63;

/// How long one cellular attempt may take: starting the modem, reading the
/// SIM, registering after a cold start, and dialling.
const CELL_CONNECT_TIMEOUT_MS: u32 = 120_000;

/// How often the coordinator looks again when nothing wakes it.
const TICK: Duration = Duration::from_secs(5);

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

#[cfg(not(test))]
static TAG: &[u8] = b"net_coord\0";

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
// Settings in system.json
// ---------------------------------------------------------------------------

/// Mobile data is off unless the user switched it on: a SIM costs money.
pub(crate) fn enabled_in(json: Option<&str>) -> bool {
    json.and_then(|j| crate::system_json::get(j, SECTION, "enabled"))
        .and_then(|v| v.as_bool())
        .unwrap_or(false)
}

/// SMS are received unless the user switched them off: a phone number that
/// cannot be reached is no use, and a registered modem with no data session
/// costs nothing but power.
pub(crate) fn sms_in(json: Option<&str>) -> bool {
    json.and_then(|j| crate::system_json::get(j, SECTION, "sms"))
        .and_then(|v| v.as_bool())
        .unwrap_or(true)
}

/// The APN saved, or "" for the one the network assigns. A saved value that
/// would not pass `valid_apn` is treated as none rather than sent.
pub(crate) fn apn_in(json: Option<&str>) -> String {
    json.and_then(|j| crate::system_json::get(j, SECTION, "apn"))
        .and_then(|v| v.as_str().map(str::to_string))
        .filter(|apn| valid_apn(apn))
        .unwrap_or_default()
}

/// An APN is dot-separated labels of letters, digits and hyphens; some
/// carriers use underscores. Anything else is refused, since it is sent to
/// the modem inside a quoted AT+CGDCONT argument.
pub(crate) fn valid_apn(apn: &str) -> bool {
    apn.len() <= APN_MAX_LEN
        && apn
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-' | b'_'))
}

fn read_system_json() -> Option<String> {
    std::fs::read_to_string(crate::system_json::path()).ok()
}

// ---------------------------------------------------------------------------
// Policy — pure, so it can be tested without a modem or a network
// ---------------------------------------------------------------------------

/// When to try again after `failures` failed attempts in a row.
fn backoff(failures: u32) -> Duration {
    const STEPS: [u64; 4] = [10, 30, 60, 120];
    let i = (failures.max(1) - 1) as usize;
    Duration::from_secs(STEPS[i.min(STEPS.len() - 1)])
}

#[derive(Debug, Default)]
struct Retry {
    failures: u32,
    next_try: Option<Instant>,
}

impl Retry {
    fn due(&self, now: Instant) -> bool {
        self.next_try.map_or(true, |t| now >= t)
    }
    fn failed(&mut self, now: Instant) {
        self.failures += 1;
        self.next_try = Some(now + backoff(self.failures));
    }
    fn reset(&mut self) {
        *self = Retry::default();
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) enum CellAction {
    None,
    /// Power the modem and register, without a data session.
    Listen,
    Connect,
    PowerOff,
}

/// What the settings ask of the modem.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub(crate) enum Want {
    #[default]
    Off,
    /// Registered, for SMS, with no data session.
    Listen,
    Data,
}

/// What the cellular link should do next.
#[derive(Debug, Default)]
pub(crate) struct CellPolicy {
    retry: Retry,
    /// The SIM was missing or locked; nothing is tried until mobile data is
    /// switched off and on again.
    blocked: bool,
    /// The modem is known to be powered down: at boot nothing is known, and
    /// a modem left running across a reset of the SoC must be shut down.
    off: bool,
    /// The modem is to be powered down and then reconnected, for a new APN.
    cycle: bool,
    /// Moves on whenever the modem is seen wanted off, or wanted for
    /// something else. An attempt that started in an earlier epoch was
    /// overtaken by the switch: its outcome says nothing about the SIM or
    /// the network as they are now.
    epoch: u32,
    want: Want,
}

impl CellPolicy {
    /// `attached` is whether the modem is on with a ready SIM, `state` the
    /// data session's hal_net_state_t. `connecting` and `powering_off` say
    /// whether an attempt or a power-down is under way.
    pub(crate) fn decide(
        &mut self,
        want: Want,
        attached: bool,
        state: u32,
        connecting: bool,
        powering_off: bool,
        now: Instant,
    ) -> CellAction {
        if powering_off {
            return CellAction::None;
        }
        if want != self.want {
            // A switch moved: start afresh, without the backoff or the SIM
            // verdict of what was wanted before.
            self.want = want;
            self.epoch = self.epoch.wrapping_add(1);
            self.retry.reset();
            self.blocked = false;
        }
        if want == Want::Off {
            self.epoch = self.epoch.wrapping_add(1);
            self.cycle = false;
            // An attempt in progress is cut short by the power-down.
            return if self.off { CellAction::None } else { CellAction::PowerOff };
        }
        if connecting {
            return CellAction::None;
        }
        // A new APN, or a data session to end while SMS are still wanted:
        // both start from a modem powered down.
        if self.cycle || (want == Want::Listen && state != NET_DISCONNECTED) {
            return CellAction::PowerOff;
        }
        if self.blocked || state != NET_DISCONNECTED || !self.retry.due(now) {
            return CellAction::None;
        }
        if attached && want == Want::Listen {
            return CellAction::None;
        }
        self.off = false;
        if attached {
            CellAction::Connect
        } else {
            CellAction::Listen
        }
    }

    /// The epoch to hand back to connect_finished() with the attempt that
    /// decide() just asked for.
    pub(crate) fn epoch(&self) -> u32 {
        self.epoch
    }

    pub(crate) fn connect_finished(&mut self, epoch: u32, result: i32, now: Instant) {
        // Whatever happened, the attempt may have left the modem on; if
        // mobile data is off by now, that has to be undone.
        self.off = false;
        if epoch != self.epoch {
            return;
        }
        match result {
            ESP_OK => self.retry.reset(),
            ESP_ERR_NOT_FOUND => self.blocked = true,
            _ => self.retry.failed(now),
        }
    }

    pub(crate) fn power_off_finished(&mut self) {
        self.off = true;
        self.cycle = false;
    }

    /// Reconnect from cold, so a changed APN is used.
    pub(crate) fn request_cycle(&mut self) {
        self.cycle = true;
        self.retry.reset();
    }

    pub(crate) fn blocked(&self) -> bool {
        self.blocked
    }

    /// The device woke: a session ended for sleep is redialled at once, not
    /// after whatever backoff came before.
    pub(crate) fn woke(&mut self) {
        self.retry.reset();
    }
}

/// Whether to rejoin the saved WiFi network now.
#[derive(Debug, Default)]
pub(crate) struct WifiPolicy {
    retry: Retry,
    /// No saved network, or the WiFi manager cannot join one here (the
    /// simulator): nothing is tried until WiFi is next seen connected, or is
    /// switched off and on.
    parked: bool,
}

impl WifiPolicy {
    /// `switched_on` is whether WiFi is switched on in Settings.
    pub(crate) fn decide(&mut self, state: u32, switched_on: bool, now: Instant) -> bool {
        if !switched_on {
            // Switched on again, it tries at once.
            *self = WifiPolicy::default();
            return false;
        }
        match state {
            WIFI_CONNECTED => {
                self.retry.reset();
                self.parked = false;
                false
            }
            WIFI_DISCONNECTED | WIFI_FAILED => !self.parked && self.retry.due(now),
            _ => false,
        }
    }

    pub(crate) fn rejoin_finished(&mut self, result: i32, now: Instant) {
        match result {
            ESP_OK => self.retry.reset(),
            ESP_ERR_NOT_FOUND | ESP_ERR_NOT_SUPPORTED => self.parked = true,
            _ => self.retry.failed(now),
        }
    }
}

// ---------------------------------------------------------------------------
// The coordinator
// ---------------------------------------------------------------------------

struct Coordinator {
    cell: CellPolicy,
    wifi: WifiPolicy,
    /// What the modem reported when its SIM turned out unusable, kept for
    /// Settings after the modem is powered down.
    blocked_info: Option<CCellInfo>,
    /// Something changed; look again now rather than at the next tick.
    poke: bool,
}

static COORD: Mutex<Option<Coordinator>> = Mutex::new(None);
static WAKE: Condvar = Condvar::new();

static CELL_WANTED: AtomicBool = AtomicBool::new(false);
static SMS_WANTED: AtomicBool = AtomicBool::new(true);
static CELL_CONNECTING: AtomicBool = AtomicBool::new(false);
static CELL_POWERING_OFF: AtomicBool = AtomicBool::new(false);

/// How much of the network to keep while the device sleeps.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Rest {
    /// Awake: everything the settings ask for.
    Normal = 0,
    /// Asleep: WiFi stopped, the modem asleep but listening, if it is on.
    Sleep = 1,
    /// About to go into deep sleep without the modem: WiFi stopped and the
    /// modem powered down. Only a restart comes back from this.
    Off = 2,
}

impl Rest {
    fn from_u8(v: u8) -> Rest {
        match v {
            1 => Rest::Sleep,
            2 => Rest::Off,
            _ => Rest::Normal,
        }
    }
}

static REST_WANTED: AtomicU8 = AtomicU8::new(Rest::Normal as u8);
static REST_REACHED: AtomicU8 = AtomicU8::new(Rest::Normal as u8);
/// The modem was put to sleep listening, and has not been woken since.
static MODEM_LISTENING: AtomicBool = AtomicBool::new(false);

pub(crate) fn cellular() -> Option<&'static HalNetDriver> {
    let drv = find_transport(HAL_NET_CELLULAR);
    // SAFETY: registered vtables are static and never unregistered.
    unsafe { drv.as_ref() }
}

fn cell_state(drv: &HalNetDriver) -> u32 {
    drv.get_state
        .map(|f| unsafe { f() })
        .unwrap_or(NET_DISCONNECTED)
}

fn cell_info(drv: &HalNetDriver) -> CCellInfo {
    let mut info = CCellInfo::default();
    if let Some(f) = drv.get_cell_info {
        unsafe { f(&mut info) };
    }
    info
}

// hal_sim_state_t
const SIM_READY: u32 = 1;

/// Whether the modem is on with a SIM it can use: registered, or about to be.
pub(crate) fn modem_ready(drv: &HalNetDriver) -> bool {
    let info = cell_info(drv);
    info.powered && info.sim == SIM_READY
}

fn want() -> Want {
    if CELL_WANTED.load(Ordering::SeqCst) {
        Want::Data
    } else if SMS_WANTED.load(Ordering::SeqCst) {
        Want::Listen
    } else {
        Want::Off
    }
}

/// Have the coordinator look again now, for a setting that changed.
pub(crate) fn poke() {
    if let Ok(mut c) = COORD.lock() {
        if let Some(c) = c.as_mut() {
            c.poke = true;
        }
    }
    WAKE.notify_all();
}

/// Rest the network for sleep, or bring it back. Returns at once; the
/// coordinator does it on its thread, and `rested` says when it is done.
pub(crate) fn rest(to: Rest) {
    REST_WANTED.store(to as u8, Ordering::SeqCst);
    let running = COORD.lock().map(|c| c.is_some()).unwrap_or(false);
    if !running {
        // Nothing to rest.
        REST_REACHED.store(to as u8, Ordering::SeqCst);
        return;
    }
    if to != Rest::Normal && CELL_CONNECTING.load(Ordering::SeqCst) {
        // An attempt can take two minutes; sleep does not wait for it.
        if let Some(cancel) = cellular().and_then(|d| d.cancel) {
            unsafe { cancel() };
        }
    }
    poke();
}

/// Whether the network has reached `to`, and nothing else was asked since.
pub(crate) fn rested(to: Rest) -> bool {
    REST_WANTED.load(Ordering::SeqCst) == to as u8 && REST_REACHED.load(Ordering::SeqCst) == to as u8
}

/// Whether the modem sleeps listening, so its RI line can wake the device.
pub(crate) fn modem_listening() -> bool {
    MODEM_LISTENING.load(Ordering::SeqCst)
}

fn modem_sleep(drv: Option<&'static HalNetDriver>, enter: bool) -> bool {
    match drv.and_then(|d| d.sleep) {
        Some(sleep) => unsafe { sleep(enter) == ESP_OK },
        None => false,
    }
}

/// Move the network from one rest to another, on the coordinator's thread.
fn change_rest(from: Rest, to: Rest) {
    let drv = cellular();
    if from == Rest::Normal {
        crate::wifi_manager::suspend(true);
    }
    match to {
        Rest::Normal => {
            if MODEM_LISTENING.swap(false, Ordering::SeqCst) {
                modem_sleep(drv, false);
            }
            crate::wifi_manager::suspend(false);
            if let Ok(mut c) = COORD.lock() {
                if let Some(c) = c.as_mut() {
                    c.wifi = WifiPolicy::default();
                    c.cell.woke();
                }
            }
            log!(ESP_LOG_INFO, b"network back from sleep\0");
        }
        Rest::Sleep => {
            if from == Rest::Normal && drv.is_some() {
                let listening = modem_sleep(drv, true);
                MODEM_LISTENING.store(listening, Ordering::SeqCst);
                log!(
                    ESP_LOG_INFO,
                    b"network resting; modem %s\0",
                    if listening {
                        b"asleep, listening\0".as_ptr()
                    } else {
                        b"off\0".as_ptr()
                    }
                );
            }
        }
        Rest::Off => {
            if let Some(drv) = drv {
                if MODEM_LISTENING.swap(false, Ordering::SeqCst) {
                    modem_sleep(Some(drv), false);
                }
                power_off(drv);
            }
        }
    }
    REST_REACHED.store(to as u8, Ordering::SeqCst);
}

#[cfg(target_os = "espidf")]
extern "C" {
    // components/kernel/src/link_deps.c
    fn thistle_pthread_name_next(name: *const u8) -> i32;
}

/// The coordinator's thread. Its stack is in internal RAM, as every stack
/// should be: one in PSRAM cannot be read while flash is written, and this
/// thread reads system.json and has the WiFi driver write NVS.
const STACK_SIZE: usize = 6144;

/// Network work, done on the coordinator's own thread, one piece at a time.
enum Work {
    None,
    PowerOff(&'static HalNetDriver),
    RejoinWifi,
    Connect(&'static HalNetDriver, u32),
    Listen(&'static HalNetDriver, u32),
    FetchSms(&'static HalNetDriver),
    Rest(Rest, Rest),
}

/// One cellular attempt: starting the modem, the SIM, a registration and,
/// unless `listen_only`, the data session, up to CELL_CONNECT_TIMEOUT_MS.
/// Switching off meanwhile cancels it through the driver
/// (net_cellular_set_enabled, net_cellular_set_sms).
fn connect(drv: &'static HalNetDriver, epoch: u32, listen_only: bool) {
    let finished = |result: i32| {
        if let Ok(mut c) = COORD.lock() {
            if let Some(c) = c.as_mut() {
                c.cell.connect_finished(epoch, result, Instant::now());
            }
        }
    };
    if (listen_only && drv.listen.is_none()) || (!listen_only && drv.connect.is_none()) {
        finished(ESP_ERR_NOT_SUPPORTED);
        return;
    }
    CELL_CONNECTING.store(true, Ordering::SeqCst);
    if let Ok(mut c) = COORD.lock() {
        if let Some(c) = c.as_mut() {
            c.blocked_info = None;
        }
    }
    let apn = apn_in(read_system_json().as_deref());
    let c_apn = std::ffi::CString::new(apn).unwrap_or_default();
    // Switched off since this attempt was decided: the driver clears a cancel
    // when an attempt starts, so one sent before then is seen only here.
    let still_wanted = match want() {
        Want::Off => false,
        Want::Listen => listen_only,
        Want::Data => true,
    };
    if !still_wanted {
        finished(ESP_ERR_INVALID_STATE);
        CELL_CONNECTING.store(false, Ordering::SeqCst);
        return;
    }
    let ret = if listen_only {
        log!(ESP_LOG_INFO, b"cellular: registering\0");
        let ret = unsafe { drv.listen.map_or(ESP_ERR_NOT_SUPPORTED, |f| f(CELL_CONNECT_TIMEOUT_MS)) };
        if ret == ESP_OK {
            log!(ESP_LOG_INFO, b"cellular: registered\0");
        } else {
            log!(ESP_LOG_WARN, b"cellular: registering failed (0x%x)\0", ret);
        }
        ret
    } else {
        log!(ESP_LOG_INFO, b"mobile data: connecting\0");
        let ret = unsafe {
            drv.connect.map_or(ESP_ERR_NOT_SUPPORTED, |f| {
                f(c_apn.as_ptr(), std::ptr::null(), CELL_CONNECT_TIMEOUT_MS)
            })
        };
        if ret == ESP_OK {
            log!(ESP_LOG_INFO, b"mobile data: connected\0");
        } else {
            log!(ESP_LOG_WARN, b"mobile data: attempt failed (0x%x)\0", ret);
        }
        ret
    };
    // A SIM that is missing or locked leaves the modem nothing to do.
    // What it said is kept for Settings: powering down forgets it.
    let blocked_info = (ret == ESP_ERR_NOT_FOUND).then(|| {
        let mut info = cell_info(drv);
        info.powered = false;
        if let Some(disconnect) = drv.disconnect {
            unsafe { disconnect() };
        }
        info
    });
    if let Ok(mut c) = COORD.lock() {
        if let Some(c) = c.as_mut() {
            c.cell.connect_finished(epoch, ret, Instant::now());
            if blocked_info.is_some() {
                c.cell.power_off_finished();
                if c.cell.blocked() {
                    c.blocked_info = blocked_info;
                }
            }
        }
    }
    CELL_CONNECTING.store(false, Ordering::SeqCst);
}

fn power_off(drv: &'static HalNetDriver) {
    CELL_POWERING_OFF.store(true, Ordering::SeqCst);
    if let Some(disconnect) = drv.disconnect {
        unsafe { disconnect() };
    }
    if let Ok(mut c) = COORD.lock() {
        if let Some(c) = c.as_mut() {
            c.cell.power_off_finished();
        }
    }
    log!(ESP_LOG_INFO, b"mobile data: modem off\0");
    CELL_POWERING_OFF.store(false, Ordering::SeqCst);
}

/// Join the saved WiFi network. Takes up to the WiFi manager's 10 s timeout;
/// traffic moves between links by esp_netif without it.
fn rejoin_wifi() {
    // A system.json caught mid-rewrite does not parse; that is a reason to
    // look again later, not to stop looking.
    let ret = match crate::wifi_manager::saved_network_enabled(read_system_json().as_deref()) {
        Some(true) => unsafe { crate::wifi_manager::wifi_manager_auto_connect() },
        Some(false) => ESP_ERR_NOT_FOUND,
        None => ESP_ERR_INVALID_STATE,
    };
    if let Ok(mut c) = COORD.lock() {
        if let Some(c) = c.as_mut() {
            c.wifi.rejoin_finished(ret, Instant::now());
        }
    }
}

/// One look at everything: the work to do next. Powering the modem down
/// comes first, then fetching SMS and rejoining WiFi, which are quick, and
/// then a cellular attempt, which can take two minutes; the attempt is
/// decided again once the rest is done.
fn evaluate(c: &mut Coordinator) -> Work {
    let now = Instant::now();

    // Sleep comes before anything the settings ask for, and while the device
    // sleeps nothing else is done.
    let wanted = Rest::from_u8(REST_WANTED.load(Ordering::SeqCst));
    let reached = Rest::from_u8(REST_REACHED.load(Ordering::SeqCst));
    if wanted != reached {
        return Work::Rest(reached, wanted);
    }
    if wanted != Rest::Normal {
        return Work::None;
    }

    let mut cell = Work::None;
    if let Some(drv) = cellular() {
        match c.cell.decide(
            want(),
            modem_ready(drv),
            cell_state(drv),
            CELL_CONNECTING.load(Ordering::SeqCst),
            CELL_POWERING_OFF.load(Ordering::SeqCst),
            now,
        ) {
            CellAction::PowerOff => return Work::PowerOff(drv),
            CellAction::Listen => cell = Work::Listen(drv, c.cell.epoch()),
            CellAction::Connect => cell = Work::Connect(drv, c.cell.epoch()),
            CellAction::None => {}
        }
        if !c.cell.blocked() {
            c.blocked_info = None;
        }
        if crate::sms_manager::due(drv) {
            return Work::FetchSms(drv);
        }
    }

    let wifi_state = crate::wifi_manager::wifi_manager_get_state();
    if c.wifi.decide(wifi_state, crate::wifi_manager::radio_on(), now) {
        return Work::RejoinWifi;
    }
    cell
}

fn run() {
    loop {
        let mut guard = match COORD.lock() {
            Ok(g) => g,
            Err(_) => return,
        };
        let work = match guard.as_mut() {
            Some(c) => {
                c.poke = false;
                evaluate(c)
            }
            None => Work::None,
        };
        // The lock is not held for the work: Settings reads the status
        // meanwhile.
        match work {
            Work::None => {}
            Work::PowerOff(drv) => {
                drop(guard);
                power_off(drv);
                continue;
            }
            Work::RejoinWifi => {
                drop(guard);
                rejoin_wifi();
                continue;
            }
            Work::Connect(drv, epoch) => {
                drop(guard);
                connect(drv, epoch, false);
                continue;
            }
            Work::Listen(drv, epoch) => {
                drop(guard);
                connect(drv, epoch, true);
                continue;
            }
            Work::FetchSms(drv) => {
                drop(guard);
                crate::sms_manager::fetch(drv);
                continue;
            }
            Work::Rest(from, to) => {
                drop(guard);
                change_rest(from, to);
                continue;
            }
        }
        let (mut guard, _) = WAKE
            .wait_timeout_while(guard, TICK, |c| c.as_ref().map_or(false, |c| !c.poke))
            .unwrap_or_else(|poison| poison.into_inner());
        if let Some(c) = guard.as_mut() {
            c.poke = false;
        }
    }
}

/// Start keeping the network up. kernel_init() calls this once WiFi is
/// initialised and has made its first attempt at the saved network.
pub(crate) fn start() {
    {
        let mut guard = match COORD.lock() {
            Ok(g) => g,
            Err(_) => return,
        };
        if guard.is_some() {
            return;
        }
        *guard = Some(Coordinator {
            cell: CellPolicy::default(),
            wifi: WifiPolicy::default(),
            blocked_info: None,
            poke: false,
        });
    }
    let json = read_system_json();
    CELL_WANTED.store(enabled_in(json.as_deref()), Ordering::SeqCst);
    SMS_WANTED.store(sms_in(json.as_deref()), Ordering::SeqCst);
    if cellular().is_some() {
        let on_off = |on: bool| if on { b"on\0".as_ptr() } else { b"off\0".as_ptr() };
        log!(
            ESP_LOG_INFO,
            b"mobile data %s, SMS %s\0",
            on_off(CELL_WANTED.load(Ordering::SeqCst)),
            on_off(SMS_WANTED.load(Ordering::SeqCst))
        );
    }
    #[cfg(target_os = "espidf")]
    unsafe {
        thistle_pthread_name_next(b"net_coord\0".as_ptr())
    };
    let started = std::thread::Builder::new()
        .name("net_coord".to_string())
        .stack_size(STACK_SIZE)
        .spawn(run)
        .is_ok();
    #[cfg(target_os = "espidf")]
    unsafe {
        thistle_pthread_name_next(std::ptr::null())
    };
    if !started {
        log!(ESP_LOG_WARN, b"could not start the network coordinator\0");
    }
}

// ---------------------------------------------------------------------------
// FFI — thistle/net_manager.h
// ---------------------------------------------------------------------------

/// net_cellular_status_t
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct CCellularStatus {
    pub present: bool,
    pub enabled: bool,
    /// hal_net_state_t of the data session
    pub data: u32,
    pub sim_blocked: bool,
    pub info: CCellInfo,
    /// SMS reception switched on
    pub sms: bool,
}

/// Whether the board has a cellular modem.
#[no_mangle]
pub extern "C" fn net_cellular_present() -> bool {
    cellular().is_some()
}

/// Switch mobile data on or off, and remember the choice.
///
/// Returns at once: the modem is started or stopped in the background. The
/// choice applies even if it could not be saved, which the return says.
#[no_mangle]
pub extern "C" fn net_cellular_set_enabled(enabled: bool) -> i32 {
    if cellular().is_none() {
        return ESP_ERR_NOT_SUPPORTED;
    }
    CELL_WANTED.store(enabled, Ordering::SeqCst);
    // An attempt under way runs on the coordinator's thread, which cannot
    // look again until it ends; the driver can end it early.
    if !enabled {
        cancel_attempt();
    }
    poke();
    crate::system_json::save(SECTION, "enabled", serde_json::Value::Bool(enabled))
}

fn cancel_attempt() {
    if CELL_CONNECTING.load(Ordering::SeqCst) {
        if let Some(cancel) = cellular().and_then(|drv| drv.cancel) {
            unsafe { cancel() };
        }
    }
}

/// Switch receiving SMS on or off, and remember the choice. With mobile data
/// off, this decides whether the modem is on at all. Returns as
/// net_cellular_set_enabled() does.
#[no_mangle]
pub extern "C" fn net_cellular_set_sms(enabled: bool) -> i32 {
    if cellular().is_none() {
        return ESP_ERR_NOT_SUPPORTED;
    }
    SMS_WANTED.store(enabled, Ordering::SeqCst);
    if want() == Want::Off {
        cancel_attempt();
    }
    poke();
    crate::system_json::save(SECTION, "sms", serde_json::Value::Bool(enabled))
}

/// Mobile data's state, for Settings.
///
/// # Safety
/// `out` must point to a writable net_cellular_status_t.
#[no_mangle]
pub unsafe extern "C" fn net_cellular_get_status(out: *mut CCellularStatus) -> i32 {
    if out.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let mut status = CCellularStatus {
        enabled: CELL_WANTED.load(Ordering::SeqCst),
        sms: SMS_WANTED.load(Ordering::SeqCst),
        ..Default::default()
    };
    if let Some(drv) = cellular() {
        status.present = true;
        status.data = if CELL_CONNECTING.load(Ordering::SeqCst) {
            NET_CONNECTING
        } else {
            cell_state(drv)
        };
        status.info = cell_info(drv);
        if let Ok(c) = COORD.lock() {
            if let Some(c) = c.as_ref() {
                status.sim_blocked = c.cell.blocked();
                if let Some(info) = c.blocked_info {
                    status.info = info;
                }
            }
        }
    }
    *out = status;
    ESP_OK
}

/// Copy the saved APN, "" for the network's own, into `buf`.
///
/// # Safety
/// `buf` must point to `len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn net_cellular_get_apn(buf: *mut c_char, len: usize) -> i32 {
    if buf.is_null() || len == 0 {
        return ESP_ERR_INVALID_ARG;
    }
    let apn = apn_in(read_system_json().as_deref());
    let n = apn.len().min(len - 1);
    std::ptr::copy_nonoverlapping(apn.as_ptr(), buf as *mut u8, n);
    *buf.add(n) = 0;
    ESP_OK
}

/// Save the APN to dial, "" or NULL for the one the network assigns. A
/// connected modem is restarted to use it.
///
/// # Safety
/// `apn` must be NULL or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn net_cellular_set_apn(apn: *const c_char) -> i32 {
    let apn = if apn.is_null() {
        ""
    } else {
        match std::ffi::CStr::from_ptr(apn).to_str() {
            Ok(s) => s.trim(),
            Err(_) => return ESP_ERR_INVALID_ARG,
        }
    };
    if !valid_apn(apn) {
        return ESP_ERR_INVALID_ARG;
    }
    let ret = crate::system_json::save(SECTION, "apn", serde_json::Value::String(apn.into()));
    if ret != ESP_OK {
        return ret;
    }
    if want() == Want::Data {
        match COORD.lock() {
            Ok(mut c) => match c.as_mut() {
                Some(c) => c.cell.request_cycle(),
                None => return ESP_ERR_INVALID_STATE,
            },
            Err(_) => return ESP_ERR_INVALID_STATE,
        }
        poke();
    }
    ESP_OK
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    const ESP_ERR_TIMEOUT: i32 = 0x107;

    fn t0() -> Instant {
        Instant::now()
    }

    #[test]
    fn mobile_data_is_off_unless_switched_on() {
        assert!(!enabled_in(None));
        assert!(!enabled_in(Some(r#"{ "thistle_os": {} }"#)));
        assert!(!enabled_in(Some(r#"{ "thistle_os": { "cellular": { "enabled": "yes" } } }"#)));
        assert!(enabled_in(Some(r#"{ "thistle_os": { "cellular": { "enabled": true } } }"#)));
    }

    #[test]
    fn sms_are_received_unless_switched_off() {
        assert!(sms_in(None));
        assert!(sms_in(Some(r#"{ "thistle_os": { "cellular": { "enabled": true } } }"#)));
        assert!(!sms_in(Some(r#"{ "thistle_os": { "cellular": { "sms": false } } }"#)));
    }

    #[test]
    fn an_apn_is_read_only_if_it_could_be_sent() {
        assert_eq!(apn_in(None), "");
        let json = |apn: &str| {
            serde_json::json!({ "thistle_os": { "cellular": { "apn": apn } } }).to_string()
        };
        assert_eq!(apn_in(Some(&json("online.telia.se"))), "online.telia.se");
        assert_eq!(apn_in(Some(&json("internet"))), "internet");
        // Would break out of AT+CGDCONT=1,"IP","<apn>".
        assert_eq!(apn_in(Some(&json("x\",\"IP\",\"y"))), "");
        assert_eq!(apn_in(Some(&json("a\r\nAT+CFUN=0"))), "");
    }

    #[test]
    fn apn_grammar() {
        assert!(valid_apn(""));
        assert!(valid_apn("web.vodafone.de"));
        assert!(valid_apn("m2m_iot-1"));
        assert!(!valid_apn("has space"));
        assert!(!valid_apn("quote\""));
        assert!(!valid_apn(&"a".repeat(APN_MAX_LEN + 1)));
    }

    #[test]
    fn backoff_grows_then_holds() {
        assert_eq!(backoff(1), Duration::from_secs(10));
        assert_eq!(backoff(2), Duration::from_secs(30));
        assert_eq!(backoff(4), Duration::from_secs(120));
        assert_eq!(backoff(40), Duration::from_secs(120));
    }

    #[test]
    fn at_boot_with_mobile_data_off_the_modem_is_powered_down_once() {
        let mut p = CellPolicy::default();
        let now = t0();
        assert_eq!(p.decide(Want::Off, false, NET_DISCONNECTED, false, false, now), CellAction::PowerOff);
        // Under way.
        assert_eq!(p.decide(Want::Off, false, NET_DISCONNECTED, false, true, now), CellAction::None);
        p.power_off_finished();
        assert_eq!(p.decide(Want::Off, false, NET_DISCONNECTED, false, false, now), CellAction::None);
    }

    #[test]
    fn switched_on_it_connects_and_stays_while_connected() {
        let mut p = CellPolicy::default();
        let now = t0();
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now), CellAction::Connect);
        assert_eq!(p.decide(Want::Data, true, NET_CONNECTING, true, false, now), CellAction::None);
        p.connect_finished(p.epoch(), ESP_OK, now);
        assert_eq!(p.decide(Want::Data, true, NET_CONNECTED, false, false, now), CellAction::None);
    }

    #[test]
    fn a_session_that_drops_is_redialled_at_once() {
        let mut p = CellPolicy::default();
        let now = t0();
        p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now);
        p.connect_finished(p.epoch(), ESP_OK, now);
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now), CellAction::Connect);
    }

    #[test]
    fn a_failed_attempt_waits_before_the_next() {
        let mut p = CellPolicy::default();
        let now = t0();
        p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now);
        p.connect_finished(p.epoch(), ESP_ERR_TIMEOUT, now);
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now), CellAction::None);
        let later = now + Duration::from_secs(9);
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, later), CellAction::None);
        let later = now + Duration::from_secs(10);
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, later), CellAction::Connect);
    }

    #[test]
    fn a_missing_sim_waits_for_mobile_data_to_be_switched_off_and_on() {
        let mut p = CellPolicy::default();
        let now = t0();
        p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now);
        p.connect_finished(p.epoch(), ESP_ERR_NOT_FOUND, now);
        // The attempt powers the modem down itself.
        p.power_off_finished();
        assert!(p.blocked());
        let much_later = now + Duration::from_secs(3600);
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, much_later), CellAction::None);

        // Off, with the modem already down, then on again.
        assert_eq!(p.decide(Want::Off, false, NET_DISCONNECTED, false, false, much_later), CellAction::None);
        assert!(!p.blocked());
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, much_later), CellAction::Connect);
    }

    #[test]
    fn switching_off_mid_attempt_powers_down_at_once() {
        let mut p = CellPolicy::default();
        let now = t0();
        p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now);
        assert_eq!(p.decide(Want::Off, false, NET_CONNECTING, true, false, now), CellAction::PowerOff);
    }

    #[test]
    fn switching_on_while_powering_down_waits_for_it() {
        let mut p = CellPolicy::default();
        let now = t0();
        assert_eq!(p.decide(Want::Off, false, NET_CONNECTED, false, false, now), CellAction::PowerOff);
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, true, now), CellAction::None);
        p.power_off_finished();
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now), CellAction::Connect);
    }

    #[test]
    fn a_new_apn_restarts_a_connected_modem() {
        let mut p = CellPolicy::default();
        let now = t0();
        p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now);
        p.connect_finished(p.epoch(), ESP_OK, now);
        p.request_cycle();
        assert_eq!(p.decide(Want::Data, true, NET_CONNECTED, false, false, now), CellAction::PowerOff);
        p.power_off_finished();
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now), CellAction::Connect);
    }

    #[test]
    fn an_attempt_that_finishes_after_mobile_data_was_switched_off_is_undone() {
        let mut p = CellPolicy::default();
        let now = t0();
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now), CellAction::Connect);
        let epoch = p.epoch();
        // Switched off before the attempt got going; the power-down ran first.
        assert_eq!(p.decide(Want::Off, false, NET_DISCONNECTED, true, false, now), CellAction::PowerOff);
        p.power_off_finished();
        assert_eq!(p.decide(Want::Off, false, NET_DISCONNECTED, true, false, now), CellAction::None);
        // Then the attempt connected anyway.
        p.connect_finished(epoch, ESP_OK, now);
        assert_eq!(p.decide(Want::Off, false, NET_CONNECTED, false, false, now), CellAction::PowerOff);
    }

    #[test]
    fn switching_off_and_on_during_a_failing_attempt_starts_afresh() {
        let mut p = CellPolicy::default();
        let now = t0();
        p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now);
        let epoch = p.epoch();
        p.decide(Want::Off, false, NET_CONNECTING, true, false, now);
        p.decide(Want::Data, true, NET_CONNECTING, true, false, now);
        // The old attempt finds no SIM, and powers the modem down.
        p.connect_finished(epoch, ESP_ERR_NOT_FOUND, now);
        p.power_off_finished();
        assert!(!p.blocked());
        // No backoff either: the switch cut the attempt short.
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now), CellAction::Connect);
    }

    #[test]
    fn with_only_sms_on_the_modem_registers_and_dials_nothing() {
        let mut p = CellPolicy::default();
        let now = t0();
        assert_eq!(p.decide(Want::Listen, false, NET_DISCONNECTED, false, false, now), CellAction::Listen);
        p.connect_finished(p.epoch(), ESP_OK, now);
        assert_eq!(p.decide(Want::Listen, true, NET_DISCONNECTED, false, false, now), CellAction::None);
        // Powered down behind its back, say by a reset: it is started again.
        assert_eq!(p.decide(Want::Listen, false, NET_DISCONNECTED, false, false, now), CellAction::Listen);
    }

    #[test]
    fn mobile_data_registers_first_and_dials_after() {
        let mut p = CellPolicy::default();
        let now = t0();
        assert_eq!(p.decide(Want::Data, false, NET_DISCONNECTED, false, false, now), CellAction::Listen);
        p.connect_finished(p.epoch(), ESP_OK, now);
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now), CellAction::Connect);
        p.connect_finished(p.epoch(), ESP_OK, now);
        assert_eq!(p.decide(Want::Data, true, NET_CONNECTED, false, false, now), CellAction::None);
    }

    #[test]
    fn mobile_data_switched_off_with_sms_on_restarts_the_modem_without_data() {
        let mut p = CellPolicy::default();
        let now = t0();
        p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now);
        p.connect_finished(p.epoch(), ESP_OK, now);
        assert_eq!(p.decide(Want::Listen, true, NET_CONNECTED, false, false, now), CellAction::PowerOff);
        p.power_off_finished();
        assert_eq!(p.decide(Want::Listen, false, NET_DISCONNECTED, false, false, now), CellAction::Listen);
    }

    #[test]
    fn sms_switched_on_with_mobile_data_on_changes_nothing() {
        let mut p = CellPolicy::default();
        let now = t0();
        p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now);
        p.connect_finished(p.epoch(), ESP_OK, now);
        assert_eq!(p.decide(Want::Data, true, NET_CONNECTED, false, false, now), CellAction::None);
    }

    #[test]
    fn both_switched_off_powers_the_modem_down() {
        let mut p = CellPolicy::default();
        let now = t0();
        p.decide(Want::Listen, false, NET_DISCONNECTED, false, false, now);
        p.connect_finished(p.epoch(), ESP_OK, now);
        assert_eq!(p.decide(Want::Off, true, NET_DISCONNECTED, false, false, now), CellAction::PowerOff);
    }

    #[test]
    fn a_missing_sim_holds_until_a_switch_moves() {
        let mut p = CellPolicy::default();
        let now = t0();
        p.decide(Want::Listen, false, NET_DISCONNECTED, false, false, now);
        p.connect_finished(p.epoch(), ESP_ERR_NOT_FOUND, now);
        p.power_off_finished();
        let much_later = now + Duration::from_secs(3600);
        assert_eq!(p.decide(Want::Listen, false, NET_DISCONNECTED, false, false, much_later), CellAction::None);
        // Mobile data switched on: tried again, registering first.
        assert_eq!(p.decide(Want::Data, false, NET_DISCONNECTED, false, false, much_later), CellAction::Listen);
    }

    #[test]
    fn a_failed_registration_waits_before_the_next() {
        let mut p = CellPolicy::default();
        let now = t0();
        p.decide(Want::Listen, false, NET_DISCONNECTED, false, false, now);
        p.connect_finished(p.epoch(), ESP_ERR_TIMEOUT, now);
        assert_eq!(p.decide(Want::Listen, false, NET_DISCONNECTED, false, false, now), CellAction::None);
        let later = now + Duration::from_secs(10);
        assert_eq!(p.decide(Want::Listen, false, NET_DISCONNECTED, false, false, later), CellAction::Listen);
    }

    #[test]
    fn wifi_is_rejoined_after_a_drop_with_backoff() {
        let mut w = WifiPolicy::default();
        let now = t0();
        assert!(!w.decide(WIFI_CONNECTED, true, now));
        assert!(w.decide(WIFI_DISCONNECTED, true, now));
        // Someone else is connecting.
        assert!(!w.decide(WIFI_CONNECTING, true, now));

        w.rejoin_finished(ESP_ERR_TIMEOUT, now);
        assert!(!w.decide(WIFI_FAILED, true, now + Duration::from_secs(5)));
        assert!(w.decide(WIFI_FAILED, true, now + Duration::from_secs(10)));
    }

    #[test]
    fn wifi_with_nothing_saved_is_left_alone_until_it_connects() {
        let mut w = WifiPolicy::default();
        let now = t0();
        w.rejoin_finished(ESP_ERR_NOT_FOUND, now);
        let much_later = now + Duration::from_secs(3600);
        assert!(!w.decide(WIFI_DISCONNECTED, true, much_later));
        // The user joined a network in Settings, which saves it.
        assert!(!w.decide(WIFI_CONNECTED, true, much_later));
        assert!(w.decide(WIFI_DISCONNECTED, true, much_later));
    }

    #[test]
    fn wifi_switched_off_is_left_off_and_switched_on_is_rejoined_at_once() {
        let mut w = WifiPolicy::default();
        let now = t0();
        w.rejoin_finished(ESP_ERR_TIMEOUT, now);
        w.rejoin_finished(ESP_ERR_TIMEOUT, now);
        assert!(!w.decide(WIFI_DISCONNECTED, false, now + Duration::from_secs(3600)));
        // No backoff left over from before.
        assert!(w.decide(WIFI_DISCONNECTED, true, now));

        // Nothing saved: switching off and on tries again, once.
        w.rejoin_finished(ESP_ERR_NOT_FOUND, now);
        assert!(!w.decide(WIFI_DISCONNECTED, true, now));
        w.decide(WIFI_DISCONNECTED, false, now);
        assert!(w.decide(WIFI_DISCONNECTED, true, now));
    }

    #[test]
    fn an_attempt_that_cannot_start_waits_like_one_that_failed() {
        let mut p = CellPolicy::default();
        let now = t0();
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now), CellAction::Connect);
        p.connect_finished(p.epoch(), ESP_ERR_NO_MEM, now);
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, now), CellAction::None);
        let later = now + Duration::from_secs(10);
        assert_eq!(p.decide(Want::Data, true, NET_DISCONNECTED, false, false, later), CellAction::Connect);
    }

    #[test]
    fn the_status_struct_matches_the_c_layout() {
        // bool, bool, pad, u32, bool, pad, hal_cell_info_t (align 4, 40 bytes).
        assert_eq!(std::mem::offset_of!(CCellularStatus, data), 4);
        assert_eq!(std::mem::offset_of!(CCellularStatus, sim_blocked), 8);
        assert_eq!(std::mem::offset_of!(CCellularStatus, info), 12);
        assert_eq!(std::mem::offset_of!(CCellularStatus, sms), 52);
        assert_eq!(std::mem::size_of::<CCellularStatus>(), 56);
    }
}
