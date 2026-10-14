// SPDX-License-Identifier: BSD-3-Clause
//! The system clock: kept in UTC, set from the best source the device has,
//! and shown in the zone the user chose.
//!
//! Sources, in the order they win (time_policy.rs decides):
//!
//! - **NTP**, over whatever network is up, WiFi or LTE. ESP-IDF's SNTP client
//!   asks `pool.ntp.org` (or `thistle_os.time.ntp_server`) once an hour, and
//!   at once whenever an interface gets an address. Its answer comes through
//!   `time_manager_ntp_synced`, so it is judged like the others.
//! - **GNSS**: the GPS receiver's time, while something has it on and it has
//!   a fix. Nothing turns the receiver on for the time alone.
//! - **The mobile network** (NITZ): the time the network set the modem's
//!   clock to, read whenever the modem answers commands anyway. It also gives
//!   the local offset, which an automatic zone follows.
//! - **A clock chip** (a PCF8563), on a board that has one: read at boot,
//!   written back whenever a better source has set the clock.
//!
//! Without a chip, as on the T-Deck Max, the time still survives a restart and
//! deep sleep: ESP-IDF keeps it in the RTC domain, which only a power-on
//! clears. `time_platform.c` records there what set the clock, and when.
//!
//! The zone is a POSIX rule in the TZ environment variable, so the C apps'
//! localtime() agrees with the status bar. Settings are in system.json under
//! `thistle_os.time`: `automatic` (default on), `zone` (an IANA name from
//! time_zones.rs, a POSIX rule, or "" to follow the mobile network, the
//! default), `format_24h` (default on) and `ntp_server`.

use std::ffi::{c_char, c_void, CStr};
use std::sync::Mutex;
use std::time::Duration;

use crate::time_policy::{self, Civil, Source, Sync, Verdict};

const ESP_OK: i32 = 0;
const ESP_FAIL: i32 = -1;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;

const SECTION: &str = "time";
const DEFAULT_NTP_SERVER: &str = "pool.ntp.org";

const US: i64 = 1_000_000;

/// How often GNSS and the mobile network are asked. Both only hand over what
/// their drivers already hold.
const TICK: Duration = Duration::from_secs(30);
/// How far a GNSS time may move against our own clock between two looks
/// and still be a receiver that is running, rather than one stuck on the
/// last time it had.
const GNSS_LIVE_SLACK_US: i64 = 5 * US;
/// How often a clock chip is rewritten while sources only confirm the time:
/// often enough that its drift, seconds a day, never shows.
const RTC_REWRITE_US: i64 = 6 * 3600 * US;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

#[cfg(not(test))]
static TAG: &[u8] = b"time\0";

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
// Settings
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Settings {
    pub automatic: bool,
    /// "" to follow the mobile network.
    pub zone: String,
    pub format_24h: bool,
    pub ntp_server: String,
}

pub(crate) fn settings_in(json: Option<&str>) -> Settings {
    let get = |key: &str| json.and_then(|j| crate::system_json::get(j, SECTION, key));
    let text = |key: &str| get(key).and_then(|v| v.as_str().map(|s| s.trim().to_string()));
    Settings {
        automatic: get("automatic").and_then(|v| v.as_bool()).unwrap_or(true),
        zone: text("zone").unwrap_or_default(),
        format_24h: get("format_24h").and_then(|v| v.as_bool()).unwrap_or(true),
        ntp_server: text("ntp_server")
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| DEFAULT_NTP_SERVER.to_string()),
    }
}

/// The POSIX rule a zone setting comes to: a zone from the table, a rule
/// written into system.json by hand, or, for "", the mobile network's
/// offset, UTC until it has given one.
pub(crate) fn zone_rule(setting: &str, network_offset: Option<i32>) -> String {
    if setting.is_empty() {
        return time_policy::posix_for_offset(network_offset.unwrap_or(0));
    }
    match crate::time_zones::posix_for(setting) {
        Some(rule) => rule.to_string(),
        None => setting.to_string(),
    }
}

/// The zone as Settings shows it: "Europe/Oslo", "Automatic (UTC+02:00)".
pub(crate) fn zone_label(setting: &str, network_offset: Option<i32>) -> String {
    if setting.is_empty() {
        let offset = time_policy::offset_label(network_offset.unwrap_or(0));
        return format!("Automatic ({offset})");
    }
    setting.replace('_', " ")
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct State {
    settings: Settings,
    last: Option<Sync>,
    /// The local offset the mobile network last gave, in minutes.
    network_offset: Option<i32>,
    /// The modem reading last offered, so each is offered once.
    network_offered: Option<i64>,
    /// The GNSS time seen at the last look, and our clock then.
    gnss_seen: Option<(i64, i64)>,
    /// The POSIX rule in force.
    tz: String,
    /// The clock chip should be written from the system clock.
    rtc_due: bool,
    rtc_written_us: Option<i64>,
}

static STATE: Mutex<Option<State>> = Mutex::new(None);

/// What the RTC domain keeps through deep sleep and restarts beside the
/// clock itself: what last set it, and the zone an automatic zone follows.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct Record {
    last: Option<Sync>,
    network_offset: Option<i32>,
}

impl Record {
    /// As time_platform.c keeps it: source, when, offset (i32::MIN for none).
    fn to_raw(self) -> (u8, i64, i32) {
        let (source, at_us) = self.last.map_or((Source::None, 0), |l| (l.source, l.at_us));
        (source as u8, at_us, self.network_offset.unwrap_or(i32::MIN))
    }

    fn from_raw(source: u8, at_us: i64, offset_min: i32) -> Record {
        Record {
            last: Source::from_u8(source)
                .filter(|&s| s != Source::None)
                .map(|source| Sync { source, at_us }),
            network_offset: Some(offset_min).filter(|&m| time_policy::offset_is_valid(m)),
        }
    }
}

fn save_record(state: &State) {
    platform::record_store(Record { last: state.last, network_offset: state.network_offset });
}

fn with<R>(f: impl FnOnce(&mut State) -> R) -> Option<R> {
    let mut guard = STATE.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
    guard.as_mut().map(f)
}

fn now_us() -> i64 {
    match std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH) {
        Ok(d) => d.as_micros().min(i64::MAX as u128) as i64,
        Err(_) => 0,
    }
}

/// Apply the zone the settings and the network come to. True if it changed.
fn apply_zone(state: &mut State) -> bool {
    let rule = zone_rule(&state.settings.zone, state.network_offset);
    if rule == state.tz {
        return false;
    }
    libc_time::apply_tz(&rule);
    state.tz = rule;
    true
}

/// The zone for the log, with the offset the C library makes of its rule
/// now: "Europe/Oslo, UTC+02:00 now". The offset shows that the rule was
/// read as meant.
fn zone_for_log(state: &State) -> std::ffi::CString {
    let label = zone_label(&state.settings.zone, state.network_offset);
    let offset = time_policy::offset_label(libc_time::offset_minutes(now_us() / US));
    std::ffi::CString::new(format!("{label}, {offset} now")).unwrap_or_default()
}

/// Act on a time from `source`. Returns what was done.
fn offer(source: Source, offered_us: i64) -> Verdict {
    let mut guard = STATE.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
    let Some(state) = guard.as_mut() else {
        return Verdict::Ignore;
    };
    let now = now_us();
    let verdict =
        time_policy::judge(state.last, state.settings.automatic, source, offered_us, now);
    let off_ms = ((offered_us - now) / 1000).clamp(i32::MIN as i64, i32::MAX as i64) as i32;
    if let Verdict::Step | Verdict::Slew = verdict {
        let ret = platform::set(offered_us, verdict == Verdict::Slew);
        if ret != ESP_OK {
            log!(ESP_LOG_WARN, b"could not set the clock: 0x%x\0", ret);
            return Verdict::Ignore;
        }
    }
    let by = source_name(source);
    match verdict {
        Verdict::Ignore => return verdict,
        Verdict::Step if !time_policy::plausible(now) => {
            log!(ESP_LOG_INFO, b"clock set by %s\0", by.as_ptr());
        }
        Verdict::Step | Verdict::Slew if off_ms != 0 => {
            let how: &[u8] = if verdict == Verdict::Slew { b"slewing\0" } else { b"stepped\0" };
            log!(ESP_LOG_INFO, b"clock %s by %d ms, from %s\0", how.as_ptr(), off_ms, by.as_ptr());
        }
        // Within a millisecond: worth a line only the first time.
        _ if state.last.map(|s| s.source) != Some(source) => {
            log!(ESP_LOG_INFO, b"clock confirmed by %s\0", by.as_ptr());
        }
        _ => {}
    }
    state.last = Some(Sync { source, at_us: offered_us });
    save_record(state);
    if source != Source::Rtc {
        let stale = state
            .rtc_written_us
            .map_or(true, |at| (now - at).abs() >= RTC_REWRITE_US);
        state.rtc_due |= verdict != Verdict::Confirm || stale;
    }
    verdict
}

fn source_name(source: Source) -> &'static CStr {
    match source {
        Source::Ntp => c"NTP",
        Source::Gnss => c"GNSS",
        Source::Network => c"the mobile network",
        Source::Rtc => c"the clock chip",
        Source::Manual => c"the user",
        Source::Kept | Source::None => c"nothing",
    }
}

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

/// The GPS receiver's time, if it is running with a fix and the time it
/// gives has moved on with our clock since the last look: a receiver whose
/// sentences stopped coming keeps its last time, and that is not now.
fn gnss_time(seen: &mut Option<(i64, i64)>) -> Option<i64> {
    let reg = crate::hal_registry::registry();
    let gps = unsafe { reg.gps.as_ref() }?;
    let get_position = gps.get_position?;
    let mut pos: crate::hal_registry::HalGpsPosition = unsafe { std::mem::zeroed() };
    let ret = unsafe { get_position(&mut pos) };
    if ret != ESP_OK || !pos.fix_valid || pos.timestamp == 0 {
        *seen = None;
        return None;
    }
    // $GNRMC names the second its fix is for, and arrives within a second
    // of it; half a second after is the best guess at when it was read.
    let now = (pos.timestamp as i64 * US + US / 2, monotonic_us());
    let live = seen.is_some_and(|then| gnss_is_live(then, now));
    *seen = Some(now);
    live.then_some(now.0)
}

/// Whether a GNSS time `now` (its value, our monotonic clock when seen) has
/// moved on from `then` by as much as our own clock has.
fn gnss_is_live(then: (i64, i64), now: (i64, i64)) -> bool {
    let moved = now.0 - then.0;
    moved > 0 && (moved - (now.1 - then.1)).abs() <= GNSS_LIVE_SLACK_US
}

fn monotonic_us() -> i64 {
    static ORIGIN: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
    ORIGIN.get_or_init(std::time::Instant::now).elapsed().as_micros() as i64
}

/// The modem's last reading of the network's time.
struct NetworkReading {
    /// The reading itself, seconds: the same until the modem is read again.
    read: i64,
    /// What it makes the time now.
    now_us: i64,
    /// The local offset, in minutes, if the network gave one.
    offset: Option<i32>,
}

fn network_time() -> Option<NetworkReading> {
    let drv = crate::net_manager::find_transport(crate::net_manager::HAL_NET_CELLULAR);
    let drv = unsafe { drv.as_ref() }?;
    let get = drv.get_network_time?;
    let mut t = crate::net_manager::CNetTime::default();
    if unsafe { get(&mut t) } != ESP_OK {
        return None;
    }
    let offset = Some(t.utc_offset_min as i32)
        .filter(|&m| t.offset_known && time_policy::offset_is_valid(m));
    Some(NetworkReading {
        read: t.utc,
        // Whole seconds, as the modem's clock counts them.
        now_us: t.utc * US + t.age_ms as i64 * 1000 + US / 2,
        offset,
    })
}

/// The clock chip's time, if there is a chip and it kept its time.
fn rtc_time() -> Option<i64> {
    let reg = crate::hal_registry::registry();
    let rtc = unsafe { reg.rtc.as_ref() }?;
    if let Some(is_valid) = rtc.is_valid {
        if !unsafe { is_valid() } {
            log!(ESP_LOG_WARN, b"the clock chip lost its time\0");
            return None;
        }
    }
    let get_time = rtc.get_time?;
    let mut dt = crate::hal_registry::HalDateTime {
        year: 0, month: 0, day: 0, weekday: 0, hour: 0, minute: 0, second: 0,
    };
    if unsafe { get_time(&mut dt) } != ESP_OK {
        return None;
    }
    let t = Civil::new(
        dt.year as i64, dt.month as u32, dt.day as u32,
        dt.hour as u32, dt.minute as u32, dt.second as u32,
    )?;
    // It counts whole seconds; the time is somewhere in the one it shows.
    Some(t.as_utc_seconds() * US + US / 2)
}

fn has_rtc() -> bool {
    !crate::hal_registry::registry().rtc.is_null()
}

/// Write the system clock to the chip, if a source has set it since.
fn rtc_write_if_due() {
    let due = with(|s| s.rtc_due).unwrap_or(false);
    if !due {
        return;
    }
    let reg = crate::hal_registry::registry();
    let Some(set_time) = unsafe { reg.rtc.as_ref() }.and_then(|rtc| rtc.set_time) else {
        with(|s| s.rtc_due = false);
        return;
    };
    // During a slew the system clock is still catching up.
    let now = now_us() + platform::pending_slew_us();
    if !time_policy::plausible(now) {
        return;
    }
    let t = Civil::from_utc_seconds((now + US / 2).div_euclid(US));
    let dt = crate::hal_registry::HalDateTime {
        year: t.year as u16,
        month: t.month as u8,
        day: t.day as u8,
        weekday: t.weekday() as u8,
        hour: t.hour as u8,
        minute: t.minute as u8,
        second: t.second as u8,
    };
    let ret = unsafe { set_time(&dt) };
    if ret != ESP_OK {
        log!(ESP_LOG_WARN, b"could not write the clock chip: 0x%x\0", ret);
        return;
    }
    with(|s| {
        s.rtc_due = false;
        s.rtc_written_us = Some(now);
    });
}

/// The kernel worker's look at GNSS and the mobile network.
unsafe extern "C" fn tick(_arg: *mut c_void) {
    let Some((automatic, mut seen, offered_before)) =
        with(|s| (s.settings.automatic, s.gnss_seen, s.network_offered))
    else {
        return;
    };

    let gnss = gnss_time(&mut seen);
    let network = network_time();
    with(|s| {
        s.gnss_seen = seen;
        if let Some(offset) = network.as_ref().and_then(|n| n.offset) {
            if s.network_offset != Some(offset) {
                s.network_offset = Some(offset);
                save_record(s);
                if apply_zone(s) {
                    let zone = zone_for_log(s);
                    log!(ESP_LOG_INFO, b"zone %s, from the mobile network\0", zone.as_ptr());
                }
            }
        }
    });

    if automatic {
        if let Some(t) = gnss {
            offer(Source::Gnss, t);
        }
        // Each reading once: a later look adds nothing but the time since.
        if let Some(n) = network.filter(|n| offered_before != Some(n.read)) {
            with(|s| s.network_offered = Some(n.read));
            offer(Source::Network, n.now_us);
        }
    }
    rtc_write_if_due();
}

unsafe extern "C" fn rtc_job(_arg: *mut c_void) {
    rtc_write_if_due();
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

/// Read the settings, take the time from the clock chip if the system clock
/// lost it, start NTP, and look at the other sources from now on. Called
/// once by kernel_init(), after the drivers are up and esp_netif is.
pub(crate) fn start() {
    let json = std::fs::read_to_string(crate::system_json::path()).ok();
    let settings = settings_in(json.as_deref());
    let automatic = settings.automatic;
    let server = settings.ntp_server.clone();
    {
        let mut guard = STATE.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        if guard.is_some() {
            return;
        }
        let now = now_us();
        let record = platform::record_load().unwrap_or_default();
        let last = record
            .last
            .filter(|r| time_policy::plausible(now) && r.at_us <= now + 60 * US);
        let mut state = State {
            settings,
            last,
            // Until the modem is read again, the zone it last gave.
            network_offset: record.network_offset,
            network_offered: None,
            gnss_seen: None,
            tz: String::new(),
            rtc_due: false,
            rtc_written_us: None,
        };
        apply_zone(&mut state);
        *guard = Some(state);
    }

    if let Some(t) = rtc_time() {
        offer(Source::Rtc, t);
    }

    let now = now_us();
    let (zone, source) =
        with(|s| (zone_for_log(s), s.last.map(|l| l.source))).unwrap_or_default();
    if time_policy::plausible(now) {
        let t = Civil::from_utc_seconds(now / US);
        let from = source_name(source.unwrap_or(Source::Kept));
        let from = if source.is_none() { c"before the restart" } else { from };
        log!(
            ESP_LOG_INFO,
            b"%04d-%02d-%02d %02d:%02d:%02d UTC, from %s; zone %s\0",
            t.year as i32, t.month as i32, t.day as i32,
            t.hour as i32, t.minute as i32, t.second as i32,
            from.as_ptr(), zone.as_ptr()
        );
    } else {
        log!(ESP_LOG_INFO, b"clock not set yet; zone %s\0", zone.as_ptr());
    }

    if automatic {
        platform::sntp_start(&server);
    }
    crate::kworker::every(tick, std::ptr::null_mut(), TICK);
}

/// Called by ESP-IDF's SNTP client (time_platform.c), on the TCP/IP thread,
/// with each answer from the time server.
#[no_mangle]
pub extern "C" fn time_manager_ntp_synced(seconds: i64, microseconds: i32) {
    offer(Source::Ntp, seconds * US + microseconds as i64);
}

// ---------------------------------------------------------------------------
// The C API (thistle/time_manager.h)
// ---------------------------------------------------------------------------

/// time_status_t
#[repr(C)]
pub struct CTimeStatus {
    pub known: bool,
    pub source: u32,
    pub synced_at: i64,
    pub automatic: bool,
    pub format_24h: bool,
    pub has_rtc: bool,
    pub zone: [c_char; 48],
}

fn copy_out(text: &str, buf: *mut c_char, len: usize) {
    if buf.is_null() || len == 0 {
        return;
    }
    let mut n = text.len().min(len - 1);
    while !text.is_char_boundary(n) {
        n -= 1;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(text.as_ptr(), buf as *mut u8, n);
        *buf.add(n) = 0;
    }
}

fn c_str<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(p) }.to_str().ok()
}

/// # Safety
/// `out` must be null or point to a writable `time_status_t`.
#[no_mangle]
pub unsafe extern "C" fn time_manager_get_status(out: *mut CTimeStatus) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else {
        return ESP_ERR_INVALID_ARG;
    };
    let now = now_us();
    let Some((settings, last)) = with(|s| (s.settings.clone(), s.last)) else {
        return ESP_ERR_INVALID_STATE;
    };
    out.known = time_policy::plausible(now);
    out.source = match (out.known, last) {
        (false, _) => Source::None as u32,
        (true, Some(sync)) => sync.source as u32,
        (true, None) => Source::Kept as u32,
    };
    out.synced_at = match last {
        Some(sync) if out.known => sync.at_us / US,
        _ => 0,
    };
    out.automatic = settings.automatic;
    out.format_24h = settings.format_24h;
    out.has_rtc = has_rtc();
    let zone = settings.zone.as_bytes();
    out.zone = [0; 48];
    for (dst, &b) in out.zone.iter_mut().zip(zone.iter().take(47)) {
        *dst = b as c_char;
    }
    ESP_OK
}

/// What set the clock, and when: "Set by NTP, 5 min ago".
///
/// # Safety
/// `buf` must be null or hold `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn time_manager_describe(buf: *mut c_char, len: usize) {
    let now = now_us();
    let last = with(|s| s.last).flatten();
    copy_out(&time_policy::describe(time_policy::plausible(now), last, now), buf, len);
}

/// The zone in force, as Settings shows it.
///
/// # Safety
/// `buf` must be null or hold `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn time_manager_zone_label(buf: *mut c_char, len: usize) {
    let label = with(|s| zone_label(&s.settings.zone, s.network_offset)).unwrap_or_default();
    copy_out(&label, buf, len);
}

/// The time of day, "14:05" or "2:05 PM", or "--:--" before the clock has
/// been set.
///
/// # Safety
/// `buf` must be null or hold `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn time_manager_format_time(buf: *mut c_char, len: usize) {
    let now = now_us();
    let face = match libc_time::local(now / US).filter(|_| time_policy::plausible(now)) {
        Some(t) => {
            let h24 = with(|s| s.settings.format_24h).unwrap_or(true);
            time_policy::clock_face(t.hour, t.minute, h24)
        }
        None => "--:--".to_string(),
    };
    copy_out(&face, buf, len);
}

/// The date, "2026-10-13", or "----/--/--" before the clock has been set.
///
/// # Safety
/// `buf` must be null or hold `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn time_manager_format_date(buf: *mut c_char, len: usize) {
    let now = now_us();
    let date = match libc_time::local(now / US).filter(|_| time_policy::plausible(now)) {
        Some(t) => format!("{:04}-{:02}-{:02}", t.year, t.month, t.day),
        None => "----/--/--".to_string(),
    };
    copy_out(&date, buf, len);
}

#[no_mangle]
pub extern "C" fn time_manager_set_automatic(on: bool) -> i32 {
    let server = with(|s| {
        s.settings.automatic = on;
        s.settings.ntp_server.clone()
    });
    let Some(server) = server else {
        return ESP_ERR_INVALID_STATE;
    };
    if on {
        platform::sntp_start(&server);
        crate::kworker::post(tick, std::ptr::null_mut());
    } else {
        platform::sntp_stop();
    }
    crate::system_json::save(SECTION, "automatic", serde_json::Value::Bool(on))
}

/// `zone`: an IANA name from the list, or "" to follow the mobile network.
///
/// # Safety
/// `zone` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn time_manager_set_zone(zone: *const c_char) -> i32 {
    let Some(zone) = c_str(zone) else {
        return ESP_ERR_INVALID_ARG;
    };
    if !zone.is_empty() && crate::time_zones::posix_for(zone).is_none() {
        return ESP_ERR_INVALID_ARG;
    }
    let applied = with(|s| {
        s.settings.zone = zone.to_string();
        apply_zone(s);
        let zone = zone_for_log(s);
        log!(ESP_LOG_INFO, b"zone %s\0", zone.as_ptr());
    });
    if applied.is_none() {
        return ESP_ERR_INVALID_STATE;
    }
    crate::system_json::save(SECTION, "zone", serde_json::Value::String(zone.to_string()))
}

#[no_mangle]
pub extern "C" fn time_manager_set_24h(on: bool) -> i32 {
    if with(|s| s.settings.format_24h = on).is_none() {
        return ESP_ERR_INVALID_STATE;
    }
    crate::system_json::save(SECTION, "format_24h", serde_json::Value::Bool(on))
}

/// Set the clock by hand to `text`, local time as "2026-10-13 14:05".
/// ESP_ERR_INVALID_STATE while automatic time is on; ESP_ERR_INVALID_ARG for
/// text that is not such a time, or one before 2026.
///
/// # Safety
/// `text` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn time_manager_set_local(text: *const c_char) -> i32 {
    let Some(civil) = c_str(text).and_then(time_policy::parse_typed) else {
        return ESP_ERR_INVALID_ARG;
    };
    match with(|s| s.settings.automatic) {
        None => return ESP_ERR_INVALID_STATE,
        Some(true) => return ESP_ERR_INVALID_STATE,
        Some(false) => {}
    }
    let Some(utc) = libc_time::utc_from_local(civil) else {
        return ESP_ERR_INVALID_ARG;
    };
    if !time_policy::plausible(utc * US) {
        return ESP_ERR_INVALID_ARG;
    }
    match offer(Source::Manual, utc * US) {
        Verdict::Step => {
            crate::kworker::post(rtc_job, std::ptr::null_mut());
            ESP_OK
        }
        _ => ESP_FAIL,
    }
}

#[no_mangle]
pub extern "C" fn time_manager_zone_count() -> usize {
    crate::time_zones::ZONES.len()
}

/// The IANA name of zone `index`, "Europe/Oslo", or NULL past the end.
#[no_mangle]
pub extern "C" fn time_manager_zone_name(index: usize) -> *const c_char {
    crate::time_zones::ZONES
        .get(index)
        .map_or(std::ptr::null(), |z| z.name.as_ptr())
}

// ---------------------------------------------------------------------------
// The C library's clock
// ---------------------------------------------------------------------------

/// Local time by the C library's own rules, so that it agrees with what C
/// apps get from localtime().
pub(crate) mod libc_time {
    use super::Civil;
    use std::ffi::c_int;

    /// struct tm: the nine fields every C library has, and room for the two
    /// glibc and musl add after them.
    #[repr(C)]
    struct Tm {
        sec: c_int,
        min: c_int,
        hour: c_int,
        mday: c_int,
        mon: c_int,
        year: c_int,
        wday: c_int,
        yday: c_int,
        isdst: c_int,
        _tail: [usize; 4],
    }

    impl Tm {
        fn zeroed() -> Self {
            Tm { sec: 0, min: 0, hour: 0, mday: 0, mon: 0, year: 0, wday: 0, yday: 0, isdst: 0, _tail: [0; 4] }
        }
    }

    // time_t is 64 bits in ESP-IDF 5 and on the simulators' hosts.
    extern "C" {
        fn tzset();
        fn localtime_r(t: *const i64, out: *mut Tm) -> *mut Tm;
        fn mktime(tm: *mut Tm) -> i64;
    }

    /// Make `rule` the zone of every localtime() in the system.
    pub(crate) fn apply_tz(rule: &str) {
        std::env::set_var("TZ", rule);
        unsafe { tzset() };
    }

    /// `t`, seconds since 1970, as a local date and time.
    pub(crate) fn local(t: i64) -> Option<Civil> {
        let mut tm = Tm::zeroed();
        if unsafe { localtime_r(&t, &mut tm) }.is_null() {
            return None;
        }
        Civil::new(
            tm.year as i64 + 1900,
            (tm.mon + 1) as u32,
            tm.mday as u32,
            tm.hour as u32,
            tm.min as u32,
            tm.sec.min(59) as u32,
        )
    }

    /// How far local time is ahead of UTC at `t`, in minutes.
    pub(crate) fn offset_minutes(t: i64) -> i32 {
        local(t).map_or(0, |l| ((l.as_utc_seconds() - t) / 60) as i32)
    }

    /// The UTC time a local date and time names, summer time worked out.
    pub(crate) fn utc_from_local(c: Civil) -> Option<i64> {
        let mut tm = Tm::zeroed();
        tm.year = (c.year - 1900) as c_int;
        tm.mon = c.month as c_int - 1;
        tm.mday = c.day as c_int;
        tm.hour = c.hour as c_int;
        tm.min = c.minute as c_int;
        tm.sec = c.second as c_int;
        tm.isdst = -1;
        let t = unsafe { mktime(&mut tm) };
        (t != -1).then_some(t)
    }
}

// ---------------------------------------------------------------------------
// The SoC side (components/kernel/src/time_platform.c)
// ---------------------------------------------------------------------------

#[cfg(target_os = "espidf")]
mod platform {
    use super::Record;
    use std::ffi::{c_char, CString};

    extern "C" {
        fn time_platform_sntp_start(server: *const c_char) -> i32;
        fn time_platform_sntp_stop();
        fn time_platform_set(utc_us: i64, slew: bool) -> i32;
        fn time_platform_pending_slew_us() -> i64;
        fn time_platform_record_load(source: *mut u8, at_us: *mut i64, offset_min: *mut i32) -> bool;
        fn time_platform_record_store(source: u8, at_us: i64, offset_min: i32);
    }

    pub(super) fn sntp_start(server: &str) {
        let Ok(server) = CString::new(server) else {
            return;
        };
        unsafe { time_platform_sntp_start(server.as_ptr()) };
    }

    pub(super) fn sntp_stop() {
        unsafe { time_platform_sntp_stop() }
    }

    pub(super) fn set(utc_us: i64, slew: bool) -> i32 {
        unsafe { time_platform_set(utc_us, slew) }
    }

    pub(super) fn pending_slew_us() -> i64 {
        unsafe { time_platform_pending_slew_us() }
    }

    pub(super) fn record_load() -> Option<Record> {
        let (mut source, mut at_us, mut offset) = (0u8, 0i64, 0i32);
        if !unsafe { time_platform_record_load(&mut source, &mut at_us, &mut offset) } {
            return None;
        }
        Some(Record::from_raw(source, at_us, offset))
    }

    pub(super) fn record_store(record: Record) {
        let (source, at_us, offset) = record.to_raw();
        unsafe { time_platform_record_store(source, at_us, offset) }
    }
}

/// Off the device the host's clock is the system clock, and nothing here may
/// set it. The host keeps it by NTP, so starting NTP reports that.
#[cfg(not(target_os = "espidf"))]
mod platform {
    use super::Record;
    use std::ffi::c_void;

    unsafe extern "C" fn host_ntp(_arg: *mut c_void) {
        let now = super::now_us();
        super::time_manager_ntp_synced(now / super::US, (now % super::US) as i32);
    }

    pub(super) fn sntp_start(_server: &str) {
        crate::kworker::post(host_ntp, std::ptr::null_mut());
    }
    pub(super) fn sntp_stop() {}
    pub(super) fn set(_utc_us: i64, _slew: bool) -> i32 {
        super::ESP_OK
    }
    pub(super) fn pending_slew_us() -> i64 {
        0
    }
    pub(super) fn record_load() -> Option<Record> {
        None
    }
    pub(super) fn record_store(_record: Record) {}
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn settings_default_to_automatic_time_in_the_networks_zone_on_a_24_hour_clock() {
        let fresh = settings_in(None);
        assert!(fresh.automatic);
        assert_eq!(fresh.zone, "");
        assert!(fresh.format_24h);
        assert_eq!(fresh.ntp_server, "pool.ntp.org");
        assert_eq!(settings_in(Some("not json")), fresh);
    }

    #[test]
    fn settings_are_read_from_the_time_section() {
        let json = r#"{"thistle_os": {"time": {"automatic": false, "zone": "Europe/Oslo",
            "format_24h": false, "ntp_server": " ntp.example.org "}}}"#;
        let s = settings_in(Some(json));
        assert!(!s.automatic);
        assert_eq!(s.zone, "Europe/Oslo");
        assert!(!s.format_24h);
        assert_eq!(s.ntp_server, "ntp.example.org");
        let blank = settings_in(Some(r#"{"thistle_os": {"time": {"ntp_server": ""}}}"#));
        assert_eq!(blank.ntp_server, "pool.ntp.org");
    }

    #[test]
    fn a_zone_setting_comes_to_a_posix_rule() {
        assert_eq!(zone_rule("Europe/Oslo", Some(330)), "CET-1CEST,M3.5.0,M10.5.0/3");
        assert_eq!(zone_rule("", Some(120)), "<+02>-2");
        assert_eq!(zone_rule("", None), "UTC0");
        assert_eq!(zone_rule("XYZ-3", None), "XYZ-3", "a rule written by hand");
        assert_eq!(zone_label("America/New_York", None), "America/New York");
        assert_eq!(zone_label("", Some(-210)), "Automatic (UTC-03:30)");
        assert_eq!(zone_label("", None), "Automatic (UTC)");
    }

    #[test]
    fn local_time_follows_the_zone_both_ways() {
        libc_time::apply_tz("CET-1CEST,M3.5.0,M10.5.0/3");
        let summer = Civil::new(2026, 7, 15, 14, 5, 0).unwrap();
        let utc = libc_time::utc_from_local(summer).unwrap();
        assert_eq!(utc, Civil::new(2026, 7, 15, 12, 5, 0).unwrap().as_utc_seconds());
        assert_eq!(libc_time::local(utc), Some(summer));
        let winter = Civil::new(2026, 1, 15, 14, 5, 0).unwrap();
        assert_eq!(
            libc_time::utc_from_local(winter).unwrap(),
            Civil::new(2026, 1, 15, 13, 5, 0).unwrap().as_utc_seconds()
        );
        libc_time::apply_tz("UTC0");
    }

    #[test]
    fn the_status_matches_its_c_layout() {
        // time_status_t: bool, enum, int64_t, three bools, char[48].
        assert_eq!(std::mem::size_of::<CTimeStatus>(), 72);
        assert_eq!(std::mem::offset_of!(CTimeStatus, source), 4);
        assert_eq!(std::mem::offset_of!(CTimeStatus, synced_at), 8);
        assert_eq!(std::mem::offset_of!(CTimeStatus, has_rtc), 18);
        assert_eq!(std::mem::offset_of!(CTimeStatus, zone), 19);
    }

    #[test]
    fn the_record_keeps_the_source_and_the_networks_zone() {
        let full = Record {
            last: Some(Sync { source: Source::Ntp, at_us: 1_791_915_850_000_000 }),
            network_offset: Some(120),
        };
        let (s, at, off) = full.to_raw();
        assert_eq!(Record::from_raw(s, at, off), full);
        assert_eq!(Record::from_raw(s, at, -330), Record { network_offset: Some(-330), ..full });

        // Neither yet: an offset is kept before any source set the clock.
        let empty = Record::default();
        assert_eq!(empty.to_raw(), (0, 0, i32::MIN));
        assert_eq!(Record::from_raw(0, 0, i32::MIN), empty);
        assert_eq!(Record::from_raw(0, 0, 60), Record { last: None, network_offset: Some(60) });

        // What no source or zone could be.
        assert_eq!(Record::from_raw(99, at, 7), empty);
    }

    #[test]
    fn a_gnss_time_counts_only_while_it_moves_with_our_clock() {
        assert!(gnss_is_live((100 * US, 0), (130 * US, 30 * US)));
        assert!(gnss_is_live((100 * US, 0), (134 * US, 30 * US)), "a late sentence");
        assert!(!gnss_is_live((100 * US, 0), (100 * US, 30 * US)), "stuck on its last time");
        assert!(!gnss_is_live((100 * US, 0), (200 * US, 30 * US)), "jumped");
        assert!(!gnss_is_live((100 * US, 0), (90 * US, 30 * US)), "went back");
    }
}
