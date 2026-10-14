// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — Hynitron CST66xx capacitive touch controller driver (Rust)
//
// This is the part fitted to the T-Deck Pro Max.  Getting to that fact took a
// while, because nothing on paper says so: the vendor pinmap's section header
// calls it a CST328, the vendor README's spec table calls it a CST3530, and
// both are wrong.  What settled it was running LilyGo's own factory firmware
// on the board and reading what the chip says about itself:
//
//     [[HYN]] Touch init SUCCEED
//     [[HYN]] IC_info fw_project_id:2512010
//     [[HYN]] ictype:[caca220e]
//     [[HYN]] fw_ver:1010001
//
// Those three values land exactly on `hyn_cst66xx.c`'s field offsets, and the
// `caca` in `ictype` is that file's own probe signature.  So the protocol here
// follows `lib/HynTouch/src/hyn_cst66xx.c` in `Xinyuan-LilyGO/T-Deck-MAX`,
// which CLAUDE.md names as the hardware authority.
//
// It shares no register map with its CST328 and CST226 cousins, which is the
// whole reason this is a separate driver rather than another branch in one:
//
//   * Register addresses are FOUR bytes, big-endian.  The report block is at
//     `0xD0070000`, not `0xD000` and not `0x00`.
//   * The address write ends with a STOP; the read is a separate transaction.
//     Further points are read by simply continuing the stream, with no address
//     at all.
//   * Every report carries a 16-bit additive checksum that must be verified,
//     and the chip holds its buffer until a release command is written.
//   * Normal reporting is selected by a four-command sequence, not one write.

use std::os::raw::{c_char, c_void};
use std::sync::atomic::{AtomicBool, Ordering};

use crate::hal_registry::{
    HalInputCb, HalInputDriver, HalInputEvent, HalInputEventData, HalInputEventType,
    HalInputKeyData, HalInputTouchData, HAL_KEY_BACK, HAL_KEY_HOME,
};

// ── ESP error codes ─────────────────────────────────────────────────────────

const ESP_OK: i32 = 0;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_FAIL: i32 = -1;

// ── CST66xx register addresses (32-bit, big-endian on the wire) ─────────────

/// Report block.  Nine bytes of header and first point; further points
/// continue the stream.
const CST66_REG_REPORT: u32 = 0xD007_0000;
/// Configuration block read during probe, 50 bytes.
const CST66_REG_INFO: u32 = 0xD003_0000;

/// Disable the low-power I²C mode, so the chip is awake enough to be
/// configured.  Sent twice, as LilyGo do — the first one wakes it, the second
/// is the one it acts on.
const CST66_CMD_WAKE: u32 = 0xD000_0400;
/// The three writes that together select normal reporting.
const CST66_CMD_NORMAL: [u32; 3] = [0xD000_0000, 0xD000_0C00, 0xD000_0100];
/// Release the report buffer, so the chip may produce the next one.
const CST66_CMD_RELEASE: u32 = 0xD000_02AB;
/// Deep sleep. Only a reset brings the part back, and until then it does not
/// acknowledge its address (LilyGo's hyn_cst66xx.c, cst66xx_supend and
/// cst66xx_resum).
const CST66_CMD_DEEP_SLEEP: u32 = 0xD000_22AB;

/// Header length: checksum, report type, point counts, and the first point.
const CST66_HEADER_LEN: usize = 9;
/// Bytes per reported point.
const CST66_POINT_LEN: usize = 5;
/// Most points the part reports, fingers and keys together.
const CST66_MAX_POINTS: usize = 5;
/// Largest assembled report: the header plus the four points beyond the first.
const CST66_REPORT_MAX: usize = CST66_HEADER_LEN + (CST66_MAX_POINTS - 1) * CST66_POINT_LEN;

/// Length of the configuration block, and the offsets worth reading out of it.
const CST66_INFO_LEN: usize = 50;

/// Report type byte for a position report.  Gestures (`0xF0`) and proximity
/// (`0xE0`) use the same block and are not surfaced here.
const CST66_REPORT_POSITION: u8 = 0xFF;
/// Seed for the report's additive checksum.
const CST66_CHECKSUM_SEED: u16 = 0x55;
/// Signature in the configuration block confirming the part is a CST66xx.
const CST66_SIGNATURE: [u8; 2] = [0xCA, 0xCA];

/// How long to wait for the part to answer after a reset, and how often to
/// ask. LilyGo wait a flat 50 ms; polling instead costs nothing when the chip
/// is quick and survives one that is not.
const CST66_ACK_TIMEOUT_MS: u32 = 500;
const CST66_ACK_POLL_MS: u32 = 20;

/// The vendor firmware drives this bus at 100 kHz and this part is the reason
/// to respect that: it shares the bus with parts that are happy at 400 kHz,
/// but the device handle sets the clock per transaction, so slowing this one
/// down costs the others nothing.
const CST66_I2C_HZ: u32 = 100_000;

// GPIO_NUM_NC: -1 means "not connected"
const GPIO_NUM_NC: i32 = -1;

// ── I2C device config layout (mirrors i2c_device_config_t) ──────────────────

#[repr(C)]
struct I2cDeviceConfig {
    dev_addr_length: u32, // I2C_ADDR_BIT_LEN_7 = 0
    device_address: u16,
    scl_speed_hz: u32,
}

// GPIO config layout (mirrors gpio_config_t) ─────────────────────────────────

#[repr(C)]
struct GpioConfig {
    pin_bit_mask: u64,
    mode: u32,
    pull_up_en: u32,
    pull_down_en: u32,
    intr_type: u32,
}

// GPIO / intr constants (from driver/gpio.h) ─────────────────────────────────

const GPIO_MODE_OUTPUT: u32 = 2;
const GPIO_MODE_INPUT: u32 = 1;
const GPIO_PULLUP_ENABLE: u32 = 1;
const GPIO_PULLUP_DISABLE: u32 = 0;
const GPIO_PULLDOWN_DISABLE: u32 = 0;
const GPIO_INTR_DISABLE: u32 = 0;
const GPIO_INTR_NEGEDGE: u32 = 2;

// ── ESP-IDF FFI ─────────────────────────────────────────────────────────────

#[cfg(target_os = "espidf")]
extern "C" {
    fn i2c_master_bus_add_device(
        bus: *mut c_void,
        cfg: *const I2cDeviceConfig,
        handle: *mut *mut c_void,
    ) -> i32;
    fn i2c_master_bus_rm_device(handle: *mut c_void) -> i32;
    fn i2c_master_transmit(handle: *mut c_void, data: *const u8, len: usize, timeout_ms: i32)
        -> i32;
    fn i2c_master_receive(handle: *mut c_void, data: *mut u8, len: usize, timeout_ms: i32) -> i32;
    fn i2c_master_probe(bus: *mut c_void, address: u16, timeout_ms: i32) -> i32;

    fn gpio_config(cfg: *const GpioConfig) -> i32;
    fn gpio_set_level(pin: i32, level: u32) -> i32;
    fn gpio_get_level(pin: i32) -> i32;
    fn gpio_isr_handler_add(
        pin: i32,
        handler: unsafe extern "C" fn(*mut c_void),
        arg: *mut c_void,
    ) -> i32;
    fn gpio_isr_handler_remove(pin: i32) -> i32;

    fn esp_timer_get_time() -> i64;
    fn vTaskDelay(ticks: u32);

    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

// ── Extern "C" bindings for simulator with sim-bus feature ───────────────────

#[cfg(all(not(target_os = "espidf"), feature = "sim-bus"))]
extern "C" {
    fn i2c_master_bus_add_device(
        bus: *mut c_void,
        cfg: *const I2cDeviceConfig,
        handle: *mut *mut c_void,
    ) -> i32;
    fn i2c_master_bus_rm_device(handle: *mut c_void) -> i32;
    fn i2c_master_transmit(handle: *mut c_void, data: *const u8, len: usize, timeout_ms: i32)
        -> i32;
    fn i2c_master_receive(handle: *mut c_void, data: *mut u8, len: usize, timeout_ms: i32) -> i32;
    fn i2c_master_probe(bus: *mut c_void, address: u16, timeout_ms: i32) -> i32;
    fn gpio_config(cfg: *const GpioConfig) -> i32;
    fn gpio_set_level(pin: i32, level: u32) -> i32;
    fn gpio_get_level(pin: i32) -> i32;
    fn gpio_isr_handler_add(
        pin: i32,
        handler: unsafe extern "C" fn(*mut c_void),
        arg: *mut c_void,
    ) -> i32;
    fn gpio_isr_handler_remove(pin: i32) -> i32;
    fn esp_timer_get_time() -> i64;
    fn vTaskDelay(ticks: u32);
}

// ── Stub implementations (host tests without sim-bus) ────────────────────────

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
unsafe fn i2c_master_bus_add_device(
    _bus: *mut c_void,
    _cfg: *const I2cDeviceConfig,
    handle: *mut *mut c_void,
) -> i32 {
    *handle = 1usize as *mut c_void;
    ESP_OK
}

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
unsafe fn i2c_master_bus_rm_device(_handle: *mut c_void) -> i32 {
    ESP_OK
}

/// Writes the host fake has seen, so a test can tell the part was left alone.
#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
static TRANSMITS: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
unsafe fn i2c_master_transmit(
    _handle: *mut c_void,
    _data: *const u8,
    _len: usize,
    _timeout_ms: i32,
) -> i32 {
    TRANSMITS.fetch_add(1, Ordering::Relaxed);
    ESP_OK
}

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
unsafe fn i2c_master_receive(
    _handle: *mut c_void,
    data: *mut u8,
    len: usize,
    _timeout_ms: i32,
) -> i32 {
    std::ptr::write_bytes(data, 0, len);
    ESP_OK
}

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
unsafe fn i2c_master_probe(_bus: *mut c_void, _address: u16, _timeout_ms: i32) -> i32 {
    ESP_OK
}

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
unsafe fn gpio_config(_cfg: *const GpioConfig) -> i32 {
    ESP_OK
}

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
unsafe fn gpio_set_level(_pin: i32, _level: u32) -> i32 {
    ESP_OK
}

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
unsafe fn gpio_get_level(_pin: i32) -> i32 {
    1 // idle high: the line is active low
}

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
unsafe fn gpio_isr_handler_add(
    _pin: i32,
    _handler: unsafe extern "C" fn(*mut c_void),
    _arg: *mut c_void,
) -> i32 {
    ESP_OK
}

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
unsafe fn gpio_isr_handler_remove(_pin: i32) -> i32 {
    ESP_OK
}

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
unsafe fn esp_timer_get_time() -> i64 {
    0
}

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
unsafe fn vTaskDelay(_ticks: u32) {}

/// ESP-IDF's tick rate is 1 kHz on the ESP32-S3, so a millisecond is a tick.
#[inline(always)]
fn ms_to_ticks(ms: u32) -> u32 {
    ms
}

// ── Logging ──────────────────────────────────────────────────────────────────

#[cfg(target_os = "espidf")]
const ESP_LOG_WARN: i32 = 2;
#[cfg(target_os = "espidf")]
const ESP_LOG_INFO: i32 = 3;
#[cfg(target_os = "espidf")]
static LOG_TAG: &[u8] = b"cst66xx\0";

/// What the chip says about itself, on every boot rather than behind a debug
/// flag: it is the only thing that identifies which Hynitron part is fitted,
/// and getting that wrong is what cost this driver its first three attempts.
#[cfg(target_os = "espidf")]
fn log_identity(info: &ChipInfo) {
    unsafe {
        esp_log_write(
            ESP_LOG_INFO,
            LOG_TAG.as_ptr(),
            b"res=%ux%u ic_type=%08x project=%08x fw=%08x\0".as_ptr(),
            info.res_x as u32,
            info.res_y as u32,
            info.chip_type,
            info.project_id,
            info.firmware,
        );
    }
}

#[cfg(not(target_os = "espidf"))]
fn log_identity(_info: &ChipInfo) {}

#[cfg(target_os = "espidf")]
fn log_probe_failed() {
    unsafe {
        esp_log_write(
            ESP_LOG_WARN,
            LOG_TAG.as_ptr(),
            b"configuration block never returned its CACA signature\0".as_ptr(),
        );
    }
}

#[cfg(not(target_os = "espidf"))]
fn log_probe_failed() {}

#[cfg(target_os = "espidf")]
fn log_ack(waited_ms: u32) {
    unsafe {
        esp_log_write(
            ESP_LOG_INFO,
            LOG_TAG.as_ptr(),
            b"answered %ums after reset\0".as_ptr(),
            waited_ms,
        );
    }
}

#[cfg(not(target_os = "espidf"))]
fn log_ack(_waited_ms: u32) {}

#[cfg(target_os = "espidf")]
fn log_no_ack() {
    unsafe {
        esp_log_write(
            ESP_LOG_WARN,
            LOG_TAG.as_ptr(),
            b"no answer at 0x%02x within %ums of reset - not starting\0".as_ptr(),
            S_TOUCH.cfg.i2c_addr as u32,
            CST66_ACK_TIMEOUT_MS,
        );
    }
}

#[cfg(not(target_os = "espidf"))]
fn log_no_ack() {}

/// Every key press, mapped or not: the chip only numbers its keys, so this
/// line is how a board profile learns which number is which key.
#[cfg(target_os = "espidf")]
fn log_key(id: u8, keycode: Option<u16>) {
    let meaning: &[u8] = match keycode {
        Some(HAL_KEY_HOME) => b" (home)\0",
        Some(HAL_KEY_BACK) => b" (back)\0",
        _ => b"\0",
    };
    unsafe {
        esp_log_write(
            ESP_LOG_INFO,
            LOG_TAG.as_ptr(),
            b"\ncst66xx: touch key %u pressed%s\n\0".as_ptr(),
            id as u32,
            meaning.as_ptr(),
        );
    }
}

#[cfg(not(target_os = "espidf"))]
fn log_key(_id: u8, _keycode: Option<u16>) {}

// ── Configuration struct (C-compatible) ─────────────────────────────────────

/// Configuration passed to `cst66xx_init`.  Layout must match the C
/// `touch_cst66xx_config_t` struct in `drv_touch_cst66xx.h`.
#[repr(C)]
pub struct TouchCst66xxConfig {
    /// I2C master bus handle (`i2c_master_bus_handle_t`).
    pub i2c_bus: *mut c_void,
    /// I2C device address.  Default: `0x1A`.
    pub i2c_addr: u8,
    /// Interrupt GPIO number; `-1` to disable.
    pub pin_int: i32,
    /// Reset GPIO number; `-1` when reset is not a GPIO.
    pub pin_rst: i32,
    /// Index of the IO expander carrying the reset line, or `-1`.  The T-Deck
    /// Max wires `T_RST` to XL9555 P07, so there is no GPIO to name.
    pub rst_expander: i32,
    /// Pin on that expander.
    pub rst_expander_pin: i32,
    /// Panel width in pixels.
    pub max_x: u16,
    /// Panel height in pixels.
    pub max_y: u16,
    /// Id of the touch key below the panel that is reported as
    /// `HAL_KEY_HOME`, or `-1`.  The chip numbers its keys and says nothing
    /// of what is printed on them, so the board profile names this one.
    pub home_key: i32,
    /// Id of the touch key reported as `HAL_KEY_BACK`, or `-1`.
    pub back_key: i32,
}

// ── Driver state ─────────────────────────────────────────────────────────────

struct TouchState {
    dev: *mut c_void,
    cfg: TouchCst66xxConfig,
    cb: HalInputCb,
    cb_data: *mut c_void,
    irq_pending: AtomicBool,
    touching: bool,
    last_x: u16,
    last_y: u16,
    /// The touch key held down, if any.
    key_down: Option<u8>,
    initialized: bool,
    /// In deep sleep: the part answers nothing until a reset, so nothing is
    /// sent to it.
    asleep: bool,
}

// SAFETY: guarded by the single-threaded init / poll contract the HAL registry
// imposes; the ISR only sets an atomic.
unsafe impl Send for TouchState {}
unsafe impl Sync for TouchState {}

impl TouchState {
    const fn new() -> Self {
        TouchState {
            dev: std::ptr::null_mut(),
            cfg: TouchCst66xxConfig {
                i2c_bus: std::ptr::null_mut(),
                i2c_addr: 0x1A,
                pin_int: GPIO_NUM_NC,
                pin_rst: GPIO_NUM_NC,
                rst_expander: -1,
                rst_expander_pin: 0,
                max_x: 240,
                max_y: 320,
                home_key: -1,
                back_key: -1,
            },
            cb: None,
            cb_data: std::ptr::null_mut(),
            irq_pending: AtomicBool::new(false),
            touching: false,
            last_x: 0,
            last_y: 0,
            key_down: None,
            initialized: false,
            asleep: false,
        }
    }
}

static mut S_TOUCH: TouchState = TouchState::new();

// ── Report decoding (pure) ───────────────────────────────────────────────────

/// One finger, in the controller's own coordinate frame.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Contact {
    pub x: u16,
    pub y: u16,
}

/// What the chip has to say about itself.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct ChipInfo {
    pub res_x: u16,
    pub res_y: u16,
    pub chip_type: u32,
    pub project_id: u32,
    pub firmware: u32,
}

/// How much of a report is still on the wire after the nine-byte header.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Shape {
    keys: usize,
    fingers: usize,
}

impl Shape {
    /// Points beyond the first, which is the part the header already carried.
    fn continuation_len(&self) -> usize {
        match self.keys + self.fingers {
            0 | 1 => 0,
            n => (n - 1) * CST66_POINT_LEN,
        }
    }

    fn total_len(&self) -> usize {
        CST66_HEADER_LEN + self.continuation_len()
    }
}

/// What a decoded report turned out to describe.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Report {
    /// A finger resting on the glass.
    Contact(Contact),
    /// One of the touch keys below the panel, going down or coming up.
    Key { id: u8, pressed: bool },
    /// A well-formed report with nobody touching.
    Empty,
    /// A torn or corrupt read, worth one retry.
    Junk,
}

/// Read the point counts out of a report header.
///
/// `None` means the counts are impossible, which is how a torn read usually
/// shows up first — the vendor driver treats it as a retry rather than as an
/// empty report, because the byte count it implies would desynchronise the
/// stream.
fn report_shape(header: &[u8; CST66_HEADER_LEN]) -> Option<Shape> {
    let fingers = (header[3] & 0x0F) as usize;
    let keys = ((header[3] & 0xF0) >> 4) as usize;
    if keys + fingers > CST66_MAX_POINTS {
        return None;
    }
    Some(Shape { keys, fingers })
}

/// The chip's additive checksum over the point data, seeded with `0x55`.
fn checksum(points: &[u8]) -> u16 {
    points
        .iter()
        .fold(CST66_CHECKSUM_SEED, |sum, byte| sum.wrapping_add(*byte as u16))
}

/// Decode an assembled report.
///
/// Layout, per `cst66xx_report`:
///
/// ```text
///   [0..2] 16-bit checksum of the point data, little endian
///   [2]    report type; 0xFF carries positions
///   [3]    finger count (low nibble) | key count (high nibble)
///   [4..]  five bytes per point, keys first:
///            +0 X low 8      +1 Y low 8      +2 pressure
///            +3 Y high 4 (high nibble) | X high 4 (low nibble)
///            +4 finger id (low nibble) | event (high nibble)
///          a key carries only its +4 byte: key id (low nibble) |
///          state (high nibble, non-zero while pressed)
/// ```
///
/// A report carrying a key is taken as that key alone, as `cst66xx_report`
/// takes it: LilyGo report keys and coordinates one at a time, key first.
fn decode_report(buf: &[u8], shape: Shape) -> Report {
    let total = shape.keys + shape.fingers;
    if buf.len() < shape.total_len() {
        return Report::Junk;
    }

    // An idle part serves a block of zeros rather than an empty report, which
    // is what it does between touches and what it answers if polled without
    // its interrupt having fired. Calling that a torn read would be worse than
    // wrong: a lift-off that arrived this way would leave the driver believing
    // the finger was still down, with nothing to change its mind.
    if buf.iter().all(|byte| *byte == 0) {
        return Report::Empty;
    }

    // Verify before trusting any of it: on a shared bus a torn read is the
    // normal failure, and an unchecked one would dispatch a touch at a
    // coordinate nobody pressed.
    let points = &buf[4..4 + total * CST66_POINT_LEN];
    if checksum(points) != u16::from_le_bytes([buf[0], buf[1]]) {
        return Report::Junk;
    }

    if buf[2] != CST66_REPORT_POSITION {
        return Report::Empty;
    }

    if shape.keys > 0 {
        let record = buf[4 + 4];
        return Report::Key {
            id: record & 0x0F,
            pressed: record >> 4 != 0,
        };
    }

    if shape.fingers == 0 {
        return Report::Empty;
    }

    // Keys are reported first, so the fingers start past them.
    let first = 4 + shape.keys * CST66_POINT_LEN;
    // The event nibble distinguishes a press from the lift that ends one.
    // LilyGo clear the whole report when no point is down, rather than
    // trusting the count alone.
    if buf[first + 4] >> 4 == 0 {
        return Report::Empty;
    }

    Report::Contact(Contact {
        x: buf[first] as u16 | (((buf[first + 3] & 0x0F) as u16) << 8),
        y: buf[first + 1] as u16 | (((buf[first + 3] & 0xF0) as u16) << 4),
    })
}

/// Read the chip's identity out of a configuration block.
///
/// `None` when the block lacks the `0xCACA` signature, which is how LilyGo
/// tell a CST66xx from whatever else might answer on this address.
fn parse_info(buf: &[u8; CST66_INFO_LEN]) -> Option<ChipInfo> {
    if buf[2..4] != CST66_SIGNATURE {
        return None;
    }
    Some(ChipInfo {
        res_x: u16::from_le_bytes([buf[28], buf[29]]),
        res_y: u16::from_le_bytes([buf[30], buf[31]]),
        chip_type: u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]),
        firmware: u32::from_le_bytes([buf[32], buf[33], buf[34], buf[35]]),
        project_id: u32::from_le_bytes([buf[36], buf[37], buf[38], buf[39]]),
    })
}

/// Map a contact into panel pixels.
///
/// Only a clamp.  The chip reports in the panel's own resolution and in the
/// orientation the display already presents — corner taps on the Max give
/// `(13, 9)` top left and `(213, 313)` bottom right — so despite the display
/// driver rotating this panel 180°, the touch frame needs no turn of its own.
/// An earlier guess that it did put every contact in the opposite corner.
fn to_panel(c: Contact, max_x: u16, max_y: u16) -> (u16, u16) {
    let w = if max_x > 0 { max_x } else { 240 };
    let h = if max_y > 0 { max_y } else { 320 };
    (c.x.min(w - 1), c.y.min(h - 1))
}

/// The keycode a touch key reports, or `None` for a key the board gave no
/// meaning.
fn keycode_for(id: u8, home_key: i32, back_key: i32) -> Option<u16> {
    match id as i32 {
        k if k == home_key => Some(HAL_KEY_HOME),
        k if k == back_key => Some(HAL_KEY_BACK),
        _ => None,
    }
}

/// What one key report does to the key held down.
///
/// Returns the key now held, then the key to release and the key to press,
/// in that order. One key is tracked at a time: a press of another releases
/// the first, and a release of a key that is not held changes nothing.
fn key_step(held: Option<u8>, id: u8, pressed: bool) -> (Option<u8>, Option<u8>, Option<u8>) {
    match (pressed, held) {
        (true, Some(h)) if h == id => (held, None, None),
        (true, _) => (Some(id), held, Some(id)),
        (false, Some(h)) if h == id => (None, Some(id), None),
        (false, _) => (held, None, None),
    }
}

// ── I2C helpers ──────────────────────────────────────────────────────────────

/// Send a four-byte command, with no payload and no reply.
///
/// # Safety
/// Requires a valid I2C device handle in `S_TOUCH.dev`.
unsafe fn cst66_command(reg: u32) -> i32 {
    let addr = reg.to_be_bytes();
    i2c_master_transmit(S_TOUCH.dev, addr.as_ptr(), addr.len(), 50)
}

/// Write a four-byte address, let the bus go idle, then read the reply as a
/// separate transaction.
///
/// The STOP in the middle is not incidental: LilyGo read this part with two
/// transactions, and a repeated START in their place returns nothing coherent.
///
/// # Safety
/// `buf` must be writable for its whole length, and `S_TOUCH.dev` valid.
unsafe fn cst66_read_at(reg: u32, buf: &mut [u8]) -> i32 {
    let ret = cst66_command(reg);
    if ret != ESP_OK {
        return ret;
    }
    i2c_master_receive(S_TOUCH.dev, buf.as_mut_ptr(), buf.len(), 50)
}

/// Continue reading the report already in flight, with no address of its own.
///
/// # Safety
/// Only valid immediately after `cst66_read_at` on the report block.
unsafe fn cst66_read_more(buf: &mut [u8]) -> i32 {
    i2c_master_receive(S_TOUCH.dev, buf.as_mut_ptr(), buf.len(), 50)
}

/// Select normal reporting.
///
/// # Safety
/// Requires a valid I2C device handle in `S_TOUCH.dev`.
unsafe fn cst66_set_normal_mode() -> i32 {
    // Twice, as LilyGo do: the first write wakes the chip out of its low-power
    // I²C mode, and only the second is acted on.
    let mut ret = cst66_command(CST66_CMD_WAKE);
    vTaskDelay(ms_to_ticks(1));
    ret |= cst66_command(CST66_CMD_WAKE);
    for cmd in CST66_CMD_NORMAL {
        ret |= cst66_command(cmd);
    }
    ret
}

// ── ISR ──────────────────────────────────────────────────────────────────────

/// # Safety
/// Registered with `gpio_isr_handler_add`; runs in interrupt context.
unsafe extern "C" fn cst66xx_isr_handler(_arg: *mut c_void) {
    S_TOUCH.irq_pending.store(true, Ordering::Relaxed);
}

// ── Reset ────────────────────────────────────────────────────────────────────

/// Drive the reset line, wherever the board put it.
///
/// # Safety
/// Calls ESP-IDF GPIO APIs, or the expander driver's global registry.
unsafe fn cst66_drive_reset(high: bool) {
    if S_TOUCH.cfg.rst_expander >= 0 {
        crate::drv_ioexp_xl9555::set_level(
            S_TOUCH.cfg.rst_expander as usize,
            S_TOUCH.cfg.rst_expander_pin as u8,
            high,
        );
    } else if S_TOUCH.cfg.pin_rst != GPIO_NUM_NC {
        gpio_set_level(S_TOUCH.cfg.pin_rst, if high { 1 } else { 0 });
    }
}

/// Pulse reset and give the chip time to come up, as `cst66xx_init` does.
///
/// # Safety
/// Calls ESP-IDF GPIO and FreeRTOS APIs.
unsafe fn cst66_hw_reset() {
    if S_TOUCH.cfg.rst_expander < 0 && S_TOUCH.cfg.pin_rst == GPIO_NUM_NC {
        return;
    }
    cst66_drive_reset(false);
    vTaskDelay(ms_to_ticks(8));
    cst66_drive_reset(true);
    vTaskDelay(ms_to_ticks(50));
}

/// Wait for the part to acknowledge its address after a reset.
///
/// This guard is not decoration. A write-only `i2c_master_transmit` to a
/// device that does not acknowledge does not fail on this ESP-IDF — it spins
/// inside `s_i2c_send_commands` until the task watchdog fires, and every
/// command this driver sends is write-only. Probing first means a touch panel
/// that never comes up costs us an error line instead of the boot.
///
/// Returns the milliseconds waited, or `None` if it never answered.
///
/// # Safety
/// Requires a valid bus handle in `S_TOUCH.cfg.i2c_bus`.
unsafe fn cst66_wait_for_ack() -> Option<u32> {
    let mut waited = 0;
    while waited <= CST66_ACK_TIMEOUT_MS {
        if i2c_master_probe(S_TOUCH.cfg.i2c_bus, S_TOUCH.cfg.i2c_addr as u16, 50) == ESP_OK {
            return Some(waited);
        }
        vTaskDelay(ms_to_ticks(CST66_ACK_POLL_MS));
        waited += CST66_ACK_POLL_MS;
    }
    None
}

// ── HAL vtable functions ─────────────────────────────────────────────────────

/// Initialise the CST66xx touch controller.
///
/// # Safety
/// Called from C via the HAL vtable; `config` must point to a valid
/// `TouchCst66xxConfig`.
unsafe extern "C" fn cst66xx_init(config: *const c_void) -> i32 {
    if config.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    if S_TOUCH.initialized {
        return ESP_OK;
    }

    let src = &*(config as *const TouchCst66xxConfig);
    S_TOUCH.cfg.i2c_bus = src.i2c_bus;
    S_TOUCH.cfg.i2c_addr = src.i2c_addr;
    S_TOUCH.cfg.pin_int = src.pin_int;
    S_TOUCH.cfg.pin_rst = src.pin_rst;
    S_TOUCH.cfg.rst_expander = src.rst_expander;
    S_TOUCH.cfg.rst_expander_pin = src.rst_expander_pin;
    S_TOUCH.cfg.max_x = src.max_x;
    S_TOUCH.cfg.max_y = src.max_y;
    S_TOUCH.cfg.home_key = src.home_key;
    S_TOUCH.cfg.back_key = src.back_key;

    S_TOUCH.irq_pending.store(false, Ordering::Relaxed);
    S_TOUCH.touching = false;
    S_TOUCH.key_down = None;

    // ── Reset, on a GPIO or through an expander ───────────────────────────
    if S_TOUCH.cfg.rst_expander >= 0 {
        crate::drv_ioexp_xl9555::set_as_output(
            S_TOUCH.cfg.rst_expander as usize,
            S_TOUCH.cfg.rst_expander_pin as u8,
        );
    } else if S_TOUCH.cfg.pin_rst != GPIO_NUM_NC {
        let rst_cfg = GpioConfig {
            pin_bit_mask: 1u64 << S_TOUCH.cfg.pin_rst,
            mode: GPIO_MODE_OUTPUT,
            pull_up_en: GPIO_PULLUP_DISABLE,
            pull_down_en: GPIO_PULLDOWN_DISABLE,
            intr_type: GPIO_INTR_DISABLE,
        };
        let ret = gpio_config(&rst_cfg);
        if ret != ESP_OK {
            return ret;
        }
    }

    // ── Add I2C device ────────────────────────────────────────────────────
    let dev_cfg = I2cDeviceConfig {
        dev_addr_length: 0, // I2C_ADDR_BIT_LEN_7
        device_address: S_TOUCH.cfg.i2c_addr as u16,
        scl_speed_hz: CST66_I2C_HZ,
    };
    let ret = i2c_master_bus_add_device(S_TOUCH.cfg.i2c_bus, &dev_cfg, &mut S_TOUCH.dev);
    if ret != ESP_OK {
        return ret;
    }

    cst66_hw_reset();

    let Some(waited_ms) = cst66_wait_for_ack() else {
        log_no_ack();
        i2c_master_bus_rm_device(S_TOUCH.dev);
        S_TOUCH.dev = std::ptr::null_mut();
        return ESP_FAIL;
    };
    log_ack(waited_ms);

    // ── Probe ─────────────────────────────────────────────────────────────
    //
    // The configuration block only reads back once the chip is in normal mode
    // and awake, and it takes a few goes to get there from cold — hence the
    // retry, which is LilyGo's too.
    let mut info = None;
    for _ in 0..5 {
        cst66_set_normal_mode();
        let mut block = [0u8; CST66_INFO_LEN];
        if cst66_read_at(CST66_REG_INFO, &mut block) == ESP_OK {
            if let Some(parsed) = parse_info(&block) {
                info = Some(parsed);
                break;
            }
        }
        vTaskDelay(ms_to_ticks(10));
        cst66_command(CST66_CMD_WAKE);
    }

    let Some(info) = info else {
        log_probe_failed();
        i2c_master_bus_rm_device(S_TOUCH.dev);
        S_TOUCH.dev = std::ptr::null_mut();
        return ESP_FAIL;
    };
    log_identity(&info);

    if cst66_set_normal_mode() != ESP_OK {
        i2c_master_bus_rm_device(S_TOUCH.dev);
        S_TOUCH.dev = std::ptr::null_mut();
        return ESP_FAIL;
    }

    // ── Optional interrupt pin ────────────────────────────────────────────
    if S_TOUCH.cfg.pin_int != GPIO_NUM_NC {
        let int_cfg = GpioConfig {
            pin_bit_mask: 1u64 << S_TOUCH.cfg.pin_int,
            mode: GPIO_MODE_INPUT,
            pull_up_en: GPIO_PULLUP_ENABLE,
            pull_down_en: GPIO_PULLDOWN_DISABLE,
            intr_type: GPIO_INTR_NEGEDGE,
        };
        let ret = gpio_config(&int_cfg);
        if ret != ESP_OK {
            i2c_master_bus_rm_device(S_TOUCH.dev);
            S_TOUCH.dev = std::ptr::null_mut();
            return ret;
        }

        let ret = crate::gpio_isr_service::ensure_installed(0);
        if ret != ESP_OK {
            i2c_master_bus_rm_device(S_TOUCH.dev);
            S_TOUCH.dev = std::ptr::null_mut();
            return ret;
        }

        let ret =
            gpio_isr_handler_add(S_TOUCH.cfg.pin_int, cst66xx_isr_handler, std::ptr::null_mut());
        if ret != ESP_OK {
            i2c_master_bus_rm_device(S_TOUCH.dev);
            S_TOUCH.dev = std::ptr::null_mut();
            return ret;
        }
    }

    S_TOUCH.initialized = true;
    ESP_OK
}

/// # Safety
/// Called from C via the HAL vtable.
unsafe extern "C" fn cst66xx_deinit() {
    if !S_TOUCH.initialized {
        return;
    }
    if S_TOUCH.cfg.pin_int != GPIO_NUM_NC {
        gpio_isr_handler_remove(S_TOUCH.cfg.pin_int);
    }
    i2c_master_bus_rm_device(S_TOUCH.dev);
    S_TOUCH.dev = std::ptr::null_mut();
    S_TOUCH.cb = None;
    S_TOUCH.cb_data = std::ptr::null_mut();
    S_TOUCH.touching = false;
    S_TOUCH.key_down = None;
    S_TOUCH.initialized = false;
    S_TOUCH.asleep = false;
}

/// Put the panel in deep sleep, or reset it out. Asleep it draws next to
/// nothing and wakes nobody: the device does not wake on a touch.
///
/// # Safety
/// Called from C via the HAL vtable.
unsafe extern "C" fn cst66xx_sleep(enter: bool) -> i32 {
    if !S_TOUCH.initialized {
        return ESP_ERR_INVALID_STATE;
    }
    if enter {
        if S_TOUCH.asleep {
            return ESP_OK;
        }
        // Out of its low-power I2C mode first, as for any command.
        let mut ret = cst66_command(CST66_CMD_WAKE);
        vTaskDelay(ms_to_ticks(1));
        ret |= cst66_command(CST66_CMD_WAKE);
        ret |= cst66_command(CST66_CMD_DEEP_SLEEP);
        // Whatever it answered, it is left alone until woken by a reset.
        S_TOUCH.asleep = true;
        S_TOUCH.touching = false;
        S_TOUCH.key_down = None;
        S_TOUCH.irq_pending.store(false, Ordering::Relaxed);
        return ret;
    }
    if !S_TOUCH.asleep {
        return ESP_OK;
    }
    cst66_hw_reset();
    let Some(waited_ms) = cst66_wait_for_ack() else {
        // Still asleep as far as anything here can tell.
        log_no_ack();
        return ESP_FAIL;
    };
    log_ack(waited_ms);
    S_TOUCH.asleep = false;
    S_TOUCH.irq_pending.store(false, Ordering::Relaxed);
    cst66_set_normal_mode()
}

/// # Safety
/// Called from C via the HAL vtable.
unsafe extern "C" fn cst66xx_register_callback(cb: HalInputCb, user_data: *mut c_void) -> i32 {
    S_TOUCH.cb = cb;
    S_TOUCH.cb_data = user_data;
    ESP_OK
}

/// Read one report, retrying once past a torn one, as LilyGo do.
///
/// # Safety
/// Requires a valid I2C device handle in `S_TOUCH.dev`.
unsafe fn cst66_read_report() -> Report {
    for _ in 0..2 {
        let mut buf = [0u8; CST66_REPORT_MAX];
        if cst66_read_at(CST66_REG_REPORT, &mut buf[..CST66_HEADER_LEN]) != ESP_OK {
            continue;
        }

        let mut header = [0u8; CST66_HEADER_LEN];
        header.copy_from_slice(&buf[..CST66_HEADER_LEN]);
        let Some(shape) = report_shape(&header) else {
            // Counts that cannot be right mean the stream is out of step;
            // release the buffer and start over rather than reading on.
            cst66_command(CST66_CMD_RELEASE);
            continue;
        };

        let extra = shape.continuation_len();
        if extra > 0
            && cst66_read_more(&mut buf[CST66_HEADER_LEN..CST66_HEADER_LEN + extra]) != ESP_OK
        {
            continue;
        }

        let report = decode_report(&buf[..shape.total_len()], shape);
        // The chip holds this report until it is released, whatever it said.
        cst66_command(CST66_CMD_RELEASE);
        if report != Report::Junk {
            return report;
        }
    }
    Report::Junk
}

/// Send a key event, if the board gave this touch key a keycode.
///
/// # Safety
/// Reads the driver state; called from `cst66xx_poll` only.
unsafe fn send_key(id: u8, event_type: HalInputEventType, now_ms: u32) {
    let keycode = keycode_for(id, S_TOUCH.cfg.home_key, S_TOUCH.cfg.back_key);
    let (Some(keycode), Some(cb)) = (keycode, S_TOUCH.cb) else {
        return;
    };
    let event = HalInputEvent {
        event_type,
        timestamp: now_ms,
        data: HalInputEventData {
            key: HalInputKeyData { keycode },
        },
    };
    cb(&event, S_TOUCH.cb_data);
}

/// Follow a touch key down or up, and report what changed.
///
/// # Safety
/// Mutates the driver state; called from `cst66xx_poll` only.
unsafe fn key_report(id: u8, pressed: bool, now_ms: u32) {
    let (held, released, newly_pressed) = key_step(S_TOUCH.key_down, id, pressed);
    S_TOUCH.key_down = held;
    if let Some(r) = released {
        send_key(r, HalInputEventType::KeyUp, now_ms);
    }
    if let Some(p) = newly_pressed {
        log_key(p, keycode_for(p, S_TOUCH.cfg.home_key, S_TOUCH.cfg.back_key));
        send_key(p, HalInputEventType::KeyDown, now_ms);
    }
}

/// Poll for new touch events and dispatch callbacks.
///
/// # Safety
/// Called from C via the HAL vtable.
unsafe extern "C" fn cst66xx_poll() -> i32 {
    if !S_TOUCH.initialized {
        return ESP_ERR_INVALID_STATE;
    }
    if S_TOUCH.asleep {
        return ESP_OK;
    }

    // Interrupt-driven optimisation: skip I2C when no edge was seen and we are
    // not mid-touch, since lift-off may not raise a second interrupt.
    // Three things can say there is something to read, and any one will do.
    //
    // The edge catches a pulse too short to see; the level catches data that
    // is still waiting when we get here, which is what an edge-only gate
    // misses if the interrupt is lost, shared, or never wired; and `touching`
    // covers lift-off, which need not raise an interrupt of its own. Relying
    // on the edge alone makes a missed interrupt indistinguishable from a
    // finger that was never there.
    if S_TOUCH.cfg.pin_int != GPIO_NUM_NC
        && !S_TOUCH.irq_pending.load(Ordering::Relaxed)
        && gpio_get_level(S_TOUCH.cfg.pin_int) != 0
        && !S_TOUCH.touching
        && S_TOUCH.key_down.is_none()
    {
        return ESP_OK;
    }

    let report = cst66_read_report();
    let now_ms = (esp_timer_get_time() / 1000) as u32;

    match report {
        Report::Contact(contact) => {
            let (x, y) = to_panel(contact, S_TOUCH.cfg.max_x, S_TOUCH.cfg.max_y);
            let ev_type = if !S_TOUCH.touching {
                S_TOUCH.touching = true;
                HalInputEventType::TouchDown
            } else {
                HalInputEventType::TouchMove
            };
            S_TOUCH.last_x = x;
            S_TOUCH.last_y = y;
            if let Some(cb) = S_TOUCH.cb {
                let event = HalInputEvent {
                    event_type: ev_type,
                    timestamp: now_ms,
                    data: HalInputEventData {
                        touch: HalInputTouchData { x, y },
                    },
                };
                cb(&event, S_TOUCH.cb_data);
            }
        }
        Report::Key { id, pressed } => key_report(id, pressed, now_ms),
        Report::Empty => {
            // Whether a key's release comes as a key report or as an idle
            // one, it ends here.
            if let Some(id) = S_TOUCH.key_down {
                key_report(id, false, now_ms);
            }
            if S_TOUCH.touching {
                S_TOUCH.touching = false;
                if let Some(cb) = S_TOUCH.cb {
                    let event = HalInputEvent {
                        event_type: HalInputEventType::TouchUp,
                        timestamp: now_ms,
                        data: HalInputEventData {
                            touch: HalInputTouchData {
                                x: S_TOUCH.last_x,
                                y: S_TOUCH.last_y,
                            },
                        },
                    };
                    cb(&event, S_TOUCH.cb_data);
                }
            }
        }
        // A read that never came back clean says nothing about the finger, so
        // leave the touch state alone and try again next poll.
        Report::Junk => {}
    }

    S_TOUCH.irq_pending.store(false, Ordering::Relaxed);
    ESP_OK
}

// ── HAL vtable ───────────────────────────────────────────────────────────────

static TOUCH_DRIVER: HalInputDriver = HalInputDriver {
    init: Some(cst66xx_init),
    deinit: Some(cst66xx_deinit),
    register_callback: Some(cst66xx_register_callback),
    poll: Some(cst66xx_poll),
    name: b"CST66xx\0".as_ptr() as *const c_char,
    is_touch: true,
    set_backlight: None,
    sleep: Some(cst66xx_sleep),
};

/// Return a pointer to the CST66xx HAL input driver vtable.
///
/// # Safety
/// May be called from C.  The pointer is valid for the program lifetime.
#[no_mangle]
pub extern "C" fn drv_touch_cst66xx_get() -> *const HalInputDriver {
    &TOUCH_DRIVER
}

// ── Tests ────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a report carrying `fingers` points, the first at (`x`, `y`).
    fn report(x: u16, y: u16, fingers: usize, keys: usize, event: u8) -> Vec<u8> {
        let total = keys + fingers;
        let mut buf = vec![0u8; CST66_HEADER_LEN + (total.saturating_sub(1)) * CST66_POINT_LEN];
        buf[2] = CST66_REPORT_POSITION;
        buf[3] = (fingers as u8) | ((keys as u8) << 4);

        let first = 4 + keys * CST66_POINT_LEN;
        buf[first] = (x & 0xFF) as u8;
        buf[first + 1] = (y & 0xFF) as u8;
        buf[first + 2] = 0x40; // pressure
        buf[first + 3] = (((y >> 4) & 0xF0) | ((x >> 8) & 0x0F)) as u8;
        buf[first + 4] = event << 4;

        let sum = checksum(&buf[4..4 + total * CST66_POINT_LEN]);
        buf[0..2].copy_from_slice(&sum.to_le_bytes());
        buf
    }

    fn decode(buf: &[u8]) -> Report {
        let mut header = [0u8; CST66_HEADER_LEN];
        header.copy_from_slice(&buf[..CST66_HEADER_LEN]);
        let shape = report_shape(&header).expect("shape");
        decode_report(buf, shape)
    }

    fn contact(buf: &[u8]) -> Option<Contact> {
        match decode(buf) {
            Report::Contact(c) => Some(c),
            _ => None,
        }
    }

    // ── Report shape ─────────────────────────────────────────────────────

    #[test]
    fn test_a_single_point_needs_no_continuation_read() {
        let shape = report_shape(&[0, 0, 0xFF, 0x01, 0, 0, 0, 0, 0]).unwrap();
        assert_eq!(shape.continuation_len(), 0);
        assert_eq!(shape.total_len(), CST66_HEADER_LEN);
    }

    #[test]
    fn test_each_point_past_the_first_adds_five_bytes() {
        let shape = report_shape(&[0, 0, 0xFF, 0x03, 0, 0, 0, 0, 0]).unwrap();
        assert_eq!(shape.continuation_len(), 10);
        assert_eq!(shape.total_len(), 19);
    }

    #[test]
    fn test_keys_and_fingers_share_the_point_budget() {
        let shape = report_shape(&[0, 0, 0xFF, 0x12, 0, 0, 0, 0, 0]).unwrap();
        assert_eq!(shape.keys, 1);
        assert_eq!(shape.fingers, 2);
        assert_eq!(shape.continuation_len(), 10);
    }

    #[test]
    fn test_an_impossible_point_count_has_no_shape() {
        // Six points cannot be right, and reading the length it implies would
        // desynchronise the stream.
        assert!(report_shape(&[0, 0, 0xFF, 0x33, 0, 0, 0, 0, 0]).is_none());
    }

    // ── Decoding ─────────────────────────────────────────────────────────

    #[test]
    fn test_decode_splits_each_coordinate_across_two_bytes() {
        // Each axis carries its low eight bits in its own byte and its high
        // four in a shared byte — X in that byte's low nibble, Y in its high.
        let c = contact(&report(0x123, 0x0AB, 1, 0, 6)).unwrap();
        assert_eq!(c.x, 0x123);
        assert_eq!(c.y, 0x0AB);
    }

    #[test]
    fn test_decode_handles_the_full_twelve_bit_range() {
        assert_eq!(
            contact(&report(0xFFF, 0xFFF, 1, 0, 6)).unwrap(),
            Contact { x: 0xFFF, y: 0xFFF }
        );
        assert_eq!(
            contact(&report(0, 0, 1, 0, 6)).unwrap(),
            Contact { x: 0, y: 0 }
        );
    }

    #[test]
    fn test_an_idle_part_serving_zeros_reads_as_empty_not_junk() {
        // Observed on hardware: with nobody touching, the report block comes
        // back all zeros. Treating that as a torn read would strand the
        // driver mid-touch, because only an empty report releases it.
        let buf = [0u8; CST66_HEADER_LEN];
        let shape = report_shape(&buf).unwrap();
        assert_eq!(decode_report(&buf, shape), Report::Empty);
    }

    #[test]
    fn test_decode_rejects_a_report_whose_checksum_does_not_hold() {
        // A torn read on a shared bus is the normal failure here, and an
        // unchecked one would dispatch a touch nobody made.
        let mut buf = report(100, 200, 1, 0, 6);
        buf[0] ^= 0xFF;
        assert_eq!(decode(&buf), Report::Junk);
    }

    #[test]
    fn test_decode_notices_corruption_in_the_point_data_itself() {
        let mut buf = report(100, 200, 1, 0, 6);
        buf[4] = buf[4].wrapping_add(1);
        assert_eq!(decode(&buf), Report::Junk);
    }

    #[test]
    fn test_decode_treats_a_lifting_finger_as_empty() {
        assert_eq!(decode(&report(100, 200, 1, 0, 0)), Report::Empty);
    }

    #[test]
    fn test_decode_treats_a_report_with_no_fingers_as_empty() {
        assert_eq!(decode(&report(0, 0, 0, 0, 0)), Report::Empty);
    }

    #[test]
    fn test_decode_ignores_gesture_and_proximity_reports() {
        // They borrow the same block but carry no coordinates.
        for kind in [0xF0u8, 0xE0] {
            let mut buf = report(100, 200, 1, 0, 6);
            buf[2] = kind;
            let sum = checksum(&buf[4..4 + CST66_POINT_LEN]);
            buf[0..2].copy_from_slice(&sum.to_le_bytes());
            assert_eq!(decode(&buf), Report::Empty);
        }
    }

    /// Build a report carrying one touch key and no finger.
    fn key_report_buf(id: u8, state: u8) -> Vec<u8> {
        let mut buf = vec![0u8; CST66_HEADER_LEN];
        buf[2] = CST66_REPORT_POSITION;
        buf[3] = 1 << 4;
        buf[8] = (state << 4) | id;
        let sum = checksum(&buf[4..4 + CST66_POINT_LEN]);
        buf[0..2].copy_from_slice(&sum.to_le_bytes());
        buf
    }

    #[test]
    fn test_decode_reads_a_key_press_and_its_release() {
        assert_eq!(decode(&key_report_buf(1, 1)), Report::Key { id: 1, pressed: true });
        assert_eq!(decode(&key_report_buf(1, 0)), Report::Key { id: 1, pressed: false });
    }

    #[test]
    fn test_decode_takes_a_report_with_a_key_as_the_key_alone() {
        // LilyGo report the key and drop the finger beside it.
        let mut buf = report(100, 200, 1, 1, 6);
        buf[8] = (1 << 4) | 2;
        let sum = checksum(&buf[4..4 + 2 * CST66_POINT_LEN]);
        buf[0..2].copy_from_slice(&sum.to_le_bytes());
        assert_eq!(decode(&buf), Report::Key { id: 2, pressed: true });
    }

    #[test]
    fn test_decode_rejects_a_key_report_whose_checksum_does_not_hold() {
        let mut buf = key_report_buf(0, 1);
        buf[8] ^= 0x02;
        assert_eq!(decode(&buf), Report::Junk);
    }

    // ── Touch keys ───────────────────────────────────────────────────────

    #[test]
    fn test_only_the_configured_key_reports_home() {
        assert_eq!(keycode_for(1, 1, -1), Some(HAL_KEY_HOME));
        assert_eq!(keycode_for(0, 1, -1), None);
        assert_eq!(keycode_for(2, 1, -1), None);
        // -1, the default, gives no key a meaning.
        assert_eq!(keycode_for(0, -1, -1), None);
    }

    #[test]
    fn test_the_t_deck_max_keys_report_home_left_and_back_right() {
        // tdeck-max.json: the heart key, leftmost, is 0; the rightmost is 2.
        assert_eq!(keycode_for(0, 0, 2), Some(HAL_KEY_HOME));
        assert_eq!(keycode_for(1, 0, 2), None);
        assert_eq!(keycode_for(2, 0, 2), Some(HAL_KEY_BACK));
    }

    #[test]
    fn test_a_key_press_is_reported_once_however_often_the_chip_repeats_it() {
        assert_eq!(key_step(None, 1, true), (Some(1), None, Some(1)));
        assert_eq!(key_step(Some(1), 1, true), (Some(1), None, None));
    }

    #[test]
    fn test_a_release_ends_the_press_of_that_key_only() {
        assert_eq!(key_step(Some(1), 1, false), (None, Some(1), None));
        assert_eq!(key_step(Some(1), 0, false), (Some(1), None, None));
        assert_eq!(key_step(None, 1, false), (None, None, None));
    }

    #[test]
    fn test_pressing_a_second_key_releases_the_first() {
        assert_eq!(key_step(Some(0), 2, true), (Some(2), Some(0), Some(2)));
    }

    #[test]
    fn test_decode_rejects_a_buffer_short_of_its_own_shape() {
        let buf = report(100, 200, 2, 0, 6);
        let mut header = [0u8; CST66_HEADER_LEN];
        header.copy_from_slice(&buf[..CST66_HEADER_LEN]);
        let shape = report_shape(&header).unwrap();
        assert_eq!(decode_report(&buf[..CST66_HEADER_LEN], shape), Report::Junk);
    }

    // ── Chip info ────────────────────────────────────────────────────────

    #[test]
    fn test_parse_info_reads_what_the_factory_firmware_reported() {
        // The values LilyGo's own firmware printed on this board:
        //   ictype:[caca220e] fw_project_id:2512010 fw_ver:1010001
        let mut buf = [0u8; CST66_INFO_LEN];
        buf[0..4].copy_from_slice(&0xCACA_220Eu32.to_le_bytes());
        buf[28..30].copy_from_slice(&240u16.to_le_bytes());
        buf[30..32].copy_from_slice(&320u16.to_le_bytes());
        buf[32..36].copy_from_slice(&0x0101_0001u32.to_le_bytes());
        buf[36..40].copy_from_slice(&0x0251_2010u32.to_le_bytes());

        let info = parse_info(&buf).unwrap();
        assert_eq!(info.chip_type, 0xCACA_220E);
        assert_eq!(info.project_id, 0x0251_2010);
        assert_eq!(info.firmware, 0x0101_0001);
        assert_eq!((info.res_x, info.res_y), (240, 320));
    }

    #[test]
    fn test_parse_info_rejects_a_block_without_the_signature() {
        let mut buf = [0u8; CST66_INFO_LEN];
        buf[0..4].copy_from_slice(&0x1234_220Eu32.to_le_bytes());
        assert!(parse_info(&buf).is_none());
    }

    // ── Panel mapping ────────────────────────────────────────────────────

    #[test]
    fn test_to_panel_keeps_the_corners_the_hardware_reported() {
        // Measured on a T-Deck Max: a tap in the top left reads (13, 9) and
        // one in the bottom right reads (213, 313). Both must survive intact,
        // or the display's own 180° rotation gets applied twice.
        assert_eq!(to_panel(Contact { x: 13, y: 9 }, 240, 320), (13, 9));
        assert_eq!(to_panel(Contact { x: 213, y: 313 }, 240, 320), (213, 313));
    }

    #[test]
    fn test_to_panel_clamps_a_contact_past_the_edge() {
        assert_eq!(to_panel(Contact { x: 400, y: 900 }, 240, 320), (239, 319));
    }

    // ── Vtable ───────────────────────────────────────────────────────────

    #[test]
    fn test_vtable_is_wired_up() {
        let drv = unsafe { &*drv_touch_cst66xx_get() };
        assert!(drv.init.is_some());
        assert!(drv.deinit.is_some());
        assert!(drv.register_callback.is_some());
        assert!(drv.poll.is_some());
        assert!(drv.is_touch);
        assert!(!drv.name.is_null());
    }

    #[test]
    fn test_poll_before_init_returns_invalid_state() {
        unsafe {
            S_TOUCH.initialized = false;
            assert_eq!(cst66xx_poll(), ESP_ERR_INVALID_STATE);
        }
    }

    #[test]
    fn test_init_rejects_a_null_config() {
        unsafe {
            S_TOUCH.initialized = false;
            assert_eq!(cst66xx_init(std::ptr::null()), ESP_ERR_INVALID_ARG);
            assert!(!S_TOUCH.initialized);
        }
    }

    #[test]
    fn sleep_is_refused_before_init() {
        unsafe {
            S_TOUCH.initialized = false;
            assert_eq!(cst66xx_sleep(true), ESP_ERR_INVALID_STATE);
        }
    }

    #[test]
    fn asleep_the_panel_is_left_alone_until_a_reset_wakes_it() {
        unsafe {
            S_TOUCH = TouchState::new();
            S_TOUCH.initialized = true;
            S_TOUCH.dev = 1usize as *mut c_void;
            S_TOUCH.cfg.pin_int = 12;
            assert_eq!(cst66xx_sleep(true), ESP_OK);
            assert!(S_TOUCH.asleep);

            // Even an interrupt does not send it anything.
            let before = TRANSMITS.load(Ordering::Relaxed);
            S_TOUCH.irq_pending.store(true, Ordering::Relaxed);
            assert_eq!(cst66xx_poll(), ESP_OK);
            assert_eq!(TRANSMITS.load(Ordering::Relaxed), before);

            assert_eq!(cst66xx_sleep(false), ESP_OK);
            assert!(!S_TOUCH.asleep);
            S_TOUCH = TouchState::new();
        }
    }
}

