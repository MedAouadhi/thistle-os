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
use crate::radio_antenna::AntennaSwitch;

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
    /// Tell the kernel which radio antenna switch the board has, if any, once
    /// the sequence has thrown it to the internal antenna.
    fn declare_antenna_switch(&mut self, switch: Option<AntennaSwitch>);
    /// Tell the kernel which band the board's LoRa radio is matched for, if
    /// it has one, so the mesh picks a frequency inside it.
    fn declare_radio_band(&mut self, band_hz: Option<u32>);
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
    let antenna_switch = parse_antenna_switch(profile, &expanders)
        .and_then(|switch| select_internal_antenna(effects, switch));
    effects.declare_antenna_switch(antenna_switch);

    load_drivers(profile, effects);
    effects.declare_radio_band(radio_band_hz(profile));

    ESP_OK
}

/// What the SX1262 driver begins at when a profile names no band.
const DEFAULT_RADIO_BAND_HZ: u32 = 915_000_000;

/// The band a board's LoRa radio is matched for: what its driver entry
/// declares, or the driver's own 915 MHz when it declares none. `None` for a
/// board with no radio.
fn radio_band_hz(profile: &str) -> Option<u32> {
    let radio = objects_in(profile, "drivers", MAX_DRIVERS)
        .find(|drv| json_get_string(drv, "hal").as_deref() == Some("radio"))?;
    let declared = extract_object(&radio, "config")
        .and_then(|config| json_get_int(&config, "freq_hz"))
        .and_then(|hz| u32::try_from(hz).ok())
        .filter(|hz| *hz > 0);
    Some(declared.unwrap_or(DEFAULT_RADIO_BAND_HZ))
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

/// Read the profile's `radio_antenna_switch`, or `None` if it declares none or
/// names a pin the kernel cannot address.
///
/// `internal_level` is required rather than defaulted: a guessed level that
/// came out wrong would route the radio to an empty connector.
fn parse_antenna_switch(profile: &str, expanders: &[bool]) -> Option<AntennaSwitch> {
    let entry = extract_object(profile, "radio_antenna_switch")?;

    let expander = json_get_int(&entry, "expander")?;
    if expander < 0 || !expanders.get(expander as usize).copied().unwrap_or(false) {
        return None;
    }

    let pin = json_get_int(&entry, "pin")?;
    if !(0..MAX_EXPANDER_PINS as i64).contains(&pin) {
        return None;
    }

    let internal_level = match json_get_int(&entry, "internal_level")? {
        0 => false,
        1 => true,
        _ => return None,
    };

    Some(AntennaSwitch {
        expander: expander as usize,
        pin: pin as u8,
        internal_level,
    })
}

/// Throw a declared antenna switch to the internal antenna, which is always
/// fitted, before any driver loads. The switch is offered as a choice only if
/// this took: one the kernel could not drive is one it cannot move later.
fn select_internal_antenna<E: BootEffects>(
    effects: &mut E,
    switch: AntennaSwitch,
) -> Option<AntennaSwitch> {
    let output = ExpanderOutput {
        expander: switch.expander,
        pin: switch.pin as i32,
        level: switch.internal_level as i32,
        delay_ms: 0,
    };
    if effects.configure_expander_output(&output) != ESP_OK {
        return None;
    }
    if effects.set_expander_level(&output) != ESP_OK {
        return None;
    }
    Some(switch)
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

    fn declare_antenna_switch(&mut self, switch: Option<AntennaSwitch>) {
        crate::radio_antenna::declare(switch);
    }

    fn declare_radio_band(&mut self, band_hz: Option<u32>) {
        crate::mesh_manager::declare_radio_band(band_hz);
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

    // A board that saved its profile before profiles had a power section
    // never sleeps until it gets one; it gets only that.
    #[cfg(target_os = "espidf")]
    {
        let board_id = extract_object(&json, "board").and_then(|b| json_get_string(&b, "board_id"));
        let updated = board_id
            .and_then(|id| embedded_profile(&id))
            .and_then(|embedded| with_power_section_from(&json, &embedded));
        if let Some(updated) = updated {
            if updated.len() <= MAX_CONFIG_SIZE && fs::write(config_path, &updated).is_ok() {
                extern "C" {
                    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
                }
                unsafe {
                    esp_log_write(
                        3,
                        b"board_config\0".as_ptr(),
                        b"added the built-in power section to board.json\0".as_ptr(),
                    );
                }
                json = updated;
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

    // The pins that woke the board from deep sleep are still routed to the
    // RTC domain; the power manager hands them back before drivers use them.
    crate::power_manager::declare_board(power_section(&json).as_ref());

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

/// The profile's `power` section: which pins wake the board and which rails
/// deep sleep turns off. Read with a real parser, as the text search below
/// would find the power driver's `"hal": "power"` first.
pub(crate) fn power_section(profile: &str) -> Option<serde_json::Value> {
    let root: serde_json::Value = serde_json::from_str(profile).ok()?;
    root.get("power").filter(|p| p.is_object()).cloned()
}

/// `stored` with the `power` section of `embedded` added, for a board that
/// saved its profile before the profile had one. Nothing else in the stored
/// profile changes: it may have been edited, to a LoRa frequency for another
/// region, say. `None` if there is nothing to add.
///
/// The section goes in as text at the top, since re-serialising the whole
/// profile would sort its keys and lose that it was written by hand.
fn with_power_section_from(stored: &str, embedded: &str) -> Option<String> {
    if power_section(stored).is_some() {
        return None;
    }
    let section = power_section(embedded)?;
    let open = stored.find('{')?;
    let updated = format!(
        "{}\n    \"power\": {},{}",
        &stored[..=open],
        serde_json::to_string(&section).ok()?,
        &stored[open + 1..]
    );
    // Only a result that still parses, and now has the section, is kept.
    power_section(&updated).map(|_| updated)
}

#[cfg(target_os = "espidf")]
fn embedded_profile(board_id: &str) -> Option<String> {
    extern "C" {
        // components/kernel/src/board_fingerprint.c
        fn board_fingerprint_profile_json(board_id: *const c_char) -> *const c_char;
    }
    let id = CString::new(board_id).ok()?;
    let json = unsafe { board_fingerprint_profile_json(id.as_ptr()) };
    if json.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(json) }.to_str().ok().map(str::to_string)
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
        /// The antenna switch the sequence declared, or `None` until it does.
        antenna_switch: Option<Option<AntennaSwitch>>,
        /// The radio band the sequence declared, or `None` until it does.
        radio_band: Option<Option<u32>>,
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

        fn declare_antenna_switch(&mut self, switch: Option<AntennaSwitch>) {
            self.antenna_switch = Some(switch);
        }

        fn declare_radio_band(&mut self, band_hz: Option<u32>) {
            self.radio_band = Some(band_hz);
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
    fn a_board_in_the_catalogue_runs_an_expander_phase_only_if_it_declares_one() {
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
            let declares_expander = find_array(profile, "io_expanders").is_some();
            let ran_expander_phase = rec
                .phases()
                .iter()
                .any(|phase| phase.starts_with("expander"));
            assert_eq!(
                ran_expander_phase,
                declares_expander,
                "a shipped board profile's expander phase does not match its \
                 declaration: {:?}",
                rec.phases()
            );
            // The boards still load every driver they declare, unchanged.
            assert!(!rec.driver_ids().is_empty());
        }
    }

    // ── The T-Deck Max ────────────────────────────────────────────────────────
    //
    // The board profile as it ships, so that what these tests assert is the
    // sequence the hardware runs rather than a fixture free to drift from it.
    const TDECK_MAX_PROFILE: &str =
        include_str!("../../../sdcard_layout/config/boards/tdeck-max.json");

    /// The expander pins the Max drives, as the vendor's board header names
    /// them: `BOARD_XL9555_07_TOUCH_RST` and `BOARD_XL9555_11_KEY_RST`, whose
    /// octal-looking suffixes are port/pin, not the pin index.
    const TOUCH_RST_PIN: i32 = 7;
    const KEY_RST_PIN: i32 = 9;

    /// `BOARD_XL9555_02_GPS_EN`, the rail the MIA-M10Q sits behind. On the Pro
    /// the same module is powered from a native GPIO; here it is reachable
    /// only once the expander is up.
    const GPS_EN_PIN: i32 = 2;

    /// `BOARD_XL9555_01_LORA_EN`, the rail the SX1262 sits behind, and
    /// `BOARD_XL9555_04_LORA_SEL`, the RF switch between the module and the
    /// two antenna paths. High selects the antenna fitted on the board; low
    /// routes to the external connector.
    const LORA_EN_PIN: i32 = 1;
    const LORA_SEL_PIN: i32 = 4;
    const LORA_ANTENNA_INTERNAL: i32 = 1;

    /// `BOARD_XL9555_03_1V8_EN`, the BHI260AP's 1.8 V rail.
    const IMU_EN_PIN: i32 = 3;

    /// `BOARD_XL9555_06_AMPLIFIER`, the NS4150B's enable, high = on, and
    /// `BOARD_XL9555_12_AUDIO_SEL`, the speaker mux, low = ES8311. The vendor
    /// names the second after its port and bit, P1.2, but numbers it 10.
    const AMP_EN_PIN: i32 = 6;
    const AUDIO_SEL_PIN: i32 = 10;
    const AUDIO_ROUTE_CODEC: i32 = 0;

    /// Where in the sequence a pin was first driven to a level, if it was.
    fn level_step(rec: &Recorder, want_pin: i32, want_level: i32) -> Option<usize> {
        rec.steps.iter().position(|step| {
            matches!(
                step,
                BootStep::ExpanderLevel { pin, level, .. }
                    if *pin == want_pin && *level == want_level
            )
        })
    }

    /// Where in the sequence the drivers began to load.
    fn driver_loader_step(rec: &Recorder) -> Option<usize> {
        rec.steps
            .iter()
            .position(|step| matches!(step, BootStep::DriverLoaderInit))
    }

    /// Every expander level the boot sequence drove, in order.
    fn expander_levels(rec: &Recorder) -> Vec<(usize, i32, i32)> {
        rec.steps
            .iter()
            .filter_map(|step| match step {
                BootStep::ExpanderLevel {
                    expander,
                    pin,
                    level,
                } => Some((*expander, *pin, *level)),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn the_tdeck_max_raises_its_expander_between_the_buses_and_its_drivers() {
        let mut rec = Recorder::default();

        assert_eq!(run_boot_sequence(TDECK_MAX_PROFILE, &mut rec), ESP_OK);

        let phases = rec.phases();
        let position = |want: &str| phases.iter().position(|phase| *phase == want);
        let last = |want: &str| phases.iter().rposition(|phase| *phase == want);

        let i2c = last("i2c").expect("the Max declares an I²C bus");
        let expander = position("expander-init").expect("the Max declares an expander");
        let last_pin = last("expander-level").expect("the Max drives expander pins");
        let drivers = position("driver-loader-init").expect("the Max declares drivers");

        // The expander sits on the I²C bus, and the pins behind it carry
        // peripheral rails and resets the drivers depend on.
        assert!(i2c < expander, "{phases:?}");
        assert!(expander < last_pin, "{phases:?}");
        assert!(last_pin < drivers, "{phases:?}");
    }

    #[test]
    fn the_tdeck_max_raises_its_rails_then_pulses_its_two_resets() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        // Both resets are active low, so each is held down and then released.
        // Leaving them high would start the parts from whatever state an
        // unclean shutdown left behind. The rails go up first and stay up — a
        // level each, not a pulse. The antenna switch is declared on its own,
        // so it is thrown once the declared pins are done.
        assert_eq!(
            expander_levels(&rec),
            vec![
                (0, GPS_EN_PIN, 1),
                (0, LORA_EN_PIN, 1),
                (0, IMU_EN_PIN, 1),
                (0, TOUCH_RST_PIN, 0),
                (0, KEY_RST_PIN, 0),
                (0, TOUCH_RST_PIN, 1),
                (0, KEY_RST_PIN, 1),
                (0, AMP_EN_PIN, 0),
                (0, AUDIO_SEL_PIN, AUDIO_ROUTE_CODEC),
                (0, LORA_SEL_PIN, LORA_ANTENNA_INTERNAL),
            ]
        );
    }

    #[test]
    fn the_tdeck_max_routes_its_speaker_to_the_codec_with_the_amplifier_off() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        // The amplifier's enable has a pull-up behind it, so an expander pin
        // left as the input it resets to leaves the speaker hissing from
        // power-on until the audio driver loads. The mux defaults the other
        // way, to the modem, which is out of scope and silent.
        let levels = expander_levels(&rec);
        assert_eq!(
            levels.iter().filter(|(_, pin, _)| *pin == AMP_EN_PIN).collect::<Vec<_>>(),
            vec![&(0, AMP_EN_PIN, 0)],
            "the board profile only ever holds the amplifier off"
        );
        let routed = level_step(&rec, AUDIO_SEL_PIN, AUDIO_ROUTE_CODEC)
            .expect("the Max routes its speaker to the codec");
        let drivers = driver_loader_step(&rec).expect("the Max loads drivers");
        assert!(routed < drivers, "{:?}", rec.phases());

        // The driver gates the amplifier itself, so it must be told where.
        let config = rec
            .loaded_config("com.thistle.drv.audio-es8311")
            .expect("the Max declares its codec");
        assert_eq!(json_get_int(config, "amp_expander"), Some(0), "{config}");
        assert_eq!(json_get_int(config, "amp_expander_pin"), Some(AMP_EN_PIN as i64), "{config}");
        // The schematic's I2S_DSDIN, the SoC's data out. LilyGo's pinmap has
        // this as GPIO17, which is the codec's ASDOUT and would play nothing.
        assert_eq!(json_get_int(config, "i2s_data"), Some(40), "{config}");
        // And the schematic's I2S_ASDOUT, the microphone coming back in.
        // LilyGo's pinmap swaps this one too, to GPIO40.
        assert_eq!(json_get_int(config, "i2s_din"), Some(17), "{config}");
    }

    #[test]
    fn the_tdeck_max_powers_its_gnss_module_before_the_driver_probes_for_it() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        // The module answers nothing until its rail is up, so a driver that
        // opens the UART first would find an idle line and conclude there is
        // no receiver fitted.
        let raised = level_step(&rec, GPS_EN_PIN, 1).expect("the Max raises its GNSS rail");
        let drivers = driver_loader_step(&rec).expect("the Max loads drivers");

        assert!(raised < drivers, "{:?}", rec.phases());
    }

    #[test]
    fn the_tdeck_max_powers_its_radio_before_the_driver_probes_for_it() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        // Unlike the Pro, where the SX1262 is powered whenever the board is,
        // here the module's rail is behind the expander. A driver that resets
        // and probes an unpowered part reads back nothing and gives up, and
        // the board comes up with no radio at all.
        let raised = level_step(&rec, LORA_EN_PIN, 1).expect("the Max raises its LoRa rail");
        let drivers = driver_loader_step(&rec).expect("the Max loads drivers");

        assert!(raised < drivers, "{:?}", rec.phases());
    }

    #[test]
    fn the_tdeck_max_points_its_radio_at_the_antenna_it_has() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        // The RF switch powers up in whatever state it powers up in. Left
        // alone it can route transmission to the external connector, which on
        // a board with nothing screwed into it is an open port — the radio
        // reports a clean transmission into a reflection.
        let selected = level_step(&rec, LORA_SEL_PIN, LORA_ANTENNA_INTERNAL)
            .expect("the Max selects an antenna path");
        let drivers = driver_loader_step(&rec).expect("the Max loads drivers");

        assert!(selected < drivers, "{:?}", rec.phases());
        assert_eq!(
            expander_levels(&rec)
                .iter()
                .filter(|(_, pin, _)| *pin == LORA_SEL_PIN)
                .count(),
            1,
            "the switch is set once and left there until Settings moves it"
        );
    }

    #[test]
    fn the_tdeck_max_offers_its_antenna_switch_as_a_choice() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        assert_eq!(
            rec.antenna_switch,
            Some(Some(AntennaSwitch {
                expander: 0,
                pin: LORA_SEL_PIN as u8,
                internal_level: LORA_ANTENNA_INTERNAL == 1,
            }))
        );
    }

    // ── The radio band ────────────────────────────────────────────────────────

    #[test]
    fn the_tdeck_max_declares_the_band_its_radio_is_matched_for() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);
        assert_eq!(rec.radio_band, Some(Some(868_000_000)));
    }

    #[test]
    fn a_radio_that_names_no_band_is_at_the_drivers_915_mhz() {
        let profile = r#"{
            "drivers": [
                { "id": "com.thistle.drv.radio-sx1262", "hal": "radio",
                  "entry": "radio-sx1262.drv.elf", "config": { "cs": 9 } }
            ]
        }"#;
        let mut rec = Recorder::default();
        run_boot_sequence(profile, &mut rec);
        assert_eq!(rec.radio_band, Some(Some(915_000_000)));
    }

    #[test]
    fn a_board_with_no_radio_declares_no_band() {
        let profile = r#"{
            "drivers": [
                { "id": "com.thistle.drv.gps-mia-m10q", "hal": "gps",
                  "entry": "gps-mia-m10q.drv.elf", "config": { "freq_hz": 868000000 } }
            ]
        }"#;
        let mut rec = Recorder::default();
        run_boot_sequence(profile, &mut rec);
        assert_eq!(rec.radio_band, Some(None));
    }

    // ── The radio antenna switch ──────────────────────────────────────────────

    /// A board with one expander and whatever antenna switch `switch` says.
    fn profile_with_antenna_switch(switch: &str) -> String {
        format!(
            r#"{{
                "buses": {{ "i2c": [{{ "port": 0, "sda": 13, "scl": 14 }}] }},
                "io_expanders": [{{ "id": "com.thistle.drv.ioexp-xl9555", "i2c_bus": 0, "i2c_addr": "0x20" }}],
                {switch}
                "drivers": []
            }}"#
        )
    }

    #[test]
    fn a_board_with_no_antenna_switch_declares_none() {
        let mut rec = Recorder::default();
        run_boot_sequence(&profile_with_antenna_switch(""), &mut rec);
        assert_eq!(rec.antenna_switch, Some(None));
    }

    #[test]
    fn an_antenna_switch_whose_internal_level_is_inverted_is_driven_low() {
        let mut rec = Recorder::default();
        let profile = profile_with_antenna_switch(
            r#""radio_antenna_switch": { "expander": 0, "pin": 5, "internal_level": 0 },"#,
        );
        run_boot_sequence(&profile, &mut rec);

        assert_eq!(expander_levels(&rec), vec![(0, 5, 0)]);
        assert_eq!(
            rec.antenna_switch,
            Some(Some(AntennaSwitch {
                expander: 0,
                pin: 5,
                internal_level: false,
            }))
        );
    }

    #[test]
    fn an_antenna_switch_with_no_internal_level_is_not_guessed_at() {
        let mut rec = Recorder::default();
        let profile = profile_with_antenna_switch(
            r#""radio_antenna_switch": { "expander": 0, "pin": 4 },"#,
        );
        run_boot_sequence(&profile, &mut rec);

        assert!(expander_levels(&rec).is_empty(), "{:?}", rec.phases());
        assert_eq!(rec.antenna_switch, Some(None));
    }

    #[test]
    fn an_antenna_switch_on_an_undeclared_expander_is_ignored() {
        let mut rec = Recorder::default();
        let profile = profile_with_antenna_switch(
            r#""radio_antenna_switch": { "expander": 1, "pin": 4, "internal_level": 1 },"#,
        );
        run_boot_sequence(&profile, &mut rec);

        assert!(expander_levels(&rec).is_empty(), "{:?}", rec.phases());
        assert_eq!(rec.antenna_switch, Some(None));
    }

    #[test]
    fn an_antenna_switch_that_cannot_be_driven_is_not_offered() {
        let mut rec = Recorder::failing_pin(0, 4);
        let profile = profile_with_antenna_switch(
            r#""radio_antenna_switch": { "expander": 0, "pin": 4, "internal_level": 1 },"#,
        );
        run_boot_sequence(&profile, &mut rec);

        assert!(expander_levels(&rec).is_empty(), "{:?}", rec.phases());
        assert_eq!(rec.antenna_switch, Some(None));
    }

    #[test]
    fn the_tdeck_max_tunes_its_radio_to_the_band_it_ships_in() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        let config = rec
            .loaded_config("com.thistle.drv.radio-sx1262")
            .expect("the Max declares a radio");
        // Same part on the same pins as the Pro: the module sits on the shared
        // SPI bus, and only its four control lines are its own.
        assert_eq!(json_get_int(config, "spi_bus"), Some(0), "{config}");
        assert_eq!(json_get_int(config, "cs"), Some(3), "{config}");
        assert_eq!(json_get_int(config, "rst"), Some(4), "{config}");
        assert_eq!(json_get_int(config, "dio1"), Some(5), "{config}");
        assert_eq!(json_get_int(config, "busy"), Some(6), "{config}");
        // The band is the board's, not the part's. This one ships with the
        // 868 MHz module, where the driver's own default is the 915 MHz the
        // North American boards use — a default that here would transmit
        // outside the band the hardware is matched and licensed for.
        assert_eq!(
            json_get_int(config, "freq_hz"),
            Some(868_000_000),
            "{config}"
        );
        // The S62F module runs a 32 MHz TCXO off the transceiver's DIO3, and
        // DIO2 drives the module's own transmit/receive switch. Left
        // undeclared the transceiver starts on a crystal that is not there:
        // it answers over SPI and reports a clean start, then times out on
        // every transmission waiting for its PLL to lock.
        assert_eq!(json_get_int(config, "tcxo_mv"), Some(2400), "{config}");
        assert!(config.contains("\"dio2_rf_switch\": true"), "{config}");
    }

    #[test]
    fn the_tdeck_max_moves_its_gnss_off_the_pins_that_carry_its_console() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        let config = rec
            .loaded_config("com.thistle.drv.gps-mia-m10q")
            .expect("the Max declares the GNSS module");
        // Same part as the Pro on different pins. The Pro runs its GNSS on
        // 43/44, which on this board is where the console comes out, so
        // inheriting the Pro's numbers would have the driver talk over the log.
        assert_eq!(json_get_int(config, "uart_num"), Some(2), "{config}");
        assert_eq!(json_get_int(config, "tx"), Some(16), "{config}");
        assert_eq!(json_get_int(config, "rx"), Some(2), "{config}");
        // The M10 leaves the factory at 38400, where the M8 on the older
        // boards sat at 9600. The driver opens at whatever the profile says
        // and does not go looking, so a module someone has moved to the other
        // rate reads as an empty line rather than as a wrong setting.
        assert_eq!(json_get_int(config, "baud_rate"), Some(38400), "{config}");
    }

    #[test]
    fn the_tdeck_max_holds_each_reset_down_long_enough_to_take() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        // A pulse with no delay between its edges is not a reset, and a rail
        // the next step reads from before it has come up is not a rail. Both
        // resets and all three power enables declare a settling delay, so each of
        // their edges is followed by a wait. The antenna switch is neither: it
        // is thrown once and nothing reads it until the drivers load.
        let settles: Vec<i32> =
            [TOUCH_RST_PIN, KEY_RST_PIN, GPS_EN_PIN, LORA_EN_PIN, IMU_EN_PIN].into();
        for (index, step) in rec.steps.iter().enumerate() {
            let BootStep::ExpanderLevel { pin, .. } = step else {
                continue;
            };
            if !settles.contains(pin) {
                continue;
            }
            assert!(
                matches!(rec.steps.get(index + 1), Some(BootStep::Delay { ms }) if *ms > 0),
                "pin {pin} is driven with nothing waiting on it: {:?}",
                rec.phases()
            );
        }

        // And nothing else waits: the boot spends time only where a pin asked
        // it to. Counting pins the settling list names, rather than every pin
        // driven, is what lets the antenna switch declare no delay and still
        // hold this line.
        let delays = rec
            .steps
            .iter()
            .filter(|step| matches!(step, BootStep::Delay { .. }))
            .count();
        let settling_pins = expander_levels(&rec)
            .iter()
            .filter(|(_, pin, _)| settles.contains(pin))
            .count();
        assert_eq!(delays, settling_pins, "{:?}", rec.phases());
    }

    #[test]
    fn the_tdeck_max_display_drives_the_reset_line_the_pro_lacks() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        let config = rec
            .loaded_config("com.thistle.drv.epaper-gdeq031t10")
            .expect("the Max declares the e-paper display");
        // The Pro leaves this at -1: the panel there has no reset line at all,
        // so a panel in a bad state needs a power cycle.
        assert_eq!(json_get_int(config, "rst"), Some(9), "{config}");
        assert_eq!(json_get_int(config, "bl"), Some(41), "{config}");
    }

    #[test]
    fn the_tdeck_max_mounts_its_panel_the_other_way_up_from_the_pro() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        let config = rec
            .loaded_config("com.thistle.drv.epaper-gdeq031t10")
            .expect("the Max declares the e-paper display");
        // Same panel as the Pro, turned around. The controller scans either
        // way, so this is a panel setting rather than work for the CPU.
        assert_eq!(json_get_int(config, "rotation"), Some(180), "{config}");
    }

    #[test]
    fn the_tdeck_max_reaches_its_touch_reset_through_the_expander() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        let config = rec
            .loaded_config("com.thistle.drv.touch-cst66xx")
            .expect("the Max declares the touch panel");
        // T_RST is an expander pin, not a GPIO. Handing the driver the pin
        // number would have it drive GPIO 7, which on this board is the
        // modem's ring indicator.
        assert_eq!(json_get_int(config, "pin_rst"), Some(-1), "{config}");
        // The expander phase sets the idle level, but the driver still has to
        // pulse the line itself right before it probes — the part needs a
        // reset it has not had for half a second by the time drivers load.
        assert_eq!(json_get_int(config, "rst_expander"), Some(0), "{config}");
        assert_eq!(json_get_int(config, "rst_expander_pin"), Some(7), "{config}");
    }

    #[test]
    fn the_tdeck_max_leaves_its_modem_to_the_driver() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        // XL9555 P00 (6609_EN) switches the module's supply and P10
        // (PWRKEY_EN) presses its power key through an NPN, so pressing is
        // driving it high (T-Deck-MAX docs/pinmap.md section 10; schematic
        // page 5, Q5). A running A7682E must be shut down before its supply
        // goes, and AT+CPOF needs the key released or the module powers
        // itself on again, so boot drives neither: only the driver, which
        // knows the module's state, may.
        const MODEM_SUPPLY_PIN: i32 = 0;
        const MODEM_PWRKEY_PIN: i32 = 8;
        let levels = expander_levels(&rec);
        assert!(
            levels
                .iter()
                .all(|(_, pin, _)| *pin != MODEM_SUPPLY_PIN && *pin != MODEM_PWRKEY_PIN),
            "{levels:?}"
        );

        let config = rec
            .loaded_config("com.thistle.drv.modem-a7682e")
            .expect("the Max declares its modem");
        assert_eq!(json_get_int(config, "power_expander"), Some(0), "{config}");
        assert_eq!(json_get_int(config, "power_expander_pin"), Some(MODEM_SUPPLY_PIN as i64));
        assert_eq!(json_get_int(config, "pwrkey_expander"), Some(0), "{config}");
        assert_eq!(json_get_int(config, "pwrkey_expander_pin"), Some(MODEM_PWRKEY_PIN as i64));
        assert_eq!(json_get_int(config, "pwrkey_press_level"), Some(1), "{config}");
        // 7682_RXD is GPIO10 and 7682_TXD GPIO11, named from the module's
        // side: the SoC transmits on 10 and receives on 11.
        assert_eq!(json_get_int(config, "tx"), Some(10), "{config}");
        assert_eq!(json_get_int(config, "rx"), Some(11), "{config}");
        // The GPS has UART 2.
        assert_eq!(json_get_int(config, "uart_num"), Some(1), "{config}");
        // The module powers up at 115200 and is switched to 921600: 8 times
        // the data rate, still slow enough for a link without RTS/CTS.
        assert_eq!(json_get_int(config, "baud_rate"), Some(115200), "{config}");
        assert_eq!(json_get_int(config, "fast_baud_rate"), Some(921600), "{config}");
        // BOARD_A7682E_RI: the module pulses it when an SMS arrives, which
        // is how one is noticed during a data session (pinmap.md section 10).
        assert_eq!(json_get_int(config, "ri"), Some(7), "{config}");
    }

    #[test]
    fn the_tdeck_max_leaves_its_vibration_motor_to_the_driver() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        // A DRV2605 at 0x5A, powered by XL9555 P05 (`M_EN`, T-Deck-MAX
        // docs/pinmap.md section 7). Boot leaves the pin alone; the driver
        // raises it when it starts and drops it when it stops.
        const MOTOR_EN_PIN: i32 = 5;
        let levels = expander_levels(&rec);
        assert!(levels.iter().all(|(_, pin, _)| *pin != MOTOR_EN_PIN), "{levels:?}");

        let config = rec
            .loaded_config("com.thistle.drv.haptic-drv2605")
            .expect("the Max declares its vibration motor");
        assert_eq!(json_get_string(config, "i2c_addr").as_deref(), Some("0x5A"), "{config}");
        assert_eq!(json_get_int(config, "en_expander"), Some(0), "{config}");
        assert_eq!(json_get_int(config, "en_expander_pin"), Some(MOTOR_EN_PIN as i64));
    }

    #[test]
    fn the_tdeck_max_drives_its_touch_panel_as_the_cst66xx_it_is() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        // The vendor pinmap calls this part a CST328 and the vendor README
        // calls it a CST3530. Both are wrong, and neither shares a register
        // map with what is actually fitted: running LilyGo's own firmware on
        // the board reports `ictype:caca220e`, the CST66xx signature. The Pro
        // keeps the CST328 driver, so the two must stay separate.
        let ids = rec.driver_ids();
        assert!(
            ids.iter().any(|id| *id == "com.thistle.drv.touch-cst66xx"),
            "the Max drives a CST66xx: {ids:?}"
        );
        assert!(
            !ids.iter().any(|id| id.contains("cst328")),
            "the CST328 driver belongs to the Pro, not the Max: {ids:?}"
        );
    }

    #[test]
    fn the_tdeck_max_declares_no_rtc_and_no_light_sensor() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        // Neither part is on this board. Leaving the interfaces unregistered
        // lets an app detect that, where a stub would have it read fiction.
        let ids = rec.driver_ids();
        assert!(
            !ids.iter().any(|id| id.contains("rtc")),
            "the Max has no RTC: {ids:?}"
        );
        assert!(
            !ids.iter().any(|id| id.contains("light")),
            "the Max has no light sensor: {ids:?}"
        );
    }

    #[test]
    fn the_tdeck_max_reads_its_battery_off_the_bus_its_charger_sits_on() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_MAX_PROFILE, &mut rec);

        let config = rec
            .loaded_config("com.thistle.drv.power-sy6970")
            .expect("the Max declares a power driver");
        // Which of the two chargers this unit carries depends on its hardware
        // revision, so the entry says only where to look, not what to find.
        assert_eq!(json_get_int(config, "i2c_bus"), Some(0), "{config}");
    }

    // ── The T-Deck Pro ────────────────────────────────────────────────────────

    const TDECK_PRO_PROFILE: &str =
        include_str!("../../../sdcard_layout/config/boards/tdeck-pro.json");

    #[test]
    fn the_tdeck_pro_no_longer_reads_its_battery_from_the_lora_reset_line() {
        let mut rec = Recorder::default();
        run_boot_sequence(TDECK_PRO_PROFILE, &mut rec);

        let ids = rec.driver_ids();
        assert!(
            !ids.iter().any(|id| id.contains("power-tp4065b")),
            "the Pro carries no TP4065B: {ids:?}"
        );

        let config = rec
            .loaded_config("com.thistle.drv.power-sy6970")
            .expect("the Pro declares a power driver");
        // The entry it replaces read battery voltage off GPIO 4, which on this
        // board is the LoRa reset line rather than a battery divider.
        assert_eq!(json_get_int(config, "adc_pin"), None, "{config}");
        assert_eq!(json_get_int(config, "i2c_bus"), Some(0), "{config}");
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

    // ── The power section ───────────────────────────────────────────────

    const STORED_WITHOUT_POWER: &str = r#"{
    "board": {"name": "Max", "board_id": "tdeck-max"},
    "drivers": [
        {"id": "com.thistle.drv.power-sy6970", "hal": "power", "entry": "power-sy6970.drv.elf"},
        {"id": "com.thistle.drv.radio-sx1262", "hal": "radio", "config": {"freq_hz": 915000000}}
    ]
}"#;

    const EMBEDDED_WITH_POWER: &str = r#"{
    "board": {"name": "Max", "board_id": "tdeck-max"},
    "drivers": [{"id": "com.thistle.drv.power-sy6970", "hal": "power"}],
    "power": {"key": {"gpio": 0, "active_level": 0}}
}"#;

    #[test]
    fn a_power_driver_is_not_a_power_section() {
        assert_eq!(power_section(STORED_WITHOUT_POWER), None);
        assert!(power_section(EMBEDDED_WITH_POWER).is_some());
    }

    #[test]
    fn a_stored_profile_gets_the_power_section_and_keeps_everything_else() {
        let updated = with_power_section_from(STORED_WITHOUT_POWER, EMBEDDED_WITH_POWER).unwrap();
        assert_eq!(power_section(&updated), power_section(EMBEDDED_WITH_POWER));
        // The edited frequency, and the rest of the text, are untouched.
        assert!(updated.ends_with(&STORED_WITHOUT_POWER[1..]));
        assert!(updated.contains("915000000"));
        // The section is found by the boot code's own search, too.
        assert!(extract_object(&updated, "power").is_some());
    }

    #[test]
    fn a_profile_that_has_a_power_section_is_left_alone() {
        assert_eq!(with_power_section_from(EMBEDDED_WITH_POWER, EMBEDDED_WITH_POWER), None);
    }

    #[test]
    fn with_no_built_in_power_section_there_is_nothing_to_add() {
        assert_eq!(with_power_section_from(STORED_WITHOUT_POWER, STORED_WITHOUT_POWER), None);
    }

    #[test]
    fn the_shipped_max_profile_has_a_power_section() {
        let shipped = include_str!("../../../sdcard_layout/config/boards/tdeck-max.json");
        let power = power_section(shipped).expect("tdeck-max.json declares power");
        assert_eq!(power["key"]["gpio"], 0);
    }
}

