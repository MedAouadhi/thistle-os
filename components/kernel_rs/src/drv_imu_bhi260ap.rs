// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — Bosch BHI260AP IMU driver (Rust)
//
// The BHI260AP is a sensor hub, not a register-mapped accelerometer. It boots
// into a ROM bootloader with no sensors at all; the host uploads Bosch's
// firmware into its program RAM over the command channel, boots it, and only
// then are there virtual sensors to configure. It keeps no firmware across a
// power cycle, and the boards that carry it fit no external flash, so the
// upload happens at every init.
//
// Everything here is the BHY2 host interface as Bosch's own SensorAPI speaks
// it, taken from the copy LilyGo ships in its T-Deck Max firmware
// (lib/SensorLib/src/bosch/bhy2_defs.h, bhy2_hif.c, bhy2.c). An earlier C
// driver in this tree used a register map for some other part and never
// uploaded firmware; it could not have returned data.
//
// The interrupt line is not used: every reading is a poll of the non-wakeup
// FIFO, taken when an app asks.

use std::os::raw::{c_char, c_void};
use std::sync::Mutex;

use crate::hal_registry::{HalImuCb, HalImuData, HalImuDriver};

// ── ESP error codes ──────────────────────────────────────────────────────────

const ESP_OK: i32 = 0;
const ESP_FAIL: i32 = -1;

// ── Host interface commands ──────────────────────────────────────────────────

/// Upload the firmware image into program RAM. Unlike every other command,
/// its length field counts 32-bit words rather than bytes.
const CMD_UPLOAD_TO_PROGRAM_RAM: u16 = 0x0002;

// ── Command packets ──────────────────────────────────────────────────────────

/// The four bytes that open a command on the command channel: the command
/// code, then the length of the payload that follows, both little-endian.
///
/// The payload is sent padded to a whole number of 32-bit words, and the
/// length names the padded size — in words for a firmware upload, in bytes for
/// everything else.
pub fn command_header(command: u16, payload_len: usize) -> [u8; 4] {
    let padded = payload_len.div_ceil(4) * 4;
    let length = if command == CMD_UPLOAD_TO_PROGRAM_RAM {
        padded / 4
    } else {
        padded
    } as u16;
    let [cmd_lo, cmd_hi] = command.to_le_bytes();
    let [len_lo, len_hi] = length.to_le_bytes();
    [cmd_lo, cmd_hi, len_lo, len_hi]
}

/// The payload of a sensor configuration command: the virtual sensor's id, its
/// sample rate as an IEEE float, and how long it may batch before reporting,
/// in milliseconds, as a 24-bit field. A rate of zero switches the sensor off.
pub fn sensor_config_payload(sensor_id: u8, rate_hz: f32, latency_ms: u32) -> [u8; 8] {
    let [r0, r1, r2, r3] = rate_hz.to_bits().to_le_bytes();
    let [l0, l1, l2, _] = latency_ms.to_le_bytes();
    [sensor_id, r0, r1, r2, r3, l0, l1, l2]
}

// ── FIFO events ──────────────────────────────────────────────────────────────

/// The accelerometer as its physical sensor reports it, before the hub's
/// calibration — the one LilyGo's own bring-up enables.
const SENSOR_ACCEL_PASSTHROUGH: u8 = 1;
/// The gyroscope, likewise.
const SENSOR_GYRO_PASSTHROUGH: u8 = 10;

/// Counts per standard gravity at the accelerometer's default range.
const ACCEL_COUNTS_PER_G: f32 = 4096.0;
const STANDARD_GRAVITY: f32 = 9.80665;
/// The gyroscope's default full scale, reached at ±32768 counts.
const GYRO_FULL_SCALE_DPS: f32 = 2000.0;

/// The size of an event, id byte included, or `None` for a sensor this driver
/// never enabled and so cannot frame. The system events are fixed by the host
/// interface (bhy2.c, `bhy2_sysid_event_size`); a three-axis passthrough event
/// is its id and three little-endian 16-bit axes.
fn event_size(id: u8) -> Option<usize> {
    match id {
        0 | 255 => Some(1),                    // padding, filler
        245 | 251 => Some(2),                  // small timestamp delta
        246 | 252 => Some(3),                  // large timestamp delta
        247 | 253 => Some(6),                  // full timestamp
        248 | 254 => Some(4),                  // meta event
        250 => Some(18),                       // debug message
        244 => Some(23),                       // log step
        SENSOR_ACCEL_PASSTHROUGH | SENSOR_GYRO_PASSTHROUGH => Some(7),
        _ => None,
    }
}

fn axes(event: &[u8]) -> [f32; 3] {
    let axis = |i: usize| i16::from_le_bytes([event[1 + 2 * i], event[2 + 2 * i]]) as f32;
    [axis(0), axis(1), axis(2)]
}

/// Fold a read of the non-wakeup FIFO into `reading`, each sensor taking the
/// value of its latest event. A sensor with no event in this read keeps what
/// it had, so a poll that finds the FIFO empty still answers with the last
/// known orientation rather than with zeroes.
///
/// Parsing stops at an event cut off by the end of the read, and at an event
/// whose size is not known, since nothing after it can be framed.
pub fn apply_fifo(fifo: &[u8], reading: &mut HalImuData) {
    let mut pos = 0;
    while pos < fifo.len() {
        let id = fifo[pos];
        let Some(size) = event_size(id) else {
            return;
        };
        let Some(event) = fifo.get(pos..pos + size) else {
            return;
        };
        match id {
            SENSOR_ACCEL_PASSTHROUGH => {
                let [x, y, z] = axes(event).map(|c| c / ACCEL_COUNTS_PER_G * STANDARD_GRAVITY);
                (reading.accel_x, reading.accel_y, reading.accel_z) = (x, y, z);
            }
            SENSOR_GYRO_PASSTHROUGH => {
                let [x, y, z] = axes(event).map(|c| c * GYRO_FULL_SCALE_DPS / 32768.0);
                (reading.gyro_x, reading.gyro_y, reading.gyro_z) = (x, y, z);
            }
            _ => {}
        }
        pos += size;
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

// ── A fake BHI260AP for host tests ───────────────────────────────────────────
//
// Enough of the part to bring it up and read it: the handful of registers the
// driver polls, a command channel that reassembles a command from however many
// writes it arrives in, and a non-wakeup FIFO that hands out its length and
// then its bytes across separate reads, as the hub does. What the driver
// uploaded and which sensors it switched on stay recorded for a test to check.

#[cfg(all(not(target_os = "espidf"), not(feature = "sim-bus")))]
mod esp_ffi {
    use std::os::raw::c_void;
    use std::sync::Mutex;

    const REG_CHAN_CMD: u8 = 0x00;
    const REG_CHAN_FIFO_NW: u8 = 0x02;
    const REG_RESET_REQ: u8 = 0x14;
    const REG_PRODUCT_ID: u8 = 0x1C;
    const REG_KERNEL_VERSION: u8 = 0x20;
    const REG_BOOT_STATUS: u8 = 0x25;
    const REG_ERROR_VALUE: u8 = 0x2E;

    const HOST_INTERFACE_READY: u8 = 0x10;
    const HOST_FW_VERIFY_DONE: u8 = 0x20;
    const HOST_FW_VERIFY_ERROR: u8 = 0x40;

    pub struct Chip {
        pub address: u8,
        pub product_id: u8,
        /// Report the uploaded image as corrupt, as the hub does when its
        /// checksum fails.
        pub reject_firmware: bool,
        registers: [u8; 0x40],
        /// Bytes written to the command channel not yet framed as a command.
        command: Vec<u8>,
        pub uploaded: Vec<u8>,
        pub booted: bool,
        /// Every sensor configuration, in order, as `(sensor id, rate)`.
        pub configured: Vec<(u8, f32)>,
        pub fifo: Vec<u8>,
        /// Bytes still owed on a FIFO transfer whose length has been read.
        fifo_owed: usize,
        /// The largest single write the driver made, which the part does not
        /// limit but the ESP-IDF transfer buffer does.
        pub largest_write: usize,
        /// Fail the next read of FIFO data, as a glitch on the bus would.
        pub fail_next_fifo_data_read: bool,
    }

    impl Chip {
        fn new(address: u8) -> Self {
            Chip {
                address,
                product_id: 0x89,
                reject_firmware: false,
                registers: [0; 0x40],
                command: Vec::new(),
                uploaded: Vec::new(),
                booted: false,
                configured: Vec::new(),
                fifo: Vec::new(),
                fifo_owed: 0,
                largest_write: 0,
                fail_next_fifo_data_read: false,
            }
        }

        fn read(&mut self, register: u8, out: &mut [u8]) {
            match register {
                REG_PRODUCT_ID => out[0] = self.product_id,
                REG_CHAN_FIFO_NW if self.fifo_owed == 0 => {
                    let owed = self.fifo.len();
                    out[..2].copy_from_slice(&(owed as u16).to_le_bytes());
                    self.fifo_owed = owed;
                }
                REG_CHAN_FIFO_NW => {
                    let n = out.len().min(self.fifo_owed);
                    out[..n].copy_from_slice(&self.fifo[..n]);
                    self.fifo.drain(..n);
                    self.fifo_owed -= n;
                }
                _ => {
                    for (i, byte) in out.iter_mut().enumerate() {
                        *byte = self.registers[(register as usize + i) % 0x40];
                    }
                }
            }
        }

        fn write(&mut self, register: u8, data: &[u8]) {
            match register {
                REG_RESET_REQ if data == [1] => {
                    let (address, product_id, reject) =
                        (self.address, self.product_id, self.reject_firmware);
                    let fifo = std::mem::take(&mut self.fifo);
                    *self = Chip::new(address);
                    self.product_id = product_id;
                    self.reject_firmware = reject;
                    self.fifo = fifo;
                    self.registers[REG_BOOT_STATUS as usize] = HOST_INTERFACE_READY;
                }
                REG_CHAN_CMD => {
                    self.command.extend_from_slice(data);
                    self.frame_commands();
                }
                _ => {}
            }
        }

        /// Execute every whole command the channel now holds.
        fn frame_commands(&mut self) {
            while self.command.len() >= 4 {
                let code = u16::from_le_bytes([self.command[0], self.command[1]]);
                let length = u16::from_le_bytes([self.command[2], self.command[3]]) as usize;
                let bytes = if code == 0x0002 { length * 4 } else { length };
                if self.command.len() < 4 + bytes {
                    return;
                }
                let payload: Vec<u8> = self.command.drain(..4 + bytes).skip(4).collect();
                self.execute(code, &payload);
            }
        }

        fn execute(&mut self, code: u16, payload: &[u8]) {
            let status = &mut self.registers[REG_BOOT_STATUS as usize];
            match code {
                0x0002 => {
                    self.uploaded = payload.to_vec();
                    *status = HOST_INTERFACE_READY
                        | if self.reject_firmware {
                            HOST_FW_VERIFY_ERROR
                        } else {
                            HOST_FW_VERIFY_DONE
                        };
                }
                0x0003 if !self.uploaded.is_empty() && !self.reject_firmware => {
                    self.booted = true;
                    self.registers[REG_KERNEL_VERSION as usize] = 0x6C;
                    self.registers[REG_KERNEL_VERSION as usize + 1] = 0x19;
                }
                0x000D if self.booted => {
                    let rate = f32::from_le_bytes([payload[1], payload[2], payload[3], payload[4]]);
                    self.configured.push((payload[0], rate));
                }
                _ => self.registers[REG_ERROR_VALUE as usize] = 0x01,
            }
        }
    }

    pub static CHIPS: Mutex<Vec<Chip>> = Mutex::new(Vec::new());

    /// Take every chip off the bus.
    pub fn clear() {
        CHIPS.lock().unwrap().clear();
    }

    /// Put a BHI260AP on the bus, freshly powered: in its bootloader, with
    /// nothing uploaded.
    pub fn attach(address: u8) {
        CHIPS.lock().unwrap().push(Chip::new(address));
    }

    /// Look at, or change, the chip at `address`.
    pub fn with_chip<T>(address: u8, f: impl FnOnce(&mut Chip) -> T) -> T {
        let mut chips = CHIPS.lock().unwrap();
        let chip = chips
            .iter_mut()
            .find(|chip| chip.address == address)
            .expect("no chip at that address");
        f(chip)
    }

    #[repr(C)]
    pub struct I2cDeviceConfig {
        pub dev_addr_length: u32,
        pub device_address: u16,
        pub scl_speed_hz: u32,
        pub scl_wait_us: u32,
        pub flags: u32,
    }

    const HANDLE_TAG: usize = 0x1000;

    fn address_of(handle: *mut c_void) -> u8 {
        (handle as usize & 0xFF) as u8
    }

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
        let mut chips = CHIPS.lock().unwrap();
        let Some(chip) = chips.iter_mut().find(|c| c.address == address_of(handle)) else {
            return -1; // nothing answered
        };
        if wsz != 1 {
            return -1;
        }
        let register = unsafe { *write };
        if register == REG_CHAN_FIFO_NW && chip.fifo_owed > 0 && chip.fail_next_fifo_data_read {
            chip.fail_next_fifo_data_read = false;
            return -1;
        }
        let out = unsafe { std::slice::from_raw_parts_mut(read, rsz) };
        chip.read(register, out);
        0
    }

    pub unsafe fn i2c_master_transmit(
        handle: *mut c_void,
        data: *const u8,
        len: usize,
        _timeout: i32,
    ) -> i32 {
        let mut chips = CHIPS.lock().unwrap();
        let Some(chip) = chips.iter_mut().find(|c| c.address == address_of(handle)) else {
            return -1;
        };
        if len < 2 {
            return -1;
        }
        let bytes = unsafe { std::slice::from_raw_parts(data, len) };
        chip.largest_write = chip.largest_write.max(len);
        chip.write(bytes[0], &bytes[1..]);
        0
    }
}

// ── Logging ──────────────────────────────────────────────────────────────────

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: u32, tag: *const u8, format: *const u8, ...);
}

static TAG: &[u8] = b"bhi260ap\0";

#[cfg(not(test))]
macro_rules! imu_log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => {
        unsafe { esp_log_write($level, TAG.as_ptr(), $fmt.as_ptr() $(, $arg)*); }
    };
}

#[cfg(test)]
macro_rules! imu_log {
    ($level:expr, $fmt:expr $(, $arg:expr)*) => { let _ = ($level, $($arg,)*); };
}

const LOG_ERROR: u32 = 1;
const LOG_INFO: u32 = 3;

#[cfg(not(test))]
fn delay_ms(ms: i32) {
    extern "C" {
        fn board_delay_ms(ms: i32);
    }
    unsafe { board_delay_ms(ms) };
}

#[cfg(test)]
fn delay_ms(_ms: i32) {}

// ── Registers and bring-up constants ─────────────────────────────────────────

const REG_CHAN_CMD: u8 = 0x00;
const REG_CHAN_FIFO_NW: u8 = 0x02;
const REG_RESET_REQ: u8 = 0x14;
const REG_PRODUCT_ID: u8 = 0x1C;
const REG_KERNEL_VERSION: u8 = 0x20;
const REG_BOOT_STATUS: u8 = 0x25;
const REG_ERROR_VALUE: u8 = 0x2E;

/// What the product id register reads on a BHI260AP (and its BHA260 sibling).
const PRODUCT_ID: u8 = 0x89;

const BOOT_HOST_INTERFACE_READY: u8 = 0x10;
const BOOT_HOST_FW_VERIFY_DONE: u8 = 0x20;
const BOOT_HOST_FW_VERIFY_ERROR: u8 = 0x40;

const CMD_BOOT_PROGRAM_RAM: u16 = 0x0003;
const CMD_CONFIG_SENSOR: u16 = 0x000D;

/// Bosch's firmware for the bare BHI260AP — no auxiliary magnetometer or
/// barometer attached — as LilyGo ships it
/// (lib/SensorLib/src/bosch/firmware/BHI260AP.fw.h), converted to binary.
/// See THIRD_PARTY_LICENSES.md.
const FIRMWARE: &[u8] = include_bytes!("../firmware/bhi260ap.fw");

/// The rate both sensors start at, which LilyGo's own bring-up uses too.
const DEFAULT_RATE_HZ: f32 = 100.0;

const DEFAULT_ADDRESS: u8 = 0x28;

/// Data bytes per write to the command channel. The hub takes a command in as
/// many writes as it arrives in; this only bounds the transfer buffer.
const COMMAND_CHUNK: usize = 252;

/// Bytes per read of the FIFO.
const FIFO_CHUNK: usize = 252;

/// How long a soft reset may take to bring the host interface back: 150 ms,
/// as Bosch's `bhy2_hif_reset` allows.
const RESET_POLLS: u32 = 15;
const RESET_POLL_MS: i32 = 10;
/// How long a firmware upload or boot may take to verify: 5 s, as Bosch's
/// `bhy2_hif_check_boot_status_ram` allows.
const VERIFY_POLLS: u32 = 100;
const VERIFY_POLL_MS: i32 = 50;

const I2C_TIMEOUT_MS: i32 = 1000;
const I2C_SPEED_HZ: u32 = 400_000;

const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_INVALID_STATE: i32 = 0x103;
const ESP_ERR_NOT_FOUND: i32 = 0x105;
const ESP_ERR_TIMEOUT: i32 = 0x107;

// ── Driver state ─────────────────────────────────────────────────────────────

/// What a board profile says about where the hub is.
struct Config {
    i2c_bus: i32,
    i2c_addr: u8,
}

struct Imu {
    config: Config,
    /// Null until the hub has been brought up, and so whether it is up.
    dev: *mut c_void,
    /// The last non-zero rate, which waking from sleep returns to.
    rate_hz: f32,
    /// Bytes of a FIFO transfer a failed read left unread.
    fifo_owed: usize,
    reading: HalImuData,
    cb: HalImuCb,
    cb_data: *mut c_void,
}

// SAFETY: the device handle is an opaque token dereferenced only by the
// ESP-IDF I²C driver, and the callback's user data only by the callback. The
// state is guarded by the mutex below.
unsafe impl Send for Imu {}

const ZERO_READING: HalImuData = HalImuData {
    accel_x: 0.0,
    accel_y: 0.0,
    accel_z: 0.0,
    gyro_x: 0.0,
    gyro_y: 0.0,
    gyro_z: 0.0,
    mag_x: 0.0,
    mag_y: 0.0,
    mag_z: 0.0,
};

impl Imu {
    const fn new() -> Self {
        Imu {
            config: Config {
                i2c_bus: 0,
                i2c_addr: DEFAULT_ADDRESS,
            },
            dev: std::ptr::null_mut(),
            rate_hz: DEFAULT_RATE_HZ,
            fifo_owed: 0,
            reading: ZERO_READING,
            cb: None,
            cb_data: std::ptr::null_mut(),
        }
    }
}

static IMU: Mutex<Imu> = Mutex::new(Imu::new());

// ── I²C helpers ──────────────────────────────────────────────────────────────

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

fn close(dev: *mut c_void) {
    if !dev.is_null() {
        unsafe { esp_ffi::i2c_master_bus_rm_device(dev) };
    }
}

fn read(dev: *mut c_void, register: u8, buf: &mut [u8]) -> bool {
    let ret = unsafe {
        esp_ffi::i2c_master_transmit_receive(
            dev,
            &register,
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
    read(dev, register, &mut value).then_some(value[0])
}

fn write(dev: *mut c_void, register: u8, data: &[u8]) -> bool {
    let mut buf = [0u8; 1 + COMMAND_CHUNK];
    buf[0] = register;
    buf[1..=data.len()].copy_from_slice(data);
    let ret =
        unsafe { esp_ffi::i2c_master_transmit(dev, buf.as_ptr(), 1 + data.len(), I2C_TIMEOUT_MS) };
    ret == ESP_OK
}

/// Send a command and its payload down the command channel, zero-padded to a
/// whole number of words, in writes no larger than the transfer buffer.
fn send_command(dev: *mut c_void, command: u16, payload: &[u8]) -> bool {
    let header = command_header(command, payload.len());
    let padding = payload.len().div_ceil(4) * 4 - payload.len();
    let mut bytes = header
        .iter()
        .chain(payload)
        .chain(std::iter::repeat_n(&0u8, padding))
        .copied();
    let mut chunk = [0u8; COMMAND_CHUNK];
    loop {
        let mut n = 0;
        for byte in bytes.by_ref().take(COMMAND_CHUNK) {
            chunk[n] = byte;
            n += 1;
        }
        if n == 0 {
            return true;
        }
        if !write(dev, REG_CHAN_CMD, &chunk[..n]) {
            return false;
        }
    }
}

/// Read everything the non-wakeup FIFO holds. The hub answers the first read
/// of a transfer with its length, then streams that many bytes across as many
/// reads as it takes. A transfer left unfinished would have the next poll read
/// event bytes as a length, so what a failed read leaves owing is counted in
/// `owed`, and the next poll reads it off and discards it before asking for a
/// new length.
fn read_fifo(dev: *mut c_void, owed: &mut usize) -> Option<Vec<u8>> {
    let mut scratch = [0u8; FIFO_CHUNK];
    while *owed > 0 {
        let n = (*owed).min(FIFO_CHUNK);
        if !read(dev, REG_CHAN_FIFO_NW, &mut scratch[..n]) {
            return None;
        }
        *owed -= n;
    }

    let mut length = [0u8; 2];
    if !read(dev, REG_CHAN_FIFO_NW, &mut length) {
        return None;
    }
    let mut fifo = vec![0u8; u16::from_le_bytes(length) as usize];
    *owed = fifo.len();
    for chunk in fifo.chunks_mut(FIFO_CHUNK) {
        if !read(dev, REG_CHAN_FIFO_NW, chunk) {
            return None;
        }
        *owed -= chunk.len();
    }
    Some(fifo)
}

/// Poll the boot status until `done` holds for it, or the polls run out.
fn wait_for_boot_status(
    dev: *mut c_void,
    polls: u32,
    interval_ms: i32,
    done: impl Fn(u8) -> bool,
) -> Option<u8> {
    let mut status = 0;
    for _ in 0..polls {
        delay_ms(interval_ms);
        if let Some(value) = read_reg(dev, REG_BOOT_STATUS) {
            status = value;
            if done(status) {
                return Some(status);
            }
        }
    }
    imu_log!(LOG_ERROR, b"bhi260ap: boot status stuck at 0x%02x\n\0", status as i32);
    None
}

/// The firmware has been checked and the host interface is back up.
fn firmware_verified(status: u8) -> bool {
    status & BOOT_HOST_INTERFACE_READY != 0
        && status & BOOT_HOST_FW_VERIFY_DONE != 0
        && status & BOOT_HOST_FW_VERIFY_ERROR == 0
}

fn configure_sensor(dev: *mut c_void, sensor_id: u8, rate_hz: f32) -> bool {
    send_command(dev, CMD_CONFIG_SENSOR, &sensor_config_payload(sensor_id, rate_hz, 0))
}

fn configure_sensors(dev: *mut c_void, rate_hz: f32) -> bool {
    configure_sensor(dev, SENSOR_ACCEL_PASSTHROUGH, rate_hz)
        && configure_sensor(dev, SENSOR_GYRO_PASSTHROUGH, rate_hz)
}

// ── Bring-up ─────────────────────────────────────────────────────────────────

/// Reset the hub, check it is one, upload the firmware and boot it. Returns
/// an ESP error code naming the step that failed.
fn bring_up(dev: *mut c_void) -> i32 {
    if !write(dev, REG_RESET_REQ, &[1]) {
        return ESP_ERR_NOT_FOUND; // nothing answered at the address
    }
    if wait_for_boot_status(dev, RESET_POLLS, RESET_POLL_MS, |s| {
        s & BOOT_HOST_INTERFACE_READY != 0
    })
    .is_none()
    {
        return ESP_ERR_TIMEOUT;
    }

    match read_reg(dev, REG_PRODUCT_ID) {
        Some(PRODUCT_ID) => {}
        Some(other) => {
            imu_log!(LOG_ERROR, b"bhi260ap: product id 0x%02x, expected 0x89\n\0", other as i32);
            return ESP_ERR_NOT_FOUND;
        }
        None => return ESP_ERR_NOT_FOUND,
    }

    if !send_command(dev, CMD_UPLOAD_TO_PROGRAM_RAM, FIRMWARE) {
        return ESP_FAIL;
    }
    if wait_for_boot_status(dev, VERIFY_POLLS, VERIFY_POLL_MS, firmware_verified).is_none() {
        return ESP_ERR_TIMEOUT;
    }
    if read_reg(dev, REG_ERROR_VALUE) != Some(0) {
        return ESP_FAIL;
    }

    if !send_command(dev, CMD_BOOT_PROGRAM_RAM, &[]) {
        return ESP_FAIL;
    }
    if wait_for_boot_status(dev, VERIFY_POLLS, VERIFY_POLL_MS, firmware_verified).is_none() {
        return ESP_ERR_TIMEOUT;
    }
    // The kernel version reads zero until the uploaded image is running.
    let mut version = [0u8; 2];
    if !read(dev, REG_KERNEL_VERSION, &mut version) || version == [0, 0] {
        return ESP_FAIL;
    }

    if !configure_sensors(dev, DEFAULT_RATE_HZ) {
        return ESP_FAIL;
    }
    imu_log!(
        LOG_INFO,
        b"bhi260ap: firmware %d bytes booted, kernel %u, accel+gyro at %d Hz\n\0",
        FIRMWARE.len() as i32,
        u16::from_le_bytes(version) as u32,
        DEFAULT_RATE_HZ as i32
    );
    ESP_OK
}

// ── vtable implementations ───────────────────────────────────────────────────

/// Bring the hub up. The configuration comes from the board profile entry
/// through `drv_imu_bhi260ap_register`, so `config` is not read.
///
/// # Safety
/// Called from C via the HAL vtable.
unsafe extern "C" fn bhi260ap_init(_config: *const c_void) -> i32 {
    let Ok(mut imu) = IMU.lock() else {
        return ESP_FAIL;
    };
    if !imu.dev.is_null() {
        return ESP_OK; // already up
    }
    let bus = crate::hal_registry::hal_bus_get_i2c(imu.config.i2c_bus);
    if bus.is_null() {
        return ESP_ERR_NOT_FOUND;
    }
    let Some(dev) = open(bus, imu.config.i2c_addr) else {
        return ESP_ERR_NOT_FOUND;
    };
    let ret = bring_up(dev);
    if ret != ESP_OK {
        imu_log!(LOG_ERROR, b"bhi260ap: bring-up failed: 0x%x\n\0", ret);
        close(dev);
        return ret;
    }
    imu.dev = dev;
    imu.rate_hz = DEFAULT_RATE_HZ;
    imu.fifo_owed = 0;
    imu.reading = ZERO_READING;
    ESP_OK
}

/// # Safety
/// Called from C via the HAL vtable.
unsafe extern "C" fn bhi260ap_deinit() {
    let Ok(mut imu) = IMU.lock() else {
        return;
    };
    if !imu.dev.is_null() {
        configure_sensors(imu.dev, 0.0);
        close(imu.dev);
    }
    imu.dev = std::ptr::null_mut();
    imu.reading = ZERO_READING;
    imu.cb = None;
    imu.cb_data = std::ptr::null_mut();
}

/// Drain the non-wakeup FIFO and answer with the newest sample of each sensor.
///
/// # Safety
/// `data` must be a writable `HalImuData` or null.
unsafe extern "C" fn bhi260ap_get_data(data: *mut HalImuData) -> i32 {
    if data.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let Ok(mut imu) = IMU.lock() else {
        return ESP_FAIL;
    };
    if imu.dev.is_null() {
        return ESP_ERR_INVALID_STATE;
    }
    let dev = imu.dev;
    let Some(fifo) = read_fifo(dev, &mut imu.fifo_owed) else {
        return ESP_FAIL;
    };
    apply_fifo(&fifo, &mut imu.reading);
    unsafe { *data = imu.reading };
    ESP_OK
}

/// Store a callback. Nothing invokes it: without the interrupt line there is
/// no moment at which new data announces itself.
///
/// # Safety
/// `cb` must remain valid until replaced or the driver is de-initialised.
unsafe extern "C" fn bhi260ap_register_callback(cb: HalImuCb, user_data: *mut c_void) -> i32 {
    let Ok(mut imu) = IMU.lock() else {
        return ESP_FAIL;
    };
    imu.cb = cb;
    imu.cb_data = user_data;
    ESP_OK
}

/// Run both sensors at `hz`, zero switching them off. A non-zero rate is
/// remembered, so waking from sleep returns to it.
///
/// # Safety
/// Called from C via the HAL vtable.
unsafe extern "C" fn bhi260ap_set_sample_rate(hz: u16) -> i32 {
    let Ok(mut imu) = IMU.lock() else {
        return ESP_FAIL;
    };
    if imu.dev.is_null() {
        return ESP_ERR_INVALID_STATE;
    }
    if !configure_sensors(imu.dev, hz as f32) {
        return ESP_FAIL;
    }
    if hz > 0 {
        imu.rate_hz = hz as f32;
    }
    ESP_OK
}

/// Sleep by switching both sensors off, which lets the hub idle; wake by
/// switching them back on at the rate they had.
///
/// # Safety
/// Called from C via the HAL vtable.
unsafe extern "C" fn bhi260ap_sleep(enter: bool) -> i32 {
    let Ok(imu) = IMU.lock() else {
        return ESP_FAIL;
    };
    if imu.dev.is_null() {
        return ESP_ERR_INVALID_STATE;
    }
    let rate = if enter { 0.0 } else { imu.rate_hz };
    if configure_sensors(imu.dev, rate) {
        ESP_OK
    } else {
        ESP_FAIL
    }
}

// ── HAL vtable ───────────────────────────────────────────────────────────────

static IMU_DRIVER: HalImuDriver = HalImuDriver {
    init: Some(bhi260ap_init),
    deinit: Some(bhi260ap_deinit),
    get_data: Some(bhi260ap_get_data),
    register_callback: Some(bhi260ap_register_callback),
    set_sample_rate: Some(bhi260ap_set_sample_rate),
    sleep: Some(bhi260ap_sleep),
    name: b"BHI260AP\0".as_ptr() as *const c_char,
};

/// The driver's vtable, for the compiled-in T-Deck Pro board, which registers
/// it with no configuration: bus 0, address 0x28, which is where that board
/// has it.
#[no_mangle]
pub extern "C" fn drv_imu_bhi260ap_get() -> *const HalImuDriver {
    &IMU_DRIVER
}

fn parse_config(config_json: &str) -> Config {
    Config {
        i2c_bus: crate::manifest::json_get_int(config_json, "i2c_bus").unwrap_or(0) as i32,
        i2c_addr: parse_address(config_json, "i2c_addr").unwrap_or(DEFAULT_ADDRESS),
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
pub unsafe extern "C" fn drv_imu_bhi260ap_register(config_json: *const c_char) -> i32 {
    let config = if config_json.is_null() {
        parse_config("{}")
    } else {
        match unsafe { std::ffi::CStr::from_ptr(config_json) }.to_str() {
            Ok(json) => parse_config(json),
            Err(_) => return ESP_ERR_INVALID_ARG,
        }
    };
    match IMU.lock() {
        Ok(mut imu) => imu.config = config,
        Err(_) => return ESP_FAIL,
    }
    unsafe { crate::hal_registry::hal_imu_register(&IMU_DRIVER, std::ptr::null()) }
}

#[cfg(test)]
mod tests {
    use super::*;

    // ── Command packets ──────────────────────────────────────────────────────

    #[test]
    fn a_sensor_configuration_names_its_eight_byte_payload_in_bytes() {
        // bhy2_hif_exec_sensor_conf_cmd: command 0x000D, 8-byte payload.
        assert_eq!(command_header(0x000D, 8), [0x0D, 0x00, 0x08, 0x00]);
    }

    #[test]
    fn a_firmware_upload_names_its_length_in_words() {
        // The shipped image is 103 676 bytes: 25 919 words, 0x653F.
        assert_eq!(command_header(0x0002, 103_676), [0x02, 0x00, 0x3F, 0x65]);
    }

    #[test]
    fn a_payload_that_is_not_whole_words_is_named_at_its_padded_length() {
        assert_eq!(command_header(0x000D, 5), [0x0D, 0x00, 0x08, 0x00]);
        assert_eq!(command_header(0x0002, 5), [0x02, 0x00, 0x02, 0x00]);
    }

    #[test]
    fn a_command_with_no_payload_names_a_length_of_zero() {
        // Boot program RAM, 0x0003, carries nothing.
        assert_eq!(command_header(0x0003, 0), [0x03, 0x00, 0x00, 0x00]);
    }

    #[test]
    fn a_sensor_is_configured_by_id_then_rate_as_a_float_then_a_24_bit_latency() {
        // 100.0f is 0x42C80000.
        assert_eq!(
            sensor_config_payload(10, 100.0, 0),
            [10, 0x00, 0x00, 0xC8, 0x42, 0x00, 0x00, 0x00]
        );
        assert_eq!(
            sensor_config_payload(1, 0.0, 0x0012_3456),
            [1, 0x00, 0x00, 0x00, 0x00, 0x56, 0x34, 0x12]
        );
    }

    #[test]
    fn a_latency_beyond_24_bits_is_cut_to_the_field() {
        assert_eq!(sensor_config_payload(1, 0.0, 0xFF12_3456)[5..], [0x56, 0x34, 0x12]);
    }

    // ── FIFO events ──────────────────────────────────────────────────────────

    fn empty_reading() -> HalImuData {
        HalImuData {
            accel_x: 0.0,
            accel_y: 0.0,
            accel_z: 0.0,
            gyro_x: 0.0,
            gyro_y: 0.0,
            gyro_z: 0.0,
            mag_x: 0.0,
            mag_y: 0.0,
            mag_z: 0.0,
        }
    }

    fn assert_close(actual: f32, expected: f32) {
        assert!(
            (actual - expected).abs() < 1e-3,
            "expected {expected}, got {actual}"
        );
    }

    #[test]
    fn an_accelerometer_event_reads_4096_counts_as_one_standard_gravity() {
        // x = +4096 (1 g), y = -2048 (-0.5 g), z = 0.
        let fifo = [1, 0x00, 0x10, 0x00, 0xF8, 0x00, 0x00];
        let mut reading = empty_reading();
        apply_fifo(&fifo, &mut reading);
        assert_close(reading.accel_x, 9.80665);
        assert_close(reading.accel_y, -4.903325);
        assert_close(reading.accel_z, 0.0);
    }

    #[test]
    fn a_gyroscope_event_reads_full_scale_as_2000_degrees_per_second() {
        // x = 16384 (half of full scale), y = -32768 (full scale), z = 164.
        let fifo = [10, 0x00, 0x40, 0x00, 0x80, 0xA4, 0x00];
        let mut reading = empty_reading();
        apply_fifo(&fifo, &mut reading);
        assert_close(reading.gyro_x, 1000.0);
        assert_close(reading.gyro_y, -2000.0);
        assert_close(reading.gyro_z, 10.009766);
    }

    #[test]
    fn timestamps_meta_events_and_padding_are_stepped_over_at_their_own_sizes() {
        let mut fifo = vec![];
        fifo.extend([0]); // padding, 1
        fifo.extend([251, 0x10]); // small delta, 2
        fifo.extend([252, 0x10, 0x00]); // large delta, 3
        fifo.extend([253, 1, 2, 3, 4, 5]); // full timestamp, 6
        fifo.extend([254, 1, 2, 3]); // meta event, 4
        fifo.extend([245, 0x10]); // wake-up small delta, 2
        fifo.extend([246, 0x10, 0x00]); // wake-up large delta, 3
        fifo.extend([247, 1, 2, 3, 4, 5]); // wake-up full timestamp, 6
        fifo.extend([248, 1, 2, 3]); // wake-up meta event, 4
        fifo.extend([250; 18]); // debug message, 18
        fifo.extend([244; 23]); // log step, 23
        fifo.extend([255]); // filler, 1
        fifo.extend([1, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00]);
        let mut reading = empty_reading();
        apply_fifo(&fifo, &mut reading);
        assert_close(reading.accel_x, 9.80665);
    }

    #[test]
    fn the_latest_event_of_each_sensor_is_the_one_reported() {
        let fifo = [
            1, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, // accel x = 1 g
            10, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, // gyro x = 1000 dps
            1, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, // accel x = 2 g
        ];
        let mut reading = empty_reading();
        apply_fifo(&fifo, &mut reading);
        assert_close(reading.accel_x, 2.0 * 9.80665);
        assert_close(reading.gyro_x, 1000.0);
    }

    #[test]
    fn a_sensor_with_no_event_in_the_fifo_keeps_its_previous_reading() {
        let mut reading = empty_reading();
        reading.gyro_z = 12.5;
        apply_fifo(&[1, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00], &mut reading);
        assert_close(reading.gyro_z, 12.5);
    }

    #[test]
    fn an_event_cut_off_by_the_end_of_the_read_is_not_decoded() {
        let fifo = [1, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 1, 0x00, 0x20, 0x00];
        let mut reading = empty_reading();
        apply_fifo(&fifo, &mut reading);
        assert_close(reading.accel_x, 9.80665);
    }

    #[test]
    fn an_event_of_unknown_size_ends_the_parse_without_misreading_what_follows() {
        // Sensor 4 is the corrected accelerometer, never enabled here, so its
        // event size is not known. The bytes after it cannot be framed.
        let fifo = [4, 1, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00];
        let mut reading = empty_reading();
        apply_fifo(&fifo, &mut reading);
        assert_close(reading.accel_x, 0.0);
    }

    #[test]
    fn the_vendored_image_is_a_bosch_firmware_image_of_whole_words() {
        // bhy2_hif_upload_firmware_to_ram refuses an image without the magic
        // 0x662B, and the upload's length field counts words.
        assert_eq!(&FIRMWARE[..2], &[0x2B, 0x66]);
        assert_eq!(FIRMWARE.len() % 4, 0);
    }

    // ── Bring-up and reading, over a fake chip ───────────────────────────────

    const ADDRESS: u8 = 0x28;

    /// An empty bus and a driver that has not been brought up, whatever the
    /// test before this one left behind.
    fn an_empty_board() {
        unsafe { bhi260ap_deinit() };
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
        unsafe { drv_imu_bhi260ap_register(json.as_ptr()) }
    }

    fn init() -> i32 {
        unsafe { bhi260ap_init(std::ptr::null()) }
    }

    #[test]
    fn bring_up_uploads_the_bosch_firmware_boots_it_and_starts_both_sensors() {
        an_empty_board();
        esp_ffi::attach(ADDRESS);

        assert_eq!(configured("{\"i2c_bus\": 0, \"i2c_addr\": \"0x28\"}"), ESP_OK);
        assert_eq!(init(), ESP_OK);

        esp_ffi::with_chip(ADDRESS, |chip| {
            assert!(chip.uploaded == FIRMWARE, "the whole image should have been uploaded");
            assert!(chip.booted, "the uploaded image should have been booted");
            assert_eq!(chip.configured, [(1, 100.0), (10, 100.0)]);
        });
    }

    fn get_data() -> (i32, HalImuData) {
        let mut data = empty_reading();
        let ret = unsafe { bhi260ap_get_data(&mut data) };
        (ret, data)
    }

    fn an_imu_that_is_up() {
        an_empty_board();
        esp_ffi::attach(ADDRESS);
        assert_eq!(configured("{\"i2c_bus\": 0}"), ESP_OK);
        assert_eq!(init(), ESP_OK);
    }

    fn queue_fifo(bytes: &[u8]) {
        esp_ffi::with_chip(ADDRESS, |chip| chip.fifo.extend_from_slice(bytes));
    }

    #[test]
    fn a_part_that_is_not_a_bhi260ap_is_left_alone() {
        an_empty_board();
        esp_ffi::attach(ADDRESS);
        esp_ffi::with_chip(ADDRESS, |chip| chip.product_id = 0x70);
        configured("{}");

        assert_eq!(init(), ESP_ERR_NOT_FOUND);
        esp_ffi::with_chip(ADDRESS, |chip| assert!(chip.uploaded.is_empty()));
        assert_eq!(get_data().0, ESP_ERR_INVALID_STATE);
    }

    #[test]
    fn a_board_with_nothing_at_the_address_reports_no_imu() {
        an_empty_board();
        configured("{}");
        assert_eq!(init(), ESP_ERR_NOT_FOUND);
    }

    #[test]
    fn firmware_the_hub_will_not_verify_leaves_the_driver_down() {
        an_empty_board();
        esp_ffi::attach(ADDRESS);
        esp_ffi::with_chip(ADDRESS, |chip| chip.reject_firmware = true);
        configured("{}");

        assert_ne!(init(), ESP_OK);
        esp_ffi::with_chip(ADDRESS, |chip| assert!(!chip.booted));
        assert_eq!(get_data().0, ESP_ERR_INVALID_STATE);
    }

    #[test]
    fn the_board_profile_can_place_the_hub_at_its_other_address() {
        an_empty_board();
        esp_ffi::attach(0x29);
        configured("{\"i2c_bus\": 0, \"i2c_addr\": \"0x29\"}");
        assert_eq!(init(), ESP_OK);
    }

    #[test]
    fn a_reading_is_the_latest_the_fifo_holds_in_physical_units() {
        an_imu_that_is_up();
        queue_fifo(&[
            252, 0x10, 0x00, // a timestamp
            1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, // accel z = 1 g
            10, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, // gyro x = 1000 dps
        ]);

        let (ret, data) = get_data();
        assert_eq!(ret, ESP_OK);
        assert_close(data.accel_z, 9.80665);
        assert_close(data.gyro_x, 1000.0);
    }

    #[test]
    fn a_poll_that_finds_the_fifo_empty_repeats_the_last_reading() {
        an_imu_that_is_up();
        queue_fifo(&[1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10]);
        get_data();

        let (ret, data) = get_data();
        assert_eq!(ret, ESP_OK);
        assert_close(data.accel_z, 9.80665);
    }

    #[test]
    fn a_backlog_longer_than_one_read_is_drained_to_its_newest_sample() {
        an_imu_that_is_up();
        // A second of accelerometer events at 100 Hz, the last one different.
        for _ in 0..99 {
            queue_fifo(&[1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10]);
        }
        queue_fifo(&[1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20]);

        let (_, data) = get_data();
        assert_close(data.accel_z, 2.0 * 9.80665);
        esp_ffi::with_chip(ADDRESS, |chip| assert!(chip.fifo.is_empty()));
    }

    fn configurations() -> Vec<(u8, f32)> {
        esp_ffi::with_chip(ADDRESS, |chip| std::mem::take(&mut chip.configured))
    }

    #[test]
    fn a_new_sample_rate_is_applied_to_both_sensors() {
        an_imu_that_is_up();
        configurations();

        assert_eq!(unsafe { bhi260ap_set_sample_rate(25) }, ESP_OK);
        assert_eq!(configurations(), [(1, 25.0), (10, 25.0)]);
    }

    #[test]
    fn sleep_stops_both_sensors_and_waking_restores_the_rate_they_had() {
        an_imu_that_is_up();
        unsafe { bhi260ap_set_sample_rate(25) };
        configurations();

        assert_eq!(unsafe { bhi260ap_sleep(true) }, ESP_OK);
        assert_eq!(configurations(), [(1, 0.0), (10, 0.0)]);

        assert_eq!(unsafe { bhi260ap_sleep(false) }, ESP_OK);
        assert_eq!(configurations(), [(1, 25.0), (10, 25.0)]);
    }

    #[test]
    fn a_read_that_fails_mid_transfer_does_not_leave_the_next_poll_misframed() {
        an_imu_that_is_up();
        queue_fifo(&[1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10]);
        esp_ffi::with_chip(ADDRESS, |chip| chip.fail_next_fifo_data_read = true);
        assert_ne!(get_data().0, ESP_OK);

        // The hub still owes the rest of that transfer. Read as a length, its
        // bytes would frame everything after them wrongly.
        queue_fifo(&[1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20]);
        let (ret, data) = get_data();
        assert_eq!(ret, ESP_OK);
        assert_close(data.accel_z, 2.0 * 9.80665);
    }

    #[test]
    fn waking_after_a_rate_of_zero_restores_the_last_rate_that_ran() {
        an_imu_that_is_up();
        unsafe { bhi260ap_set_sample_rate(25) };
        unsafe { bhi260ap_set_sample_rate(0) };
        configurations();

        assert_eq!(unsafe { bhi260ap_sleep(false) }, ESP_OK);
        assert_eq!(configurations(), [(1, 25.0), (10, 25.0)]);
    }

    #[test]
    fn rate_and_sleep_are_refused_before_the_hub_is_up() {
        an_empty_board();
        assert_eq!(unsafe { bhi260ap_set_sample_rate(25) }, ESP_ERR_INVALID_STATE);
        assert_eq!(unsafe { bhi260ap_sleep(true) }, ESP_ERR_INVALID_STATE);
    }
}
