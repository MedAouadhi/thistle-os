# 09: IMU on the T-Deck Max

**What to build:** Orientation data is available, and orientation-aware apps work.

The IMU is the same part the T-Deck Pro carries and sits on a 1.8 V rail that is now behind the IO expander, so the rail must be raised before the driver probes.

One thing needs resolving first: there are two drivers for this part in the tree, one C and one Rust, and the Rust one is a stub that returns not-supported. Which of them actually links is not established. Settle that before wiring the driver entry, or the board will appear to have an IMU that never returns data.

**Blocked by:** 03 (T-Deck Max boots to a usable system).

**Status:** in-review

- [x] The IMU's 1.8 V rail is raised via an expander pin at boot, before drivers load
- [x] It is established which of the two IMU drivers in the tree links, and the stub is not the one serving this board
- [x] A driver entry declares the IMU
- [x] Orientation data is available through the IMU HAL interface
- [x] A TODO is recorded that the board's haptic driver has no HAL interface to register against — adding one is a kernel ABI change deserving its own design, and would serve two boards since the Pro's later revision carries the same part

## Comments

### Neither driver could have worked

The Rust stub was the one linking: `kernel_rs` is WHOLE_ARCHIVE, so its
`drv_imu_bhi260ap_get` beat the C one, as with the MIA-M10Q (`5ce8b24`). But
the C driver would not have served either. Against the vendor's
`lib/SensorLib/src/bosch/bhy2_defs.h` it read the product id from 0x01 (the
wake-up FIFO channel; the id is at 0x1C), reset through 0x1C (0x14), polled
boot status at 0x0A (0x25), and took gyro passthrough to be sensor 3 (10). It
also never uploaded firmware. The BHI260AP boots into a ROM bootloader with no
sensors; the host has to upload Bosch's ~100 KB image into program RAM at
every power-up and boot it. And no board ever reached either driver:
`board_builtin_drivers.c` had no IMU case, so the Pro's entry was skipped with
"no builtin for driver id".

Decided with the user: a real Rust driver, with the Bosch firmware embedded in
kernel_rs. The C file is emptied like `drv_kbd_tca8418.c`.

### What was built

- `kernel_rs/src/drv_imu_bhi260ap.rs`: the BHY2 host interface ported from
  Bosch's SensorAPI. It resets, checks product id 0x89, uploads
  `kernel_rs/firmware/bhi260ap.fw` (LilyGo's `BHI260AP.fw.h` converted to
  binary unchanged, 103 676 bytes), waits for verify, boots, checks the kernel
  version, then starts accelerometer and gyro passthrough at 100 Hz.
  `get_data` drains the non-wake-up FIFO and reports the newest sample of
  each sensor in m/s^2 and deg/s, repeating the last one when the FIFO is
  empty. Polling only; `pin_int` is not read.
- Tests sit at two seams agreed with the user: the pure host-interface
  functions (command header, sensor config payload, FIFO event framing and
  scaling), and the vtable over a fake chip that reassembles commands and
  streams its FIFO the way the hub does.
- `tdeck-max.json` and its embedded copy raise XL9555 P03 (`1V8_EN`, pinmap
  section 6) high, with 10 ms settling, after the LoRa antenna select and
  before any driver loads. The profile also declares the driver entry. The
  dispatcher registers it through `drv_imu_bhi260ap_register(config_json)`,
  on the sy6970 pattern.
- Haptic TODO: `components/thistle_hal/include/hal/board.h`, beside the IMU
  field of the registry.
- `THIRD_PARTY_LICENSES.md` credits the firmware to Bosch under BSD-3-Clause.
  The `.fw.h` itself carries no notice; the one quoted is from the SensorAPI
  sources it ships with.

### Not verified, and what it touches beyond the Max

- **Not built with idf.py and not run on hardware.** Only host tests ran,
  plus `cargo +esp check`. Three things need a board:
  - that the hub verifies and boots the image;
  - that 252-byte I2C transfers are accepted (SensorLib uses 64, citing an
    Arduino limit);
  - how long the upload takes. At 400 kHz, roughly 2.5 s spent inside HAL
    init.
- The dispatch case is keyed on the driver id, so `tdeck-pro.json`,
  `twatch-ultra.json` and the compiled-in `board_tdeck_pro.c` now bring the
  hub up too. The Pro's and the Watch's IMU power has not been checked.
- A part that answers but never verifies holds HAL init for up to two 5 s
  waits, the timeouts Bosch's own code uses. A board with nothing at the
  address fails at once.
- The wake-up FIFO is never drained. Only meta events land there when
  nothing wake-up is enabled, and an overflow there drops events without
  blocking anything.
- The vtable stays registered when init fails, and `get_data` then answers
  `ESP_ERR_INVALID_STATE`. That is the same as sy6970.
