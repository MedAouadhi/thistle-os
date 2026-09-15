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
// carries a code, including the two that carry no character — Alt and the
// microphone key — because a key that decodes to nothing is a key the user
// sees as broken.

/// Keys that carry no character of their own. The window manager receives
/// these codes verbatim, so they sit below printable ASCII where they cannot
/// be mistaken for text.
const KEY_ALT: u16 = 0x01;
const KEY_SYM: u16 = 0x02;
const KEY_SHIFT: u16 = 0x03;
const KEY_MIC: u16 = 0x04;

#[rustfmt::skip]
static KEY_MAP: [[u16; 10]; 8] = [
    // C0     C1     C2     C3     C4     C5     C6     C7     C8     C9
    [  b'q' as u16, b'w' as u16, b'e' as u16, b'r' as u16, b't' as u16, b'y' as u16, b'u' as u16, b'i' as u16, b'o' as u16, b'p' as u16  ],
    [  b'a' as u16, b's' as u16, b'd' as u16, b'f' as u16, b'g' as u16, b'h' as u16, b'j' as u16, b'k' as u16, b'l' as u16, 0x08u16      ],  // 0x08 = backspace
    [  KEY_ALT,     b'z' as u16, b'x' as u16, b'c' as u16, b'v' as u16, b'b' as u16, b'n' as u16, b'm' as u16, b'.' as u16, b'\n' as u16 ],  // Alt Z X C V B N M . Enter
    // Row 3: only 5 physical keys at TCA cols 0..4 (after mirror, c 5..9):
    //   c=5 shift(L)  c=6 mic  c=7 space  c=8 sym  c=9 shift(R)
    [  0,           0,           0,           0,           0,           KEY_SHIFT,   KEY_MIC,     b' ' as u16, KEY_SYM,     KEY_SHIFT   ],
    [  b'7' as u16, b'8' as u16, b'9' as u16, b'0' as u16, b'-' as u16, b'=' as u16, b'[' as u16, b']' as u16, b'\\' as u16, b'\'' as u16 ],
    [  b';' as u16, b'/' as u16, b'`' as u16, 0x1Bu16,     0,          0,           0,           0,           0,           0            ],  // 0x1B = Esc
    [  0,           0,           0,           0,           0,           0,           0,           0,           0,           0            ],
    [  0,           0,           0,           0,           0,           0,           0,           0,           0,           0            ],
];

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
        _data: *const u8,
        _len: usize,
        _timeout: i32,
    ) -> i32 {
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
        }
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
    let ret = backlight_init(kbd.cfg.pin_backlight);
    if ret != ESP_OK {
        return ret;
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
    if kbd.cfg.pin_backlight == GPIO_NUM_NC {
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

    // Acknowledge before draining, for the same reason. Acknowledging
    // afterwards clears the interrupt belonging to whatever arrived while the
    // FIFO was being read, and the INT line is edge-triggered, so nothing
    // would announce that event again.
    tca8418_write_reg(TCA8418_REG_INT_STAT, TCA8418_INT_STAT_K_INT);

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
        let kc0 = key_code - 1; // make 0-based
        let row = (kc0 / 10) as usize;
        let col_raw = (kc0 % 10) as usize;
        // Mirror the column index: T-Deck Pro wiring puts physical col 0 on
        // TCA col 9 and vice versa. KEY_MAP is written in physical order.
        let col = 9 - col_raw;

        let keycode: u16 = if row < 8 && col < 10 {
            KEY_MAP[row][col]
        } else {
            0
        };

        if keycode == 0 {
            // Unmapped key — skip silently
            continue;
        }

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
        // user can press, so they have to reach the window manager — and they
        // must not reach it as each other.
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

    #[test]
    fn test_keymap_unmapped_rows_are_zero() {
        for col in 0..10 {
            assert_eq!(KEY_MAP[6][col], 0);
            assert_eq!(KEY_MAP[7][col], 0);
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
