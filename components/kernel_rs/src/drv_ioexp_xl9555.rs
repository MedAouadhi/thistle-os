// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — XL9555 16-bit I²C IO expander driver (Rust)
//
// The XL9555 carries the PCA9535-compatible eight-register layout: input,
// output, polarity-inversion and configuration registers, two ports of eight
// pins each. It is not a peripheral class the HAL models, so it registers no
// vtable — the board layer drives it directly, during the expander phase of
// the board profile boot sequence.
//
// The part is poll-only here by necessity rather than choice: on the boards
// that carry it, its interrupt line is not routed to the SoC, so there is no
// interrupt path to design.

use std::os::raw::c_void;
use std::sync::Mutex;

// ── ESP error codes ──────────────────────────────────────────────────────────

const ESP_OK: i32 = 0;
const ESP_FAIL: i32 = -1;
const ESP_ERR_NOT_FOUND: i32 = 0x105;
const ESP_ERR_INVALID_ARG: i32 = 0x102;

// ── XL9555 register map ──────────────────────────────────────────────────────

// Eight registers, port 0 then port 1: input 0x00/0x01, output 0x02/0x03,
// polarity inversion 0x04/0x05, configuration 0x06/0x07. Polarity inversion
// is left at its reset value — nothing here reads pins through it.

/// Output port 0 — the level driven on pins 0-7. Resets to all-high.
const XL9555_REG_OUTPUT_0: u8 = 0x02;
/// Configuration port 0 — `0 = output, 1 = input`. Resets to all-inputs,
/// which is why a board profile must declare direction alongside level.
const XL9555_REG_CONFIG_0: u8 = 0x06;

/// Pins per port. Port 1's registers sit one above port 0's throughout.
const PINS_PER_PORT: u8 = 8;
/// The width of the part, and the bound on a pin number it will accept.
pub const XL9555_PINS: u8 = 16;

/// How long an expander register write may take before it is given up on.
const I2C_TIMEOUT_MS: i32 = 50;

/// Default I²C speed for the expander's device handle. The bus's own
/// frequency is configured per device under ESP-IDF's i2c_master driver.
const I2C_SPEED_HZ: u32 = 400_000;

// ── Register arithmetic ──────────────────────────────────────────────────────

/// Which of the two ports a pin belongs to.
///
/// # Examples
/// ```
/// assert_eq!(drv_ioexp_xl9555::pin_port(0), 0);
/// assert_eq!(drv_ioexp_xl9555::pin_port(8), 1);
/// ```
#[inline]
pub fn pin_port(pin: u8) -> usize {
    (pin / PINS_PER_PORT) as usize
}

/// The bit a pin occupies within its port's registers.
///
/// # Examples
/// ```
/// assert_eq!(drv_ioexp_xl9555::pin_mask(0), 0x01);
/// assert_eq!(drv_ioexp_xl9555::pin_mask(9), 0x02);
/// ```
#[inline]
pub fn pin_mask(pin: u8) -> u8 {
    1 << (pin % PINS_PER_PORT)
}

/// A register byte with one bit set or cleared, the others left alone.
#[inline]
pub fn with_bit(register: u8, mask: u8, set: bool) -> u8 {
    if set {
        register | mask
    } else {
        register & !mask
    }
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

// ── Stub impls for host tests ────────────────────────────────────────────────

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
        // A non-null sentinel, so the driver can tell "success".
        *handle = 1usize as *mut c_void;
        0
    }
    pub unsafe fn i2c_master_bus_rm_device(_handle: *mut c_void) -> i32 {
        0
    }
    pub unsafe fn i2c_master_transmit_receive(
        _handle: *mut c_void,
        _write: *const u8,
        _wsz: usize,
        read: *mut u8,
        rsz: usize,
        _timeout: i32,
    ) -> i32 {
        // The part's reset state: every pin an input, every output latch high.
        unsafe { std::ptr::write_bytes(read, 0xFF, rsz) };
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
}

// ── Driver state ─────────────────────────────────────────────────────────────

/// One attached expander, holding the shadow copies of the two registers that
/// cannot be read back meaningfully: the output latch reads back as the pin
/// levels, and a read-modify-write off the input port would clobber pins the
/// board has already driven.
struct Xl9555 {
    /// i2c_master_dev_handle_t
    dev: *mut c_void,
    outputs: [u8; 2],
    config: [u8; 2],
}

// SAFETY: `dev` is an opaque C handle, only ever dereferenced by the ESP-IDF
// I²C driver, which is itself thread-safe. The expanders are guarded by the
// mutex below.
unsafe impl Send for Xl9555 {}

/// The expanders a board profile has brought up, in declaration order: the nth
/// entry is the expander a board profile's `expander_outputs` address as `n`.
static EXPANDERS: Mutex<Vec<Xl9555>> = Mutex::new(Vec::new());

// ── Board-layer API ──────────────────────────────────────────────────────────

/// Bring up the expander at `i2c_addr` on the bus registered at `i2c_bus`,
/// claiming the next expander index.
///
/// The part's output and configuration registers are read once, so that every
/// later pin write changes one bit and leaves the rest of the port as it was.
pub fn attach(i2c_bus: i32, i2c_addr: u8) -> i32 {
    let bus = crate::hal_registry::hal_bus_get_i2c(i2c_bus);
    if bus.is_null() {
        return ESP_ERR_NOT_FOUND;
    }

    let cfg = esp_ffi::I2cDeviceConfig {
        dev_addr_length: 0,
        device_address: i2c_addr as u16,
        scl_speed_hz: I2C_SPEED_HZ,
        scl_wait_us: 0,
        flags: 0,
    };
    let mut dev: *mut c_void = std::ptr::null_mut();
    let ret = unsafe { esp_ffi::i2c_master_bus_add_device(bus, &cfg, &mut dev) };
    if ret != ESP_OK || dev.is_null() {
        return if ret != ESP_OK { ret } else { ESP_FAIL };
    }

    let mut outputs = [0u8; 2];
    let mut config = [0u8; 2];
    for (reg, shadow) in [
        (XL9555_REG_OUTPUT_0, &mut outputs),
        (XL9555_REG_CONFIG_0, &mut config),
    ] {
        let ret = unsafe { read_regs(dev, reg, shadow) };
        if ret != ESP_OK {
            unsafe { esp_ffi::i2c_master_bus_rm_device(dev) };
            return ret;
        }
    }

    match EXPANDERS.lock() {
        Ok(mut expanders) => {
            expanders.push(Xl9555 {
                dev,
                outputs,
                config,
            });
            ESP_OK
        }
        Err(_) => {
            unsafe { esp_ffi::i2c_master_bus_rm_device(dev) };
            ESP_FAIL
        }
    }
}

/// Configure an expander pin as an output, so that it drives its rail rather
/// than floating at the all-inputs state the part resets to.
pub fn set_as_output(index: usize, pin: u8) -> i32 {
    with_expander(index, pin, |expander, port, mask| {
        // `0 = output, 1 = input` — the part's configuration semantics.
        expander.config[port] = with_bit(expander.config[port], mask, false);
        let value = expander.config[port];
        unsafe { write_reg(expander.dev, XL9555_REG_CONFIG_0 + port as u8, value) }
    })
}

/// Drive an expander pin high or low.
pub fn set_level(index: usize, pin: u8, high: bool) -> i32 {
    with_expander(index, pin, |expander, port, mask| {
        expander.outputs[port] = with_bit(expander.outputs[port], mask, high);
        let value = expander.outputs[port];
        unsafe { write_reg(expander.dev, XL9555_REG_OUTPUT_0 + port as u8, value) }
    })
}

/// Release every attached expander, so a re-run of the boot sequence starts
/// from an empty table rather than appending to the previous run's.
pub fn detach_all() {
    if let Ok(mut expanders) = EXPANDERS.lock() {
        for expander in expanders.drain(..) {
            unsafe { esp_ffi::i2c_master_bus_rm_device(expander.dev) };
        }
    }
}

fn with_expander<F>(index: usize, pin: u8, apply: F) -> i32
where
    F: FnOnce(&mut Xl9555, usize, u8) -> i32,
{
    if pin >= XL9555_PINS {
        return ESP_ERR_INVALID_ARG;
    }
    let Ok(mut expanders) = EXPANDERS.lock() else {
        return ESP_FAIL;
    };
    let Some(expander) = expanders.get_mut(index) else {
        return ESP_ERR_NOT_FOUND;
    };
    apply(expander, pin_port(pin), pin_mask(pin))
}

// ── I²C helpers ──────────────────────────────────────────────────────────────

/// # Safety
/// `dev` must be a valid I²C device handle.
unsafe fn write_reg(dev: *mut c_void, reg: u8, value: u8) -> i32 {
    let buf: [u8; 2] = [reg, value];
    esp_ffi::i2c_master_transmit(dev, buf.as_ptr(), buf.len(), I2C_TIMEOUT_MS)
}

/// Read consecutive registers starting at `reg`, which the part auto-increments.
///
/// # Safety
/// `dev` must be a valid I²C device handle.
unsafe fn read_regs(dev: *mut c_void, reg: u8, buf: &mut [u8]) -> i32 {
    esp_ffi::i2c_master_transmit_receive(
        dev,
        &reg as *const u8,
        1,
        buf.as_mut_ptr(),
        buf.len(),
        I2C_TIMEOUT_MS,
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_low_eight_pins_are_port_zero_and_the_rest_port_one() {
        assert_eq!(pin_port(0), 0);
        assert_eq!(pin_port(7), 0);
        assert_eq!(pin_port(8), 1);
        assert_eq!(pin_port(15), 1);
    }

    #[test]
    fn a_pin_occupies_the_same_bit_in_either_port() {
        assert_eq!(pin_mask(0), 0b0000_0001);
        assert_eq!(pin_mask(8), 0b0000_0001);
        assert_eq!(pin_mask(7), 0b1000_0000);
        assert_eq!(pin_mask(15), 0b1000_0000);
    }

    #[test]
    fn setting_a_bit_leaves_every_other_pin_in_the_port_alone() {
        assert_eq!(with_bit(0b0000_0000, pin_mask(3), true), 0b0000_1000);
        assert_eq!(with_bit(0b1111_1111, pin_mask(3), false), 0b1111_0111);
        assert_eq!(with_bit(0b1010_1010, pin_mask(1), true), 0b1010_1010);
    }

    #[test]
    fn configuring_a_pin_as_an_output_clears_its_configuration_bit() {
        // The part resets to all-inputs, so an output is a cleared bit.
        let reset_state = 0xFF;
        assert_eq!(with_bit(reset_state, pin_mask(2), false), 0b1111_1011);
    }

    #[test]
    fn a_pin_beyond_the_parts_width_is_refused() {
        assert_eq!(set_as_output(0, XL9555_PINS), ESP_ERR_INVALID_ARG);
        assert_eq!(set_level(0, XL9555_PINS, true), ESP_ERR_INVALID_ARG);
    }

    #[test]
    fn an_expander_index_no_board_profile_declared_is_refused() {
        detach_all();
        assert_eq!(set_as_output(0, 1), ESP_ERR_NOT_FOUND);
        assert_eq!(set_level(0, 1, true), ESP_ERR_NOT_FOUND);
    }
}
