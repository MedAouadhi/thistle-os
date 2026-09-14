// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — SY6970 / BQ25896 charger + BQ27220 fuel gauge power driver (Rust)
//
// One driver, one power HAL interface, two chips.
//
// The charger reports battery voltage and charge state. The SY6970 and the
// BQ25896 are register-map clones — same registers, same scaling constants,
// same bit-level decode — so they are a variant of one driver rather than two
// drivers. They differ in I²C address and in the part-number field of their
// device identification register, which is what tells them apart here. A
// T-Deck Max carries one or the other depending on its hardware revision, and
// a T-Deck Pro carries the BQ25896, so nothing may be assumed from the board.
//
// The fuel gauge reports the percentage, which a voltage curve can only
// approximate. When no gauge answers, the curve is the fallback.
//
// Neither chip's interrupt is routed to the SoC on the boards that carry them,
// so every reading here is a poll.

use std::os::raw::{c_char, c_void};
use std::sync::Mutex;

use crate::hal_registry::{HalPowerDriver, HalPowerInfo, HalPowerState};

// ── ESP error codes ──────────────────────────────────────────────────────────

const ESP_OK: i32 = 0;
const ESP_FAIL: i32 = -1;
const ESP_ERR_NOT_FOUND: i32 = 0x105;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;

// ── Charger register map (SY6970 / BQ25896) ──────────────────────────────────

/// ADC control. Bit 7 starts a conversion, bit 6 selects the rate.
const CHG_REG_ADC_CONTROL: u8 = 0x02;
/// Charge termination and timer control, holding the I²C watchdog setting.
const CHG_REG_TIMER_CONTROL: u8 = 0x07;
/// System status: bus status in bits 7:5, charge status in bits 4:3.
const CHG_REG_STATUS: u8 = 0x0B;
/// Battery voltage ADC conversion.
const CHG_REG_BATTERY_VOLTAGE: u8 = 0x0E;
/// Charge current ADC conversion.
const CHG_REG_CHARGE_CURRENT: u8 = 0x12;
/// Device identification: part number in bits 5:3, revision in bits 1:0.
const CHG_REG_IDENTIFICATION: u8 = 0x14;

/// Battery voltage at a zero conversion, in millivolts.
const BATTERY_MV_BASE: u16 = 2304;
/// Millivolts per step of the battery voltage conversion.
const BATTERY_MV_STEP: u16 = 20;
/// Milliamps per step of the charge current conversion.
const CHARGE_CURRENT_MA_STEP: u16 = 50;

/// The conversion fields are seven bits wide; the eighth carries a status flag.
const ADC_VALUE_MASK: u8 = 0x7F;

/// Bit 7 starts an ADC conversion, bit 6 asks for one every second rather than
/// one only when started. Both are needed: the converter does not free-run, and
/// without it the voltage register reads whatever it last converted.
const ADC_CONTINUOUS_BITS: u8 = 0b1100_0000;

/// The I²C watchdog setting, bits 5:4 of the timer control register. It resets
/// to 40 seconds, and on expiry returns every configuration register to its
/// default — silently, while an idle system is not talking to the charger.
const WATCHDOG_BITS: u8 = 0b0011_0000;

// ── Fuel gauge command map (BQ27220) ─────────────────────────────────────────
//
// The gauge answers standard commands as little-endian 16-bit words. None of
// the readings here need the gauge unsealed: sealed access covers the standard
// commands and is refused only for data memory.

/// Battery voltage, in millivolts.
const GAUGE_CMD_VOLTAGE: u8 = 0x08;
/// Instantaneous current, signed: positive charging, negative discharging.
const GAUGE_CMD_CURRENT: u8 = 0x0C;
/// State of charge, as a percentage of full charge capacity.
const GAUGE_CMD_STATE_OF_CHARGE: u8 = 0x2C;

/// The gauge's I²C address, unless a board profile names another.
const GAUGE_DEFAULT_ADDRESS: u8 = 0x55;

// ── I²C plumbing constants ───────────────────────────────────────────────────

/// How long a register transfer may take before it is given up on.
const I2C_TIMEOUT_MS: i32 = 50;

/// Default I²C speed for a device handle. The bus's own frequency is
/// configured per device under ESP-IDF's i2c_master driver.
const I2C_SPEED_HZ: u32 = 400_000;

// ── Charger variant ──────────────────────────────────────────────────────────

/// Which of the two register-compatible chargers answered.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChargerVariant {
    Sy6970,
    Bq25896,
}

impl ChargerVariant {
    /// The address the part answers at. The two never collide, so both are
    /// probed and whichever answers is read for its identification register.
    pub fn address(self) -> u8 {
        match self {
            ChargerVariant::Sy6970 => 0x6A,
            ChargerVariant::Bq25896 => 0x6B,
        }
    }

    /// The part's name, null-terminated because its only reader is the C log
    /// line someone watches during bring-up to see which part answered.
    pub fn name(self) -> &'static [u8] {
        match self {
            ChargerVariant::Sy6970 => b"SY6970\0",
            ChargerVariant::Bq25896 => b"BQ25896\0",
        }
    }
}

/// Every address a charger might answer at, in probe order.
const CHARGER_PROBE_ORDER: [ChargerVariant; 2] = [ChargerVariant::Sy6970, ChargerVariant::Bq25896];

// ── Charger decoding ─────────────────────────────────────────────────────────

/// Identify the charger from its device identification register.
///
/// The part number is the field to read, not the revision beside it: both
/// datasheets document the revision as starting from zero and counting up per
/// die, so a part that is otherwise identical would stop being recognised the
/// first time the vendor spun it.
///
/// Returns `None` for a part number neither datasheet claims — including the
/// `0xFF` an unpopulated bus reads back — so an unknown chip is declined
/// rather than driven as if it were one of these two.
pub fn charger_variant(identification: u8) -> Option<ChargerVariant> {
    match (identification >> 3) & 0b111 {
        0b000 => Some(ChargerVariant::Bq25896),
        0b001 => Some(ChargerVariant::Sy6970),
        _ => None,
    }
}

/// Battery voltage in millivolts, from the charger's ADC conversion register.
///
/// A zero conversion is reported as zero rather than as the 2304 mV the
/// formula would give, because the part reports zero when it has no battery
/// reading to give — the formula's floor is not a voltage any cell reaches.
pub fn battery_mv(conversion: u8) -> u16 {
    let steps = (conversion & ADC_VALUE_MASK) as u16;
    if steps == 0 {
        return 0;
    }
    BATTERY_MV_BASE + steps * BATTERY_MV_STEP
}

/// Charge state, from the charger's status register.
///
/// Pre-charge and fast charge are both charging as far as the HAL is
/// concerned; what a user wants to tell apart is charging from finished.
pub fn charge_state(status: u8) -> HalPowerState {
    match (status >> 3) & 0b11 {
        0b00 => HalPowerState::Discharging,
        0b11 => HalPowerState::Charged,
        _ => HalPowerState::Charging,
    }
}

/// Charge current in milliamps, from the charger's ADC conversion register.
///
/// The status register is read alongside it because the conversion register
/// keeps its last value when the charger stops charging rather than falling to
/// zero. Reported on its own it would claim a disconnected board was still
/// drawing whatever it drew when the cable came out.
pub fn charge_current_ma(status: u8, conversion: u8) -> u16 {
    if charge_state(status) == HalPowerState::Discharging {
        return 0;
    }
    (conversion & ADC_VALUE_MASK) as u16 * CHARGE_CURRENT_MA_STEP
}

/// The ADC control register with continuous conversion asked for, leaving the
/// bits that configure boost mode and input detection as they were.
pub fn with_adc_continuous(adc_control: u8) -> u8 {
    adc_control | ADC_CONTINUOUS_BITS
}

/// The timer control register with the I²C watchdog turned off, leaving
/// termination and the safety timer as they were.
pub fn with_watchdog_disabled(timer_control: u8) -> u8 {
    timer_control & !WATCHDOG_BITS
}

// ── Fuel gauge decoding ──────────────────────────────────────────────────────

/// A gauge reply, which arrives least significant byte first.
pub fn gauge_word(reply: [u8; 2]) -> u16 {
    u16::from_le_bytes(reply)
}

/// State of charge as a percentage.
///
/// The gauge reports 0-100 and nothing else is meaningful, so a reading above
/// full is clamped rather than passed on to a battery icon that would overflow
/// its bar.
pub fn gauge_percent(word: u16) -> u8 {
    word.min(100) as u8
}

/// Battery voltage in millivolts, which the gauge reports directly.
pub fn gauge_voltage_mv(word: u16) -> u16 {
    word
}

/// Current in milliamps, positive into the battery and negative out of it.
///
/// The gauge's current commands are two's complement, so the word is signed:
/// read unsigned, a discharging board reports tens of thousands of milliamps.
pub fn gauge_current_ma(word: u16) -> i16 {
    word as i16
}

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

// ── A fake bus for host tests ────────────────────────────────────────────────
//
// The charger and the gauge are both plain register files over I²C, which is
// little enough to model: a device answers at an address or it does not, and
// what the driver wrote to it stays written. That makes the state the driver
// leaves the charger in — its watchdog off, its converter running — something
// a test can look at, rather than something only hardware can confirm.

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
mod esp_ffi {
    use std::os::raw::c_void;
    use std::sync::Mutex;

    /// Registers per device. Wide enough for the gauge's command codes, which
    /// run further than the charger's registers do.
    const REGISTERS: usize = 0x80;

    /// A device handle carries the address it was opened for, which is all the
    /// fake needs to route a transfer.
    const HANDLE_TAG: usize = 0x1000;

    static BUS: Mutex<Vec<(u8, [u8; REGISTERS])>> = Mutex::new(Vec::new());

    #[repr(C)]
    pub struct I2cDeviceConfig {
        pub dev_addr_length: u32,
        pub device_address: u16,
        pub scl_speed_hz: u32,
        pub scl_wait_us: u32,
        pub flags: u32,
    }

    fn address_of(handle: *mut c_void) -> u8 {
        (handle as usize & 0xFF) as u8
    }

    /// Take every device off the bus, for a test that wants to start from an
    /// empty one.
    pub fn clear() {
        BUS.lock().unwrap().clear();
    }

    /// Put a device on the bus, with every register reading zero.
    pub fn attach(address: u8) {
        let mut bus = BUS.lock().unwrap();
        if !bus.iter().any(|(addr, _)| *addr == address) {
            bus.push((address, [0u8; REGISTERS]));
        }
    }

    /// Set one register of an attached device.
    pub fn set(address: u8, register: u8, value: u8) {
        let mut bus = BUS.lock().unwrap();
        if let Some((_, registers)) = bus.iter_mut().find(|(addr, _)| *addr == address) {
            registers[register as usize] = value;
        }
    }

    /// Set a little-endian word, as the gauge's commands answer.
    pub fn set_word(address: u8, command: u8, value: u16) {
        let [low, high] = value.to_le_bytes();
        set(address, command, low);
        set(address, command + 1, high);
    }

    /// Read one register back, to see what the driver left behind.
    pub fn get(address: u8, register: u8) -> u8 {
        let bus = BUS.lock().unwrap();
        bus.iter()
            .find(|(addr, _)| *addr == address)
            .map(|(_, registers)| registers[register as usize])
            .unwrap_or(0)
    }

    /// Opening a device always succeeds, as it does on hardware: an address
    /// with nothing behind it is only discovered on the first transfer.
    pub unsafe fn i2c_master_bus_add_device(
        _bus: *mut c_void,
        cfg: *const I2cDeviceConfig,
        handle: *mut *mut c_void,
    ) -> i32 {
        unsafe { *handle = (HANDLE_TAG | (*cfg).device_address as usize) as *mut c_void };
        0
    }

    pub unsafe fn i2c_master_bus_rm_device(_handle: *mut c_void) -> i32 {
        0
    }

    pub unsafe fn i2c_master_transmit_receive(
        handle: *mut c_void,
        write: *const u8,
        wsz: usize,
        read: *mut u8,
        rsz: usize,
        _timeout: i32,
    ) -> i32 {
        let bus = BUS.lock().unwrap();
        let Some((_, registers)) = bus.iter().find(|(addr, _)| *addr == address_of(handle)) else {
            return -1; // nothing answered
        };
        if wsz != 1 {
            return -1;
        }
        let first = unsafe { *write } as usize;
        for offset in 0..rsz {
            unsafe { *read.add(offset) = registers[(first + offset) % REGISTERS] };
        }
        0
    }

    pub unsafe fn i2c_master_transmit(
        handle: *mut c_void,
        data: *const u8,
        len: usize,
        _timeout: i32,
    ) -> i32 {
        let mut bus = BUS.lock().unwrap();
        let Some((_, registers)) = bus.iter_mut().find(|(addr, _)| *addr == address_of(handle))
        else {
            return -1;
        };
        if len != 2 {
            return -1;
        }
        let register = unsafe { *data } as usize;
        registers[register] = unsafe { *data.add(1) };
        0
    }
}

// ── Logging ──────────────────────────────────────────────────────────────────

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: u32, tag: *const u8, format: *const u8, ...);
}

static TAG: &[u8] = b"power\0";

#[cfg(not(test))]
macro_rules! power_logi {
    ($fmt:expr $(, $arg:expr)*) => {
        unsafe { esp_log_write(3, TAG.as_ptr(), $fmt.as_ptr() $(, $arg)*); }
    };
}

#[cfg(test)]
macro_rules! power_logi {
    ($fmt:expr $(, $arg:expr)*) => { let _ = ($($arg,)*); };
}

// ── Driver state ─────────────────────────────────────────────────────────────

/// What a board profile says about where the two chips are.
///
/// The charger's address is not among it: the two variants answer at addresses
/// only they use, so the driver finds it by probing rather than by being told,
/// which is the point — a board carries whichever part its revision shipped
/// with, and the board profile cannot know which that was.
struct Config {
    i2c_bus: i32,
    gauge_address: u8,
}

struct Power {
    config: Config,
    /// Which charger answered, and therefore whether the driver is up.
    variant: Option<ChargerVariant>,
    charger: *mut c_void,
    /// Null when no gauge answered, in which case percentage comes off the
    /// discharge curve instead.
    gauge: *mut c_void,
}

// SAFETY: the device handles are opaque tokens, dereferenced only by the
// ESP-IDF I²C driver, which is itself thread-safe. The state is guarded by the
// mutex below.
unsafe impl Send for Power {}

impl Power {
    const fn new() -> Self {
        Power {
            config: Config {
                i2c_bus: 0,
                gauge_address: GAUGE_DEFAULT_ADDRESS,
            },
            variant: None,
            charger: std::ptr::null_mut(),
            gauge: std::ptr::null_mut(),
        }
    }
}

static POWER: Mutex<Power> = Mutex::new(Power::new());

// ── I²C helpers ──────────────────────────────────────────────────────────────

/// Open a device on `bus` at `address`. Opening succeeds whether or not
/// anything is there; the first transfer is what finds out.
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
    if ret != ESP_OK || dev.is_null() {
        return None;
    }
    Some(dev)
}

fn close(dev: *mut c_void) {
    if !dev.is_null() {
        unsafe { esp_ffi::i2c_master_bus_rm_device(dev) };
    }
}

fn read_bytes(dev: *mut c_void, register: u8, buf: &mut [u8]) -> bool {
    if dev.is_null() {
        return false;
    }
    let ret = unsafe {
        esp_ffi::i2c_master_transmit_receive(
            dev,
            &register as *const u8,
            1,
            buf.as_mut_ptr(),
            buf.len(),
            I2C_TIMEOUT_MS,
        )
    };
    ret == ESP_OK
}

fn read_reg(dev: *mut c_void, register: u8) -> Option<u8> {
    let mut value = [0u8; 1];
    read_bytes(dev, register, &mut value).then_some(value[0])
}

fn write_reg(dev: *mut c_void, register: u8, value: u8) -> bool {
    if dev.is_null() {
        return false;
    }
    let buf: [u8; 2] = [register, value];
    let ret = unsafe { esp_ffi::i2c_master_transmit(dev, buf.as_ptr(), buf.len(), I2C_TIMEOUT_MS) };
    ret == ESP_OK
}

/// Change one register, leaving the bits the driver has no opinion about as
/// the charger had them.
fn update_reg(dev: *mut c_void, register: u8, edit: fn(u8) -> u8) -> bool {
    match read_reg(dev, register) {
        Some(value) => write_reg(dev, register, edit(value)),
        None => false,
    }
}

/// Read one of the gauge's 16-bit commands.
fn read_gauge(dev: *mut c_void, command: u8) -> Option<u16> {
    let mut reply = [0u8; 2];
    read_bytes(dev, command, &mut reply).then(|| gauge_word(reply))
}

// ── Bring-up ─────────────────────────────────────────────────────────────────

/// A charger that answered: which part it says it is, the address it actually
/// answered at, and the handle to talk to it through.
///
/// The identity and the address are separate facts on purpose. The address
/// narrows the search, but the identification register is what settles which
/// part this is, so a log line has to report the address that was probed
/// rather than the one the identity implies.
struct AttachedCharger {
    variant: ChargerVariant,
    address: u8,
    dev: *mut c_void,
}

/// Find the charger, by asking both addresses which part they are.
///
/// A part that answers but is not one of the two is left closed rather than
/// driven on the assumption that whatever sits there is close enough: an
/// unrecognised part number means an unknown register map, and the next thing
/// this driver does is write to configuration registers.
fn attach_charger(bus: *mut c_void) -> Option<AttachedCharger> {
    for candidate in CHARGER_PROBE_ORDER {
        let address = candidate.address();
        let Some(dev) = open(bus, address) else {
            continue;
        };
        match read_reg(dev, CHG_REG_IDENTIFICATION).and_then(charger_variant) {
            Some(variant) => {
                return Some(AttachedCharger {
                    variant,
                    address,
                    dev,
                })
            }
            None => close(dev),
        }
    }
    None
}

/// Put the charger into the state the rest of this driver assumes.
///
/// Neither of these is optional on this hardware and neither announces itself
/// when it is missing: an armed watchdog quietly returns every configuration
/// register to its default while the system idles, and a converter that is not
/// running leaves the voltage register reading whatever it last converted.
fn configure_charger(dev: *mut c_void) -> bool {
    update_reg(dev, CHG_REG_TIMER_CONTROL, with_watchdog_disabled)
        && update_reg(dev, CHG_REG_ADC_CONTROL, with_adc_continuous)
}

/// Open the fuel gauge, or report that none answered.
///
/// Nothing is unsealed on the way: the gauge answers its standard commands
/// sealed, and unsealing is for writing data memory, which this driver never
/// does.
fn attach_gauge(bus: *mut c_void, address: u8) -> *mut c_void {
    let Some(dev) = open(bus, address) else {
        return std::ptr::null_mut();
    };
    if read_gauge(dev, GAUGE_CMD_STATE_OF_CHARGE).is_some() {
        return dev;
    }
    close(dev);
    std::ptr::null_mut()
}

// ── Readings ─────────────────────────────────────────────────────────────────

fn battery_millivolts(power: &Power) -> u16 {
    read_reg(power.charger, CHG_REG_BATTERY_VOLTAGE)
        .map(battery_mv)
        .unwrap_or(0)
}

/// Battery percentage, from the gauge where there is one.
///
/// The discharge curve is a fallback and not a good one — a cell's voltage
/// says little about its charge across the flat middle of its range — so it is
/// used only where no gauge answered.
fn battery_percent(power: &Power) -> u8 {
    if let Some(percent) = read_gauge(power.gauge, GAUGE_CMD_STATE_OF_CHARGE).map(gauge_percent) {
        return percent;
    }
    crate::battery_curve::voltage_to_percent(battery_millivolts(power))
}

fn charger_status(power: &Power) -> Option<u8> {
    read_reg(power.charger, CHG_REG_STATUS)
}

// ── vtable implementations ───────────────────────────────────────────────────

/// Bring up both chips.
///
/// The configuration arrived at registration rather than through this pointer,
/// which the board profile path has nowhere to keep alive until the driver
/// manager gets here.
///
/// # Safety
/// Called from C via the HAL vtable.
unsafe extern "C" fn power_init(_config: *const c_void) -> i32 {
    let Ok(mut power) = POWER.lock() else {
        return ESP_FAIL;
    };
    if power.variant.is_some() {
        return ESP_OK; // already up
    }

    let bus = crate::hal_registry::hal_bus_get_i2c(power.config.i2c_bus);
    if bus.is_null() {
        return ESP_ERR_NOT_FOUND;
    }

    let Some(charger) = attach_charger(bus) else {
        return ESP_ERR_NOT_FOUND;
    };
    if !configure_charger(charger.dev) {
        close(charger.dev);
        return ESP_FAIL;
    }

    power.charger = charger.dev;
    power.gauge = attach_gauge(bus, power.config.gauge_address);
    power.variant = Some(charger.variant);

    // One line naming what answered and what it says, because this driver is
    // brought up on a board nobody can step through: which part, where it
    // actually answered, and two independent readings of the same cell — the
    // charger's converter and the gauge's — which should agree.
    //
    // It carries its own tag and newline. esp_log_write does not add either,
    // and an unterminated line is the one the console drops first when the
    // boot burst overruns it — which is exactly when this line is read.
    let millivolts = battery_millivolts(&power) as i32;
    let percent = battery_percent(&power) as i32;
    let charge_current = charger_status(&power)
        .and_then(|status| {
            read_reg(power.charger, CHG_REG_CHARGE_CURRENT)
                .map(|conversion| charge_current_ma(status, conversion))
        })
        .unwrap_or(0) as i32;
    let gauge_millivolts = read_gauge(power.gauge, GAUGE_CMD_VOLTAGE)
        .map(gauge_voltage_mv)
        .unwrap_or(0) as i32;
    let gauge_current = read_gauge(power.gauge, GAUGE_CMD_CURRENT)
        .map(gauge_current_ma)
        .unwrap_or(0) as i32;
    power_logi!(
        b"power: %s at 0x%02x: %d mV, %d %%, charging at %d mA, gauge %d mV / %d mA\n\0",
        charger.variant.name().as_ptr(),
        charger.address as i32,
        millivolts,
        percent,
        charge_current,
        gauge_millivolts,
        gauge_current
    );

    ESP_OK
}

/// # Safety
/// Called from C via the HAL vtable.
unsafe extern "C" fn power_deinit() {
    let Ok(mut power) = POWER.lock() else {
        return;
    };
    close(power.charger);
    close(power.gauge);
    power.charger = std::ptr::null_mut();
    power.gauge = std::ptr::null_mut();
    power.variant = None;
}

/// # Safety
/// Called from C via the HAL vtable; `info` must be a valid non-null pointer.
unsafe extern "C" fn power_get_info(info: *mut HalPowerInfo) -> i32 {
    if info.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let Ok(power) = POWER.lock() else {
        return ESP_FAIL;
    };
    if power.variant.is_none() {
        return ESP_ERR_INVALID_STATE;
    }

    let millivolts = battery_millivolts(&power);
    // The charger reports no conversion at all rather than a floor voltage
    // when there is no cell for it to measure.
    let state = if millivolts == 0 {
        HalPowerState::NoBattery
    } else {
        charger_status(&power)
            .map(charge_state)
            .unwrap_or(HalPowerState::Discharging)
    };

    unsafe {
        (*info).voltage_mv = millivolts;
        (*info).percent = battery_percent(&power);
        (*info).state = state;
    }
    ESP_OK
}

/// # Safety
/// Called from C via the HAL vtable.
unsafe extern "C" fn power_get_battery_mv() -> u16 {
    match POWER.lock() {
        Ok(power) => battery_millivolts(&power),
        Err(_) => 0,
    }
}

/// # Safety
/// Called from C via the HAL vtable.
unsafe extern "C" fn power_get_battery_percent() -> u8 {
    match POWER.lock() {
        Ok(power) => battery_percent(&power),
        Err(_) => 0,
    }
}

/// # Safety
/// Called from C via the HAL vtable.
unsafe extern "C" fn power_is_charging() -> bool {
    let Ok(power) = POWER.lock() else {
        return false;
    };
    charger_status(&power).map(charge_state) == Some(HalPowerState::Charging)
}

/// Sleep and wake are nothing to these two chips: the charger goes on
/// charging with the SoC asleep, which is the point of it, and the gauge's own
/// consumption is below what turning it off would save.
///
/// # Safety
/// Called from C via the HAL vtable.
unsafe extern "C" fn power_sleep(_enter: bool) -> i32 {
    ESP_OK
}

// ── HAL vtable ───────────────────────────────────────────────────────────────

static POWER_DRIVER: HalPowerDriver = HalPowerDriver {
    init: Some(power_init),
    deinit: Some(power_deinit),
    get_info: Some(power_get_info),
    get_battery_mv: Some(power_get_battery_mv),
    get_battery_percent: Some(power_get_battery_percent),
    is_charging: Some(power_is_charging),
    sleep: Some(power_sleep),
    // One driver, two parts. Which one this board carries is in the init log.
    name: b"SY6970/BQ25896\0".as_ptr() as *const c_char,
};

// ── Registration ─────────────────────────────────────────────────────────────

/// Read the driver entry's configuration.
///
/// Both chips sit on a bus the board profile names by index. The gauge's
/// address is accepted as an override for a board that wires it elsewhere,
/// written either bare or as the quoted hex driver entries use.
fn parse_config(config_json: &str) -> Config {
    Config {
        i2c_bus: crate::manifest::json_get_int(config_json, "i2c_bus").unwrap_or(0) as i32,
        gauge_address: parse_address(config_json, "gauge_i2c_addr")
            .unwrap_or(GAUGE_DEFAULT_ADDRESS),
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
/// The configuration is read here rather than handed to `init` through the
/// registry, because the board profile path has nowhere to keep a config
/// struct alive between registering a driver and the driver manager starting
/// it.
///
/// # Safety
/// Called from C. `config_json` must be null or a valid null-terminated string.
#[no_mangle]
pub unsafe extern "C" fn drv_power_sy6970_register(config_json: *const c_char) -> i32 {
    let config = if config_json.is_null() {
        parse_config("{}")
    } else {
        match unsafe { std::ffi::CStr::from_ptr(config_json) }.to_str() {
            Ok(json) => parse_config(json),
            Err(_) => return ESP_ERR_INVALID_ARG,
        }
    };

    match POWER.lock() {
        Ok(mut power) => power.config = config,
        Err(_) => return ESP_FAIL,
    }

    unsafe { crate::hal_registry::hal_power_register(&POWER_DRIVER, std::ptr::null()) }
}

#[cfg(test)]
mod tests {
    use super::*;

    // ── Variant identification ───────────────────────────────────────────────

    #[test]
    fn a_part_number_of_zero_is_the_bq25896() {
        assert_eq!(charger_variant(0b0000_0000), Some(ChargerVariant::Bq25896));
    }

    #[test]
    fn a_part_number_of_one_is_the_sy6970() {
        assert_eq!(charger_variant(0b0000_1000), Some(ChargerVariant::Sy6970));
    }

    #[test]
    fn a_die_revision_does_not_change_which_part_was_identified() {
        // The revision is the low two bits. The BQ25896 ships revision 2 and
        // the SY6970 revision 0 today, and either could be spun tomorrow
        // without becoming a different part.
        for revision in 0..=0b11 {
            assert_eq!(
                charger_variant(0b0000_0000 | revision),
                Some(ChargerVariant::Bq25896),
                "revision {revision} should still read as a BQ25896"
            );
            assert_eq!(
                charger_variant(0b0000_1000 | revision),
                Some(ChargerVariant::Sy6970),
                "revision {revision} should still read as an SY6970"
            );
        }
    }

    #[test]
    fn the_flags_sharing_the_register_do_not_change_the_identification() {
        // Bit 7 is the register reset, bit 6 the input current limit status,
        // bit 2 the temperature profile — all live alongside the part number.
        assert_eq!(charger_variant(0b1100_0100), Some(ChargerVariant::Bq25896));
        assert_eq!(charger_variant(0b1100_1100), Some(ChargerVariant::Sy6970));
    }

    #[test]
    fn a_part_number_neither_datasheet_claims_is_declined() {
        assert_eq!(charger_variant(0b0001_0000), None);
        assert_eq!(charger_variant(0b0011_1000), None);
    }

    #[test]
    fn an_all_ones_read_is_declined_rather_than_taken_for_a_charger() {
        // What a read past the last register, or off an absent part, gives.
        assert_eq!(charger_variant(0xFF), None);
    }

    #[test]
    fn the_two_variants_answer_at_different_addresses() {
        assert_eq!(ChargerVariant::Sy6970.address(), 0x6A);
        assert_eq!(ChargerVariant::Bq25896.address(), 0x6B);
    }

    #[test]
    fn each_variant_names_itself_for_the_log_line_that_reads_it() {
        // The reader is C, so the name has to carry its terminator with it.
        for variant in CHARGER_PROBE_ORDER {
            assert_eq!(variant.name().last(), Some(&0u8), "{variant:?}");
        }
        assert_eq!(ChargerVariant::Sy6970.name(), b"SY6970\0");
        assert_eq!(ChargerVariant::Bq25896.name(), b"BQ25896\0");
    }

    // ── Battery voltage ──────────────────────────────────────────────────────

    #[test]
    fn battery_voltage_steps_by_twenty_millivolts_from_2304() {
        // The datasheet's own table: 0000001 = 2.324 V, 1111111 = 4.844 V.
        assert_eq!(battery_mv(0b000_0001), 2324);
        assert_eq!(battery_mv(0b111_1111), 4844);
    }

    #[test]
    fn a_battery_voltage_of_zero_reports_no_reading() {
        assert_eq!(battery_mv(0), 0);
    }

    #[test]
    fn the_thermal_status_flag_is_not_read_as_voltage() {
        // Bit 7 of the register is the thermal regulation status, not part of
        // the conversion. Left in, it would add 2.56 V to every reading.
        assert_eq!(battery_mv(0b1000_0001), 2324);
        assert_eq!(battery_mv(0xFF), 4844);
        assert_eq!(battery_mv(0b1000_0000), 0);
    }

    #[test]
    fn a_typical_half_charged_cell_decodes_to_its_datasheet_voltage() {
        // 3.824 V = 2.304 V + 76 * 20 mV.
        assert_eq!(battery_mv(76), 3824);
    }

    // ── Charge state ─────────────────────────────────────────────────────────

    #[test]
    fn the_four_charge_status_codes_map_onto_three_states() {
        assert_eq!(charge_state(0b0_00_000), HalPowerState::Discharging);
        assert_eq!(charge_state(0b0_01_000), HalPowerState::Charging);
        assert_eq!(charge_state(0b0_10_000), HalPowerState::Charging);
        assert_eq!(charge_state(0b0_11_000), HalPowerState::Charged);
    }

    #[test]
    fn the_bus_status_sharing_the_register_does_not_change_the_charge_state() {
        // Bits 7:5 are the bus status, which differs between the two parts by
        // a few extra codes and says nothing about the battery.
        assert_eq!(charge_state(0b111_00_000), HalPowerState::Discharging);
        assert_eq!(charge_state(0b101_11_000), HalPowerState::Charged);
        assert_eq!(charge_state(0b011_10_111), HalPowerState::Charging);
    }

    // ── Charge current ───────────────────────────────────────────────────────

    #[test]
    fn charge_current_steps_by_fifty_milliamps() {
        let fast_charging = 0b0_10_000;
        assert_eq!(charge_current_ma(fast_charging, 0b000_0001), 50);
        assert_eq!(charge_current_ma(fast_charging, 0b111_1111), 6350);
    }

    #[test]
    fn a_stopped_charge_reports_no_current_however_the_register_reads() {
        // The conversion register keeps its last value when the charger
        // disconnects rather than falling to zero, so the status register is
        // what says whether that value still means anything.
        let not_charging = 0b0_00_000;
        assert_eq!(charge_current_ma(not_charging, 0b010_0000), 0);
    }

    #[test]
    fn a_finished_charge_still_reports_what_the_register_holds() {
        let charge_done = 0b0_11_000;
        assert_eq!(charge_current_ma(charge_done, 0b000_0010), 100);
    }

    #[test]
    fn the_reserved_top_bit_is_not_read_as_current() {
        let fast_charging = 0b0_10_000;
        assert_eq!(charge_current_ma(fast_charging, 0b1000_0001), 50);
    }

    // ── Initialisation register edits ────────────────────────────────────────

    #[test]
    fn asking_for_continuous_conversion_leaves_the_rest_of_the_register_alone() {
        // Reset value of the ADC control register: detection and limit bits
        // set, conversion bits clear.
        assert_eq!(with_adc_continuous(0b0001_1101), 0b1101_1101);
    }

    #[test]
    fn continuous_conversion_is_idempotent() {
        assert_eq!(with_adc_continuous(0b1101_1101), 0b1101_1101);
    }

    #[test]
    fn disabling_the_watchdog_leaves_termination_and_the_safety_timer_alone() {
        // Reset value: termination on, watchdog at 40 s, safety timer on,
        // fast charge timer 12 h, JEITA at 20 %.
        assert_eq!(with_watchdog_disabled(0b1001_1101), 0b1000_1101);
    }

    #[test]
    fn disabling_an_already_disabled_watchdog_changes_nothing() {
        assert_eq!(with_watchdog_disabled(0b1000_1101), 0b1000_1101);
    }

    // ── Fuel gauge ───────────────────────────────────────────────────────────

    #[test]
    fn a_gauge_reply_is_least_significant_byte_first() {
        assert_eq!(gauge_word([0x34, 0x12]), 0x1234);
    }

    #[test]
    fn the_gauge_reports_a_percentage_directly() {
        assert_eq!(gauge_percent(0), 0);
        assert_eq!(gauge_percent(57), 57);
        assert_eq!(gauge_percent(100), 100);
    }

    #[test]
    fn a_percentage_above_full_is_clamped() {
        assert_eq!(gauge_percent(101), 100);
        assert_eq!(gauge_percent(u16::MAX), 100);
    }

    #[test]
    fn the_gauge_reports_millivolts_directly() {
        assert_eq!(gauge_voltage_mv(gauge_word([0xC4, 0x0E])), 3780);
    }

    #[test]
    fn a_discharging_current_is_negative() {
        // Two's complement: 0xFF9C is -100 mA out of the battery.
        assert_eq!(gauge_current_ma(gauge_word([0x9C, 0xFF])), -100);
        assert_eq!(gauge_current_ma(0xFFFF), -1);
    }

    #[test]
    fn a_charging_current_is_positive() {
        assert_eq!(gauge_current_ma(gauge_word([0x64, 0x00])), 100);
    }

    #[test]
    fn the_full_signed_range_decodes() {
        assert_eq!(gauge_current_ma(0x7FFF), 32767);
        assert_eq!(gauge_current_ma(0x8000), -32768);
        assert_eq!(gauge_current_ma(0), 0);
    }

    // ── Bring-up, against parts on a fake bus ────────────────────────────────
    //
    // These drive the whole driver, so they need the fake bus, which stands in
    // for the I²C calls only where nothing real is behind them. A simulator
    // build routes those calls to the simulator's own virtual bus instead, and
    // there is no device model for these two chips there, so this half of the
    // suite does not apply to that build.

    #[cfg(not(feature = "sim-bus"))]
    mod bringup {
        use super::*;

        /// Part number 001, revision 0 — an SY6970 as it ships today.
        const SY6970_IDENTIFICATION: u8 = 0b0000_1000;
        /// Part number 000, revision 2 — a BQ25896 as it ships today.
        const BQ25896_IDENTIFICATION: u8 = 0b0000_0010;
        /// Part number 010, which neither datasheet claims.
        const STRANGER_IDENTIFICATION: u8 = 0b0001_0000;

        /// The charger's registers as a board would present them: a half-charged
        /// cell, fast charging at 500 mA, watchdog armed at its reset 40 seconds
        /// and the converter idle.
        const HALF_CHARGED: u8 = 76; // 3824 mV
        const FAST_CHARGING: u8 = 0b0_10_000;
        const TIMER_CONTROL_AT_RESET: u8 = 0b1001_1101;
        const ADC_CONTROL_AT_RESET: u8 = 0b0001_1101;

        /// What the discharge curve makes of `HALF_CHARGED`, which is what a board
        /// with no fuel gauge has to fall back on.
        const CURVE_PERCENT: u8 = 51;

        fn a_charger_at(address: u8, identification: u8) {
            esp_ffi::attach(address);
            esp_ffi::set(address, CHG_REG_IDENTIFICATION, identification);
            esp_ffi::set(address, CHG_REG_BATTERY_VOLTAGE, HALF_CHARGED);
            esp_ffi::set(address, CHG_REG_STATUS, FAST_CHARGING);
            esp_ffi::set(address, CHG_REG_CHARGE_CURRENT, 10); // 500 mA
            esp_ffi::set(address, CHG_REG_TIMER_CONTROL, TIMER_CONTROL_AT_RESET);
            esp_ffi::set(address, CHG_REG_ADC_CONTROL, ADC_CONTROL_AT_RESET);
        }

        fn a_gauge_at(address: u8, percent: u16) {
            esp_ffi::attach(address);
            esp_ffi::set_word(address, GAUGE_CMD_STATE_OF_CHARGE, percent);
            esp_ffi::set_word(address, GAUGE_CMD_VOLTAGE, 3780);
            esp_ffi::set_word(address, GAUGE_CMD_CURRENT, 0xFF9C); // -100 mA
        }

        /// An empty bus and a driver that has not been brought up, whatever the
        /// test before this one left behind.
        fn an_empty_board() {
            unsafe { power_deinit() };
            esp_ffi::clear();
            // The board profile's bus, which the fake ignores but the driver looks
            // for before it opens anything.
            if crate::hal_registry::hal_bus_get_i2c(0).is_null() {
                unsafe { crate::hal_registry::hal_bus_register_i2c(0, 1usize as *mut c_void) };
            }
        }

        /// Register the driver the way a board profile entry does.
        fn configured(config_json: &str) -> i32 {
            let json = std::ffi::CString::new(config_json).unwrap();
            unsafe { drv_power_sy6970_register(json.as_ptr()) }
        }

        fn info() -> HalPowerInfo {
            let mut info = HalPowerInfo {
                voltage_mv: 0,
                percent: 0,
                state: HalPowerState::NoBattery,
            };
            let ret = unsafe { power_get_info(&mut info) };
            assert_eq!(ret, ESP_OK, "get_info should succeed once the driver is up");
            info
        }

        #[test]
        fn a_board_carrying_an_sy6970_reports_its_battery() {
            an_empty_board();
            a_charger_at(ChargerVariant::Sy6970.address(), SY6970_IDENTIFICATION);
            a_gauge_at(GAUGE_DEFAULT_ADDRESS, 42);

            assert_eq!(configured("{\"i2c_bus\": 0}"), ESP_OK);
            assert_eq!(unsafe { power_init(std::ptr::null()) }, ESP_OK);

            let info = info();
            assert_eq!(info.voltage_mv, 3824);
            assert_eq!(info.percent, 42);
            assert_eq!(info.state, HalPowerState::Charging);
        }

        #[test]
        fn a_board_carrying_a_bq25896_reports_its_battery_the_same_way() {
            an_empty_board();
            a_charger_at(ChargerVariant::Bq25896.address(), BQ25896_IDENTIFICATION);
            a_gauge_at(GAUGE_DEFAULT_ADDRESS, 42);

            assert_eq!(configured("{\"i2c_bus\": 0}"), ESP_OK);
            assert_eq!(unsafe { power_init(std::ptr::null()) }, ESP_OK);

            let info = info();
            assert_eq!(info.voltage_mv, 3824);
            assert_eq!(info.percent, 42);
            assert_eq!(info.state, HalPowerState::Charging);
        }

        #[test]
        fn a_part_that_is_neither_charger_is_left_alone() {
            an_empty_board();
            // Something answers at both addresses, but says it is neither part.
            a_charger_at(ChargerVariant::Sy6970.address(), STRANGER_IDENTIFICATION);
            a_charger_at(ChargerVariant::Bq25896.address(), STRANGER_IDENTIFICATION);

            assert_eq!(configured("{\"i2c_bus\": 0}"), ESP_OK);
            assert_eq!(unsafe { power_init(std::ptr::null()) }, ESP_ERR_NOT_FOUND);
        }

        #[test]
        fn a_board_with_no_charger_on_the_bus_declines_to_come_up() {
            an_empty_board();
            a_gauge_at(GAUGE_DEFAULT_ADDRESS, 42);

            assert_eq!(configured("{\"i2c_bus\": 0}"), ESP_OK);
            assert_eq!(unsafe { power_init(std::ptr::null()) }, ESP_ERR_NOT_FOUND);
        }

        #[test]
        fn coming_up_disarms_the_watchdog_that_would_undo_the_configuration() {
            an_empty_board();
            let address = ChargerVariant::Sy6970.address();
            a_charger_at(address, SY6970_IDENTIFICATION);

            configured("{\"i2c_bus\": 0}");
            assert_eq!(unsafe { power_init(std::ptr::null()) }, ESP_OK);

            let timer_control = esp_ffi::get(address, CHG_REG_TIMER_CONTROL);
            assert_eq!(
                timer_control & WATCHDOG_BITS,
                0,
                "the watchdog should be off, or it resets the charger's configuration while idle"
            );
            assert_eq!(
                timer_control & !WATCHDOG_BITS,
                TIMER_CONTROL_AT_RESET & !WATCHDOG_BITS,
                "termination and the safety timer should be as the charger had them"
            );
        }

        #[test]
        fn coming_up_starts_the_converter_so_the_voltage_is_not_read_stale() {
            an_empty_board();
            let address = ChargerVariant::Sy6970.address();
            a_charger_at(address, SY6970_IDENTIFICATION);

            configured("{\"i2c_bus\": 0}");
            assert_eq!(unsafe { power_init(std::ptr::null()) }, ESP_OK);

            let adc_control = esp_ffi::get(address, CHG_REG_ADC_CONTROL);
            assert_eq!(
                adc_control & ADC_CONTINUOUS_BITS,
                ADC_CONTINUOUS_BITS,
                "the converter should be running every second, not waiting to be asked"
            );
        }

        #[test]
        fn the_percentage_comes_from_the_gauge_rather_than_the_voltage() {
            an_empty_board();
            a_charger_at(ChargerVariant::Sy6970.address(), SY6970_IDENTIFICATION);
            // A number the discharge curve would never produce for this voltage.
            a_gauge_at(GAUGE_DEFAULT_ADDRESS, 88);

            configured("{\"i2c_bus\": 0}");
            assert_eq!(unsafe { power_init(std::ptr::null()) }, ESP_OK);

            assert_ne!(CURVE_PERCENT, 88, "the test would prove nothing otherwise");
            assert_eq!(unsafe { power_get_battery_percent() }, 88);
        }

        #[test]
        fn without_a_gauge_the_percentage_falls_back_to_the_discharge_curve() {
            an_empty_board();
            a_charger_at(ChargerVariant::Sy6970.address(), SY6970_IDENTIFICATION);

            configured("{\"i2c_bus\": 0}");
            assert_eq!(unsafe { power_init(std::ptr::null()) }, ESP_OK);

            assert_eq!(unsafe { power_get_battery_percent() }, CURVE_PERCENT);
        }

        #[test]
        fn a_board_profile_may_move_the_gauge_to_another_address() {
            an_empty_board();
            a_charger_at(ChargerVariant::Sy6970.address(), SY6970_IDENTIFICATION);
            a_gauge_at(0x56, 77);

            configured("{\"i2c_bus\": 0, \"gauge_i2c_addr\": \"0x56\"}");
            assert_eq!(unsafe { power_init(std::ptr::null()) }, ESP_OK);

            assert_eq!(unsafe { power_get_battery_percent() }, 77);
        }

        #[test]
        fn charging_is_reported_from_the_charger_not_from_the_gauge() {
            an_empty_board();
            let address = ChargerVariant::Sy6970.address();
            a_charger_at(address, SY6970_IDENTIFICATION);
            a_gauge_at(GAUGE_DEFAULT_ADDRESS, 42);

            configured("{\"i2c_bus\": 0}");
            assert_eq!(unsafe { power_init(std::ptr::null()) }, ESP_OK);
            assert!(unsafe { power_is_charging() });

            esp_ffi::set(address, CHG_REG_STATUS, 0b0_11_000); // charge done
            assert!(!unsafe { power_is_charging() });
            assert_eq!(info().state, HalPowerState::Charged);

            esp_ffi::set(address, CHG_REG_STATUS, 0b0_00_000); // unplugged
            assert!(!unsafe { power_is_charging() });
            assert_eq!(info().state, HalPowerState::Discharging);
        }

        #[test]
        fn a_charger_with_no_cell_to_measure_reports_no_battery() {
            an_empty_board();
            let address = ChargerVariant::Sy6970.address();
            a_charger_at(address, SY6970_IDENTIFICATION);
            esp_ffi::set(address, CHG_REG_BATTERY_VOLTAGE, 0);

            configured("{\"i2c_bus\": 0}");
            assert_eq!(unsafe { power_init(std::ptr::null()) }, ESP_OK);

            assert_eq!(info().state, HalPowerState::NoBattery);
        }

        #[test]
        fn readings_before_the_driver_is_up_report_no_state_rather_than_zero() {
            an_empty_board();
            let mut info = HalPowerInfo {
                voltage_mv: 0,
                percent: 0,
                state: HalPowerState::NoBattery,
            };
            assert_eq!(
                unsafe { power_get_info(&mut info) },
                ESP_ERR_INVALID_STATE,
                "a driver that never found its charger should say so"
            );
        }

        #[test]
        fn the_charger_is_identified_by_what_it_says_not_by_where_it_answered() {
            an_empty_board();
            // A part answering at one variant's address but identifying as the
            // other. Implausible on a shipped board, and the point: the identity
            // comes from the register, while the address is only where the driver
            // happened to find it — which is what the bring-up log has to report.
            a_charger_at(ChargerVariant::Sy6970.address(), BQ25896_IDENTIFICATION);

            let bus = crate::hal_registry::hal_bus_get_i2c(0);
            let charger = attach_charger(bus).expect("something answered");

            assert_eq!(charger.variant, ChargerVariant::Bq25896);
            assert_eq!(charger.address, ChargerVariant::Sy6970.address());
            close(charger.dev);
        }

        #[test]
        fn bringing_the_driver_up_twice_changes_nothing() {
            an_empty_board();
            a_charger_at(ChargerVariant::Sy6970.address(), SY6970_IDENTIFICATION);
            a_gauge_at(GAUGE_DEFAULT_ADDRESS, 42);

            configured("{\"i2c_bus\": 0}");
            assert_eq!(unsafe { power_init(std::ptr::null()) }, ESP_OK);
            assert_eq!(unsafe { power_init(std::ptr::null()) }, ESP_OK);
            assert_eq!(info().percent, 42);
        }

        #[test]
        fn the_vtable_is_complete() {
            let driver = &POWER_DRIVER;
            assert!(driver.init.is_some());
            assert!(driver.deinit.is_some());
            assert!(driver.get_info.is_some());
            assert!(driver.get_battery_mv.is_some());
            assert!(driver.get_battery_percent.is_some());
            assert!(driver.is_charging.is_some());
            assert!(driver.sleep.is_some());
            let name = unsafe { std::ffi::CStr::from_ptr(driver.name) };
            assert_eq!(name.to_str().unwrap(), "SY6970/BQ25896");
        }
    } // mod bringup

    // ── Configuration ────────────────────────────────────────────────────────

    #[test]
    fn a_driver_entry_with_no_configuration_takes_the_defaults() {
        let config = parse_config("{}");
        assert_eq!(config.i2c_bus, 0);
        assert_eq!(config.gauge_address, GAUGE_DEFAULT_ADDRESS);
    }

    #[test]
    fn a_gauge_address_is_read_whether_it_is_quoted_hex_or_bare() {
        assert_eq!(
            parse_config("{\"gauge_i2c_addr\": \"0x55\"}").gauge_address,
            0x55
        );
        assert_eq!(parse_config("{\"gauge_i2c_addr\": 85}").gauge_address, 0x55);
    }

    #[test]
    fn an_unreadable_gauge_address_falls_back_to_the_default() {
        assert_eq!(
            parse_config("{\"gauge_i2c_addr\": \"nonsense\"}").gauge_address,
            GAUGE_DEFAULT_ADDRESS
        );
        assert_eq!(
            parse_config("{\"gauge_i2c_addr\": 4096}").gauge_address,
            GAUGE_DEFAULT_ADDRESS
        );
    }
}
