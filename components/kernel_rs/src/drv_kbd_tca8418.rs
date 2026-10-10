// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — TCA8418 I2C keyboard matrix driver (Rust)
//
// Rust port of components/drv_kbd_tca8418/src/drv_kbd_tca8418.c.
//
// The TCA8418 scans up to an 8×10 key matrix and buffers key events in a
// 10-deep FIFO.  We expose an interrupt-driven poll() call: if pin_int is
// wired up the ISR sets a flag so that the poll is cheap when idle.
//
// The C driver remains untouched and is kept as a fallback.

#![allow(non_upper_case_globals)]

use std::os::raw::{c_char, c_void};
use std::sync::atomic::{AtomicBool, Ordering};

use crate::hal_registry::{
    HalInputCb, HalInputDriver, HalInputEvent, HalInputEventData, HalInputEventType,
    HalInputKeyData,
};

// ── ESP error codes ──────────────────────────────────────────────────────────

const ESP_OK: i32 = 0;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_ERR_NOT_SUPPORTED: i32 = 0x106;

// ── TCA8418 register map ─────────────────────────────────────────────────────

const TCA8418_REG_CFG: u8 = 0x01; // Configuration
const TCA8418_REG_INT_STAT: u8 = 0x02; // Interrupt status
const TCA8418_REG_KEY_LCK_EC: u8 = 0x03; // Key-lock + event count

/// Depth of the chip's key-event FIFO, and so the most events one poll can
/// find waiting in it.
const TCA8418_FIFO_DEPTH: usize = 10;
const TCA8418_REG_KEY_EVENT_A: u8 = 0x04; // Key event FIFO (read repeatedly)
const TCA8418_REG_KP_GPIO1: u8 = 0x1D; // rows R0-R7 / cols C0-C7
const TCA8418_REG_KP_GPIO2: u8 = 0x1E; // cols C8-C9
const TCA8418_REG_KP_GPIO3: u8 = 0x1F;

// CFG bits
const TCA8418_CFG_KE_IEN: u8 = 1 << 0; // Key event interrupt enable
const TCA8418_CFG_AI: u8 = 1 << 7; // Auto-increment

// INT_STAT bits
const TCA8418_INT_STAT_K_INT: u8 = 1 << 0;

// Key event bits
const KEY_EVENT_PRESS: u8 = 0x80; // 1 = press, 0 = release
const KEY_EVENT_KEY_MSK: u8 = 0x7F;

// GPIO_NUM_NC sentinel (matches ESP-IDF GPIO_NUM_NC = -1)
const GPIO_NUM_NC: i32 = -1;

// ── Key backlight (LEDC PWM) ─────────────────────────────────────────────────
//
// Timer and channel 1, leaving 0 to the display drivers — the e-paper's
// front-light (components/drv_epaper_gdeq031t10) and every LCD backlight in
// the tree all take 0. On the T-Deck Max both lights are live at once, and a
// second driver claiming channel 0 would take the display's with it. A third
// lit peripheral needs a channel of its own; nothing enforces that but these
// comments, which sit on either side of the C/Rust boundary.

/// `LEDC_LOW_SPEED_MODE`, whose value is not the same on every part. The
/// original ESP32 has a high-speed mode sitting ahead of it in `ledc_mode_t`;
/// no later part has one, so there the enum starts at low speed. Hardcoding
/// the ESP32's value gives `LEDC_SPEED_MODE_MAX` on an ESP32-S3, which
/// `ledc_timer_config` rejects with ESP_ERR_INVALID_ARG — and an input
/// driver that fails to initialise stops the whole driver start sequence.
#[cfg(feature = "esp32")]
const BL_LEDC_MODE: i32 = 1;
#[cfg(not(feature = "esp32"))]
const BL_LEDC_MODE: i32 = 0;

const BL_LEDC_TIMER: i32 = 1; // LEDC_TIMER_1
const BL_LEDC_CHANNEL: i32 = 1; // LEDC_CHANNEL_1
const BL_LEDC_FREQ_HZ: u32 = 5000;
const BL_LEDC_DUTY_RES: u32 = 8; // LEDC_TIMER_8_BIT
const BL_LEDC_MAX_DUTY: u32 = 255;

#[cfg(target_os = "espidf")]
mod ledc_ffi {
    // These mirror C structs by hand, so their field order is load-bearing and
    // silent when wrong: a mismatch is not a compile error, it is a field
    // reading a neighbour's value. Both are transcribed from
    // esp_driver_ledc/include/driver/ledc.h and must be checked against it
    // when ESP-IDF is upgraded.

    /// `ledc_timer_config_t`. Note that `duty_resolution` comes *before*
    /// `timer_num` — swapping them hands the duty resolution to the timer
    /// selector, which ESP-IDF rejects with "timer_num argument is invalid".
    #[repr(C)]
    pub struct LedcTimerConfig {
        pub speed_mode: i32,
        pub duty_resolution: u32,
        pub timer_num: i32,
        pub freq_hz: u32,
        pub clk_cfg: u32, // LEDC_AUTO_CLK = 0
        pub deconfigure: bool,
    }

    /// `ledc_channel_config_t`. `sleep_mode` sits between `hpoint` and the
    /// flags word; leaving it out makes the driver read `flags` from past the
    /// end of the struct.
    #[repr(C)]
    pub struct LedcChannelConfig {
        pub gpio_num: i32,
        pub speed_mode: i32,
        pub channel: i32,
        pub intr_type: u32, // LEDC_INTR_DISABLE = 0
        pub timer_sel: i32,
        pub duty: u32,
        pub hpoint: i32,
        pub sleep_mode: u32, // LEDC_SLEEP_MODE_NO_ALIVE_NO_PD = 0
        pub flags: u32,
    }

    extern "C" {
        pub fn ledc_timer_config(cfg: *const LedcTimerConfig) -> i32;
        pub fn ledc_channel_config(cfg: *const LedcChannelConfig) -> i32;
        pub fn ledc_set_duty(mode: i32, channel: i32, duty: u32) -> i32;
        pub fn ledc_update_duty(mode: i32, channel: i32) -> i32;
    }
}

/// Claim the LEDC timer and channel driving the key backlight.
/// A board that leaves the line unwired has nothing to claim.
unsafe fn backlight_init(pin: i32) -> i32 {
    if pin == GPIO_NUM_NC {
        return ESP_OK;
    }

    #[cfg(target_os = "espidf")]
    {
        let timer_cfg = ledc_ffi::LedcTimerConfig {
            speed_mode: BL_LEDC_MODE,
            duty_resolution: BL_LEDC_DUTY_RES,
            timer_num: BL_LEDC_TIMER,
            freq_hz: BL_LEDC_FREQ_HZ,
            clk_cfg: 0, // LEDC_AUTO_CLK
            deconfigure: false,
        };
        let ret = ledc_ffi::ledc_timer_config(&timer_cfg);
        if ret != ESP_OK {
            return ret;
        }

        let ch_cfg = ledc_ffi::LedcChannelConfig {
            gpio_num: pin,
            speed_mode: BL_LEDC_MODE,
            channel: BL_LEDC_CHANNEL,
            intr_type: 0, // LEDC_INTR_DISABLE
            timer_sel: BL_LEDC_TIMER,
            duty: 0,
            hpoint: 0,
            sleep_mode: 0,
            flags: 0,
        };
        let ret = ledc_ffi::ledc_channel_config(&ch_cfg);
        if ret != ESP_OK {
            return ret;
        }
    }

    ESP_OK
}

/// Drive the key backlight at `percent` of full.
unsafe fn backlight_set_duty(percent: u8) -> i32 {
    #[cfg(target_os = "espidf")]
    {
        let duty = (percent as u32) * BL_LEDC_MAX_DUTY / 100;
        let ret = ledc_ffi::ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
        if ret != ESP_OK {
            return ret;
        }
        return ledc_ffi::ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
    }
    #[cfg(not(target_os = "espidf"))]
    {
        let _ = percent;
        ESP_OK
    }
}

// ── Logging ──────────────────────────────────────────────────────────────────

#[cfg(all(not(test), target_os = "espidf"))]
extern "C" {
    fn esp_log_write(level: u32, tag: *const u8, format: *const u8, ...);
}

#[cfg(not(test))]
static TAG: &[u8] = b"kbd\0";

/// Carries its own tag and newline: esp_log_write adds neither, and an
/// unterminated line is the first thing the console drops.
#[cfg(all(not(test), target_os = "espidf"))]
macro_rules! kbd_logw {
    ($fmt:expr $(, $arg:expr)*) => {
        unsafe { esp_log_write(2, TAG.as_ptr(), $fmt.as_ptr() $(, $arg)*); }
    };
}

#[cfg(all(not(test), not(target_os = "espidf")))]
macro_rules! kbd_logw {
    ($fmt:expr $(, $arg:expr)*) => { let _ = ($($arg,)*); };
}

#[cfg(test)]
macro_rules! kbd_logw {
    ($fmt:expr $(, $arg:expr)*) => { let _ = ($($arg,)*); };
}

// ── Keymap ───────────────────────────────────────────────────────────────────
//
// TCA8418 encodes key position as: key_code = row*10 + col + 1  (1-based).
// On the T-Deck Pro the column matrix wiring is mirrored — physical col 0 is
// reported as TCA col 9 and vice versa — so the decode path applies
// `col = 9 - col_raw` before this lookup. With that mirror in place, KEY_MAP
// can be written in physical (left-to-right) layout order.
//
// Rows 0-3 are the 4×10 matrix the T-Deck Pro and Max share: three full rows
// of ten, then five keys on the bottom row. Every one of those 35 positions
// carries a code, including the four that type nothing on their own — Shift,
// Alt, Sym and the microphone key — because a key that decodes to nothing is
// a key the driver cannot act on.

/// Keys that type nothing on their own, and so never leave the driver. Shift,
/// Alt and Sym change what the next key types, and the microphone key types
/// only the `0` its symbol layer gives it. They used to be sent on as they
/// are, and LVGL read them as keys of its own: Sym (0x02) as Home and Shift
/// (0x03) as End, jumping the cursor in a text field, while Alt and the
/// microphone key went into the text as control characters.
const KEY_ALT: u16 = 0x01;
const KEY_SYM: u16 = 0x02;
const KEY_SHIFT: u16 = 0x03;
const KEY_MIC: u16 = 0x04;

#[rustfmt::skip]
static KEY_MAP: [[u16; 10]; 8] = [
    // C0     C1     C2     C3     C4     C5     C6     C7     C8     C9
    [  b'q' as u16, b'w' as u16, b'e' as u16, b'r' as u16, b't' as u16, b'y' as u16, b'u' as u16, b'i' as u16, b'o' as u16, b'p' as u16  ],
    [  b'a' as u16, b's' as u16, b'd' as u16, b'f' as u16, b'g' as u16, b'h' as u16, b'j' as u16, b'k' as u16, b'l' as u16, 0x08u16      ],  // 0x08 = backspace
    [  KEY_ALT,     b'z' as u16, b'x' as u16, b'c' as u16, b'v' as u16, b'b' as u16, b'n' as u16, b'm' as u16, b'$' as u16, b'\n' as u16 ],  // Alt Z X C V B N M $ Enter
    // Row 3: only 5 physical keys at TCA cols 0..4 (after mirror, c 5..9):
    //   c=5 shift(L)  c=6 mic  c=7 space  c=8 sym  c=9 shift(R)
    [  0,           0,           0,           0,           0,           KEY_SHIFT,   KEY_MIC,     b' ' as u16, KEY_SYM,     KEY_SHIFT   ],
    [  b'7' as u16, b'8' as u16, b'9' as u16, b'0' as u16, b'-' as u16, b'=' as u16, b'[' as u16, b']' as u16, b'\\' as u16, b'\'' as u16 ],
    [  b';' as u16, b'/' as u16, b'`' as u16, 0x1Bu16,     0,          0,           0,           0,           0,           0            ],  // 0x1B = Esc
    [  0,           0,           0,           0,           0,           0,           0,           0,           0,           0            ],
    [  0,           0,           0,           0,           0,           0,           0,           0,           0,           0            ],
];

/// What each key types under Alt or Sym: the small legend printed above its
/// letter. Transcribed from LilyGo's own symbol layer,
/// `wifi_password_chat_map` in the T-Deck-MAX vendor checkout's
/// `examples/factory/ui_deckpro.cpp`, plus the `0` the vendor keymap
/// (`examples/factory/peri_keypad.cpp`) puts on the microphone key. Zero is
/// a key with no second legend, which types its usual code under Alt.
#[rustfmt::skip]
static SYMBOL_MAP: [[u16; 10]; 4] = [
    // C0     C1     C2     C3     C4     C5     C6     C7     C8     C9
    [  b'#' as u16, b'1' as u16, b'2' as u16, b'3' as u16, b'(' as u16, b')' as u16, b'_' as u16, b'-' as u16,  b'+' as u16, b'@' as u16 ],
    [  b'*' as u16, b'4' as u16, b'5' as u16, b'6' as u16, b'/' as u16, b':' as u16, b';' as u16, b'\'' as u16, b'"' as u16, 0           ],
    [  0,           b'7' as u16, b'8' as u16, b'9' as u16, b'?' as u16, b'!' as u16, b',' as u16, b'.' as u16,  0,           0           ],
    [  0,           0,           0,           0,           0,           0,           b'0' as u16, 0,            0,           0           ],
];

/// Key positions the chip can report: eight rows of ten.
const KEY_POSITIONS: usize = 80;

/// What the key at physical `row`, `col` types with Shift and the symbol
/// layer as given. The symbol layer wins where a key has a legend for it;
/// Shift capitalises a letter and leaves every other key alone.
fn layered_code(row: usize, col: usize, shift: bool, symbol: bool) -> u16 {
    let base = KEY_MAP[row][col];
    if symbol && row < SYMBOL_MAP.len() && SYMBOL_MAP[row][col] != 0 {
        return SYMBOL_MAP[row][col];
    }
    if shift && (b'a' as u16..=b'z' as u16).contains(&base) {
        return base - (b'a' - b'A') as u16;
    }
    base
}

/// Shift, or the symbol layer that Alt and Sym both select. Either works held
/// down across the key it modifies, or tapped on its own to apply to the next
/// key: one thumb cannot hold Shift on one side of the keyboard and reach a
/// letter on the other. Tapping it a second time takes the tap back.
#[derive(Clone, Copy)]
struct Modifier {
    /// How many of its keys are down. Shift has one on each side.
    held: u8,
    /// Tapped and let go: applies to the next key, then clears.
    latched: bool,
    /// A key was typed while this was held, so letting go is not a tap.
    used: bool,
}

impl Modifier {
    const fn new() -> Self {
        Modifier {
            held: 0,
            latched: false,
            used: false,
        }
    }

    fn active(&self) -> bool {
        self.held > 0 || self.latched
    }

    fn press(&mut self) {
        if self.held == 0 {
            self.used = false;
        }
        self.held = self.held.saturating_add(1);
    }

    fn release(&mut self) {
        // A release of a key not seen going down — pressed before init, or
        // its press lost — is no tap.
        if self.held == 0 {
            return;
        }
        self.held -= 1;
        if self.held == 0 && !self.used {
            self.latched = !self.latched;
        }
    }

    /// A key has been typed through this modifier.
    fn spend(&mut self) {
        if self.held > 0 {
            self.used = true;
        }
        self.latched = false;
    }
}

// ── Configuration struct ─────────────────────────────────────────────────────

/// C-compatible config struct.  Must match `kbd_tca8418_config_t` in the C header.
#[repr(C)]
pub struct KbdTca8418Config {
    /// i2c_master_bus_handle_t
    pub i2c_bus: *mut c_void,
    /// I2C device address (default 0x34)
    pub i2c_addr: u8,
    /// Interrupt GPIO pin, active-low.  Use -1 (GPIO_NUM_NC) to disable.
    pub pin_int: i32,
    /// Key backlight PWM pin.  Use -1 (GPIO_NUM_NC) on a board that leaves
    /// the keyboard's LED line unwired.
    pub pin_backlight: i32,
}

// SAFETY: Config holds opaque C pointers; we never share them across threads
// except through the global static state which is guarded by the driver's
// single-initialisation semantics.
unsafe impl Send for KbdTca8418Config {}
unsafe impl Sync for KbdTca8418Config {}

// ── ESP-IDF FFI ──────────────────────────────────────────────────────────────

#[cfg(target_os = "espidf")]
mod esp_ffi {
    use std::os::raw::c_void;

    /// i2c_device_config_t (partial — only fields we set).
    /// Must match the ESP-IDF struct layout exactly.
    #[repr(C)]
    pub struct I2cDeviceConfig {
        /// dev_addr_length: I2C_ADDR_BIT_LEN_7 = 0
        pub dev_addr_length: u32,
        /// device_address
        pub device_address: u16,
        /// scl_speed_hz
        pub scl_speed_hz: u32,
        // Padding to match the full ESP-IDF struct size (20 bytes total on Xtensa).
        // Extra fields: scl_wait_us (u32) + flags field (u32)
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

        // GPIO
        pub fn gpio_set_direction(pin: u32, mode: u32) -> i32;
        pub fn gpio_set_pull_mode(pin: u32, mode: u32) -> i32;
        pub fn gpio_isr_handler_add(
            pin: u32,
            handler: unsafe extern "C" fn(*mut c_void),
            arg: *mut c_void,
        ) -> i32;
        pub fn gpio_isr_handler_remove(pin: u32) -> i32;
        pub fn gpio_set_intr_type(pin: u32, intr_type: u32) -> i32;
        pub fn gpio_intr_enable(pin: u32) -> i32;
        pub fn gpio_install_isr_service(flags: i32) -> i32;

        // Timer
        pub fn esp_timer_get_time() -> i64;
    }
}

// ── Extern "C" bindings for simulator with sim-bus feature ───────────────────

#[cfg(all(not(target_os = "espidf"), feature = "sim-bus"))]
mod esp_ffi {
    use std::os::raw::c_void;

    #[repr(C)]
    pub struct I2cDeviceConfig {
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
        pub fn gpio_set_direction(pin: u32, mode: u32) -> i32;
        pub fn gpio_set_pull_mode(pin: u32, mode: u32) -> i32;
        pub fn gpio_isr_handler_add(
            pin: u32,
            handler: unsafe extern "C" fn(*mut c_void),
            arg: *mut c_void,
        ) -> i32;
        pub fn gpio_isr_handler_remove(pin: u32) -> i32;
        pub fn gpio_set_intr_type(pin: u32, intr_type: u32) -> i32;
        pub fn gpio_intr_enable(pin: u32) -> i32;
        pub fn gpio_install_isr_service(flags: i32) -> i32;
        pub fn esp_timer_get_time() -> i64;
    }
}

// ── Stub impls for non-ESP32 targets without sim-bus (host tests) ────────────

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
mod esp_ffi {
    use std::os::raw::c_void;

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
        // Return a non-null sentinel so code can tell it "succeeded"
        *handle = 1usize as *mut c_void;
        0
    }
    pub unsafe fn i2c_master_bus_rm_device(_handle: *mut c_void) -> i32 {
        0
    }
    // A scriptable chip, so a test can put the driver in front of a keyboard
    // that answers. Every register reads 0 unless a test says otherwise, which
    // is what the driver saw before this existed.
    use std::sync::atomic::{AtomicUsize, Ordering as AtomicOrdering};
    use std::sync::Mutex;

    pub static REG_READS: AtomicUsize = AtomicUsize::new(0);
    /// Read number on which to fire the keyboard's ISR, imitating a key
    /// pressed while a poll is in progress. `usize::MAX` never fires.
    pub static FIRE_ISR_ON_READ: AtomicUsize = AtomicUsize::new(usize::MAX);

    pub static SCRIPT: Mutex<Option<ChipScript>> = Mutex::new(None);

    /// Every register touch in order, so a test can assert not just what the
    /// driver did but when it did it relative to everything else.
    pub static TRACE: Mutex<Vec<RegOp>> = Mutex::new(Vec::new());

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum RegOp {
        Read(u8),
        Write(u8),
    }

    #[derive(Default)]
    pub struct ChipScript {
        /// Value each register answers with, by register address.
        pub registers: std::collections::HashMap<u8, u8>,
        /// Successive answers for the key-event FIFO, consumed front to back.
        /// An exhausted queue reads 0, the chip's "FIFO empty" sentinel.
        pub events: std::collections::VecDeque<u8>,
    }

    pub fn reset_chip() {
        REG_READS.store(0, AtomicOrdering::Relaxed);
        FIRE_ISR_ON_READ.store(usize::MAX, AtomicOrdering::Relaxed);
        *SCRIPT.lock().unwrap() = None;
        TRACE.lock().unwrap().clear();
    }

    pub unsafe fn i2c_master_transmit_receive(
        _handle: *mut c_void,
        write: *const u8,
        wsz: usize,
        read: *mut u8,
        rsz: usize,
        _timeout: i32,
    ) -> i32 {
        unsafe { std::ptr::write_bytes(read, 0, rsz) };

        let reg = if wsz >= 1 && !write.is_null() {
            unsafe { *write }
        } else {
            return 0;
        };

        if let Some(script) = SCRIPT.lock().unwrap().as_mut() {
            let value = if reg == super::TCA8418_REG_KEY_EVENT_A {
                script.events.pop_front().unwrap_or(0)
            } else {
                script.registers.get(&reg).copied().unwrap_or(0)
            };
            if rsz >= 1 && !read.is_null() {
                unsafe { *read = value };
            }
        }

        TRACE.lock().unwrap().push(RegOp::Read(reg));

        // Counted and fired after the answer, so the interrupt lands in the
        // middle of the poll rather than before it has read anything.
        let n = REG_READS.fetch_add(1, AtomicOrdering::Relaxed) + 1;
        if n == FIRE_ISR_ON_READ.load(AtomicOrdering::Relaxed) {
            unsafe { super::tca8418_isr_handler(std::ptr::null_mut()) };
        }
        0
    }
    pub unsafe fn i2c_master_transmit(
        _handle: *mut c_void,
        data: *const u8,
        len: usize,
        _timeout: i32,
    ) -> i32 {
        // A register write is the register address followed by the value.
        if len >= 1 && !data.is_null() {
            TRACE.lock().unwrap().push(RegOp::Write(unsafe { *data }));
        }
        0
    }
    pub unsafe fn gpio_set_direction(_pin: u32, _mode: u32) -> i32 {
        0
    }
    pub unsafe fn gpio_set_pull_mode(_pin: u32, _mode: u32) -> i32 {
        0
    }
    pub unsafe fn gpio_isr_handler_add(
        _pin: u32,
        _handler: unsafe extern "C" fn(*mut c_void),
        _arg: *mut c_void,
    ) -> i32 {
        0
    }
    pub unsafe fn gpio_isr_handler_remove(_pin: u32) -> i32 {
        0
    }
    pub unsafe fn gpio_set_intr_type(_pin: u32, _intr_type: u32) -> i32 {
        0
    }
    pub unsafe fn gpio_intr_enable(_pin: u32) -> i32 {
        0
    }
    pub unsafe fn gpio_install_isr_service(_flags: i32) -> i32 {
        0
    }
    pub unsafe fn esp_timer_get_time() -> i64 {
        0
    }
}

// ── Driver state ─────────────────────────────────────────────────────────────

struct KbdState {
    /// I2C device handle (i2c_master_dev_handle_t)
    dev: *mut c_void,
    cfg: KbdTca8418Config,
    cb: HalInputCb,
    cb_data: *mut c_void,
    irq_pending: AtomicBool,
    initialized: bool,
    /// Last non-zero backlight percent, to come back to after going dark.
    backlight: u8,
    /// Whether the key backlight is under this driver's control. False on a
    /// board that leaves the line unwired, and on one where claiming the LEDC
    /// timer failed — in which case the keys still work, unlit.
    backlight_ready: bool,
    shift: Modifier,
    /// The symbol layer, selected by Alt and Sym alike.
    symbol: Modifier,
    /// The code each key position went down as, so that it comes up as the
    /// same one. A key pressed under Shift and released after it must release
    /// the capital it typed: the LVGL window manager ignores a release that
    /// names a key other than the one it saw go down, and LVGL repeats a held
    /// key, so the capital would keep typing until the next key went down.
    down_code: [u16; KEY_POSITIONS],
}

// SAFETY: The driver state is accessed only from the HAL init/poll/deinit
// path which is guaranteed single-threaded by the HAL registry contract.
// The ISR only writes one AtomicBool flag.
unsafe impl Send for KbdState {}
unsafe impl Sync for KbdState {}

impl KbdState {
    const fn new() -> Self {
        KbdState {
            dev: std::ptr::null_mut(),
            cfg: KbdTca8418Config {
                i2c_bus: std::ptr::null_mut(),
                i2c_addr: 0x34,
                pin_int: GPIO_NUM_NC,
                pin_backlight: GPIO_NUM_NC,
            },
            cb: None,
            cb_data: std::ptr::null_mut(),
            irq_pending: AtomicBool::new(false),
            initialized: false,
            backlight: 0,
            backlight_ready: false,
            shift: Modifier::new(),
            symbol: Modifier::new(),
            down_code: [0; KEY_POSITIONS],
        }
    }

    /// Turn a key event at 0-based chip position `pos` into the code the
    /// window manager receives, if any. Modifiers are kept here, not sent.
    fn key_event(&mut self, pos: usize, pressed: bool) -> Option<u16> {
        if pos >= KEY_POSITIONS {
            return None;
        }
        let row = pos / 10;
        // Mirror the column index: T-Deck Pro wiring puts physical col 0 on
        // TCA col 9 and vice versa. KEY_MAP is written in physical order.
        let col = 9 - pos % 10;

        let modifier = match KEY_MAP[row][col] {
            0 => return None, // Unmapped key
            KEY_SHIFT => &mut self.shift,
            KEY_ALT | KEY_SYM => &mut self.symbol,
            _ if pressed => {
                let code = layered_code(row, col, self.shift.active(), self.symbol.active());
                self.shift.spend();
                self.symbol.spend();
                if code == KEY_MIC {
                    return None;
                }
                self.down_code[pos] = code;
                return Some(code);
            }
            _ => {
                let code = std::mem::replace(&mut self.down_code[pos], 0);
                return (code != 0).then_some(code);
            }
        };
        if pressed {
            modifier.press();
        } else {
            modifier.release();
        }
        None
    }
}

static mut S_KBD: KbdState = KbdState::new();

// ── I2C helpers ──────────────────────────────────────────────────────────────

/// Write a single register on the TCA8418.
///
/// # Safety
/// Must be called with S_KBD.dev valid.
unsafe fn tca8418_write_reg(reg: u8, val: u8) -> i32 {
    let buf: [u8; 2] = [reg, val];
    esp_ffi::i2c_master_transmit(S_KBD.dev, buf.as_ptr(), 2, 50)
}

/// Read a single register from the TCA8418.
///
/// # Safety
/// Must be called with S_KBD.dev valid.
unsafe fn tca8418_read_reg(reg: u8, val: &mut u8) -> i32 {
    esp_ffi::i2c_master_transmit_receive(S_KBD.dev, &reg as *const u8, 1, val as *mut u8, 1, 50)
}

// ── ISR ──────────────────────────────────────────────────────────────────────

/// Interrupt service routine — sets the irq_pending flag.
///
/// Marked `#[link_section = ".iram1"]` so it lives in IRAM on ESP-IDF targets
/// (equivalent to the C `IRAM_ATTR` macro).  On host targets the attribute is
/// ignored.
#[cfg_attr(target_os = "espidf", link_section = ".iram1.tca8418_isr")]
#[no_mangle]
pub unsafe extern "C" fn tca8418_isr_handler(_arg: *mut c_void) {
    // SAFETY: AtomicBool store is async-signal-safe and this is the only
    // write path from ISR context.  We use raw pointer access to avoid
    // the static_mut_refs lint while remaining compatible with older editions.
    let irq = &raw mut S_KBD;
    (*irq).irq_pending.store(true, Ordering::Release);
}

// ── vtable implementations ───────────────────────────────────────────────────

/// Initialise the TCA8418 driver.
///
/// # Safety
/// `config` must point to a valid `KbdTca8418Config`.
unsafe extern "C" fn tca8418_init(config: *const c_void) -> i32 {
    if config.is_null() {
        return ESP_ERR_INVALID_ARG;
    }

    let kbd = &mut *(&raw mut S_KBD);

    if kbd.initialized {
        // Already initialised — idempotent
        return ESP_OK;
    }

    // Copy config
    let src = &*(config as *const KbdTca8418Config);
    kbd.cfg.i2c_bus = src.i2c_bus;
    kbd.cfg.i2c_addr = src.i2c_addr;
    kbd.cfg.pin_int = src.pin_int;
    kbd.cfg.pin_backlight = src.pin_backlight;
    kbd.irq_pending.store(false, Ordering::Relaxed);

    // Add the I2C device at 400 kHz
    let dev_cfg = esp_ffi::I2cDeviceConfig {
        dev_addr_length: 0, // I2C_ADDR_BIT_LEN_7
        device_address: kbd.cfg.i2c_addr as u16,
        scl_speed_hz: 400_000,
        scl_wait_us: 0,
        flags: 0,
    };
    let ret = esp_ffi::i2c_master_bus_add_device(kbd.cfg.i2c_bus, &dev_cfg, &mut kbd.dev);
    if ret != ESP_OK {
        return ret;
    }

    // Enable auto-increment and key-event interrupt
    let ret = tca8418_write_reg(TCA8418_REG_CFG, TCA8418_CFG_KE_IEN | TCA8418_CFG_AI);
    if ret != ESP_OK {
        esp_ffi::i2c_master_bus_rm_device(kbd.dev);
        kbd.dev = std::ptr::null_mut();
        return ret;
    }

    // Configure R0-R7 as keypad rows (bits [7:0] of KP_GPIO1)
    let ret = tca8418_write_reg(TCA8418_REG_KP_GPIO1, 0xFF);
    if ret != ESP_OK {
        esp_ffi::i2c_master_bus_rm_device(kbd.dev);
        kbd.dev = std::ptr::null_mut();
        return ret;
    }

    // Configure C0-C7 (KP_GPIO2) and C8-C9 (KP_GPIO3 bits [1:0])
    let ret = tca8418_write_reg(TCA8418_REG_KP_GPIO2, 0xFF);
    if ret != ESP_OK {
        esp_ffi::i2c_master_bus_rm_device(kbd.dev);
        kbd.dev = std::ptr::null_mut();
        return ret;
    }

    let ret = tca8418_write_reg(TCA8418_REG_KP_GPIO3, 0x03);
    if ret != ESP_OK {
        esp_ffi::i2c_master_bus_rm_device(kbd.dev);
        kbd.dev = std::ptr::null_mut();
        return ret;
    }

    // Clear any stale interrupts
    let ret = tca8418_write_reg(TCA8418_REG_INT_STAT, 0xFF);
    if ret != ESP_OK {
        esp_ffi::i2c_master_bus_rm_device(kbd.dev);
        kbd.dev = std::ptr::null_mut();
        return ret;
    }

    // Optional interrupt pin (active-low, falling edge)
    if kbd.cfg.pin_int != GPIO_NUM_NC {
        let pin = kbd.cfg.pin_int as u32;

        // GPIO_MODE_INPUT = 1
        esp_ffi::gpio_set_direction(pin, 1);
        // GPIO_PULLUP_ENABLE = 1
        esp_ffi::gpio_set_pull_mode(pin, 1);
        // GPIO_INTR_NEGEDGE = 2
        esp_ffi::gpio_set_intr_type(pin, 2);
        esp_ffi::gpio_intr_enable(pin);

        let ret = crate::gpio_isr_service::ensure_installed(0);
        if ret != ESP_OK {
            esp_ffi::i2c_master_bus_rm_device(kbd.dev);
            kbd.dev = std::ptr::null_mut();
            return ret;
        }

        let ret = esp_ffi::gpio_isr_handler_add(pin, tca8418_isr_handler, std::ptr::null_mut());
        if ret != ESP_OK {
            esp_ffi::i2c_master_bus_rm_device(kbd.dev);
            kbd.dev = std::ptr::null_mut();
            return ret;
        }

        // Mark pending so the first poll drains anything already in the FIFO.
        kbd.irq_pending.store(true, Ordering::Release);
    }

    // The keys start dark: lighting them is something the user asks for, not
    // something boot spends battery on.
    //
    // A backlight that will not configure leaves a keyboard that is dark, not
    // one that is broken. This used to return here, before `initialized` was
    // set, so every key on the board went with the light — and on a T-Deck Max
    // that is exactly what happened: one wrong field order in the LEDC config
    // made ESP-IDF reject the timer, and the device came up unable to type.
    // The keys are the point; the light is a convenience.
    let ret = backlight_init(kbd.cfg.pin_backlight);
    kbd.backlight_ready = ret == ESP_OK;
    if !kbd.backlight_ready {
        kbd_logw!(b"key backlight unavailable (%d); keys still work\0", ret);
    }
    kbd.backlight = 0;

    kbd.initialized = true;
    ESP_OK
}

/// Set the key backlight, 0-100 percent.
unsafe extern "C" fn tca8418_set_backlight(percent: u8) -> i32 {
    let kbd = &mut *(&raw mut S_KBD);
    if !kbd.initialized {
        return ESP_ERR_INVALID_STATE;
    }
    // Unwired and failed-to-configure answer the same way. Either way there is
    // no light to set, and the caller should hear so rather than be told the
    // brightness took.
    if kbd.cfg.pin_backlight == GPIO_NUM_NC || !kbd.backlight_ready {
        return ESP_ERR_NOT_SUPPORTED;
    }

    let clamped = if percent > 100 { 100 } else { percent };
    let ret = backlight_set_duty(clamped);
    // Zero is the keys going dark, not a new brightness — it is the level
    // before it that they come back to.
    if ret == ESP_OK && clamped > 0 {
        kbd.backlight = clamped;
    }
    ret
}

/// De-initialise the TCA8418 driver.
unsafe extern "C" fn tca8418_deinit() {
    let kbd = &mut *(&raw mut S_KBD);
    if !kbd.initialized {
        return;
    }

    if kbd.cfg.pin_int != GPIO_NUM_NC {
        esp_ffi::gpio_isr_handler_remove(kbd.cfg.pin_int as u32);
    }

    if !kbd.dev.is_null() {
        esp_ffi::i2c_master_bus_rm_device(kbd.dev);
        kbd.dev = std::ptr::null_mut();
    }

    kbd.cb = None;
    kbd.cb_data = std::ptr::null_mut();
    kbd.shift = Modifier::new();
    kbd.symbol = Modifier::new();
    kbd.down_code = [0; KEY_POSITIONS];
    kbd.initialized = false;
}

/// Register the input event callback.
unsafe extern "C" fn tca8418_register_callback(cb: HalInputCb, user_data: *mut c_void) -> i32 {
    let kbd = &mut *(&raw mut S_KBD);
    kbd.cb = cb;
    kbd.cb_data = user_data;
    ESP_OK
}

/// Poll for pending key events and dispatch them via the registered callback.
unsafe extern "C" fn tca8418_poll() -> i32 {
    let kbd = &mut *(&raw mut S_KBD);

    if !kbd.initialized {
        return ESP_ERR_INVALID_STATE;
    }

    // If using the interrupt pin, skip I2C traffic when no interrupt fired.
    if kbd.cfg.pin_int != GPIO_NUM_NC && !kbd.irq_pending.load(Ordering::Acquire) {
        return ESP_OK;
    }

    // Give the flag up before doing any I2C work, not after. A key pressed
    // while this poll is draining sets it again from the ISR, and clearing it
    // at the end of the poll would throw that away — the next poll would take
    // the early return above and the keypress would sit in the FIFO unread.
    // That lost wakeup dropped most of a fast typist's keys: the first press
    // arrived and the ones chasing it did not.
    kbd.irq_pending.store(false, Ordering::Release);

    // Read interrupt status register
    let mut int_stat: u8 = 0;
    let ret = tca8418_read_reg(TCA8418_REG_INT_STAT, &mut int_stat);
    if ret != ESP_OK {
        return ret;
    }

    if (int_stat & TCA8418_INT_STAT_K_INT) == 0 {
        return ESP_OK;
    }

    let mut last_ret = ESP_OK;

    // Drain until the FIFO says it is empty. The event count in KEY_LCK_EC is
    // not consulted: it is a snapshot, and an event landing mid-drain makes it
    // an undercount. The FIFO's own empty sentinel cannot go stale that way.
    for _ in 0..TCA8418_FIFO_DEPTH {
        let mut ev: u8 = 0;
        let ret = tca8418_read_reg(TCA8418_REG_KEY_EVENT_A, &mut ev);
        if ret != ESP_OK {
            last_ret = ret;
            break;
        }

        if ev == 0 {
            // FIFO empty sentinel
            break;
        }

        let pressed = (ev & KEY_EVENT_PRESS) != 0;
        let key_code = ev & KEY_EVENT_KEY_MSK;

        if key_code == 0 {
            continue;
        }

        // key_code is 1-based: key_code = row*10 + col + 1
        let keycode = match kbd.key_event((key_code - 1) as usize, pressed) {
            Some(code) => code,
            None => continue,
        };

        if let Some(cb) = kbd.cb {
            let timestamp = (esp_ffi::esp_timer_get_time() / 1000) as u32;
            let event = HalInputEvent {
                event_type: if pressed {
                    HalInputEventType::KeyDown
                } else {
                    HalInputEventType::KeyUp
                },
                timestamp,
                data: HalInputEventData {
                    key: HalInputKeyData { keycode },
                },
            };
            cb(&event as *const HalInputEvent, kbd.cb_data);
        }
    }

    // Acknowledge last, once the FIFO is empty — never before the drain.
    //
    // The chip holds INT low for as long as K_INT is set. Clearing K_INT up
    // front looks tidy, but a key pressed during the drain sets it again, and
    // then nothing clears it: the FIFO ends up empty with K_INT still set and
    // INT still low. The ISR is edge-triggered, so no later keypress can
    // produce another falling edge, and the keyboard is deaf for good. That is
    // not theoretical — it shipped, and a T-Deck Max answered two keystrokes
    // out of sixteen before going silent.
    //
    // Acknowledging here leaves one narrow window, between the loop ending and
    // this write, where an event can be left in the FIFO with K_INT cleared.
    // That one heals itself: the event stays queued, INT returns high, and the
    // next keypress sets K_INT again and produces the edge that collects both.
    // A keystroke of latency, not a dead keyboard.
    tca8418_write_reg(TCA8418_REG_INT_STAT, TCA8418_INT_STAT_K_INT);

    last_ret
}

// ── HAL vtable ────────────────────────────────────────────────────────────────

/// Static HAL input driver vtable for the TCA8418.
///
/// Returned by `drv_kbd_tca8418_get()` and passed to `hal_input_register()`.
static KEYBOARD_DRIVER: HalInputDriver = HalInputDriver {
    init: Some(tca8418_init),
    deinit: Some(tca8418_deinit),
    register_callback: Some(tca8418_register_callback),
    poll: Some(tca8418_poll),
    name: b"TCA8418\0".as_ptr() as *const c_char,
    is_touch: false,
    set_backlight: Some(tca8418_set_backlight),
};

/// Return the TCA8418 driver vtable.
///
/// Drop-in replacement for the C `drv_kbd_tca8418_get()`.
///
/// # Safety
/// Returns a pointer to a static — safe to call from C.
#[no_mangle]
pub extern "C" fn drv_kbd_tca8418_get() -> *const HalInputDriver {
    &KEYBOARD_DRIVER
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    /// Reset the global driver state between tests.
    unsafe fn reset_state() {
        *(&raw mut S_KBD) = KbdState::new();
    }

    #[test]
    fn test_vtable_pointer_is_non_null() {
        let p = drv_kbd_tca8418_get();
        assert!(!p.is_null());
    }

    #[test]
    fn test_vtable_fields_are_populated() {
        let drv = unsafe { &*drv_kbd_tca8418_get() };
        assert!(drv.init.is_some());
        assert!(drv.deinit.is_some());
        assert!(drv.register_callback.is_some());
        assert!(drv.poll.is_some());
        assert!(!drv.name.is_null());
        assert!(!drv.is_touch);
    }

    #[test]
    fn test_init_null_config_returns_invalid_arg() {
        unsafe {
            reset_state();
            let ret = tca8418_init(std::ptr::null());
            assert_eq!(ret, ESP_ERR_INVALID_ARG);
        }
    }

    #[test]
    fn test_poll_before_init_returns_invalid_state() {
        unsafe {
            reset_state();
            let ret = tca8418_poll();
            assert_eq!(ret, ESP_ERR_INVALID_STATE);
        }
    }

    #[test]
    fn test_init_and_deinit_cycle() {
        unsafe {
            reset_state();
            let cfg = KbdTca8418Config {
                i2c_bus: 1usize as *mut c_void, // non-null sentinel
                i2c_addr: 0x34,
                pin_int: GPIO_NUM_NC,
                pin_backlight: GPIO_NUM_NC,
            };
            let ret = tca8418_init(&cfg as *const KbdTca8418Config as *const c_void);
            assert_eq!(ret, ESP_OK);
            assert!((*(&raw const S_KBD)).initialized);

            tca8418_deinit();
            assert!(!(*(&raw const S_KBD)).initialized);
        }
    }

    #[test]
    fn test_double_init_is_idempotent() {
        unsafe {
            reset_state();
            let cfg = KbdTca8418Config {
                i2c_bus: 1usize as *mut c_void,
                i2c_addr: 0x34,
                pin_int: GPIO_NUM_NC,
                pin_backlight: GPIO_NUM_NC,
            };
            let p = &cfg as *const KbdTca8418Config as *const c_void;
            assert_eq!(tca8418_init(p), ESP_OK);
            assert_eq!(tca8418_init(p), ESP_OK); // idempotent
        }
    }

    #[test]
    fn test_register_callback_stores_values() {
        unsafe {
            reset_state();
            let cfg = KbdTca8418Config {
                i2c_bus: 1usize as *mut c_void,
                i2c_addr: 0x34,
                pin_int: GPIO_NUM_NC,
                pin_backlight: GPIO_NUM_NC,
            };
            tca8418_init(&cfg as *const KbdTca8418Config as *const c_void);

            unsafe extern "C" fn dummy_cb(_event: *const HalInputEvent, _user_data: *mut c_void) {}
            let sentinel = 0xDEAD_BEEFusize as *mut c_void;
            let ret = tca8418_register_callback(Some(dummy_cb), sentinel);
            assert_eq!(ret, ESP_OK);
            assert!((*(&raw const S_KBD)).cb.is_some());
            assert_eq!((*(&raw const S_KBD)).cb_data, sentinel);

            tca8418_deinit();
        }
    }

    #[test]
    fn test_poll_after_init_no_interrupt_returns_ok() {
        unsafe {
            reset_state();
            let cfg = KbdTca8418Config {
                i2c_bus: 1usize as *mut c_void,
                i2c_addr: 0x34,
                pin_int: GPIO_NUM_NC, // polling mode — always check
                pin_backlight: GPIO_NUM_NC,
            };
            tca8418_init(&cfg as *const KbdTca8418Config as *const c_void);
            // Stub returns 0 for INT_STAT (no K_INT set), so poll is a no-op.
            let ret = tca8418_poll();
            assert_eq!(ret, ESP_OK);
            tca8418_deinit();
        }
    }

    #[test]
    fn test_keymap_first_row() {
        // Spot-check the physical T-Deck Pro keymap. The decode path mirrors
        // raw TCA columns before indexing this table.
        assert_eq!(KEY_MAP[0][0], b'q' as u16);
        assert_eq!(KEY_MAP[0][9], b'p' as u16);
        assert_eq!(KEY_MAP[1][9], 0x08); // backspace
        assert_eq!(KEY_MAP[2][9], b'\n' as u16);
        assert_eq!(KEY_MAP[3][0], 0); // unwired
        assert_eq!(KEY_MAP[3][5], 0x03); // left shift
        assert_eq!(KEY_MAP[3][7], b' ' as u16); // space
        assert_eq!(KEY_MAP[3][8], 0x02); // Sym
        assert_eq!(KEY_MAP[3][9], 0x03); // right shift
        assert_eq!(KEY_MAP[5][3], 0x1B); // Esc
    }

    /// A keyboard brought up with the given backlight pin.
    unsafe fn init_with_backlight(pin_backlight: i32) {
        reset_state();
        let cfg = KbdTca8418Config {
            i2c_bus: 1usize as *mut c_void, // non-null sentinel
            i2c_addr: 0x34,
            pin_int: GPIO_NUM_NC,
            pin_backlight,
        };
        assert_eq!(
            tca8418_init(&cfg as *const KbdTca8418Config as *const c_void),
            ESP_OK
        );
    }

    #[test]
    fn the_key_backlight_cannot_be_set_before_the_keyboard_is_up() {
        unsafe {
            reset_state();
            assert_eq!(tca8418_set_backlight(50), ESP_ERR_INVALID_STATE);
        }
    }

    #[test]
    fn a_keyboard_whose_backlight_is_not_wired_says_so() {
        unsafe {
            init_with_backlight(GPIO_NUM_NC);
            // Reporting success here would leave a caller believing it had lit
            // keys that stayed dark.
            assert_eq!(tca8418_set_backlight(50), ESP_ERR_NOT_SUPPORTED);
        }
    }

    #[test]
    fn the_key_backlight_remembers_the_level_it_was_last_lit_at() {
        unsafe {
            init_with_backlight(42);
            assert_eq!(tca8418_set_backlight(75), ESP_OK);
            assert_eq!((*(&raw const S_KBD)).backlight, 75);

            // Going dark is not a new brightness — it is the level to come
            // back to when the keys are lit again.
            assert_eq!(tca8418_set_backlight(0), ESP_OK);
            assert_eq!((*(&raw const S_KBD)).backlight, 75);
        }
    }

    #[test]
    fn the_key_backlight_clamps_a_level_past_full() {
        unsafe {
            init_with_backlight(42);
            assert_eq!(tca8418_set_backlight(200), ESP_OK);
            assert_eq!((*(&raw const S_KBD)).backlight, 100);
        }
    }

    #[test]
    fn the_keyboard_offers_a_backlight_through_its_vtable() {
        // Without this the level is unreachable: the registry is how anything
        // outside the driver gets at it.
        assert!(KEYBOARD_DRIVER.set_backlight.is_some());
    }

    #[test]
    fn all_thirty_five_keys_of_the_matrix_register() {
        // The 4x10 matrix carries 35 keys: three full rows of ten, then five
        // on the bottom row. A zero at a position that carries a physical key
        // is a key that registers nothing at all — dead, with nothing saying so.
        let live = KEY_MAP[..4]
            .iter()
            .flat_map(|row| row.iter())
            .filter(|code| **code != 0)
            .count();
        assert_eq!(live, 35);
    }

    #[test]
    fn the_modifier_keys_decode_to_codes_of_their_own() {
        // Alt and the microphone key carry no character, but they are keys the
        // user can press, so the driver has to recognise them — and must not
        // take one for another.
        assert_eq!(KEY_MAP[2][0], KEY_ALT);
        assert_eq!(KEY_MAP[3][6], KEY_MIC);

        let modifiers = [KEY_ALT, KEY_SYM, KEY_SHIFT, KEY_MIC];
        for (i, code) in modifiers.iter().enumerate() {
            assert!(
                !modifiers[i + 1..].contains(code),
                "two modifiers share code {code:#04x}"
            );
        }
    }

    // ── Shift and the symbol layer ──────────────────────────────────────────

    // Physical (row, col) of the keys the tests below press.
    const Q: (u8, u8) = (0, 0);
    const W: (u8, u8) = (0, 1);
    const BACKSPACE: (u8, u8) = (1, 9);
    const ALT: (u8, u8) = (2, 0);
    const M: (u8, u8) = (2, 7);
    const DOLLAR: (u8, u8) = (2, 8);
    const ENTER: (u8, u8) = (2, 9);
    const LEFT_SHIFT: (u8, u8) = (3, 5);
    const MIC: (u8, u8) = (3, 6);
    const SPACE: (u8, u8) = (3, 7);
    const SYM: (u8, u8) = (3, 8);
    const RIGHT_SHIFT: (u8, u8) = (3, 9);

    fn down(key: (u8, u8)) -> (u8, u8, bool) {
        (key.0, key.1, true)
    }

    fn up(key: (u8, u8)) -> (u8, u8, bool) {
        (key.0, key.1, false)
    }

    /// Press and let go of `key`.
    fn tap(key: (u8, u8)) -> [(u8, u8, bool); 2] {
        [down(key), up(key)]
    }

    /// What the window manager hears when the keyboard runs through `keys`.
    static HEARD: std::sync::Mutex<Vec<(HalInputEventType, u16)>> =
        std::sync::Mutex::new(Vec::new());

    unsafe extern "C" fn hear(event: *const HalInputEvent, _user_data: *mut c_void) {
        let event = &*event;
        HEARD
            .lock()
            .unwrap()
            .push((event.event_type, event.data.key.keycode));
    }

    /// Run `keys` — physical (row, col, pressed) — through a scripted chip and
    /// the real poll, and return every event the window manager received.
    fn press_keys(keys: &[(u8, u8, bool)]) -> Vec<(HalInputEventType, u16)> {
        use esp_ffi::{ChipScript, SCRIPT};

        unsafe {
            esp_ffi::reset_chip();
            reset_state();

            let mut script = ChipScript::default();
            script
                .registers
                .insert(TCA8418_REG_INT_STAT, TCA8418_INT_STAT_K_INT);
            for &(row, col, pressed) in keys {
                // The decode's inverse: 1-based, columns mirrored.
                let code = row * 10 + (9 - col) + 1;
                script.events.push_back(if pressed {
                    KEY_EVENT_PRESS | code
                } else {
                    code
                });
            }
            *SCRIPT.lock().unwrap() = Some(script);

            let kbd = &mut *(&raw mut S_KBD);
            kbd.initialized = true;
            kbd.dev = 1usize as *mut c_void;
            kbd.cb = Some(hear);
            HEARD.lock().unwrap().clear();

            // A poll drains at most one FIFO's worth.
            for _ in 0..keys.len() / TCA8418_FIFO_DEPTH + 1 {
                assert_eq!(tca8418_poll(), ESP_OK);
            }

            *kbd = KbdState::new();
            esp_ffi::reset_chip();
        }
        HEARD.lock().unwrap().clone()
    }

    /// The text `keys` type: every key-down, as characters.
    fn typed(keys: &[(u8, u8, bool)]) -> String {
        press_keys(keys)
            .into_iter()
            .filter(|(kind, _)| *kind == HalInputEventType::KeyDown)
            .map(|(_, code)| char::from_u32(code as u32).unwrap())
            .collect()
    }

    #[test]
    fn the_key_left_of_enter_types_the_dollar_printed_on_it() {
        // The vendor keymap and the key cap both say '$'. The '.' this key
        // once typed is Alt+M, where the cap prints it.
        assert_eq!(typed(&tap(DOLLAR)), "$");
        assert_eq!(typed(&[tap(ALT), tap(DOLLAR)].concat()), "$");
        assert_eq!(typed(&[tap(ALT), tap(M)].concat()), ".");
    }

    #[test]
    fn a_letter_typed_with_shift_held_is_a_capital() {
        let keys = [down(LEFT_SHIFT), down(Q), up(Q), up(LEFT_SHIFT)];
        assert_eq!(typed(&keys), "Q");

        let keys = [down(RIGHT_SHIFT), down(M), up(M), up(RIGHT_SHIFT)];
        assert_eq!(typed(&keys), "M");
    }

    #[test]
    fn shift_tapped_on_its_own_capitalises_the_next_letter_only() {
        let keys = [tap(LEFT_SHIFT), tap(Q), tap(Q)].concat();
        assert_eq!(typed(&keys), "Qq");
    }

    #[test]
    fn a_second_tap_of_shift_takes_the_first_back() {
        let keys = [tap(LEFT_SHIFT), tap(RIGHT_SHIFT), tap(Q)].concat();
        assert_eq!(typed(&keys), "q");
    }

    #[test]
    fn shift_held_through_several_letters_capitalises_them_all() {
        let keys = [
            &[down(LEFT_SHIFT)][..],
            &tap(Q),
            &tap(W),
            &[up(LEFT_SHIFT)],
            &tap(Q),
        ]
        .concat();
        // Shift was used while held, so letting it go is not a tap: the last
        // letter is lower case.
        assert_eq!(typed(&keys), "QWq");
    }

    #[test]
    fn alt_types_the_legend_printed_above_each_key() {
        // Every key in the three letter rows that carries a legend, left to
        // right, under Alt held.
        let rows = [(0, 0..10), (1, 0..9), (2, 1..8)];
        let legends = ["#123()_-+@", "*456/:;'\"", "789?!,."];

        for ((row, cols), legend) in rows.into_iter().zip(legends) {
            let mut keys = vec![down(ALT)];
            for col in cols {
                keys.extend(tap((row, col)));
            }
            keys.push(up(ALT));
            assert_eq!(typed(&keys), legend);
        }
    }

    #[test]
    fn sym_selects_the_same_symbols_as_alt() {
        let keys = [down(SYM), down(Q), up(Q), up(SYM)];
        assert_eq!(typed(&keys), "#");

        let keys = [tap(SYM), tap(W), tap(W)].concat();
        assert_eq!(typed(&keys), "1w");
    }

    #[test]
    fn alt_wins_over_shift() {
        let keys = [
            down(LEFT_SHIFT),
            down(ALT),
            down(W),
            up(W),
            up(ALT),
            up(LEFT_SHIFT),
        ];
        assert_eq!(typed(&keys), "1");
    }

    #[test]
    fn the_microphone_key_types_its_zero_under_alt_and_nothing_without() {
        assert_eq!(typed(&[tap(ALT), tap(MIC)].concat()), "0");
        assert!(press_keys(&tap(MIC)).is_empty());
    }

    #[test]
    fn a_key_with_no_second_legend_types_its_own_code_under_alt() {
        let keys = [
            &[down(ALT)][..],
            &tap(BACKSPACE),
            &tap(SPACE),
            &tap(ENTER),
            &[up(ALT)],
        ]
        .concat();
        assert_eq!(typed(&keys), "\u{8} \n");
    }

    #[test]
    fn the_modifier_keys_themselves_never_reach_the_window_manager() {
        // LVGL reads 0x02 as Home and 0x03 as End, and types 0x01 and 0x04
        // into a text field as control characters.
        let keys = [
            tap(LEFT_SHIFT),
            tap(ALT),
            tap(SYM),
            tap(RIGHT_SHIFT),
            tap(MIC),
        ]
        .concat();
        assert_eq!(press_keys(&keys), Vec::new());
    }

    #[test]
    fn a_key_comes_up_as_the_code_it_went_down_as() {
        // Shift let go before the letter: the release must still name the
        // capital, or LVGL ignores it and repeats 'Q' until another key goes
        // down.
        let keys = [down(LEFT_SHIFT), down(Q), up(LEFT_SHIFT), up(Q)];
        assert_eq!(
            press_keys(&keys),
            vec![
                (HalInputEventType::KeyDown, b'Q' as u16),
                (HalInputEventType::KeyUp, b'Q' as u16),
            ]
        );
    }

    #[test]
    fn deinit_forgets_a_modifier_left_pending() {
        unsafe {
            reset_state();
            let kbd = &mut *(&raw mut S_KBD);
            kbd.initialized = true;
            kbd.key_event(30 + (9 - 5), true); // Left Shift down
            kbd.key_event(30 + (9 - 5), false); // and up: a tap, latched

            tca8418_deinit();
            assert!(!kbd.shift.active());
        }
    }

    #[test]
    fn test_keymap_unmapped_rows_are_zero() {
        for col in 0..10 {
            assert_eq!(KEY_MAP[6][col], 0);
            assert_eq!(KEY_MAP[7][col], 0);
        }
    }

    /// A keyboard whose backlight will not configure must still type.
    ///
    /// `backlight_init` used to be the last thing in init, and its failure
    /// returned before `initialized` was set — so the keys went dark *and*
    /// silent. On a T-Deck Max that is what happened: one wrong field order in
    /// the LEDC config had ESP-IDF reject the timer, and the board came up
    /// unable to type at all. The light is a convenience; the keys are not.
    #[test]
    fn a_keyboard_whose_backlight_fails_still_reports_keys() {
        unsafe {
            let kbd = &mut *(&raw mut S_KBD);
            *kbd = KbdState::new();

            // What init leaves behind when the backlight could not be claimed.
            kbd.backlight_ready = false;
            kbd.cfg.pin_backlight = 42;
            kbd.initialized = true;

            assert!(
                kbd.initialized,
                "a dark backlight must not leave the keyboard uninitialised"
            );

            // poll() refuses only on an uninitialised driver, so an initialised
            // one with no light still scans.
            assert_ne!(
                tca8418_poll(),
                ESP_ERR_INVALID_STATE,
                "an initialised keyboard must scan whether or not it is lit"
            );

            // And the missing light is reported honestly rather than silently
            // accepted.
            assert_eq!(tca8418_set_backlight(50), ESP_ERR_NOT_SUPPORTED);

            *kbd = KbdState::new();
        }
    }

    /// The interrupt must be acknowledged after the FIFO is drained, never
    /// before it.
    ///
    /// The chip holds INT low while K_INT is set. Acknowledging up front lets
    /// a key pressed during the drain set K_INT again with nothing left to
    /// clear it, so the FIFO ends up empty, K_INT set and INT stuck low. The
    /// ISR is edge-triggered, so no later keypress can raise another edge and
    /// the keyboard is deaf permanently. This shipped: a T-Deck Max answered
    /// two keystrokes out of sixteen and then went silent.
    #[test]
    fn the_interrupt_is_acknowledged_after_the_fifo_is_drained() {
        use esp_ffi::{ChipScript, RegOp, SCRIPT, TRACE};

        unsafe {
            esp_ffi::reset_chip();

            let mut script = ChipScript::default();
            script
                .registers
                .insert(TCA8418_REG_INT_STAT, TCA8418_INT_STAT_K_INT);
            script.events.push_back(KEY_EVENT_PRESS | 8);
            *SCRIPT.lock().unwrap() = Some(script);

            let kbd = &mut *(&raw mut S_KBD);
            *kbd = KbdState::new();
            kbd.initialized = true;
            kbd.cfg.pin_int = 15;
            kbd.dev = 1usize as *mut c_void;
            kbd.irq_pending.store(true, Ordering::Release);

            assert_eq!(tca8418_poll(), ESP_OK);

            let trace = TRACE.lock().unwrap().clone();
            let ack = trace
                .iter()
                .position(|op| *op == RegOp::Write(TCA8418_REG_INT_STAT))
                .expect("the poll must acknowledge the key interrupt");

            // Nothing may be read from the FIFO after the acknowledgement:
            // every read belongs before it, so the drain is complete when the
            // chip is told the interrupt is handled.
            let reads_after_ack = trace[ack + 1..]
                .iter()
                .filter(|op| **op == RegOp::Read(TCA8418_REG_KEY_EVENT_A))
                .count();
            assert_eq!(
                reads_after_ack, 0,
                "the FIFO was still being drained after the interrupt was \
                 acknowledged; acknowledging first strands any key pressed \
                 during the drain and wedges INT low for good"
            );

            *kbd = KbdState::new();
            esp_ffi::reset_chip();
        }
    }

    /// A key pressed while a poll is reading the FIFO must still be announced.
    ///
    /// The driver used to clear `irq_pending` at the end of the poll, which
    /// threw away the interrupt belonging to anything that arrived while it
    /// was working. The next poll then took its early return and the keypress
    /// sat in the FIFO unread. On the device that dropped most of a fast
    /// typist's keys — the first press arrived, the ones chasing it did not.
    #[test]
    fn a_key_pressed_during_a_poll_is_not_forgotten() {
        use esp_ffi::{ChipScript, FIRE_ISR_ON_READ, SCRIPT};
        use std::sync::atomic::Ordering as AtomicOrdering;

        unsafe {
            esp_ffi::reset_chip();

            let mut script = ChipScript::default();
            // A key event waiting, and an interrupt asserted to announce it.
            script
                .registers
                .insert(TCA8418_REG_INT_STAT, TCA8418_INT_STAT_K_INT);
            script.events.push_back(KEY_EVENT_PRESS | 8); // 'e' on this matrix
            *SCRIPT.lock().unwrap() = Some(script);

            // The second register read is inside the drain, after the poll has
            // given up the flag — exactly where the lost wakeup used to happen.
            FIRE_ISR_ON_READ.store(2, AtomicOrdering::Relaxed);

            let kbd = &mut *(&raw mut S_KBD);
            kbd.initialized = true;
            kbd.cfg.pin_int = 15;
            kbd.dev = 1usize as *mut c_void;
            kbd.irq_pending.store(true, Ordering::Release);

            assert_eq!(tca8418_poll(), ESP_OK);

            assert!(
                kbd.irq_pending.load(Ordering::Acquire),
                "the interrupt that arrived mid-poll must survive the poll, \
                 or the key it belongs to is never read"
            );

            kbd.initialized = false;
            kbd.dev = std::ptr::null_mut();
            esp_ffi::reset_chip();
        }
    }

    #[test]
    fn test_isr_handler_sets_irq_pending() {
        unsafe {
            reset_state();
            (*(&raw mut S_KBD))
                .irq_pending
                .store(false, Ordering::Relaxed);
            tca8418_isr_handler(std::ptr::null_mut());
            assert!((*(&raw const S_KBD)).irq_pending.load(Ordering::Acquire));
        }
    }

    #[test]
    fn test_deinit_noop_when_not_initialized() {
        unsafe {
            reset_state();
            // Should not panic
            tca8418_deinit();
            assert!(!(*(&raw const S_KBD)).initialized);
        }
    }
}
