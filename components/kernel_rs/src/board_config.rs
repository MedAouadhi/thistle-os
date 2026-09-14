// SPDX-License-Identifier: BSD-3-Clause
// Board config — reads board.json from SPIFFS, inits buses, loads drivers

use std::ffi::{CStr, CString};
use std::fs;
use std::os::raw::c_char;
use std::path::Path;
use std::sync::Mutex;

const ESP_OK: i32 = 0;
const ESP_ERR_NOT_FOUND: i32 = 0x105;
const ESP_ERR_INVALID_SIZE: i32 = 0x104;
const ESP_ERR_NO_MEM: i32 = 0x101;
const ESP_ERR_INVALID_ARG: i32 = 0x102;

const MAX_CONFIG_SIZE: usize = 8192;

static BOARD_NAME: Mutex<[u8; 64]> = Mutex::new([0u8; 64]);

// ESP-IDF FFI for bus init and driver loading
extern "C" {
    fn hal_set_board_name(name: *const c_char) -> i32;
    fn driver_loader_init() -> i32;
    fn driver_loader_load_with_config(path: *const c_char, config: *const c_char) -> i32;
    fn board_init() -> i32;
    fn board_detect_and_write() -> i32;
}

// Bus init + builtin driver init — only on real hardware (not in unit tests)
#[cfg(not(test))]
extern "C" {
    fn board_bus_init_spi(
        host: i32,
        mosi: i32,
        miso: i32,
        sclk: i32,
        max_transfer_bytes: i32,
    ) -> i32;
    fn board_bus_init_i2c(port: i32, sda: i32, scl: i32, freq_hz: i32) -> i32;
    fn board_delay_ms(ms: i32);
    fn board_builtin_driver_init(
        id: *const c_char,
        hal_type: *const c_char,
        config_json: *const c_char,
    ) -> i32;
}

// Reuse manifest JSON helpers
use crate::manifest::{json_get_int, json_get_string};

fn set_board_name(name: &str) {
    if let Ok(mut buf) = BOARD_NAME.lock() {
        let bytes = name.as_bytes();
        let len = bytes.len().min(63);
        buf[..len].copy_from_slice(&bytes[..len]);
        buf[len] = 0;
    }
}

// ── Board profile boot sequence ─────────────────────────────────────
//
// The sequence a board profile describes — board-level GPIO outputs, then
// buses, then IO expanders, then their pins, then drivers — runs through one
// function generic over its side effects. Production supplies the real
// implementations; tests supply a recorder and assert the order phases ran in.

// Every board profile array is bounded, so a malformed profile cannot spin.
const MAX_GPIO_OUTPUTS: usize = 16;
const MAX_SPI_BUSES: usize = 4;
const MAX_I2C_BUSES: usize = 4;
const MAX_IO_EXPANDERS: usize = 4;
const MAX_EXPANDER_OUTPUTS: usize = 16;
const MAX_DRIVERS: usize = 16;

/// Expander pins are numbered `0..MAX_EXPANDER_PINS`. Sixteen is the width of
/// the PCA9535-compatible family the kernel supports, the XL9555 included.
const MAX_EXPANDER_PINS: i32 = 16;

/// An SPI bus declaration from a board profile.
#[derive(Debug, PartialEq, Eq)]
struct SpiBus {
    host: i32,
    mosi: i32,
    miso: i32,
    sclk: i32,
    max_transfer_bytes: i32,
}

/// An I²C bus declaration from a board profile.
#[derive(Debug, PartialEq, Eq)]
struct I2cBus {
    port: i32,
    sda: i32,
    scl: i32,
    freq_hz: i32,
}

/// A board-level GPIO output declaration from a board profile.
#[derive(Debug, PartialEq, Eq)]
struct GpioOutput {
    gpio: i32,
    level: i32,
    delay_ms: i32,
}

/// An IO expander declaration from a board profile.
#[derive(Debug, PartialEq, Eq)]
struct IoExpander {
    /// The driver that speaks to this part.
    id: String,
    /// Bus index into the profile's declared I²C buses.
    i2c_bus: i32,
    i2c_addr: i32,
}

/// An expander pin a board profile drives, and the level it drives it at.
#[derive(Debug, PartialEq, Eq)]
struct ExpanderOutput {
    /// Index into the profile's `io_expanders` array — never a GPIO number.
    expander: usize,
    pin: i32,
    level: i32,
    delay_ms: i32,
}

/// A driver's declaration inside a board profile, parsed and ready to load.
#[derive(Debug, Clone, PartialEq, Eq)]
struct DriverEntry {
    id: String,
    /// The HAL interface the driver registers.
    hal: String,
    /// The driver's ELF filename.
    elf: String,
    /// The driver's part-specific configuration, as JSON.
    config: String,
}

/// The side effects the board profile boot sequence performs.
///
/// These are the raw operations only. Which ELF a driver entry resolves to,
/// and whether it falls back to a compiled-in driver, is sequence policy and
/// lives above this seam so that a test can observe it.
///
/// Only a bus or an expander that cannot be brought up stops the sequence: a
/// GPIO output or a driver that fails must not take the rest of the board with
/// it, while a bus that is unavailable takes every driver configured on it,
/// and an expander that is unreachable takes the peripheral rails behind it.
trait BootEffects {
    fn set_gpio_output(&mut self, output: &GpioOutput);
    fn init_spi_bus(&mut self, bus: &SpiBus) -> i32;
    fn init_i2c_bus(&mut self, bus: &I2cBus) -> i32;
    /// Bring up a declared IO expander. Expanders are handed over in
    /// declaration order, so the nth successful call owns expander index n.
    fn init_io_expander(&mut self, expander: &IoExpander) -> i32;
    /// Configure an expander pin as an output, before anything drives it.
    fn configure_expander_output(&mut self, output: &ExpanderOutput) -> i32;
    fn set_expander_level(&mut self, output: &ExpanderOutput) -> i32;
    /// Wait, for a settling delay a board profile declares.
    fn delay(&mut self, ms: i32);
    fn init_driver_loader(&mut self);
    /// Whether a driver ELF is present at `path`.
    fn driver_elf_exists(&mut self, path: &str) -> bool;
    /// Load the driver ELF at `path`, which `driver_elf_exists` reported present.
    fn load_driver_elf(&mut self, path: &str, driver: &DriverEntry);
    /// Load the compiled-in driver for an entry whose ELF is not on disk.
    fn load_builtin_driver(&mut self, driver: &DriverEntry);
}

/// Run the boot sequence a board profile describes.
///
/// Returns the bus error that aborted the sequence, or `ESP_OK`.
fn run_boot_sequence<E: BootEffects>(profile: &str, effects: &mut E) -> i32 {
    apply_gpio_outputs(profile, effects);

    let ret = initialize_buses(profile, effects);
    if ret != ESP_OK {
        // Do not load or start drivers whose configured bus is unavailable.
        return ret;
    }

    // Expanders sit on a bus, so they come up once the buses do. Their pins
    // carry the peripheral rails drivers probe for, so they are driven before
    // any driver loads.
    let expanders = match initialize_io_expanders(profile, effects) {
        Ok(addressable) => addressable,
        Err(ret) => return ret,
    };
    apply_expander_outputs(profile, effects, &expanders);

    load_drivers(profile, effects);

    ESP_OK
}

/// Drive board-level GPIO outputs (power enables, resets) before bus init.
fn apply_gpio_outputs<E: BootEffects>(profile: &str, effects: &mut E) {
    for entry in objects_in(profile, "gpio_outputs", MAX_GPIO_OUTPUTS) {
        let output = GpioOutput {
            gpio: json_get_int(&entry, "gpio").unwrap_or(-1) as i32,
            level: json_get_int(&entry, "level").unwrap_or(0) as i32,
            delay_ms: json_get_int(&entry, "delay_ms").unwrap_or(0) as i32,
        };
        if output.gpio >= 0 {
            effects.set_gpio_output(&output);
        }
    }
}

fn initialize_buses<E: BootEffects>(profile: &str, effects: &mut E) -> i32 {
    let Some(buses) = extract_object(profile, "buses") else {
        return ESP_OK;
    };

    for bus in objects_in(&buses, "spi", MAX_SPI_BUSES) {
        let ret = effects.init_spi_bus(&SpiBus {
            host: json_get_int(&bus, "host").unwrap_or(1) as i32,
            mosi: json_get_int(&bus, "mosi").unwrap_or(-1) as i32,
            miso: json_get_int(&bus, "miso").unwrap_or(-1) as i32,
            sclk: json_get_int(&bus, "sclk").unwrap_or(-1) as i32,
            max_transfer_bytes: json_get_int(&bus, "max_transfer_bytes").unwrap_or(4096) as i32,
        });
        if ret != ESP_OK {
            return ret;
        }
    }

    for bus in objects_in(&buses, "i2c", MAX_I2C_BUSES) {
        let ret = effects.init_i2c_bus(&I2cBus {
            port: json_get_int(&bus, "port").unwrap_or(0) as i32,
            sda: json_get_int(&bus, "sda").unwrap_or(-1) as i32,
            scl: json_get_int(&bus, "scl").unwrap_or(-1) as i32,
            freq_hz: json_get_int(&bus, "freq_hz").unwrap_or(400000) as i32,
        });
        if ret != ESP_OK {
            return ret;
        }
    }

    ESP_OK
}

/// Bring up the IO expanders a board profile declares, in declaration order.
///
/// Returns one flag per declaration saying whether that expander is
/// addressable, which is what an output entry's expander index is checked
/// against. A declaration the kernel cannot make sense of still consumes its
/// index: dropping it would shift the indices of those after it, silently
/// moving every pin that follows onto the wrong part.
fn initialize_io_expanders<E: BootEffects>(
    profile: &str,
    effects: &mut E,
) -> Result<Vec<bool>, i32> {
    let mut addressable = Vec::new();
    for entry in objects_in(profile, "io_expanders", MAX_IO_EXPANDERS) {
        let expander = IoExpander {
            id: json_get_string(&entry, "id").unwrap_or_default(),
            i2c_bus: json_get_int(&entry, "i2c_bus").unwrap_or(0) as i32,
            i2c_addr: json_get_address(&entry, "i2c_addr").unwrap_or(-1),
        };
        if expander.id.is_empty() || expander.i2c_addr < 0 || expander.i2c_bus < 0 {
            addressable.push(false);
            continue;
        }

        // A declared expander that cannot be reached is a different matter:
        // the rails behind it stay down, so drivers must not go looking for
        // the peripherals they feed.
        let ret = effects.init_io_expander(&expander);
        if ret != ESP_OK {
            return Err(ret);
        }
        addressable.push(true);
    }
    Ok(addressable)
}

/// Drive the expander pins a board profile declares, in declaration order.
///
/// A pin is configured as an output before it is driven, because the XL9555's
/// configuration registers reset to all-inputs: a pin given a level but left
/// an input floats, and the rail behind it stays off. A pin that could not be
/// configured is not then driven, so the rest of the board still comes up but
/// nothing pretends that rail is live.
fn apply_expander_outputs<E: BootEffects>(profile: &str, effects: &mut E, expanders: &[bool]) {
    for entry in objects_in(profile, "expander_outputs", MAX_EXPANDER_OUTPUTS) {
        let Some(output) = parse_expander_output(&entry, expanders) else {
            continue;
        };

        if effects.configure_expander_output(&output) != ESP_OK {
            continue;
        }
        if effects.set_expander_level(&output) != ESP_OK {
            continue;
        }
        if output.delay_ms > 0 {
            effects.delay(output.delay_ms);
        }
    }
}

/// Read one `expander_outputs` entry, or `None` if the kernel cannot address
/// the pin it names. Rejecting an entry leaves the rest of the boot alone —
/// unlike a bad expander declaration, a bad pin entry moves nothing else.
fn parse_expander_output(entry: &str, expanders: &[bool]) -> Option<ExpanderOutput> {
    // Direction is required rather than inferred: an entry that sets a level
    // without setting direction would leave the pin floating and its rail off.
    if json_get_string(entry, "direction")? != "output" {
        return None;
    }

    let expander = json_get_int(entry, "expander").unwrap_or(0) as i32;
    if expander < 0 || !expanders.get(expander as usize).copied().unwrap_or(false) {
        return None;
    }

    let pin = json_get_int(entry, "pin")? as i32;
    if !(0..MAX_EXPANDER_PINS).contains(&pin) {
        return None;
    }

    Some(ExpanderOutput {
        expander: expander as usize,
        pin,
        level: json_get_int(entry, "level").unwrap_or(0) as i32,
        delay_ms: json_get_int(entry, "delay_ms").unwrap_or(0) as i32,
    })
}

/// Read an I²C address, which board profiles write either as a bare integer or
/// as the quoted hex string driver entries use (`"0x20"`).
fn json_get_address(json: &str, key: &str) -> Option<i32> {
    let Some(text) = json_get_string(json, key) else {
        return json_get_int(json, key).map(|value| value as i32);
    };
    let text = text.trim();
    match text.strip_prefix("0x").or_else(|| text.strip_prefix("0X")) {
        Some(hex) => i32::from_str_radix(hex, 16).ok(),
        None => text.parse().ok(),
    }
}

fn load_drivers<E: BootEffects>(profile: &str, effects: &mut E) {
    effects.init_driver_loader();

    let board_arch = extract_object(profile, "board")
        .and_then(|board| json_get_string(&board, "arch"))
        .unwrap_or_default();

    for drv in objects_in(profile, "drivers", MAX_DRIVERS) {
        let elf = json_get_string(&drv, "entry").unwrap_or_default();
        if elf.is_empty() || !is_safe_elf_filename(&elf) {
            continue;
        }

        let id = json_get_string(&drv, "id").unwrap_or_default();
        let raw_config = extract_object(&drv, "config").unwrap_or_else(|| "{}".to_string());
        let config = normalize_driver_config(&id, &board_arch, &raw_config);

        let driver = DriverEntry {
            id,
            hal: json_get_string(&drv, "hal").unwrap_or_default(),
            elf,
            config,
        };

        match driver_elf_search_paths(&driver.elf)
            .into_iter()
            .find(|path| effects.driver_elf_exists(path))
        {
            Some(path) => effects.load_driver_elf(&path, &driver),
            // ELF not on disk — fall back to the compiled-in driver, which
            // needs both an id and a HAL interface to be identifiable.
            None => {
                if !driver.id.is_empty() && !driver.hal.is_empty() {
                    effects.load_builtin_driver(&driver);
                }
            }
        }
    }
}

/// Where a driver ELF is looked for, in order: SPIFFS first, then SD card.
fn driver_elf_search_paths(elf: &str) -> [String; 2] {
    [
        format!("/spiffs/drivers/{}", elf),
        format!("/tmp/thistle_sdcard/drivers/{}", elf),
    ]
}

/// Reject traversal, absolute and hidden paths — a driver entry's ELF filename
/// names a file inside the drivers directory, nothing else.
fn is_safe_elf_filename(elf: &str) -> bool {
    !elf.contains("..") && !elf.contains('/') && !elf.contains('\\') && !elf.starts_with('.')
}

/// The objects of a board profile array, up to the bound the schema allows.
/// A missing array, or one holding fewer objects than the bound, simply ends.
fn objects_in(json: &str, key: &str, max: usize) -> impl Iterator<Item = String> {
    let array = find_array(json, key);
    (0..max).map_while(move |i| nth_object(array.as_deref()?, i))
}

/// The boot sequence's effects as performed on a real board.
#[cfg(not(test))]
struct HardwareEffects;

#[cfg(not(test))]
impl HardwareEffects {
    /// Start from a board that has nothing attached, so a second run of the
    /// sequence re-numbers the expanders from zero rather than appending to
    /// the previous run's table.
    fn new() -> Self {
        crate::drv_ioexp_xl9555::detach_all();
        HardwareEffects
    }
}

#[cfg(not(test))]
impl BootEffects for HardwareEffects {
    fn set_gpio_output(&mut self, output: &GpioOutput) {
        extern "C" {
            fn board_gpio_set_output(pin: i32, level: i32, delay_ms: i32) -> i32;
        }
        unsafe {
            board_gpio_set_output(output.gpio, output.level, output.delay_ms);
        }
    }

    fn init_spi_bus(&mut self, bus: &SpiBus) -> i32 {
        unsafe {
            board_bus_init_spi(
                bus.host,
                bus.mosi,
                bus.miso,
                bus.sclk,
                bus.max_transfer_bytes,
            )
        }
    }

    fn init_i2c_bus(&mut self, bus: &I2cBus) -> i32 {
        unsafe { board_bus_init_i2c(bus.port, bus.sda, bus.scl, bus.freq_hz) }
    }

    fn init_io_expander(&mut self, expander: &IoExpander) -> i32 {
        match expander.id.as_str() {
            "com.thistle.drv.ioexp-xl9555" => {
                crate::drv_ioexp_xl9555::attach(expander.i2c_bus, expander.i2c_addr as u8)
            }
            // An expander the kernel has no driver for leaves its rails down,
            // so it stops the boot rather than letting drivers probe for
            // peripherals that were never powered.
            _ => ESP_ERR_NOT_FOUND,
        }
    }

    fn configure_expander_output(&mut self, output: &ExpanderOutput) -> i32 {
        crate::drv_ioexp_xl9555::set_as_output(output.expander, output.pin as u8)
    }

    fn set_expander_level(&mut self, output: &ExpanderOutput) -> i32 {
        crate::drv_ioexp_xl9555::set_level(output.expander, output.pin as u8, output.level != 0)
    }

    fn delay(&mut self, ms: i32) {
        unsafe {
            board_delay_ms(ms);
        }
    }

    fn init_driver_loader(&mut self) {
        unsafe {
            driver_loader_init();
        }
    }

    fn driver_elf_exists(&mut self, path: &str) -> bool {
        Path::new(path).exists()
    }

    fn load_driver_elf(&mut self, path: &str, driver: &DriverEntry) {
        if let (Ok(cpath), Ok(cconfig)) = (CString::new(path), CString::new(driver.config.as_str()))
        {
            unsafe {
                driver_loader_load_with_config(cpath.as_ptr(), cconfig.as_ptr());
            }
        }
    }

    fn load_builtin_driver(&mut self, driver: &DriverEntry) {
        if let (Ok(cid), Ok(chal), Ok(cconfig)) = (
            CString::new(driver.id.as_str()),
            CString::new(driver.hal.as_str()),
            CString::new(driver.config.as_str()),
        ) {
            unsafe {
                board_builtin_driver_init(cid.as_ptr(), chal.as_ptr(), cconfig.as_ptr());
            }
        }
    }
}

// This function does the main work but requires ESP-IDF APIs for bus init.
// On the simulator, it falls back to compiled board_init().
fn load_config(config_path: &str) -> i32 {
    let mut json = match fs::read_to_string(config_path) {
        Ok(s) => s,
        Err(_) => {
            // No board.json — try I2C auto-detection to create one
            let detect_ret = unsafe { board_detect_and_write() };
            if detect_ret == ESP_OK {
                // Detection wrote board.json to SPIFFS — retry loading it
                match fs::read_to_string(config_path) {
                    Ok(s) => s,
                    Err(_) => {
                        // Detection succeeded but file still unreadable — safe mode
                        let ret = unsafe { board_init() };
                        if ret != ESP_OK {
                            return ret;
                        }
                        unsafe {
                            crate::driver_manager::driver_manager_start_all();
                        }
                        return ESP_OK;
                    }
                }
            } else {
                // Detection not available or failed — fall back to compiled board_init()
                let ret = unsafe { board_init() };
                if ret != ESP_OK {
                    return ret;
                }
                unsafe {
                    crate::driver_manager::driver_manager_start_all();
                }
                return ESP_OK;
            }
        }
    };

    if json.len() > MAX_CONFIG_SIZE {
        return ESP_ERR_INVALID_SIZE;
    }

    // If the cached board.json is missing the buses section (written by an older
    // version of the firmware), force re-detection to get a fresh copy.
    if extract_object(&json, "buses").is_none() {
        let _ = unsafe { board_detect_and_write() };
        if let Ok(fresh) = fs::read_to_string(config_path) {
            if fresh.len() <= MAX_CONFIG_SIZE {
                json = fresh;
            }
        }
    }

    // Parse board metadata
    if let Some(board_section) = extract_object(&json, "board") {
        if let Some(name) = json_get_string(&board_section, "name") {
            set_board_name(&name);
            if let Ok(cname) = CString::new(name.as_str()) {
                unsafe {
                    hal_set_board_name(cname.as_ptr());
                }
            }
        }
    }

    // Disable deep sleep GPIO hold before configuring any pins.
    // Required when recovering from deep sleep — otherwise GPIO levels set
    // during sleep persist and prevent normal output configuration.
    // (Mirrors board_tdeck_pro.c::gpio_deep_sleep_hold_dis() on boot.)
    #[cfg(all(not(test), not(feature = "esp32c6")))]
    {
        extern "C" {
            fn gpio_deep_sleep_hold_dis();
        }
        unsafe {
            gpio_deep_sleep_hold_dis();
        }
    }

    // Run the sequence the board profile describes: GPIO outputs, buses,
    // IO expanders, expander pins, drivers.
    // Gated to hardware builds because every effect here is an ESP-IDF call;
    // the sequence itself is covered by tests that inject their own effects.
    #[cfg(not(test))]
    {
        let ret = run_boot_sequence(&json, &mut HardwareEffects::new());
        if ret != ESP_OK {
            return ret;
        }
    }

    // Start all registered drivers (display, inputs, radio, GPS, audio, power, IMU, storage)
    unsafe {
        crate::driver_manager::driver_manager_start_all();
    }

    ESP_OK
}

fn normalize_driver_config(driver_id: &str, board_arch: &str, config: &str) -> String {
    if matches!(
        driver_id,
        "com.thistle.drv.power-tp4065b" | "com.thistle.drv.power-heltec-v3"
    ) {
        return normalize_tp4065b_power_config(board_arch, config);
    }
    config.to_string()
}

fn normalize_tp4065b_power_config(board_arch: &str, config: &str) -> String {
    if json_get_int(config, "adc_channel").is_some()
        && json_get_int(config, "pin_charge_status").is_some()
    {
        return config.to_string();
    }

    let adc_pin = match json_get_int(config, "adc_pin") {
        Some(pin) => pin as i32,
        None => return config.to_string(),
    };
    let charge_pin = json_get_int(config, "charge_pin").unwrap_or(-1) as i32;

    let adc_channel = match adc_pin_to_channel(board_arch, adc_pin) {
        Some(channel) => channel,
        None => return config.to_string(),
    };

    append_json_fields(
        config,
        &format!(
            "\"adc_channel\": {}, \"pin_charge_status\": {}",
            adc_channel, charge_pin
        ),
    )
}

fn adc_pin_to_channel(board_arch: &str, adc_pin: i32) -> Option<i32> {
    match board_arch {
        // ESP32-S3 ADC1 channels map directly: GPIO1..GPIO10 -> channel 0..9.
        "esp32s3" if (1..=10).contains(&adc_pin) => Some(adc_pin - 1),
        _ => None,
    }
}

fn append_json_fields(config: &str, fields: &str) -> String {
    let trimmed = config.trim();
    if !trimmed.ends_with('}') {
        return config.to_string();
    }

    let insert_at = trimmed.len() - 1;
    let needs_comma = !trimmed[..insert_at].trim_end().ends_with('{');
    let separator = if needs_comma { ", " } else { "" };
    format!(
        "{}{}{}{}",
        &trimmed[..insert_at],
        separator,
        fields,
        &trimmed[insert_at..]
    )
}

// Simple JSON array/object extraction helpers
fn find_array(json: &str, key: &str) -> Option<String> {
    let pattern = format!("\"{}\"", key);
    let start = json.find(&pattern)?;
    let after = &json[start + pattern.len()..];
    let after = after.trim_start().strip_prefix(':')?;
    let trimmed = after.trim_start();
    if !trimmed.starts_with('[') {
        return None;
    }
    let mut depth = 0;
    for (i, ch) in trimmed.char_indices() {
        match ch {
            '[' => depth += 1,
            ']' => {
                depth -= 1;
                if depth == 0 {
                    return Some(trimmed[..=i].to_string());
                }
            }
            _ => {}
        }
    }
    None
}

pub fn extract_object(json: &str, key: &str) -> Option<String> {
    let pattern = format!("\"{}\"", key);
    let start = json.find(&pattern)?;
    let after = &json[start + pattern.len()..];
    let after = after.trim_start().strip_prefix(':')?;
    let trimmed = after.trim_start();
    if !trimmed.starts_with('{') {
        return None;
    }
    let mut depth = 0;
    for (i, ch) in trimmed.char_indices() {
        match ch {
            '{' => depth += 1,
            '}' => {
                depth -= 1;
                if depth == 0 {
                    return Some(trimmed[..=i].to_string());
                }
            }
            _ => {}
        }
    }
    None
}

fn nth_object(array_str: &str, n: usize) -> Option<String> {
    let inner = array_str.trim().strip_prefix('[')?.strip_suffix(']')?;
    let mut depth = 0;
    let mut count = 0;
    let mut obj_start = None;
    for (i, ch) in inner.char_indices() {
        match ch {
            '{' => {
                if depth == 0 {
                    if count == n {
                        obj_start = Some(i);
                    }
                }
                depth += 1;
            }
            '}' => {
                depth -= 1;
                if depth == 0 {
                    if count == n {
                        return Some(inner[obj_start?..=i].to_string());
                    }
                    count += 1;
                }
            }
            _ => {}
        }
    }
    None
}

// FFI exports

#[no_mangle]
pub unsafe extern "C" fn board_config_init(config_path: *const c_char) -> i32 {
    let path = if config_path.is_null() {
        "/spiffs/config/board.json".to_string()
    } else {
        match CStr::from_ptr(config_path).to_str() {
            Ok(s) => s.to_string(),
            Err(_) => return ESP_ERR_INVALID_ARG,
        }
    };

    load_config(&path)
}

#[no_mangle]
pub extern "C" fn board_config_get_name() -> *const c_char {
    match BOARD_NAME.lock() {
        Ok(buf) => buf.as_ptr() as *const c_char,
        Err(_) => b"Unknown\0".as_ptr() as *const c_char,
    }
}

// ── Window manager preference from system.json ──────────────────────

static WM_NAME: Mutex<[u8; 64]> = Mutex::new([0u8; 64]);

/// Read the "window_manager" value from system.json.
/// Tries /spiffs/config/system.json first, then SD card.
fn load_wm_name() {
    let paths = [
        "/spiffs/config/system.json",
        "/tmp/thistle_sdcard/config/system.json",
    ];
    for path in &paths {
        if let Ok(json) = fs::read_to_string(path) {
            // Look inside "thistle_os" object for "window_manager"
            if let Some(os_obj) = extract_object(&json, "thistle_os") {
                if let Some(wm) = json_get_string(&os_obj, "window_manager") {
                    if let Ok(mut buf) = WM_NAME.lock() {
                        let bytes = wm.as_bytes();
                        let len = bytes.len().min(63);
                        buf[..len].copy_from_slice(&bytes[..len]);
                        buf[len] = 0;
                    }
                    return;
                }
            }
        }
    }
}

/// Return the configured WM name, or NULL if not set.
/// Reads system.json on first call and caches the result.
#[no_mangle]
pub extern "C" fn board_config_get_wm_name() -> *const c_char {
    // Load lazily on first call
    {
        let buf = WM_NAME.lock().unwrap();
        if buf[0] == 0 {
            drop(buf);
            load_wm_name();
        }
    }
    let buf = WM_NAME.lock().unwrap();
    if buf[0] == 0 {
        return std::ptr::null();
    }
    buf.as_ptr() as *const c_char
}

#[cfg(test)]
mod tests {
    use super::*;

    /// One side effect the boot sequence performed, in the order it happened.
    #[derive(Debug, PartialEq, Eq)]
    enum BootStep {
        Gpio {
            gpio: i32,
            level: i32,
            delay_ms: i32,
        },
        ExpanderInit {
            id: String,
            i2c_bus: i32,
            i2c_addr: i32,
        },
        ExpanderConfigured {
            expander: usize,
            pin: i32,
        },
        ExpanderLevel {
            expander: usize,
            pin: i32,
            level: i32,
        },
        Delay {
            ms: i32,
        },
        Spi {
            host: i32,
            mosi: i32,
            miso: i32,
            sclk: i32,
            max_transfer_bytes: i32,
        },
        I2c {
            port: i32,
            sda: i32,
            scl: i32,
            freq_hz: i32,
        },
        DriverLoaderInit,
        ElfProbe {
            path: String,
        },
        DriverElf {
            id: String,
            path: String,
        },
        BuiltinDriver {
            id: String,
        },
    }

    /// Records every effect the boot sequence performs. A chosen bus can be
    /// made to fail, and chosen driver ELFs can be made to exist on disk.
    #[derive(Default)]
    struct Recorder {
        steps: Vec<BootStep>,
        fail_spi_host: Option<(i32, i32)>,
        fail_i2c_port: Option<(i32, i32)>,
        fail_expander_addr: Option<(i32, i32)>,
        /// An expander pin that refuses to be configured as an output.
        unconfigurable_pin: Option<(usize, i32)>,
        present_elfs: Vec<String>,
        /// Every driver entry handed to an effect, whatever it resolved to.
        loaded: Vec<DriverEntry>,
    }

    impl Recorder {
        fn failing_spi(host: i32, err: i32) -> Self {
            Recorder {
                fail_spi_host: Some((host, err)),
                ..Default::default()
            }
        }

        fn failing_i2c(port: i32, err: i32) -> Self {
            Recorder {
                fail_i2c_port: Some((port, err)),
                ..Default::default()
            }
        }

        /// A recorder for which the expander at this address cannot be reached.
        fn failing_expander(i2c_addr: i32, err: i32) -> Self {
            Recorder {
                fail_expander_addr: Some((i2c_addr, err)),
                ..Default::default()
            }
        }

        /// A recorder for which this expander pin refuses to be configured.
        fn failing_pin(expander: usize, pin: i32) -> Self {
            Recorder {
                unconfigurable_pin: Some((expander, pin)),
                ..Default::default()
            }
        }

        /// A recorder for which these driver ELF paths are present on disk.
        fn with_elfs_on_disk(paths: &[&str]) -> Self {
            Recorder {
                present_elfs: paths.iter().map(|p| p.to_string()).collect(),
                ..Default::default()
            }
        }

        fn driver_ids(&self) -> Vec<&str> {
            self.steps
                .iter()
                .filter_map(|step| match step {
                    BootStep::DriverElf { id, .. } | BootStep::BuiltinDriver { id } => {
                        Some(id.as_str())
                    }
                    _ => None,
                })
                .collect()
        }

        fn position(&self, step: &BootStep) -> Option<usize> {
            self.steps.iter().position(|s| s == step)
        }

        /// The configuration a named driver was loaded with.
        fn loaded_config(&self, id: &str) -> Option<&str> {
            self.loaded
                .iter()
                .find(|d| d.id == id)
                .map(|d| d.config.as_str())
        }

        /// The recorded steps as phase labels, for asserting order alone.
        fn phases(&self) -> Vec<&'static str> {
            self.steps
                .iter()
                .map(|step| match step {
                    BootStep::Gpio { .. } => "gpio",
                    BootStep::Spi { .. } => "spi",
                    BootStep::I2c { .. } => "i2c",
                    BootStep::ExpanderInit { .. } => "expander-init",
                    BootStep::ExpanderConfigured { .. } => "expander-configure",
                    BootStep::ExpanderLevel { .. } => "expander-level",
                    BootStep::Delay { .. } => "delay",
                    BootStep::DriverLoaderInit => "driver-loader-init",
                    BootStep::ElfProbe { .. } => "elf-probe",
                    BootStep::DriverElf { .. } => "driver-elf",
                    BootStep::BuiltinDriver { .. } => "builtin-driver",
                })
                .collect()
        }
    }

    impl BootEffects for Recorder {
        fn set_gpio_output(&mut self, output: &GpioOutput) {
            self.steps.push(BootStep::Gpio {
                gpio: output.gpio,
                level: output.level,
                delay_ms: output.delay_ms,
            });
        }

        fn init_spi_bus(&mut self, bus: &SpiBus) -> i32 {
            self.steps.push(BootStep::Spi {
                host: bus.host,
                mosi: bus.mosi,
                miso: bus.miso,
                sclk: bus.sclk,
                max_transfer_bytes: bus.max_transfer_bytes,
            });
            match self.fail_spi_host {
                Some((failing, err)) if failing == bus.host => err,
                _ => ESP_OK,
            }
        }

        fn init_i2c_bus(&mut self, bus: &I2cBus) -> i32 {
            self.steps.push(BootStep::I2c {
                port: bus.port,
                sda: bus.sda,
                scl: bus.scl,
                freq_hz: bus.freq_hz,
            });
            match self.fail_i2c_port {
                Some((failing, err)) if failing == bus.port => err,
                _ => ESP_OK,
            }
        }

        fn init_io_expander(&mut self, expander: &IoExpander) -> i32 {
            self.steps.push(BootStep::ExpanderInit {
                id: expander.id.clone(),
                i2c_bus: expander.i2c_bus,
                i2c_addr: expander.i2c_addr,
            });
            match self.fail_expander_addr {
                Some((failing, err)) if failing == expander.i2c_addr => err,
                _ => ESP_OK,
            }
        }

        fn configure_expander_output(&mut self, output: &ExpanderOutput) -> i32 {
            self.steps.push(BootStep::ExpanderConfigured {
                expander: output.expander,
                pin: output.pin,
            });
            match self.unconfigurable_pin {
                Some((expander, pin)) if (expander, pin) == (output.expander, output.pin) => {
                    ESP_ERR_INVALID_ARG
                }
                _ => ESP_OK,
            }
        }

        fn set_expander_level(&mut self, output: &ExpanderOutput) -> i32 {
            self.steps.push(BootStep::ExpanderLevel {
                expander: output.expander,
                pin: output.pin,
                level: output.level,
            });
            ESP_OK
        }

        fn delay(&mut self, ms: i32) {
            self.steps.push(BootStep::Delay { ms });
        }

        fn init_driver_loader(&mut self) {
            self.steps.push(BootStep::DriverLoaderInit);
        }

        fn driver_elf_exists(&mut self, path: &str) -> bool {
            self.steps.push(BootStep::ElfProbe {
                path: path.to_string(),
            });
            self.present_elfs.iter().any(|p| p == path)
        }

        fn load_driver_elf(&mut self, path: &str, driver: &DriverEntry) {
            self.steps.push(BootStep::DriverElf {
                id: driver.id.clone(),
                path: path.to_string(),
            });
            self.loaded.push(driver.clone());
        }

        fn load_builtin_driver(&mut self, driver: &DriverEntry) {
            self.steps.push(BootStep::BuiltinDriver {
                id: driver.id.clone(),
            });
            self.loaded.push(driver.clone());
        }
    }

    const FULL_PROFILE: &str = r#"{
        "gpio_outputs": [
            {"gpio": 10, "level": 1, "delay_ms": 5},
            {"gpio": 11, "level": 0}
        ],
        "buses": {
            "spi": [{"host": 2, "mosi": 41, "miso": 38, "sclk": 40, "max_transfer_bytes": 8192}],
            "i2c": [{"port": 0, "sda": 13, "scl": 14, "freq_hz": 100000}]
        },
        "drivers": [
            {"id": "com.thistle.drv.epaper", "hal": "display", "entry": "epaper.drv.elf"},
            {"id": "com.thistle.drv.kbd", "hal": "input", "entry": "kbd.drv.elf"}
        ]
    }"#;

    #[test]
    fn boot_sequence_runs_gpio_outputs_then_buses_then_drivers() {
        let mut rec = Recorder::with_elfs_on_disk(&[
            "/spiffs/drivers/epaper.drv.elf",
            "/spiffs/drivers/kbd.drv.elf",
        ]);
        let result = run_boot_sequence(FULL_PROFILE, &mut rec);

        assert_eq!(result, ESP_OK);
        assert_eq!(
            rec.phases(),
            vec![
                "gpio",
                "gpio",
                "spi",
                "i2c",
                "driver-loader-init",
                "elf-probe",
                "driver-elf",
                "elf-probe",
                "driver-elf",
            ]
        );
    }

    #[test]
    fn boot_sequence_passes_each_declared_pin_through_unchanged() {
        let mut rec = Recorder::default();
        run_boot_sequence(FULL_PROFILE, &mut rec);

        assert_eq!(
            rec.steps[..4],
            [
                BootStep::Gpio {
                    gpio: 10,
                    level: 1,
                    delay_ms: 5
                },
                BootStep::Gpio {
                    gpio: 11,
                    level: 0,
                    delay_ms: 0
                },
                BootStep::Spi {
                    host: 2,
                    mosi: 41,
                    miso: 38,
                    sclk: 40,
                    max_transfer_bytes: 8192
                },
                BootStep::I2c {
                    port: 0,
                    sda: 13,
                    scl: 14,
                    freq_hz: 100000
                },
            ]
        );
    }

    #[test]
    fn boot_sequence_initializes_driver_loader_before_loading_any_driver() {
        let mut rec = Recorder::default();
        run_boot_sequence(FULL_PROFILE, &mut rec);

        let loader_init = rec.position(&BootStep::DriverLoaderInit).unwrap();
        let first_driver = rec
            .position(&BootStep::BuiltinDriver {
                id: "com.thistle.drv.epaper".to_string(),
            })
            .unwrap();
        assert!(loader_init < first_driver);
    }

    #[test]
    fn a_driver_elf_is_looked_for_on_spiffs_before_the_sd_card() {
        let json = r#"{
            "drivers": [{"id": "sd-only", "hal": "storage", "entry": "late.drv.elf"}]
        }"#;
        let mut rec = Recorder::with_elfs_on_disk(&["/tmp/thistle_sdcard/drivers/late.drv.elf"]);

        run_boot_sequence(json, &mut rec);

        assert_eq!(
            rec.steps,
            vec![
                BootStep::DriverLoaderInit,
                BootStep::ElfProbe {
                    path: "/spiffs/drivers/late.drv.elf".to_string()
                },
                BootStep::ElfProbe {
                    path: "/tmp/thistle_sdcard/drivers/late.drv.elf".to_string()
                },
                BootStep::DriverElf {
                    id: "sd-only".to_string(),
                    path: "/tmp/thistle_sdcard/drivers/late.drv.elf".to_string()
                },
            ]
        );
    }

    #[test]
    fn a_driver_with_no_elf_on_disk_falls_back_to_the_compiled_in_driver() {
        let json = r#"{
            "drivers": [{"id": "com.thistle.drv.epaper", "hal": "display", "entry": "epaper.drv.elf"}]
        }"#;
        let mut rec = Recorder::default();

        run_boot_sequence(json, &mut rec);

        assert_eq!(
            rec.steps.last(),
            Some(&BootStep::BuiltinDriver {
                id: "com.thistle.drv.epaper".to_string()
            })
        );
    }

    #[test]
    fn the_builtin_fallback_needs_both_an_id_and_a_hal_interface() {
        let json = r#"{
            "drivers": [
                {"id": "", "hal": "display", "entry": "no-id.drv.elf"},
                {"id": "no-hal", "entry": "no-hal.drv.elf"},
                {"id": "complete", "hal": "display", "entry": "complete.drv.elf"}
            ]
        }"#;
        let mut rec = Recorder::default();

        run_boot_sequence(json, &mut rec);

        assert_eq!(rec.driver_ids(), vec!["complete"]);
    }

    #[test]
    fn bus_failure_stops_initialization_before_dependent_buses() {
        let json = r#"{
            "buses": {
                "spi": [
                    {"host": 1},
                    {"host": 2},
                    {"host": 3}
                ],
                "i2c": [{"port": 0}]
            },
            "drivers": [{"id": "com.thistle.drv.epaper", "hal": "display", "entry": "epaper.drv.elf"}]
        }"#;
        let mut rec = Recorder::failing_spi(3, ESP_ERR_NO_MEM);

        let result = run_boot_sequence(json, &mut rec);

        assert_eq!(result, ESP_ERR_NO_MEM);
        assert_eq!(
            rec.steps,
            vec![
                BootStep::Spi {
                    host: 1,
                    mosi: -1,
                    miso: -1,
                    sclk: -1,
                    max_transfer_bytes: 4096
                },
                BootStep::Spi {
                    host: 2,
                    mosi: -1,
                    miso: -1,
                    sclk: -1,
                    max_transfer_bytes: 4096
                },
                BootStep::Spi {
                    host: 3,
                    mosi: -1,
                    miso: -1,
                    sclk: -1,
                    max_transfer_bytes: 4096
                },
            ]
        );
    }

    #[test]
    fn i2c_initialization_preserves_original_error() {
        let json = r#"{"buses":{"i2c":[{"port":0},{"port":1}]}}"#;
        let mut rec = Recorder::failing_i2c(1, ESP_ERR_INVALID_ARG);

        let result = run_boot_sequence(json, &mut rec);

        assert_eq!(result, ESP_ERR_INVALID_ARG);
        assert_eq!(rec.steps.len(), 2);
    }

    #[test]
    fn gpio_outputs_are_applied_before_a_bus_that_fails() {
        let json = r#"{
            "gpio_outputs": [{"gpio": 7, "level": 1}],
            "buses": {"i2c": [{"port": 0}]}
        }"#;
        let mut rec = Recorder::failing_i2c(0, ESP_ERR_NO_MEM);

        assert_eq!(run_boot_sequence(json, &mut rec), ESP_ERR_NO_MEM);
        assert_eq!(
            rec.steps.first(),
            Some(&BootStep::Gpio {
                gpio: 7,
                level: 1,
                delay_ms: 0
            })
        );
    }

    #[test]
    fn profile_without_optional_sections_loads_drivers_only() {
        let json = r#"{
            "drivers": [{"id": "com.thistle.drv.sdcard", "hal": "storage", "entry": "sdcard.drv.elf"}]
        }"#;
        let mut rec = Recorder::with_elfs_on_disk(&["/spiffs/drivers/sdcard.drv.elf"]);

        assert_eq!(run_boot_sequence(json, &mut rec), ESP_OK);
        assert_eq!(
            rec.phases(),
            vec!["driver-loader-init", "elf-probe", "driver-elf"]
        );
    }

    #[test]
    fn driver_entries_with_unsafe_paths_are_skipped() {
        let json = r#"{
            "drivers": [
                {"id": "traversal", "hal": "display", "entry": "../evil.drv.elf"},
                {"id": "absolute", "hal": "display", "entry": "/spiffs/evil.drv.elf"},
                {"id": "hidden", "hal": "display", "entry": ".evil.drv.elf"},
                {"id": "missing-entry", "hal": "display"},
                {"id": "ok", "hal": "display", "entry": "good.drv.elf"}
            ]
        }"#;
        let mut rec = Recorder::default();

        run_boot_sequence(json, &mut rec);

        assert_eq!(rec.driver_ids(), vec!["ok"]);
    }

    #[test]
    fn gpio_entries_without_a_pin_are_skipped() {
        let json = r#"{"gpio_outputs": [{"level": 1}, {"gpio": 3, "level": 1}]}"#;
        let mut rec = Recorder::default();

        run_boot_sequence(json, &mut rec);

        assert_eq!(
            rec.steps.first(),
            Some(&BootStep::Gpio {
                gpio: 3,
                level: 1,
                delay_ms: 0
            })
        );
    }

    // ── IO expander phase ───────────────────────────────────────────

    /// A board whose peripheral rails sit behind an I²C expander.
    const EXPANDER_PROFILE: &str = r#"{
        "gpio_outputs": [{"gpio": 10, "level": 1}],
        "buses": {
            "i2c": [{"port": 0, "sda": 13, "scl": 14, "freq_hz": 400000}]
        },
        "io_expanders": [
            {"id": "com.thistle.drv.ioexp-xl9555", "i2c_bus": 0, "i2c_addr": "0x20"}
        ],
        "expander_outputs": [
            {"expander": 0, "pin": 1, "direction": "output", "level": 1, "delay_ms": 10},
            {"expander": 0, "pin": 2, "direction": "output", "level": 0}
        ],
        "drivers": [
            {"id": "com.thistle.drv.epaper", "hal": "display", "entry": "epaper.drv.elf"}
        ]
    }"#;

    #[test]
    fn expanders_come_up_after_buses_and_their_pins_are_driven_before_drivers_load() {
        let mut rec = Recorder::default();

        assert_eq!(run_boot_sequence(EXPANDER_PROFILE, &mut rec), ESP_OK);
        assert_eq!(
            rec.phases(),
            vec![
                "gpio",
                "i2c",
                "expander-init",
                "expander-configure",
                "expander-level",
                "delay",
                "expander-configure",
                "expander-level",
                "driver-loader-init",
                "elf-probe",
                "elf-probe",
                "builtin-driver",
            ]
        );
    }

    #[test]
    fn an_expander_declaration_reaches_the_driver_unchanged() {
        let mut rec = Recorder::default();

        run_boot_sequence(EXPANDER_PROFILE, &mut rec);

        assert_eq!(
            rec.position(&BootStep::ExpanderInit {
                id: "com.thistle.drv.ioexp-xl9555".to_string(),
                i2c_bus: 0,
                i2c_addr: 0x20,
            }),
            Some(2)
        );
    }

    #[test]
    fn an_expander_pin_is_configured_as_an_output_before_it_is_driven() {
        let mut rec = Recorder::default();

        run_boot_sequence(EXPANDER_PROFILE, &mut rec);

        let direction = rec
            .position(&BootStep::ExpanderConfigured {
                expander: 0,
                pin: 1,
            })
            .unwrap();
        let level = rec
            .position(&BootStep::ExpanderLevel {
                expander: 0,
                pin: 1,
                level: 1,
            })
            .unwrap();
        assert!(direction < level);
    }

    #[test]
    fn a_declared_delay_follows_the_pin_it_is_declared_on() {
        let mut rec = Recorder::default();

        run_boot_sequence(EXPANDER_PROFILE, &mut rec);

        let level = rec
            .position(&BootStep::ExpanderLevel {
                expander: 0,
                pin: 1,
                level: 1,
            })
            .unwrap();
        assert_eq!(rec.steps[level + 1], BootStep::Delay { ms: 10 });
    }

    #[test]
    fn an_expander_that_cannot_be_reached_stops_the_boot_before_drivers_load() {
        let mut rec = Recorder::failing_expander(0x20, ESP_ERR_NOT_FOUND);

        let result = run_boot_sequence(EXPANDER_PROFILE, &mut rec);

        assert_eq!(result, ESP_ERR_NOT_FOUND);
        assert_eq!(rec.phases(), vec!["gpio", "i2c", "expander-init"]);
    }

    #[test]
    fn a_second_expander_is_not_touched_once_the_first_has_failed() {
        let json = r#"{
            "io_expanders": [
                {"id": "com.thistle.drv.ioexp-xl9555", "i2c_bus": 0, "i2c_addr": "0x20"},
                {"id": "com.thistle.drv.ioexp-xl9555", "i2c_bus": 0, "i2c_addr": "0x21"}
            ],
            "expander_outputs": [{"expander": 1, "pin": 0, "direction": "output", "level": 1}]
        }"#;
        let mut rec = Recorder::failing_expander(0x20, ESP_ERR_NO_MEM);

        assert_eq!(run_boot_sequence(json, &mut rec), ESP_ERR_NO_MEM);
        assert_eq!(rec.phases(), vec!["expander-init"]);
    }

    #[test]
    fn an_expander_declaration_missing_its_address_is_never_reached_for() {
        let json = r#"{
            "io_expanders": [{"id": "com.thistle.drv.ioexp-xl9555", "i2c_bus": 0}],
            "expander_outputs": [{"expander": 0, "pin": 1, "direction": "output", "level": 1}],
            "drivers": [{"id": "com.thistle.drv.epaper", "hal": "display", "entry": "e.drv.elf"}]
        }"#;
        let mut rec = Recorder::default();

        assert_eq!(run_boot_sequence(json, &mut rec), ESP_OK);
        assert_eq!(
            rec.phases(),
            vec![
                "driver-loader-init",
                "elf-probe",
                "elf-probe",
                "builtin-driver"
            ]
        );
    }

    #[test]
    fn a_decimal_expander_address_is_read_the_same_as_a_hex_one() {
        let json = r#"{
            "io_expanders": [{"id": "com.thistle.drv.ioexp-xl9555", "i2c_bus": 1, "i2c_addr": 32}]
        }"#;
        let mut rec = Recorder::default();

        run_boot_sequence(json, &mut rec);

        assert_eq!(
            rec.steps[0],
            BootStep::ExpanderInit {
                id: "com.thistle.drv.ioexp-xl9555".to_string(),
                i2c_bus: 1,
                i2c_addr: 0x20,
            }
        );
    }

    #[test]
    fn expander_outputs_the_kernel_cannot_address_are_skipped_without_stopping_the_boot() {
        let json = r#"{
            "io_expanders": [
                {"id": "com.thistle.drv.ioexp-xl9555", "i2c_bus": 0, "i2c_addr": "0x20"}
            ],
            "expander_outputs": [
                {"expander": 0, "pin": 3, "level": 1},
                {"expander": 1, "pin": 3, "direction": "output", "level": 1},
                {"expander": 0, "pin": 16, "direction": "output", "level": 1},
                {"expander": 0, "pin": -1, "direction": "output", "level": 1},
                {"expander": 0, "direction": "output", "level": 1},
                {"expander": 0, "pin": 3, "direction": "sideways", "level": 1},
                {"expander": 0, "pin": 4, "direction": "output", "level": 1}
            ]
        }"#;
        let mut rec = Recorder::default();

        assert_eq!(run_boot_sequence(json, &mut rec), ESP_OK);
        assert_eq!(
            rec.phases(),
            vec![
                "expander-init",
                "expander-configure",
                "expander-level",
                "driver-loader-init",
            ]
        );
        assert_eq!(
            rec.steps[1],
            BootStep::ExpanderConfigured {
                expander: 0,
                pin: 4,
            }
        );
    }

    #[test]
    fn a_pin_that_cannot_be_configured_as_an_output_is_not_then_driven() {
        let mut rec = Recorder::failing_pin(0, 1);

        assert_eq!(run_boot_sequence(EXPANDER_PROFILE, &mut rec), ESP_OK);
        assert_eq!(
            rec.phases(),
            vec![
                "gpio",
                "i2c",
                "expander-init",
                // Pin 1 is configured, fails, and is then neither driven nor
                // waited on. Pin 2 carries on regardless.
                "expander-configure",
                "expander-configure",
                "expander-level",
                "driver-loader-init",
                "elf-probe",
                "elf-probe",
                "builtin-driver",
            ]
        );
    }

    #[test]
    fn a_malformed_expander_declaration_keeps_its_index_without_stopping_the_boot() {
        let json = r#"{
            "io_expanders": [
                {"i2c_bus": 0, "i2c_addr": "0x20"},
                {"id": "com.thistle.drv.ioexp-xl9555", "i2c_bus": 0, "i2c_addr": "0x21"}
            ],
            "expander_outputs": [
                {"expander": 0, "pin": 1, "direction": "output", "level": 1},
                {"expander": 1, "pin": 2, "direction": "output", "level": 1}
            ]
        }"#;
        let mut rec = Recorder::default();

        assert_eq!(run_boot_sequence(json, &mut rec), ESP_OK);
        assert_eq!(
            rec.phases(),
            vec![
                "expander-init",
                "expander-configure",
                "expander-level",
                "driver-loader-init",
            ]
        );
        // The nameless declaration is unusable, so its pins are dropped — but
        // it keeps index 0, leaving the second expander addressable as 1.
        assert_eq!(
            rec.steps[1],
            BootStep::ExpanderConfigured {
                expander: 1,
                pin: 2,
            }
        );
    }

    #[test]
    fn a_profile_declaring_no_expanders_runs_exactly_as_before() {
        let mut rec = Recorder::default();

        run_boot_sequence(FULL_PROFILE, &mut rec);

        assert!(!rec
            .phases()
            .iter()
            .any(|phase| phase.starts_with("expander")));
    }

    #[test]
    fn every_board_in_the_catalogue_boots_without_an_expander_phase() {
        let catalogue =
            Path::new(env!("CARGO_MANIFEST_DIR")).join("../../sdcard_layout/config/boards");
        let profiles: Vec<_> = fs::read_dir(&catalogue)
            .expect("board catalogue")
            .filter_map(|entry| {
                let path = entry.ok()?.path();
                (path.extension()? == "json").then(|| fs::read_to_string(&path).ok())?
            })
            .collect();
        assert!(
            !profiles.is_empty(),
            "no board profiles found in {catalogue:?}"
        );

        for profile in &profiles {
            let mut rec = Recorder::default();

            assert_eq!(run_boot_sequence(profile, &mut rec), ESP_OK);
            assert!(
                !rec.phases()
                    .iter()
                    .any(|phase| phase.starts_with("expander")),
                "a shipped board profile gained an expander phase: {:?}",
                rec.phases()
            );
            // The boards still load every driver they declare, unchanged.
            assert!(!rec.driver_ids().is_empty());
        }
    }

    #[test]
    fn expander_outputs_are_ignored_when_no_expander_is_declared() {
        let json = r#"{
            "expander_outputs": [{"expander": 0, "pin": 1, "direction": "output", "level": 1}],
            "drivers": [{"id": "com.thistle.drv.sdcard", "hal": "storage", "entry": "sd.drv.elf"}]
        }"#;
        let mut rec = Recorder::default();

        assert_eq!(run_boot_sequence(json, &mut rec), ESP_OK);
        assert_eq!(
            rec.phases(),
            vec![
                "driver-loader-init",
                "elf-probe",
                "elf-probe",
                "builtin-driver"
            ]
        );
    }

    #[test]
    fn driver_config_is_normalized_for_the_board_arch() {
        let json = r#"{
            "board": {"id": "tdeck-pro", "arch": "esp32s3"},
            "drivers": [{
                "id": "com.thistle.drv.power-tp4065b",
                "hal": "power",
                "entry": "power.drv.elf",
                "config": {"adc_pin": 4, "charge_pin": 10}
            }]
        }"#;

        let mut rec = Recorder::default();
        run_boot_sequence(json, &mut rec);

        let config = rec.loaded_config("com.thistle.drv.power-tp4065b").unwrap();
        assert!(config.contains(r#""adc_channel": 3"#));
        assert!(config.contains(r#""pin_charge_status": 10"#));
    }

    #[test]
    fn test_normalize_tp4065b_power_config_adds_channel_fields_for_esp32s3() {
        let config = r#"{ "adc_pin": 4, "charge_pin": 10 }"#;
        let normalized = normalize_tp4065b_power_config("esp32s3", config);

        assert!(normalized.contains(r#""adc_pin": 4"#));
        assert!(normalized.contains(r#""charge_pin": 10"#));
        assert!(normalized.contains(r#""adc_channel": 3"#));
        assert!(normalized.contains(r#""pin_charge_status": 10"#));
    }

    #[test]
    fn test_normalize_tp4065b_power_config_keeps_existing_channel_fields() {
        let config = r#"{ "adc_channel": 3, "pin_charge_status": 10 }"#;
        let normalized = normalize_tp4065b_power_config("esp32s3", config);

        assert_eq!(normalized, config);
    }

    #[test]
    fn test_normalize_tp4065b_power_config_skips_unknown_adc_pin_mapping() {
        let config = r#"{ "adc_pin": 33, "charge_pin": 10 }"#;
        let normalized = normalize_tp4065b_power_config("esp32s3", config);

        assert_eq!(normalized, config);
    }

    // ─── find_array tests ────────────────────────────────────────────────

    #[test]
    fn test_find_array_simple() {
        let json = r#"{"drivers": [1, 2, 3]}"#;
        let result = find_array(json, "drivers").unwrap();
        assert_eq!(result, "[1, 2, 3]");
    }

    #[test]
    fn test_find_array_with_whitespace() {
        let json = r#"{"drivers"  :  [1, 2, 3]}"#;
        let result = find_array(json, "drivers").unwrap();
        assert_eq!(result, "[1, 2, 3]");
    }

    #[test]
    fn test_find_array_nested_arrays() {
        let json = r#"{"items": [{"a": [1]}, {"b": 2}]}"#;
        let result = find_array(json, "items").unwrap();
        assert_eq!(result, r#"[{"a": [1]}, {"b": 2}]"#);
    }

    #[test]
    fn test_find_array_missing_key() {
        let json = r#"{"drivers": [1, 2, 3]}"#;
        let result = find_array(json, "nonexistent");
        assert_eq!(result, None);
    }

    #[test]
    fn test_find_array_key_not_array() {
        let json = r#"{"drivers": "not an array"}"#;
        let result = find_array(json, "drivers");
        assert_eq!(result, None);
    }

    #[test]
    fn test_find_array_empty_array() {
        let json = r#"{"items": []}"#;
        let result = find_array(json, "items").unwrap();
        assert_eq!(result, "[]");
    }

    #[test]
    fn test_find_array_nested_mixed() {
        let json = r#"{"config": [{"name": "test", "values": [1, 2, 3]}, {"name": "other"}]}"#;
        let result = find_array(json, "config").unwrap();
        assert_eq!(
            result,
            r#"[{"name": "test", "values": [1, 2, 3]}, {"name": "other"}]"#
        );
    }

    // ─── extract_object tests ────────────────────────────────────────────

    #[test]
    fn test_extract_object_simple() {
        let json = r#"{"board": {"name": "tdeck"}}"#;
        let result = extract_object(json, "board").unwrap();
        assert_eq!(result, r#"{"name": "tdeck"}"#);
    }

    #[test]
    fn test_extract_object_with_whitespace() {
        let json = r#"{"board"  :  {"name": "tdeck"}}"#;
        let result = extract_object(json, "board").unwrap();
        assert_eq!(result, r#"{"name": "tdeck"}"#);
    }

    #[test]
    fn test_extract_object_nested_objects() {
        let json = r#"{"root": {"inner": {"deep": "value"}}}"#;
        let result = extract_object(json, "root").unwrap();
        assert_eq!(result, r#"{"inner": {"deep": "value"}}"#);
    }

    #[test]
    fn test_extract_object_missing_key() {
        let json = r#"{"board": {"name": "tdeck"}}"#;
        let result = extract_object(json, "nonexistent");
        assert_eq!(result, None);
    }

    #[test]
    fn test_extract_object_key_not_object() {
        let json = r#"{"board": "not an object"}"#;
        let result = extract_object(json, "board");
        assert_eq!(result, None);
    }

    #[test]
    fn test_extract_object_empty_object() {
        let json = r#"{"empty": {}}"#;
        let result = extract_object(json, "empty").unwrap();
        assert_eq!(result, "{}");
    }

    #[test]
    fn test_extract_object_with_nested_array() {
        let json = r#"{"config": {"items": [1, 2, 3], "name": "test"}}"#;
        let result = extract_object(json, "config").unwrap();
        assert_eq!(result, r#"{"items": [1, 2, 3], "name": "test"}"#);
    }

    // ─── nth_object tests ────────────────────────────────────────────────

    #[test]
    fn test_nth_object_first() {
        let array = r#"[{"a": 1}, {"b": 2}, {"c": 3}]"#;
        let result = nth_object(array, 0).unwrap();
        assert_eq!(result, r#"{"a": 1}"#);
    }

    #[test]
    fn test_nth_object_middle() {
        let array = r#"[{"a": 1}, {"b": 2}, {"c": 3}]"#;
        let result = nth_object(array, 1).unwrap();
        assert_eq!(result, r#"{"b": 2}"#);
    }

    #[test]
    fn test_nth_object_last() {
        let array = r#"[{"a": 1}, {"b": 2}, {"c": 3}]"#;
        let result = nth_object(array, 2).unwrap();
        assert_eq!(result, r#"{"c": 3}"#);
    }

    #[test]
    fn test_nth_object_out_of_range() {
        let array = r#"[{"a": 1}, {"b": 2}, {"c": 3}]"#;
        let result = nth_object(array, 5);
        assert_eq!(result, None);
    }

    #[test]
    fn test_nth_object_empty_array() {
        let array = "[]";
        let result = nth_object(array, 0);
        assert_eq!(result, None);
    }

    #[test]
    fn test_nth_object_single_element() {
        let array = r#"[{"single": true}]"#;
        let result = nth_object(array, 0).unwrap();
        assert_eq!(result, r#"{"single": true}"#);
    }

    #[test]
    fn test_nth_object_nested_objects() {
        let array = r#"[{"outer": {"inner": "value"}}, {"other": "data"}]"#;
        let result = nth_object(array, 0).unwrap();
        assert_eq!(result, r#"{"outer": {"inner": "value"}}"#);
    }

    #[test]
    fn test_nth_object_with_whitespace() {
        let array = r#"[  {"a": 1}  ,  {"b": 2}  ]"#;
        let result = nth_object(array, 0).unwrap();
        assert_eq!(result, r#"{"a": 1}"#);
    }

    #[test]
    fn test_nth_object_with_whitespace_index_one() {
        let array = r#"[  {"a": 1}  ,  {"b": 2}  ]"#;
        let result = nth_object(array, 1).unwrap();
        assert_eq!(result, r#"{"b": 2}"#);
    }

    // ─── set_board_name tests ────────────────────────────────────────────

    #[test]
    fn test_set_board_name_simple() {
        // Clear BOARD_NAME first
        if let Ok(mut buf) = BOARD_NAME.lock() {
            buf[0] = 0;
        }

        set_board_name("tdeck");
        let buf = BOARD_NAME.lock().unwrap();
        let cstr = CStr::from_bytes_until_nul(&buf[..]).unwrap();
        let name = cstr.to_str().unwrap();
        assert_eq!(name, "tdeck");
    }

    #[test]
    fn test_set_board_name_longer_string() {
        // Clear BOARD_NAME first
        if let Ok(mut buf) = BOARD_NAME.lock() {
            buf[0] = 0;
        }

        set_board_name("t-display-s3-pro");
        let buf = BOARD_NAME.lock().unwrap();
        let cstr = CStr::from_bytes_until_nul(&buf[..]).unwrap();
        let name = cstr.to_str().unwrap();
        assert_eq!(name, "t-display-s3-pro");
    }

    #[test]
    fn test_set_board_name_truncate() {
        // Clear BOARD_NAME first
        if let Ok(mut buf) = BOARD_NAME.lock() {
            buf[0] = 0;
        }

        let long_name = "a".repeat(100);
        set_board_name(&long_name);

        let buf = BOARD_NAME.lock().unwrap();
        let cstr = CStr::from_bytes_until_nul(&buf[..]).unwrap();
        let name = cstr.to_str().unwrap();
        assert_eq!(name.len(), 63);
        assert_eq!(name, "a".repeat(63));
    }

    #[test]
    fn test_set_board_name_empty() {
        // Clear BOARD_NAME first
        if let Ok(mut buf) = BOARD_NAME.lock() {
            buf[0] = 0;
        }

        set_board_name("");
        let buf = BOARD_NAME.lock().unwrap();
        assert_eq!(buf[0], 0);
    }

    #[test]
    fn test_set_board_name_overwrite() {
        // Clear BOARD_NAME first
        if let Ok(mut buf) = BOARD_NAME.lock() {
            buf[0] = 0;
        }

        set_board_name("first");
        let buf1 = BOARD_NAME.lock().unwrap();
        let cstr1 = CStr::from_bytes_until_nul(&buf1[..]).unwrap();
        assert_eq!(cstr1.to_str().unwrap(), "first");
        drop(buf1);

        set_board_name("second");
        let buf2 = BOARD_NAME.lock().unwrap();
        let cstr2 = CStr::from_bytes_until_nul(&buf2[..]).unwrap();
        assert_eq!(cstr2.to_str().unwrap(), "second");
    }
}
