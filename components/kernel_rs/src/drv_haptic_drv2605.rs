// SPDX-License-Identifier: BSD-3-Clause
//! TI DRV2605 haptic driver: the vibration motor on the T-Deck Max.
//!
//! The part plays waveforms from a ROM library on its own once told which
//! and given GO, so a play is a handful of register writes on the kernel
//! worker. Between plays it is held in standby, at 1.9 µA rather than the
//! 0.6 mA it draws ready (datasheet 6.5); since standby stops a waveform at
//! once (7.4.1.4), a job a little later waits for GO to clear before setting
//! it. Nothing waits on the worker meanwhile.
//!
//! Set up as LilyGo's own examples set it up (T-Deck-MAX
//! `examples/motor/basic`, `lib/Adafruit_DRV2605`): an ERM motor, open loop,
//! TS2200 Library A, internal trigger. The part's power is an IO expander
//! pin on the T-Deck Max (XL9555 P05, `M_EN`), which the profile names.

use std::os::raw::{c_char, c_void};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use crate::hal_registry::{HalHapticDriver, HAL_HAPTIC_CLICK, HAL_HAPTIC_MESSAGE};

const ESP_OK: i32 = 0;
const ESP_FAIL: i32 = -1;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_ERR_NOT_FOUND: i32 = 0x105;

const DEFAULT_ADDRESS: u8 = 0x5A;
const I2C_SPEED_HZ: u32 = 400_000;
const I2C_TIMEOUT_MS: i32 = 50;

// ── Registers (datasheet 8.6) ────────────────────────────────────────────────

const REG_STATUS: u8 = 0x00;
const REG_MODE: u8 = 0x01;
const REG_RTP_INPUT: u8 = 0x02;
const REG_LIBRARY: u8 = 0x03;
const REG_WAVESEQ1: u8 = 0x04;
const REG_GO: u8 = 0x0C;
const REG_OVERDRIVE: u8 = 0x0D;
const REG_SUSTAIN_POS: u8 = 0x0E;
const REG_SUSTAIN_NEG: u8 = 0x0F;
const REG_BRAKE: u8 = 0x10;
const REG_FEEDBACK: u8 = 0x1A;
const REG_CONTROL3: u8 = 0x1D;

const MODE_INTERNAL_TRIGGER: u8 = 0x00;
const MODE_STANDBY: u8 = 0x40;
const FEEDBACK_N_ERM_LRA: u8 = 0x80;
const CONTROL3_ERM_OPEN_LOOP: u8 = 0x20;
/// TS2200 Library A, for ERM motors, which runs open loop only (7.5.3).
const LIBRARY_A: u8 = 1;
/// A waveform sequencer slot with this bit set waits 10 ms per unit below it.
const WAIT_10MS: u8 = 0x80;
const SEQUENCER_SLOTS: usize = 8;

/// STATUS[7:5]: 3 is the DRV2605, 7 the low-voltage DRV2605L. The DRV2604s
/// (4, 6) have no ROM library to play from.
fn has_library(status: u8) -> bool {
    matches!(status >> 5, 3 | 7)
}

// Library A's waveforms (datasheet 11.2).
const STRONG_CLICK: u8 = 1;
const STRONG_BUZZ: u8 = 14;

/// What each effect plays.
fn sequence(effect: u32) -> Option<&'static [u8]> {
    match effect {
        HAL_HAPTIC_CLICK => Some(&[STRONG_CLICK]),
        HAL_HAPTIC_MESSAGE => Some(&[STRONG_BUZZ, WAIT_10MS | 10, STRONG_BUZZ]),
        _ => None,
    }
}

/// From EN high to the part answering: 1.5 ms typical (datasheet 6.7).
const POWER_UP: Duration = Duration::from_millis(5);
/// How often a play is looked at to see if it has ended.
const SETTLE_POLL: Duration = Duration::from_millis(100);
/// A play still going after this is stopped: none of ours is near it.
const PLAY_LIMIT: Duration = Duration::from_secs(3);

// ── ESP-IDF FFI ──────────────────────────────────────────────────────────────

#[cfg(any(target_os = "espidf", feature = "sim-bus"))]
mod esp_ffi {
    use std::os::raw::c_void;

    /// i2c_device_config_t — partial layout matching ESP-IDF v5.x on Xtensa.
    #[repr(C)]
    pub struct I2cDeviceConfig {
        /// dev_addr_length: I2C_ADDR_BIT_LEN_7 = 0
        pub dev_addr_length: u32,
        pub device_address: u16,
        pub scl_speed_hz: u32,
        pub scl_wait_us: u32,
        pub flags: u32,
    }

    extern "C" {
        pub fn i2c_master_bus_add_device(
            bus: *mut c_void,
            cfg: *const I2cDeviceConfig,
            handle: *mut *mut c_void,
        ) -> i32;
        pub fn i2c_master_bus_rm_device(handle: *mut c_void) -> i32;
        pub fn i2c_master_transmit_receive(
            handle: *mut c_void,
            write_data: *const u8,
            write_size: usize,
            read_data: *mut u8,
            read_size: usize,
            timeout_ms: i32,
        ) -> i32;
        pub fn i2c_master_transmit(
            handle: *mut c_void,
            data: *const u8,
            len: usize,
            timeout_ms: i32,
        ) -> i32;
    }
}

/// A DRV2605 for host tests: its registers, a GO bit that clears after the
/// waveform has been asked about a set number of times, and a record of
/// every write.
#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
mod esp_ffi {
    use std::os::raw::c_void;
    use std::sync::Mutex;

    pub struct Chip {
        pub present: bool,
        pub regs: [u8; 0x23],
        /// Reads of GO that still find it set after a play starts.
        pub go_reads: u32,
        pub writes: Vec<(u8, u8)>,
    }

    impl Chip {
        const fn new() -> Self {
            // Reset values (datasheet 8.6): a DRV2605 in standby.
            let mut regs = [0u8; 0x23];
            regs[0x00] = 0x60;
            regs[0x01] = 0x40;
            regs[0x1A] = 0x36;
            regs[0x1D] = 0xA0;
            Chip {
                present: false,
                regs,
                go_reads: 0,
                writes: Vec::new(),
            }
        }
    }

    pub static CHIP: Mutex<Chip> = Mutex::new(Chip::new());

    /// A DRV2605 fresh from power-up, whose plays last `go_reads` looks.
    pub fn attach(go_reads: u32) {
        let mut chip = CHIP.lock().unwrap();
        *chip = Chip::new();
        chip.present = true;
        chip.go_reads = go_reads;
    }

    pub fn detach() {
        *CHIP.lock().unwrap() = Chip::new();
    }

    #[repr(C)]
    pub struct I2cDeviceConfig {
        pub dev_addr_length: u32,
        pub device_address: u16,
        pub scl_speed_hz: u32,
        pub scl_wait_us: u32,
        pub flags: u32,
    }

    pub unsafe fn i2c_master_bus_add_device(
        _bus: *mut c_void,
        _cfg: *const I2cDeviceConfig,
        handle: *mut *mut c_void,
    ) -> i32 {
        unsafe { *handle = 0x5A as *mut c_void };
        0
    }

    pub unsafe fn i2c_master_bus_rm_device(_handle: *mut c_void) -> i32 {
        0
    }

    pub unsafe fn i2c_master_transmit_receive(
        _handle: *mut c_void,
        write: *const u8,
        _wsz: usize,
        read: *mut u8,
        _rsz: usize,
        _timeout: i32,
    ) -> i32 {
        let mut chip = CHIP.lock().unwrap();
        if !chip.present {
            return -1;
        }
        let register = unsafe { *write } as usize;
        if register == 0x0C && chip.regs[0x0C] & 1 != 0 {
            if chip.go_reads == 0 {
                chip.regs[0x0C] = 0;
            } else {
                chip.go_reads -= 1;
            }
        }
        unsafe { *read = chip.regs[register] };
        0
    }

    pub unsafe fn i2c_master_transmit(
        _handle: *mut c_void,
        data: *const u8,
        len: usize,
        _timeout: i32,
    ) -> i32 {
        let mut chip = CHIP.lock().unwrap();
        if !chip.present || len != 2 {
            return -1;
        }
        let bytes = unsafe { std::slice::from_raw_parts(data, len) };
        chip.regs[bytes[0] as usize] = bytes[1];
        chip.writes.push((bytes[0], bytes[1]));
        0
    }
}

// ── Logging ──────────────────────────────────────────────────────────────────

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: u32, tag: *const u8, format: *const u8, ...);
}

static TAG: &[u8] = b"drv2605\0";

#[cfg(not(test))]
macro_rules! haptic_log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {
        unsafe { esp_log_write($level, TAG.as_ptr(), $fmt.as_ptr() $(, $arg)*); }
    };
}

#[cfg(test)]
macro_rules! haptic_log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => { let _ = ($level, $($arg,)*); };
}

const LOG_WARN: u32 = 2;
const LOG_INFO: u32 = 3;

// ── State ────────────────────────────────────────────────────────────────────

#[derive(Clone, Copy, Debug, PartialEq)]
struct Config {
    i2c_bus: i32,
    i2c_addr: u8,
    /// The IO expander pin that powers the part, if one does.
    enable: Option<(i32, i32)>,
}

const DEFAULT_CONFIG: Config = Config {
    i2c_bus: 0,
    i2c_addr: DEFAULT_ADDRESS,
    enable: None,
};

struct Haptic {
    config: Config,
    dev: *mut c_void,
    /// When the play under way began; None in standby.
    playing_since: Option<Instant>,
}

// The device handle is only used with the mutex held.
unsafe impl Send for Haptic {}

static HAPTIC: Mutex<Haptic> = Mutex::new(Haptic {
    config: DEFAULT_CONFIG,
    dev: std::ptr::null_mut(),
    playing_since: None,
});

fn lock() -> std::sync::MutexGuard<'static, Haptic> {
    HAPTIC
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

// ── I²C ──────────────────────────────────────────────────────────────────────

fn open(bus: *mut c_void, address: u8) -> Option<*mut c_void> {
    let cfg = esp_ffi::I2cDeviceConfig {
        dev_addr_length: 0,
        device_address: address as u16,
        scl_speed_hz: I2C_SPEED_HZ,
        scl_wait_us: 0,
        flags: 0,
    };
    let mut dev: *mut c_void = std::ptr::null_mut();
    let ret = unsafe { esp_ffi::i2c_master_bus_add_device(bus, &cfg, &mut dev) };
    (ret == ESP_OK && !dev.is_null()).then_some(dev)
}

fn read_reg(dev: *mut c_void, register: u8) -> Option<u8> {
    let mut value = 0u8;
    let ret = unsafe {
        esp_ffi::i2c_master_transmit_receive(dev, &register, 1, &mut value, 1, I2C_TIMEOUT_MS)
    };
    (ret == ESP_OK).then_some(value)
}

fn write_reg(dev: *mut c_void, register: u8, value: u8) -> bool {
    let buf = [register, value];
    unsafe { esp_ffi::i2c_master_transmit(dev, buf.as_ptr(), 2, I2C_TIMEOUT_MS) == ESP_OK }
}

fn update_reg(dev: *mut c_void, register: u8, f: impl FnOnce(u8) -> u8) -> bool {
    read_reg(dev, register).is_some_and(|v| write_reg(dev, register, f(v)))
}

fn power(config: &Config, on: bool) {
    if let Some((expander, pin)) = config.enable {
        let ret = crate::drv_ioexp_xl9555::thistle_expander_set_level(expander, pin, on as i32);
        if ret != ESP_OK {
            haptic_log!(LOG_WARN, b"could not switch the part's power: 0x%x\0", ret);
        }
    }
}

/// Library A, ERM, open loop, internal trigger; left in standby. Registers
/// keep their values in standby and take writes there (7.4.1.4).
fn configure(dev: *mut c_void) -> bool {
    write_reg(dev, REG_MODE, MODE_STANDBY | MODE_INTERNAL_TRIGGER)
        && write_reg(dev, REG_RTP_INPUT, 0)
        && write_reg(dev, REG_OVERDRIVE, 0)
        && write_reg(dev, REG_SUSTAIN_POS, 0)
        && write_reg(dev, REG_SUSTAIN_NEG, 0)
        && write_reg(dev, REG_BRAKE, 0)
        && write_reg(dev, REG_LIBRARY, LIBRARY_A)
        && update_reg(dev, REG_FEEDBACK, |v| v & !FEEDBACK_N_ERM_LRA)
        && update_reg(dev, REG_CONTROL3, |v| v | CONTROL3_ERM_OPEN_LOOP)
}

// ── Playing ──────────────────────────────────────────────────────────────────

/// Start `effect`. Some(delay) when a look at whether it has ended is due
/// that long from now: none is while one is already due.
fn start(effect: u32, now: Instant) -> Result<Option<Duration>, i32> {
    let steps = sequence(effect).ok_or(ESP_ERR_INVALID_ARG)?;
    let mut h = lock();
    if h.dev.is_null() {
        return Err(ESP_ERR_INVALID_STATE);
    }
    let dev = h.dev;
    let mut ok = write_reg(dev, REG_MODE, MODE_INTERNAL_TRIGGER);
    for (slot, step) in steps.iter().enumerate() {
        ok = ok && write_reg(dev, REG_WAVESEQ1 + slot as u8, *step);
    }
    if steps.len() < SEQUENCER_SLOTS {
        ok = ok && write_reg(dev, REG_WAVESEQ1 + steps.len() as u8, 0);
    }
    ok = ok && write_reg(dev, REG_GO, 1);
    if !ok {
        write_reg(dev, REG_MODE, MODE_STANDBY);
        return Err(ESP_FAIL);
    }
    let first = h.playing_since.is_none();
    h.playing_since = Some(now);
    Ok(first.then_some(SETTLE_POLL))
}

/// Back to standby once the play has ended. Some(delay) while it has not,
/// for another look then.
fn settle(now: Instant) -> Option<Duration> {
    let mut h = lock();
    let since = h.playing_since?;
    let dev = h.dev;
    if dev.is_null() {
        h.playing_since = None;
        return None;
    }
    let going = read_reg(dev, REG_GO).is_some_and(|go| go & 1 != 0);
    if going && now.duration_since(since) < PLAY_LIMIT {
        return Some(SETTLE_POLL);
    }
    if going {
        write_reg(dev, REG_GO, 0);
    }
    write_reg(dev, REG_MODE, MODE_STANDBY);
    h.playing_since = None;
    None
}

unsafe extern "C" fn start_job(arg: *mut c_void) {
    match start(arg as usize as u32, Instant::now()) {
        Ok(Some(delay)) => {
            crate::kworker::after(settle_job, std::ptr::null_mut(), delay);
        }
        Ok(None) => {}
        Err(ret) => {
            haptic_log!(LOG_WARN, b"play failed: 0x%x\0", ret);
        }
    }
}

unsafe extern "C" fn settle_job(_arg: *mut c_void) {
    if let Some(delay) = settle(Instant::now()) {
        crate::kworker::after(settle_job, std::ptr::null_mut(), delay);
    }
}

// ── HAL vtable ───────────────────────────────────────────────────────────────

unsafe extern "C" fn drv2605_init(_config: *const c_void) -> i32 {
    let mut h = lock();
    if !h.dev.is_null() {
        return ESP_OK;
    }
    let config = h.config;
    power(&config, true);
    std::thread::sleep(POWER_UP);

    let bus = crate::hal_registry::hal_bus_get_i2c(config.i2c_bus);
    if bus.is_null() {
        power(&config, false);
        return ESP_ERR_INVALID_STATE;
    }
    let Some(dev) = open(bus, config.i2c_addr) else {
        power(&config, false);
        return ESP_FAIL;
    };
    let status = read_reg(dev, REG_STATUS);
    if !status.is_some_and(has_library) {
        match status {
            None => {
                haptic_log!(
                    LOG_WARN,
                    b"nothing answers at 0x%02x\0",
                    config.i2c_addr as u32
                );
            }
            Some(s) => {
                haptic_log!(LOG_WARN, b"not a DRV2605: id %u\0", (s >> 5) as u32);
            }
        }
        unsafe { esp_ffi::i2c_master_bus_rm_device(dev) };
        power(&config, false);
        return ESP_ERR_NOT_FOUND;
    }
    if !configure(dev) {
        unsafe { esp_ffi::i2c_master_bus_rm_device(dev) };
        power(&config, false);
        return ESP_FAIL;
    }
    h.dev = dev;
    haptic_log!(
        LOG_INFO,
        b"ready (id %u), in standby\0",
        (status.unwrap_or(0) >> 5) as u32
    );
    ESP_OK
}

unsafe extern "C" fn drv2605_deinit() {
    let mut h = lock();
    if h.dev.is_null() {
        return;
    }
    write_reg(h.dev, REG_GO, 0);
    write_reg(h.dev, REG_MODE, MODE_STANDBY);
    unsafe { esp_ffi::i2c_master_bus_rm_device(h.dev) };
    h.dev = std::ptr::null_mut();
    h.playing_since = None;
    let config = h.config;
    drop(h);
    power(&config, false);
}

unsafe extern "C" fn drv2605_play(effect: u32) -> i32 {
    if sequence(effect).is_none() {
        return ESP_ERR_INVALID_ARG;
    }
    if lock().dev.is_null() {
        return ESP_ERR_INVALID_STATE;
    }
    crate::kworker::post(start_job, effect as usize as *mut c_void);
    ESP_OK
}

static HAPTIC_DRIVER: HalHapticDriver = HalHapticDriver {
    init: Some(drv2605_init),
    deinit: Some(drv2605_deinit),
    play: Some(drv2605_play),
    name: b"DRV2605\0".as_ptr() as *const c_char,
};

fn parse_config(json: &str) -> Config {
    let int = |key: &str| crate::manifest::json_get_int(json, key);
    let enable = match (int("en_expander"), int("en_expander_pin")) {
        (Some(expander), Some(pin)) if expander >= 0 && pin >= 0 => {
            Some((expander as i32, pin as i32))
        }
        _ => None,
    };
    Config {
        i2c_bus: int("i2c_bus").unwrap_or(0) as i32,
        i2c_addr: parse_address(json, "i2c_addr").unwrap_or(DEFAULT_ADDRESS),
        enable,
    }
}

fn parse_address(json: &str, key: &str) -> Option<u8> {
    let value = match crate::manifest::json_get_string(json, key) {
        Some(text) => {
            let text = text.trim().to_string();
            match text.strip_prefix("0x").or_else(|| text.strip_prefix("0X")) {
                Some(hex) => i64::from_str_radix(hex, 16).ok()?,
                None => text.parse().ok()?,
            }
        }
        None => crate::manifest::json_get_int(json, key)?,
    };
    u8::try_from(value).ok()
}

/// Register the driver for a board profile entry, taking its configuration as
/// the JSON object the entry declared.
///
/// # Safety
/// Called from C. `config_json` must be null or a valid null-terminated string.
#[no_mangle]
pub unsafe extern "C" fn drv_haptic_drv2605_register(config_json: *const c_char) -> i32 {
    let config = if config_json.is_null() {
        DEFAULT_CONFIG
    } else {
        match unsafe { std::ffi::CStr::from_ptr(config_json) }.to_str() {
            Ok(json) => parse_config(json),
            Err(_) => return ESP_ERR_INVALID_ARG,
        }
    };
    lock().config = config;
    unsafe { crate::hal_registry::hal_haptic_register(&HAPTIC_DRIVER, std::ptr::null()) }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A board with a DRV2605 on bus 0 whose plays last `go_reads` looks, and
    /// a driver that has not been brought up.
    fn a_board(go_reads: u32) {
        unsafe { drv2605_deinit() };
        esp_ffi::attach(go_reads);
        if crate::hal_registry::hal_bus_get_i2c(0).is_null() {
            unsafe { crate::hal_registry::hal_bus_register_i2c(0, 1usize as *mut c_void) };
        }
        lock().config = DEFAULT_CONFIG;
    }

    fn reg(register: u8) -> u8 {
        esp_ffi::CHIP.lock().unwrap().regs[register as usize]
    }

    fn clear_writes() {
        esp_ffi::CHIP.lock().unwrap().writes.clear();
    }

    #[test]
    fn bring_up_selects_library_a_open_loop_erm_and_leaves_it_in_standby() {
        a_board(0);
        assert_eq!(unsafe { drv2605_init(std::ptr::null()) }, ESP_OK);
        assert_eq!(reg(REG_LIBRARY), LIBRARY_A);
        assert_eq!(reg(REG_FEEDBACK) & FEEDBACK_N_ERM_LRA, 0);
        assert_ne!(reg(REG_CONTROL3) & CONTROL3_ERM_OPEN_LOOP, 0);
        assert_eq!(reg(REG_MODE), MODE_STANDBY);
        unsafe { drv2605_deinit() };
    }

    #[test]
    fn a_part_that_does_not_answer_is_not_found() {
        a_board(0);
        esp_ffi::detach();
        assert_eq!(unsafe { drv2605_init(std::ptr::null()) }, ESP_ERR_NOT_FOUND);
        assert_eq!(
            unsafe { drv2605_play(HAL_HAPTIC_MESSAGE) },
            ESP_ERR_INVALID_STATE
        );
    }

    #[test]
    fn a_drv2604_has_no_library_to_play_from() {
        a_board(0);
        esp_ffi::CHIP.lock().unwrap().regs[0] = 4 << 5;
        assert_eq!(unsafe { drv2605_init(std::ptr::null()) }, ESP_ERR_NOT_FOUND);
        assert!(has_library(7 << 5));
    }

    #[test]
    fn a_message_plays_two_buzzes_then_the_part_goes_back_to_standby() {
        a_board(2);
        assert_eq!(unsafe { drv2605_init(std::ptr::null()) }, ESP_OK);
        clear_writes();
        let t0 = Instant::now();
        assert_eq!(start(HAL_HAPTIC_MESSAGE, t0), Ok(Some(SETTLE_POLL)));
        assert_eq!(reg(REG_MODE), MODE_INTERNAL_TRIGGER);
        assert_eq!(
            [reg(0x04), reg(0x05), reg(0x06), reg(0x07)],
            [STRONG_BUZZ, WAIT_10MS | 10, STRONG_BUZZ, 0]
        );
        assert_eq!(
            esp_ffi::CHIP.lock().unwrap().writes.last(),
            Some(&(REG_GO, 1))
        );
        // Still playing at the first two looks; never put to standby meanwhile,
        // which would cut it short.
        assert_eq!(settle(t0 + SETTLE_POLL), Some(SETTLE_POLL));
        assert_eq!(settle(t0 + SETTLE_POLL * 2), Some(SETTLE_POLL));
        assert_eq!(reg(REG_MODE), MODE_INTERNAL_TRIGGER);
        assert_eq!(settle(t0 + SETTLE_POLL * 3), None);
        assert_eq!(reg(REG_MODE), MODE_STANDBY);
        unsafe { drv2605_deinit() };
    }

    #[test]
    fn a_play_during_a_play_needs_no_second_look() {
        a_board(5);
        assert_eq!(unsafe { drv2605_init(std::ptr::null()) }, ESP_OK);
        let t0 = Instant::now();
        assert_eq!(start(HAL_HAPTIC_MESSAGE, t0), Ok(Some(SETTLE_POLL)));
        assert_eq!(start(HAL_HAPTIC_CLICK, t0), Ok(None));
        unsafe { drv2605_deinit() };
    }

    #[test]
    fn a_play_that_never_ends_is_stopped() {
        a_board(u32::MAX);
        assert_eq!(unsafe { drv2605_init(std::ptr::null()) }, ESP_OK);
        let t0 = Instant::now();
        start(HAL_HAPTIC_CLICK, t0).unwrap();
        assert_eq!(settle(t0 + PLAY_LIMIT), None);
        assert_eq!(reg(REG_GO), 0);
        assert_eq!(reg(REG_MODE), MODE_STANDBY);
        unsafe { drv2605_deinit() };
    }

    #[test]
    fn an_unknown_effect_is_refused() {
        a_board(0);
        assert_eq!(unsafe { drv2605_init(std::ptr::null()) }, ESP_OK);
        assert_eq!(unsafe { drv2605_play(99) }, ESP_ERR_INVALID_ARG);
        assert_eq!(start(99, Instant::now()), Err(ESP_ERR_INVALID_ARG));
        unsafe { drv2605_deinit() };
    }

    #[test]
    fn the_profile_entry_names_the_address_and_the_power_pin() {
        let c = parse_config(
            r#"{"i2c_bus": 0, "i2c_addr": "0x5A", "en_expander": 0, "en_expander_pin": 5}"#,
        );
        assert_eq!(
            c,
            Config {
                i2c_bus: 0,
                i2c_addr: 0x5A,
                enable: Some((0, 5))
            }
        );
        assert_eq!(parse_config("{}"), DEFAULT_CONFIG);
        assert_eq!(
            parse_config(r#"{"en_expander": -1, "en_expander_pin": 5}"#).enable,
            None
        );
    }
}
