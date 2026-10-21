// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — U-blox MIA-M10Q GPS driver (Rust)
//
// Rust port of components/drv_gps_mia_m10q/src/drv_gps_mia_m10q.c.
//
// Communicates with the U-blox MIA-M10Q via UART. A job on the kernel worker
// reads NMEA sentences byte-by-byte, parses $GNRMC and $GNGGA for the fix and
// each constellation's GSV for what the receiver hears, and fires the
// registered callback on a valid fix.
//
// The receiver goes into software standby (UBX-RXM-PMREQ) while GPS is off
// and while the device sleeps, keeping what it knows of the sky for a quick
// start, and an edge on its UART RX wakes it. So anything sent to it wakes it.
//
// Once the receiver talks after a wake it is sent its signals: GPS,
// Galileo, GLONASS and BeiDou B1C. A second later goes AssistNow
// Autonomous, switched on, then the assistance (`assist`): the time, where
// it last was, and of AssistNow's messages the almanacs and the orbit
// predictions for today, a few each pass with the receiver asked to
// acknowledge each (UBX-MGA-ACK). What AssistNow registers a receiver by,
// its UBX-SEC-UNIQID and UBX-MON-VER replies, is polled when asked for, and
// so is its report of each satellite, UBX-NAV-SAT (`get_satellites`).

#![allow(non_upper_case_globals)]

use std::os::raw::{c_char, c_void};
use std::time::Duration;

use crate::hal_registry::{
    HalGpsAssist, HalGpsCb, HalGpsDriver, HalGpsIdentity, HalGpsPosition, HalGpsSat, HalGpsSats,
    HalGpsSky, HAL_GPS_HAS_ALMANAC, HAL_GPS_HAS_AUTONOMOUS, HAL_GPS_HAS_EPHEMERIS,
    HAL_GPS_HAS_PREDICTED, HAL_GPS_ORBIT_ALMANAC, HAL_GPS_ORBIT_AUTONOMOUS,
    HAL_GPS_ORBIT_EPHEMERIS, HAL_GPS_ORBIT_NONE, HAL_GPS_ORBIT_OTHER, HAL_GPS_ORBIT_PREDICTED,
    HAL_GPS_SATS_MAX,
};

// ── ESP error codes ──────────────────────────────────────────────────────────

const ESP_OK: i32 = 0;
const ESP_FAIL: i32 = -1;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_ERR_NOT_FOUND: i32 = 0x105;

// ── Driver constants ─────────────────────────────────────────────────────────

/// The UART's receive buffer: about two seconds at 38400 baud, so the reader
/// loses nothing while another job holds the kernel worker, a LoRa send for
/// one. Over 4 KB, the UART driver takes it from PSRAM.
const UART_RX_BUF_SIZE: i32 = 8192;
const KNOTS_TO_KMH: f32 = 1.852;

/// How often the reader takes what the UART has buffered.
const RX_POLL: Duration = Duration::from_millis(100);
/// Bytes taken off the UART per read.
const RX_CHUNK: usize = 128;
/// How long the line may go without one good sentence before the driver says
/// so. A receiver that is powered and at the right rate talks within a second.
const SILENCE_WARN_TICKS: u32 = 5000;

/// UBX-RXM-PMREQ: software standby until an edge on the receiver's UART RX.
///
/// The 16-byte form, the only one the M10's firmware (ROM SPG 5.10) defines,
/// with `force`, which the integration manual (§3.6.3.2) says standby needs.
/// Standby keeps the battery-backed RAM, so a receiver woken within the
/// ephemeris' four hours starts hot.
///
/// The 8-byte form this driver sent before is the M8's, and the M10 does not
/// take it reliably: on the bench it sometimes ran on, and sometimes went
/// into a standby that the single byte the driver then sent did not wake it
/// from. From such a sleep on, the receiver stayed off until the board lost
/// power or deep-slept. Bench, 2026-10-20, ticket 06.
const UBX_RXM_PMREQ_STANDBY: [u8; 24] = [
    0xB5, 0x62, // sync chars
    0x02, 0x41, // class, id
    0x10, 0x00, // payload length = 16
    0x00, 0x00, 0x00, 0x00, // version 0, reserved
    0x00, 0x00, 0x00, 0x00, // duration = 0: until woken
    0x06, 0x00, 0x00, 0x00, // flags: backup | force
    0x08, 0x00, 0x00, 0x00, // wakeupSources: uartrx
    0x61, 0x6B, // checksum
];

/// What wakes the receiver. One byte of anything wakes one this driver sent
/// to standby, talking again 0.3 s later. One the 8-byte form sent there
/// comes back less readily: on the bench single bytes woke it once in nine
/// tries, and sixteen zero bytes did on the first to the third. A restart
/// leaves the receiver powered, so after an update it may still be there.
/// A receiver already awake ignores them.
const WAKE_PULSE: [u8; 16] = [0; 16];
/// How long a woken receiver gets to say something before it is woken again,
/// and how many times: for the one the 8-byte form left in standby, which
/// may take several pulses. One that never answers costs 160 bytes on its
/// RX line.
const WAKE_RETRY_TICKS: u32 = 3000;
const WAKE_TRIES: u8 = 10;

/// How long a constellation's GSV report counts: the receiver sends one a
/// second, so an older one is a constellation it stopped reporting.
const SKY_STALE_TICKS: u32 = 3000;
/// Constellation and signal pairs kept: GPS, Galileo, BeiDou and QZSS by
/// default, GLONASS when it is switched on, with room to spare.
const SKY_REPORTS: usize = 8;
/// When the first "no fix" line is due after the receiver starts, and the
/// longest gap between two. Each gap doubles the one before: a receiver left
/// indoors says so a few times, not every minute all day.
const SKY_LOG_FIRST_TICKS: u32 = 30_000;
const SKY_LOG_MAX_GAP_TICKS: u32 = 600_000;

/// The longest UBX-MON-VER kept: 40 bytes and 30 a string after them,
/// with eleven strings. This receiver sends six.
const UBX_MAX: usize = 400;
/// The longest UBX frame read off the wire: UBX-NAV-SAT, 8 bytes and 12 a
/// satellite, for 84 satellites. With four constellations the receiver
/// lists 40 to 60.
const UBX_FRAME_MAX: usize = 1024;
/// The poll for UBX-NAV-SAT.
const UBX_POLL_NAV_SAT: [u8; 8] = [0xB5, 0x62, 0x01, 0x35, 0x00, 0x00, 0x36, 0xA3];
/// The polls for what AssistNow registers a receiver by.
const UBX_POLL_SEC_UNIQID: [u8; 8] = [0xB5, 0x62, 0x27, 0x03, 0x00, 0x00, 0x2A, 0xA5];
const UBX_POLL_MON_VER: [u8; 8] = [0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34];
/// How long a receiver gets to answer them, and how many times it is asked
/// between two wakes.
const IDENTITY_RETRY_TICKS: u32 = 3000;
const IDENTITY_POLLS: u8 = 3;
/// UBX-CFG-VALSET's payload for the signals tracked: GPS, Galileo, GLONASS
/// and BeiDou's B1C, with QZSS and SBAS as they come, in RAM and in the
/// battery-backed RAM, which standby keeps. GLONASS cannot be had with
/// BeiDou's B1I, the module's default, and B1C, on GPS's frequency, makes
/// the four (integration manual §2.1.3); B1C acquires a little less
/// readily. GLONASS is wanted for its orbit predictions, which this
/// receiver takes, as it does GPS's.
const VALSET_SIGNALS: [u8; 29] = [
    0x00, 0x03, 0x00, 0x00, // version 0, layers RAM and BBR
    0x25, 0x00, 0x31, 0x10, 0x01, // CFG-SIGNAL-GLO_ENA
    0x18, 0x00, 0x31, 0x10, 0x01, // CFG-SIGNAL-GLO_L1_ENA
    0x22, 0x00, 0x31, 0x10, 0x01, // CFG-SIGNAL-BDS_ENA
    0x0D, 0x00, 0x31, 0x10, 0x00, // CFG-SIGNAL-BDS_B1_ENA
    0x0F, 0x00, 0x31, 0x10, 0x01, // CFG-SIGNAL-BDS_B1C_ENA
];
/// A change of signals restarts the receiver's GNSS: the manual asks for
/// its acknowledgement and half a second before anything more is sent.
const SIGNALS_SETTLE_TICKS: u32 = 1000;
/// UBX-CFG-VALSET's payload for CFG-ANA-USE_ANA = 1, in RAM and BBR:
/// AssistNow Autonomous, the receiver's own orbit predictions, made from
/// each ephemeris it decodes and good for three days (integration manual
/// §3.12.4). They cover Galileo and BeiDou, whose AssistNow predictions this
/// receiver does not take, but not GLONASS, which would need a flash it does
/// not have. Kept in BBR, so through standby but not deep sleep. The manual
/// recommends it alongside AssistNow Offline: the receiver picks the better
/// prediction for each satellite. Sent apart from the signals, so a receiver
/// that refused it would still have those.
const VALSET_AUTONOMOUS: [u8; 9] = [0x00, 0x03, 0x00, 0x00, 0x01, 0x00, 0x23, 0x10, 0x01];
/// UBX-CFG-VALSET's payload for CFG-NAVSPG-ACKAIDING = 1, in RAM: the
/// receiver acknowledges each assistance message, UBX-MGA-ACK, and says why
/// one was refused. Off by default; RAM's settings go with the power.
const VALSET_ACK_AIDING: [u8; 9] = [0x00, 0x01, 0x00, 0x00, 0x25, 0x00, 0x11, 0x10, 0x01];
/// The constellations whose orbit predictions (UBX-MGA-ANO) this receiver
/// takes: GPS and GLONASS, the datasheet's table 5. AssistNow sends
/// Galileo's and BeiDou's too, for receivers that take them.
const ANO_GNSS: [u8; 2] = [0, 6];
/// Bytes of assistance sent a pass, at least one message: about half the
/// line's 3.8 KB a second, so the receiver's buffer never fills while it
/// also searches the sky. A day's messages take a few seconds.
const ASSIST_BUDGET: usize = 200;
/// How long after the last message the acknowledgements may still come.
const ACK_WAIT_TICKS: u32 = 2000;

// ── ESP-IDF FFI ──────────────────────────────────────────────────────────────

#[cfg(target_os = "espidf")]
extern "C" {
    fn uart_param_config(port: i32, cfg: *const c_void) -> i32;
    fn uart_set_pin(port: i32, tx: i32, rx: i32, rts: i32, cts: i32) -> i32;
    fn uart_driver_install(
        port: i32,
        rx_buf: i32,
        tx_buf: i32,
        queue_size: i32,
        queue: *mut *mut c_void,
        flags: i32,
    ) -> i32;
    fn uart_driver_delete(port: i32) -> i32;
    fn uart_read_bytes(port: i32, buf: *mut u8, len: u32, timeout: u32) -> i32;
    fn uart_write_bytes(port: i32, buf: *const u8, len: usize) -> i32;
    fn uart_wait_tx_done(port: i32, ticks: u32) -> i32;
    fn uart_flush_input(port: i32) -> i32;
    fn xTaskGetTickCount() -> u32;
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

const ESP_LOG_WARN: i32 = 2;
const ESP_LOG_INFO: i32 = 3;

/// One console line under the driver's own tag. esp_log_write adds neither the
/// tag nor the newline, so the macro does.
macro_rules! gps_log {
    ($level:expr, $fmt:literal $(, $arg:expr)*) => {{
        #[cfg(target_os = "espidf")]
        unsafe {
            esp_log_write($level, b"gps\0".as_ptr(), concat!("gps: ", $fmt, "\n\0").as_ptr() $(, $arg)*);
        }
        #[cfg(not(target_os = "espidf"))]
        {
            let _ = ($level, $($arg,)*);
        }
    }};
}

// ── Simulator / host stubs ───────────────────────────────────────────────────

#[cfg(not(target_os = "espidf"))]
unsafe fn uart_param_config(_port: i32, _cfg: *const c_void) -> i32 {
    0
}

#[cfg(not(target_os = "espidf"))]
unsafe fn uart_set_pin(_port: i32, _tx: i32, _rx: i32, _rts: i32, _cts: i32) -> i32 {
    0
}

#[cfg(not(target_os = "espidf"))]
unsafe fn uart_driver_install(
    _port: i32,
    _rx_buf: i32,
    _tx_buf: i32,
    _queue_size: i32,
    _queue: *mut *mut c_void,
    _flags: i32,
) -> i32 {
    0
}

#[cfg(not(target_os = "espidf"))]
unsafe fn uart_driver_delete(_port: i32) -> i32 {
    0
}

#[cfg(not(target_os = "espidf"))]
unsafe fn uart_read_bytes(_port: i32, _buf: *mut u8, _len: u32, _timeout: u32) -> i32 {
    0
}

/// Everything the driver sent the receiver, for the tests to read.
#[cfg(not(target_os = "espidf"))]
static UART_SENT: std::sync::Mutex<Vec<u8>> = std::sync::Mutex::new(Vec::new());

#[cfg(not(target_os = "espidf"))]
unsafe fn uart_write_bytes(_port: i32, buf: *const u8, len: usize) -> i32 {
    let bytes = std::slice::from_raw_parts(buf, len);
    UART_SENT.lock().unwrap_or_else(|e| e.into_inner()).extend_from_slice(bytes);
    len as i32
}

#[cfg(not(target_os = "espidf"))]
unsafe fn uart_wait_tx_done(_port: i32, _ticks: u32) -> i32 {
    0
}

#[cfg(not(target_os = "espidf"))]
unsafe fn uart_flush_input(_port: i32) -> i32 {
    0
}

#[cfg(not(target_os = "espidf"))]
unsafe fn xTaskGetTickCount() -> u32 {
    0
}

// ── uart_config_t layout (must match ESP-IDF struct on Xtensa) ───────────────

#[cfg(target_os = "espidf")]
#[repr(C)]
struct UartConfig {
    baud_rate: i32,
    data_bits: u32, // UART_DATA_8_BITS = 3
    parity: u32,    // UART_PARITY_DISABLE = 0
    stop_bits: u32, // UART_STOP_BITS_1 = 1
    flow_ctrl: u32, // UART_HW_FLOWCTRL_DISABLE = 0
    rx_flow_ctrl_thresh: u8,
    source_clk: u32, // UART_SCLK_DEFAULT = 0
    /// The `flags` bitfield ESP-IDF reads for `allow_pd` / `backup_before_sleep`.
    ///
    /// Leaving it off the end did not make it go unread: `uart_param_config`
    /// reads it either way, so it took whatever the stack happened to hold
    /// past the struct. On a chip without sleep retention — the ESP32-S3 —
    /// a non-zero read is rejected outright, and the whole driver failed to
    /// start with "not able to power down in light sleep".
    flags: u32,
}

/// The struct above is copied out of a C header by hand, so nothing but this
/// stops it drifting when that header gains a field, which is exactly how the
/// missing `flags` word got in.
#[cfg(target_os = "espidf")]
const _: () = assert!(core::mem::size_of::<UartConfig>() == 32);

// ── Configuration struct ─────────────────────────────────────────────────────

/// C-compatible config for the MIA-M10Q driver.
/// Must match `gps_mia_m10q_config_t` in the C header.
#[repr(C)]
pub struct GpsMiaM10qConfig {
    /// uart_port_t (0 or 1)
    pub uart_num: i32,
    /// TX GPIO number (gpio_num_t)
    pub pin_tx: i32,
    /// RX GPIO number (gpio_num_t)
    pub pin_rx: i32,
    /// Baud rate; 0 defaults to 9600
    pub baud_rate: u32,
}

// SAFETY: Config holds only primitives; accessed from single-threaded
// board-init context before the RX task starts.
unsafe impl Send for GpsMiaM10qConfig {}
unsafe impl Sync for GpsMiaM10qConfig {}

// ── Driver state ─────────────────────────────────────────────────────────────

struct GpsState {
    cfg: GpsMiaM10qConfig,
    cb: Option<unsafe extern "C" fn(*const HalGpsPosition, *mut c_void)>,
    cb_data: *mut c_void,
    last_position: HalGpsPosition,
    /// The reader's kernel worker job, or 0.
    rx_job: u32,
    /// When the reader started, in ticks, and what it has said since.
    rx_started: u32,
    link_reported: bool,
    fix_reported: bool,
    /// When the next "no fix" line is due, in ticks after `rx_started`.
    next_sky_log: u32,
    initialized: bool,
    enabled: bool,
    /// Whether the receiver was last sent to standby rather than woken. True
    /// until the first wake: an ESP restart leaves the receiver as it was,
    /// so one sent to standby before it is still there.
    standby: bool,
    /// Tries at waking it since it was last heard from; 0 when not waking.
    wake_tries: u8,
    /// When the last wake went, and the sentence count then.
    wake_sent: u32,
    sentences_at_wake: u32,
    sky: Sky,
    nmea_buf: [u8; 256],
    nmea_idx: usize,
    /// Bytes off the wire and sentences that passed their checksum, since the
    /// last `enable`. Bytes with no sentences is a rate mismatch; no bytes at
    /// all is an unpowered or unwired receiver.
    bytes_seen: u32,
    sentences_seen: u32,
    /// Whether the signals and AssistNow Autonomous have been sent since
    /// the last wake, and when the signals went.
    signals_sent: bool,
    signals_at: u32,
    autonomous_sent: bool,
    /// The UBX-CFG-VALSETs awaiting their answer (UBX-ACK), oldest first:
    /// the receiver answers in order.
    cfg_awaited: Vec<Cfg>,
    /// Whether `get_satellites` has asked for the next UBX-NAV-SAT.
    sats_wanted: bool,
    /// Whether `get_identity` has asked for what the receiver has not yet
    /// answered, and the polls for it since the last wake.
    identity_wanted: bool,
    identity_polls: u8,
    identity_polled: u32,
    /// The assistance `assist` was handed, until the receiver talks, and
    /// then what is being sent of it.
    aid: Option<Box<Aid>>,
    sending: Option<Box<Sending>>,
}

// SAFETY: The driver state is only mutated during single-threaded board-init
// and from the reader job; last_position updates mirror C portENTER_CRITICAL
// semantics via the static_mut + unsafe convention used throughout this crate.
unsafe impl Send for GpsState {}
unsafe impl Sync for GpsState {}

impl GpsState {
    const fn new() -> Self {
        GpsState {
            cfg: GpsMiaM10qConfig {
                uart_num: 1,
                pin_tx: -1,
                pin_rx: -1,
                baud_rate: 9600,
            },
            cb: None,
            cb_data: std::ptr::null_mut(),
            last_position: HalGpsPosition {
                latitude: 0.0,
                longitude: 0.0,
                altitude_m: 0.0,
                speed_kmh: 0.0,
                heading_deg: 0.0,
                satellites: 0,
                fix_valid: false,
                timestamp: 0,
            },
            rx_job: 0,
            rx_started: 0,
            link_reported: false,
            fix_reported: false,
            next_sky_log: SKY_LOG_FIRST_TICKS,
            initialized: false,
            enabled: false,
            standby: true,
            wake_tries: 0,
            wake_sent: 0,
            sentences_at_wake: 0,
            sky: Sky::new(),
            nmea_buf: [0u8; 256],
            nmea_idx: 0,
            bytes_seen: 0,
            sentences_seen: 0,
            signals_sent: false,
            signals_at: 0,
            autonomous_sent: false,
            cfg_awaited: Vec::new(),
            sats_wanted: false,
            identity_wanted: false,
            identity_polls: 0,
            identity_polled: 0,
            aid: None,
            sending: None,
        }
    }
}

static mut S_GPS: GpsState = GpsState::new();

/// The receiver's UBX replies: the frame being read; what it said to the
/// identity polls, whole frames, 0 long until it answers; and its last
/// report of its satellites, with the tick it came at. In PSRAM, as only
/// the reader, `get_identity` and `get_satellites`, all on the kernel
/// worker, touch them.
struct Ubx {
    frame: [u8; UBX_FRAME_MAX],
    len: usize,
    unique_id: [u8; 32],
    unique_id_len: usize,
    version: [u8; UBX_MAX],
    version_len: usize,
    sats: HalGpsSats,
    sats_at: u32,
    /// Whether `sats` came since the last wake.
    sats_fresh: bool,
}

#[cfg_attr(target_os = "espidf", link_section = ".ext_ram.bss")]
static mut S_UBX: Ubx = Ubx {
    frame: [0; UBX_FRAME_MAX],
    len: 0,
    unique_id: [0; 32],
    unique_id_len: 0,
    version: [0; UBX_MAX],
    version_len: 0,
    sats: HalGpsSats::EMPTY,
    sats_at: 0,
    sats_fresh: false,
};

// ── NMEA helpers (pure Rust, no external crates) ─────────────────────────────

/// Verify the NMEA XOR checksum.
///
/// The checksum covers all bytes strictly between `$` and `*`.
/// Returns `false` if the sentence is malformed or the checksum does not match.
pub fn nmea_verify_checksum(sentence: &[u8]) -> bool {
    if sentence.is_empty() || sentence[0] != b'$' {
        return false;
    }

    let inner = &sentence[1..]; // skip '$'
    let star_pos = match inner.iter().position(|&b| b == b'*') {
        Some(p) => p,
        None => return false,
    };

    let computed: u8 = inner[..star_pos].iter().fold(0u8, |acc, &b| acc ^ b);

    let hex = &inner[star_pos + 1..];
    if hex.len() < 2 {
        return false;
    }

    let received = match u8::from_str_radix(core::str::from_utf8(&hex[..2]).unwrap_or("XX"), 16) {
        Ok(v) => v,
        Err(_) => return false,
    };

    computed == received
}

/// Convert an NMEA coordinate field (`ddmm.mmmm` or `dddmm.mmmm`) plus a
/// direction character (`N`, `S`, `E`, `W`) to signed decimal degrees.
///
/// Returns `0.0` for an empty or unparseable field.
pub fn nmea_parse_coord(field: &str, dir: &str) -> f64 {
    if field.is_empty() {
        return 0.0;
    }

    let raw: f64 = match field.parse() {
        Ok(v) => v,
        Err(_) => return 0.0,
    };

    let deg = (raw / 100.0).floor();
    let mins = raw - deg * 100.0;
    let mut dd = deg + mins / 60.0;

    if let Some(c) = dir.chars().next() {
        if c == 'S' || c == 'W' {
            dd = -dd;
        }
    }

    dd
}

/// Convert NMEA UTC time (`hhmmss` or `hhmmss.ss`) and date (`ddmmyy`)
/// strings to a Unix timestamp (seconds since 1970-01-01 UTC).
///
/// Returns `0` if either string is too short or the date is invalid.
///
/// Uses a hand-rolled UTC epoch calculation that matches `mktime` behaviour on
/// an ESP32 where the TZ is always UTC.
pub fn nmea_to_timestamp(time_str: &str, date_str: &str) -> u32 {
    if time_str.len() < 6 || date_str.len() < 6 {
        return 0;
    }

    let tb = time_str.as_bytes();
    let db = date_str.as_bytes();

    let digit = |b: u8| -> u32 {
        if b.is_ascii_digit() {
            (b - b'0') as u32
        } else {
            0
        }
    };

    let hour = digit(tb[0]) * 10 + digit(tb[1]);
    let min = digit(tb[2]) * 10 + digit(tb[3]);
    let sec = digit(tb[4]) * 10 + digit(tb[5]);
    let mday = digit(db[0]) * 10 + digit(db[1]);
    let month = digit(db[2]) * 10 + digit(db[3]); // 1-based
    let yy = digit(db[4]) * 10 + digit(db[5]);
    // Match C mktime convention: yy is years since 1900, so yy ≥ 70 → 1970+,
    // yy < 70 → 2000+.  (Same rule as the C driver's `t.tm_year = yy + 100`.)
    let year = if yy >= 70 { 1900 + yy } else { 2000 + yy };

    if mday == 0 || month == 0 || month > 12 {
        return 0;
    }

    const DAYS_IN_MONTH: [u32; 12] = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];

    let is_leap = |y: u32| (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);

    // Days from 1970-01-01 to start of `year`
    let mut days: u64 = 0;
    for y in 1970..year {
        days += if is_leap(y) { 366 } else { 365 };
    }

    // Days for completed months within `year`
    for m in 1..month {
        let extra = if m == 2 && is_leap(year) { 1 } else { 0 };
        days += DAYS_IN_MONTH[(m - 1) as usize] as u64 + extra;
    }

    // Days completed in current month (mday is 1-based)
    days += (mday - 1) as u64;

    let ts = days * 86400 + hour as u64 * 3600 + min as u64 * 60 + sec as u64;
    ts as u32
}

// ── What the receiver hears ──────────────────────────────────────────────────

/// One constellation's last GSV report.
#[derive(Clone, Copy)]
struct SkyReport {
    /// Talker ID and signal ID, `GP1` or `GA7`; `-` for a sentence older than
    /// NMEA 4.10, which has no signal ID. All zero for a slot not in use.
    key: [u8; 3],
    in_view: u8,
    heard: u8,
    best_cno: u8,
    /// When its last sentence came, in ticks.
    at: u32,
}

impl SkyReport {
    const EMPTY: SkyReport = SkyReport { key: [0; 3], in_view: 0, heard: 0, best_cno: 0, at: 0 };
}

/// The GSV sentences put together. The receiver sends a set a second for each
/// constellation, four satellites to a sentence, so a constellation's report
/// is kept only once its set is whole.
struct Sky {
    reports: [SkyReport; SKY_REPORTS],
    /// The set being read, and the number of its sentence due next; 0 when
    /// none is being read.
    partial: SkyReport,
    next_msg: u8,
}

impl Sky {
    const fn new() -> Self {
        Sky { reports: [SkyReport::EMPTY; SKY_REPORTS], partial: SkyReport::EMPTY, next_msg: 0 }
    }

    /// Take one GSV sentence, split into fields:
    /// `$xxGSV,numMsg,msgNum,numSV{,svid,elv,az,cno},signalId`.
    fn feed(&mut self, fields: &[&str], now: u32) {
        if fields.len() < 4 || fields[0].len() != 6 {
            return;
        }
        let num_msg: u8 = fields[1].parse().unwrap_or(0);
        let msg_num: u8 = fields[2].parse().unwrap_or(0);
        let in_view: u8 = fields[3].parse().unwrap_or(0);
        if msg_num == 0 || msg_num > num_msg {
            self.next_msg = 0;
            return;
        }

        // NMEA 4.10 and later put a signal ID after the groups of four.
        let rest = &fields[4..];
        let (sats, signal) = if rest.len() % 4 == 1 {
            (&rest[..rest.len() - 1], rest[rest.len() - 1])
        } else {
            (rest, "")
        };
        let talker = fields[0].as_bytes();
        let key = [talker[1], talker[2], signal.bytes().next().unwrap_or(b'-')];

        if msg_num == 1 {
            self.partial = SkyReport { key, in_view, heard: 0, best_cno: 0, at: now };
        } else if msg_num != self.next_msg || key != self.partial.key {
            // A sentence of the set went missing: the set is lost.
            self.next_msg = 0;
            return;
        }

        for sat in sats.chunks(4) {
            // C/N0 is empty for a satellite the receiver expects but does not hear.
            let cno: u8 = sat.get(3).and_then(|c| c.parse().ok()).unwrap_or(0);
            if cno > 0 {
                self.partial.heard = self.partial.heard.saturating_add(1);
                self.partial.best_cno = self.partial.best_cno.max(cno);
            }
        }

        if msg_num == num_msg {
            self.partial.at = now;
            self.keep(self.partial, now);
            self.next_msg = 0;
        } else {
            self.next_msg = msg_num + 1;
        }
    }

    /// Keep a whole report in its constellation's slot, else a free one, else
    /// the oldest.
    fn keep(&mut self, report: SkyReport, now: u32) {
        let slot = self
            .reports
            .iter()
            .position(|r| r.key == report.key)
            .or_else(|| self.reports.iter().position(|r| r.key[0] == 0))
            .unwrap_or_else(|| {
                (0..SKY_REPORTS)
                    .max_by_key(|&i| now.wrapping_sub(self.reports[i].at))
                    .unwrap_or(0)
            });
        self.reports[slot] = report;
    }

    /// Every constellation reported in the last few seconds, added up.
    fn summary(&self, now: u32) -> HalGpsSky {
        let mut sky = HalGpsSky::default();
        for r in &self.reports {
            if r.key[0] == 0 || now.wrapping_sub(r.at) >= SKY_STALE_TICKS {
                continue;
            }
            sky.in_view = sky.in_view.saturating_add(r.in_view);
            sky.heard = sky.heard.saturating_add(r.heard);
            sky.best_cno = sky.best_cno.max(r.best_cno);
        }
        sky
    }
}

// ── NMEA sentence processors ─────────────────────────────────────────────────

/// Split `sentence` on commas, stripping the `*CS` suffix if present.
fn nmea_split(sentence: &str) -> Vec<&str> {
    let body = if let Some(pos) = sentence.find('*') {
        &sentence[..pos]
    } else {
        sentence
    };
    body.split(',').collect()
}

/// Parse a `$GNRMC` / `$GPRMC` sentence and update the global position.
///
/// Field layout (0-based after comma-split):
/// ```text
/// 0  $GNRMC
/// 1  UTC time  (hhmmss.ss)
/// 2  Status    (A=active, V=void)
/// 3  Latitude  (ddmm.mmmm)
/// 4  N/S
/// 5  Longitude (dddmm.mmmm)
/// 6  E/W
/// 7  Speed (knots)
/// 8  Course (degrees true)
/// 9  Date  (ddmmyy)
/// ```
///
/// # Safety
/// Writes to `S_GPS.last_position`.
unsafe fn process_gnrmc(fields: &[&str]) {
    if fields.len() < 10 {
        return;
    }

    let valid = fields[2].starts_with('A');
    let lat = nmea_parse_coord(fields[3], fields[4]);
    let lon = nmea_parse_coord(fields[5], fields[6]);
    let speed = if !fields[7].is_empty() {
        fields[7].parse::<f32>().unwrap_or(0.0) * KNOTS_TO_KMH
    } else {
        0.0
    };
    let heading = if !fields[8].is_empty() {
        fields[8].parse::<f32>().unwrap_or(0.0)
    } else {
        0.0
    };
    let ts = nmea_to_timestamp(fields[1], fields[9]);

    let gps = &mut *(&raw mut S_GPS);
    gps.last_position.latitude = lat;
    gps.last_position.longitude = lon;
    gps.last_position.speed_kmh = speed;
    gps.last_position.heading_deg = heading;
    gps.last_position.fix_valid = valid;
    if ts != 0 {
        gps.last_position.timestamp = ts;
    }
}

/// Parse a `$GNGGA` / `$GPGGA` sentence and update altitude + satellite count.
///
/// Field layout (0-based):
/// ```text
/// 0  $GNGGA
/// 1  UTC time
/// 2  Latitude
/// 3  N/S
/// 4  Longitude
/// 5  E/W
/// 6  Fix quality (0=no fix, 1=GPS, 2=DGPS)
/// 7  Satellites in use
/// 8  HDOP
/// 9  Altitude MSL (metres)
/// ```
///
/// # Safety
/// Writes to `S_GPS.last_position`.
unsafe fn process_gngga(fields: &[&str]) {
    if fields.len() < 10 {
        return;
    }

    let quality: i32 = fields[6].parse().unwrap_or(0);
    let satellites: i32 = fields[7].parse().unwrap_or(0);
    let altitude: f32 = fields[9].parse().unwrap_or(0.0);

    let gps = &mut *(&raw mut S_GPS);
    gps.last_position.satellites = satellites.max(0) as u8;
    gps.last_position.altitude_m = altitude;
    if quality == 0 {
        gps.last_position.fix_valid = false;
    }
}

/// Dispatch a complete NMEA sentence to the appropriate parser.
///
/// Validates the checksum first; silently drops sentences that fail, and
/// returns whether this one passed.
/// After a valid `$GNRMC` update the registered callback is fired if the
/// fix is valid, matching the C driver's behaviour exactly.
///
/// # Safety
/// May mutate `S_GPS` via `process_gnrmc` / `process_gngga`.
unsafe fn nmea_process_sentence(sentence: &[u8]) -> bool {
    if !nmea_verify_checksum(sentence) {
        return false;
    }

    let s = match core::str::from_utf8(sentence) {
        Ok(v) => v,
        Err(_) => return false,
    };

    // Strip trailing CR / LF / space
    let s = s.trim_end_matches(|c: char| c == '\r' || c == '\n' || c == ' ');

    let fields = nmea_split(s);
    if fields.is_empty() {
        return false;
    }

    match fields[0] {
        "$GNRMC" | "$GPRMC" => {
            process_gnrmc(&fields);

            // Fire callback (position + validity are fresh after RMC).
            let gps = &*(&raw const S_GPS);
            if let Some(cb) = gps.cb {
                let snap = gps.last_position;
                if snap.fix_valid {
                    cb(&snap as *const HalGpsPosition, gps.cb_data);
                }
            }
        }
        "$GNGGA" | "$GPGGA" => {
            process_gngga(&fields);
        }
        // Each constellation's under its own talker ID, never GN.
        id if id.len() == 6 && id.ends_with("GSV") => {
            let gps = &mut *(&raw mut S_GPS);
            gps.sky.feed(&fields, xTaskGetTickCount());
        }
        _ => {}
    }
    true
}

/// Take one byte off the wire, assembling NMEA sentences delimited by `$`
/// (start) and `\n` (end) and dispatching each complete one to the parser.
///
/// # Safety
/// Mutates `S_GPS`. Called only from the RX task, or a test.
unsafe fn nmea_feed_byte(byte: u8) {
    let gps = &mut *(&raw mut S_GPS);
    gps.bytes_seen = gps.bytes_seen.saturating_add(1);

    if byte == b'$' {
        gps.nmea_idx = 0;
    }

    if gps.nmea_idx < gps.nmea_buf.len() - 1 {
        gps.nmea_buf[gps.nmea_idx] = byte;
        gps.nmea_idx += 1;
    }

    if byte == b'\n' {
        // Copied out, so the parser's own writes to the driver state do not
        // alias a borrow of the buffer it is reading.
        let end = gps.nmea_idx;
        let mut line = [0u8; 256];
        line[..end].copy_from_slice(&gps.nmea_buf[..end]);
        gps.nmea_idx = 0;

        if nmea_process_sentence(&line[..end]) {
            let gps = &mut *(&raw mut S_GPS);
            gps.sentences_seen = gps.sentences_seen.saturating_add(1);
        }
    }
}

/// Take one byte off the wire: NMEA and UBX come mixed, and neither's
/// framing can start inside the other's text.
///
/// # Safety
/// Mutates `S_GPS` and `S_UBX`. Called only from the reader, or a test.
unsafe fn feed_byte(byte: u8) {
    nmea_feed_byte(byte);
    ubx_feed_byte(byte);
}

// ── UBX ──────────────────────────────────────────────────────────────────────

/// UBX's Fletcher checksum, over the class, ID, length and payload.
fn ubx_checksum(bytes: &[u8]) -> [u8; 2] {
    let (mut a, mut b) = (0u8, 0u8);
    for &x in bytes {
        a = a.wrapping_add(x);
        b = b.wrapping_add(a);
    }
    [a, b]
}

/// Append one UBX frame to `out`.
fn ubx_push(out: &mut Vec<u8>, class: u8, id: u8, payload: &[u8]) {
    let start = out.len();
    out.extend_from_slice(&[0xB5, 0x62, class, id]);
    out.extend_from_slice(&(payload.len() as u16).to_le_bytes());
    out.extend_from_slice(payload);
    let check = ubx_checksum(&out[start + 2..]);
    out.extend_from_slice(&check);
}

/// The whole UBX frames in `data`, in order, those with a bad checksum left
/// out.
fn ubx_frames(data: &[u8]) -> impl Iterator<Item = &[u8]> {
    let mut at = 0;
    std::iter::from_fn(move || {
        while at + 8 <= data.len() {
            if data[at] != 0xB5 || data[at + 1] != 0x62 {
                at += 1;
                continue;
            }
            let end = at + 8 + u16::from_le_bytes([data[at + 4], data[at + 5]]) as usize;
            if end > data.len() {
                break;
            }
            let frame = &data[at..end];
            if ubx_checksum(&frame[2..frame.len() - 2]) == frame[frame.len() - 2..] {
                at = end;
                return Some(frame);
            }
            at += 2;
        }
        None
    })
}

/// Take one byte toward a UBX frame, and once one is whole and checks, what
/// it says: an assistance message's acknowledgement, a configuration's, an
/// identity reply, or the satellites.
///
/// # Safety
/// Mutates `S_UBX`, and `S_GPS` through `take_ack`.
unsafe fn ubx_feed_byte(byte: u8) {
    let ubx = &mut *(&raw mut S_UBX);
    match ubx.len {
        0 => {
            if byte == 0xB5 {
                ubx.frame[0] = byte;
                ubx.len = 1;
            }
            return;
        }
        1 => {
            ubx.len = match byte {
                0x62 => 2,
                0xB5 => 1,
                _ => 0,
            };
            ubx.frame[1] = byte;
            return;
        }
        _ => {}
    }
    ubx.frame[ubx.len] = byte;
    ubx.len += 1;
    if ubx.len < 6 {
        return;
    }
    let total = 8 + u16::from_le_bytes([ubx.frame[4], ubx.frame[5]]) as usize;
    if total > UBX_FRAME_MAX {
        // Nothing this driver reads is this long.
        ubx.len = 0;
        return;
    }
    if ubx.len < total {
        return;
    }
    ubx.len = 0;
    if ubx_checksum(&ubx.frame[2..total - 2]) != ubx.frame[total - 2..total] {
        return;
    }
    match (ubx.frame[2], ubx.frame[3]) {
        (0x13, 0x60) if total == 16 => {
            let mut ack = [0u8; 8];
            ack.copy_from_slice(&ubx.frame[6..14]);
            take_ack(&ack);
        }
        // UBX-ACK-ACK and -NAK, for a UBX-CFG-VALSET.
        (0x05, id @ (0x00 | 0x01)) if total == 10 && ubx.frame[6..8] == [0x06, 0x8A] => {
            take_cfg_answer(id == 0x01);
        }
        (0x27, 0x03) if total <= ubx.unique_id.len() => {
            ubx.unique_id[..total].copy_from_slice(&ubx.frame[..total]);
            ubx.unique_id_len = total;
        }
        (0x0A, 0x04) if total <= ubx.version.len() => {
            ubx.version[..total].copy_from_slice(&ubx.frame[..total]);
            ubx.version_len = total;
        }
        (0x01, 0x35) if total >= 16 => {
            let listed = nav_sat(&ubx.frame[6..total - 2], &mut ubx.sats.sats);
            ubx.sats.count = listed as u16;
            ubx.sats_at = xTaskGetTickCount();
            ubx.sats_fresh = true;
        }
        _ => {}
    }
}

/// UBX-NAV-SAT's payload into `out`: how many satellites it lists, up to
/// what `out` holds.
fn nav_sat(payload: &[u8], out: &mut [HalGpsSat]) -> usize {
    let listed = (payload[5] as usize).min((payload.len() - 8) / 12).min(out.len());
    for (sat, b) in out.iter_mut().zip(payload[8..].chunks_exact(12)).take(listed) {
        let flags = u32::from_le_bytes([b[8], b[9], b[10], b[11]]);
        let elev = b[3] as i8;
        let azim = i16::from_le_bytes([b[4], b[5]]);
        let has = |bit: u32, what: u8| if flags & (1 << bit) != 0 { what } else { 0 };
        *sat = HalGpsSat {
            gnss: match b[0] {
                0 => b'G',
                1 => b'S',
                2 => b'E',
                3 => b'C',
                5 => b'J',
                6 => b'R',
                7 => b'I',
                _ => b'?',
            },
            sv: b[1],
            cno: b[2],
            elev: if (-90..=90).contains(&elev) { elev } else { -128 },
            azim: if (0..=360).contains(&azim) { azim } else { -1 },
            quality: (flags & 0x7) as u8,
            used: flags & 0x8 != 0,
            health: ((flags >> 4) & 0x3) as u8,
            orbit: match (flags >> 8) & 0x7 {
                0 => HAL_GPS_ORBIT_NONE,
                1 => HAL_GPS_ORBIT_EPHEMERIS,
                2 => HAL_GPS_ORBIT_ALMANAC,
                3 => HAL_GPS_ORBIT_PREDICTED,
                4 => HAL_GPS_ORBIT_AUTONOMOUS,
                _ => HAL_GPS_ORBIT_OTHER,
            },
            orbits: has(11, HAL_GPS_HAS_EPHEMERIS)
                | has(12, HAL_GPS_HAS_ALMANAC)
                | has(13, HAL_GPS_HAS_PREDICTED)
                | has(14, HAL_GPS_HAS_AUTONOMOUS),
            reserved: 0,
        };
    }
    listed
}

// ── Assistance ───────────────────────────────────────────────────────────────

/// What `assist` was handed, kept until the receiver talks.
struct Aid {
    utc_us: i64,
    /// The tick the time was read at: it is brought up to date when sent.
    utc_tick: u32,
    utc_acc_ms: u32,
    /// Latitude and longitude in 1e-7 degrees, altitude and accuracy in cm,
    /// as UBX-MGA-INI-POS_LLH has them.
    position: Option<[i32; 3]>,
    position_acc_cm: u32,
    /// The service's messages this receiver takes, whole frames.
    data: Vec<u8>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind {
    Time,
    Position,
    Almanac,
    Orbit,
    Other,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Answer {
    None,
    Taken,
    /// Not used, and UBX-MGA-ACK's infoCode for why.
    Refused(u8),
}

/// One assistance message in `Sending::bytes`.
struct Msg {
    at: usize,
    len: usize,
    kind: Kind,
    /// Its ID and the first four bytes of its payload, which its
    /// acknowledgement repeats.
    key: [u8; 5],
    answer: Answer,
}

/// The assistance being sent: every message, and those still to go.
struct Sending {
    bytes: Vec<u8>,
    msgs: Vec<Msg>,
    queue: Vec<usize>,
    next: usize,
    /// Whether the messages that went unanswered have been sent again.
    retried: bool,
    last_sent: u32,
}

/// UBX-MGA-ACK's infoCode for a receiver not ready for the data yet: worth
/// sending again, unlike the others.
const ACK_NOT_READY: u8 = 5;

/// Days since 1970-01-01 to (year, month, day). Howard Hinnant's algorithm.
fn civil_from_days(days: i64) -> (i64, u32, u32) {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let month = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let year = yoe + era * 400 + if month <= 2 { 1 } else { 0 };
    (year, month, day)
}

/// Of the service's messages, those this receiver takes: the almanacs and
/// anything else but orbit predictions and UBX-MGA-INI, and the orbit
/// predictions for `today`'s UTC date (years since 2000, month, day) of the
/// constellations it predicts. The time and position are sent fresh.
fn kept_messages(data: &[u8], today: Option<(u8, u8, u8)>) -> Vec<u8> {
    let mut kept = Vec::new();
    for frame in ubx_frames(data) {
        if frame[2] != 0x13 {
            continue;
        }
        let payload = &frame[6..frame.len() - 2];
        let keep = match frame[3] {
            0x20 => {
                payload.len() >= 7
                    && ANO_GNSS.contains(&payload[3])
                    && today == Some((payload[4], payload[5], payload[6]))
            }
            0x40 => false,
            _ => true,
        };
        if keep {
            kept.extend_from_slice(frame);
        }
    }
    kept
}

impl Aid {
    fn new(aid: &HalGpsAssist, data: &[u8], tick: u32) -> Aid {
        let today = (aid.utc_us > 0).then(|| {
            let (year, month, day) = civil_from_days(aid.utc_us.div_euclid(86_400_000_000));
            ((year - 2000).clamp(0, 255) as u8, month as u8, day as u8)
        });
        Aid {
            utc_us: aid.utc_us,
            utc_tick: tick,
            utc_acc_ms: aid.utc_acc_ms,
            position: aid.has_position.then(|| {
                [
                    (aid.latitude * 1e7).round() as i32,
                    (aid.longitude * 1e7).round() as i32,
                    (aid.altitude_m * 100.0).round() as i32,
                ]
            }),
            position_acc_cm: aid.position_acc_m.saturating_mul(100),
            data: kept_messages(data, today),
        }
    }
}

/// UBX-MGA-INI-TIME_UTC's payload for `utc_us`, good to `acc_ms`, taken
/// as of its arrival. The leap seconds are left to the receiver.
fn ini_time_utc(utc_us: i64, acc_ms: u32) -> [u8; 24] {
    let secs = utc_us.div_euclid(1_000_000);
    let (year, month, day) = civil_from_days(secs.div_euclid(86_400));
    let of_day = secs.rem_euclid(86_400);
    let mut p = [0u8; 24];
    p[0] = 0x10; // type: UTC time
    p[3] = 0x80; // leapSecs: unknown
    p[4..6].copy_from_slice(&(year as u16).to_le_bytes());
    p[6] = month as u8;
    p[7] = day as u8;
    p[8] = (of_day / 3600) as u8;
    p[9] = (of_day / 60 % 60) as u8;
    p[10] = (of_day % 60) as u8;
    p[12..16].copy_from_slice(&((utc_us.rem_euclid(1_000_000) * 1000) as u32).to_le_bytes());
    p[16..18].copy_from_slice(&((acc_ms / 1000).min(u16::MAX as u32) as u16).to_le_bytes());
    p[20..24].copy_from_slice(&((acc_ms % 1000) * 1_000_000).to_le_bytes());
    p
}

/// UBX-MGA-INI-POS_LLH's payload.
fn ini_pos_llh(llh: [i32; 3], acc_cm: u32) -> [u8; 20] {
    let mut p = [0u8; 20];
    p[0] = 0x01; // type: position, WGS84 latitude, longitude and altitude
    p[4..8].copy_from_slice(&llh[0].to_le_bytes());
    p[8..12].copy_from_slice(&llh[1].to_le_bytes());
    p[12..16].copy_from_slice(&llh[2].to_le_bytes());
    p[16..20].copy_from_slice(&acc_cm.to_le_bytes());
    p
}

impl Sending {
    /// Everything `aid` holds, as sent `now`: the time first, as the
    /// receiver needs it to use the rest, then the position, then the
    /// service's messages.
    fn new(aid: &Aid, now: u32) -> Sending {
        let mut s = Sending {
            bytes: Vec::with_capacity(aid.data.len() + 60),
            msgs: Vec::new(),
            queue: Vec::new(),
            next: 0,
            retried: false,
            last_sent: now,
        };
        if aid.utc_us > 0 {
            let elapsed_us = now.wrapping_sub(aid.utc_tick) as i64 * 1000;
            s.push(0x40, &ini_time_utc(aid.utc_us + elapsed_us, aid.utc_acc_ms));
        }
        if let Some(llh) = aid.position {
            s.push(0x40, &ini_pos_llh(llh, aid.position_acc_cm));
        }
        for frame in ubx_frames(&aid.data) {
            s.push(frame[3], &frame[6..frame.len() - 2]);
        }
        s.queue = (0..s.msgs.len()).collect();
        s
    }

    fn push(&mut self, id: u8, payload: &[u8]) {
        let at = self.bytes.len();
        ubx_push(&mut self.bytes, 0x13, id, payload);
        let kind = match (id, payload.first()) {
            (0x40, Some(0x10)) => Kind::Time,
            (0x40, Some(0x01)) => Kind::Position,
            (0x20, _) => Kind::Orbit,
            (_, Some(0x02)) => Kind::Almanac,
            _ => Kind::Other,
        };
        let mut key = [id, 0, 0, 0, 0];
        for (k, &b) in key[1..].iter_mut().zip(payload) {
            *k = b;
        }
        self.msgs.push(Msg { at, len: self.bytes.len() - at, kind, key, answer: Answer::None });
    }

    /// Of `kind`'s messages, how many were taken, and how many there are.
    fn tally(&self, kind: Kind) -> (u32, u32) {
        let of_kind = self.msgs.iter().filter(|m| m.kind == kind);
        let taken = of_kind.clone().filter(|m| m.answer == Answer::Taken).count();
        (taken as u32, of_kind.count() as u32)
    }

    /// What became of the time or the position, as the log says it.
    fn word(&self, kind: Kind) -> &'static [u8] {
        match self.msgs.iter().find(|m| m.kind == kind).map(|m| m.answer) {
            None => b"none\0",
            Some(Answer::Taken) => b"taken\0",
            Some(Answer::Refused(_)) => b"refused\0",
            Some(Answer::None) => b"unanswered\0",
        }
    }
}

/// An acknowledgement, UBX-MGA-ACK's payload: matched to the first message
/// not yet answered that it repeats the start of.
///
/// # Safety
/// Mutates `S_GPS`.
unsafe fn take_ack(ack: &[u8; 8]) {
    let gps = &mut *(&raw mut S_GPS);
    let Some(s) = gps.sending.as_mut() else {
        return;
    };
    let key = [ack[3], ack[4], ack[5], ack[6], ack[7]];
    if let Some(m) = s.msgs.iter_mut().find(|m| m.answer == Answer::None && m.key == key) {
        m.answer = if ack[0] == 1 { Answer::Taken } else { Answer::Refused(ack[2]) };
    }
}

/// A UBX-CFG-VALSET the reader sent, for its answer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Cfg {
    Signals,
    Autonomous,
    AckAiding,
}

/// Send the receiver a UBX-CFG-VALSET, and await its answer.
///
/// # Safety
/// Mutates `S_GPS`.
unsafe fn send_valset(gps: &mut GpsState, what: Cfg, payload: &[u8]) {
    let mut valset = Vec::with_capacity(8 + payload.len());
    ubx_push(&mut valset, 0x06, 0x8A, payload);
    send(gps, &valset);
    gps.cfg_awaited.push(what);
}

/// The receiver's answer to the oldest UBX-CFG-VALSET awaiting one. Said
/// once a boot if taken, each time if not.
///
/// # Safety
/// Mutates `S_GPS`.
unsafe fn take_cfg_answer(taken: bool) {
    use std::sync::atomic::{AtomicBool, Ordering};
    static SIGNALS_SAID: AtomicBool = AtomicBool::new(false);
    static AUTONOMOUS_SAID: AtomicBool = AtomicBool::new(false);
    let gps = &mut *(&raw mut S_GPS);
    if gps.cfg_awaited.is_empty() {
        return;
    }
    match (gps.cfg_awaited.remove(0), taken) {
        (Cfg::Signals, false) => {
            gps_log!(ESP_LOG_WARN, "the receiver refused GLONASS with BeiDou B1C (UBX-ACK-NAK)")
        }
        (Cfg::Signals, true) if !SIGNALS_SAID.swap(true, Ordering::Relaxed) => {
            gps_log!(ESP_LOG_INFO, "tracking GPS, Galileo, GLONASS and BeiDou B1C")
        }
        (Cfg::Autonomous, false) => {
            gps_log!(ESP_LOG_WARN, "the receiver refused AssistNow Autonomous (UBX-ACK-NAK)")
        }
        (Cfg::Autonomous, true) if !AUTONOMOUS_SAID.swap(true, Ordering::Relaxed) => {
            gps_log!(ESP_LOG_INFO, "AssistNow Autonomous on: the receiver predicts the orbits it decodes")
        }
        _ => {}
    }
}

/// Write `bytes` to the receiver.
///
/// # Safety
/// Reads `S_GPS`.
unsafe fn send(gps: &GpsState, bytes: &[u8]) {
    uart_write_bytes(gps.cfg.uart_num, bytes.as_ptr(), bytes.len());
}

/// What the reader sends a receiver that talks: the signals, once a wake,
/// and a second later, once the GNSS restart they may cause is over,
/// AssistNow Autonomous, the polls `get_identity` and `get_satellites`
/// asked for, and the assistance, a few messages a pass. Once the last has
/// had its time to be acknowledged, those that went unanswered, or found
/// the receiver not ready, go again, once.
///
/// # Safety
/// Mutates `S_GPS`; reads `S_UBX`.
unsafe fn assist_checks(gps: &mut GpsState, now: u32) {
    let talking =
        !gps.standby && gps.wake_tries == 0 && gps.sentences_seen != gps.sentences_at_wake;
    if !talking {
        return;
    }

    if !gps.signals_sent {
        send_valset(gps, Cfg::Signals, &VALSET_SIGNALS);
        gps.signals_sent = true;
        gps.signals_at = now;
        return;
    }
    if now.wrapping_sub(gps.signals_at) < SIGNALS_SETTLE_TICKS {
        return;
    }
    // An answer not in by now is lost; the next would be taken for it.
    if gps.cfg_awaited.first() == Some(&Cfg::Signals) {
        gps.cfg_awaited.remove(0);
    }
    if !gps.autonomous_sent {
        send_valset(gps, Cfg::Autonomous, &VALSET_AUTONOMOUS);
        gps.autonomous_sent = true;
    }
    if std::mem::take(&mut gps.sats_wanted) {
        send(gps, &UBX_POLL_NAV_SAT);
    }

    let ubx = &*(&raw const S_UBX);
    if gps.identity_wanted && (ubx.unique_id_len == 0 || ubx.version_len == 0) {
        let due = gps.identity_polls == 0
            || now.wrapping_sub(gps.identity_polled) >= IDENTITY_RETRY_TICKS;
        if due && gps.identity_polls < IDENTITY_POLLS {
            send(gps, &UBX_POLL_SEC_UNIQID);
            send(gps, &UBX_POLL_MON_VER);
            gps.identity_polls += 1;
            gps.identity_polled = now;
        }
    }

    if gps.sending.is_none() {
        let Some(aid) = gps.aid.take() else {
            return;
        };
        send_valset(gps, Cfg::AckAiding, &VALSET_ACK_AIDING);
        gps.sending = Some(Box::new(Sending::new(&aid, now)));
    }
    let Some(s) = gps.sending.as_mut() else {
        return;
    };

    let mut budget = ASSIST_BUDGET;
    loop {
        while let Some(&i) = s.queue.get(s.next) {
            let (at, len) = (s.msgs[i].at, s.msgs[i].len);
            if len > budget && budget < ASSIST_BUDGET {
                return;
            }
            uart_write_bytes(gps.cfg.uart_num, s.bytes[at..].as_ptr(), len);
            budget = budget.saturating_sub(len);
            s.next += 1;
            s.last_sent = now;
        }
        if now.wrapping_sub(s.last_sent) < ACK_WAIT_TICKS || s.retried {
            break;
        }
        s.retried = true;
        // With no answer at all, the receiver is not acknowledging: going
        // again would only say the same.
        let again: Vec<usize> = (0..s.msgs.len())
            .filter(|&i| matches!(s.msgs[i].answer, Answer::None | Answer::Refused(ACK_NOT_READY)))
            .collect();
        if again.is_empty() || again.len() == s.msgs.len() {
            break;
        }
        s.queue = again;
        s.next = 0;
    }
    if now.wrapping_sub(s.last_sent) < ACK_WAIT_TICKS {
        return;
    }

    let (almanacs, almanacs_of) = s.tally(Kind::Almanac);
    let (orbits, orbits_of) = s.tally(Kind::Orbit);
    gps_log!(
        ESP_LOG_INFO,
        "assistance: time %s, position %s, %u of %u almanacs and %u of %u orbits taken",
        s.word(Kind::Time).as_ptr(),
        s.word(Kind::Position).as_ptr(),
        almanacs,
        almanacs_of,
        orbits,
        orbits_of
    );
    if let Some(code) = s.msgs.iter().find_map(|m| match m.answer {
        Answer::Refused(code) => Some(code),
        _ => None,
    }) {
        gps_log!(
            ESP_LOG_WARN,
            "assistance: %u of %u refused, the first for reason %u (UBX-MGA-ACK)",
            s.msgs.iter().filter(|m| matches!(m.answer, Answer::Refused(_))).count() as u32,
            s.msgs.len() as u32,
            code as u32
        );
    }
    gps.sending = None;
}

// ── UART reader ──────────────────────────────────────────────────────────────

/// The reader, a job on the kernel worker every `RX_POLL`: feeds what the UART
/// has buffered to the sentence assembler, then makes `reader_checks`.
///
/// # Safety
/// Accesses `S_GPS` directly. Runs only as the job `mia_m10q_enable` starts.
pub unsafe extern "C" fn gps_rx_job(_arg: *mut c_void) {
    // Host builds have no UART, and the tests own `S_GPS`.
    #[cfg(target_os = "espidf")]
    {
        let mut chunk = [0u8; RX_CHUNK];
        loop {
            let len = uart_read_bytes(
                (*(&raw const S_GPS)).cfg.uart_num,
                chunk.as_mut_ptr(),
                RX_CHUNK as u32,
                0,
            );
            if len <= 0 {
                break;
            }
            for &byte in &chunk[..len as usize] {
                feed_byte(byte);
            }
        }
        reader_checks(xTaskGetTickCount());
    }
}


/// What the reader decides once the UART is drained, `now` in ticks: whether
/// a receiver just woken needs waking again; once, whether the receiver is
/// talking; until the first fix, what it hears; and what of the assistance
/// and the identity polls goes next.
///
/// Both outcomes of the second get a line. Before it had either, a receiver
/// that never spoke looked exactly like one still waiting for a fix. The
/// third tells one that needs a better view of the sky from one that hears
/// nothing: the satellites in use stay at 0 in both until the fix.
///
/// # Safety
/// Mutates `S_GPS`.
unsafe fn reader_checks(now: u32) {
    let gps = &mut *(&raw mut S_GPS);
    let elapsed = now.wrapping_sub(gps.rx_started);

    if gps.wake_tries > 0 {
        if gps.sentences_seen != gps.sentences_at_wake {
            if gps.wake_tries > 1 {
                gps_log!(
                    ESP_LOG_INFO,
                    "receiver woke on try %u at waking it",
                    gps.wake_tries as u32
                );
            }
            gps.wake_tries = 0;
        } else if now.wrapping_sub(gps.wake_sent) >= WAKE_RETRY_TICKS {
            if gps.wake_tries < WAKE_TRIES {
                send_wake(gps, now);
            } else {
                gps_log!(
                    ESP_LOG_WARN,
                    "receiver silent after %u tries at waking it",
                    gps.wake_tries as u32
                );
                gps.wake_tries = 0;
            }
        }
    }

    if !gps.link_reported {
        if gps.sentences_seen > 0 {
            gps_log!(
                ESP_LOG_INFO,
                "receiver talking on UART%d at %u baud",
                gps.cfg.uart_num,
                gps.cfg.baud_rate
            );
            gps.link_reported = true;
        } else if elapsed >= SILENCE_WARN_TICKS {
            gps_log!(
                ESP_LOG_WARN,
                "no NMEA on UART%d at %u baud after 5 s (%u bytes received)",
                gps.cfg.uart_num,
                gps.cfg.baud_rate,
                gps.bytes_seen
            );
            gps.link_reported = true;
        }
    }

    assist_checks(gps, now);

    if gps.fix_reported || gps.sentences_seen == 0 {
        return;
    }
    let sky = gps.sky.summary(now);
    if gps.last_position.fix_valid {
        gps_log!(
            ESP_LOG_INFO,
            "fix after %u s with %u satellites, best signal %u dB-Hz",
            elapsed / 1000,
            gps.last_position.satellites as u32,
            sky.best_cno as u32
        );
        gps.fix_reported = true;
    } else if elapsed >= gps.next_sky_log {
        gps_log!(
            ESP_LOG_INFO,
            "no fix after %u s: %u satellites in view, %u heard, best %u dB-Hz",
            elapsed / 1000,
            sky.in_view as u32,
            sky.heard as u32,
            sky.best_cno as u32
        );
        gps.next_sky_log = gps
            .next_sky_log
            .saturating_add(gps.next_sky_log.min(SKY_LOG_MAX_GAP_TICKS));
    }
}

// ── The receiver's power ─────────────────────────────────────────────────────

/// Send the receiver to standby, unless it is there already: anything sent
/// to a receiver in standby wakes it, and the command would be lost with it.
/// What it reported goes stale from here.
///
/// # Safety
/// Mutates `S_GPS`.
unsafe fn enter_standby(gps: &mut GpsState) -> i32 {
    if gps.standby {
        return ESP_OK;
    }
    let written = uart_write_bytes(
        gps.cfg.uart_num,
        UBX_RXM_PMREQ_STANDBY.as_ptr(),
        UBX_RXM_PMREQ_STANDBY.len(),
    );
    if written < 0 {
        return ESP_FAIL;
    }
    // Out on the wire before the caller sleeps: a UART stopped halfway
    // through would leave the receiver half a command, and running.
    uart_wait_tx_done(gps.cfg.uart_num, 100);

    gps.standby = true;
    gps.wake_tries = 0;
    gps.last_position.fix_valid = false;
    gps.last_position.satellites = 0;
    gps.sky = Sky::new();
    // What was not sent yet is for a start that is over; the kernel hands
    // the next one its own.
    gps.aid = None;
    gps.sending = None;
    gps.cfg_awaited.clear();
    gps.sats_wanted = false;
    (*(&raw mut S_UBX)).sats_fresh = false;
    ESP_OK
}

/// Send the receiver the edge that wakes it, and watch for it to talk.
///
/// # Safety
/// Mutates `S_GPS`.
unsafe fn send_wake(gps: &mut GpsState, now: u32) {
    uart_write_bytes(gps.cfg.uart_num, WAKE_PULSE.as_ptr(), WAKE_PULSE.len());
    gps.standby = false;
    gps.wake_tries += 1;
    gps.wake_sent = now;
    gps.sentences_at_wake = gps.sentences_seen;
}

/// Wake the receiver, and time the next fix from now.
///
/// # Safety
/// Mutates `S_GPS`.
unsafe fn wake_receiver(gps: &mut GpsState, now: u32) {
    gps.wake_tries = 0;
    gps.signals_sent = false;
    gps.autonomous_sent = false;
    gps.cfg_awaited.clear();
    gps.identity_polls = 0;
    (*(&raw mut S_UBX)).sats_fresh = false;
    send_wake(gps, now);
    gps.rx_started = now;
    gps.fix_reported = false;
    gps.next_sky_log = SKY_LOG_FIRST_TICKS;
}

// ── vtable implementations ───────────────────────────────────────────────────

/// Initialise the MIA-M10Q UART peripheral.
///
/// Configures baud rate / pins / driver but does **not** start the RX task.
/// Call `mia_m10q_enable()` to begin receiving NMEA data.
///
/// # Safety
/// `config` must point to a valid `GpsMiaM10qConfig`.
unsafe extern "C" fn mia_m10q_init(config: *const c_void) -> i32 {
    if config.is_null() {
        return ESP_ERR_INVALID_ARG;
    }

    let gps = &mut *(&raw mut S_GPS);

    if gps.initialized {
        return ESP_ERR_INVALID_STATE;
    }

    let src = &*(config as *const GpsMiaM10qConfig);
    gps.cfg.uart_num = src.uart_num;
    gps.cfg.pin_tx = src.pin_tx;
    gps.cfg.pin_rx = src.pin_rx;
    gps.cfg.baud_rate = src.baud_rate;

    let baud = if gps.cfg.baud_rate == 0 {
        9600
    } else {
        gps.cfg.baud_rate
    };
    gps.cfg.baud_rate = baud;

    #[cfg(target_os = "espidf")]
    {
        let uart_cfg = UartConfig {
            baud_rate: baud as i32,
            data_bits: 3, // UART_DATA_8_BITS
            parity: 0,    // UART_PARITY_DISABLE
            stop_bits: 1, // UART_STOP_BITS_1
            flow_ctrl: 0, // UART_HW_FLOWCTRL_DISABLE
            rx_flow_ctrl_thresh: 0,
            source_clk: 0, // UART_SCLK_DEFAULT
            // No sleep retention asked for: this receiver is powered from a
            // rail the board profile raises, not from a domain kept alive
            // across light sleep.
            flags: 0,
        };
        let ret = uart_param_config(
            gps.cfg.uart_num,
            &uart_cfg as *const UartConfig as *const c_void,
        );
        if ret != ESP_OK {
            return ret;
        }
    }
    #[cfg(not(target_os = "espidf"))]
    {
        let _ = baud;
        let ret = uart_param_config(gps.cfg.uart_num, std::ptr::null());
        if ret != ESP_OK {
            return ret;
        }
    }

    let ret = uart_set_pin(
        gps.cfg.uart_num,
        gps.cfg.pin_tx,
        gps.cfg.pin_rx,
        -1, // UART_PIN_NO_CHANGE
        -1,
    );
    if ret != ESP_OK {
        return ret;
    }

    let ret = uart_driver_install(
        gps.cfg.uart_num,
        UART_RX_BUF_SIZE,
        0,
        0,
        std::ptr::null_mut(),
        0,
    );
    if ret != ESP_OK {
        return ret;
    }

    gps.last_position = HalGpsPosition {
        latitude: 0.0,
        longitude: 0.0,
        altitude_m: 0.0,
        speed_kmh: 0.0,
        heading_deg: 0.0,
        satellites: 0,
        fix_valid: false,
        timestamp: 0,
    };
    gps.nmea_idx = 0;
    gps.rx_job = 0;
    gps.enabled = false;
    gps.standby = true;
    gps.wake_tries = 0;
    gps.sky = Sky::new();
    gps.signals_sent = false;
    gps.autonomous_sent = false;
    gps.cfg_awaited.clear();
    gps.sats_wanted = false;
    gps.identity_wanted = false;
    gps.aid = None;
    gps.sending = None;
    gps.initialized = true;

    ESP_OK
}

/// De-initialise the driver.
///
/// Stops the reader if running and removes the UART driver.
///
/// # Safety
/// Must be called from the same context as `mia_m10q_init`.
unsafe extern "C" fn mia_m10q_deinit() {
    let gps = &mut *(&raw mut S_GPS);

    if !gps.initialized {
        return;
    }

    stop_reader(gps);

    uart_driver_delete(gps.cfg.uart_num);

    gps.initialized = false;
    gps.enabled = false;
}

/// Wake the receiver and start the reader.
///
/// # Safety
/// Driver must be initialised.
unsafe extern "C" fn mia_m10q_enable() -> i32 {
    let gps = &mut *(&raw mut S_GPS);

    if !gps.initialized {
        return ESP_ERR_INVALID_STATE;
    }
    if gps.enabled {
        return ESP_OK;
    }

    // What came in while nobody read is from before: an old RMC would show
    // as a fix.
    uart_flush_input(gps.cfg.uart_num);
    gps.nmea_idx = 0;
    gps.bytes_seen = 0;
    gps.sentences_seen = 0;
    gps.link_reported = false;
    // In standby, or it may be: switched off, or left there by a restart,
    // which does not take its power away.
    wake_receiver(gps, xTaskGetTickCount());
    gps.rx_job = crate::kworker::every(gps_rx_job, std::ptr::null_mut(), RX_POLL);

    gps.enabled = true;
    ESP_OK
}

/// Stop the reader. Once this returns it is not running and will not run.
unsafe fn stop_reader(gps: &mut GpsState) {
    if gps.rx_job != 0 {
        crate::kworker::cancel(gps.rx_job);
        gps.rx_job = 0;
    }
}

/// Stop the reader and send the receiver to standby, where it draws next to
/// nothing and keeps what it knows of the sky.
///
/// # Safety
/// Driver must be initialised.
unsafe extern "C" fn mia_m10q_disable() -> i32 {
    let gps = &mut *(&raw mut S_GPS);

    if !gps.initialized {
        return ESP_ERR_INVALID_STATE;
    }
    if !gps.enabled {
        return ESP_OK;
    }

    stop_reader(gps);
    gps.enabled = false;
    // Off is the reader stopped. A receiver that missed its standby only
    // costs power.
    let _ = enter_standby(gps);
    ESP_OK
}

/// Copy the last known GPS position into `*pos`.
///
/// Returns `ESP_OK` whenever the receiver is being read, fix or not;
/// `fix_valid` says which. Satellites in use are worth showing while a fix is
/// still being acquired, and the callers — Navigator, Settings, the simulator's
/// driver — all read the struct that way. `ESP_ERR_INVALID_STATE` means nobody
/// is reading the receiver, so whatever is in the struct is stale.
///
/// # Safety
/// `pos` must be a writable `HalGpsPosition`.
unsafe extern "C" fn mia_m10q_get_position(pos: *mut HalGpsPosition) -> i32 {
    if pos.is_null() {
        return ESP_ERR_INVALID_ARG;
    }

    let gps = &*(&raw const S_GPS);
    *pos = gps.last_position;

    if gps.initialized && gps.enabled {
        ESP_OK
    } else {
        ESP_ERR_INVALID_STATE
    }
}

/// Register a callback invoked on each valid fix.
///
/// # Safety
/// `cb` must remain valid until it is replaced or the driver is de-initialised.
unsafe extern "C" fn mia_m10q_register_callback(cb: HalGpsCb, user_data: *mut c_void) -> i32 {
    let gps = &mut *(&raw mut S_GPS);
    gps.cb = cb;
    gps.cb_data = user_data;
    ESP_OK
}

/// The last few seconds' sky. `ESP_OK` exactly when `get_position` is.
///
/// # Safety
/// `sky` must be a writable `HalGpsSky`.
unsafe extern "C" fn mia_m10q_get_sky(sky: *mut HalGpsSky) -> i32 {
    if sky.is_null() {
        return ESP_ERR_INVALID_ARG;
    }

    let gps = &*(&raw const S_GPS);
    if gps.initialized && gps.enabled {
        *sky = gps.sky.summary(xTaskGetTickCount());
        ESP_OK
    } else {
        *sky = HalGpsSky::default();
        ESP_ERR_INVALID_STATE
    }
}

/// Take assistance for the receiver's start: sent once it talks, the time
/// and position made current then. A newer one replaces what is left of an
/// older one; a receiver with a fix has no use for any.
///
/// # Safety
/// `aid` must be a readable `HalGpsAssist`, its `data` `data_len` bytes.
unsafe extern "C" fn mia_m10q_assist(aid: *const HalGpsAssist) -> i32 {
    if aid.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let gps = &mut *(&raw mut S_GPS);
    if !gps.initialized {
        return ESP_ERR_INVALID_STATE;
    }
    if gps.last_position.fix_valid {
        return ESP_OK;
    }
    let aid = &*aid;
    let data = if aid.data.is_null() || aid.data_len == 0 {
        &[][..]
    } else {
        std::slice::from_raw_parts(aid.data, aid.data_len)
    };
    gps.aid = Some(Box::new(Aid::new(aid, data, xTaskGetTickCount())));
    gps.sending = None;
    ESP_OK
}

/// The receiver's UBX-SEC-UNIQID and UBX-MON-VER replies. Until it has
/// answered, `ESP_ERR_NOT_FOUND`, and the reader polls it for them while it
/// talks.
///
/// # Safety
/// `id` must be a writable `HalGpsIdentity`.
unsafe extern "C" fn mia_m10q_get_identity(id: *mut HalGpsIdentity) -> i32 {
    if id.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let gps = &mut *(&raw mut S_GPS);
    if !gps.initialized {
        return ESP_ERR_INVALID_STATE;
    }
    let ubx = &*(&raw const S_UBX);
    if ubx.unique_id_len == 0 || ubx.version_len == 0 {
        gps.identity_wanted = true;
        return ESP_ERR_NOT_FOUND;
    }
    let out = &mut *id;
    *out = HalGpsIdentity::EMPTY;
    out.unique_id[..ubx.unique_id_len].copy_from_slice(&ubx.unique_id[..ubx.unique_id_len]);
    out.unique_id_len = ubx.unique_id_len as u16;
    out.version[..ubx.version_len].copy_from_slice(&ubx.version[..ubx.version_len]);
    out.version_len = ubx.version_len as u16;
    gps.identity_wanted = false;
    ESP_OK
}

/// The receiver's last UBX-NAV-SAT, and a poll for the next: until one has
/// come since the last wake, `ESP_ERR_NOT_FOUND`. `ESP_ERR_INVALID_STATE`
/// while nobody reads the receiver.
///
/// # Safety
/// `sats` must be a writable `HalGpsSats`. Called on the kernel worker, as
/// the reader is.
unsafe extern "C" fn mia_m10q_get_satellites(sats: *mut HalGpsSats) -> i32 {
    if sats.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let gps = &mut *(&raw mut S_GPS);
    if !gps.initialized || !gps.enabled {
        return ESP_ERR_INVALID_STATE;
    }
    gps.sats_wanted = true;
    let ubx = &*(&raw const S_UBX);
    if !ubx.sats_fresh {
        return ESP_ERR_NOT_FOUND;
    }
    let out = &mut *sats;
    *out = ubx.sats;
    out.age_ms = xTaskGetTickCount().wrapping_sub(ubx.sats_at);
    ESP_OK
}

/// Send the receiver to standby while the device sleeps, or wake it after.
///
/// Waking does not wait for it: the reader watches for it to talk and wakes
/// it again if it does not. One switched off stays in standby.
///
/// # Safety
/// Driver must be initialised.
unsafe extern "C" fn mia_m10q_sleep(enter: bool) -> i32 {
    let gps = &mut *(&raw mut S_GPS);

    if !gps.initialized {
        return ESP_ERR_INVALID_STATE;
    }

    if enter {
        enter_standby(gps)
    } else {
        if gps.enabled && gps.standby {
            wake_receiver(gps, xTaskGetTickCount());
        }
        ESP_OK
    }
}

// ── HAL vtable ────────────────────────────────────────────────────────────────

/// Static HAL GPS driver vtable for the MIA-M10Q.
///
/// Pass to `hal_gps_register()`. Returned by `drv_gps_mia_m10q_get()`.
static GPS_DRIVER: HalGpsDriver = HalGpsDriver {
    init: Some(mia_m10q_init),
    deinit: Some(mia_m10q_deinit),
    enable: Some(mia_m10q_enable),
    disable: Some(mia_m10q_disable),
    get_position: Some(mia_m10q_get_position),
    register_callback: Some(mia_m10q_register_callback),
    sleep: Some(mia_m10q_sleep),
    name: b"MIA-M10Q\0".as_ptr() as *const c_char,
    get_sky: Some(mia_m10q_get_sky),
    assist: Some(mia_m10q_assist),
    get_identity: Some(mia_m10q_get_identity),
    get_satellites: Some(mia_m10q_get_satellites),
};

/// Return the MIA-M10Q driver vtable.
///
/// Drop-in replacement for the C `drv_gps_mia_m10q_get()`.
///
/// # Safety
/// Returns a pointer to a `'static` — safe to call from C.
#[no_mangle]
pub extern "C" fn drv_gps_mia_m10q_get() -> *const HalGpsDriver {
    &GPS_DRIVER
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    /// Reset driver state between tests.
    unsafe fn reset_state() {
        *(&raw mut S_GPS) = GpsState::new();
        let ubx = &mut *(&raw mut S_UBX);
        ubx.len = 0;
        ubx.unique_id_len = 0;
        ubx.version_len = 0;
        ubx.sats_fresh = false;
        sent();
    }

    /// What the driver has sent the receiver since this was last asked.
    fn sent() -> Vec<u8> {
        std::mem::take(&mut *UART_SENT.lock().unwrap_or_else(|e| e.into_inner()))
    }

    // ── nmea_verify_checksum ──────────────────────────────────────────────────

    #[test]
    fn test_checksum_valid_gnrmc() {
        // XOR of bytes in "GNRMC,092751.000,A,5321.6802,N,00630.3372,W,0.06,31.66,280511,,,A" = 0x58
        let s = b"$GNRMC,092751.000,A,5321.6802,N,00630.3372,W,0.06,31.66,280511,,,A*58";
        assert!(nmea_verify_checksum(s));
    }

    #[test]
    fn test_checksum_valid_gngga() {
        // XOR of bytes in "GNGGA,092751.000,5321.6802,N,00630.3372,W,1,08,1.03,61.7,M,55.3,M,," = 0x58
        let s = b"$GNGGA,092751.000,5321.6802,N,00630.3372,W,1,08,1.03,61.7,M,55.3,M,,*58";
        assert!(nmea_verify_checksum(s));
    }

    #[test]
    fn test_checksum_wrong_value() {
        let s = b"$GNRMC,092751.000,A,5321.6802,N,00630.3372,W,0.06,31.66,280511,,,A*FF";
        assert!(!nmea_verify_checksum(s));
    }

    #[test]
    fn test_checksum_missing_star() {
        let s = b"$GNRMC,092751.000,A";
        assert!(!nmea_verify_checksum(s));
    }

    #[test]
    fn test_checksum_empty_input() {
        assert!(!nmea_verify_checksum(b""));
    }

    #[test]
    fn test_checksum_no_leading_dollar() {
        assert!(!nmea_verify_checksum(b"GNRMC,test*00"));
    }

    #[test]
    fn test_checksum_minimal_valid() {
        // '$' then 'A' then '*41' — XOR of 'A' (0x41) == 0x41.
        let s = b"$A*41";
        assert!(nmea_verify_checksum(s));
    }

    // ── nmea_parse_coord ──────────────────────────────────────────────────────

    #[test]
    fn test_coord_north() {
        // 5321.6802 N => 53 + 21.6802/60
        let dd = nmea_parse_coord("5321.6802", "N");
        let expected = 53.0 + 21.6802 / 60.0;
        assert!((dd - expected).abs() < 1e-6, "got {dd}, want {expected}");
    }

    #[test]
    fn test_coord_south() {
        let dd = nmea_parse_coord("3400.0000", "S");
        let expected = -34.0;
        assert!((dd - expected).abs() < 1e-6);
    }

    #[test]
    fn test_coord_west() {
        // 00630.3372 W => -(6 + 30.3372/60)
        let dd = nmea_parse_coord("00630.3372", "W");
        let expected = -(6.0 + 30.3372 / 60.0);
        assert!((dd - expected).abs() < 1e-6);
    }

    #[test]
    fn test_coord_east() {
        let dd = nmea_parse_coord("01200.0000", "E");
        assert!((dd - 12.0).abs() < 1e-6);
    }

    #[test]
    fn test_coord_empty_field() {
        assert_eq!(nmea_parse_coord("", "N"), 0.0);
    }

    #[test]
    fn test_coord_zero() {
        assert_eq!(nmea_parse_coord("0000.0000", "N"), 0.0);
    }

    // ── nmea_to_timestamp ─────────────────────────────────────────────────────

    #[test]
    fn test_timestamp_unix_epoch() {
        // 1970-01-01 00:00:00 UTC
        // yy=70 → year=1970 (yy >= 70 maps to 1900+yy, matching C mktime convention)
        let ts = nmea_to_timestamp("000000", "010170");
        assert_eq!(ts, 0);
    }

    #[test]
    fn test_timestamp_known_value() {
        // 2011-05-28 09:27:51 UTC
        // Hand-computed: 14975 days (1970→2011) + 147 days (Jan–May28) = 15122 days
        // 15122 × 86400 + 9×3600 + 27×60 + 51 = 1306574871
        let ts = nmea_to_timestamp("092751", "280511");
        assert_eq!(ts, 1306574871);
    }

    #[test]
    fn test_timestamp_empty_time() {
        assert_eq!(nmea_to_timestamp("", "280511"), 0);
    }

    #[test]
    fn test_timestamp_empty_date() {
        assert_eq!(nmea_to_timestamp("092751", ""), 0);
    }

    #[test]
    fn test_timestamp_sub_seconds_ignored() {
        // hhmmss.ss — only the first 6 bytes matter
        let ts1 = nmea_to_timestamp("092751.00", "280511");
        let ts2 = nmea_to_timestamp("092751", "280511");
        assert_eq!(ts1, ts2);
    }

    #[test]
    fn test_timestamp_leap_year_2000() {
        // 2000-02-29 00:00:00 UTC (2000 is a leap year)
        // python3: calendar.timegm((2000,2,29,0,0,0,0,0,0)) = 951782400
        let ts = nmea_to_timestamp("000000", "290200");
        assert_eq!(ts, 951782400);
    }

    #[test]
    fn test_timestamp_day_boundary_2020() {
        // 2020-01-01 00:00:00 UTC
        // python3: calendar.timegm((2020,1,1,0,0,0,0,0,0)) = 1577836800
        let ts = nmea_to_timestamp("000000", "010120");
        assert_eq!(ts, 1577836800);
    }

    // ── nmea_split ────────────────────────────────────────────────────────────

    #[test]
    fn test_split_basic() {
        let fields = nmea_split("$GNRMC,time,A,lat,N,lon,W");
        assert_eq!(fields[0], "$GNRMC");
        assert_eq!(fields[2], "A");
    }

    #[test]
    fn test_split_strips_checksum_suffix() {
        let fields = nmea_split("$GNRMC,a,b*2B");
        assert_eq!(fields.len(), 3);
        assert_eq!(fields[2], "b");
    }

    // ── process_gnrmc ─────────────────────────────────────────────────────────

    #[test]
    fn test_process_gnrmc_updates_position() {
        unsafe {
            reset_state();
            // Sentence with valid checksum (0x58 — same body as test_checksum_valid_gnrmc).
            let s = b"$GNRMC,092751.000,A,5321.6802,N,00630.3372,W,0.06,31.66,280511,,,A*58\n";
            nmea_process_sentence(s);

            let pos = (*(&raw const S_GPS)).last_position;
            assert!(pos.fix_valid, "expected fix_valid = true");

            let expected_lat = 53.0 + 21.6802 / 60.0;
            let expected_lon = -(6.0 + 30.3372 / 60.0);
            assert!((pos.latitude - expected_lat).abs() < 1e-5);
            assert!((pos.longitude - expected_lon).abs() < 1e-5);

            let expected_speed = 0.06_f32 * KNOTS_TO_KMH;
            assert!((pos.speed_kmh - expected_speed).abs() < 1e-4);
            assert!((pos.heading_deg - 31.66).abs() < 1e-3);
        }
    }

    #[test]
    fn test_process_gnrmc_void_status_clears_fix() {
        unsafe {
            reset_state();
            (*(&raw mut S_GPS)).last_position.fix_valid = true;

            let fields: Vec<&str> = vec![
                "$GNRMC",
                "000000.000",
                "V",
                "0000.0000",
                "N",
                "00000.0000",
                "E",
                "0.00",
                "0.00",
                "010170",
                "",
                "",
                "N",
            ];
            process_gnrmc(&fields);

            assert!(!(*(&raw const S_GPS)).last_position.fix_valid);
        }
    }

    // ── process_gngga ─────────────────────────────────────────────────────────

    #[test]
    fn test_process_gngga_updates_altitude_and_sats() {
        unsafe {
            reset_state();

            let fields: Vec<&str> = vec![
                "$GNGGA",
                "092751.000",
                "5321.6802",
                "N",
                "00630.3372",
                "W",
                "1",
                "8",
                "1.03",
                "61.7",
                "M",
                "55.3",
                "M",
                "",
                "",
            ];
            process_gngga(&fields);

            let pos = (*(&raw const S_GPS)).last_position;
            assert_eq!(pos.satellites, 8);
            assert!((pos.altitude_m - 61.7).abs() < 0.01);
        }
    }

    #[test]
    fn test_process_gngga_quality_zero_clears_fix() {
        unsafe {
            reset_state();
            (*(&raw mut S_GPS)).last_position.fix_valid = true;

            let fields: Vec<&str> = vec![
                "$GNGGA",
                "000000.000",
                "0000.0000",
                "N",
                "00000.0000",
                "W",
                "0",
                "0",
                "99.0",
                "0.0",
                "M",
                "0.0",
                "M",
                "",
                "",
            ];
            process_gngga(&fields);

            assert!(!(*(&raw const S_GPS)).last_position.fix_valid);
        }
    }

    // ── vtable ────────────────────────────────────────────────────────────────

    #[test]
    fn test_vtable_pointer_non_null() {
        assert!(!drv_gps_mia_m10q_get().is_null());
    }

    #[test]
    fn test_vtable_fields_populated() {
        let drv = unsafe { &*drv_gps_mia_m10q_get() };
        assert!(drv.init.is_some());
        assert!(drv.deinit.is_some());
        assert!(drv.enable.is_some());
        assert!(drv.disable.is_some());
        assert!(drv.get_position.is_some());
        assert!(drv.register_callback.is_some());
        assert!(drv.sleep.is_some());
        assert!(!drv.name.is_null());
    }

    // ── init / deinit ─────────────────────────────────────────────────────────

    #[test]
    fn test_init_null_config_returns_invalid_arg() {
        unsafe {
            reset_state();
            assert_eq!(mia_m10q_init(std::ptr::null()), ESP_ERR_INVALID_ARG);
        }
    }

    #[test]
    fn test_init_and_deinit_cycle() {
        unsafe {
            reset_state();
            let cfg = GpsMiaM10qConfig {
                uart_num: 1,
                pin_tx: 10,
                pin_rx: 9,
                baud_rate: 9600,
            };
            assert_eq!(
                mia_m10q_init(&cfg as *const GpsMiaM10qConfig as *const c_void),
                ESP_OK
            );
            assert!((*(&raw const S_GPS)).initialized);
            mia_m10q_deinit();
            assert!(!(*(&raw const S_GPS)).initialized);
        }
    }

    #[test]
    fn test_double_init_returns_invalid_state() {
        unsafe {
            reset_state();
            let cfg = GpsMiaM10qConfig {
                uart_num: 1,
                pin_tx: 10,
                pin_rx: 9,
                baud_rate: 9600,
            };
            let p = &cfg as *const GpsMiaM10qConfig as *const c_void;
            assert_eq!(mia_m10q_init(p), ESP_OK);
            assert_eq!(mia_m10q_init(p), ESP_ERR_INVALID_STATE);
            mia_m10q_deinit();
        }
    }

    #[test]
    fn test_enable_before_init_returns_invalid_state() {
        unsafe {
            reset_state();
            assert_eq!(mia_m10q_enable(), ESP_ERR_INVALID_STATE);
        }
    }

    #[test]
    fn test_enable_disable_cycle() {
        unsafe {
            reset_state();
            let cfg = GpsMiaM10qConfig {
                uart_num: 1,
                pin_tx: 10,
                pin_rx: 9,
                baud_rate: 9600,
            };
            mia_m10q_init(&cfg as *const GpsMiaM10qConfig as *const c_void);
            assert_eq!(mia_m10q_enable(), ESP_OK);
            assert!((*(&raw const S_GPS)).enabled);
            assert_eq!(mia_m10q_disable(), ESP_OK);
            assert!(!(*(&raw const S_GPS)).enabled);
            mia_m10q_deinit();
        }
    }

    #[test]
    fn test_double_enable_is_idempotent() {
        unsafe {
            reset_state();
            let cfg = GpsMiaM10qConfig {
                uart_num: 1,
                pin_tx: 10,
                pin_rx: 9,
                baud_rate: 9600,
            };
            mia_m10q_init(&cfg as *const GpsMiaM10qConfig as *const c_void);
            assert_eq!(mia_m10q_enable(), ESP_OK);
            assert_eq!(mia_m10q_enable(), ESP_OK);
            mia_m10q_deinit();
        }
    }

    #[test]
    fn test_disable_before_enable_is_ok() {
        unsafe {
            reset_state();
            let cfg = GpsMiaM10qConfig {
                uart_num: 1,
                pin_tx: 10,
                pin_rx: 9,
                baud_rate: 9600,
            };
            mia_m10q_init(&cfg as *const GpsMiaM10qConfig as *const c_void);
            assert_eq!(mia_m10q_disable(), ESP_OK);
            mia_m10q_deinit();
        }
    }

    #[test]
    fn test_deinit_noop_when_not_initialized() {
        unsafe {
            reset_state();
            mia_m10q_deinit(); // must not panic
            assert!(!(*(&raw const S_GPS)).initialized);
        }
    }

    // ── get_position ──────────────────────────────────────────────────────────

    #[test]
    fn test_get_position_null_returns_invalid_arg() {
        unsafe {
            assert_eq!(
                mia_m10q_get_position(std::ptr::null_mut()),
                ESP_ERR_INVALID_ARG
            );
        }
    }

    fn empty_position() -> HalGpsPosition {
        HalGpsPosition {
            latitude: 0.0,
            longitude: 0.0,
            altitude_m: 0.0,
            speed_kmh: 0.0,
            heading_deg: 0.0,
            satellites: 0,
            fix_valid: false,
            timestamp: 0,
        }
    }

    /// Initialise and enable the driver, as the boot sequence does.
    unsafe fn start_receiver() {
        reset_state();
        let cfg = GpsMiaM10qConfig {
            uart_num: 2,
            pin_tx: 16,
            pin_rx: 2,
            baud_rate: 38400,
        };
        assert_eq!(
            mia_m10q_init(&cfg as *const GpsMiaM10qConfig as *const c_void),
            ESP_OK
        );
        assert_eq!(mia_m10q_enable(), ESP_OK);
    }

    unsafe fn feed(bytes: &[u8]) {
        for &b in bytes {
            feed_byte(b);
        }
    }

    #[test]
    fn test_get_position_not_reading_returns_invalid_state() {
        unsafe {
            reset_state();
            let mut pos = empty_position();
            assert_eq!(
                mia_m10q_get_position(&mut pos as *mut HalGpsPosition),
                ESP_ERR_INVALID_STATE,
            );
        }
    }

    /// Acquiring is not an error: the satellite count is what a user watches
    /// while waiting for a fix, so it has to come back with ESP_OK.
    #[test]
    fn test_get_position_acquiring_returns_ok_without_fix() {
        unsafe {
            start_receiver();
            (*(&raw mut S_GPS)).last_position.satellites = 3;

            let mut pos = empty_position();
            assert_eq!(
                mia_m10q_get_position(&mut pos as *mut HalGpsPosition),
                ESP_OK
            );
            assert!(!pos.fix_valid);
            assert_eq!(pos.satellites, 3);
            mia_m10q_deinit();
        }
    }

    #[test]
    fn test_get_position_valid_fix_returns_ok() {
        unsafe {
            start_receiver();
            (*(&raw mut S_GPS)).last_position.fix_valid = true;
            (*(&raw mut S_GPS)).last_position.latitude = 53.36;

            let mut pos = empty_position();
            assert_eq!(
                mia_m10q_get_position(&mut pos as *mut HalGpsPosition),
                ESP_OK
            );
            assert!(pos.fix_valid);
            assert!((pos.latitude - 53.36).abs() < 1e-9);
            mia_m10q_deinit();
        }
    }

    #[test]
    fn test_get_position_after_disable_returns_invalid_state() {
        unsafe {
            start_receiver();
            assert_eq!(mia_m10q_disable(), ESP_OK);
            let mut pos = empty_position();
            assert_eq!(
                mia_m10q_get_position(&mut pos as *mut HalGpsPosition),
                ESP_ERR_INVALID_STATE,
            );
            mia_m10q_deinit();
        }
    }

    // ── nmea_feed_byte ────────────────────────────────────────────────────────

    #[test]
    fn test_feed_assembles_a_fix_from_the_wire() {
        unsafe {
            start_receiver();
            feed(b"$GNGGA,092751.000,5321.6802,N,00630.3372,W,1,08,1.03,61.7,M,55.3,M,,*58\r\n");
            feed(b"$GNRMC,092751.000,A,5321.6802,N,00630.3372,W,0.06,31.66,280511,,,A*58\r\n");

            let gps = &*(&raw const S_GPS);
            assert_eq!(gps.sentences_seen, 2);
            assert!(gps.last_position.fix_valid);
            assert_eq!(gps.last_position.satellites, 8);
            assert!((gps.last_position.latitude - (53.0 + 21.6802 / 60.0)).abs() < 1e-5);
            mia_m10q_deinit();
        }
    }

    /// What a receiver at the other baud rate looks like: bytes, never a
    /// sentence. The counters tell that apart from an unpowered receiver.
    #[test]
    fn test_feed_counts_bytes_but_no_sentence_for_line_noise() {
        unsafe {
            start_receiver();
            feed(&[0xF8, 0x80, 0x00, b'$', 0xE0, 0x1C, b'\n', 0xFE, b'\n']);

            let gps = &*(&raw const S_GPS);
            assert_eq!(gps.bytes_seen, 9);
            assert_eq!(gps.sentences_seen, 0);
            assert!(!gps.last_position.fix_valid);
            mia_m10q_deinit();
        }
    }

    #[test]
    fn test_feed_resyncs_on_dollar_after_a_torn_sentence() {
        unsafe {
            start_receiver();
            feed(b"$GNRMC,0927");
            feed(b"$GNGGA,092751.000,5321.6802,N,00630.3372,W,1,08,1.03,61.7,M,55.3,M,,*58\r\n");

            let gps = &*(&raw const S_GPS);
            assert_eq!(gps.sentences_seen, 1);
            assert_eq!(gps.last_position.satellites, 8);
            mia_m10q_deinit();
        }
    }

    #[test]
    fn test_enable_resets_the_counters() {
        unsafe {
            start_receiver();
            feed(b"$GNGGA,092751.000,5321.6802,N,00630.3372,W,1,08,1.03,61.7,M,55.3,M,,*58\r\n");
            assert_eq!(mia_m10q_disable(), ESP_OK);
            assert_eq!(mia_m10q_enable(), ESP_OK);

            let gps = &*(&raw const S_GPS);
            assert_eq!(gps.bytes_seen, 0);
            assert_eq!(gps.sentences_seen, 0);
            mia_m10q_deinit();
        }
    }

    // ── register_callback ─────────────────────────────────────────────────────

    #[test]
    fn test_register_callback_stores_values() {
        unsafe {
            reset_state();

            unsafe extern "C" fn dummy_cb(_pos: *const HalGpsPosition, _ud: *mut c_void) {}

            let sentinel = 0xDEAD_BEEFusize as *mut c_void;
            assert_eq!(mia_m10q_register_callback(Some(dummy_cb), sentinel), ESP_OK);
            assert!((*(&raw const S_GPS)).cb.is_some());
            assert_eq!((*(&raw const S_GPS)).cb_data, sentinel);
        }
    }

    // ── GSV: what the receiver hears ──────────────────────────────────────────

    /// The interface description's own example: GPS, nine satellites in
    /// three sentences, every one heard.
    const GPS_SET: &[u8] = b"$GPGSV,3,1,09,09,,,17,10,,,40,12,,,49,13,,,35,1*6F\r\n\
$GPGSV,3,2,09,15,,,44,17,,,45,19,,,44,24,,,50,1*64\r\n\
$GPGSV,3,3,09,25,,,40,1*6E\r\n";

    fn sky_of(lines: &[&str], now: u32) -> Sky {
        let mut sky = Sky::new();
        for line in lines {
            sky.feed(&nmea_split(line), now);
        }
        sky
    }

    #[test]
    fn gsv_counts_a_constellation_once_its_set_is_whole() {
        unsafe {
            start_receiver();
            feed(GPS_SET);
            let mut sky = HalGpsSky::default();
            assert_eq!(mia_m10q_get_sky(&mut sky), ESP_OK);
            assert_eq!(sky, HalGpsSky { in_view: 9, heard: 9, best_cno: 50 });
            mia_m10q_deinit();
        }
    }

    #[test]
    fn gsv_adds_constellations_up() {
        unsafe {
            start_receiver();
            feed(GPS_SET);
            feed(b"$GBGSV,2,1,06,07,,,,10,,,21,19,,,,22,,,,1*7E\r\n");
            feed(b"$GBGSV,2,2,06,27,,,28,30,,,,1*7C\r\n");
            feed(b"$GAGSV,1,1,00,2*76\r\n");
            let mut sky = HalGpsSky::default();
            assert_eq!(mia_m10q_get_sky(&mut sky), ESP_OK);
            assert_eq!(sky, HalGpsSky { in_view: 15, heard: 11, best_cno: 50 });
            mia_m10q_deinit();
        }
    }

    /// A satellite with no C/N0 is one the receiver expects to see but does
    /// not hear: in view, not heard. Indoors that is most of them.
    #[test]
    fn gsv_counts_a_satellite_with_no_signal_as_in_view_only() {
        let sky = sky_of(&["$GBGSV,2,1,06,07,,,,10,,,21,19,,,,22,,,,1", "$GBGSV,2,2,06,27,,,28,30,,,,1"], 0);
        assert_eq!(sky.summary(0), HalGpsSky { in_view: 6, heard: 2, best_cno: 28 });
    }

    #[test]
    fn gsv_drops_a_set_with_a_sentence_missing() {
        let sky = sky_of(&["$GPGSV,3,1,09,09,,,17,10,,,40,12,,,49,13,,,35,1", "$GPGSV,3,3,09,25,,,40,1"], 0);
        assert_eq!(sky.summary(0), HalGpsSky::default());
    }

    #[test]
    fn gsv_keeps_one_report_per_constellation_and_signal() {
        let mut sky = sky_of(&["$GPGSV,1,1,03,12,,,42,24,,,47,32,,,37,5"], 0);
        sky.feed(&nmea_split("$GPGSV,1,1,03,12,,,30,24,,,31,32,,,,5"), 1000);
        assert_eq!(sky.summary(1000), HalGpsSky { in_view: 3, heard: 2, best_cno: 31 });
        // The same talker on another signal is reported apart.
        sky.feed(&nmea_split("$GPGSV,1,1,01,12,,,20,1"), 1000);
        assert_eq!(sky.summary(1000), HalGpsSky { in_view: 4, heard: 3, best_cno: 31 });
    }

    /// NMEA before 4.10 has no signal ID after the satellites.
    #[test]
    fn gsv_reads_a_sentence_with_no_signal_id() {
        let sky = sky_of(&["$GPGSV,1,1,02,12,,,42,24,,,"], 0);
        assert_eq!(sky.summary(0), HalGpsSky { in_view: 2, heard: 1, best_cno: 42 });
    }

    #[test]
    fn gsv_forgets_a_constellation_no_longer_reported() {
        let mut sky = sky_of(&["$GPGSV,1,1,03,12,,,42,24,,,47,32,,,37,5"], 0);
        sky.feed(&nmea_split("$GAGSV,1,1,01,04,,,33,7"), SKY_STALE_TICKS);
        assert_eq!(sky.summary(SKY_STALE_TICKS), HalGpsSky { in_view: 1, heard: 1, best_cno: 33 });
    }

    #[test]
    fn get_sky_is_an_error_while_nobody_reads_the_receiver() {
        unsafe {
            reset_state();
            let mut sky = HalGpsSky { in_view: 1, heard: 1, best_cno: 1 };
            assert_eq!(mia_m10q_get_sky(&mut sky), ESP_ERR_INVALID_STATE);
            assert_eq!(sky, HalGpsSky::default());
            assert_eq!(mia_m10q_get_sky(std::ptr::null_mut()), ESP_ERR_INVALID_ARG);
        }
    }

    // ── standby and waking ────────────────────────────────────────────────────

    #[test]
    fn the_standby_command_is_the_m10s_sixteen_byte_form() {
        let frame = &UBX_RXM_PMREQ_STANDBY;
        assert_eq!(&frame[..4], &[0xB5, 0x62, 0x02, 0x41]);
        assert_eq!(u16::from_le_bytes([frame[4], frame[5]]) as usize, frame.len() - 8);
        let field = |at: usize| u32::from_le_bytes(frame[6 + at..10 + at].try_into().unwrap());
        assert_eq!(field(4), 0, "duration: until woken");
        assert_eq!(field(8), 0b110, "flags: backup and force");
        assert_eq!(field(12), 1 << 3, "wakeupSources: uartrx");

        let (mut a, mut b) = (0u8, 0u8);
        for &x in &frame[2..frame.len() - 2] {
            a = a.wrapping_add(x);
            b = b.wrapping_add(a);
        }
        assert_eq!([a, b], frame[frame.len() - 2..]);
    }

    #[test]
    fn sleep_before_init_is_refused() {
        unsafe {
            reset_state();
            assert_eq!(mia_m10q_sleep(true), ESP_ERR_INVALID_STATE);
            assert_eq!(mia_m10q_sleep(false), ESP_ERR_INVALID_STATE);
            assert!(sent().is_empty());
        }
    }

    /// A restart does not take the receiver's power away, so one sent to
    /// standby before it is still there: the first enable wakes it.
    #[test]
    fn enable_wakes_the_receiver() {
        unsafe {
            start_receiver();
            assert_eq!(sent(), WAKE_PULSE);
            mia_m10q_deinit();
        }
    }

    #[test]
    fn sleep_sends_the_receiver_to_standby_and_wakes_it_after() {
        unsafe {
            start_receiver();
            sent();
            assert_eq!(mia_m10q_sleep(true), ESP_OK);
            assert_eq!(sent(), UBX_RXM_PMREQ_STANDBY);
            assert_eq!(mia_m10q_sleep(false), ESP_OK);
            assert_eq!(sent(), WAKE_PULSE);
            mia_m10q_deinit();
        }
    }

    /// Anything sent to a receiver in standby wakes it, the command lost.
    #[test]
    fn a_receiver_in_standby_is_sent_nothing_more() {
        unsafe {
            start_receiver();
            assert_eq!(mia_m10q_sleep(true), ESP_OK);
            sent();
            assert_eq!(mia_m10q_sleep(true), ESP_OK);
            assert!(sent().is_empty());
            mia_m10q_deinit();
        }
    }

    #[test]
    fn switching_off_sends_the_receiver_to_standby_until_switched_on() {
        unsafe {
            start_receiver();
            sent();
            assert_eq!(mia_m10q_disable(), ESP_OK);
            assert_eq!(sent(), UBX_RXM_PMREQ_STANDBY);

            // A sleep and a wake while off leave it there.
            assert_eq!(mia_m10q_sleep(true), ESP_OK);
            assert_eq!(mia_m10q_sleep(false), ESP_OK);
            assert!(sent().is_empty());

            assert_eq!(mia_m10q_enable(), ESP_OK);
            assert_eq!(sent(), WAKE_PULSE);
            mia_m10q_deinit();
        }
    }

    /// Navigator must not show the fix from before a sleep as current.
    #[test]
    fn standby_takes_the_fix_and_the_sky_with_it() {
        unsafe {
            start_receiver();
            feed(b"$GNGGA,092751.000,5321.6802,N,00630.3372,W,1,08,1.03,61.7,M,55.3,M,,*58\r\n");
            feed(b"$GNRMC,092751.000,A,5321.6802,N,00630.3372,W,0.06,31.66,280511,,,A*58\r\n");
            feed(GPS_SET);
            assert_eq!(mia_m10q_sleep(true), ESP_OK);
            assert_eq!(mia_m10q_sleep(false), ESP_OK);

            let mut pos = empty_position();
            assert_eq!(mia_m10q_get_position(&mut pos), ESP_OK);
            assert!(!pos.fix_valid);
            assert_eq!(pos.satellites, 0);
            let mut sky = HalGpsSky::default();
            assert_eq!(mia_m10q_get_sky(&mut sky), ESP_OK);
            assert_eq!(sky, HalGpsSky::default());
            mia_m10q_deinit();
        }
    }

    #[test]
    fn a_receiver_that_does_not_answer_a_wake_is_woken_again_then_left() {
        unsafe {
            start_receiver();
            sent();
            reader_checks(WAKE_RETRY_TICKS - 1);
            assert!(sent().is_empty());
            for try_at in 1..WAKE_TRIES as u32 {
                reader_checks(try_at * WAKE_RETRY_TICKS);
                assert_eq!(sent(), WAKE_PULSE, "try {}", try_at + 1);
            }
            reader_checks(WAKE_TRIES as u32 * WAKE_RETRY_TICKS);
            reader_checks((WAKE_TRIES as u32 + 1) * WAKE_RETRY_TICKS);
            assert!(sent().is_empty());
            mia_m10q_deinit();
        }
    }

    #[test]
    fn a_receiver_that_talks_after_a_wake_is_left_alone() {
        unsafe {
            start_receiver();
            sent();
            feed(b"$GAGSV,1,1,00,2*76\r\n");
            reader_checks(WAKE_RETRY_TICKS);
            reader_checks(2 * WAKE_RETRY_TICKS);
            // Its signals and AssistNow Autonomous, and no more wake pulses.
            let mut want = signals();
            want.extend(autonomous());
            assert_eq!(sent(), want);
            mia_m10q_deinit();
        }
    }

    /// Indoors there may never be a fix: the line comes at 30 s, then at
    /// gaps doubling up to ten minutes, not every pass.
    #[test]
    fn the_no_fix_line_comes_less_often_the_longer_it_waits() {
        unsafe {
            start_receiver();
            feed(GPS_SET);
            let mut logged = Vec::new();
            for second in 0..3600u32 {
                let due = (*(&raw const S_GPS)).next_sky_log;
                reader_checks(second * 1000);
                if (*(&raw const S_GPS)).next_sky_log != due {
                    logged.push(second);
                }
            }
            assert_eq!(logged, [30, 60, 120, 240, 480, 960, 1560, 2160, 2760, 3360]);
            mia_m10q_deinit();
        }
    }

    // ── UBX ───────────────────────────────────────────────────────────────────

    /// A sentence: the receiver talking.
    const TALK: &[u8] = b"$GAGSV,1,1,00,2*76\r\n";
    /// 2026-10-20 12:34:56 UTC.
    const NOW_S: i64 = 1_792_499_696;
    const TODAY: (u8, u8, u8) = (26, 10, 20);

    fn frame(id: u8, payload: &[u8]) -> Vec<u8> {
        let mut f = Vec::new();
        ubx_push(&mut f, 0x13, id, payload);
        f
    }

    /// UBX-MGA-ANO for one satellite and day.
    fn ano(gnss: u8, sv: u8, (y, m, d): (u8, u8, u8)) -> Vec<u8> {
        let mut p = [0u8; 76];
        (p[2], p[3], p[4], p[5], p[6]) = (sv, gnss, y, m, d);
        frame(0x20, &p)
    }

    /// UBX-MGA-GPS-ALM for one satellite.
    fn gps_alm(sv: u8) -> Vec<u8> {
        let mut p = [0u8; 36];
        (p[0], p[2]) = (0x02, sv);
        frame(0x00, &p)
    }

    /// The receiver's UBX-MGA-ACK for `msg`.
    fn ack(msg: &[u8], taken: bool, code: u8) -> Vec<u8> {
        let mut f = Vec::new();
        ubx_push(&mut f, 0x13, 0x60, &[taken as u8, 0, code, msg[3], msg[6], msg[7], msg[8], msg[9]]);
        f
    }

    fn aid(utc_s: i64, data: &[u8]) -> HalGpsAssist {
        HalGpsAssist {
            utc_us: utc_s * 1_000_000,
            utc_acc_ms: 10_500,
            has_position: true,
            latitude: 59.3293,
            longitude: 18.0686,
            altitude_m: 28.0,
            position_acc_m: 1000,
            data: data.as_ptr(),
            data_len: data.len(),
        }
    }

    fn signals() -> Vec<u8> {
        let mut f = Vec::new();
        ubx_push(&mut f, 0x06, 0x8A, &VALSET_SIGNALS);
        f
    }

    fn autonomous() -> Vec<u8> {
        let mut f = Vec::new();
        ubx_push(&mut f, 0x06, 0x8A, &VALSET_AUTONOMOUS);
        f
    }

    /// The receiver talking, and sent its signals: what goes after the
    /// settle is what the test is about.
    unsafe fn talk_and_settle(at: u32) -> u32 {
        feed(TALK);
        reader_checks(at);
        assert_eq!(sent(), signals(), "the signals first");
        at + SIGNALS_SETTLE_TICKS
    }

    /// What the first pass after the settle sends after AssistNow
    /// Autonomous, which goes first.
    unsafe fn settled(at: u32) -> Vec<u8> {
        reader_checks(at);
        let pass = sent();
        assert!(pass.starts_with(&autonomous()), "AssistNow Autonomous first");
        pass[autonomous().len()..].to_vec()
    }

    /// UBX-NAV-SAT listing `sats`: (gnssId, svId, cno, elev, azim, flags).
    fn nav_sat_frame(sats: &[(u8, u8, u8, i8, i16, u32)]) -> Vec<u8> {
        let mut p = vec![0u8; 8];
        p[4] = 1;
        p[5] = sats.len() as u8;
        for &(gnss, sv, cno, elev, azim, flags) in sats {
            p.extend_from_slice(&[gnss, sv, cno, elev as u8]);
            p.extend_from_slice(&azim.to_le_bytes());
            p.extend_from_slice(&[0, 0]);
            p.extend_from_slice(&flags.to_le_bytes());
        }
        let mut f = Vec::new();
        ubx_push(&mut f, 0x01, 0x35, &p);
        f
    }

    /// The frames in what was sent, as (class, id, payload).
    fn frames_in(bytes: &[u8]) -> Vec<(u8, u8, Vec<u8>)> {
        ubx_frames(bytes).map(|f| (f[2], f[3], f[6..f.len() - 2].to_vec())).collect()
    }

    #[test]
    fn civil_dates() {
        assert_eq!(civil_from_days(0), (1970, 1, 1));
        assert_eq!(civil_from_days(-1), (1969, 12, 31));
        assert_eq!(civil_from_days(11_016), (2000, 2, 29));
        assert_eq!(civil_from_days(20_454), (2026, 1, 1));
        assert_eq!(civil_from_days(NOW_S / 86_400), (2026, 10, 20));
    }

    #[test]
    fn the_polls_check() {
        for poll in [UBX_POLL_SEC_UNIQID, UBX_POLL_MON_VER, UBX_POLL_NAV_SAT] {
            assert_eq!(ubx_checksum(&poll[2..6]), poll[6..]);
        }
        let mut valset = Vec::new();
        ubx_push(&mut valset, 0x06, 0x8A, &VALSET_ACK_AIDING);
        assert_eq!(u32::from_le_bytes(valset[10..14].try_into().unwrap()), 0x1011_0025);
    }

    #[test]
    fn frames_are_found_among_junk_and_bad_ones_left_out() {
        let good = gps_alm(3);
        let mut bad = gps_alm(4);
        let last = bad.len() - 1;
        bad[last] ^= 0xFF;
        let mut data = b"junk\xB5".to_vec();
        data.extend_from_slice(&bad);
        data.extend_from_slice(&good);
        data.extend_from_slice(&good[..10]);
        let found: Vec<&[u8]> = ubx_frames(&data).collect();
        assert_eq!(found, [&good[..]]);
    }

    /// The service sends every constellation's predictions for a week, and
    /// the time it was asked: the receiver gets today's GPS predictions,
    /// every almanac, and the time made fresh.
    #[test]
    fn kept_are_the_almanacs_and_todays_predictions_it_takes() {
        let mut data = frame(0x40, &[0x10; 24]); // the service's own time
        data.extend(gps_alm(1));
        data.extend(ano(0, 1, (26, 10, 19)));
        data.extend(ano(0, 1, TODAY));
        data.extend(ano(2, 5, TODAY)); // Galileo
        data.extend(ano(6, 3, TODAY)); // GLONASS
        data.extend(ano(0, 1, (26, 10, 21)));
        let mut want = gps_alm(1);
        want.extend(ano(0, 1, TODAY));
        want.extend(ano(6, 3, TODAY));
        assert_eq!(kept_messages(&data, Some(TODAY)), want);
        assert_eq!(kept_messages(&data, None), gps_alm(1), "no date, no predictions");
    }

    #[test]
    fn the_time_is_sent_as_ubx_mga_ini_time_utc() {
        let p = ini_time_utc(NOW_S * 1_000_000 + 789_000, 10_500);
        assert_eq!(p[0], 0x10);
        assert_eq!(p[3], 0x80, "leap seconds unknown");
        assert_eq!(u16::from_le_bytes([p[4], p[5]]), 2026);
        assert_eq!(&p[6..11], &[10, 20, 12, 34, 56]);
        assert_eq!(u32::from_le_bytes(p[12..16].try_into().unwrap()), 789_000_000);
        assert_eq!(u16::from_le_bytes([p[16], p[17]]), 10);
        assert_eq!(u32::from_le_bytes(p[20..24].try_into().unwrap()), 500_000_000);
    }

    #[test]
    fn the_position_is_sent_as_ubx_mga_ini_pos_llh() {
        let a = Aid::new(&aid(NOW_S, &[]), &[], 0);
        let p = ini_pos_llh(a.position.unwrap(), a.position_acc_cm);
        assert_eq!(p[0], 0x01);
        assert_eq!(i32::from_le_bytes(p[4..8].try_into().unwrap()), 593_293_000);
        assert_eq!(i32::from_le_bytes(p[8..12].try_into().unwrap()), 180_686_000);
        assert_eq!(i32::from_le_bytes(p[12..16].try_into().unwrap()), 2800);
        assert_eq!(u32::from_le_bytes(p[16..20].try_into().unwrap()), 100_000);
    }

    #[test]
    fn identity_is_polled_once_the_receiver_talks_and_kept() {
        unsafe {
            start_receiver();
            let mut id = HalGpsIdentity::EMPTY;
            assert_eq!(mia_m10q_get_identity(&mut id), ESP_ERR_NOT_FOUND);
            sent();
            reader_checks(100);
            assert!(sent().is_empty(), "a receiver not yet talking is sent nothing");

            let at = talk_and_settle(200);
            let mut polls = UBX_POLL_SEC_UNIQID.to_vec();
            polls.extend_from_slice(&UBX_POLL_MON_VER);
            assert_eq!(settled(at), polls);
            reader_checks(at + 100);
            assert!(sent().is_empty(), "asked again only after a while");

            let mut uniq = Vec::new();
            ubx_push(&mut uniq, 0x27, 0x03, &[2, 0, 0, 0, 1, 2, 3, 4, 5, 6]);
            let mut ver = Vec::new();
            ubx_push(&mut ver, 0x0A, 0x04, &[b'R'; 220]);
            feed(&uniq);
            feed(TALK);
            feed(&ver);
            assert_eq!(mia_m10q_get_identity(&mut id), ESP_OK);
            assert_eq!(&id.unique_id[..id.unique_id_len as usize], &uniq[..]);
            assert_eq!(&id.version[..id.version_len as usize], &ver[..]);
            reader_checks(at + IDENTITY_RETRY_TICKS);
            assert!(sent().is_empty(), "nothing more to ask");
            mia_m10q_deinit();
        }
    }

    #[test]
    fn identity_is_asked_for_a_few_times_a_wake_then_left() {
        unsafe {
            start_receiver();
            let mut id = HalGpsIdentity::EMPTY;
            assert_eq!(mia_m10q_get_identity(&mut id), ESP_ERR_NOT_FOUND);
            sent();
            let at = talk_and_settle(0);
            let mut asked = (settled(at).len() == 16) as u32;
            for pass in 1..10u32 {
                reader_checks(at + pass * IDENTITY_RETRY_TICKS);
                asked += (sent().len() == 16) as u32;
            }
            assert_eq!(asked, IDENTITY_POLLS as u32);
            mia_m10q_deinit();
        }
    }

    /// Sends what was handed, and from the receiver's acknowledgements
    /// sends again, once, what went unanswered.
    #[test]
    fn assistance_goes_once_the_receiver_talks_a_few_messages_a_pass() {
        unsafe {
            start_receiver();
            let mut data = Vec::new();
            for sv in 1..=10 {
                data.extend(gps_alm(sv));
            }
            for sv in 1..=10 {
                data.extend(ano(0, sv, TODAY));
            }
            assert_eq!(mia_m10q_assist(&aid(NOW_S, &data)), ESP_OK);
            sent();
            reader_checks(100);
            assert!(sent().is_empty(), "a receiver not yet talking is sent nothing");

            let mut now = talk_and_settle(200);
            let mut all = settled(now);
            now += 100;
            loop {
                reader_checks(now);
                let pass = sent();
                if pass.is_empty() {
                    break;
                }
                assert!(pass.len() <= ASSIST_BUDGET + 17, "{} bytes in a pass", pass.len());
                all.extend(pass);
                now += 100;
            }
            let frames = frames_in(&all);
            assert_eq!(frames.len(), 1 + 2 + 20);
            assert_eq!((frames[0].0, frames[0].1), (0x06, 0x8A), "acknowledgements asked for first");
            assert_eq!((frames[1].1, frames[1].2[0]), (0x40, 0x10), "then the time");
            assert_eq!((frames[2].1, frames[2].2[0]), (0x40, 0x01), "then the position");
            assert!(frames[3..].iter().all(|f| f.0 == 0x13));

            // The receiver takes all but the third almanac and the last
            // prediction, which it never answers.
            let mut sent_msgs: Vec<Vec<u8>> = ubx_frames(&all).skip(1).map(|f| f.to_vec()).collect();
            let lost = sent_msgs.pop().unwrap();
            for (i, msg) in sent_msgs.iter().enumerate() {
                if i == 4 {
                    feed(&ack(msg, false, ACK_NOT_READY));
                } else {
                    feed(&ack(msg, true, 0));
                }
            }
            reader_checks(now + ACK_WAIT_TICKS);
            let again = frames_in(&sent());
            assert_eq!(again.len(), 2, "the one not ready and the one unanswered, again");
            assert_eq!(again[1].2, frames_in(&lost)[0].2);
            reader_checks(now + 2 * ACK_WAIT_TICKS);
            assert!((*(&raw const S_GPS)).sending.is_none(), "done after the second try");
            reader_checks(now + 3 * ACK_WAIT_TICKS);
            assert!(sent().is_empty());
            mia_m10q_deinit();
        }
    }

    #[test]
    fn a_receiver_with_a_fix_takes_no_assistance() {
        unsafe {
            start_receiver();
            feed(b"$GNRMC,092751.000,A,5321.6802,N,00630.3372,W,0.06,31.66,280511,,,A*58\r\n");
            assert_eq!(mia_m10q_assist(&aid(NOW_S, &[])), ESP_OK);
            sent();
            let at = talk_and_settle(100);
            assert!(settled(at).is_empty());
            mia_m10q_deinit();
        }
    }

    /// Anything sent wakes a receiver in standby; the next wake brings
    /// the kernel's next assistance.
    #[test]
    fn standby_drops_the_assistance() {
        unsafe {
            start_receiver();
            assert_eq!(mia_m10q_assist(&aid(NOW_S, &gps_alm(1))), ESP_OK);
            assert_eq!(mia_m10q_sleep(true), ESP_OK);
            assert_eq!(mia_m10q_sleep(false), ESP_OK);
            sent();
            let at = talk_and_settle(100);
            assert!(settled(at).is_empty());
            mia_m10q_deinit();
        }
    }

    /// GLONASS, and so BeiDou on B1C: CFG-SIGNAL's keys, as the
    /// interface description numbers them.
    #[test]
    fn the_signals_are_gps_galileo_glonass_and_beidou_b1c() {
        assert_eq!(&VALSET_SIGNALS[..4], &[0, 0x03, 0, 0], "RAM and BBR");
        let items: Vec<(u32, u8)> = VALSET_SIGNALS[4..]
            .chunks(5)
            .map(|c| (u32::from_le_bytes(c[..4].try_into().unwrap()), c[4]))
            .collect();
        assert_eq!(
            items,
            [(0x1031_0025, 1), (0x1031_0018, 1), (0x1031_0022, 1), (0x1031_000D, 0), (0x1031_000F, 1)]
        );
    }

    /// Each wake sends them, before anything else and a second before the
    /// rest: a change restarts the receiver's GNSS.
    #[test]
    fn the_signals_go_first_at_each_wake_and_the_rest_waits() {
        unsafe {
            start_receiver();
            assert_eq!(mia_m10q_assist(&aid(NOW_S, &gps_alm(1))), ESP_OK);
            sent();
            let at = talk_and_settle(100);
            reader_checks(at - 1);
            assert!(sent().is_empty(), "nothing during the settle");
            assert!(frames_in(&settled(at)).iter().any(|f| f.0 == 0x13), "then the assistance");

            let mut nak = Vec::new();
            ubx_push(&mut nak, 0x05, 0x00, &[0x06, 0x8A]);
            feed(&nak); // after the settle: not the signals', but the next
            assert_eq!((*(&raw const S_GPS)).cfg_awaited, [Cfg::AckAiding]);

            assert_eq!(mia_m10q_sleep(true), ESP_OK);
            assert_eq!(mia_m10q_sleep(false), ESP_OK);
            sent();
            talk_and_settle(at + 5000);
            mia_m10q_deinit();
        }
    }

    /// CFG-ANA-USE_ANA, as the interface description numbers it, kept
    /// through standby.
    #[test]
    fn assistnow_autonomous_is_cfg_ana_use_ana_in_ram_and_bbr() {
        assert_eq!(&VALSET_AUTONOMOUS[..4], &[0, 0x03, 0, 0]);
        assert_eq!(u32::from_le_bytes(VALSET_AUTONOMOUS[4..8].try_into().unwrap()), 0x1023_0001);
        assert_eq!(VALSET_AUTONOMOUS[8], 1);
    }

    /// Once a wake, after the settle; each answer is matched to the
    /// configuration it answers, in order.
    #[test]
    fn assistnow_autonomous_goes_once_a_wake_and_its_answer_is_its_own() {
        unsafe {
            start_receiver();
            sent();
            let mut ack = Vec::new();
            ubx_push(&mut ack, 0x05, 0x01, &[0x06, 0x8A]);
            let at = talk_and_settle(100);
            feed(&ack);
            assert!((*(&raw const S_GPS)).cfg_awaited.is_empty(), "the signals' answer");
            assert!(settled(at).is_empty());
            assert_eq!((*(&raw const S_GPS)).cfg_awaited, [Cfg::Autonomous]);
            reader_checks(at + 100);
            assert!(sent().is_empty(), "once a wake");
            feed(&ack);
            assert!((*(&raw const S_GPS)).cfg_awaited.is_empty());

            assert_eq!(mia_m10q_sleep(true), ESP_OK);
            assert_eq!(mia_m10q_sleep(false), ESP_OK);
            sent();
            let at = talk_and_settle(at + 5000);
            assert!(settled(at).is_empty(), "and again after a wake");
            mia_m10q_deinit();
        }
    }

    /// A signals' answer that never came is not taken from the next.
    #[test]
    fn a_lost_answer_is_forgotten_at_the_settle() {
        unsafe {
            start_receiver();
            sent();
            let at = talk_and_settle(100);
            settled(at);
            assert_eq!((*(&raw const S_GPS)).cfg_awaited, [Cfg::Autonomous]);
            mia_m10q_deinit();
        }
    }

    #[test]
    fn nav_sat_says_what_the_receiver_has_of_each_satellite() {
        // GPS 12: 38 dB-Hz, carrier locked, used, healthy, navigated by an
        // AssistNow prediction, holding its ephemeris and the prediction.
        let used = 0x7 | 0x8 | (1 << 4) | (3 << 8) | (1 << 11) | (1 << 13);
        // Galileo 5: searching, its own prediction and an almanac held.
        let searching = 0x1 | (4 << 8) | (1 << 12) | (1 << 14);
        let frame = nav_sat_frame(&[(0, 12, 38, 45, 123, used), (2, 5, 0, -100, 400, searching)]);
        let mut out = [HalGpsSat::default(); 4];
        assert_eq!(nav_sat(&frame[6..frame.len() - 2], &mut out), 2);
        assert_eq!(
            out[0],
            HalGpsSat {
                gnss: b'G',
                sv: 12,
                cno: 38,
                elev: 45,
                azim: 123,
                quality: 7,
                orbit: HAL_GPS_ORBIT_PREDICTED,
                orbits: HAL_GPS_HAS_EPHEMERIS | HAL_GPS_HAS_PREDICTED,
                health: 1,
                used: true,
                reserved: 0,
            }
        );
        let s = out[1];
        assert_eq!((s.gnss, s.sv, s.cno, s.elev, s.azim), (b'E', 5, 0, -128, -1), "out of range");
        assert_eq!((s.quality, s.used, s.orbit), (1, false, HAL_GPS_ORBIT_AUTONOMOUS));
        assert_eq!(s.orbits, HAL_GPS_HAS_ALMANAC | HAL_GPS_HAS_AUTONOMOUS);

        let mut one = [HalGpsSat::default(); 1];
        assert_eq!(nav_sat(&frame[6..frame.len() - 2], &mut one), 1, "as many as fit");
    }

    /// Asked for, polled once the receiver talks, and read off the wire,
    /// longer than any other frame; gone with a standby.
    #[test]
    fn the_satellites_are_polled_when_asked_for() {
        unsafe {
            reset_state();
            let mut sats = HalGpsSats::EMPTY;
            assert_eq!(mia_m10q_get_satellites(&mut sats), ESP_ERR_INVALID_STATE);
            start_receiver();
            assert_eq!(mia_m10q_get_satellites(&mut sats), ESP_ERR_NOT_FOUND);
            sent();
            let at = talk_and_settle(100);
            assert_eq!(settled(at), UBX_POLL_NAV_SAT, "polled once settled");
            reader_checks(at + 100);
            assert!(sent().is_empty(), "once an ask");

            let many: Vec<_> = (0..60u8).map(|i| (i % 4, i, 30, 20, 90, 0x4)).collect();
            let frame = nav_sat_frame(&many);
            assert!(frame.len() > UBX_MAX);
            feed(&frame);
            assert_eq!(mia_m10q_get_satellites(&mut sats), ESP_OK);
            assert_eq!(sats.count, 60);
            assert_eq!((sats.sats[59].gnss, sats.sats[59].sv), (b'C', 59));
            reader_checks(at + 200);
            assert_eq!(sent(), UBX_POLL_NAV_SAT, "each ask polls the next");

            assert_eq!(mia_m10q_sleep(true), ESP_OK);
            assert_eq!(mia_m10q_sleep(false), ESP_OK);
            assert_eq!(mia_m10q_get_satellites(&mut sats), ESP_ERR_NOT_FOUND, "stale after a standby");
            mia_m10q_deinit();
        }
    }

    #[test]
    fn assistance_before_init_is_refused() {
        unsafe {
            reset_state();
            assert_eq!(mia_m10q_assist(&aid(NOW_S, &[])), ESP_ERR_INVALID_STATE);
            assert_eq!(mia_m10q_assist(std::ptr::null()), ESP_ERR_INVALID_ARG);
            let mut id = HalGpsIdentity::EMPTY;
            assert_eq!(mia_m10q_get_identity(&mut id), ESP_ERR_INVALID_STATE);
        }
    }
}

